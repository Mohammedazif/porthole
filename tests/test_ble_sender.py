"""Queued HID reports must stay paced even when the GLib loop is busy."""
import importlib.util
import queue
import subprocess
import sys
import threading
import unittest
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
