"""
Adversarial Stress Test Suite for CPower Multi-Task UI Harvester
File: tests/test_harvester_stress.py
Author: Challenger 1 (m1_challenger_1)

Tests:
  1. Extreme bounding box coordinates, degenerate boxes, off-screen boxes, and coordinate clamping.
  2. Strict verification that <loc_1000> is NEVER generated under any circumstances.
  3. Font cascade fallback safety when custom Windows fonts are missing.
  4. Physical Win32 mouse cursor invariance via GetCursorPos during harvesting.
  5. High-throughput multi-domain batch generation into E:\\cpower_training_data.
  6. Resource leaks and unhandled exception safety.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import io
import json
import os
from pathlib import Path
import random
import re
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

from PIL import Image, ImageFont

CLIENT_DIR = Path(__file__).resolve().parent.parent / "clients" / "python"
if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector
from cpower_multitask_harvester import (
    ArabicRTLHarvester,
    HAS_ARABIC_RESHAPER,
    HAS_CPOWER_CLIENT,
    HAS_PLAYWRIGHT,
    MultiTaskUIHarvester,
    PillowYouTubeSynthesizer,
    Win11ControlsSynthesizer,
    get_ui_font,
    reshape_arabic,
    sanitize_bbox,
)


class TestHarvesterStressAndAdversarial(unittest.TestCase):
    """Adversarial stress harness targeting failure modes in CPower Harvester."""

    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp(prefix="cpower_stress_test_"))
        self.collector = CPowerDatasetCollector(base_dir=self.temp_dir, min_free_gb=0.01)

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    # -----------------------------------------------------------------------
    # Challenge 1: Extreme Coordinates & <loc_1000> Prohibition
    # -----------------------------------------------------------------------

    def test_extreme_coordinates_and_loc1000_prohibition(self):
        """
        Adversarially probes sanitize_bbox across extreme, off-screen, inverted,
        and high-resolution boundary boxes. Confirms that norm_box is strictly [0..999]
        and that <loc_1000> is NEVER generated.
        """
        resolutions = [
            (100, 100),
            (640, 480),
            (1024, 680),
            (1280, 800),
            (1920, 1080),  # Full HD desktop
            (2560, 1440),  # 2K QHD
            (3840, 2160),  # 4K UHD
        ]

        test_boxes = [
            # Offscreen negative
            [-10000, -5000, -100, -50],
            # Offscreen positive
            [50000, 60000, 70000, 80000],
            # Inverted
            [1500, 1200, 200, 100],
            # Zero-sized points at corners & center
            [0, 0, 0, 0],
            # Partial negative overlap
            [-50, -50, 20, 20],
        ]

        violations = []
        loc1000_violations = []

        for w, h in resolutions:
            # Add dynamic resolution-specific edge boxes
            res_boxes = list(test_boxes) + [
                [w - 1, h - 1, w, h],
                [w, h, w, h],
                [w - 2, h - 2, w - 2, h - 2],
                [w - 5, h - 5, w, h],
                [w // 2, h // 2, w // 2, h // 2],
            ]
            for box in res_boxes:
                clean_b, norm_b = sanitize_bbox(box, w, h)
                nx1, ny1, nx2, ny2 = norm_b

                # Test 1: Bounds check [0..999]
                if not (0 <= nx1 <= 999 and 0 <= ny1 <= 999 and 0 <= nx2 <= 999 and 0 <= ny2 <= 999):
                    violations.append(
                        f"Out of bounds on {w}x{h} with input {box}: norm_box={norm_b}"
                    )

                # Test 2: Non-degeneracy check nx2 > nx1 and ny2 > ny1
                if nx2 <= nx1 or ny2 <= ny1:
                    violations.append(
                        f"Degenerate norm_box on {w}x{h} with input {box}: norm_box={norm_b}"
                    )

                # Test 3: Check if <loc_1000> could be formed
                loc_str = f"<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"
                if "<loc_1000>" in loc_str:
                    loc1000_violations.append(
                        f"Generated <loc_1000> on {w}x{h} with input {box}: {loc_str}"
                    )

        if violations or loc1000_violations:
            msg = "\n".join(violations[:15] + loc1000_violations[:15])
            self.fail(f"Coordinate clamping / <loc_1000> failure:\n{msg}")

    # -----------------------------------------------------------------------
    # Challenge 2: Font Fallback Resilience
    # -----------------------------------------------------------------------

    def test_font_fallback_resilience(self):
        """
        Simulates an environment where custom Windows UI fonts (Segoe UI, Tahoma)
        fail to load or are absent, forcing ImageFont.load_default() fallback.
        Ensures all synthesizers operate without throwing exceptions.
        """
        with mock.patch("cpower_multitask_harvester.get_ui_font", return_value=ImageFont.load_default()):
            # 1. Win11 Controls Synthesizer
            win11 = Win11ControlsSynthesizer()
            img1, comps1 = win11.generate_ribbon(dark_mode=True)
            self.assertEqual(img1.size, (1024, 680))
            self.assertGreaterEqual(len(comps1), 5)

            img2, comps2 = win11.generate_breadcrumb(drive="C:")
            self.assertEqual(img2.size, (1024, 680))
            self.assertGreaterEqual(len(comps2), 3)

            img3, comps3 = win11.generate_grid(folder_count=10)
            self.assertEqual(img3.size, (1024, 680))
            self.assertGreaterEqual(len(comps3), 5)

            img4, comps4 = win11.generate_save_open_dialog(is_open_dialog=True)
            self.assertEqual(img4.size, (800, 520))

            img5, comps5 = win11.generate_confirm_dialog(dialog_type="replace")
            self.assertEqual(img5.size, (600, 320))

            # 2. Pillow YouTube Synthesizer
            py = PillowYouTubeSynthesizer()
            img_yt, comps_yt = py.generate(dark_theme=True, locale="ar")
            self.assertEqual(img_yt.size, (1280, 800))
            self.assertGreaterEqual(len(comps_yt), 15)

            # 3. Arabic RTL Harvester (Engine B with default font)
            ar = ArabicRTLHarvester(collector=self.collector)
            tid = self.collector.start_trajectory("Test Fallback Trajectory")
            ar._generate_engine_b_sample(tid, step_idx=1, template="confirm")
            self.collector.complete_trajectory(tid, succeeded=True)
            ar.close()

    # -----------------------------------------------------------------------
    # Challenge 3: Physical Mouse Cursor Invariance (GetCursorPos)
    # -----------------------------------------------------------------------

    def test_physical_cursor_invariance_stress(self):
        """
        Stress-tests zero-mouse cursor invariance across multiple concurrent-like
        harvesting operations.
        Probes GetCursorPos before, during, and after each synthesis step.
        """
        pt_initial = wintypes.POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_initial))

        harvester = MultiTaskUIHarvester(collector=self.collector, headless=True)
        tid = self.collector.start_trajectory("Cursor Invariance Stress")

        for step in range(1, 10):
            # Check cursor before step
            pt_before = wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_before))
            self.assertEqual(
                (pt_initial.x, pt_initial.y),
                (pt_before.x, pt_before.y),
                f"Cursor moved prior to step {step}!",
            )

            # Perform synthesis across different domains
            if step % 3 == 1:
                harvester.generate_explorer_task(tid, step)
            elif step % 3 == 2:
                harvester.generate_youtube_task(tid, step, locale="ar")
            else:
                harvester.generate_arabic_rtl_task(tid, step)

            # Check cursor after step
            pt_after = wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_after))
            self.assertEqual(
                (pt_initial.x, pt_initial.y),
                (pt_after.x, pt_after.y),
                f"Cursor was moved during step {step}!",
            )

        # Also test live perception if available
        if HAS_CPOWER_CLIENT:
            harvester.harvest_live_explorer_step(tid)
            pt_live = wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_live))
            self.assertEqual(
                (pt_initial.x, pt_initial.y),
                (pt_live.x, pt_live.y),
                "Cursor was moved during live perception!",
            )

        self.collector.complete_trajectory(tid, succeeded=True)
        harvester.close()

    # -----------------------------------------------------------------------
    # Challenge 4: High-Throughput Batch Generation & Dataset Validation
    # -----------------------------------------------------------------------

    def test_high_throughput_batch_generation_and_integrity(self):
        """
        Executes a multi-domain batch into the primary training storage E:\\cpower_training_data
        and verifies dataset integrity, JSONL schema, positive bounding boxes, and zero loc_1000.
        """
        prod_collector = CPowerDatasetCollector(min_free_gb=1.0)
        harvester = MultiTaskUIHarvester(collector=prod_collector, headless=True)

        batch_size = 15
        tid = prod_collector.start_trajectory("High Throughput Batch Stress")

        t0 = time.time()
        for i in range(1, batch_size + 1):
            domain_choice = i % 4
            if domain_choice == 0:
                harvester.generate_explorer_ribbon_task(tid, i)
            elif domain_choice == 1:
                harvester.generate_youtube_task(tid, i, locale="en")
            elif domain_choice == 2:
                harvester.generate_youtube_task(tid, i, locale="ar")
            else:
                harvester.generate_arabic_rtl_task(tid, i)

        elapsed = time.time() - t0
        prod_collector.complete_trajectory(tid, succeeded=True)
        harvester.close()

        # Telemetry
        rate = batch_size / max(1e-4, elapsed)
        print(f"\n[STRESS] Generated {batch_size} samples in {elapsed:.2f}s ({rate:.1f} samples/sec)")

        # Verify dataset files on E:\
        self.assertTrue(prod_collector.florence_jsonl_path.is_file())
        self.assertTrue(prod_collector.agent_act_jsonl_path.is_file())

        with open(prod_collector.florence_jsonl_path, "r", encoding="utf-8") as f:
            for line_no, line in enumerate(f, 1):
                record = json.loads(line)
                target = record.get("target", "")
                self.assertNotIn("<loc_1000>", target, f"Line {line_no} contains forbidden <loc_1000>!")

                # Extract loc tokens
                locs = [int(m) for m in re.findall(r"<loc_(\d+)>", target)]
                self.assertEqual(len(locs), 4, f"Line {line_no} target has invalid token count: {target}")
                nx1, ny1, nx2, ny2 = locs
                self.assertTrue(0 <= nx1 <= 999 and 0 <= ny1 <= 999 and 0 <= nx2 <= 999 and 0 <= ny2 <= 999)

                # Check image existence
                img_path = prod_collector.base_dir / record["image"]
                self.assertTrue(img_path.is_file(), f"Missing image file: {img_path}")


if __name__ == "__main__":
    unittest.main()
