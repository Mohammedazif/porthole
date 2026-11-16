"""The live tap spike must stop before clicking when feedback is unsafe."""

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "spikes" / "ble_hid"))
import tap_test


class TapSafetyTests(unittest.TestCase):
    def setUp(self):
        self.frame = Image.new("RGB", (500, 1080))
        self.capture = patch.object(tap_test, "grab_xwd", return_value=self.frame).start()
        patch.object(tap_test, "window_id", return_value="0x1").start()
        patch.object(tap_test.time, "sleep").start()
        self.send = patch.object(tap_test, "send").start()
        self.addCleanup(patch.stopall)
        self.tapper = tap_test.Tapper("unused.log", "mirror", self.frame, (10, 650, 490, 1050))

    def assert_no_click(self):
        self.assertFalse(any(c.args[0] == "click" for c in self.send.call_args_list))

    def test_repeated_positioning_misses_do_not_click(self):
        with (patch.object(self.tapper, "anchor", return_value=(16, 16)),
              patch.object(tap_test, "changed_centre", return_value=(250, 700))):
            with self.assertRaises(tap_test.PositioningFailed):
                self.tapper.tap((200, 700))
        self.assert_no_click()

    def test_missing_pointer_does_not_click(self):
        with (patch.object(self.tapper, "anchor", return_value=(16, 16)),
              patch.object(tap_test, "changed_centre", return_value=None)):
            with self.assertRaises(tap_test.PositioningFailed):
                self.tapper.tap((200, 700))
        self.assert_no_click()

    def test_screen_change_during_positioning_does_not_click(self):
        def move(target):
            self.capture.return_value = Image.new("RGB", self.frame.size, "white")
            return target, 1

        with patch.object(self.tapper, "goto", side_effect=move):
            with self.assertRaises(tap_test.ScreenMismatch):
                self.tapper.tap((200, 700))
        self.assert_no_click()

    def test_resize_during_positioning_does_not_click(self):
        def move(target):
            self.capture.return_value = Image.new("RGB", (500, 1095))
            return target, 1

        with patch.object(self.tapper, "goto", side_effect=move):
            with self.assertRaises(tap_test.ScreenMismatch):
                self.tapper.tap((200, 700))
        self.assert_no_click()

    def test_stale_reference_is_rejected(self):
        with self.assertRaises(tap_test.ScreenMismatch):
            tap_test.Tapper("unused.log", "mirror", Image.new("RGB", (500, 1095)),
                            (10, 650, 490, 1050))
        self.assert_no_click()

    def test_invalid_target_sends_nothing(self):
        for target in ((-1, 700), (500, 700), (200, 1080), (float("nan"), 700)):
            with self.subTest(target=target):
                with self.assertRaises(ValueError):
                    self.tapper.tap(target)
        self.send.assert_not_called()

    def test_matching_screen_and_accurate_position_click_once(self):
        with (patch.object(self.tapper, "anchor", return_value=(16, 16)),
              patch.object(tap_test, "changed_centre", return_value=(202, 701)),
              patch("builtins.print")):
            self.tapper.tap((200, 700))
        self.assertEqual(sum(c.args[0] == "click" for c in self.send.call_args_list), 1)


if __name__ == "__main__":
    unittest.main()
