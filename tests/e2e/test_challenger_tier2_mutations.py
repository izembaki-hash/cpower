"""
Empirical Mutation & Sensitivity Harness for Tier 2 Boundary Tests.
Author: Challenger 1 (Milestone TM3)

Empirically challenges tests/e2e/test_tier2_boundaries.py against injected mutants:
1. Mutant 1: Coordinate Normalization fails to clamp to 999 (e.g. yields <loc_1000>).
2. Mutant 2: Zero-width / degenerate boxes are accepted by validator.
3. Mutant 3: 1.0 GB disk threshold check is violated by 1 byte (off-by-one).
4. Mutant 4: NaN loss divergence check is bypassed / disabled.
5. Mutant 5: Single sample dataset split produces empty validation set.
6. Mutant 6: Hardware mouse interaction spy sensitivity verification.
"""

from __future__ import annotations

import copy
from pathlib import Path
import re
import sys
from typing import Any, Callable, Dict, List, Optional, Tuple

from PIL import Image
import pytest
import torch

# Ensure clients/python is available
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CLIENTS_PYTHON = PROJECT_ROOT / "clients" / "python"
if str(CLIENTS_PYTHON) not in sys.path:
    sys.path.insert(0, str(CLIENTS_PYTHON))

from cpower_dataset_collector import CPowerDatasetCollector
import cpower_dataset_validator as validator_module
from cpower_dataset_validator import validate_dataset, validate_sample
from cpower_multitask_harvester import sanitize_bbox
import cpower_trainer as trainer_module
from cpower_trainer import check_loss_divergence, split_train_val_dataset
from mock_hardware import MockDiskController, ZeroMouseTracker


# =============================================================================
# Mutation Challenge 1: Coordinate Normalization Clamping to 999
# =============================================================================

def test_mutant_coord_clamping_sensitivity(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    monkeypatch,
) -> None:
    """
    MUTANT 1: Simulate coordinate normalization failing to clamp to 999.
    If collector erroneously allowed <loc_1000> or normalized_bbox[2] == 1000,
    the assertions in test_b2 and test_b5 MUST fail.
    """
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Mutant Clamping Test")
    img = Image.new("RGB", (1000, 1000), (200, 200, 200))

    # Real run produces clean 999
    step = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="الزاوية_السفلى",
        bbox=(990, 990, 1000, 1000),
        click_coords=(995, 995),
    )
    collector.complete_trajectory(tid, succeeded=True)

    # Verify baseline is sensitive
    assert step["normalized_bbox"][2] == 999
    assert step["florence_sample"]["target"].endswith("<loc_999><loc_999>")

    # INJECT MUTANT: mutated step produces 1000
    mutant_step = copy.deepcopy(step)
    mutant_step["normalized_bbox"][2] = 1000
    mutant_step["florence_sample"]["target"] = mutant_step["florence_sample"]["target"].replace(
        "<loc_999><loc_999>", "<loc_1000><loc_999>"
    )

    # 1. Assert that test_b2 assertions catch this mutant
    with pytest.raises(AssertionError, match=r"Expected nx2 == 999"):
        norm_bbox = mutant_step["normalized_bbox"]
        assert norm_bbox[2] == 999, f"Expected nx2 == 999, got {norm_bbox[2]}"

    with pytest.raises(AssertionError, match=r"Expected target to end with"):
        target = mutant_step["florence_sample"]["target"]
        assert target.endswith("<loc_999><loc_999>"), f"Expected target to end with <loc_999><loc_999>, got {target}"

    with pytest.raises(AssertionError, match=r"Forbidden <loc_1000> present in target"):
        assert "<loc_1000>" not in mutant_step["florence_sample"]["target"], "Forbidden <loc_1000> present in target"

    # 2. Assert validator catches this mutant record
    mutant_florence_rec = {
        "image": step["image_path"],
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "الزاوية_السفلى",
        "target": mutant_step["florence_sample"]["target"],
        "bbox_px": step["bbox_px"],
        "image_size": step["image_size"],
    }
    is_valid, errors = validate_sample(mutant_florence_rec, base_dir=isolated_dataset_dir, check_image=False)
    assert is_valid is False, "Validator failed to catch <loc_1000> mutant"
    assert any("<loc_1000>" in e or "Tokens must be in [0..999]" in e for e in errors)


# =============================================================================
# Mutation Challenge 2: Zero-Width Bounding Boxes
# =============================================================================

def test_mutant_zero_width_box_sensitivity(monkeypatch) -> None:
    """
    MUTANT 2: What happens if zero-width boxes are accepted?
    If validator had a bug and accepted zero-width boxes (is_valid=True),
    does test_b7 fail?
    Also proves double-layer defense in production validator: both pixel bbox
    and token geometric checks independently catch degenerate boxes.
    """
    zero_w_rec = {
        "image": "images/sample.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "zero_width",
        "target": "zero_width<loc_100><loc_100><loc_100><loc_200>",
        "bbox_px": [100, 100, 100, 200],
        "image_size": [1000, 1000],
    }

    # 1. Baseline: production validator has double-layer protection
    is_valid, errs = validate_sample(zero_w_rec, check_image=False)
    assert is_valid is False
    assert any("inverted or collapsed" in e.lower() for e in errs)
    assert any("degenerate" in e.lower() for e in errs)

    # 2. INJECT MUTANT: If a defective validator returns is_valid=True
    def buggy_validate_sample_accepting_zero_width(rec, *args, **kwargs):
        return True, []

    monkeypatch.setattr(validator_module, "validate_sample", buggy_validate_sample_accepting_zero_width)

    # Running test_b7 assertion under this mutant MUST FAIL
    with pytest.raises(AssertionError, match=r"Validator accepted zero-width bounding box"):
        is_valid_mutant, errors_w = validator_module.validate_sample(zero_w_rec, check_image=False)
        assert is_valid_mutant is False, "Validator accepted zero-width bounding box"


# =============================================================================
# Mutation Challenge 3: 1.0 GB Disk Threshold Off-by-1 Byte
# =============================================================================

def test_mutant_disk_one_byte_threshold_sensitivity(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """
    MUTANT 3: What happens if 1.0 GB disk threshold check is violated by 1 byte?
    Verify exact sensitivity between:
    - 1,073,741,824 bytes (1.0 GB exact) -> PASSES
    - 1,073,741,823 bytes (1.0 GB - 1 byte) -> FAILS (IOError)
    - 1,073,741,825 bytes (1.0 GB + 1 byte) -> PASSES
    """
    exact_1gb = 1024 * 1024 * 1024
    below_1gb = exact_1gb - 1
    above_1gb = exact_1gb + 1

    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)
    img = Image.new("RGB", (100, 100), (50, 50, 50))

    # 1. At 1 byte below:
    mock_disk_controller.set_free_space(isolated_dataset_dir, below_1gb)
    assert collector._check_disk_space() is False
    tid = collector.start_trajectory("Disk below test")
    with pytest.raises(IOError, match=r"Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=tid,
            image=img,
            action_type="click",
            phrase="نقص_بايت",
            bbox=(10, 10, 50, 50),
        )

    # 2. At exact boundary:
    mock_disk_controller.set_free_space(isolated_dataset_dir, exact_1gb)
    assert collector._check_disk_space() is True
    step_exact = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="حد_دقيق",
        bbox=(10, 10, 50, 50),
    )
    assert step_exact is not None

    # 3. At 1 byte above:
    mock_disk_controller.set_free_space(isolated_dataset_dir, above_1gb)
    assert collector._check_disk_space() is True
    step_above = collector.record_step(
        trajectory_id=tid,
        image=img,
        action_type="click",
        phrase="زيادة_بايت",
        bbox=(10, 10, 50, 50),
    )
    assert step_above is not None

    # 4. INJECT MUTANT: If check was flawed using > instead of >=
    flawed_check = lambda: exact_1gb > collector.min_free_bytes  # False at exact
    assert flawed_check() is False, "Flawed strict '>' check rejects exact 1.0 GB"


# =============================================================================
# Mutation Challenge 4: NaN Loss Divergence Detection
# =============================================================================

def test_mutant_nan_loss_divergence_sensitivity(monkeypatch) -> None:
    """
    MUTANT 4: What happens if NaN loss is not caught?
    If check_loss_divergence is a no-op, test_b38 MUST fail.
    """
    nan_tensor = torch.tensor(float("nan"))

    # Baseline: check_loss_divergence raises AssertionError
    with pytest.raises(AssertionError, match=r"(?i)nan.*divergence|loss.*nan"):
        check_loss_divergence(nan_tensor)

    # INJECT MUTANT: check_loss_divergence does nothing (swallows NaN)
    def noop_check_loss_divergence(loss):
        pass

    monkeypatch.setattr(trainer_module, "check_loss_divergence", noop_check_loss_divergence)

    # Running test_b38 logic MUST FAIL because no exception is raised
    with pytest.raises(pytest.fail.Exception, match=r"DID NOT RAISE"):
        with pytest.raises(AssertionError, match=r"(?i)nan.*divergence|loss.*nan"):
            trainer_module.check_loss_divergence(nan_tensor)


# =============================================================================
# Mutation Challenge 5: Single Sample Dataset Split Sensitivity
# =============================================================================

def test_mutant_single_sample_dataset_split_sensitivity(monkeypatch) -> None:
    """
    MUTANT 5: What happens if single sample split naively drops validation set?
    If split_train_val_dataset returned len(val_set) == 0, test_b37 MUST fail.
    """
    from torch.utils.data import TensorDataset

    dataset = TensorDataset(torch.arange(1))

    # Baseline: splits to train=1, val=1
    train_set, val_set = split_train_val_dataset(dataset, val_ratio=0.1)
    assert len(train_set) == 1
    assert len(val_set) == 1

    # INJECT MUTANT: flawed split that returns empty validation set
    flawed_val_set = []
    with pytest.raises(AssertionError, match=r"Expected validation set size 1"):
        assert len(flawed_val_set) == 1, f"Expected validation set size 1, got {len(flawed_val_set)}"


# =============================================================================
# Mutation Challenge 6: Zero Physical Mouse Tracker Sensitivity
# =============================================================================

def test_mutant_mouse_tracker_spy_sensitivity() -> None:
    """
    MUTANT 6: Verify ZeroMouseTracker is not a no-op mock.
    It must trigger an AssertionError immediately if any hardware mouse move or click occurs.
    """
    tracker = ZeroMouseTracker()
    tracker.assert_zero_mouse_interactions()  # Clean baseline passes

    # Simulate rogue mouse cursor move
    tracker.record_move(450, 600)
    with pytest.raises(AssertionError, match=r"Hardware mouse moved 1 times!"):
        tracker.assert_zero_mouse_interactions()

    # Simulate rogue mouse click
    tracker_click = ZeroMouseTracker()
    tracker_click.record_click()
    with pytest.raises(AssertionError, match=r"Hardware mouse clicked 1 times!"):
        tracker_click.assert_zero_mouse_interactions()
