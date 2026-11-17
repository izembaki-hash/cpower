"""
Exhaustive Adversarial Coordinate Probe and Harvester Stress Test
File: tests/test_adversarial_coordinate_probes.py
Author: Challenger 1 (m1_challenger_r2_1)

Empirically stress-tests coordinate boundary conditions, edge cases,
non-degeneracy, <loc_1000> prohibition, pixel coordinate mirroring,
Win32 mouse cursor invariance, and font fallback robustness in CPower Harvester.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import json
import math
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


class TestAdversarialCoordinateProbes(unittest.TestCase):
    """Deep adversarial testing of coordinate sanitization and harvester integrity."""

    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp(prefix="cpower_coord_probe_"))
        self.collector = CPowerDatasetCollector(base_dir=self.temp_dir, min_free_gb=0.01)

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_exhaustive_resolution_and_boundary_probes(self):
        """
        Tests sanitize_bbox across all mandated resolutions:
        800x600, 1024x768, 1280x720, 1280x800, 1366x768, 1920x1080, 2560x1440, 3840x2160,
        plus 1x1, 100x100, 7680x4320 (8K).
        
        Checks:
        1. <loc_1000> is NEVER generated.
        2. Strictly non-degenerate: 0 <= nx1 < nx2 <= 999 and 0 <= ny1 < ny2 <= 999.
        3. Mirrored pixel coords: 0 <= x1 < x2 <= w and 0 <= y1 < y2 <= h.
        """
        resolutions = [
            (800, 600),
            (1024, 768),
            (1280, 720),
            (1280, 800),
            (1366, 768),
            (1920, 1080),  # 1080p Full HD
            (2560, 1440),  # 2K QHD
            (3840, 2160),  # 4K UHD
            (100, 100),
            (7680, 4320),  # 8K
            (1, 1),
            (2, 2),
        ]

        static_probes = [
            # Extreme offscreen
            [50000, 60000, 70000, 80000],
            [-50000, -60000, -70000, -80000],
            [1000000, 1000000, 2000000, 2000000],
            [-2000000, -2000000, -1000000, -1000000],
            # Origin zero points
            [0, 0, 0, 0],
            [0, 0, 1, 1],
            [-10, -10, 0, 0],
            # Inverted extreme
            [90000, 90000, 100, 100],
            [-100, -100, -50000, -50000],
        ]

        total_probes_run = 0
        violations = []

        for w, h in resolutions:
            # Generate dynamic boundary and edge probes
            dynamic_probes = list(static_probes)
            dynamic_probes.extend([
                # Boundary edges
                [w - 1, h - 1, w, h],
                [w, h, w, h],
                [w, h, w + 100, h + 100],
                [w - 1, 0, w, 1],
                [0, h - 1, 1, h],
                # Sub-pixel points
                [w - 2, h - 2, w - 2, h - 2],
                [w - 1, h - 1, w - 1, h - 1],
                [w - 3, h - 3, w - 3, h - 3],
                # Inverted near boundary
                [w, h, w - 1, h - 1],
                [w, h, 0, 0],
                [w + 10, h + 10, -10, -10],
                # Float sub-pixel edges
                [w - 0.999, h - 0.999, w - 0.001, h - 0.001],
                [w - 1.5, h - 1.5, w - 0.5, h - 0.5],
                [0.001, 0.001, 0.999, 0.999],
            ])

            # Dense sweeps around edges
            for dx in range(-6, 7):
                for dy in range(-6, 7):
                    dynamic_probes.append([w + dx, h + dy, w + dx + 1, h + dy + 1])
                    dynamic_probes.append([w + dx, h + dy, w + dx, h + dy])

            # Dense sweeps around origin
            for dx in range(-5, 6):
                for dy in range(-5, 6):
                    dynamic_probes.append([dx, dy, dx + 1, dy + 1])
                    dynamic_probes.append([dx, dy, dx, dy])

            # Random fuzzing: 1,000 random boxes per resolution
            rng = random.Random(42 + w + h)
            for _ in range(1000):
                rx1 = rng.uniform(-w, 2 * w)
                ry1 = rng.uniform(-h, 2 * h)
                rx2 = rng.uniform(-w, 2 * w)
                ry2 = rng.uniform(-h, 2 * h)
                dynamic_probes.append([rx1, ry1, rx2, ry2])

            for box in dynamic_probes:
                total_probes_run += 1
                (x1, y1, x2, y2), (nx1, ny1, nx2, ny2) = sanitize_bbox(box, w, h)

                # Check 1: <loc_1000> prohibition
                loc_str = f"<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"
                if "<loc_1000>" in loc_str:
                    violations.append(
                        f"[LOC_1000] On {w}x{h} with input {box}: {loc_str}"
                    )

                # Check 2: Normalized non-degeneracy and bounds
                if not (0 <= nx1 <= 998):
                    violations.append(
                        f"[NX1_BOUND] On {w}x{h} with input {box}: nx1={nx1} not in [0..998]"
                    )
                if not (0 <= ny1 <= 998):
                    violations.append(
                        f"[NY1_BOUND] On {w}x{h} with input {box}: ny1={ny1} not in [0..998]"
                    )
                if not (0 <= nx2 <= 999):
                    violations.append(
                        f"[NX2_BOUND] On {w}x{h} with input {box}: nx2={nx2} not in [0..999]"
                    )
                if not (0 <= ny2 <= 999):
                    violations.append(
                        f"[NY2_BOUND] On {w}x{h} with input {box}: ny2={ny2} not in [0..999]"
                    )
                if nx1 >= nx2:
                    violations.append(
                        f"[NX_DEGENERATE] On {w}x{h} with input {box}: nx1={nx1} >= nx2={nx2}"
                    )
                if ny1 >= ny2:
                    violations.append(
                        f"[NY_DEGENERATE] On {w}x{h} with input {box}: ny1={ny1} >= ny2={ny2}"
                    )

                # Check 3: Mirrored pixel coordinates maintain bounds and non-degeneracy
                if not (0 <= x1 < x2 <= w):
                    violations.append(
                        f"[PIXEL_X_BOUND] On {w}x{h} with input {box}: x1={x1}, x2={x2}, w={w}"
                    )
                if not (0 <= y1 < y2 <= h):
                    violations.append(
                        f"[PIXEL_Y_BOUND] On {w}x{h} with input {box}: y1={y1}, y2={y2}, h={h}"
                    )

        print(f"\n[PROBE SUCCESS] Tested {total_probes_run} adversarial coordinate probes across {len(resolutions)} resolutions.")
        if violations:
            msg = "\n".join(violations[:20])
            self.fail(f"Adversarial coordinate probe violations ({len(violations)} total):\n{msg}")

    def test_dataset_collector_florence_target_token_prohibition(self):
        """
        Feeds boundary edge cases directly into CPowerDatasetCollector.record_step
        both with and without normalized_bbox to verify that written JSONL target strings
        never contain <loc_1000> and strictly contain exactly 4 valid non-degenerate tokens.
        """
        tid = self.collector.start_trajectory("Probe Collector Tokens")
        resolutions = [(800, 600), (1024, 768), (1280, 720), (1366, 768), (1920, 1080), (2560, 1440), (3840, 2160)]

        step_counter = 0
        for w, h in resolutions:
            dummy_img = Image.new("RGB", (w, h), color=(50, 50, 50))
            test_boxes = [
                [w - 1, h - 1, w, h],
                [w, h, w, h],
                [w - 2, h - 2, w - 2, h - 2],
                [50000, 60000, 70000, 80000],
                [-5000, -5000, 0, 0],
                [0, 0, 0, 0],
            ]
            for box in test_boxes:
                # 1. With normalized_bbox derived from sanitize_bbox
                clean_b, norm_b = sanitize_bbox(box, w, h)
                self.collector.record_step(
                    trajectory_id=tid,
                    image=dummy_img,
                    action_type="click",
                    phrase="button",
                    bbox=clean_b,
                    click_coords=[clean_b[0], clean_b[1]],
                    window_title="Probe Window",
                    normalized_bbox=norm_b,
                )
                step_counter += 1

                # 2. Without normalized_bbox (letting collector normalize raw bbox)
                self.collector.record_step(
                    trajectory_id=tid,
                    image=dummy_img,
                    action_type="click",
                    phrase="button_raw",
                    bbox=box,
                    click_coords=None,
                    window_title="Probe Window Raw",
                    normalized_bbox=None,
                )
                step_counter += 1

        self.collector.complete_trajectory(tid, succeeded=True)

        # Inspect generated dataset_florence2.jsonl
        with open(self.collector.florence_jsonl_path, "r", encoding="utf-8") as f:
            lines = f.readlines()

        self.assertEqual(len(lines), step_counter)
        for idx, line in enumerate(lines, 1):
            data = json.loads(line)
            target = data["target"]
            self.assertNotIn("<loc_1000>", target, f"Line {idx} generated <loc_1000>! Target: {target}")

            tokens = [int(m) for m in re.findall(r"<loc_(\d+)>", target)]
            self.assertEqual(len(tokens), 4, f"Line {idx} does not have exactly 4 loc tokens: {target}")
            nx1, ny1, nx2, ny2 = tokens
            self.assertTrue(0 <= nx1 < nx2 <= 999, f"Line {idx} invalid nx: {tokens} in {target}")
            self.assertTrue(0 <= ny1 < ny2 <= 999, f"Line {idx} invalid ny: {tokens} in {target}")

        # Inspect generated dataset_agent_act.jsonl
        with open(self.collector.agent_act_jsonl_path, "r", encoding="utf-8") as f:
            act_lines = f.readlines()

        self.assertEqual(len(act_lines), step_counter)
        for idx, line in enumerate(act_lines, 1):
            data = json.loads(line)
            action = data["action"]
            click = action.get("click")
            self.assertIsNotNone(click, f"Line {idx} action click is None: {action}")
            self.assertIsInstance(click, list)
            self.assertEqual(len(click), 2)
            cx, cy = click
            fl_data = json.loads(lines[idx - 1])
            box = fl_data["bbox_px"]
            self.assertTrue(box[0] <= cx <= box[2], f"Click x {cx} out of bbox {box}")
            self.assertTrue(box[1] <= cy <= box[3], f"Click y {cy} out of bbox {box}")

    def test_physical_mouse_cursor_zero_movement(self):
        """
        Empirically verifies zero Win32 physical cursor displacement across 30 multi-domain tasks.
        """
        pt_start = wintypes.POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_start))

        harvester = MultiTaskUIHarvester(collector=self.collector, headless=True)
        tid = self.collector.start_trajectory("Mouse Cursor Zero Displacement Test")

        displacements = []
        for i in range(1, 31):
            domain = i % 3
            if domain == 0:
                harvester.generate_explorer_ribbon_task(tid, i)
            elif domain == 1:
                harvester.generate_youtube_task(tid, i, locale="ar" if i % 2 == 0 else "en")
            else:
                harvester.generate_arabic_rtl_task(tid, i)

            pt_now = wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_now))
            dx = pt_now.x - pt_start.x
            dy = pt_now.y - pt_start.y
            if dx != 0 or dy != 0:
                displacements.append((i, dx, dy))

        self.collector.complete_trajectory(tid, succeeded=True)
        harvester.close()

        self.assertEqual(
            len(displacements), 0,
            f"Physical mouse movement detected during harvesting: {displacements}"
        )

    def test_font_fallback_and_missing_font_robustness(self):
        """
        Verifies font cascade fallback when fonts are missing, damaged, or ImageFont.load_default() is forced.
        Ensures all components produce valid bboxes and images.
        """
        # Test A: ImageFont.load_default()
        with mock.patch("cpower_multitask_harvester.get_ui_font", return_value=ImageFont.load_default()):
            win11 = Win11ControlsSynthesizer()
            for name, func in [
                ("ribbon", lambda: win11.generate_ribbon(dark_mode=False)),
                ("ribbon_dark", lambda: win11.generate_ribbon(dark_mode=True)),
                ("breadcrumb", lambda: win11.generate_breadcrumb("D:")),
                ("grid", lambda: win11.generate_grid(folder_count=8)),
                ("save_dialog", lambda: win11.generate_save_open_dialog(is_open_dialog=False)),
                ("confirm_dialog", lambda: win11.generate_confirm_dialog(dialog_type="delete")),
            ]:
                img, comps = func()
                self.assertIsInstance(img, Image.Image)
                self.assertGreater(len(comps), 0)
                for comp in comps:
                    box = comp[2]
                    cb, nb = sanitize_bbox(box, img.width, img.height)
                    self.assertTrue(0 <= nb[0] < nb[2] <= 999)
                    self.assertTrue(0 <= nb[1] < nb[3] <= 999)

            yt = PillowYouTubeSynthesizer()
            img_yt, comps_yt = yt.generate(dark_theme=True, locale="ar")
            self.assertIsInstance(img_yt, Image.Image)
            self.assertGreater(len(comps_yt), 10)
            for comp in comps_yt:
                box = comp[2]
                cb, nb = sanitize_bbox(box, img_yt.width, img_yt.height)
                self.assertTrue(0 <= nb[0] < nb[2] <= 999)
                self.assertTrue(0 <= nb[1] < nb[3] <= 999)

            ar = ArabicRTLHarvester(collector=self.collector)
            tid = self.collector.start_trajectory("Arabic Font Fallback")
            ar._generate_engine_b_sample(tid, step_idx=1, template="dialog")
            self.collector.complete_trajectory(tid, succeeded=True)
            ar.close()
            with open(self.collector.florence_jsonl_path, "r", encoding="utf-8") as f:
                ar_lines = f.readlines()
            self.assertGreater(len(ar_lines), 0)


if __name__ == "__main__":
    unittest.main()
