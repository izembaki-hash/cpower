r"""
CPower Dataset Integrity & Grounding Validator (clients/python/cpower_dataset_validator.py).

Validates CPower multimodal datasets and trajectories for Florence-2 fine-tuning and agent training:
  1. JSONL schema conformance (dataset_florence2.jsonl & dataset_agent_act.jsonl).
  2. Image existence, format (JPEG/PNG), readability, and dimension integrity.
  3. Bounding box Cartesian geometry (0 <= x1 < x2 <= W, 0 <= y1 < y2 <= H).
  4. Florence-2 coordinate token bounds (integers in [0..999], no <loc_1000>, non-degenerate).
  5. Action click containment within target bboxes.
  6. Sub-pixel roundtrip dequantization precision against Florence-2 BoxQuantizer (< 1.0px).
  7. Cross-dataset synchronization (1-to-1 record alignment and stats.json sync).
"""

from __future__ import annotations

import argparse
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import time
from typing import Any, Dict, List, Optional, Sequence, Tuple, Union

from PIL import Image

# Filename standards
FLORENCE_JSONL_NAME = "dataset_florence2.jsonl"
AGENT_ACT_JSONL_NAME = "dataset_agent_act.jsonl"
STATS_JSON_NAME = "stats.json"
IMAGES_DIR_NAME = "images"

# Regex for Florence-2 location tokens
LOC_TOKEN_REGEX = re.compile(r"<loc_(\d+)>")

# Actions requiring click containment check
CLICK_ACTION_TYPES = ("click", "double_click", "right_click")
DEFAULT_MAX_DEQUANT_ERROR = 1.0
DEFAULT_DATASET_DIR = r"E:\cpower_training_data"


def dequantize_bbox(
    tokens: Sequence[int],
    image_size: Sequence[int],
) -> Tuple[float, float, float, float]:
    """
    Recovers pixel coordinates from Florence-2 normalized tokens [0..999].
    Exact parity with Florence-2 BoxQuantizer.dequantize (floor mode, center-of-bin):
      dequant_coord = (token + 0.5) * (dim / 1000.0)
    """
    if len(tokens) != 4:
        raise ValueError(f"Expected exactly 4 tokens, got {len(tokens)}")
    if len(image_size) != 2:
        raise ValueError(f"Expected 2 image dimensions (width, height), got {len(image_size)}")

    nx1, ny1, nx2, ny2 = tokens
    w, h = image_size
    bx1 = (nx1 + 0.5) * (float(w) / 1000.0)
    by1 = (ny1 + 0.5) * (float(h) / 1000.0)
    bx2 = (nx2 + 0.5) * (float(w) / 1000.0)
    by2 = (ny2 + 0.5) * (float(h) / 1000.0)
    return (bx1, by1, bx2, by2)


def compute_max_dequant_error(
    original_bbox: Sequence[float],
    dequantized_bbox: Sequence[float],
) -> float:
    """Computes Chebyshev L-infinity maximum coordinate error between original and recovered bbox."""
    if len(original_bbox) != 4 or len(dequantized_bbox) != 4:
        raise ValueError("Both bounding boxes must contain exactly 4 coordinates")
    return max(abs(float(o) - float(d)) for o, d in zip(original_bbox, dequantized_bbox))


def check_click_inside_bbox(
    click: Sequence[float],
    bbox: Sequence[float],
) -> bool:
    """Checks whether click coordinates fall within [x1, y1, x2, y2] bounding box."""
    if len(click) != 2:
        raise ValueError("Click coordinates must be (cx, cy)")
    if len(bbox) != 4:
        raise ValueError("Bounding box must be (x1, y1, x2, y2)")
    cx, cy = click
    x1, y1, x2, y2 = bbox
    return (x1 <= cx <= x2) and (y1 <= cy <= y2)


def validate_sample(
    florence_record: Dict[str, Any],
    act_record: Optional[Dict[str, Any]] = None,
    base_dir: Optional[Union[str, Path]] = None,
    check_image: bool = True,
    max_dequant_error: float = DEFAULT_MAX_DEQUANT_ERROR,
) -> Tuple[bool, List[str]]:
    """
    Validates a single in-memory Florence grounding record and optional Act record pair.

    Parameters:
      florence_record: Dict containing keys ['image', 'prefix', 'suffix', 'target', 'bbox_px'/'bbox', 'image_size']
      act_record: Optional dict containing keys ['action', 'image', ...]
      base_dir: Root directory for resolving relative image paths
      check_image: If True and base_dir provided, checks image existence, format, and dimensions
      max_dequant_error: Maximum allowed coordinate discrepancy in pixels

    Returns:
      (is_valid, list_of_error_strings)
    """
    errors: List[str] = []
    if not isinstance(florence_record, dict):
        return False, ["florence_record must be a dictionary"]

    # 1. Florence schema fields
    required_florence_fields = ["image", "prefix", "suffix", "target", "image_size"]
    for f in required_florence_fields:
        if f not in florence_record:
            errors.append(f"Missing required Florence field: '{f}'")

    # Bbox key can be bbox_px or bbox
    bbox = florence_record.get("bbox_px")
    if bbox is None:
        bbox = florence_record.get("bbox")
    if bbox is None:
        errors.append("Missing required Florence field: 'bbox_px'")

    if errors:
        return False, errors

    # Type safety checks for schema fields
    img_rel = florence_record.get("image")
    if not isinstance(img_rel, str) or not img_rel.strip():
        errors.append(f"Field 'image' must be a non-empty string, got {img_rel!r}")
        return False, errors

    target = florence_record.get("target")
    if not isinstance(target, str):
        errors.append(f"Field 'target' must be a string, got {target!r}")
        return False, errors

    suffix = florence_record.get("suffix")
    if suffix is not None and not isinstance(suffix, str):
        errors.append(f"Field 'suffix' must be a string, got {suffix!r}")
        return False, errors

    prefix = florence_record.get("prefix")
    if prefix is not None and (not isinstance(prefix, str) or not prefix.strip()):
        errors.append(f"Field 'prefix' must be a non-empty string, got {prefix!r}")
        return False, errors

    img_size = florence_record.get("image_size")

    # 2. Image size validity
    if not (isinstance(img_size, (list, tuple)) and len(img_size) == 2):
        errors.append(f"Invalid image_size format: {img_size}")
        return False, errors
    try:
        w_f, h_f = float(img_size[0]), float(img_size[1])
        if not (math.isfinite(w_f) and math.isfinite(h_f)):
            errors.append(f"Image dimensions must be finite numbers, got {img_size}")
            return False, errors
        w, h = int(w_f), int(h_f)
    except (ValueError, TypeError, OverflowError):
        errors.append(f"Image dimensions must be integers, got {img_size}")
        return False, errors

    if w <= 0 or h <= 0:
        errors.append(f"Image dimensions must be positive, got ({w}, {h})")
        return False, errors

    # 3. Bounding box geometry
    if not (isinstance(bbox, (list, tuple)) and len(bbox) == 4):
        errors.append(f"Invalid bbox_px format: {bbox}")
        return False, errors

    try:
        x1, y1, x2, y2 = float(bbox[0]), float(bbox[1]), float(bbox[2]), float(bbox[3])
    except (ValueError, TypeError):
        errors.append(f"Bbox coordinates must be numeric, got {bbox}")
        return False, errors

    if any(math.isnan(c) or math.isinf(c) for c in (x1, y1, x2, y2)):
        errors.append(f"Bbox coordinates contain NaN or Inf values: [{x1}, {y1}, {x2}, {y2}]")
        return False, errors

    if x1 < 0 or x2 > w:
        errors.append(f"Bbox X coordinates out of bounds: expected 0 <= x1 < x2 <= {w}, got [{x1}, {x2}]")
    if y1 < 0 or y2 > h:
        errors.append(f"Bbox Y coordinates out of bounds: expected 0 <= y1 < y2 <= {h}, got [{y1}, {y2}]")
    if x1 >= x2:
        errors.append(f"Bbox X coordinates inverted or collapsed: expected x1 < x2, got x1={x1}, x2={x2}")
    if y1 >= y2:
        errors.append(f"Bbox Y coordinates inverted or collapsed: expected y1 < y2, got y1={y1}, y2={y2}")

    # 4. Florence-2 location tokens
    tokens = LOC_TOKEN_REGEX.findall(target)
    if len(tokens) != 4:
        errors.append(f"Target must contain exactly 4 <loc_...> tokens, found {len(tokens)} in '{target}'")
    else:
        # Check that target starts with phrase suffix
        if suffix and not target.startswith(suffix):
            errors.append(f"Target '{target}' does not start with phrase suffix '{suffix}'")

        int_tokens = [int(t) for t in tokens]
        if any(t < 0 or t > 999 for t in int_tokens):
            errors.append(f"Tokens must be in [0..999], got {int_tokens}")
        if "<loc_1000>" in target:
            errors.append("Target contains forbidden <loc_1000>")

        nx1, ny1, nx2, ny2 = int_tokens
        if nx1 >= nx2 or ny1 >= ny2:
            errors.append(f"Degenerate normalized tokens: [{nx1}, {ny1}, {nx2}, {ny2}]")
        else:
            # 5. Roundtrip dequantization precision (only if geometry was valid)
            if x1 < x2 and y1 < y2 and w > 0 and h > 0:
                dequant_box = dequantize_bbox((nx1, ny1, nx2, ny2), (w, h))
                err = compute_max_dequant_error((x1, y1, x2, y2), dequant_box)
                if err > max_dequant_error:
                    errors.append(f"Dequantization error {err:.4f}px exceeds limit {max_dequant_error}px")

    # 6. Act record & click containment
    if act_record is not None:
        if not isinstance(act_record, dict):
            errors.append(f"Act record must be a dict, got {type(act_record).__name__}")
            return False, errors

        for act_req_key in ("trajectory_id", "step_index", "image", "action"):
            if act_req_key not in act_record:
                errors.append(f"Missing required Act field: '{act_req_key}'")

        act_img = act_record.get("image")
        if act_img != img_rel:
            errors.append(f"Image mismatch between Florence ({img_rel}) and Act ({act_img})")

        action = act_record.get("action")
        if not isinstance(action, dict):
            errors.append(f"Invalid action format in Act record: {action}")
        else:
            act_type = action.get("type", "")
            click = action.get("click")
            if act_type in CLICK_ACTION_TYPES:
                if click is None:
                    errors.append(f"Action '{act_type}' requires 'click' coordinates, got None")
                elif not (isinstance(click, (list, tuple)) and len(click) == 2):
                    errors.append(f"Invalid click format: {click}")
                else:
                    try:
                        cx, cy = float(click[0]), float(click[1])
                    except (ValueError, TypeError):
                        errors.append(f"Click coordinates must be numeric, got {click}")
                    else:
                        if any(math.isnan(c) or math.isinf(c) for c in (cx, cy)):
                            errors.append(f"Click coordinates contain NaN or Inf values: [{cx}, {cy}]")
                        elif not check_click_inside_bbox((cx, cy), (x1, y1, x2, y2)):
                            errors.append(f"Click [{cx}, {cy}] outside target bbox [{x1}, {y1}, {x2}, {y2}]")

    # 7. Image file check (if requested and base_dir provided)
    if check_image and base_dir is not None:
        try:
            img_path = Path(base_dir) / img_rel
            if not img_path.exists():
                errors.append(f"Image file does not exist: {img_path}")
            else:
                with Image.open(img_path) as im:
                    fmt = im.format
                    if fmt not in ("JPEG", "PNG"):
                        errors.append(f"Unsupported image format: {fmt} (expected JPEG or PNG)")
                    im.verify()
                with Image.open(img_path) as im:
                    actual_w, actual_h = im.size
                    if actual_w != w or actual_h != h:
                        errors.append(f"Image dimension mismatch: disk has ({actual_w}, {actual_h}), JSON has ({w}, {h})")
        except Exception as e:
            errors.append(f"Corrupted or unreadable image file {img_rel}: {e}")

    return len(errors) == 0, errors


def validate_dataset(
    dataset_dir: Optional[Union[str, Path]] = None,
    check_images: bool = True,
    max_dequant_error: float = DEFAULT_MAX_DEQUANT_ERROR,
    verbose: bool = False,
    **kwargs: Any,
) -> Tuple[bool, Dict[str, Any]]:
    r"""
    Validates a complete CPower dataset directory.

    Parameters:
      dataset_dir: Root directory of the dataset (e.g. E:\cpower_training_data).
                   If None, resolves to DEFAULT_DATASET_DIR.
      check_images: Whether to open and verify image files on disk.
      max_dequant_error: Maximum allowed coordinate discrepancy in pixels.
      verbose: Whether to log verbose validation messages.

    Returns:
      (is_valid: bool, report_dict: Dict[str, Any])
    """
    start_time = time.perf_counter()

    if dataset_dir is None:
        root = Path(DEFAULT_DATASET_DIR)
        if not root.exists():
            # Fallback to local data dir if default does not exist
            local_fallback = Path("data")
            if local_fallback.exists():
                root = local_fallback
    else:
        root = Path(dataset_dir)

    root = root.resolve()

    errors: List[str] = []
    warnings: List[str] = []

    report: Dict[str, Any] = {
        "valid": False,
        "dataset_dir": str(root),
        "florence_count": 0,
        "act_count": 0,
        "images_checked": 0,
        "errors": errors,
        "warnings": warnings,
        "stats": {
            "total_trajectories": 0,
            "total_steps": 0,
            "florence_grounding_samples": 0,
            "max_observed_dequant_error": 0.0,
            "avg_dequant_error": 0.0,
            "duration_sec": 0.0,
            "checks": {
                "json_schema": "PASS",
                "image_integrity": "PASS" if check_images else "SKIPPED",
                "bbox_geometry": "PASS",
                "loc_tokens": "PASS",
                "click_containment": "PASS",
                "dequant_precision": "PASS",
                "dataset_sync": "PASS",
                "stats_sync": "PASS",
                "unhandled_exceptions": "PASS",
            },
        },
    }

    if not root.exists() or not root.is_dir():
        errors.append(f"Dataset root directory does not exist or is not a directory: {root}")
        report["stats"]["checks"]["json_schema"] = "FAIL"
        report["stats"]["checks"]["dataset_sync"] = "FAIL"
        report["stats"]["duration_sec"] = round(time.perf_counter() - start_time, 3)
        return False, report

    # Check disk safety (>= 1.0 GB)
    try:
        usage = shutil.disk_usage(root)
        free_gb = usage.free / (1024**3)
        if free_gb < 1.0:
            warnings.append(f"Disk space below recommended floor (1.0 GB): free={free_gb:.2f} GB")
    except Exception as e:
        warnings.append(f"Could not check disk usage on {root}: {e}")

    florence_file = root / FLORENCE_JSONL_NAME
    act_file = root / AGENT_ACT_JSONL_NAME
    stats_file = root / STATS_JSON_NAME
    images_dir = root / IMAGES_DIR_NAME

    if not florence_file.is_file():
        errors.append(f"Missing Florence-2 dataset file: {florence_file}")
    if not act_file.is_file():
        errors.append(f"Missing Agent Act dataset file: {act_file}")
    if check_images and not images_dir.is_dir():
        errors.append(f"Missing images directory: {images_dir}")

    if errors:
        report["stats"]["checks"]["json_schema"] = "FAIL"
        report["stats"]["checks"]["dataset_sync"] = "FAIL"
        report["stats"]["duration_sec"] = round(time.perf_counter() - start_time, 3)
        return False, report

    # 1. Parse Florence entries
    florence_entries: List[Dict[str, Any]] = []
    florence_parse_errors = 0
    with open(florence_file, "r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, start=1):
            s = line.strip()
            if not s:
                continue
            try:
                rec = json.loads(s)
                if isinstance(rec, dict):
                    rec["__line__"] = line_no
                florence_entries.append(rec)
            except Exception as e:
                errors.append(f"[{FLORENCE_JSONL_NAME}:{line_no}] Invalid JSON: {e}")
                florence_parse_errors += 1

    # 2. Parse Act entries
    act_entries: List[Dict[str, Any]] = []
    act_parse_errors = 0
    with open(act_file, "r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, start=1):
            s = line.strip()
            if not s:
                continue
            try:
                rec = json.loads(s)
                if isinstance(rec, dict):
                    rec["__line__"] = line_no
                act_entries.append(rec)
            except Exception as e:
                errors.append(f"[{AGENT_ACT_JSONL_NAME}:{line_no}] Invalid JSON: {e}")
                act_parse_errors += 1

    report["florence_count"] = len(florence_entries)
    report["act_count"] = len(act_entries)

    if florence_parse_errors > 0 or act_parse_errors > 0:
        report["stats"]["checks"]["json_schema"] = "FAIL"

    # 3. Synchronize datasets
    if len(florence_entries) == 0:
        errors.append(f"{FLORENCE_JSONL_NAME} contains 0 valid samples")
        report["stats"]["checks"]["dataset_sync"] = "FAIL"
    if len(act_entries) == 0:
        errors.append(f"{AGENT_ACT_JSONL_NAME} contains 0 valid samples")
        report["stats"]["checks"]["dataset_sync"] = "FAIL"

    if len(florence_entries) != len(act_entries):
        errors.append(f"Record count mismatch: Florence ({len(florence_entries)}) vs Act ({len(act_entries)})")
        report["stats"]["checks"]["dataset_sync"] = "FAIL"

    # 4. Validate Stats File
    if stats_file.is_file():
        try:
            stats_data = json.loads(stats_file.read_text(encoding="utf-8"))
            report["stats"]["total_trajectories"] = stats_data.get("total_trajectories", 0)
            report["stats"]["total_steps"] = stats_data.get("total_steps", 0)
            report["stats"]["florence_grounding_samples"] = stats_data.get("florence_grounding_samples", 0)

            stat_samples = stats_data.get("florence_grounding_samples")
            if stat_samples != len(florence_entries):
                warnings.append(
                    f"stats.json samples ({stat_samples}) does not match florence_jsonl ({len(florence_entries)})"
                )
                report["stats"]["checks"]["stats_sync"] = "WARN"
        except Exception as e:
            warnings.append(f"Could not parse stats.json: {e}")
            report["stats"]["checks"]["stats_sync"] = "WARN"
    else:
        warnings.append(f"stats.json not found at {stats_file}")
        report["stats"]["checks"]["stats_sync"] = "WARN"

    # 5. Individual record validation & dequantization tracking
    dequant_errors: List[float] = []
    images_checked = 0

    # Build act lookup by index and image
    act_by_image = {
        e.get("image"): e
        for e in act_entries
        if isinstance(e, dict) and isinstance(e.get("image"), str)
    }

    for idx, f_rec in enumerate(florence_entries):
        line_no = f_rec.get("__line__", idx + 1) if isinstance(f_rec, dict) else (idx + 1)
        img_rel = f_rec.get("image") if isinstance(f_rec, dict) else None

        # Correlate with corresponding act record by lockstep line index
        act_rec = act_entries[idx] if idx < len(act_entries) else None
        if act_rec is None and isinstance(img_rel, str) and img_rel in act_by_image:
            act_rec = act_by_image.get(img_rel)

        try:
            ok, sample_errs = validate_sample(
                florence_record=f_rec,
                act_record=act_rec,
                base_dir=root,
                check_image=check_images,
                max_dequant_error=max_dequant_error,
            )
        except Exception as exc:
            ok = False
            sample_errs = [f"Unhandled exception during sample validation: {type(exc).__name__}: {exc}"]
            report["stats"]["checks"]["unhandled_exceptions"] = "FAIL"

        if not ok:
            for err in sample_errs:
                errors.append(f"[{FLORENCE_JSONL_NAME}:{line_no}] {err}")
                # Track check failure categories
                err_lower = err.lower()
                if "json" in err_lower or "field" in err_lower or "missing" in err_lower:
                    report["stats"]["checks"]["json_schema"] = "FAIL"
                if "must be a" in err_lower:
                    report["stats"]["checks"]["json_schema"] = "FAIL"
                if "image" in err_lower:
                    report["stats"]["checks"]["image_integrity"] = "FAIL"
                if "bbox" in err_lower:
                    report["stats"]["checks"]["bbox_geometry"] = "FAIL"
                if "token" in err_lower or "<loc_" in err_lower:
                    report["stats"]["checks"]["loc_tokens"] = "FAIL"
                if "click" in err_lower:
                    report["stats"]["checks"]["click_containment"] = "FAIL"
                if "dequantization" in err_lower:
                    report["stats"]["checks"]["dequant_precision"] = "FAIL"
                if "unhandled" in err_lower or "exception" in err_lower:
                    report["stats"]["checks"]["unhandled_exceptions"] = "FAIL"
                    report["stats"]["checks"]["json_schema"] = "FAIL"

        # Compute metric if valid box and tokens
        if ok and isinstance(f_rec, dict):
            bbox = f_rec.get("bbox_px") or f_rec.get("bbox")
            size = f_rec.get("image_size")
            target = f_rec.get("target")
            if isinstance(target, str):
                tokens = LOC_TOKEN_REGEX.findall(target)
                if len(tokens) == 4 and bbox and size and len(bbox) == 4 and len(size) == 2:
                    try:
                        int_tokens = [int(t) for t in tokens]
                        if int_tokens[0] < int_tokens[2] and int_tokens[1] < int_tokens[3]:
                            deq = dequantize_bbox(int_tokens, size)
                            err_val = compute_max_dequant_error(bbox, deq)
                            if math.isfinite(err_val):
                                dequant_errors.append(err_val)
                    except Exception:
                        pass

        if check_images and isinstance(img_rel, str) and img_rel.strip():
            images_checked += 1

    report["images_checked"] = images_checked

    if dequant_errors:
        report["stats"]["max_observed_dequant_error"] = round(max(dequant_errors), 4)
        report["stats"]["avg_dequant_error"] = round(sum(dequant_errors) / len(dequant_errors), 4)
        if max(dequant_errors) > max_dequant_error:
            report["stats"]["checks"]["dequant_precision"] = "FAIL"

    # 6. Overall Pass/Fail status
    report["valid"] = (len(errors) == 0)
    report["stats"]["duration_sec"] = round(time.perf_counter() - start_time, 3)

    return report["valid"], report


def format_summary_table(report: Dict[str, Any]) -> str:
    """Renders a structured, clean terminal table summarizing all checks."""
    lines: List[str] = []
    lines.append("=" * 76)
    lines.append("CPower Dataset Integrity & Grounding Validator")
    lines.append(f"Target Directory: {report['dataset_dir']}")
    lines.append("=" * 76)
    lines.append(f"{'CHECK ITEM':<36} {'STATUS':<10} {'DETAILS'}")
    lines.append("-" * 76)

    # Check 1: Record counts & sync
    f_cnt = report["florence_count"]
    a_cnt = report["act_count"]
    sync_check = report["stats"]["checks"].get("dataset_sync", "PASS")
    lines.append(f"{'Dataset Record Synchronization':<36} {sync_check:<10} {f_cnt} Florence / {a_cnt} Act lines")

    # Check 2: Schema
    schema_check = report["stats"]["checks"].get("json_schema", "PASS")
    lines.append(f"{'JSONL Schema Conformance':<36} {schema_check:<10} Required keys, UTF-8 syntax")

    # Check 3: Images
    img_cnt = report["images_checked"]
    img_check = report["stats"]["checks"].get("image_integrity", "PASS")
    lines.append(f"{'Image Format & Dimension Check':<36} {img_check:<10} {img_cnt} images inspected")

    # Check 4: Bbox geometry
    bbox_check = report["stats"]["checks"].get("bbox_geometry", "PASS")
    lines.append(f"{'Bounding Box Cartesian Geometry':<36} {bbox_check:<10} 0 <= x1 < x2 <= W, 0 <= y1 < y2 <= H")

    # Check 5: Tokens
    token_check = report["stats"]["checks"].get("loc_tokens", "PASS")
    lines.append(f"{'Florence-2 Token Bounds':<36} {token_check:<10} [0..999], no <loc_1000>, non-degenerate")

    # Check 6: Click containment
    click_check = report["stats"]["checks"].get("click_containment", "PASS")
    lines.append(f"{'Action Click Containment':<36} {click_check:<10} Clicks fall strictly within bboxes")

    # Check 7: Dequantization precision
    max_err = report["stats"].get("max_observed_dequant_error", 0.0)
    deq_check = report["stats"]["checks"].get("dequant_precision", "PASS")
    lines.append(f"{'Florence-2 Dequant Precision':<36} {deq_check:<10} Max error: {max_err:.4f}px (threshold: 1.0px)")

    # Check 8: Stats sync
    stats_check = report["stats"]["checks"].get("stats_sync", "PASS")
    lines.append(f"{'Stats Synchronization':<36} {stats_check:<10} {len(report['warnings'])} warnings")

    lines.append("-" * 76)
    overall = "PASS" if report["valid"] else "FAIL"
    lines.append(f"OVERALL VERDICT: {overall} ({len(report['errors'])} errors, {len(report['warnings'])} warnings) in {report['stats']['duration_sec']}s")
    lines.append("=" * 76)

    if report["errors"]:
        lines.append("\nERRORS DETECTED:")
        for err in report["errors"][:15]:
            lines.append(f"  [ERROR] {err}")
        if len(report["errors"]) > 15:
            lines.append(f"  ... and {len(report['errors']) - 15} more errors.")

    if report["warnings"]:
        lines.append("\nWARNINGS:")
        for w in report["warnings"]:
            lines.append(f"  [WARN]  {w}")

    return "\n".join(lines)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="CPower Multimodal Dataset & Grounding Integrity Validator",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--dir", "-d",
        type=str,
        default=DEFAULT_DATASET_DIR,
        help="Path to dataset root containing dataset_florence2.jsonl",
    )
    parser.add_argument(
        "--no-images",
        action="store_true",
        help="Skip image file decoding for high-speed metadata validation",
    )
    parser.add_argument(
        "--max-dequant-error",
        type=float,
        default=DEFAULT_MAX_DEQUANT_ERROR,
        help="Maximum allowable dequantization coordinate error in pixels",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit output strictly as formatted JSON to stdout",
    )
    parser.add_argument(
        "--quiet", "-q",
        action="store_true",
        help="Suppress console output, rely exclusively on return code",
    )

    args = parser.parse_args(argv)

    is_valid, report = validate_dataset(
        dataset_dir=args.dir,
        check_images=not args.no_images,
        max_dequant_error=args.max_dequant_error,
    )

    if not args.quiet:
        if args.json:
            print(json.dumps(report, indent=2, ensure_ascii=False))
        else:
            print(format_summary_table(report))

    return 0 if is_valid else 1


if __name__ == "__main__":
    sys.exit(main())
