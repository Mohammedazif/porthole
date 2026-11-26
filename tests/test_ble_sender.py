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
    def test_repeated_final_neutral_reports_continue_after_teardown_errors(self):
        reports = {kind: Mock() for kind in (1, 2, 3)}
        reports[1].notify.side_effect = ble.dbus.exceptions.DBusException('radio removed')
        reports[2].notify.side_effect = ble.dbus.exceptions.DBusException('provider gone')
        with patch.object(ble, 'log') as log:
            ble.release_reports(reports)
            ble.release_reports(reports)
        for kind, size in ((1, 8), (2, 7), (3, 2)):
            self.assertEqual([call.args for call in reports[kind].notify.call_args_list],
                             [(bytes(size),), (bytes(size),)])
        self.assertEqual(log.call_count, 4)

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

    def run_helper(self, scenario, initial_devices=None, auto_register=True):
        h = SimpleNamespace(bus=Mock(), gatt=Mock(), adv=Mock(),
                            properties=Mock(), device=Mock(), manager=Mock(), loop=Mock(),
                            emit=Mock(), log=Mock(), clock=Mock(return_value=10.0), bonded=True,
                            timers=[], states={}, reports={kind: Mock(subscribed_at=0.0)
                                                          for kind in (1, 2, 3)})
        h.objects = {str(path): {name: dict(properties) for name, properties in interfaces.items()}
                     for path, interfaces in (initial_devices or {}).items()}
        h.devices = {}
        h.property_sets = []
        h.application = Mock(get_path=lambda: ble.APP_PATH)
        h.threads = Mock()
        h.registration_checks = []
        h.gatt.RegisterApplication.side_effect = lambda *args, **kwargs: h.registration_checks.append(
            {'InterfacesAdded', 'InterfacesRemoved', 'PropertiesChanged'}.issubset(
                call.kwargs.get('signal_name') for call in h.bus.add_signal_receiver.call_args_list)
            and h.manager.GetManagedObjects.called
            and callable(h.application.on_device))
        h.bus.get_object.side_effect = lambda service, path: path
        h.manager.GetManagedObjects.side_effect = lambda: h.objects
        h.properties.Get.return_value = True

        def properties_for(path):
            return h.objects.setdefault(str(path), {}).setdefault(ble.DEVICE_IFACE, {})

        def get_property(path, property_name):
            properties = properties_for(path)
            if property_name == 'Connected':
                return bool(h.states.get(str(path), properties.get('Connected', False)))
            if property_name == 'Bonded':
                return bool(properties.get('Bonded', h.bonded))
            return properties.get(property_name, False)

        def device_for(path):
            path = str(path)
            if path not in h.devices:
                h.devices[path] = Mock()
                h.devices[path].Disconnect.side_effect = lambda *args, **kwargs: h.device.Disconnect(path)
            return h.devices[path]

        def interface(obj, name):
            if name == ble.DBUS_PROP_IFACE:
                if obj == '/adapter':
                    return h.properties
                def set_property(iface, prop, value, obj=obj):
                    h.property_sets.append((str(obj), prop, bool(value)))
                    properties_for(obj)[prop] = bool(value)
                return SimpleNamespace(Get=lambda iface, prop: get_property(obj, prop), Set=set_property)
            if name == ble.DEVICE_IFACE:
                return device_for(obj)
            return {ble.GATT_MANAGER_IFACE: h.gatt, ble.LE_ADV_MANAGER_IFACE: h.adv,
                    ble.DBUS_OM_IFACE: h.manager,
                    ble.DEVICE_IFACE: h.device}[name]

        def timer(ms, callback, *args):
            h.timers.append((ms, callback, args))
            return len(h.timers)

        def event(connected, path='/adapter/dev_PHONE'):
            first = path not in h.objects
            properties_for(path).setdefault('Icon', 'phone')
            properties_for(path).setdefault('Trusted', True)
            properties_for(path)['Connected'] = connected
            h.states[path] = connected
            if first:
                for call in h.bus.add_signal_receiver.call_args_list:
                    if call.kwargs.get('signal_name') == 'InterfacesAdded':
                        call.args[0](path, {ble.DEVICE_IFACE: dict(properties_for(path))})
            callback = next(call.args[0] for call in h.bus.add_signal_receiver.call_args_list
                            if call.kwargs.get('signal_name') == 'PropertiesChanged')
            callback(ble.DEVICE_IFACE, {'Connected': connected}, [], path=path)

        def added(path, properties):
            properties_for(path).update(properties)
            h.states[path] = bool(properties.get('Connected', False))
            callback = next(call.args[0] for call in h.bus.add_signal_receiver.call_args_list
                            if call.kwargs.get('signal_name') == 'InterfacesAdded')
            callback(path, {ble.DEVICE_IFACE: properties})

        def subscribed(at):
            h.clock.return_value = at
            for report in h.reports.values():
                report.subscribed_at = at
                report.on_subscribe()

        def find_timer(name):
            return next(item for item in reversed(h.timers) if item[1].__name__ == name)

        h.event, h.added, h.subscribe, h.timer = event, added, subscribed, find_timer
        h.fire = lambda item: item[1](*item[2])
        h.registered = lambda: h.gatt.RegisterApplication.call_args.kwargs['reply_handler']()
        def run_scenario():
            if auto_register:
                h.registered()
            scenario(h)
        h.loop.run.side_effect = run_scenario
        with ExitStack() as stack:
            for owner, name, value in (
                    (ble.dbus.mainloop.glib, 'DBusGMainLoop', Mock()),
                    (ble.dbus, 'SystemBus', Mock(return_value=h.bus)),
                    (ble.dbus, 'Interface', interface),
                    (ble, 'find_adapter', Mock(return_value='/adapter')),
                    (ble, 'load_report_descriptor', Mock(return_value=(b'HID', {}))),
                    (ble, 'Application', Mock(return_value=h.application)),
                    (ble, 'Advertisement', Mock(return_value=Mock(get_path=lambda: ble.ADV_PATH))),
                    (ble, 'build_hid', Mock(return_value=h.reports)),
                    (ble.threading, 'Thread', h.threads),
                    (ble.GLib, 'MainLoop', Mock(return_value=h.loop)),
                    (ble.GLib, 'timeout_add', timer),
                    (ble.GLib, 'unix_signal_add', Mock()),
                    (ble, 'emit', h.emit), (ble, 'log', h.log),
                    (ble.time, 'time', h.clock)):
                stack.enter_context(patch.object(owner, name, value))
            self.assertEqual(ble.main(), 0)
        return h

    def test_advertising_waits_for_hid_registration(self):
        def scenario(h):
            self.assertEqual(h.registration_checks, [True])
            h.adv.RegisterAdvertisement.assert_not_called()
            h.registered()
            h.adv.RegisterAdvertisement.assert_called_once()
        self.run_helper(scenario, auto_register=False)

    def test_failed_registration_never_advertises(self):
        def scenario(h):
            h.gatt.RegisterApplication.call_args.kwargs['error_handler']('registration failed')
            h.adv.RegisterAdvertisement.assert_not_called()
            h.loop.quit.assert_called_once()
        self.run_helper(scenario, auto_register=False)

    def test_startup_cleanup_does_not_disconnect_a_fresh_link(self):
        def scenario(h):
            h.added('/adapter/dev_FRESH', {'Connected': True, 'Icon': 'phone',
                                         'Bonded': False, 'Paired': False, 'Trusted': False})
            self.assertFalse(any(timer[0] == 2000 for timer in h.timers))
            h.fire(h.timer('check_resubscribed'))
            h.device.Disconnect.assert_not_called()
            h.gatt.UnregisterApplication.assert_not_called()
        self.run_helper(scenario)

    def test_startup_bonded_link_reconnects_once_after_replacement_registration_grace(self):
        def scenario(h):
            self.assertFalse(any(timer[0] == 2000 for timer in h.timers))
            self.assertGreaterEqual(h.timer('check_resubscribed')[0], 15000)
            h.fire(h.timer('check_resubscribed'))
            self.assertFalse(any(timer[1].__name__ == 'subscription_deadline' for timer in h.timers))
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            self.assertEqual(h.timer('subscription_deadline')[0], 15000)
            h.fire(h.timer('subscription_deadline'))
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
            handlers = h.devices['/adapter/dev_PHONE'].Disconnect.call_args.kwargs
            self.assertTrue(callable(handlers['reply_handler']))
            self.assertTrue(callable(handlers['error_handler']))
            h.gatt.UnregisterApplication.assert_called_once()
        self.run_helper(scenario, {'/adapter/dev_PHONE': {ble.DEVICE_IFACE: {
            'Connected': True, 'Icon': 'phone', 'Bonded': True, 'Trusted': True}}})

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
            registration = next(t for t in h.timers if t[0] == 500)
            h.fire(registration)
            h.registered()
            deadline = h.timer('subscription_deadline')
            h.event(False)
            h.event(True)
            h.fire(deadline)
            h.device.Disconnect.assert_not_called()
            # Restoring the application after refresh must not advertise twice.
            h.adv.RegisterAdvertisement.assert_called_once()
        self.run_helper(scenario)

    def test_subscribed_connection_is_left_alone(self):
        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
            h.fire(h.timer('check_resubscribed'))
            h.gatt.UnregisterApplication.assert_not_called()
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_bonded_reconnect_reuses_enabled_notifications(self):
        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
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

    def test_initial_unknown_private_client_is_identified_by_our_gatt_read(self):
        def scenario(h):
            path = '/adapter/dev_PRIVATE'
            h.added(path, {'Connected': True, 'AddressType': 'random', 'Paired': False,
                           'Bonded': False, 'Trusted': False})
            self.assertNotIn(('connected',), [call.args for call in h.emit.call_args_list])
            characteristic = SimpleNamespace(service=SimpleNamespace(app=h.application), value=b'HID')
            characteristic.observe_device = lambda options: ble.Characteristic.observe_device(characteristic, options)
            self.assertEqual(bytes(ble.Characteristic.ReadValue(characteristic, {'device': path})), b'HID')
            h.emit.assert_any_call('connected')
            ble.Characteristic.WriteValue(characteristic, b'\x01', {'device': path})
            self.assertEqual([call.args for call in h.emit.call_args_list].count(('connected',)), 1)
            h.fire(h.timer('check_resubscribed'))
            h.device.Disconnect.assert_not_called()
            h.gatt.UnregisterApplication.assert_not_called()
            h.subscribe(12.0)
            h.emit.assert_any_call('subscribed')
        self.run_helper(scenario)

    def test_other_adapter_and_headset_events_cannot_replace_the_phone(self):
        def scenario(h):
            h.event(True)
            phone_timer = h.timer('check_resubscribed')
            h.emit.reset_mock()
            h.added('/other_adapter/dev_PHONE', {'Connected': True, 'Icon': 'phone',
                                                'Bonded': True, 'Trusted': True})
            h.added('/adapter/dev_HEADSET', {'Connected': True, 'Icon': 'audio-headset',
                                            'Bonded': True, 'Trusted': True})
            h.event(False, '/adapter/dev_HEADSET')
            h.emit.assert_not_called()
            h.fire(phone_timer)
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            h.fire(h.timer('subscription_deadline'))
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
            h.gatt.UnregisterApplication.assert_called_once()
        self.run_helper(scenario)

    def test_early_cccds_are_reconciled_when_unknown_client_is_identified_later(self):
        def scenario(h):
            path = '/adapter/dev_PRIVATE'
            h.added(path, {'Connected': True, 'Paired': False, 'Bonded': False, 'Trusted': False})
            h.subscribe(11.0)  # No Device1 Connected delta or identifying read yet.
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
            h.clock.return_value = 20.0
            h.application.on_device(path)
            h.emit.assert_any_call('connected')
            h.emit.assert_any_call('subscribed')
            self.assertFalse(h.timers)
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_late_gatt_callback_cannot_resurrect_a_disconnected_phone(self):
        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
            h.event(False)
            h.emit.reset_mock()
            timers = len(h.timers)
            h.application.on_device('/adapter/dev_PHONE')
            h.emit.assert_not_called()
            self.assertEqual(len(h.timers), timers)
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_startup_ignores_connected_headsets_and_other_adapters(self):
        def scenario(h):
            self.assertFalse(h.timers)
            h.device.Disconnect.assert_not_called()
            self.assertNotIn(('connected',), [call.args for call in h.emit.call_args_list])
        self.run_helper(scenario, {
            '/adapter/dev_HEADSET': {ble.DEVICE_IFACE: {'Connected': True, 'Icon': 'audio-headset',
                                                     'Bonded': True, 'Trusted': True}},
            '/other_adapter/dev_PHONE': {ble.DEVICE_IFACE: {'Connected': True, 'Icon': 'phone',
                                                         'Bonded': True, 'Trusted': True}}})

    def test_provider_refresh_requires_current_trust_and_bond(self):
        def scenario(h):
            path = '/adapter/dev_PHONE'
            h.added(path, {'Connected': True, 'Icon': 'phone', 'Bonded': False,
                           'Paired': False, 'Trusted': False})
            recovery = h.timer('check_resubscribed')
            h.fire(recovery)
            h.gatt.UnregisterApplication.assert_not_called()
            h.device.Disconnect.assert_not_called()
            self.assertEqual(h.property_sets, [])  # a phone that never paired is not trusted
            h.objects[path][ble.DEVICE_IFACE].update(Bonded=True, Trusted=True)
            h.fire(recovery)
            h.gatt.UnregisterApplication.assert_called_once()
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            deadline = h.timer('subscription_deadline')
            h.objects[path][ble.DEVICE_IFACE]['Trusted'] = False
            h.fire(deadline)
            h.device.Disconnect.assert_not_called()
            h.objects[path][ble.DEVICE_IFACE].update(Trusted=True, Bonded=False)
            h.fire(deadline)
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_bonded_untrusted_phone_is_trusted_so_a_stale_link_recovers(self):
        # A fresh pairing from the iPhone leaves it untrusted. When the link
        # outlives the previous helper, recovery needs trust, or the mouse and
        # keyboard only came back after forgetting and pairing again.
        def scenario(h):
            path = '/adapter/dev_PHONE'
            h.added(path, {'Connected': True, 'Icon': 'phone', 'Bonded': True,
                           'Paired': True, 'Trusted': False})
            self.assertEqual(h.property_sets, [(path, 'Trusted', True)])
            h.fire(h.timer('check_resubscribed'))
            h.gatt.UnregisterApplication.assert_called_once()
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            h.fire(h.timer('subscription_deadline'))
            h.device.Disconnect.assert_called_once_with(path)
        self.run_helper(scenario)

    def test_one_hundred_same_bond_idle_reconnect_cycles_need_no_new_pairing(self):
        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
            timers = len(h.timers)
            for cycle in range(100):
                h.event(False)
                h.clock.return_value = 20.0 + cycle * 60
                h.emit.reset_mock()
                h.event(True)
                h.emit.assert_any_call('subscribed')
                self.assertEqual(len(h.timers), timers)
            h.device.Disconnect.assert_not_called()
            h.gatt.UnregisterApplication.assert_not_called()
        self.run_helper(scenario)

    def test_a_different_phone_cannot_reuse_the_previous_peer_cccds(self):
        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
            h.event(False)
            h.clock.return_value = 20.0
            h.emit.reset_mock()
            h.added('/adapter/dev_PHONE_B', {'Connected': True, 'Icon': 'phone',
                                           'Bonded': True, 'Trusted': True})
            self.assertTrue(all(report.subscribed_at == 0 for report in h.reports.values()))
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
        self.run_helper(scenario)

    def test_removing_the_active_device_invalidates_its_cached_subscriptions(self):
        def scenario(h):
            h.event(True)
            old = h.timer('check_resubscribed')
            h.subscribe(11.0)
            removed = next(call.args[0] for call in h.bus.add_signal_receiver.call_args_list
                           if call.args[0].__name__ == 'on_device_removed')
            h.emit.reset_mock()
            removed('/adapter/dev_PHONE', [ble.DEVICE_IFACE])
            h.emit.assert_any_call('disconnected')
            self.assertTrue(all(report.subscribed_at == 0 for report in h.reports.values()))
            h.fire(old)
            h.device.Disconnect.assert_not_called()
            h.gatt.UnregisterApplication.assert_not_called()
        self.run_helper(scenario)

    def test_fresh_helper_replaces_provider_without_dropping_the_existing_bonded_link(self):
        phone = '/adapter/dev_PHONE'
        initial = {phone: {ble.DEVICE_IFACE: {'Connected': True, 'Paired': True,
                                             'Bonded': True, 'Trusted': True, 'Icon': 'phone'}}}
        old_reports = {}

        def previous_helper(h):
            h.subscribe(11.0)
            h.emit.assert_any_call('subscribed')
            old_reports.update(h.reports)
        self.run_helper(previous_helper, initial)

        def restarted_helper(h):
            self.assertTrue(all(report is not old_reports[kind] and report.subscribed_at == 0
                                for kind, report in h.reports.items()))
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
            old_registered = h.gatt.RegisterApplication.call_args.kwargs['reply_handler']
            h.clock.return_value = 25.0
            h.fire(h.timer('check_resubscribed'))
            h.gatt.UnregisterApplication.assert_called_once()
            self.assertFalse(any(timer[0] == 1500 for timer in h.timers))
            registration = next(timer for timer in h.timers if timer[0] == 500)
            h.fire(registration)
            old_registered()  # A stale success cannot finish the replacement.
            self.assertFalse(any(timer[1].__name__ == 'subscription_deadline' for timer in h.timers))
            h.clock.return_value = 32.0  # Slow async registration exceeds the old 1.5 s recovery.
            h.registered()
            deadline = h.timer('subscription_deadline')
            self.assertEqual(deadline[0], 15000)
            h.subscribe(35.0)  # Restored CCCDs arrive three seconds after completion.
            h.emit.assert_any_call('subscribed')
            h.clock.return_value = 47.0
            h.fire(deadline)
            h.device.Disconnect.assert_not_called()
            h.gatt.UnregisterApplication.assert_called_once()
            self.assertEqual(h.gatt.RegisterApplication.call_count, 2)
            self.assertTrue(h.objects[phone][ble.DEVICE_IFACE]['Bonded'])
            self.assertTrue(h.objects[phone][ble.DEVICE_IFACE]['Trusted'])
        self.run_helper(restarted_helper, initial)

    def test_missing_replacement_cccds_reconnect_only_once_across_physical_reconnects(self):
        def scenario(h):
            h.event(True)
            old_timer = h.timer('check_resubscribed')
            h.fire(old_timer)
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            deadline = h.timer('subscription_deadline')
            for _ in range(3):
                h.fire(deadline)
                h.fire(old_timer)
            h.gatt.UnregisterApplication.assert_called_once()
            self.assertEqual(h.gatt.RegisterApplication.call_count, 2)
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
            # Missing CCCDs after that reconnect cannot start another provider/
            # Disconnect cycle, even though this is a new physical connection.
            for _ in range(3):
                h.event(False)
                h.clock.return_value += 20.0
                h.event(True)
                h.fire(h.timer('check_resubscribed'))
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
            h.gatt.UnregisterApplication.assert_called_once()
            self.assertEqual(h.gatt.RegisterApplication.call_count, 2)
            self.assertTrue(any('waiting for the phone' in call.args[0] for call in h.log.call_args_list))
            self.assertFalse(any(call.args[0].startswith('error') for call in h.emit.call_args_list))
        self.run_helper(scenario)

    def test_replacement_disconnect_waits_for_async_registration_to_complete(self):
        def scenario(h):
            h.event(True)
            h.fire(h.timer('check_resubscribed'))
            h.device.Disconnect.assert_not_called()
            h.clock.return_value = 45.0
            # Time while Unregister/RegisterApplication is pending contributes
            # nothing to the replacement provider's full 15 second grace.
            self.assertFalse(any(timer[1].__name__ == 'subscription_deadline' for timer in h.timers))
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.device.Disconnect.assert_not_called()
            self.assertFalse(any(timer[1].__name__ == 'subscription_deadline' for timer in h.timers))
            h.clock.return_value = 70.0
            h.registered()
            deadline = h.timer('subscription_deadline')
            self.assertEqual(deadline[0], 15000)
            h.device.Disconnect.assert_not_called()
            h.clock.return_value = 85.0
            h.fire(deadline)
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
        self.run_helper(scenario)

    def test_replacement_partial_or_complete_cccd_progress_is_never_interrupted(self):
        for subscribed_kinds in ((1,), (2,), (3,), (1, 2), (1, 2, 3)):
            with self.subTest(subscribed_kinds=subscribed_kinds):
                def scenario(h):
                    h.event(True)
                    h.fire(h.timer('check_resubscribed'))
                    h.fire(next(timer for timer in h.timers if timer[0] == 500))
                    h.registered()
                    deadline = h.timer('subscription_deadline')
                    for kind in subscribed_kinds:
                        h.reports[kind].subscribed_at = 20.0
                        h.reports[kind].on_subscribe()
                    h.fire(deadline)
                    h.device.Disconnect.assert_not_called()
                    h.gatt.UnregisterApplication.assert_called_once()
                self.run_helper(scenario)

    def test_replacement_disconnect_requires_current_bond_trust_and_connection(self):
        for changed in ({'Bonded': False}, {'Trusted': False}, {'Connected': False}):
            with self.subTest(changed=changed):
                def scenario(h):
                    h.event(True)
                    h.fire(h.timer('check_resubscribed'))
                    h.fire(next(timer for timer in h.timers if timer[0] == 500))
                    h.registered()
                    deadline = h.timer('subscription_deadline')
                    h.objects['/adapter/dev_PHONE'][ble.DEVICE_IFACE].update(changed)
                    if 'Connected' in changed:
                        h.states['/adapter/dev_PHONE'] = False  # before its signal arrives
                    h.fire(deadline)
                    h.device.Disconnect.assert_not_called()
                self.run_helper(scenario)

    def test_replacement_deadline_cannot_disconnect_a_different_peer(self):
        def scenario(h):
            h.event(True)
            h.fire(h.timer('check_resubscribed'))
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            deadline = h.timer('subscription_deadline')
            h.event(False)
            h.added('/adapter/dev_PHONE_B', {'Connected': True, 'Icon': 'phone',
                                           'Bonded': True, 'Trusted': True})
            h.fire(deadline)
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario)

    def test_failed_disconnect_still_consumes_the_single_recovery_attempt(self):
        def scenario(h):
            h.event(True)
            h.fire(h.timer('check_resubscribed'))
            h.fire(next(timer for timer in h.timers if timer[0] == 500))
            h.registered()
            deadline = h.timer('subscription_deadline')
            h.fire(deadline)
            handlers = h.devices['/adapter/dev_PHONE'].Disconnect.call_args.kwargs
            self.assertTrue(callable(handlers['reply_handler']))
            handlers['error_handler'](ble.dbus.exceptions.DBusException('already in progress'))
            for _ in range(3):
                h.fire(deadline)
            h.device.Disconnect.assert_called_once_with('/adapter/dev_PHONE')
            self.assertTrue(any('already in progress' in call.args[0] for call in h.log.call_args_list))
        self.run_helper(scenario)

    def test_early_cccds_wait_for_provider_completion_and_late_disconnected_cccds_stay_off(self):
        def scenario(h):
            h.event(True)
            self.assertFalse(h.timers)  # No discovery timer before provider publication.
            h.subscribe(11.0)
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
            h.clock.return_value = 20.0
            h.registered()
            h.emit.assert_any_call('subscribed')
            h.event(False)
            h.emit.reset_mock()
            h.subscribe(21.0)
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
            h.device.Disconnect.assert_not_called()
        self.run_helper(scenario, auto_register=False)

    def test_cccd_callback_cannot_mark_ready_when_bluez_already_reports_disconnected(self):
        def scenario(h):
            h.event(True)
            # The live state can change before its PropertiesChanged is delivered.
            h.states['/adapter/dev_PHONE'] = False
            h.emit.reset_mock()
            h.subscribe(11.0)
            self.assertNotIn(('subscribed',), [call.args for call in h.emit.call_args_list])
        self.run_helper(scenario)

    def test_sigterm_neutralizes_before_unregister_and_blocks_queued_normal_dispatch(self):
        real_thread = threading.Thread
        pending, history, workers = queue.Queue(), [], []

        def scenario(h):
            h.event(True)
            h.subscribe(11.0)
            for kind, report in h.reports.items():
                report.notify.side_effect = lambda value, kind=kind: history.append(('notify', kind, value))
            stopping = next(call.kwargs['kwargs']['stopping'] for call in h.threads.call_args_list
                            if call.kwargs.get('target') == ble.sender)
            frames = queue.Queue()
            frames.put((ble.FRAME_MOUSE, b'\x01' + bytes(6)))
            with patch.object(ble.GLib, 'idle_add', side_effect=lambda *args: pending.put(args)):
                worker = real_thread(target=ble.sender, args=(frames, h.reports, h.loop, False),
                                     kwargs={'stopping': stopping}, daemon=True)
                workers.append(worker)
                worker.start()
                dispatch, *arguments = pending.get(timeout=1)

            def unregister(*_arguments):
                history.append(('unregister',))
                dispatch(*arguments)  # Simulate a queued callback during D-Bus teardown.
            h.gatt.UnregisterApplication.side_effect = unregister
            sigterm = next(call.args[2] for call in ble.GLib.unix_signal_add.call_args_list
                           if call.args[1] == ble.signal.SIGTERM)
            sigterm()

        self.run_helper(scenario)
        for worker in workers:
            worker.join(timeout=1)
            self.assertFalse(worker.is_alive())
        self.assertEqual(history, [('notify', 1, bytes(8)), ('notify', 2, bytes(7)),
                                   ('notify', 3, bytes(2)), ('unregister',)])

    def test_teardown_attempts_all_neutrals_and_remaining_unregisters_after_errors(self):
        def scenario(h):
            h.reports[1].notify.side_effect = ble.dbus.exceptions.DBusException('lost keyboard')
            h.reports[2].notify.side_effect = ble.dbus.exceptions.DBusException('lost mouse')
            h.gatt.UnregisterApplication.side_effect = ble.dbus.exceptions.DBusException('radio removed')
        h = self.run_helper(scenario)
        for kind, size in ((1, 8), (2, 7), (3, 2)):
            h.reports[kind].notify.assert_called_once_with(bytes(size))
        h.adv.UnregisterAdvertisement.assert_called_once()
        h.gatt.UnregisterApplication.assert_called_once()
        h.properties.Set.assert_not_called()


if __name__ == "__main__":
    unittest.main()
