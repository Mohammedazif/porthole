"""Queued HID reports must stay paced even when the GLib loop is busy."""
import importlib.util
import queue
import subprocess
import sys
import threading
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

source = Path(__file__).resolve().parents[1] / "helpers" / "ble-bluez" / "porthole_ble.py"
spec = importlib.util.spec_from_file_location("porthole_ble", source)
ble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ble)


class SenderTests(unittest.TestCase):
    def test_mouse_alone_cannot_mark_composite_input_ready(self):
        reports = {kind: Mock(subscribed_at=0.0) for kind in (1, 2, 3)}
        reports[ble.FRAME_MOUSE].subscribed_at = 20.0
        self.assertFalse(ble.reports_subscribed(reports))
        reports[ble.FRAME_KEYBOARD].subscribed_at = 20.0
        reports[ble.FRAME_CONSUMER].subscribed_at = 20.0
        self.assertTrue(ble.reports_subscribed(reports, connected_at=19.0))
        # A cached keyboard subscription from the old connection is insufficient.
        reports[ble.FRAME_KEYBOARD].subscribed_at = 10.0
        self.assertFalse(ble.reports_subscribed(reports, connected_at=19.0))

    def test_pause_keeps_report_order_and_does_not_change_the_interval(self):
        frames = queue.Queue()
        frames.put((ble.FRAME_MOUSE, b"first"))
        frames.put((ble.FRAME_DELAY, b"\x96\x00"))
        frames.put((ble.FRAME_MOUSE, b"second"))
        frames.put((ble.FRAME_DELAY, b"\x3c\x00"))
        frames.put((ble.FRAME_SYNC, b"\x07\0\0\0"))
        frames.put(None)
        report = Mock()
        loop = Mock()
        with (patch.object(ble.GLib, "idle_add", side_effect=lambda cb, *args: cb(*args)),
              patch.object(ble.time, "sleep") as sleep,
              patch.object(ble, "emit") as emit):
            ble.sender(frames, {ble.FRAME_MOUSE: report}, loop)
            self.assertEqual([c.args for c in sleep.call_args_list],
                             [(0.015,), (0.15,), (0.015,), (0.06,)])
            self.assertEqual([c.args[0] for c in report.notify.call_args_list],
                             [b"first", b"second"])
            emit.assert_called_once_with("sync 7")
            loop.quit.assert_called_once()

    def test_keys_are_routed_to_their_own_lane(self):
        read_end, write_end = ble.os.pipe()
        stream = bytes([ble.FRAME_MOUSE, 7]) + b"m" * 7
        stream += bytes([ble.FRAME_KEYBOARD, 8]) + b"k" * 8
        stream += bytes([ble.FRAME_SYNC, 4]) + b"\x07\0\0\0"
        stream += bytes([ble.FRAME_CONSUMER, 2]) + b"c" * 2
        ble.os.write(write_end, stream)
        ble.os.close(write_end)
        motion, keys = queue.Queue(), queue.Queue()
        stdin = Mock()
        stdin.fileno.return_value = read_end
        try:
            with patch.object(ble.sys, "stdin", stdin):
                ble.stdin_reader(motion, keys)
        finally:
            ble.os.close(read_end)
        self.assertEqual([motion.get_nowait() for _ in range(3)],
                         [(ble.FRAME_MOUSE, b"m" * 7), (ble.FRAME_SYNC, b"\x07\0\0\0"), None])
        self.assertEqual([keys.get_nowait() for _ in range(3)],
                         [(ble.FRAME_KEYBOARD, b"k" * 8), (ble.FRAME_CONSUMER, b"c" * 2), None])

    def test_key_press_does_not_wait_behind_pointer_movement(self):
        motion, keys = queue.Queue(), queue.Queue()
        motion.put((ble.FRAME_MOUSE, b"anchor"))
        motion.put((ble.FRAME_DELAY, b"\x2c\x01"))  # 300 ms pause, then more movement
        motion.put((ble.FRAME_MOUSE, b"move"))
        mouse, keyboard = Mock(), Mock()
        reports = {ble.FRAME_MOUSE: mouse, ble.FRAME_KEYBOARD: keyboard}
        loop = Mock()
        key_sent = threading.Event()
        keyboard.notify.side_effect = lambda value: key_sent.set()
        with patch.object(ble.GLib, "idle_add", side_effect=lambda cb, *args: cb(*args)):
            lanes = [threading.Thread(target=ble.sender, args=(motion, reports, loop),
                                      daemon=True),
                     threading.Thread(target=ble.sender, args=(keys, reports, loop, False),
                                      daemon=True)]
            for lane in lanes:
                lane.start()
            try:
                # The motion lane is now inside its pause.
                keys.put((ble.FRAME_KEYBOARD, b"key"))
                self.assertTrue(key_sent.wait(timeout=0.15))
                self.assertEqual([c.args[0] for c in mouse.notify.call_args_list], [b"anchor"])
            finally:
                motion.put(None)
                keys.put(None)
                for lane in lanes:
                    lane.join(timeout=2)
            self.assertFalse(any(lane.is_alive() for lane in lanes))
            self.assertEqual([c.args[0] for c in mouse.notify.call_args_list],
                             [b"anchor", b"move"])
            # Only the motion lane ends the helper.
            loop.quit.assert_called_once()

    def test_shutdown_while_stdin_reader_is_blocked(self):
        script = (
            "import importlib.util, queue, threading, time; "
            f"s=importlib.util.spec_from_file_location('ble', {str(source)!r}); "
            "b=importlib.util.module_from_spec(s); s.loader.exec_module(b); "
            "threading.Thread(target=b.stdin_reader,args=(queue.Queue(),),daemon=True).start(); "
            "time.sleep(0.1)"
        )
        child = subprocess.Popen([sys.executable, "-c", script], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # Keep stdin open: advertising failure returns while this reader is blocked.
            self.assertEqual(child.wait(timeout=4), 0, child.stderr.read().decode())
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stdin.close()
            child.stdout.close()
            child.stderr.close()

    def test_shutdown_drains_key_releases_before_quitting(self):
        motion, keys, pending = queue.Queue(), queue.Queue(), queue.Queue()
        motion.put(None)
        keys.put((ble.FRAME_KEYBOARD, b"press"))
        keys.put((ble.FRAME_KEYBOARD, b"release"))
        keys.put(None)
        keyboard, loop = Mock(), Mock()
        reports = {ble.FRAME_KEYBOARD: keyboard}
        with patch.object(ble.GLib, "idle_add", side_effect=lambda *args: pending.put(args)):
            key_lane = threading.Thread(target=ble.sender,
                                        args=(keys, reports, loop, False), daemon=True)
            motion_lane = threading.Thread(target=ble.sender,
                                           args=(motion, reports, loop),
                                           kwargs={"wait_for": key_lane}, daemon=True)
            key_lane.start()
            press = pending.get(timeout=1)
            motion_lane.start()
            try:
                # EOF on the empty motion lane cannot stop the GLib loop while keys
                # are still waiting for dispatch, even if that loop has stalled.
                with self.assertRaises(queue.Empty):
                    pending.get(timeout=0.08)
                press[0](*press[1:])
                release = pending.get(timeout=1)
                self.assertEqual([c.args[0] for c in keyboard.notify.call_args_list],
                                 [b"press"])
                release[0](*release[1:])
                finish = pending.get(timeout=1)
                self.assertEqual([c.args[0] for c in keyboard.notify.call_args_list],
                                 [b"press", b"release"])
                finish[0](*finish[1:])
                loop.quit.assert_called_once()
            finally:
                # Drain callbacks so a failed assertion cannot strand either worker.
                if key_lane.is_alive() and keyboard.notify.call_count == 0:
                    press[0](*press[1:])
                while key_lane.is_alive() or motion_lane.is_alive():
                    try:
                        callback, *args = pending.get(timeout=1)
                    except queue.Empty:
                        break
                    callback(*args)
                key_lane.join(timeout=1)
                motion_lane.join(timeout=1)
            self.assertFalse(key_lane.is_alive() or motion_lane.is_alive())

    def test_missing_bluez_returns_without_starting_a_service(self):
        bus = Mock()
        bus.name_has_owner.return_value = False
        self.assertIsNone(ble.find_adapter(bus))
        bus.get_object.assert_not_called()

    def test_dispatch_finishes_before_next_report_or_sync(self):
        frames = queue.Queue()
        frames.put((ble.FRAME_MOUSE, b"first"))
        frames.put((ble.FRAME_MOUSE, b"second"))
        frames.put((ble.FRAME_SYNC, b"\x07\x00\x00\x00"))
        frames.put(None)
        pending = queue.Queue()
        report = Mock()
        loop = Mock()
        with (patch.object(ble.GLib, "idle_add", side_effect=lambda *args: pending.put(args)),
              patch.object(ble, "emit") as emit):
            worker = threading.Thread(target=ble.sender,
                                      args=(frames, {ble.FRAME_MOUSE: report}, loop), daemon=True)
            worker.start()
            try:
                first = pending.get(timeout=1)
                # Simulate a GLib stall longer than several report intervals.
                with self.assertRaises(queue.Empty):
                    pending.get(timeout=0.08)
                emit.assert_not_called()
                first[0](*first[1:])
                report.notify.assert_called_once_with(b"first")
                second = pending.get(timeout=1)
                emit.assert_not_called()
                second[0](*second[1:])
                worker.join(timeout=1)
                self.assertFalse(worker.is_alive())
                emit.assert_called_once_with("sync 7")
                self.assertEqual(report.notify.call_count, 2)
                end = pending.get(timeout=1)
                end[0](*end[1:])
                loop.quit.assert_called_once()
            finally:
                # Release a blocked worker even if an assertion fails.
                while not pending.empty():
                    callback, *args = pending.get_nowait()
                    callback(*args)
                if worker.is_alive():
                    first[0](*first[1:])
                    worker.join(timeout=0.1)


class ConnectionTests(unittest.TestCase):
    """Drive the real helper setup and recovery callbacks without a radio."""

    def run_helper(self, scenario, initial_devices=None):
        h = SimpleNamespace(bus=Mock(), gatt=Mock(), adv=Mock(), agent=Mock(),
                            properties=Mock(), device=Mock(), manager=Mock(), loop=Mock(),
                            emit=Mock(), clock=Mock(return_value=10.0), bonded=True,
                            timers=[], states={}, reports={kind: Mock(subscribed_at=0.0)
                                                          for kind in (1, 2, 3)})
        h.bus.get_object.side_effect = lambda service, path: path
        h.manager.GetManagedObjects.return_value = initial_devices or {}
        h.properties.Get.return_value = 'Porthole'

        def interface(obj, name):
            if name == ble.DBUS_PROP_IFACE:
                if obj == '/adapter':
                    return h.properties
                return Mock(Get=lambda iface, prop: h.bonded if prop == 'Bonded'
                            else h.states.get(str(obj), True))
            return {ble.GATT_MANAGER_IFACE: h.gatt, ble.LE_ADV_MANAGER_IFACE: h.adv,
                    ble.AGENT_MANAGER_IFACE: h.agent, ble.DBUS_OM_IFACE: h.manager,
                    ble.DEVICE_IFACE: h.device}[name]

        def timer(ms, callback, *args):
            h.timers.append((ms, callback, args))
            return len(h.timers)

        def event(connected, path='/phone'):
            h.states[path] = connected
            callback = next(call.args[0] for call in h.bus.add_signal_receiver.call_args_list
                            if call.kwargs.get('signal_name') == 'PropertiesChanged')
            callback(ble.DEVICE_IFACE, {'Connected': connected}, [], path=path)

        def find_timer(name):
            return next(item for item in reversed(h.timers) if item[1].__name__ == name)

        h.event, h.timer = event, find_timer
        h.fire = lambda item: item[1](*item[2])
        h.registered = lambda: h.gatt.RegisterApplication.call_args.kwargs['reply_handler']()
        h.loop.run.side_effect = lambda: scenario(h)
        with ExitStack() as stack:
            for owner, name, value in (
                    (ble.dbus.mainloop.glib, 'DBusGMainLoop', Mock()),
                    (ble.dbus, 'SystemBus', Mock(return_value=h.bus)),
                    (ble.dbus, 'Interface', interface),
                    (ble, 'find_adapter', Mock(return_value='/adapter')),
                    (ble, 'load_report_descriptor', Mock(return_value=(b'HID', {}))),
                    (ble, 'Application', Mock(return_value=Mock(get_path=lambda: ble.APP_PATH))),
                    (ble, 'Advertisement', Mock(return_value=Mock(get_path=lambda: ble.ADV_PATH))),
                    (ble, 'Agent', Mock()),
                    (ble, 'build_hid', Mock(return_value=h.reports)),
                    (ble.threading, 'Thread', Mock()),
                    (ble.GLib, 'MainLoop', Mock(return_value=h.loop)),
                    (ble.GLib, 'timeout_add', timer),
                    (ble.GLib, 'unix_signal_add', Mock()),
                    (ble, 'emit', h.emit), (ble, 'log', Mock()),
                    (ble.time, 'time', h.clock)):
                stack.enter_context(patch.object(owner, name, value))
            self.assertEqual(ble.main(), 0)

    def test_advertising_waits_for_hid_registration(self):
        def scenario(h):
            h.adv.RegisterAdvertisement.assert_not_called()
            h.registered()
            h.adv.RegisterAdvertisement.assert_called_once()
        self.run_helper(scenario)

    def test_failed_registration_never_advertises(self):
        def scenario(h):
            h.gatt.RegisterApplication.call_args.kwargs['error_handler']('registration failed')
            h.adv.RegisterAdvertisement.assert_not_called()
            h.loop.quit.assert_called_once()
        self.run_helper(scenario)

    def test_startup_cleanup_does_not_disconnect_a_fresh_link(self):
        def scenario(h):
            h.event(True)
            h.fire(h.timer('drop_stale_links'))
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario, {'/phone': {ble.DEVICE_IFACE: {'Connected': True}}})

    def test_startup_cleanup_still_drops_an_old_link(self):
        def scenario(h):
            h.fire(h.timer('drop_stale_links'))
            h.device.Disconnect.assert_called_once()
        self.run_helper(scenario, {'/phone': {ble.DEVICE_IFACE: {'Connected': True}}})

    def test_disconnected_and_replaced_link_timers_cannot_refresh_services(self):
        def scenario(h):
            h.event(True)
            old = h.timer('check_resubscribed')
            self.assertGreaterEqual(old[0], 10000)  # allow discovery more than three seconds
            h.event(False)
            h.fire(old)
            h.gatt.UnregisterApplication.assert_not_called()
            h.event(True)
            # Both connects have exactly the same mocked clock time: the connection
            # generation, rather than a timestamp comparison, must reject the timer.
            h.fire(old)
            h.gatt.UnregisterApplication.assert_not_called()
        self.run_helper(scenario)

    def test_old_recovery_cannot_disconnect_a_new_link(self):
        def scenario(h):
            h.registered()
            h.event(True)
            h.fire(h.timer('check_resubscribed'))
            h.gatt.UnregisterApplication.assert_called_once()
            disconnect = h.timer('reconnect_if_stale')
            h.event(False)
            h.event(True)
            h.fire(disconnect)
            h.device.Disconnect.assert_not_called()
            # Restoring the application after refresh must not advertise twice.
            registration = next(t for t in h.timers if t[0] == 500)
            h.fire(registration)
            h.registered()
            h.adv.RegisterAdvertisement.assert_called_once()
        self.run_helper(scenario)

    def test_subscribed_connection_is_left_alone(self):
        def scenario(h):
            h.event(True)
            for report in h.reports.values():
                report.subscribed_at = 11.0
            h.fire(h.timer('check_resubscribed'))
            h.gatt.UnregisterApplication.assert_not_called()
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_bonded_reconnect_reuses_enabled_notifications(self):
        def scenario(h):
            h.event(True)
            for report in h.reports.values():
                report.subscribed_at = 11.0
            h.event(False)
            h.clock.return_value = 20.0
            h.emit.reset_mock()
            timers = len(h.timers)
            h.event(True)
            h.emit.assert_any_call('subscribed')
            self.assertEqual(len(h.timers), timers)
        self.run_helper(scenario)

    def test_unbonded_reconnect_requires_new_notifications(self):
        def scenario(h):
            for report in h.reports.values():
                report.subscribed_at = 5.0
            h.bonded = False
            h.event(True)
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
            h.timer('check_resubscribed')
        self.run_helper(scenario)

    def test_service_refresh_invalidates_cached_notifications(self):
        def scenario(h):
            h.reports[1].subscribed_at = h.reports[2].subscribed_at = 5.0
            h.event(True)
            h.fire(h.timer('check_resubscribed'))
            self.assertTrue(all(report.subscribed_at == 0 for report in h.reports.values()))
        self.run_helper(scenario)

    def test_stop_notify_invalidates_the_subscription(self):
        report = SimpleNamespace(subscribed_at=10.0)
        ble.Characteristic.StopNotify(report)
        self.assertEqual(report.subscribed_at, 0.0)


if __name__ == "__main__":
    unittest.main()
