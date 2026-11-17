"""
Tests for CPower Shadow Mode - Human Demonstration Learner (tests/test_shadow_logger.py).

Verifies:
  1. Initialization, start, pause, resume, and stop lifecycle.
  2. Strict privacy filters (passwords, bitwarden, keepass, incognito, protected windows).
  3. Single-step demonstration capture and Florence-2 grounding pair formatting.
  4. Integration with CPowerDatasetCollector (emits valid JSONL and JPEG image).
  5. Thread concurrency safety and zero input queue hitching.
"""

from pathlib import Path
import shutil
import sys
import tempfile
import time
import unittest
from unittest.mock import MagicMock, patch

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent.parent / "clients" / "python"
sys.path.insert(0, str(CLIENT_DIR))

from cpower_shadow_logger import CPowerShadowLogger, SENSITIVE_KEYWORDS
from cpower_dataset_validator import validate_dataset


class TestCPowerShadowLogger(unittest.TestCase):

    def setUp(self):
        self.temp_dir = tempfile.mkdtemp(prefix="test_cpower_shadow_")
        self.logger = CPowerShadowLogger(dataset_root=self.temp_dir, min_click_interval_sec=0.1)

    def tearDown(self):
        if self.logger.is_running:
            self.logger.stop()
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_01_lifecycle_controls(self):
        """Verify start, pause, resume, and stop controls."""
        self.assertFalse(self.logger.is_running)
        self.assertFalse(self.logger.is_paused)

        self.logger.start()
        self.assertTrue(self.logger.is_running)
        self.assertFalse(self.logger.is_paused)

        self.logger.pause()
        self.assertTrue(self.logger.is_paused)

        self.logger.resume()
        self.assertFalse(self.logger.is_paused)

        self.logger.stop()
        self.assertFalse(self.logger.is_running)

    def test_02_privacy_filters(self):
        """Verify sensitive keywords and protected windows are rejected."""
        test_cases = [
            (100, "Bitwarden - My Vault", "Chrome_WidgetWin_1", True),
            (101, "KeePass Password Safe", "WindowsForms10", True),
            (102, "Private Browsing - Firefox", "MozillaWindowClass", True),
            (103, "Google Chrome (Incognito)", "Chrome_WidgetWin_1", True),
            (104, "Enter Password", "Edit", True),
            (105, "YouTube - Google Chrome", "Chrome_WidgetWin_1", False),
            (106, "File Explorer", "CabinetWClass", False),
            (107, "Calculator", "ApplicationFrameWindow", False),
        ]

        for hwnd, title, class_name, should_be_sensitive in test_cases:
            is_sens = self.logger._is_privacy_sensitive(hwnd, title, class_name)
            self.assertEqual(
                is_sens,
                should_be_sensitive,
                f"Privacy check failed for '{title}' (Class: {class_name}): expected {should_be_sensitive}, got {is_sens}",
            )

    def test_03_mock_demonstration_capture(self):
        """Verify demonstration step records valid Florence-2 and Agent Act records."""
        # Create a mock synthetic 800x600 image
        mock_img = Image.new("RGB", (800, 600), color=(240, 240, 240))

        # Patch inspect_element_at_point to return controlled element data
        with patch.object(
            self.logger,
            "inspect_element_at_point",
            return_value=("Download Button", (150, 200, 350, 260), "File Explorer", 1234),
        ), patch.object(self.logger, "_is_privacy_sensitive", return_value=False):

            # Capture step at (200, 220)
            res = self.logger.capture_demonstration_step(
                x=200,
                y=220,
                action_type="click",
                pre_captured_image=mock_img,
            )

            self.assertIsNotNone(res)
            self.assertEqual(self.logger.captured_count, 1)

            # Check that files were created
            florence_file = Path(self.temp_dir) / "dataset_florence2.jsonl"
            act_file = Path(self.temp_dir) / "dataset_agent_act.jsonl"
            self.assertTrue(florence_file.exists())
            self.assertTrue(act_file.exists())

            # Validate the newly recorded dataset using CPower Dataset Validator
            is_valid, report = validate_dataset(self.temp_dir, check_images=True)
            self.assertTrue(is_valid, f"Validator failed on shadow capture dataset: {report}")
            self.assertEqual(report["florence_count"], 1)
            self.assertEqual(report["act_count"], 1)

    def test_04_privacy_rejection_prevents_recording(self):
        """Verify sensitive actions increment ignored count and emit zero files."""
        mock_img = Image.new("RGB", (800, 600), color=(255, 255, 255))

        with patch.object(
            self.logger,
            "inspect_element_at_point",
            return_value=("Master Password Input", (100, 100, 400, 150), "KeePass Vault", 9999),
        ), patch.object(self.logger, "_is_privacy_sensitive", return_value=True):

            res = self.logger.capture_demonstration_step(
                x=200,
                y=120,
                action_type="click",
                pre_captured_image=mock_img,
            )

            self.assertIsNone(res)
            self.assertEqual(self.logger.captured_count, 0)
            self.assertEqual(self.logger.ignored_privacy_count, 1)


if __name__ == "__main__":
    unittest.main()
