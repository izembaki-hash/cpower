"""
Tests for CPower Local Vision & Multimodal Perception (tests/test_vision_local.py).
Verifies:
  1. VisualMark coordinate math and bounding box semantics.
  2. Strict Process Guard: protected windows (Antigravity/ZCode) cannot be captured.
  3. Computer Vision Contour & Element Detection on synthetic UI images.
  4. CPowerBrain priority resolution (HyperStream / Gemini / NIM / Semantic).
"""

import sys
import unittest
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent.parent / "clients" / "python"))

from cpower_vision_local import VisualMark, CPowerLocalVision
from cpower_brain import CPowerBrain
from cpower_process_guard import is_protected_window


class TestCPowerVisionLocal(unittest.TestCase):

    def test_visual_mark_math(self):
        m = VisualMark(1, 100, 200, 80, 40, label="button")
        self.assertEqual(m.mark_id, 1)
        self.assertEqual(m.cx, 140)
        self.assertEqual(m.cy, 220)
        self.assertEqual(m.to_dict()["bbox"], [100, 200, 80, 40])
        self.assertIn("[V1]", repr(m))

    def test_protected_window_guard(self):
        vision = CPowerLocalVision()
        # Mock or test with 0 / protected hwnd
        with self.assertRaises((PermissionError, RuntimeError)):
            # If 0 is passed or protected, it must refuse
            vision.capture_window_image(0)

    def test_cv_contour_detection_synthetic(self):
        vision = CPowerLocalVision()
        # Create a synthetic 800x600 UI image with dark header and 3 crisp buttons
        img = Image.new("RGB", (800, 600), color=(240, 240, 240))
        draw = ImageDraw.Draw(img)

        # Draw a simulated search bar (aspect > 4)
        draw.rectangle([100, 150, 500, 190], fill=(255, 255, 255), outline=(0, 0, 0), width=2)
        # Draw a simulated button 1
        draw.rectangle([100, 230, 220, 280], fill=(70, 130, 180), outline=(0, 0, 0), width=2)
        # Draw a simulated button 2
        draw.rectangle([250, 230, 370, 280], fill=(70, 130, 180), outline=(0, 0, 0), width=2)
        # Draw a simulated icon button (square)
        draw.rectangle([550, 150, 590, 190], fill=(200, 200, 200), outline=(0, 0, 0), width=2)

        window_bounds = (0, 0, 800, 600)
        marks = vision.detect_visual_elements(img, window_bounds, titlebar_guard_px=100)

        # Verify that interactive regions were discovered
        self.assertGreaterEqual(len(marks), 3)
        labels = [m.label for m in marks]
        self.assertTrue(any("input" in lbl or "button" in lbl for lbl in labels))

    def test_florence_grounding_live(self):
        vision = CPowerLocalVision()
        if not vision.is_local_model_ready():
            self.skipTest("Local Florence-2 weights not found.")

        img = Image.new("RGB", (600, 400), color=(255, 255, 255))
        draw = ImageDraw.Draw(img)
        draw.rectangle([100, 100, 300, 160], fill=(220, 220, 250), outline=(0, 0, 0), width=2)
        draw.text((120, 120), "Submit Form", fill=(0, 0, 0))

        bounds = (0, 0, 600, 400)
        coords = vision.ground_phrase_florence(img, "Submit Form", bounds)
        self.assertIsNotNone(coords)
        cx, cy = coords
        # Center should be roughly around (200, 130)
        self.assertTrue(90 <= cx <= 310)
        self.assertTrue(90 <= cy <= 170)

    def test_brain_semantic_fallback(self):
        brain = CPowerBrain(api_key=None)
        elements_summary = (
            "[1] <Hyperlink> 'Breaking News: Major Discovery in AI'\n"
            "[2] <Button> 'Search'\n"
            "[3] <Hyperlink> 'Privacy Policy'\n"
        )
        decision = brain.decide(
            goal="Read about AI discovery",
            elements_summary=elements_summary,
            current_url="https://news.example.com",
            history=[],
        )
        self.assertEqual(decision["action"], "click")
        self.assertEqual(decision["mark_id"], 1)


if __name__ == "__main__":
    unittest.main()
