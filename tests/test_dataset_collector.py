"""
Unit Tests for CPower Autonomous Trajectory & Dataset Collector (tests/test_dataset_collector.py).

Verifies:
  1. Initialization in a sandboxed temporary directory.
  2. Trajectory lifecycle: start, record steps with bounding boxes, complete.
  3. Florence-2 coordinate normalization [0..1000] and <loc_x1><loc_y1><loc_x2><loc_y2> formatting.
  4. Integration with Florence-2 processor post_process_generation.
  5. JSONL append integrity and stats tracking.
  6. PyTorch Dataset loader script emission.
"""

import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent.parent / "clients" / "python"))

from cpower_dataset_collector import CPowerDatasetCollector


class TestCPowerDatasetCollector(unittest.TestCase):

    def setUp(self):
        self.test_dir = Path(tempfile.mkdtemp(prefix="cpower_dataset_test_"))
        self.collector = CPowerDatasetCollector(base_dir=self.test_dir, min_free_gb=0.01)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_directory_initialization(self):
        self.assertTrue(self.collector.images_dir.is_dir())
        self.assertTrue(self.collector.trajectories_dir.is_dir())
        self.assertTrue(self.collector.stats_path.is_file())

    def test_record_step_and_florence_formatting(self):
        # Create a synthetic 1000x500 image with a target button at [100, 50, 400, 150]
        img = Image.new("RGB", (1000, 500), color=(240, 240, 240))
        draw = ImageDraw.Draw(img)
        draw.rectangle([100, 50, 400, 150], fill=(50, 100, 200), outline=(0, 0, 0))

        traj_id = self.collector.start_trajectory(goal="Click Search Button", window_title="Test Window")
        self.assertTrue(traj_id.startswith("traj_"))

        phrase = "Search Button"
        bbox = (100, 50, 400, 150)
        step = self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="click",
            phrase=phrase,
            bbox=bbox,
            click_coords=(250, 100),
            window_title="Test Window",
        )

        # Coordinate math check:
        # img_w = 1000 -> x1: 100/1000 * 1000 = 100, x2: 400/1000 * 1000 = 400
        # img_h = 500  -> y1: 50/500 * 1000 = 100,  y2: 150/500 * 1000 = 300
        self.assertEqual(step["normalized_bbox"], [100, 100, 400, 300])
        expected_target = "Search Button<loc_100><loc_100><loc_400><loc_300>"
        self.assertEqual(step["florence_sample"]["target"], expected_target)

        # Verify image file was written
        saved_img_path = self.test_dir / step["image_path"]
        self.assertTrue(saved_img_path.is_file())

        # Complete trajectory
        out_file = self.collector.complete_trajectory(traj_id, succeeded=True)
        self.assertTrue(out_file.is_file())

        # Verify JSONL lines exist and are valid
        florence_lines = self.collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(florence_lines), 1)
        entry = json.loads(florence_lines[0])
        self.assertEqual(entry["target"], expected_target)

    def test_florence_post_processing_compatibility(self):
        """Verifies that the generated target string is parsed cleanly by Florence-2 processor."""
        weights_dir = Path("C:/Users/Administrator/.models/florence-2-base")
        if not weights_dir.is_dir():
            self.skipTest("Florence-2 weights not present.")

        from transformers import AutoProcessor

        processor = AutoProcessor.from_pretrained(str(weights_dir), trust_remote_code=True)
        target_str = "Submit Form<loc_100><loc_100><loc_300><loc_160>"

        parsed = processor.post_process_generation(
            target_str,
            task="<CAPTION_TO_PHRASE_GROUNDING>",
            image_size=(1000, 1000),
        )
        grounding = parsed.get("<CAPTION_TO_PHRASE_GROUNDING>", {})
        bboxes = grounding.get("bboxes", [])
        self.assertGreaterEqual(len(bboxes), 1)
        # BBox should match [100, 100, 300, 160] (with .5 quantization offsets)
        b = bboxes[0]
        self.assertAlmostEqual(b[0], 100.5, delta=1.0)
        self.assertAlmostEqual(b[1], 100.5, delta=1.0)
        self.assertAlmostEqual(b[2], 300.5, delta=1.0)
        self.assertAlmostEqual(b[3], 160.5, delta=1.0)

    def test_stats_and_loader_emission(self):
        loader_path = self.collector.generate_training_loader_script()
        self.assertTrue(loader_path.is_file())
        self.assertIn("CPowerFlorenceDataset", loader_path.read_text(encoding="utf-8"))

        summary = self.collector.get_dataset_summary()
        self.assertIn("free_space_gb", summary)
        self.assertEqual(summary["total_trajectories"], 0)


if __name__ == "__main__":
    unittest.main()
