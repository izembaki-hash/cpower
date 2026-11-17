r"""
Unit and Integration Test Suite for CPower Dataset Integrity Validator.
Module under test: clients/python/cpower_dataset_validator.py

Covers 11 test classes and 48+ test cases:
  1. TestValidDatasets: Full valid datasets, PNG formats, no-images flag, non-click actions.
  2. TestJsonlSyntaxAndFiles: Missing files, malformed syntax, blank lines, missing required keys.
  3. TestImageIntegrity: Missing image, truncated bytes, corrupt text file, dimension mismatch.
  4. TestBBoxGeometry: Bounds violations, negative coordinates, inverted coordinates, collapsed zero area, malformed types.
  5. TestFlorenceTokens: <loc_1000> forbidden, out-of-range, wrong count, inverted bounds.
  6. TestDequantization: Standard pass, threshold violations, custom threshold, math helpers.
  7. TestClickContainment: Center click, boundary click, outside X/Y, missing click for click action.
  8. TestDatasetSynchronization: Florence > Act, Act > Florence, image desync, stats.json desync.
  9. TestInFlightSample: validate_sample with valid pair, Florence only, skip image check, inverted bbox, click outside.
 10. TestCLIInterface: Exit codes, stdout/stderr, JSON output mode, --no-images, --quiet, missing dir.
 11. TestLiveIntegration: Verification against live E:\cpower_training_data (if present).
"""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple, Union
import unittest
from unittest.mock import patch

from PIL import Image, ImageDraw

# Add clients/python to sys.path
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "clients" / "python"))

from cpower_dataset_validator import (  # noqa: E402
    check_click_inside_bbox,
    compute_max_dequant_error,
    dequantize_bbox,
    format_summary_table,
    main,
    validate_dataset,
    validate_sample,
)


class SyntheticDatasetBuilder:
    """
    Lightweight, isolated test fixture for building synthetic CPower datasets
    with granular control over valid records and deliberate fault injections.
    """

    def __init__(self, root_dir: Union[str, Path]):
        self.root_dir = Path(root_dir)
        self.images_dir = self.root_dir / "images"
        self.images_dir.mkdir(parents=True, exist_ok=True)
        self.florence_jsonl_path = self.root_dir / "dataset_florence2.jsonl"
        self.agent_act_jsonl_path = self.root_dir / "dataset_agent_act.jsonl"
        self.stats_path = self.root_dir / "stats.json"

        self.florence_records: List[Dict[str, Any]] = []
        self.act_records: List[Dict[str, Any]] = []
        self.stats_data: Dict[str, Any] = {
            "version": "1.0",
            "created_at": "2026-11-14 00:00:00",
            "total_trajectories": 1,
            "total_steps": 0,
            "successful_trajectories": 1,
            "florence_grounding_samples": 0,
        }

    def add_step(
        self,
        traj_id: str = "traj_test_01",
        step_idx: int = 0,
        img_size: Tuple[int, int] = (100, 100),
        bbox: Tuple[int, int, int, int] = (10, 10, 50, 50),
        phrase: str = "Submit Button",
        action_type: str = "click",
        click_coords: Optional[Tuple[int, int]] = (30, 30),
        img_format: str = "JPEG",
        image_state: str = "valid",  # "valid", "missing", "truncated", "corrupt_text"
        custom_target: Optional[str] = None,
        custom_florence_override: Optional[Dict[str, Any]] = None,
        custom_act_override: Optional[Dict[str, Any]] = None,
    ) -> Tuple[Dict[str, Any], Dict[str, Any]]:
        w, h = img_size
        ext = "jpg" if img_format.upper() == "JPEG" else "png"
        img_filename = f"{traj_id}_step_{step_idx:02d}.{ext}"
        img_path = self.images_dir / img_filename
        rel_img_path = f"images/{img_filename}"

        # 1. Manage image file creation on disk
        if image_state == "valid":
            img = Image.new("RGB", (w, h), color=(200, 220, 240))
            draw = ImageDraw.Draw(img)
            try:
                rx1, ry1, rx2, ry2 = bbox
                if rx1 > rx2:
                    rx1, rx2 = rx2, rx1
                if ry1 > ry2:
                    ry1, ry2 = ry2, ry1
                draw.rectangle([rx1, ry1, rx2, ry2], fill=(50, 100, 200), outline=(0, 0, 0))
            except Exception:
                pass
            img.save(img_path, format=img_format)
        elif image_state == "truncated":
            img_path.write_bytes(b"\xff\xd8\xff\xe0" + b"\x00" * 10)  # corrupt truncated header
        elif image_state == "corrupt_text":
            img_path.write_text("NOT AN IMAGE FILE", encoding="utf-8")
        elif image_state == "missing":
            pass  # Do not create file

        # 2. Florence-2 token calculation (center-based binning matching midpoint dequant)
        x1, y1, x2, y2 = bbox
        nx1 = max(0, min(998, int(1000.0 * x1 / max(1, w))))
        ny1 = max(0, min(998, int(1000.0 * y1 / max(1, h))))
        nx2 = max(nx1 + 1, min(999, int(1000.0 * x2 / max(1, w))))
        ny2 = max(ny1 + 1, min(999, int(1000.0 * y2 / max(1, h))))
        target = custom_target or f"{phrase}<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"

        f_rec = {
            "id": f"{traj_id}_{step_idx:02d}",
            "image": rel_img_path,
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": phrase,
            "target": target,
            "bbox_px": list(bbox),
            "image_size": [w, h],
        }
        if custom_florence_override:
            f_rec.update(custom_florence_override)

        a_rec = {
            "trajectory_id": traj_id,
            "step_index": step_idx,
            "goal": "Test goal",
            "image": rel_img_path,
            "action": {
                "type": action_type,
                "phrase": phrase,
                "click": list(click_coords) if click_coords is not None else None,
                "text": "",
                "keys": "",
            },
            "window": "Test Window",
        }
        if custom_act_override:
            a_rec.update(custom_act_override)

        self.florence_records.append(f_rec)
        self.act_records.append(a_rec)
        self.stats_data["total_steps"] += 1
        self.stats_data["florence_grounding_samples"] += 1
        return f_rec, a_rec

    def write_dataset(self) -> None:
        with open(self.florence_jsonl_path, "w", encoding="utf-8") as f:
            for r in self.florence_records:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        with open(self.agent_act_jsonl_path, "w", encoding="utf-8") as f:
            for r in self.act_records:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        self.stats_path.write_text(json.dumps(self.stats_data, indent=2), encoding="utf-8")


class BaseValidatorTestCase(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp(prefix="cpower_validator_test_")
        self.root_path = Path(self.temp_dir)
        self.builder = SyntheticDatasetBuilder(self.root_path)

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)


class TestValidDatasets(BaseValidatorTestCase):
    """1. Tests for fully valid datasets and standard configuration variations."""

    def test_valid_full_dataset(self):
        self.builder.add_step(step_idx=0, bbox=(10, 10, 50, 50), click_coords=(30, 30))
        self.builder.add_step(step_idx=1, bbox=(20, 20, 80, 70), click_coords=(50, 45))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid, f"Expected valid dataset, got errors: {report['errors']}")
        self.assertEqual(len(report["errors"]), 0)
        self.assertEqual(report["florence_count"], 2)
        self.assertEqual(report["act_count"], 2)
        self.assertEqual(report["images_checked"], 2)
        self.assertLess(report["stats"]["max_observed_dequant_error"], 1.0)
        self.assertEqual(report["stats"]["checks"]["json_schema"], "PASS")

    def test_valid_png_images(self):
        self.builder.add_step(step_idx=0, img_format="PNG", bbox=(15, 15, 60, 60), click_coords=(35, 35))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid, f"Expected valid PNG dataset, errors: {report['errors']}")
        self.assertEqual(report["images_checked"], 1)

    def test_valid_no_images_flag(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path, check_images=False)
        self.assertTrue(is_valid)
        self.assertEqual(report["images_checked"], 0)
        self.assertEqual(report["stats"]["checks"]["image_integrity"], "SKIPPED")

    def test_valid_non_click_actions(self):
        self.builder.add_step(step_idx=0, action_type="type", click_coords=None)
        self.builder.add_step(step_idx=1, action_type="wait", click_coords=None)
        self.builder.add_step(step_idx=2, action_type="hotkey", click_coords=None)
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid, f"Expected valid non-click actions, errors: {report['errors']}")
        self.assertEqual(report["florence_count"], 3)


class TestJsonlSyntaxAndFiles(BaseValidatorTestCase):
    """2. Tests for JSONL syntax, missing files, blank lines, and missing keys."""

    def test_missing_florence_file(self):
        self.builder.add_step()
        self.builder.write_dataset()
        self.builder.florence_jsonl_path.unlink()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Missing Florence-2" in e for e in report["errors"]))

    def test_missing_act_file(self):
        self.builder.add_step()
        self.builder.write_dataset()
        self.builder.agent_act_jsonl_path.unlink()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Missing Agent Act" in e for e in report["errors"]))

    def test_malformed_jsonl_florence_syntax(self):
        self.builder.add_step()
        self.builder.write_dataset()
        with open(self.builder.florence_jsonl_path, "a", encoding="utf-8") as f:
            f.write("{broken json line\n")

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Invalid JSON" in e for e in report["errors"]))

    def test_malformed_jsonl_act_syntax(self):
        self.builder.add_step()
        self.builder.write_dataset()
        with open(self.builder.agent_act_jsonl_path, "a", encoding="utf-8") as f:
            f.write("{\"not_closed\": 1\n")

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Invalid JSON" in e for e in report["errors"]))

    def test_empty_lines_ignored(self):
        self.builder.add_step()
        self.builder.write_dataset()
        with open(self.builder.florence_jsonl_path, "a", encoding="utf-8") as f:
            f.write("\n   \n\t\n")
        with open(self.builder.agent_act_jsonl_path, "a", encoding="utf-8") as f:
            f.write("\n\n")

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid, f"Empty lines should be ignored, errors: {report['errors']}")

    def test_missing_required_keys_florence(self):
        self.builder.add_step(custom_florence_override={"target": None})
        # Remove target entirely
        del self.builder.florence_records[0]["target"]
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Missing required Florence field: 'target'" in e for e in report["errors"]))

    def test_missing_required_keys_act(self):
        self.builder.add_step()
        del self.builder.act_records[0]["action"]
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Missing required Act field: 'action'" in e for e in report["errors"]))


class TestImageIntegrity(BaseValidatorTestCase):
    """3. Tests for image existence, formatting, corruption, and dimension sync."""

    def test_missing_image_file(self):
        self.builder.add_step(image_state="missing")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Image file does not exist" in e for e in report["errors"]))

    def test_truncated_image_file(self):
        self.builder.add_step(image_state="truncated")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Corrupted or unreadable image" in e for e in report["errors"]))

    def test_corrupt_text_image_file(self):
        self.builder.add_step(image_state="corrupt_text")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Corrupted or unreadable image" in e for e in report["errors"]))

    def test_image_dimension_mismatch(self):
        self.builder.add_step(
            img_size=(100, 100),
            custom_florence_override={"image_size": [200, 200]},
        )
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("dimension mismatch" in e for e in report["errors"]))


class TestBBoxGeometry(BaseValidatorTestCase):
    """4. Tests for Cartesian bounding box geometric constraints."""

    def test_bbox_exceeds_image_width(self):
        self.builder.add_step(img_size=(100, 100), bbox=(10, 10, 105, 50))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("out of bounds" in e and "X" in e for e in report["errors"]))

    def test_bbox_exceeds_image_height(self):
        self.builder.add_step(img_size=(100, 100), bbox=(10, 10, 50, 105))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("out of bounds" in e and "Y" in e for e in report["errors"]))

    def test_bbox_negative_coordinate(self):
        self.builder.add_step(img_size=(100, 100), bbox=(-5, 10, 50, 50))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("out of bounds" in e and "X" in e for e in report["errors"]))

    def test_bbox_inverted_x(self):
        self.builder.add_step(img_size=(100, 100), bbox=(60, 10, 30, 50))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("X coordinates inverted" in e for e in report["errors"]))

    def test_bbox_inverted_y(self):
        self.builder.add_step(img_size=(100, 100), bbox=(10, 60, 50, 30))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Y coordinates inverted" in e for e in report["errors"]))

    def test_bbox_zero_area_collapsed(self):
        self.builder.add_step(img_size=(100, 100), bbox=(30, 10, 30, 50))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("collapsed" in e for e in report["errors"]))

    def test_bbox_invalid_type_or_length(self):
        self.builder.add_step(custom_florence_override={"bbox_px": [10, 20, 30]})
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Invalid bbox_px format" in e for e in report["errors"]))


class TestFlorenceTokens(BaseValidatorTestCase):
    """5. Tests for Florence-2 location tokens bounds, counts, and ranges."""

    def test_token_forbidden_loc_1000(self):
        self.builder.add_step(custom_target="Button<loc_100><loc_100><loc_1000><loc_500>")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("forbidden <loc_1000>" in e for e in report["errors"]))

    def test_token_negative_or_corrupt(self):
        self.builder.add_step(custom_target="Button<loc_abc><loc_100><loc_300><loc_400>")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Target must contain exactly 4" in e for e in report["errors"]))

    def test_token_count_not_four(self):
        self.builder.add_step(custom_target="Button<loc_100><loc_200>")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Expected 4" in e or "exactly 4" in e for e in report["errors"]))

    def test_token_inverted_normalized_bounds(self):
        # nx1=500, ny1=100, nx2=200, ny2=300 (nx1 > nx2)
        self.builder.add_step(custom_target="Button<loc_500><loc_100><loc_200><loc_300>")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Degenerate normalized tokens" in e for e in report["errors"]))

    def test_token_missing_phrase_prefix(self):
        # Target without text phrase
        self.builder.add_step(custom_target="<loc_100><loc_100><loc_200><loc_200>")
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("does not start with phrase suffix" in e for e in report["errors"]))


class TestDequantization(BaseValidatorTestCase):
    """6. Tests for Florence-2 dequantization midpoint formula and precision thresholds."""

    def test_dequantization_pass(self):
        # 100x100 image, bbox [10, 10, 50, 50], tokens <loc_100><loc_100><loc_500><loc_500>
        # dequant: (100 + 0.5) * 0.1 = 10.05px, error = 0.05px < 1.0px
        self.builder.add_step(
            phrase="Button",
            img_size=(100, 100),
            bbox=(10, 10, 50, 50),
            custom_target="Button<loc_100><loc_100><loc_500><loc_500>",
        )
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid, f"Expected pass, got errors: {report['errors']}")
        self.assertLess(report["stats"]["max_observed_dequant_error"], 0.1)

    def test_dequantization_fail_exceeds_threshold(self):
        # Artificially shift tokens by 20 bins (2px discrepancy)
        self.builder.add_step(
            phrase="Button",
            img_size=(100, 100),
            bbox=(10, 10, 50, 50),
            custom_target="Button<loc_120><loc_100><loc_500><loc_500>",
        )
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path, max_dequant_error=1.0)
        self.assertFalse(is_valid)
        self.assertTrue(any("Dequantization error" in e for e in report["errors"]))

    def test_dequantization_custom_threshold(self):
        # 12px shift -> 1.2px error
        self.builder.add_step(
            phrase="Button",
            img_size=(100, 100),
            bbox=(10, 10, 50, 50),
            custom_target="Button<loc_112><loc_100><loc_500><loc_500>",
        )
        self.builder.write_dataset()

        # Fail at default threshold 1.0
        is_valid_1, _ = validate_dataset(self.root_path, max_dequant_error=1.0)
        self.assertFalse(is_valid_1)

        # Pass at threshold 2.0
        is_valid_2, report_2 = validate_dataset(self.root_path, max_dequant_error=2.0)
        self.assertTrue(is_valid_2, f"Expected pass at threshold 2.0, got: {report_2['errors']}")

    def test_dequantize_bbox_formula_parity(self):
        # Midpoint formula: (token + 0.5) * (dim / 1000.0)
        tokens = (100, 200, 300, 400)
        size = (1000, 500)
        bx1, by1, bx2, by2 = dequantize_bbox(tokens, size)
        self.assertAlmostEqual(bx1, 100.5, places=4)
        self.assertAlmostEqual(by1, (200 + 0.5) * 0.5, places=4)
        self.assertAlmostEqual(bx2, 300.5, places=4)
        self.assertAlmostEqual(by2, (400 + 0.5) * 0.5, places=4)

    def test_compute_max_dequant_error_math(self):
        b1 = [10.0, 20.0, 30.0, 40.0]
        b2 = [10.2, 19.5, 30.0, 40.8]
        err = compute_max_dequant_error(b1, b2)
        self.assertAlmostEqual(err, 0.8, places=4)


class TestClickContainment(BaseValidatorTestCase):
    """7. Tests for action click containment within target bounding boxes."""

    def test_click_inside_bbox_center(self):
        self.builder.add_step(bbox=(10, 10, 50, 50), click_coords=(30, 30))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid)

    def test_click_on_bbox_boundary(self):
        self.builder.add_step(bbox=(10, 10, 50, 50), click_coords=(10, 10))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertTrue(is_valid)

    def test_click_outside_bbox_x(self):
        self.builder.add_step(bbox=(10, 10, 50, 50), click_coords=(60, 30))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("outside target bbox" in e for e in report["errors"]))

    def test_click_outside_bbox_y(self):
        self.builder.add_step(bbox=(10, 10, 50, 50), click_coords=(30, 5))
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("outside target bbox" in e for e in report["errors"]))

    def test_click_action_missing_coords(self):
        self.builder.add_step(action_type="click", click_coords=None)
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("requires 'click' coordinates" in e for e in report["errors"]))

    def test_check_click_inside_bbox_helper(self):
        self.assertTrue(check_click_inside_bbox((25, 25), (10, 10, 50, 50)))
        self.assertTrue(check_click_inside_bbox((10, 10), (10, 10, 50, 50)))
        self.assertTrue(check_click_inside_bbox((50, 50), (10, 10, 50, 50)))
        self.assertFalse(check_click_inside_bbox((9, 25), (10, 10, 50, 50)))
        self.assertFalse(check_click_inside_bbox((25, 51), (10, 10, 50, 50)))


class TestDatasetSynchronization(BaseValidatorTestCase):
    """8. Tests for dataset synchronization between Florence-2, Agent Act, and stats.json."""

    def test_count_mismatch_florence_greater(self):
        self.builder.add_step(step_idx=0)
        self.builder.add_step(step_idx=1)
        self.builder.write_dataset()

        # Remove 1 act entry
        self.builder.act_records.pop()
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Record count mismatch" in e for e in report["errors"]))

    def test_count_mismatch_act_greater(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        # Add extra act entry
        extra_act = dict(self.builder.act_records[0])
        extra_act["step_index"] = 99
        with open(self.builder.agent_act_jsonl_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(extra_act) + "\n")

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Record count mismatch" in e for e in report["errors"]))

    def test_mismatched_image_references(self):
        self.builder.add_step(step_idx=0)
        self.builder.act_records[0]["image"] = "images/completely_different.jpg"
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid)
        self.assertTrue(any("Image mismatch" in e for e in report["errors"]))

    def test_stats_json_count_mismatch(self):
        self.builder.add_step(step_idx=0)
        self.builder.stats_data["florence_grounding_samples"] = 999
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        # Stats mismatch produces a warning, does not fail valid dataset records
        self.assertTrue(any("does not match florence_jsonl" in w for w in report["warnings"]))


class TestInFlightSample(BaseValidatorTestCase):
    """9. Tests for validate_sample in-flight record validation API."""

    def test_validate_sample_valid_pair(self):
        f_rec, a_rec = self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        ok, errors = validate_sample(
            florence_record=f_rec,
            act_record=a_rec,
            base_dir=self.root_path,
            check_image=True,
        )
        self.assertTrue(ok, f"Expected valid sample, got errors: {errors}")
        self.assertEqual(len(errors), 0)

    def test_validate_sample_florence_only(self):
        f_rec, _ = self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        ok, errors = validate_sample(
            florence_record=f_rec,
            act_record=None,
            base_dir=self.root_path,
            check_image=True,
        )
        self.assertTrue(ok)
        self.assertEqual(len(errors), 0)

    def test_validate_sample_without_image_check(self):
        f_rec = {
            "image": "images/nonexistent_image.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [10, 10, 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(
            florence_record=f_rec,
            act_record=None,
            base_dir=self.root_path,
            check_image=False,
        )
        self.assertTrue(ok, f"Expected pass without image check, errors: {errors}")

    def test_validate_sample_inverted_bbox(self):
        f_rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [60, 10, 30, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(
            florence_record=f_rec,
            act_record=None,
            check_image=False,
        )
        self.assertFalse(ok)
        self.assertTrue(any("X coordinates inverted" in e for e in errors))

    def test_validate_sample_click_outside(self):
        f_rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [10, 10, 50, 50],
            "image_size": [100, 100],
        }
        a_rec = {
            "trajectory_id": "traj_01",
            "step_index": 0,
            "image": "images/test.jpg",
            "action": {
                "type": "click",
                "click": [80, 80],
            },
        }
        ok, errors = validate_sample(
            florence_record=f_rec,
            act_record=a_rec,
            check_image=False,
        )
        self.assertFalse(ok)
        self.assertTrue(any("outside target bbox" in e for e in errors))


class TestCLIInterface(BaseValidatorTestCase):
    """10. Tests for CLI argument parsing, output formatting, and exit codes."""

    def test_cli_valid_dataset(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        code = main(["--dir", str(self.root_path), "--quiet"])
        self.assertEqual(code, 0)

    def test_cli_invalid_dataset(self):
        self.builder.add_step(step_idx=0, bbox=(100, 10, 50, 50))  # inverted bbox
        self.builder.write_dataset()

        code = main(["--dir", str(self.root_path), "--quiet"])
        self.assertEqual(code, 1)

    def test_cli_json_output_mode(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        captured = io.StringIO()
        old_stdout = sys.stdout
        try:
            sys.stdout = captured
            code = main(["--dir", str(self.root_path), "--json"])
        finally:
            sys.stdout = old_stdout

        self.assertEqual(code, 0)
        out_str = captured.getvalue()
        parsed = json.loads(out_str)
        self.assertTrue(parsed["valid"])
        self.assertEqual(parsed["florence_count"], 1)

    def test_cli_no_images_flag(self):
        self.builder.add_step(step_idx=0, image_state="missing")
        self.builder.write_dataset()

        # Should fail with image check
        code_fail = main(["--dir", str(self.root_path), "--quiet"])
        self.assertEqual(code_fail, 1)

        # Should pass with --no-images
        code_pass = main(["--dir", str(self.root_path), "--no-images", "--quiet"])
        self.assertEqual(code_pass, 0)

    def test_cli_missing_directory(self):
        nonexistent = self.root_path / "nonexistent_subfolder"
        code = main(["--dir", str(nonexistent), "--quiet"])
        self.assertEqual(code, 1)

    def test_cli_quiet_flag(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        captured = io.StringIO()
        old_stdout = sys.stdout
        try:
            sys.stdout = captured
            code = main(["--dir", str(self.root_path), "--quiet"])
        finally:
            sys.stdout = old_stdout

        self.assertEqual(code, 0)
        self.assertEqual(captured.getvalue(), "")

    def test_format_summary_table_rendering(self):
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()
        _, report = validate_dataset(self.root_path)

        table_str = format_summary_table(report)
        self.assertIn("CPower Dataset Integrity & Grounding Validator", table_str)
        self.assertIn("OVERALL VERDICT: PASS", table_str)
        self.assertIn("Dataset Record Synchronization", table_str)


class TestAdversarialRobustness(BaseValidatorTestCase):
    """12. Tests for adversarial inputs, IEEE 754 edge cases, null handling, and crash containment."""

    def test_bbox_nan_coordinates(self):
        """Test that bbox_px containing float('nan') with non-click action fails validation with clear error."""
        self.builder.add_step(
            action_type="wait",
            click_coords=None,
            bbox=(10, 10, 50, 50),
            custom_florence_override={"bbox_px": [float("nan"), 10, 50, 50]},
        )
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid, "Expected dataset with NaN bbox to fail validation")
        self.assertTrue(
            any("nan" in e.lower() for e in report["errors"]),
            f"Expected error message mentioning NaN, got: {report['errors']}",
        )

        # In-flight validation check
        f_rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [float("nan"), 10, 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(florence_record=f_rec, check_image=False)
        self.assertFalse(ok, "Expected in-flight sample with NaN bbox to fail validation")
        self.assertTrue(any("nan" in e.lower() for e in errors))

    def test_bbox_inf_coordinates(self):
        """Test that bbox_px containing float('inf') or -float('inf') fails validation."""
        # Test positive infinity
        self.builder.add_step(
            step_idx=0,
            action_type="wait",
            click_coords=None,
            bbox=(10, 10, 50, 50),
            custom_florence_override={"bbox_px": [float("inf"), 10, 50, 50]},
        )
        self.builder.write_dataset()

        is_valid, report = validate_dataset(self.root_path)
        self.assertFalse(is_valid, "Expected dataset with Inf bbox coordinates to fail validation")
        self.assertTrue(
            any("inf" in e.lower() for e in report["errors"]),
            f"Expected error message mentioning Inf, got: {report['errors']}",
        )

        # In-flight validation check with -inf
        f_rec_inf = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [10, float("-inf"), 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(florence_record=f_rec_inf, check_image=False)
        self.assertFalse(ok)
        self.assertTrue(any("inf" in e.lower() for e in errors))

    def test_explicit_null_target(self):
        """Test that 'target': None fails validation cleanly without unhandled TypeError."""
        self.builder.add_step(custom_florence_override={"target": None})
        self.builder.write_dataset()

        try:
            is_valid, report = validate_dataset(self.root_path)
        except TypeError as e:
            self.fail(f"validate_dataset raised unhandled TypeError on null target: {e}")

        self.assertFalse(is_valid, "Expected dataset with null target to fail validation")
        self.assertTrue(
            any("target" in e.lower() and "string" in e.lower() for e in report["errors"]),
            f"Expected type error message for target, got: {report['errors']}",
        )

        # In-flight validation check
        f_rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": None,
            "bbox_px": [10, 10, 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(florence_record=f_rec, check_image=False)
        self.assertFalse(ok)
        self.assertTrue(any("target" in e.lower() and "string" in e.lower() for e in errors))

    def test_explicit_null_image(self):
        """Test that 'image': None fails validation cleanly without unhandled TypeError."""
        self.builder.add_step(custom_florence_override={"image": None})
        self.builder.write_dataset()

        try:
            is_valid, report = validate_dataset(self.root_path)
        except TypeError as e:
            self.fail(f"validate_dataset raised unhandled TypeError on null image: {e}")

        self.assertFalse(is_valid, "Expected dataset with null image to fail validation")
        self.assertTrue(
            any("image" in e.lower() and "string" in e.lower() for e in report["errors"]),
            f"Expected type error message for image, got: {report['errors']}",
        )

        # In-flight validation check
        f_rec = {
            "image": None,
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "Button",
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [10, 10, 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(florence_record=f_rec, base_dir=self.root_path, check_image=True)
        self.assertFalse(ok)
        self.assertTrue(any("image" in e.lower() and "string" in e.lower() for e in errors))

    def test_non_string_suffix(self):
        """Test that 'suffix': 123 fails validation cleanly without unhandled TypeError."""
        self.builder.add_step(custom_florence_override={"suffix": 123})
        self.builder.write_dataset()

        try:
            is_valid, report = validate_dataset(self.root_path)
        except TypeError as e:
            self.fail(f"validate_dataset raised unhandled TypeError on non-string suffix: {e}")

        self.assertFalse(is_valid, "Expected dataset with integer suffix to fail validation")
        self.assertTrue(
            any("suffix" in e.lower() and "string" in e.lower() for e in report["errors"]),
            f"Expected type error message for suffix, got: {report['errors']}",
        )

        # In-flight validation check
        f_rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": 123,
            "target": "Button<loc_100><loc_100><loc_500><loc_500>",
            "bbox_px": [10, 10, 50, 50],
            "image_size": [100, 100],
        }
        ok, errors = validate_sample(florence_record=f_rec, check_image=False)
        self.assertFalse(ok)
        self.assertTrue(any("suffix" in e.lower() and "string" in e.lower() for e in errors))

    def test_unhandled_exception_containment(self):
        """Test that validate_dataset catches any unexpected exception in a sample without crashing."""
        self.builder.add_step(step_idx=0)
        self.builder.write_dataset()

        with patch("cpower_dataset_validator.validate_sample", side_effect=RuntimeError("Simulated unexpected crash")):
            try:
                is_valid, report = validate_dataset(self.root_path)
            except Exception as e:
                self.fail(f"validate_dataset crashed with unhandled exception: {type(e).__name__}: {e}")

            self.assertFalse(is_valid, "Dataset with sample exception must report valid=False")
            self.assertTrue(
                any("Unhandled exception during sample validation: RuntimeError: Simulated unexpected crash" in e for e in report["errors"]),
                f"Expected error message with exception details, got: {report['errors']}",
            )
            self.assertEqual(report["stats"]["checks"].get("unhandled_exceptions", "FAIL"), "FAIL")


class TestLiveIntegration(unittest.TestCase):
    """11. Integration test against live training data directory (if present)."""

    def test_live_training_data_if_exists(self):
        live_dir = Path(r"E:\cpower_training_data")
        if not live_dir.is_dir():
            self.skipTest(f"Live dataset directory {live_dir} not available on this machine.")

        is_valid, report = validate_dataset(dataset_dir=live_dir, check_images=True, max_dequant_error=1.0)
        self.assertTrue(is_valid, f"Live dataset failed validation: {report['errors'][:10]}")
        self.assertEqual(len(report["errors"]), 0)
        self.assertGreaterEqual(report["florence_count"], 406)
        self.assertGreaterEqual(report["act_count"], 406)
        self.assertGreaterEqual(report["images_checked"], 406)
        self.assertLess(report["stats"]["max_observed_dequant_error"], 1.0)
        print(f"\n[LIVE INTEGRITY VERIFIED] {report['florence_count']} samples valid on {live_dir} in {report['stats']['duration_sec']}s")


if __name__ == "__main__":
    unittest.main(verbosity=2)
