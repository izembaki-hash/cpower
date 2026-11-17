"""
Adversarial Dataset Integrity and Grounding Validation Harness (tests/test_adversarial_dataset_validation.py).
Author: Challenger 2 (Milestone 1)

Empirically validates:
1. UTF-8 JSON validity of dataset_florence2.jsonl and dataset_agent_act.jsonl.
2. Image file existence, format (JPEG/PNG), readability, and dimension integrity.
3. Bounding box geometric properties (0 <= x1 < x2 <= width, 0 <= y1 < y2 <= height).
4. Florence-2 coordinate token bounds (integers in [0..999], no <loc_1000>, non-inverted).
5. Action click containment (click falls inside target bounding box).
6. Roundtrip dequantization error against Florence-2 AutoProcessor.post_process_generation (< 1.0px).
7. Cross-dataset consistency and stats synchronization.
"""

import json
from pathlib import Path
import re
import sys
import unittest

from PIL import Image
from transformers import AutoProcessor


DATASET_DIR = Path(r"E:\cpower_training_data")
FLORENCE_JSONL = DATASET_DIR / "dataset_florence2.jsonl"
AGENT_ACT_JSONL = DATASET_DIR / "dataset_agent_act.jsonl"
STATS_JSON = DATASET_DIR / "stats.json"
MODEL_DIR = Path(r"C:\Users\Administrator\.models\florence-2-base")


class AdversarialDatasetValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.processor = AutoProcessor.from_pretrained(str(MODEL_DIR), trust_remote_code=True)
        cls.florence_entries = []
        cls.act_entries = []

        # Read Florence dataset
        if FLORENCE_JSONL.exists():
            with open(FLORENCE_JSONL, "r", encoding="utf-8") as f:
                for line_idx, line in enumerate(f, start=1):
                    line_str = line.strip()
                    if not line_str:
                        continue
                    try:
                        record = json.loads(line_str)
                        record["__line__"] = line_idx
                        cls.florence_entries.append(record)
                    except Exception as err:
                        cls.florence_entries.append({"__error__": str(err), "__line__": line_idx, "__raw__": line_str})

        # Read Agent Act dataset
        if AGENT_ACT_JSONL.exists():
            with open(AGENT_ACT_JSONL, "r", encoding="utf-8") as f:
                for line_idx, line in enumerate(f, start=1):
                    line_str = line.strip()
                    if not line_str:
                        continue
                    try:
                        record = json.loads(line_str)
                        record["__line__"] = line_idx
                        cls.act_entries.append(record)
                    except Exception as err:
                        cls.act_entries.append({"__error__": str(err), "__line__": line_idx, "__raw__": line_str})

    def test_01_utf8_json_parsing(self):
        """Verify every line parses as valid UTF-8 JSON with no parse errors."""
        florence_errors = [e for e in self.florence_entries if "__error__" in e]
        act_errors = [e for e in self.act_entries if "__error__" in e]

        self.assertEqual(len(florence_errors), 0, f"Florence JSON parse errors: {florence_errors}")
        self.assertEqual(len(act_errors), 0, f"Act JSON parse errors: {act_errors}")
        self.assertGreater(len(self.florence_entries), 0, "Florence dataset is empty")
        self.assertGreater(len(self.act_entries), 0, "Act dataset is empty")
        print(f"\n[PASS] test_01_utf8_json_parsing: {len(self.florence_entries)} Florence, {len(self.act_entries)} Act lines valid UTF-8 JSON.")

    def test_02_images_existence_and_validity(self):
        """Verify all referenced images exist on disk, are valid readable JPEG/PNGs, and match stated dimensions."""
        corrupted_images = []
        missing_images = []
        dim_mismatches = []
        checked = 0

        for entry in self.florence_entries:
            if "__error__" in entry:
                continue
            rel_path = entry.get("image")
            img_path = DATASET_DIR / rel_path
            expected_w, expected_h = entry.get("image_size", (0, 0))

            if not img_path.exists():
                missing_images.append((entry["__line__"], str(rel_path)))
                continue

            try:
                with Image.open(img_path) as im:
                    im.verify()  # Verify JPEG/PNG integrity
                with Image.open(img_path) as im:
                    actual_w, actual_h = im.size
                    fmt = im.format
                    if fmt not in ("JPEG", "PNG"):
                        corrupted_images.append((entry["__line__"], str(rel_path), f"Unexpected format {fmt}"))
                    if actual_w != expected_w or actual_h != expected_h:
                        dim_mismatches.append((entry["__line__"], str(rel_path), (actual_w, actual_h), (expected_w, expected_h)))
            except Exception as e:
                corrupted_images.append((entry["__line__"], str(rel_path), str(e)))

            checked += 1

        self.assertEqual(len(missing_images), 0, f"Missing images: {missing_images[:5]}")
        self.assertEqual(len(corrupted_images), 0, f"Corrupted images: {corrupted_images[:5]}")
        self.assertEqual(len(dim_mismatches), 0, f"Dimension mismatches: {dim_mismatches[:5]}")
        print(f"[PASS] test_02_images_existence_and_validity: {checked} images verified (100% valid JPEG/PNG, matching dimensions).")

    def test_03_bounding_box_geometry(self):
        """Verify every bbox satisfies 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height."""
        geom_violations = []
        checked = 0

        for entry in self.florence_entries:
            if "__error__" in entry:
                continue
            bbox = entry.get("bbox_px")
            w, h = entry.get("image_size", (0, 0))

            if not isinstance(bbox, (list, tuple)) or len(bbox) != 4:
                geom_violations.append((entry["__line__"], bbox, "Invalid format or length"))
                continue

            x1, y1, x2, y2 = bbox

            # Check bounds: 0 <= x1 < x2 <= w, 0 <= y1 < y2 <= h
            if not (0 <= x1 < x2 <= w):
                geom_violations.append((entry["__line__"], bbox, (w, h), f"Failed 0 <= x1 < x2 <= {w}"))
            if not (0 <= y1 < y2 <= h):
                geom_violations.append((entry["__line__"], bbox, (w, h), f"Failed 0 <= y1 < y2 <= {h}"))

            checked += 1

        self.assertEqual(len(geom_violations), 0, f"Bounding box geometry violations: {geom_violations[:5]}")
        print(f"[PASS] test_03_bounding_box_geometry: {checked} bboxes satisfy 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height.")

    def test_04_florence_loc_tokens_integer_range(self):
        """Verify all <loc_...> tokens are integers in [0..999], exactly 4 tokens, and no <loc_1000>."""
        loc_violations = []
        loc_pattern = re.compile(r"<loc_(\d+)>")
        checked = 0

        for entry in self.florence_entries:
            if "__error__" in entry:
                continue
            target = entry.get("target", "")
            matches = loc_pattern.findall(target)

            if len(matches) != 4:
                loc_violations.append((entry["__line__"], target, f"Expected 4 tokens, found {len(matches)}"))
                continue

            int_tokens = [int(m) for m in matches]
            for tok in int_tokens:
                if tok < 0 or tok > 999:
                    loc_violations.append((entry["__line__"], target, f"Token {tok} outside [0..999]"))

            nx1, ny1, nx2, ny2 = int_tokens
            if "<loc_1000>" in target:
                loc_violations.append((entry["__line__"], target, "Contains forbidden <loc_1000>"))

            # Check for degenerate/collapsed tokens: nx1 >= nx2 or ny1 >= ny2
            if nx1 >= nx2 or ny1 >= ny2:
                loc_violations.append((entry["__line__"], target, f"Degenerate normalized box: [{nx1}, {ny1}, {nx2}, {ny2}]"))

            checked += 1

        self.assertEqual(len(loc_violations), 0, f"Token violations found: {loc_violations[:5]}")
        print(f"[PASS] test_04_florence_loc_tokens_integer_range: {checked} targets have exactly 4 integer tokens in [0..999] with nx1 < nx2 and ny1 < ny2.")

    def test_05_action_clicks_containment(self):
        """Verify action clicks in dataset_agent_act.jsonl fall strictly inside target bounding box."""
        # Index florence entries by image path
        florence_map = {e["image"]: e for e in self.florence_entries if "__error__" not in e}

        click_violations = []
        missing_clicks = []
        checked = 0

        for entry in self.act_entries:
            if "__error__" in entry:
                continue
            img = entry.get("image")
            action = entry.get("action", {})
            act_type = action.get("type", "")

            # Look up corresponding ground truth bbox
            f_entry = florence_map.get(img)
            if not f_entry:
                continue

            x1, y1, x2, y2 = f_entry["bbox_px"]
            click = action.get("click")

            if act_type in ("click", "double_click"):
                if click is None:
                    missing_clicks.append((entry["__line__"], entry.get("trajectory_id"), action))
                    continue

                cx, cy = click
                # Strict containment: x1 <= cx <= x2 and y1 <= cy <= y2
                if not (x1 <= cx <= x2 and y1 <= cy <= y2):
                    click_violations.append((entry["__line__"], click, [x1, y1, x2, y2], f"Click [{cx}, {cy}] outside bbox"))

                checked += 1

        self.assertEqual(len(missing_clicks), 0, f"Actions with click=None: {missing_clicks}")
        self.assertEqual(len(click_violations), 0, f"Action clicks outside bbox: {click_violations[:5]}")
        print(f"[PASS] test_05_action_clicks_containment: {checked} clicks fall strictly inside target bounding boxes.")

    def test_06_florence2_roundtrip_dequantization_precision(self):
        """
        Verify roundtrip dequantization with Florence-2 AutoProcessor.post_process_generation
        achieves < 1.0px discrepancy against original pixel boxes.
        """
        discrepancies = []
        checked = 0

        for entry in self.florence_entries:
            if "__error__" in entry:
                continue
            w, h = entry.get("image_size", (0, 0))
            x1, y1, x2, y2 = entry.get("bbox_px", (0, 0, 0, 0))
            target = entry.get("target", "")

            res = self.processor.post_process_generation(
                target,
                task="<CAPTION_TO_PHRASE_GROUNDING>",
                image_size=(w, h),
            )
            parsed_boxes = res.get("<CAPTION_TO_PHRASE_GROUNDING>", {}).get("bboxes", [])
            if not parsed_boxes:
                discrepancies.append((entry["__line__"], "No parsed bboxes", target, (w, h)))
                continue

            bx1, by1, bx2, by2 = parsed_boxes[0]
            err_x1 = abs(bx1 - x1)
            err_y1 = abs(by1 - y1)
            err_x2 = abs(bx2 - x2)
            err_y2 = abs(by2 - y2)
            max_err = max(err_x1, err_y1, err_x2, err_y2)

            if max_err >= 1.0:
                discrepancies.append((
                    entry["__line__"],
                    f"Max error {max_err:.4f}px >= 1.0px",
                    f"Orig: {[x1, y1, x2, y2]}",
                    f"Dequant: {[round(bx1, 2), round(by1, 2), round(bx2, 2), round(by2, 2)]}",
                    f"Size: ({w}, {h})",
                    target,
                ))

            checked += 1

        self.assertEqual(
            len(discrepancies),
            0,
            f"Dequantization errors >= 1.0px found on {len(discrepancies)}/{checked} samples:\n"
            + "\n".join(str(d) for d in discrepancies[:10]),
        )
        print(f"[PASS] test_06_florence2_roundtrip_dequantization_precision: All {checked} samples achieve < 1.0px discrepancy.")

    def test_07_cross_dataset_count_and_synchronization(self):
        """Verify Florence and Act dataset records are 1-to-1 synchronized and match stats.json."""
        f_count = len(self.florence_entries)
        a_count = len(self.act_entries)

        self.assertEqual(f_count, a_count, f"Dataset count mismatch: Florence={f_count} vs Act={a_count}")

        if STATS_JSON.exists():
            with open(STATS_JSON, "r", encoding="utf-8") as f:
                stats = json.load(f)
            stat_samples = stats.get("florence_grounding_samples", -1)
            self.assertEqual(
                stat_samples,
                f_count,
                f"stats.json florence_grounding_samples ({stat_samples}) does not match florence_jsonl ({f_count})",
            )
        print(f"[PASS] test_07_cross_dataset_count_and_synchronization: Florence and Act both contain {f_count} records.")


if __name__ == "__main__":
    unittest.main(verbosity=2)
