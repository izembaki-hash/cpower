"""
Challenger 2 Deep Adversarial Verification and Stress Suite (Milestone 1 Iteration 2).
Author: Challenger 2 (Empirical Challenger)
"""

import concurrent.futures
import json
import math
from pathlib import Path
import random
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image
from transformers import AutoProcessor

from clients.python.cpower_dataset_collector import CPowerDatasetCollector
from clients.python.cpower_multitask_harvester import sanitize_bbox


DATASET_DIR = Path(r"E:\cpower_training_data")
FLORENCE_JSONL = DATASET_DIR / "dataset_florence2.jsonl"
AGENT_ACT_JSONL = DATASET_DIR / "dataset_agent_act.jsonl"
STATS_JSON = DATASET_DIR / "stats.json"
MODEL_DIR = Path(r"C:\Users\Administrator\.models\florence-2-base")


class ChallengerDeepAdversarialTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.processor = AutoProcessor.from_pretrained(str(MODEL_DIR), trust_remote_code=True)

        with open(FLORENCE_JSONL, "r", encoding="utf-8") as f:
            cls.florence_records = [json.loads(line) for line in f if line.strip()]

        with open(AGENT_ACT_JSONL, "r", encoding="utf-8") as f:
            cls.act_records = [json.loads(line) for line in f if line.strip()]

    def test_01_florence2_dequantization_forensic_distribution(self):
        """Audit exact pixel discrepancy distribution for all dataset samples."""
        max_error = 0.0
        total_error = 0.0
        count = 0
        error_above_threshold = []

        for idx, entry in enumerate(self.florence_records):
            w, h = entry["image_size"]
            x1, y1, x2, y2 = entry["bbox_px"]
            target = entry["target"]

            res = self.processor.post_process_generation(
                target,
                task="<CAPTION_TO_PHRASE_GROUNDING>",
                image_size=(w, h),
            )
            parsed_boxes = res.get("<CAPTION_TO_PHRASE_GROUNDING>", {}).get("bboxes", [])
            self.assertTrue(len(parsed_boxes) > 0, f"Line {idx+1}: failed to parse bboxes from target {target}")

            bx1, by1, bx2, by2 = parsed_boxes[0]
            err = max(abs(bx1 - x1), abs(by1 - y1), abs(bx2 - x2), abs(by2 - y2))

            if err >= 1.0:
                error_above_threshold.append((idx + 1, err, [x1, y1, x2, y2], [bx1, by1, bx2, by2], target))

            max_error = max(max_error, err)
            total_error += err
            count += 1

        mean_error = total_error / count if count > 0 else 0.0
        print(f"\n[FORENSIC] Florence-2 Dequantization: N={count}, Max Error={max_error:.4f}px, Mean Error={mean_error:.4f}px")
        self.assertEqual(len(error_above_threshold), 0, f"Samples exceeding 1.0px: {error_above_threshold}")
        self.assertLess(max_error, 1.0, f"Max error {max_error} exceeds 1.0px floor threshold")

    def test_02_id_uniqueness_and_utf8_arabic_preservation(self):
        """Verify 100% ID uniqueness and proper Arabic UTF-8 text serialization (no escaped slop)."""
        florence_ids = [e["id"] for e in self.florence_records]
        self.assertEqual(len(florence_ids), len(set(florence_ids)), "Duplicate IDs found in Florence dataset!")

        arabic_found = 0
        for entry in self.florence_records:
            suffix = entry.get("suffix", "")
            target = entry.get("target", "")
            # Check for non-escaped Arabic characters (Unicode range \u0600-\u06FF)
            if any("\u0600" <= c <= "\u06ff" for c in suffix):
                arabic_found += 1
                # Ensure no raw unicode escapes like \u062a in target
                self.assertNotIn(r"\u06", target, "Found escaped unicode string in target")

        print(f"[FORENSIC] Arabic UI Records Verified: {arabic_found} records contain verified raw Arabic text.")
        self.assertGreater(arabic_found, 10, "Expected at least 10 Arabic records in dataset")

    def test_03_action_clicks_strict_containment_all_types(self):
        """Verify containment across click, double_click, and right_click without exception."""
        florence_map = {e["image"]: e for e in self.florence_records}
        checked = 0

        for entry in self.act_records:
            act = entry.get("action", {})
            act_type = act.get("type")
            if act_type in ("click", "double_click", "right_click"):
                click = act.get("click")
                self.assertIsNotNone(click, f"Null click for {act_type} in {entry.get('trajectory_id')}")
                img = entry.get("image")
                f_entry = florence_map.get(img)
                self.assertIsNotNone(f_entry, f"No matching Florence entry for {img}")

                x1, y1, x2, y2 = f_entry["bbox_px"]
                cx, cy = click
                self.assertTrue(
                    x1 <= cx <= x2 and y1 <= cy <= y2,
                    f"Click ({cx}, {cy}) outside bbox [{x1}, {y1}, {x2}, {y2}] for {img}",
                )
                checked += 1

        print(f"[FORENSIC] Action Clicks Strictly Contained: {checked} click actions verified inside bbox.")

    def test_04_sanitize_bbox_extreme_adversarial_stress(self):
        """Stress-test sanitize_bbox with 10,000 extreme, boundary, and inverted inputs."""
        test_resolutions = [
            (1920, 1080),
            (1366, 768),
            (1280, 800),
            (800, 600),
            (3840, 2160),
            (2560, 1440),
            (100, 100),
        ]

        violations = []
        random.seed(42)

        for w, h in test_resolutions:
            # Boundary extreme cases
            edge_cases = [
                (0, 0, 0, 0),
                (0, 0, 1, 1),
                (w - 1, h - 1, w, h),
                (w - 2, h - 2, w - 1, h - 1),
                (0, 0, w, h),
                (-500, -500, -100, -100),
                (w + 100, h + 100, w + 500, h + 500),
                (w, h, 0, 0),  # Inverted
                (w // 2, h // 2, w // 2, h // 2),  # Zero area
            ]

            # Generate 1,000 random boxes per resolution
            for _ in range(1000):
                rx1 = random.randint(-100, w + 100)
                ry1 = random.randint(-100, h + 100)
                rx2 = random.randint(-100, w + 100)
                ry2 = random.randint(-100, h + 100)
                edge_cases.append((rx1, ry1, rx2, ry2))

            for box in edge_cases:
                (x1, y1, x2, y2), (nx1, ny1, nx2, ny2) = sanitize_bbox(box, w, h)

                # Check pixel box
                if not (0 <= x1 < x2 <= w):
                    violations.append((box, (w, h), (x1, y1, x2, y2), "Pixel x invalid"))
                if not (0 <= y1 < y2 <= h):
                    violations.append((box, (w, h), (x1, y1, x2, y2), "Pixel y invalid"))

                # Check normalized tokens
                if not (0 <= nx1 < nx2 <= 999):
                    violations.append((box, (w, h), (nx1, ny1, nx2, ny2), "Norm nx invalid"))
                if not (0 <= ny1 < ny2 <= 999):
                    violations.append((box, (w, h), (nx1, ny1, nx2, ny2), "Norm ny invalid"))

                if nx2 >= 1000 or ny2 >= 1000:
                    violations.append((box, (w, h), (nx1, ny1, nx2, ny2), "Token >= 1000 forbidden"))

        self.assertEqual(len(violations), 0, f"Sanitize bbox violations: {violations[:5]}")
        print(f"[FORENSIC] Sanitize BBox Adversarial Stress: 7,063 extreme inputs verified (0 violations, 0 <loc_1000>).")

    def test_05_multithreaded_collector_concurrency_stress(self):
        """Stress test CPowerDatasetCollector under high concurrent load with 10 threads."""
        temp_dir = Path(tempfile.mkdtemp(prefix="cpower_stress_test_"))
        try:
            collector = CPowerDatasetCollector(base_dir=str(temp_dir), min_free_gb=0.01)

            dummy_img = Image.new("RGB", (640, 480), color=(100, 150, 200))
            num_threads = 10
            steps_per_thread = 15
            total_expected_steps = num_threads * steps_per_thread

            def worker_fn(worker_id):
                traj_id = collector.start_trajectory(f"Concurrent goal {worker_id}")
                for s in range(steps_per_thread):
                    collector.record_step(
                        trajectory_id=traj_id,
                        image=dummy_img,
                        action_type="click",
                        phrase=f"worker_{worker_id}_btn_{s}",
                        bbox=(10 + s * 2, 20 + s * 2, 80 + s * 2, 90 + s * 2),
                        click_coords=None,  # Should auto-derive centroid
                    )
                collector.complete_trajectory(traj_id, succeeded=True)

            with concurrent.futures.ThreadPoolExecutor(max_workers=num_threads) as executor:
                futures = [executor.submit(worker_fn, i) for i in range(num_threads)]
                for f in concurrent.futures.as_completed(futures):
                    f.result()  # Surface any thread exceptions

            # Check JSONL row counts
            with open(temp_dir / "dataset_florence2.jsonl", "r", encoding="utf-8") as f:
                f_lines = [line for line in f if line.strip()]
            with open(temp_dir / "dataset_agent_act.jsonl", "r", encoding="utf-8") as f:
                a_lines = [line for line in f if line.strip()]

            with open(temp_dir / "stats.json", "r", encoding="utf-8") as f:
                stats = json.load(f)

            self.assertEqual(len(f_lines), total_expected_steps, f"Florence line count mismatch under 10 threads")
            self.assertEqual(len(a_lines), total_expected_steps, f"Act line count mismatch under 10 threads")
            self.assertEqual(stats["total_steps"], total_expected_steps)
            self.assertEqual(stats["florence_grounding_samples"], total_expected_steps)
            self.assertEqual(stats["total_trajectories"], num_threads)

            print(f"[FORENSIC] Concurrency Stress Passed: {total_expected_steps} records written across {num_threads} concurrent threads with zero record loss.")
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
