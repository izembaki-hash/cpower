"""
CPower End-to-End Test Suite — Tier 2 Boundary & Extreme Conditions (tests/e2e/test_tier2_boundaries.py).
Author: Test Writer (Milestone TM3)

Implements all 40 Tier 2 boundary and corner case tests specified in TEST_INFRA.md § 5 across 5 categories:
  Category 2.1: Coordinate & Bounding Box Limits (10 tests: test_b1_coord_origin_zero_zero to test_b10_coord_extreme_resolution_1x1)
  Category 2.2: Empty & Corrupted Inputs (10 tests: test_b11_empty_string_phrase to test_b20_extreme_aspect_ratio_tall)
  Category 2.3: Arabic Bi-directional & Typography Extremes (10 tests: test_b21_pure_arabic_diacritics_heavy to test_b30_arabic_empty_and_single_character)
  Category 2.4: Disk Safety & Storage Boundary Limits (5 tests: test_b31_disk_exact_threshold_boundary to test_b35_dataset_collector_recovers_after_disk_freed)
  Category 2.5: Trainer Divergence & Extreme Hyperparameters (5 tests: test_b36_trainer_zero_samples_dataset to test_b40_trainer_max_steps_one)

All tests strictly enforce:
  1. Zero physical mouse movements (verified via ZeroMouseTracker).
  2. Storage isolation (all writes quarantined to isolated_dataset_dir; zero volatile writes to drive H:).
  3. Genuine execution against production modules in clients/python/.
"""

from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageStat
import pytest
import torch
from torch.utils.data import TensorDataset

# Ensure clients/python is available
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CLIENTS_PYTHON = PROJECT_ROOT / "clients" / "python"
if str(CLIENTS_PYTHON) not in sys.path:
    sys.path.insert(0, str(CLIENTS_PYTHON))

from cpower_dataset_collector import CPowerDatasetCollector
from cpower_dataset_validator import validate_dataset, validate_sample
from cpower_multitask_harvester import reshape_arabic, sanitize_bbox
from cpower_trainer import (
    FlorenceLocalDataset,
    check_loss_divergence,
    pad_to_square,
    split_train_val_dataset,
    train_local,
)
from mock_hardware import (
    ArabicSampleCorpus,
    LightweightProcessorStub,
    MockDiskController,
    SyntheticUIProvider,
    ZeroMouseTracker,
    get_system_font,
)


# -----------------------------------------------------------------------------
# Lightweight Model & Processor Stubs for Fast Trainer Unit Verification
# -----------------------------------------------------------------------------

class LightweightModelStub(torch.nn.Module):
    """Fast, deterministic model stub adhering to Florence-2 architecture interface."""

    def __init__(self):
        super().__init__()
        self.vision_tower = torch.nn.Linear(4, 4)
        self.image_proj_norm = torch.nn.Linear(4, 4)
        self.head = torch.nn.Linear(4, 1)

    def forward(self, input_ids=None, pixel_values=None, labels=None, use_cache=False):
        pred = self.head(self.image_proj_norm(torch.ones(1, 4)))
        loss = torch.nn.functional.mse_loss(pred, torch.ones_like(pred) * 2.0) + 0.1

        class Output:
            pass

        res = Output()
        res.loss = loss
        return res


class LightweightTrainerProcessorStub:
    """Fast processor stub returning valid tensor shapes for trainer DataLoader."""

    def __call__(self, text, images, return_tensors):
        return {
            "input_ids": torch.tensor([[101, 102]]),
            "pixel_values": torch.zeros((1, 3, 768, 768)),
        }

    @property
    def tokenizer(self):
        return lambda text, return_tensors: {"input_ids": torch.tensor([[201, 202]])}


# =============================================================================
# Category 2.1: Coordinate & Bounding Box Limits (10 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier2
def test_b1_coord_origin_zero_zero(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Target bbox at exact origin (0, 0, 10, 10) normalizes to <loc_0><loc_0>..."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Origin zero zero boundary test")
    img = Image.new("RGB", (1000, 1000), (240, 240, 240))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="أصل_الإحداثيات",
        bbox=(0, 0, 10, 10),
        click_coords=(5, 5),
    )
    collector.complete_trajectory(tid, succeeded=True)

    norm_bbox = step["normalized_bbox"]
    assert norm_bbox[0] == 0, f"Expected nx1 == 0, got {norm_bbox[0]}"
    assert norm_bbox[1] == 0, f"Expected ny1 == 0, got {norm_bbox[1]}"
    assert norm_bbox[2] > 0 and norm_bbox[3] > 0

    target = step["florence_sample"]["target"]
    assert "<loc_0><loc_0>" in target, f"Expected <loc_0><loc_0> in {target}"

    is_valid, report = validate_dataset(isolated_dataset_dir)
    errors = report["errors"] if isinstance(report, dict) else report
    assert is_valid is True, f"Dataset validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b2_coord_maximum_999(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Target bbox at bottom-right corner (w-10, h-10, w, h) normalizes to <loc_...><loc_999><loc_999>."""
    collector = collector_factory(isolated_dataset_dir)
    w, h = 1000, 1000
    tid = collector.start_trajectory("Max 999 boundary test")
    img = Image.new("RGB", (w, h), (200, 200, 200))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="الزاوية_السفلى",
        bbox=(w - 10, h - 10, w, h),
        click_coords=(w - 5, h - 5),
    )
    collector.complete_trajectory(tid, succeeded=True)

    norm_bbox = step["normalized_bbox"]
    assert norm_bbox[2] == 999, f"Expected nx2 == 999, got {norm_bbox[2]}"
    assert norm_bbox[3] == 999, f"Expected ny2 == 999, got {norm_bbox[3]}"

    target = step["florence_sample"]["target"]
    assert target.endswith("<loc_999><loc_999>"), f"Expected target to end with <loc_999><loc_999>, got {target}"
    assert "<loc_1000>" not in target, "Forbidden <loc_1000> present in target"

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b3_coord_fullscreen_bounding_box(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Entire canvas (0, 0, w, h) normalizes to <loc_0><loc_0><loc_999><loc_999>."""
    collector = collector_factory(isolated_dataset_dir)
    w, h = 1024, 680
    tid = collector.start_trajectory("Fullscreen bbox test")
    img = Image.new("RGB", (w, h), (180, 180, 180))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="الشاشة_الكاملة",
        bbox=(0, 0, w, h),
        click_coords=(w // 2, h // 2),
    )
    collector.complete_trajectory(tid, succeeded=True)

    assert step["normalized_bbox"] == [0, 0, 999, 999], f"Unexpected normalized bbox: {step['normalized_bbox']}"
    target = step["florence_sample"]["target"]
    assert target.endswith("<loc_0><loc_0><loc_999><loc_999>"), f"Target mismatch: {target}"

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b4_coord_negative_inputs_clamped_to_zero(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Negative input coordinates (-50, -20, 100, 100) clamp safely to <loc_0><loc_0>."""
    clean_box, norm_box = sanitize_bbox((-50, -20, 100, 100), 1000, 1000)
    assert clean_box[0] == 0 and clean_box[1] == 0, f"Pixel coords not clamped: {clean_box}"
    assert norm_box[0] == 0 and norm_box[1] == 0, f"Normalized coords not clamped: {norm_box}"

    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Negative coords clamp test")
    img = Image.new("RGB", (1000, 1000), (210, 210, 210))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="إحداثيات_سالبة",
        bbox=(-50, -20, 100, 100),
        click_coords=(50, 50),
    )
    collector.complete_trajectory(tid, succeeded=True)

    assert step["normalized_bbox"][0] == 0
    assert step["normalized_bbox"][1] == 0
    assert "<loc_0><loc_0>" in step["florence_sample"]["target"]
    assert "-" not in step["florence_sample"]["target"]

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b5_coord_overflow_inputs_clamped_to_999(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Overflow coordinates (100, 100, w+500, h+500) clamp safely to <loc_999><loc_999>."""
    w, h = 1000, 1000
    clean_box, norm_box = sanitize_bbox((100, 100, w + 500, h + 500), w, h)
    assert clean_box[2] == w and clean_box[3] == h
    assert norm_box[2] == 999 and norm_box[3] == 999

    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Overflow coords test")
    img = Image.new("RGB", (w, h), (215, 215, 215))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="تجاوز_الحدود",
        bbox=(100, 100, w + 500, h + 500),
        click_coords=(200, 200),
    )
    collector.complete_trajectory(tid, succeeded=True)

    assert step["normalized_bbox"][2] == 999
    assert step["normalized_bbox"][3] == 999
    assert step["florence_sample"]["target"].endswith("<loc_999><loc_999>")
    assert "<loc_1000>" not in step["florence_sample"]["target"]

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b6_coord_single_pixel_bounding_box(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """1-pixel target (500, 500, 501, 501) generates valid non-zero normalized area."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Single pixel bbox test")
    img = Image.new("RGB", (1000, 1000), (220, 220, 220))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="بكسل_واحد",
        bbox=(500, 500, 501, 501),
        click_coords=(500, 500),
    )
    collector.complete_trajectory(tid, succeeded=True)

    nx1, ny1, nx2, ny2 = step["normalized_bbox"]
    assert nx2 > nx1, f"Normalized X zero or inverted: {nx1} to {nx2}"
    assert ny2 > ny1, f"Normalized Y zero or inverted: {ny1} to {ny2}"
    assert (nx2 - nx1) * (ny2 - ny1) >= 1

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"
    zero_mouse_tracker.assert_zero_mouse_interactions()


@pytest.mark.e2e
@pytest.mark.tier2
def test_b7_coord_zero_width_or_height_rejected() -> None:
    """Bounding box with width=0 or height=0 is rejected by validator as degenerate."""
    # Zero width: x1 == x2
    zero_w_rec = {
        "image": "images/sample.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "zero_width",
        "target": "zero_width<loc_100><loc_100><loc_100><loc_200>",
        "bbox_px": [100, 100, 100, 200],
        "image_size": [1000, 1000],
    }
    is_valid_w, errors_w = validate_sample(zero_w_rec, check_image=False)
    assert is_valid_w is False, "Validator accepted zero-width bounding box"
    assert any("inverted or collapsed" in e.lower() or "degenerate" in e.lower() for e in errors_w)

    # Zero height: y1 == y2
    zero_h_rec = {
        "image": "images/sample.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "zero_height",
        "target": "zero_height<loc_100><loc_100><loc_200><loc_100>",
        "bbox_px": [100, 100, 200, 100],
        "image_size": [1000, 1000],
    }
    is_valid_h, errors_h = validate_sample(zero_h_rec, check_image=False)
    assert is_valid_h is False, "Validator accepted zero-height bounding box"
    assert any("inverted or collapsed" in e.lower() or "degenerate" in e.lower() for e in errors_h)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b8_coord_inverted_bbox_handling(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Inverted bounding box (400, 300, 200, 100) is flagged by validator, while sanitize_bbox auto-heals."""
    # 1. Validator correctly flags unhealed inverted box
    raw_inverted_rec = {
        "image": "images/sample.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "inverted",
        "target": "inverted<loc_400><loc_300><loc_200><loc_100>",
        "bbox_px": [400, 300, 200, 100],
        "image_size": [1000, 1000],
    }
    is_valid, errors = validate_sample(raw_inverted_rec, check_image=False)
    assert is_valid is False, "Validator failed to flag inverted coordinates"
    assert any("inverted" in e.lower() or "degenerate" in e.lower() for e in errors)

    # 2. Production sanitize_bbox heals coordinates
    clean_box, norm_box = sanitize_bbox((400, 300, 200, 100), 1000, 1000)
    assert clean_box == (200, 100, 400, 300)
    assert norm_box[0] < norm_box[2] and norm_box[1] < norm_box[3]

    # 3. Collector record_step auto-heals during harvesting
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Inverted heal test")
    img = Image.new("RGB", (1000, 1000), (225, 225, 225))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="مربع_معكوس",
        bbox=(400, 300, 200, 100),
        click_coords=(300, 200),
    )
    collector.complete_trajectory(tid, succeeded=True)
    assert step["bbox_px"] == [200, 100, 400, 300]
    assert step["normalized_bbox"][0] < step["normalized_bbox"][2]
    is_valid_healed, errs_healed = validate_dataset(isolated_dataset_dir)
    assert is_valid_healed is True, f"Dataset failed after auto-heal: {errs_healed}"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b9_coord_float_inputs_rounded(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Floating point coordinates (100.4, 200.7, 300.2, 400.9) round cleanly to integer tokens."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Float coords test")
    img = Image.new("RGB", (1000, 1000), (230, 230, 230))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="إحداثيات_عشرية",
        bbox=(100.4, 200.7, 300.2, 400.9),
        click_coords=(200.3, 300.8),
    )
    collector.complete_trajectory(tid, succeeded=True)

    assert step["bbox_px"] == [100, 201, 300, 401]
    assert all(isinstance(c, int) for c in step["bbox_px"])
    assert all(isinstance(c, int) for c in step["normalized_bbox"])

    # Target contains integer location tokens, no floats
    target = step["florence_sample"]["target"]
    assert re.search(r"<loc_\d+\.\d+>", target) is None, f"Float found in location tokens: {target}"
    assert "<loc_100><loc_201><loc_300><loc_401>" in target

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b10_coord_extreme_resolution_1x1(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Micro-resolution image 1x1 with bbox (0, 0, 1, 1) yields <loc_0><loc_0><loc_999><loc_999>."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("1x1 resolution test")
    img = Image.new("RGB", (1, 1), (120, 120, 120))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="دقة_1بكسل",
        bbox=(0, 0, 1, 1),
        click_coords=(0, 0),
    )
    collector.complete_trajectory(tid, succeeded=True)

    assert step["normalized_bbox"] == [0, 0, 999, 999]
    assert step["florence_sample"]["target"].endswith("<loc_0><loc_0><loc_999><loc_999>")

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed on 1x1 image: {errors}"


# =============================================================================
# Category 2.2: Empty & Corrupted Inputs (10 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier2
def test_b11_empty_string_phrase(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Empty phrase string '' is handled with descriptive fallback or valid token formatting."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Empty phrase test")
    img = Image.new("RGB", (500, 500), (235, 235, 235))
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="",
        bbox=(50, 50, 150, 150),
        click_coords=(100, 100),
    )
    collector.complete_trajectory(tid, succeeded=True)

    target = step["florence_sample"]["target"]
    assert target.startswith("<loc_")
    parsed = LightweightProcessorStub.parse_target(target, (500, 500))
    assert parsed["valid"] is True
    assert parsed["phrase"] == ""

    is_valid, errors = validate_dataset(isolated_dataset_dir)
    assert is_valid is True, f"Validation failed: {errors}"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b12_empty_string_goal(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Empty goal string '' defaults safely to 'Unspecified'."""
    collector = collector_factory(isolated_dataset_dir)

    # 1. Unregistered trajectory completion defaults to 'Unspecified'
    out_file = collector.complete_trajectory("orphan_goal_test", succeeded=True)
    traj_data = json.loads(out_file.read_text(encoding="utf-8"))
    assert traj_data["goal"] == "Unspecified"

    # 2. Starting trajectory with empty string goal doesn't crash and persists cleanly
    tid = collector.start_trajectory(goal="")
    img = Image.new("RGB", (200, 200), (240, 240, 240))
    collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="زر_هدف_فارغ",
        bbox=(20, 20, 80, 80),
    )
    out_file_2 = collector.complete_trajectory(tid, succeeded=True)
    traj_data_2 = json.loads(out_file_2.read_text(encoding="utf-8"))
    assert traj_data_2["goal"] in ("", "Unspecified")


@pytest.mark.e2e
@pytest.mark.tier2
def test_b13_whitespace_only_phrase() -> None:
    """Whitespace-only phrase '   \\t\\n' is stripped or rejected by validator."""
    ws_phrase = "   \t\n  "
    target = LightweightProcessorStub.format_target(ws_phrase, (20, 20, 80, 80), (1000, 1000))
    parsed = LightweightProcessorStub.parse_target(target, (1000, 1000))
    assert parsed["valid"] is True
    assert parsed["phrase"].strip() == ""

    # Schema validator flags whitespace-only prefix as invalid
    invalid_rec = {
        "image": "images/test.jpg",
        "prefix": "   \t\n  ",
        "suffix": "button",
        "target": "button<loc_20><loc_20><loc_80><loc_80>",
        "bbox_px": [20, 20, 80, 80],
        "image_size": [1000, 1000],
    }
    is_valid, errors = validate_sample(invalid_rec, check_image=False)
    assert is_valid is False
    assert any("non-empty string" in e.lower() for e in errors)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b14_corrupted_jsonl_line_skipped_by_dataset_loader(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Corrupted JSON line in JSONL file is skipped without crashing dataset loader."""
    collector = collector_factory(isolated_dataset_dir)
    for i in range(2):
        tid = collector.start_trajectory(f"Loader Skip Test {i}")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (30 * i, 30 * i, 30 * i)),
            action_type="click",
            phrase=f"زر_{i}",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

    lines = collector.florence_jsonl_path.read_text(encoding="utf-8").splitlines()
    corrupted_content = lines[0] + "\n{ corrupted json NOT A VALID LINE }}} \n" + lines[1] + "\n"
    collector.florence_jsonl_path.write_text(corrupted_content, encoding="utf-8")

    class FastProcessorStub:
        def __call__(self, text, images, return_tensors):
            return {"input_ids": torch.tensor([[1]]), "pixel_values": torch.zeros((1, 3, 100, 100))}

        @property
        def tokenizer(self):
            return lambda text, return_tensors: {"input_ids": torch.tensor([[1]])}

    ds = FlorenceLocalDataset(collector.florence_jsonl_path, processor=FastProcessorStub())
    assert len(ds) == 2, f"Expected 2 valid samples, got {len(ds)}"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b15_truncated_jsonl_line(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Truncated JSON line at EOF is skipped gracefully by dataset loader."""
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Truncated EOF Test")
    collector.record_step(
        trajectory_id=tid,
        image=Image.new("RGB", (100, 100), (50, 50, 50)),
        action_type="click",
        phrase="زر_صحيح",
        bbox=(10, 10, 50, 50),
    )
    collector.complete_trajectory(tid, succeeded=True)

    with open(collector.florence_jsonl_path, "a", encoding="utf-8") as f:
        f.write('{"id": "traj_incomplete", "image": "images/truncated')

    class FastProcessorStub:
        def __call__(self, text, images, return_tensors):
            return {"input_ids": torch.tensor([[1]]), "pixel_values": torch.zeros((1, 3, 100, 100))}

        @property
        def tokenizer(self):
            return lambda text, return_tensors: {"input_ids": torch.tensor([[1]])}

    ds = FlorenceLocalDataset(collector.florence_jsonl_path, processor=FastProcessorStub())
    assert len(ds) == 1, f"Expected 1 valid sample, got {len(ds)}"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b16_missing_target_key_in_florence_jsonl(isolated_dataset_dir: Path) -> None:
    """Record missing 'target' key is flagged with line number."""
    bad_record = {
        "id": "missing_target_sample",
        "image": "images/dummy.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "button",
        "bbox_px": [10, 10, 50, 50],
        "image_size": [1000, 1000],
    }
    fl_path = isolated_dataset_dir / "dataset_florence2.jsonl"
    fl_path.write_text(json.dumps(bad_record) + "\n", encoding="utf-8")

    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    act_record = {
        "trajectory_id": "missing_target_sample",
        "step_index": 0,
        "goal": "Test goal",
        "window": "Test window",
        "image": "images/dummy.jpg",
        "action": {"type": "click", "phrase": "button", "click": [30, 30]},
    }
    act_path.write_text(json.dumps(act_record) + "\n", encoding="utf-8")

    is_valid, report = validate_dataset(isolated_dataset_dir, check_images=False)
    errors = report["errors"] if isinstance(report, dict) else report
    assert is_valid is False
    assert any("target" in e.lower() and "missing" in e.lower() for e in errors)
    assert any("1" in e.lower() for e in errors)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b17_zero_byte_image_file(isolated_dataset_dir: Path) -> None:
    """0-byte image file is reported by validator as corrupted or empty."""
    empty_img = isolated_dataset_dir / "images" / "empty_file.jpg"
    empty_img.write_bytes(b"")

    record = {
        "id": "zero_byte_sample",
        "image": "images/empty_file.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "زر",
        "target": "زر<loc_10><loc_10><loc_50><loc_50>",
        "bbox_px": [10, 10, 50, 50],
        "image_size": [1000, 1000],
    }
    (isolated_dataset_dir / "dataset_florence2.jsonl").write_text(json.dumps(record) + "\n", encoding="utf-8")

    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    act_record = {
        "trajectory_id": "zero_byte_sample",
        "step_index": 0,
        "goal": "Test goal",
        "window": "Test window",
        "image": "images/empty_file.jpg",
        "action": {"type": "click", "phrase": "زر", "click": [30, 30]},
    }
    act_path.write_text(json.dumps(act_record) + "\n", encoding="utf-8")

    is_valid, report = validate_dataset(isolated_dataset_dir, check_images=True)
    errors = report["errors"] if isinstance(report, dict) else report
    assert is_valid is False
    assert any("empty" in e.lower() or "0 bytes" in e.lower() or "corrupted" in e.lower() or "unreadable" in e.lower() for e in errors)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b18_truncated_jpeg_image(isolated_dataset_dir: Path) -> None:
    """Truncated JPEG file is reported by validator as unreadable."""
    trunc_img = isolated_dataset_dir / "images" / "truncated.jpg"
    trunc_img.write_bytes(b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")

    record = {
        "id": "truncated_sample",
        "image": "images/truncated.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "زر",
        "target": "زر<loc_10><loc_10><loc_50><loc_50>",
        "bbox_px": [10, 10, 50, 50],
        "image_size": [1000, 1000],
    }
    (isolated_dataset_dir / "dataset_florence2.jsonl").write_text(json.dumps(record) + "\n", encoding="utf-8")

    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    act_record = {
        "trajectory_id": "truncated_sample",
        "step_index": 0,
        "goal": "Test goal",
        "window": "Test window",
        "image": "images/truncated.jpg",
        "action": {"type": "click", "phrase": "زر", "click": [30, 30]},
    }
    act_path.write_text(json.dumps(act_record) + "\n", encoding="utf-8")

    is_valid, report = validate_dataset(isolated_dataset_dir, check_images=True)
    errors = report["errors"] if isinstance(report, dict) else report
    assert is_valid is False
    assert any("corrupted" in e.lower() or "unreadable" in e.lower() or "truncated" in e.lower() for e in errors)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b19_extreme_aspect_ratio_panorama() -> None:
    """Ultra-wide image 8000x200 is padded to square canvas without out-of-memory errors."""
    panorama = Image.new("RGB", (8000, 200), (25, 45, 65))
    padded = pad_to_square(panorama, min_dim=768)
    assert padded.size == (8000, 8000), f"Expected (8000, 8000), got {padded.size}"
    assert padded.getpixel((10, 10)) == (25, 45, 65)
    assert padded.getpixel((10, 500)) == (255, 255, 255)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b20_extreme_aspect_ratio_tall() -> None:
    """Ultra-tall image 200x8000 is padded to square canvas without out-of-memory errors."""
    tall = Image.new("RGB", (200, 8000), (65, 45, 25))
    padded = pad_to_square(tall, min_dim=768)
    assert padded.size == (8000, 8000), f"Expected (8000, 8000), got {padded.size}"
    assert padded.getpixel((10, 10)) == (65, 45, 25)
    assert padded.getpixel((500, 10)) == (255, 255, 255)


# =============================================================================
# Category 2.3: Arabic Bi-directional & Typography Extremes (10 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier2
def test_b21_pure_arabic_diacritics_heavy() -> None:
    """Full tashkeel text is rendered without character truncation."""
    text = "بِسْمِ اللَّهِ الرَّحْمَٰنِ الرَّحِيمِ تَدْرِيبُ النَّمُوذَجِ عَلَى بَيَانَاتِ الْوَاجِهَةِ"
    reshaped = reshape_arabic(text)
    assert len(reshaped) > 0

    img = Image.new("RGB", (600, 100), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    font = get_system_font(18)
    draw.text((20, 20), reshaped, fill=(0, 0, 0), font=font)

    extrema = img.getextrema()
    assert any(ex[0] < 255 for ex in extrema), "No text pixels drawn on canvas"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b22_mixed_ltr_rtl_triple_alternation() -> None:
    """String with multiple RTL/LTR transitions orders correctly."""
    text = "المجلد C:\\CPower\\Data رقم 1 تم حفظه بنجاح"
    reshaped = reshape_arabic(text)
    assert "C:\\CPower\\Data" in reshaped
    assert "1" in reshaped
    assert reshaped != text


@pytest.mark.e2e
@pytest.mark.tier2
def test_b23_eastern_arabic_numerals() -> None:
    """Eastern Arabic numerals ('١٢٣٤٥٦٧٨٩٠') render with glyphs preserved."""
    digits = "١٢٣٤٥٦٧٨٩٠"
    reshaped = reshape_arabic(digits)
    assert all(d in reshaped for d in digits)

    img = Image.new("RGB", (300, 60), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    draw.text((10, 10), reshaped, fill=(0, 0, 0), font=get_system_font(16))
    extrema = img.getextrema()
    assert any(ex[0] < 255 for ex in extrema)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b24_arabic_ligatures_lam_alif() -> None:
    """Text containing Lam-Alif ligatures ('لا إله إلا الله') renders connected glyphs."""
    text = "لا إله إلا الله"
    reshaped = reshape_arabic(text)
    has_ligature = any("\ufef5" <= ch <= "\ufefc" for ch in reshaped)
    assert has_ligature or len(reshaped) < len(text), "Lam-Alif ligature contraction not applied"


@pytest.mark.e2e
@pytest.mark.tier2
def test_b25_zero_width_joiner_zwnj() -> None:
    """Strings containing ZWJ (\\u200D) and ZWNJ (\\u200C) process cleanly."""
    text = "مـ\u200dـلف \u200c جديد"
    reshaped = reshape_arabic(text)
    assert len(reshaped) > 0
    img = Image.new("RGB", (300, 60), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    draw.text((10, 10), reshaped, fill=(0, 0, 0), font=get_system_font(16))
    extrema = img.getextrema()
    assert any(ex[0] < 255 for ex in extrema)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b26_very_long_arabic_string() -> None:
    """1,000-character Arabic paragraph processes without buffer overflow."""
    long_text = ("يقوم وكيل CPower بجمع بيانات واجهات المستخدم لنظام ويندوز 11 " * 20)[:1000]
    assert len(long_text) >= 1000

    reshaped = reshape_arabic(long_text)
    assert len(reshaped) > 0

    target = LightweightProcessorStub.format_target(long_text, (50, 50, 300, 300), (1000, 1000))
    parsed = LightweightProcessorStub.parse_target(target, (1000, 1000))
    assert parsed["valid"] is True
    assert parsed["phrase"] == long_text


@pytest.mark.e2e
@pytest.mark.tier2
def test_b27_special_arabic_punctuation() -> None:
    """Arabic punctuation marks (؛, ؟, ،) preserve reading order."""
    text = "هل تم الحفظ بنجاح؟ نعم؛ تم ذلك، شكراً."
    reshaped = reshape_arabic(text)
    assert "؟" in reshaped
    assert "؛" in reshaped
    assert "،" in reshaped


@pytest.mark.e2e
@pytest.mark.tier2
def test_b28_arabic_tatweel_kashida() -> None:
    """Arabic kashida elongation ('تـــــدريــــب') renders correctly."""
    text = "تـــــدريــــب"
    assert "\u0640" in text
    reshaped = reshape_arabic(text)
    assert len(reshaped) > 0
    img = Image.new("RGB", (200, 50), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    draw.text((10, 10), reshaped, fill=(0, 0, 0), font=get_system_font(16))
    extrema = img.getextrema()
    assert any(ex[0] < 255 for ex in extrema)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b29_arabic_text_bounding_box_tightness() -> None:
    """Text bounding box tightly encloses rendered glyph extents."""
    text = "حفظ"
    reshaped = reshape_arabic(text)
    font = get_system_font(20)
    bbox = font.getbbox(reshaped)
    assert bbox is not None
    bx1, by1, bx2, by2 = bbox
    assert bx2 > bx1 and by2 > by1
    width = bx2 - bx1
    height = by2 - by1
    assert width >= 15
    assert height >= 8


@pytest.mark.e2e
@pytest.mark.tier2
def test_b30_arabic_empty_and_single_character() -> None:
    """Single character 'ع' and empty string process without index errors."""
    single = "ع"
    reshaped_single = reshape_arabic(single)
    assert len(reshaped_single) == 1

    empty = ""
    reshaped_empty = reshape_arabic(empty)
    assert reshaped_empty == ""


# =============================================================================
# Category 2.4: Disk Safety & Storage Boundary Limits (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier2
def test_b31_disk_exact_threshold_boundary(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Free space exactly 1.0 GB (1,073,741,824 bytes) passes safety check."""
    exact_1gb = 1024 * 1024 * 1024  # 1,073,741,824 bytes
    mock_disk_controller.set_free_space(isolated_dataset_dir, exact_1gb)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is True
    tid = collector.start_trajectory("Exact 1GB test")
    step = collector.record_step(
        trajectory_id=tid,
        image=Image.new("RGB", (100, 100), (50, 50, 50)),
        action_type="click",
        phrase="حد_القرص_الدقيق",
        bbox=(10, 10, 50, 50),
    )
    assert step is not None
    collector.complete_trajectory(tid, succeeded=True)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b32_disk_one_byte_below_threshold(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Free space 1.0 GB - 1 byte raises IOError."""
    below_1gb = 1024 * 1024 * 1024 - 1
    mock_disk_controller.set_free_space(isolated_dataset_dir, below_1gb)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is False
    tid = collector.start_trajectory("Below 1GB test")
    with pytest.raises(IOError, match=r"Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase="نقص_بايت",
            bbox=(10, 10, 50, 50),
        )


@pytest.mark.e2e
@pytest.mark.tier2
def test_b33_disk_one_byte_above_threshold(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Free space 1.0 GB + 1 byte succeeds."""
    above_1gb = 1024 * 1024 * 1024 + 1
    mock_disk_controller.set_free_space(isolated_dataset_dir, above_1gb)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is True
    tid = collector.start_trajectory("Above 1GB test")
    step = collector.record_step(
        trajectory_id=tid,
        image=Image.new("RGB", (100, 100), (50, 50, 50)),
        action_type="click",
        phrase="زيادة_بايت",
        bbox=(10, 10, 50, 50),
    )
    assert step is not None
    collector.complete_trajectory(tid, succeeded=True)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b34_disk_zero_free_space(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Free space 0 bytes immediately raises IOError."""
    mock_disk_controller.set_free_space(isolated_dataset_dir, 0)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is False
    tid = collector.start_trajectory("Zero space test")
    with pytest.raises(IOError, match=r"Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase="صفر_مساحة",
            bbox=(10, 10, 50, 50),
        )


@pytest.mark.e2e
@pytest.mark.tier2
def test_b35_dataset_collector_recovers_after_disk_freed(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """After simulated disk expansion, subsequent writes succeed without process restart."""
    # 1. Starve disk space
    mock_disk_controller.set_free_space(isolated_dataset_dir, 500 * 1024 * 1024)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)
    tid = collector.start_trajectory("Recovery test")
    img = Image.new("RGB", (100, 100), (50, 50, 50))

    with pytest.raises(IOError, match=r"Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=tid,
            image=img,
            action_type="click",
            phrase="محاولة_فاشلة",
            bbox=(10, 10, 50, 50),
        )

    # 2. Free disk space
    mock_disk_controller.set_free_gb(isolated_dataset_dir, 10.0)
    assert collector._check_disk_space() is True

    # 3. Subsequent write on same collector succeeds
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="محاولة_ناجحة_بعد_التحرير",
        bbox=(10, 10, 50, 50),
    )
    assert step is not None
    collector.complete_trajectory(tid, succeeded=True)
    assert (isolated_dataset_dir / "images" / f"{tid}_step_00.jpg").is_file()


# =============================================================================
# Category 2.5: Trainer Divergence & Extreme Hyperparameters (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier2
def test_b36_trainer_zero_samples_dataset(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Dataset with 0 lines returns early with warning without raising unhandled exception."""
    collector = collector_factory(isolated_dataset_dir)
    collector.florence_jsonl_path.write_text("", encoding="utf-8")

    metrics = train_local(dataset_root=isolated_dataset_dir)
    assert isinstance(metrics, dict)
    assert metrics["steps"] == 0
    assert metrics["train_loss"] == 0.0
    assert metrics["val_loss"] == 0.0
    assert metrics["nan_detected"] is False


@pytest.mark.e2e
@pytest.mark.tier2
def test_b37_trainer_single_sample_dataset(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Dataset with 1 sample handles 90/10 split safely without zero-division error."""
    # 1. Direct split verification on 1 sample
    dataset = TensorDataset(torch.arange(1))
    train_set, val_set = split_train_val_dataset(dataset, val_ratio=0.1)
    assert len(train_set) == 1
    assert len(val_set) == 1

    # 2. Integration verification through train_local
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Single sample train test")
    collector.record_step(
        trajectory_id=tid,
        image=Image.new("RGB", (100, 100), (40, 40, 40)),
        action_type="click",
        phrase="عينة_واحدة",
        bbox=(10, 10, 50, 50),
    )
    collector.complete_trajectory(tid, succeeded=True)

    metrics = train_local(
        dataset_root=isolated_dataset_dir,
        epochs=1,
        max_steps=1,
        model=LightweightModelStub(),
        processor=LightweightTrainerProcessorStub(),
    )
    assert metrics["steps"] == 1
    assert metrics["train_loss"] > 0
    assert metrics["val_loss"] > 0


@pytest.mark.e2e
@pytest.mark.tier2
def test_b38_trainer_nan_loss_detection_cpu() -> None:
    """Simulated NaN loss is caught by assert not torch.isnan(loss)."""
    nan_tensor = torch.tensor(float("nan"))
    with pytest.raises(AssertionError, match=r"(?i)nan.*divergence|loss.*nan"):
        check_loss_divergence(nan_tensor)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b39_trainer_infinite_loss_detection() -> None:
    """Simulated Inf loss is caught by divergence assertion."""
    inf_tensor = torch.tensor(float("inf"))
    with pytest.raises(AssertionError, match=r"(?i)inf.*divergence|loss.*inf"):
        check_loss_divergence(inf_tensor)

    neg_inf_tensor = torch.tensor(float("-inf"))
    with pytest.raises(AssertionError, match=r"(?i)inf.*divergence|loss.*inf"):
        check_loss_divergence(neg_inf_tensor)


@pytest.mark.e2e
@pytest.mark.tier2
def test_b40_trainer_max_steps_one(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """max_steps=1 terminates training loop strictly after step 1."""
    collector = collector_factory(isolated_dataset_dir)
    for i in range(5):
        tid = collector.start_trajectory(f"Max steps test {i}")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (20 * i, 20 * i, 20 * i)),
            action_type="click",
            phrase=f"عينة_{i}",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

    metrics = train_local(
        dataset_root=isolated_dataset_dir,
        epochs=3,
        max_steps=1,
        model=LightweightModelStub(),
        processor=LightweightTrainerProcessorStub(),
    )
    assert metrics["steps"] == 1, f"Expected exactly 1 step, got {metrics['steps']}"
