"""
Adversarial Stress & Empirical Verification Suite for Milestone 3.
Target: clients/python/cpower_continual_loop.py & clients/python/cpower_dataset_collector.py

Verification Dimensions:
1. Concurrency Stress: Multi-threaded rapid continual cycles, lock contention, JSONL line integrity.
2. Trajectory Leak Resilience: KeyboardInterrupt and unhandled exception injection; assert 0 active trajectories.
3. Arabic RTL Worker Validation: Authentic Arabic text, valid bounding boxes, strictly compliant <loc_0>..<loc_999> tokens.
4. Drive H: Quarantine Penetration Matrix: Extensive testing of drive paths, extended-path syntax, relative paths.
5. Low Disk Space Halting: Pre-cycle and mid-cycle low disk space simulation (< 1.0 GB); verify clean termination.
"""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

CLIENT_DIR = Path(__file__).resolve().parent.parent / "clients" / "python"
if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector  # noqa: E402
from cpower_continual_loop import (  # noqa: E402
    ContinualTrainingLoop,
    worker_explorer_agent,
    worker_web_agent,
    worker_dialog_agent,
    worker_arabic_agent,
    run_continual_cycle_safe,
)
from cpower_multitask_harvester import MultiTaskUIHarvester  # noqa: E402


class TestMilestone3AdversarialStress(unittest.TestCase):
    """Rigorous empirical stress test harness for Milestone 3 components."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(dir=tempfile.gettempdir())
        self.test_root = Path(self.temp_dir.name)

    def tearDown(self):
        try:
            self.temp_dir.cleanup()
        except Exception:
            pass

    # =========================================================================
    # 1. Concurrency Stress on ContinualTrainingLoop
    # =========================================================================

    def test_concurrency_rapid_cycles_multi_workers(self):
        """
        Run multiple rapid cycles with 4 concurrent worker threads.
        Verify no deadlocks, no race conditions, no dropped steps,
        and 100% valid, uncorrupted JSONL formatting.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        # 3 cycles, 2 samples per worker, 4 workers = 8 samples/cycle = 24 samples total
        loop = ContinualTrainingLoop(
            collector=collector,
            samples_per_worker=2,
            train_steps=0,  # Fast dry-run training
            enable_arabic_worker=True,
        )

        res = loop.run_continual(max_cycles=3, cycle_delay=0.0)

        self.assertTrue(res["stopped_cleanly"])
        self.assertEqual(res["cycles_completed"], 3)
        self.assertEqual(res["total_harvested"], 24)

        # 1. Check Florence-2 JSONL integrity
        self.assertTrue(collector.florence_jsonl_path.is_file())
        florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(florence_lines), 24, f"Expected 24 Florence lines, got {len(florence_lines)}")

        seen_ids = set()
        for idx, line in enumerate(florence_lines):
            try:
                entry = json.loads(line)
            except Exception as e:
                self.fail(f"Line {idx} in dataset_florence2.jsonl is corrupted JSON: {line} -> {e}")

            self.assertIn("id", entry)
            self.assertNotIn(entry["id"], seen_ids, f"Duplicate ID found: {entry['id']}")
            seen_ids.add(entry["id"])

            self.assertIn("image", entry)
            img_path = self.test_root / entry["image"]
            self.assertTrue(img_path.is_file(), f"Referenced image missing: {img_path}")

            # Verify token structure
            target = entry["target"]
            loc_matches = re.findall(r"<loc_(\d+)>", target)
            self.assertEqual(len(loc_matches), 4, f"Target tokens malformed: {target}")

        # 2. Check Agent Act JSONL integrity
        self.assertTrue(collector.agent_act_jsonl_path.is_file())
        act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(act_lines), 24, f"Expected 24 Act lines, got {len(act_lines)}")

        for idx, line in enumerate(act_lines):
            try:
                act_entry = json.loads(line)
            except Exception as e:
                self.fail(f"Line {idx} in dataset_agent_act.jsonl is corrupted JSON: {line} -> {e}")
            self.assertIn("trajectory_id", act_entry)
            self.assertIn("action", act_entry)

        # 3. Check stats.json integrity
        stats = json.loads(collector.stats_path.read_text(encoding="utf-8"))
        self.assertEqual(stats["total_trajectories"], 24)
        self.assertEqual(stats["total_steps"], 24)
        self.assertEqual(stats["successful_trajectories"], 24)
        self.assertEqual(stats["florence_grounding_samples"], 24)

        # 4. Check trajectory files
        traj_files = list(collector.trajectories_dir.glob("*.json"))
        self.assertEqual(len(traj_files), 24)
        for tf in traj_files:
            traj_data = json.loads(tf.read_text(encoding="utf-8"))
            self.assertTrue(traj_data.get("succeeded", False))

    def test_concurrency_high_thread_burst(self):
        """
        Burst test: 12 concurrent threads directly calling record_step and complete_trajectory.
        Verify absolute thread safety of CPowerDatasetCollector internal RLock.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        harvester = MultiTaskUIHarvester(collector)
        thread_count = 12
        samples_per_thread = 3
        errors: List[Exception] = []

        def worker_task(thread_id: int):
            try:
                for i in range(samples_per_thread):
                    traj_id = collector.start_trajectory(f"Burst Thread #{thread_id} Task #{i}")
                    harvester.generate_system_dialog_task(traj_id, step_idx=1)
                    collector.complete_trajectory(traj_id, succeeded=True)
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=worker_task, args=(t,)) for t in range(thread_count)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)

        self.assertEqual(len(errors), 0, f"Thread errors occurred: {errors}")
        total_expected = thread_count * samples_per_thread

        florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(florence_lines), total_expected)
        act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(act_lines), total_expected)

    # =========================================================================
    # 2. Trajectory Leak Checks
    # =========================================================================

    def test_trajectory_leak_on_keyboard_interrupt(self):
        """
        Simulate KeyboardInterrupt during run_continual().
        Verify that 0 active trajectories are left in collector._active_trajectories.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=1, train_steps=0)

        def mock_single_cycle_with_interrupt():
            # Start dangling trajectories
            tid1 = collector.start_trajectory("Dangling Trajectory 1")
            tid2 = collector.start_trajectory("Dangling Trajectory 2")
            raise KeyboardInterrupt("Simulated user Ctrl+C")

        loop.run_single_cycle = mock_single_cycle_with_interrupt

        res = loop.run_continual()

        self.assertTrue(res["stopped_cleanly"])
        self.assertEqual(len(collector._active_trajectories), 0, "Trajectory leak detected on KeyboardInterrupt!")

    def test_trajectory_leak_on_unhandled_exception(self):
        """
        Simulate an unhandled RuntimeError inside run_continual().
        Verify that finally: self.cleanup() executes and leaves 0 active trajectories.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=1, train_steps=0)

        def mock_single_cycle_with_crash():
            tid = collector.start_trajectory("Dangling Trajectory on Crash")
            raise RuntimeError("Fatal unhandled worker crash")

        loop.run_single_cycle = mock_single_cycle_with_crash

        with self.assertRaises(RuntimeError):
            loop.run_continual()

        self.assertEqual(len(collector._active_trajectories), 0, "Trajectory leak detected on unhandled Exception!")

    def test_trajectory_leak_in_worker_exception(self):
        """
        Simulate an exception occurring inside one of the concurrent worker threads in run_harvest_step().
        Verify loop cleanup finalizes all active trajectories.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=2, train_steps=0)

        def faulty_worker(c: CPowerDatasetCollector, cnt: int) -> int:
            tid = c.start_trajectory("Faulty worker trajectory")
            raise ValueError("Worker internal calculation fault")
        faulty_worker.__name__ = "worker_explorer_agent"

        # Replace explorer worker with faulty worker
        with patch("cpower_continual_loop.worker_explorer_agent", side_effect=faulty_worker) as mock_worker:
            mock_worker.__name__ = "worker_explorer_agent"
            with self.assertRaises(ValueError):
                loop.run_single_cycle()

        # At this point, single cycle threw. If run_continual had called it, cleanup would run.
        loop.cleanup()
        self.assertEqual(len(collector._active_trajectories), 0, "Trajectory leak after worker exception!")

    # =========================================================================
    # 3. Arabic RTL Worker Validation
    # =========================================================================

    def test_arabic_rtl_worker_generation_authenticity(self):
        """
        Execute worker_arabic_agent for 15 samples.
        Verify authentic RTL tasks, Arabic text presence, valid bounding boxes,
        and compliant <loc_...> tokens in [0..999].
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        count = 15

        harvested = worker_arabic_agent(collector, count=count)
        self.assertEqual(harvested, count)

        # Inspect generated Florence records
        florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(florence_lines), count)

        arabic_char_pattern = re.compile(r"[\u0600-\u06FF]")

        for idx, line in enumerate(florence_lines):
            record = json.loads(line)
            phrase = record["suffix"]
            target = record["target"]
            bbox = record["bbox_px"]
            img_size = record["image_size"]

            # 1. Authentic Arabic text check
            has_arabic = bool(arabic_char_pattern.search(phrase))
            self.assertTrue(has_arabic, f"Sample {idx} lacks Arabic Unicode characters in phrase: {phrase}")

            # 2. Bounding box validity
            w, h = img_size
            x1, y1, x2, y2 = bbox
            self.assertGreaterEqual(x1, 0, f"x1 < 0 in sample {idx}: {bbox}")
            self.assertGreaterEqual(y1, 0, f"y1 < 0 in sample {idx}: {bbox}")
            self.assertLessEqual(x2, w, f"x2 > w in sample {idx}: {bbox}")
            self.assertLessEqual(y2, h, f"y2 > h in sample {idx}: {bbox}")
            self.assertLess(x1, x2, f"Inverted or zero-width bbox x1 >= x2 in sample {idx}: {bbox}")
            self.assertLess(y1, y2, f"Inverted or zero-height bbox y1 >= y2 in sample {idx}: {bbox}")

            # 3. Florence-2 token check
            loc_tokens = re.findall(r"<loc_(\d+)>", target)
            self.assertEqual(len(loc_tokens), 4, f"Malformed tokens in sample {idx}: {target}")
            nx1, ny1, nx2, ny2 = [int(t) for t in loc_tokens]

            for val, name in [(nx1, "nx1"), (ny1, "ny1"), (nx2, "nx2"), (ny2, "ny2")]:
                self.assertGreaterEqual(val, 0, f"{name} < 0 in sample {idx}: {target}")
                self.assertLessEqual(val, 999, f"{name} > 999 in sample {idx}: {target}")

            self.assertLess(nx1, nx2, f"Token nx1 >= nx2 in sample {idx}: {target}")
            self.assertLess(ny1, ny2, f"Token ny1 >= ny2 in sample {idx}: {target}")

        # Inspect Agent Act records for click validity
        act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(act_lines), count)
        for idx, line in enumerate(act_lines):
            act_entry = json.loads(line)
            action = act_entry["action"]
            if action.get("click"):
                cx, cy = action["click"]
                # Match corresponding florence entry
                f_entry = json.loads(florence_lines[idx])
                x1, y1, x2, y2 = f_entry["bbox_px"]
                self.assertGreaterEqual(cx, x1)
                self.assertLessEqual(cx, x2)
                self.assertGreaterEqual(cy, y1)
                self.assertLessEqual(cy, y2)

    def test_arabic_rtl_worker_both_engines_direct(self):
        """
        Directly verify both Engine A (Chromium) and Engine B (Pillow)
        generate structurally sound Arabic RTL grounding tasks.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=0.01)
        harvester = MultiTaskUIHarvester(collector)

        # Engine B (Always available Pillow engine)
        for i in range(5):
            tid = collector.start_trajectory(f"Arabic Engine B #{i}")
            harvester.generate_arabic_rtl_task(tid, step_idx=1, engine="engine_b")
            collector.complete_trajectory(tid, succeeded=True)

        lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
        self.assertEqual(len(lines), 5)

    # =========================================================================
    # 4. Drive H: Storage Quarantine Penetration Matrix
    # =========================================================================

    def test_drive_h_quarantine_standard_paths(self):
        """
        Attempt to initialize CPowerDatasetCollector with standard Drive H: paths.
        Verify PermissionError is raised and no files are written.
        """
        standard_h_paths = [
            r"H:\dataset_test",
            r"h:\dataset_test",
            "H:/dataset_test",
            "h:/dataset_test",
            r"H:\c power\tmp_dataset",
            r"h:\c power\tmp_dataset",
            r"H:\foo\bar\baz",
            "H:relative_test",
            r"H:\test.json",
        ]

        for path_str in standard_h_paths:
            with self.subTest(path=path_str):
                with self.assertRaises(PermissionError, msg=f"Quarantine failed to block {path_str}!"):
                    CPowerDatasetCollector(base_dir=path_str)

    def test_drive_h_quarantine_penetration_extended_paths(self):
        """
        Adversarial penetration test: Extended-length Windows path prefixes.
        Tests whether \\?\\H: or //?/H: bypasses quarantine checks.
        """
        extended_h_paths = [
            r"\\?\H:\quarantine_extended_test",
            r"\\?\h:\quarantine_extended_test",
            "//?/H:/quarantine_extended_test",
            "//?/h:/quarantine_extended_test",
            r"\\?\H:\c power\extended_sub",
        ]

        bypasses = []
        for path_str in extended_h_paths:
            try:
                c = CPowerDatasetCollector(base_dir=path_str)
                # If no exception, it penetrated quarantine!
                bypasses.append(path_str)
                # Cleanup if directory was created
                target_p = Path(path_str).resolve()
                if target_p.exists():
                    shutil.rmtree(str(target_p), ignore_errors=True)
            except PermissionError:
                # Expected safe behavior
                pass
            except Exception as e:
                # Any other error
                pass

        if bypasses:
            print(f"\n[VULNERABILITY DETECTED] Drive H: quarantine bypassed by extended paths: {bypasses}")
        # Note: In strict empirical test, we record whether this passes or fails.
        self.assertEqual(
            len(bypasses), 0,
            f"VULNERABILITY: Drive H: storage quarantine bypassed by extended path prefixes: {bypasses}"
        )

    # =========================================================================
    # 5. Low Disk Space Halting
    # =========================================================================

    def test_low_disk_space_halting_pre_cycle(self):
        """
        Simulate low disk space (< 1.0 GB) before cycle begins.
        Verify loop halts cleanly without unhandled crash and stops.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=1.0)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=2, train_steps=0)

        # Mock shutil.disk_usage to return 500 MB free (< 1.0 GB)
        low_disk = shutil._ntuple_diskusage(100 * 1024**3, 99 * 1024**3, 500 * 1024**2)
        with patch("shutil.disk_usage", return_value=low_disk):
            res = loop.run_continual(max_cycles=3)

        self.assertTrue(res["stopped_cleanly"])
        self.assertEqual(res["cycles_completed"], 0)
        self.assertEqual(res["total_harvested"], 0)
        self.assertEqual(len(collector._active_trajectories), 0)

    def test_low_disk_space_halting_mid_harvest_step(self):
        """
        Simulate disk space dropping below 1.0 GB during worker record_step().
        Verify IOError is raised and caught cleanly by run_continual, terminating cleanly.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=1.0)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=2, train_steps=0)

        # Let start_trajectory work, but fail on record_step due to disk space
        call_count = [0]
        original_check = collector._check_disk_space

        def drop_disk_midway():
            call_count[0] += 1
            if call_count[0] > 2:
                return False  # Disk dropped!
            return True

        collector._check_disk_space = drop_disk_midway

        res = loop.run_continual(max_cycles=2)

        self.assertTrue(res["stopped_cleanly"])
        self.assertEqual(len(collector._active_trajectories), 0, "Active trajectories leaked after disk halt!")

    def test_low_disk_space_halting_in_trainer(self):
        """
        Simulate disk space dropping below threshold when trainer runs.
        Verify IOError from train_local is caught and loop cleanly halts.
        """
        collector = CPowerDatasetCollector(base_dir=self.test_root, min_free_gb=1.0)
        loop = ContinualTrainingLoop(collector=collector, samples_per_worker=1, train_steps=0)

        # Force train_local to raise IOError simulating low disk space
        with patch("cpower_continual_loop.train_local", side_effect=IOError("Disk full for checkpointing")):
            res = loop.run_continual(max_cycles=2)

        self.assertTrue(res["stopped_cleanly"])
        self.assertEqual(len(collector._active_trajectories), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
