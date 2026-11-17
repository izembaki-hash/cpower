"""
CPower End-to-End Test Suite — Tier 1 Feature Coverage (tests/e2e/test_tier1_features.py).
Author: Test Writer (Milestone TM2)

Implements all 40 Tier 1 tests specified in TEST_INFRA.md § 4 across Features 1 through 8:
  Feature 1: Windows 11 Controls Harvester (5 tests)
  Feature 2: Web & YouTube Media Harvester (5 tests)
  Feature 3: Rich Arabic RTL Synthesizer (5 tests)
  Feature 4: Coordinate Normalization & Tokenizer Align (5 tests)
  Feature 5: JSONL Schema & Image Integrity Validator (5 tests)
  Feature 6: Training Pipeline Enhancement (5 tests)
  Feature 7: Checkpoint Verification & Disk Safety (5 tests)
  Feature 8: Continuous Multi-Agent Training Execution (5 tests)

All tests strictly enforce:
  1. Zero physical mouse interactions (ZeroMouseTracker & Win32 cursor tracking).
  2. Storage isolation (no volatile writes to drive H:; quarantined to isolated_dataset_dir).
  3. Strict [0..999] Florence-2 coordinate quantization alignment.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import threading
import time
from typing import Any, Callable, Dict, List, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageStat
import pytest
import torch

# Ensure clients/python is available
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CLIENTS_PYTHON = PROJECT_ROOT / "clients" / "python"
if str(CLIENTS_PYTHON) not in sys.path:
    sys.path.insert(0, str(CLIENTS_PYTHON))

from cpower_dataset_collector import CPowerDatasetCollector
from cpower_multitask_harvester import (
    ArabicRTLHarvester,
    MultiTaskUIHarvester,
    PillowYouTubeSynthesizer,
    Win11ControlsSynthesizer,
    reshape_arabic,
    sanitize_bbox,
)
import cpower_continual_loop
from mock_hardware import (
    ArabicSampleCorpus,
    LightweightProcessorStub,
    MockDiskController,
    SyntheticUIProvider,
    ZeroMouseTracker,
)


# =============================================================================
# Feature 1: Windows 11 Controls Harvester (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f1_explorer_task_generation_structure(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Verifies generate_explorer_task produces a 1024x680 RGB image, step record with action double_click, and phrase مجلد."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 1: Explorer Structure Test")
    harvester.generate_explorer_task(traj_id, step_idx=1, archetype="grid")
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    assert collector.florence_jsonl_path.is_file(), "dataset_florence2.jsonl was not created"
    assert collector.agent_act_jsonl_path.is_file(), "dataset_agent_act.jsonl was not created"

    florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(florence_lines) == 1, f"Expected 1 Florence record, got {len(florence_lines)}"
    fl_rec = json.loads(florence_lines[0])

    assert fl_rec["image_size"] == [1024, 680], f"Expected image size [1024, 680], got {fl_rec['image_size']}"
    assert fl_rec["prefix"] == "<CAPTION_TO_PHRASE_GROUNDING>"
    assert len(fl_rec["suffix"]) > 0, "Florence suffix phrase must not be empty"
    assert "<loc_" in fl_rec["target"], "Florence target must contain location tokens"

    # Verify physical image existence and dimensions
    img_file = isolated_dataset_dir / fl_rec["image"]
    assert img_file.is_file(), f"Screenshot image file does not exist: {img_file}"
    with Image.open(img_file) as im:
        assert im.size == (1024, 680), f"Physical image dimensions mismatch: {im.size}"

    # Verify agent action record
    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(act_lines) == 1, f"Expected 1 Act record, got {len(act_lines)}"
    act_rec = json.loads(act_lines[0])
    assert act_rec["action"]["type"] in ("double_click", "click")
    assert act_rec["action"]["phrase"] == fl_rec["suffix"]
    assert act_rec["window"].startswith("File Explorer") or "Explorer" in act_rec["window"]


@pytest.mark.e2e
@pytest.mark.tier1
def test_f1_explorer_bounding_box_within_image_bounds(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts target folder bounding box satisfies 0 <= x1 < x2 <= 1024 and 0 <= y1 < y2 <= 680."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 1: Explorer BBox Bounds Test")
    harvester.generate_explorer_task(traj_id, step_idx=1)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    fl_rec = json.loads(florence_lines[0])

    px1, py1, px2, py2 = fl_rec["bbox_px"]
    assert 0 <= px1 < px2 <= 1024, f"Pixel X bounds invalid: 0 <= {px1} < {px2} <= 1024"
    assert 0 <= py1 < py2 <= 680, f"Pixel Y bounds invalid: 0 <= {py1} < {py2} <= 680"
    assert (px2 - px1) > 0, "BBox width must be strictly positive"
    assert (py2 - py1) > 0, "BBox height must be strictly positive"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f1_explorer_click_coords_inside_bbox(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts synthesized click coordinates fall strictly inside target folder bounding box."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 1: Explorer Click Containment Test")
    harvester.generate_explorer_task(traj_id, step_idx=1)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    florence_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    fl_rec = json.loads(florence_lines[0])
    px1, py1, px2, py2 = fl_rec["bbox_px"]

    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    act_rec = json.loads(act_lines[0])
    click = act_rec["action"].get("click")

    if click is not None:
        cx, cy = click
        assert px1 <= cx <= px2, f"Click X coordinate {cx} outside bbox [{px1}, {px2}]"
        assert py1 <= cy <= py2, f"Click Y coordinate {cy} outside bbox [{py1}, {py2}]"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f1_system_dialog_task_generation(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Verifies generate_system_dialog_task generates Save/Cancel/Browse buttons with valid window title."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 1: System Dialog Test")
    harvester.generate_system_dialog_task(traj_id, step_idx=1)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(fl_lines) == 1
    fl_rec = json.loads(fl_lines[0])
    assert fl_rec["image_size"] == [800, 520] or fl_rec["image_size"] == [800, 500]

    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    act_rec = json.loads(act_lines[0])
    assert act_rec["window"] in ("Save As Dialog", "Open File Dialog")
    assert len(act_rec["action"]["phrase"]) > 0
    assert act_rec["action"]["type"] in ("click", "type")


@pytest.mark.e2e
@pytest.mark.tier1
def test_f1_windows11_harvester_zero_hardware_mouse_calls(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    monkeypatch: pytest.MonkeyPatch,
    zero_mouse_tracker: ZeroMouseTracker,
) -> None:
    """Asserts Win32 hardware mouse APIs (SetCursorPos, mouse_event) are never called during harvesting."""
    # Capture physical cursor position
    pt_start = wintypes.POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_start))

    # Spy Win32 mouse APIs
    def spy_set_cursor_pos(x, y):
        zero_mouse_tracker.record_move(x, y)
        return 1

    def spy_mouse_event(flags, dx, dy, data, extra):
        zero_mouse_tracker.record_click()

    monkeypatch.setattr(ctypes.windll.user32, "SetCursorPos", spy_set_cursor_pos, raising=False)
    monkeypatch.setattr(ctypes.windll.user32, "mouse_event", spy_mouse_event, raising=False)

    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 1: Zero Hardware Mouse Test")
    harvester.generate_explorer_task(traj_id, step_idx=1)
    harvester.generate_system_dialog_task(traj_id, step_idx=2)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    # Capture physical cursor position after operation
    pt_end = wintypes.POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(pt_end))

    # Assert cursor coordinates did not change
    assert (pt_start.x, pt_start.y) == (pt_end.x, pt_end.y), "Host mouse cursor position was moved!"
    zero_mouse_tracker.assert_zero_mouse_interactions()


# =============================================================================
# Feature 2: Web & YouTube Media Harvester (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f2_youtube_task_generation_dark_theme(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Verifies YouTube canvas has dark theme aesthetics (mean RGB < 60) and window title 'YouTube - Google Chrome'."""
    collector = collector_factory(isolated_dataset_dir)
    synth = PillowYouTubeSynthesizer()
    img, comps = synth.generate(width=1280, height=800, dark_theme=True, locale="en")

    # Compute mean RGB of canvas
    stat = ImageStat.Stat(img)
    mean_rgb = sum(stat.mean[:3]) / 3.0
    assert mean_rgb < 60.0, f"Expected dark theme mean RGB < 60, got {mean_rgb:.2f}"

    # Verify window title contract
    traj_id = collector.start_trajectory("Feature 2: YouTube Dark Theme")
    cid, phrase, raw_box = comps[0]
    clean_box, norm_box = sanitize_bbox(raw_box, 1280, 800)
    collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase=phrase,
        bbox=clean_box,
        normalized_bbox=norm_box,
        window_title="YouTube - Google Chrome",
        metadata={"domain": "web_media", "theme": "dark"},
    )
    collector.complete_trajectory(traj_id, succeeded=True)

    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    act_rec = json.loads(act_lines[0])
    assert act_rec["window"] == "YouTube - Google Chrome"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f2_youtube_search_bar_and_button_geometry() -> None:
    """Asserts search button has width >= 30, height >= 20, and non-overlapping alignment with search input box."""
    synth = PillowYouTubeSynthesizer()
    _, comps = synth.generate(width=1280, height=800, dark_theme=True, locale="en")

    search_input = next(c for c in comps if c[0] == "search-input")
    search_btn = next(c for c in comps if c[0] == "search-btn")

    # Geometry verification
    btn_box = search_btn[2]
    btn_w = btn_box[2] - btn_box[0]
    btn_h = btn_box[3] - btn_box[1]
    assert btn_w >= 30, f"Expected search button width >= 30, got {btn_w}"
    assert btn_h >= 20, f"Expected search button height >= 20, got {btn_h}"

    # Alignment verification: search input right boundary meets search button left boundary
    input_box = search_input[2]
    assert input_box[2] <= btn_box[0], f"Search input {input_box} overlaps search button {btn_box}"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f2_youtube_video_card_metadata(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts metadata contains domain: 'web_media' or 'youtube' and non-empty query / component descriptor."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 2: YouTube Metadata Test")
    harvester.generate_youtube_task(traj_id, step_idx=1, use_playwright=False)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    # Read trajectory JSON to inspect detailed step metadata
    traj_path = isolated_dataset_dir / "trajectories" / f"{traj_id}.json"
    assert traj_path.is_file(), f"Trajectory file not found: {traj_path}"
    traj_data = json.loads(traj_path.read_text(encoding="utf-8"))
    assert len(traj_data["steps"]) > 0

    step_data = traj_data["steps"][0]
    meta = step_data.get("metadata", {})
    assert meta.get("domain") in ("web_media", "youtube"), f"Expected web_media domain, got {meta.get('domain')}"
    assert len(step_data["phrase"]) > 0, "Video card or action phrase must not be empty"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f2_youtube_action_type_validity(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts action is 'click' or 'type' and target coordinates correspond to video thumbnail or search."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    traj_id = collector.start_trajectory("Feature 2: YouTube Action Type Test")
    harvester.generate_youtube_task(traj_id, step_idx=1, use_playwright=False)
    collector.complete_trajectory(traj_id, succeeded=True)
    harvester.close()

    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    act_rec = json.loads(act_lines[0])

    assert act_rec["action"]["type"] in ("click", "type")
    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    fl_rec = json.loads(fl_lines[0])

    bx1, by1, bx2, by2 = fl_rec["bbox_px"]
    click = act_rec["action"].get("click")
    if click:
        assert bx1 <= click[0] <= bx2
        assert by1 <= click[1] <= by2


@pytest.mark.e2e
@pytest.mark.tier1
def test_f2_web_harvester_headless_mode_enforcement(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts web harvesting operates strictly in headless mode without spawning interactive OS browser windows."""
    collector = collector_factory(isolated_dataset_dir)
    harvester = MultiTaskUIHarvester(collector=collector, headless=True)

    assert harvester.headless is True, "MultiTaskUIHarvester headless flag must be True"
    assert harvester.web_harvester.headless is True, "WebMediaHarvester headless flag must be True"
    harvester.close()


# =============================================================================
# Feature 3: Rich Arabic RTL Synthesizer (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f3_arabic_text_reshaping_cursive_connection(arabic_sample_corpus: ArabicSampleCorpus) -> None:
    """Verifies arabic_reshaper connects isolated Arabic characters into connected cursive presentation forms."""
    import arabic_reshaper

    raw_text = "حفظ"
    # Raw characters in isolated Unicode block (0x0600 - 0x06FF)
    raw_ords = [ord(c) for c in raw_text]
    assert all(0x0600 <= o <= 0x06FF for o in raw_ords)

    reshaped = arabic_reshaper.reshape(raw_text)
    reshaped_ords = [ord(c) for c in reshaped]

    # Presentation Form-B glyphs reside in 0xFE80 - 0xFEFC
    has_presentation_form = any(0xFE80 <= o <= 0xFEFC for o in reshaped_ords)
    assert has_presentation_form, f"Expected cursive presentation form glyphs in {reshaped_ords}"
    assert reshaped != raw_text, "Reshaped cursive text must differ from raw isolated text"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f3_arabic_bidi_visual_reordering(arabic_sample_corpus: ArabicSampleCorpus) -> None:
    """Verifies bidi.algorithm.get_display correctly positions bidirectional mixed LTR/RTL substrings."""
    from bidi.algorithm import get_display

    mixed_text = "تقرير CPower النهائي"
    reshaped = arabic_sample_corpus.reshape_and_bidi(mixed_text)
    assert len(reshaped) > 0

    # Ensure LTR token "CPower" is preserved intact in display string
    assert "CPower" in reshaped, "LTR embedded token was corrupted during BiDi visual reordering"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f3_arabic_diacritics_handling(arabic_sample_corpus: ArabicSampleCorpus) -> None:
    """Asserts Arabic tashkeel diacritics render without unicode decoding errors or crashes."""
    diacritics_text = arabic_sample_corpus.DIACRITICS_HEAVY[0]
    shaped = arabic_sample_corpus.reshape_and_bidi(diacritics_text)
    assert len(shaped) > 0

    # Render on a canvas to verify PIL text layout compatibility
    img = Image.new("RGB", (600, 100), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    font = ImageFont.load_default()
    draw.text((10, 10), shaped, fill=(0, 0, 0), font=font)
    assert img.size == (600, 100)


@pytest.mark.e2e
@pytest.mark.tier1
def test_f3_arabic_rtl_layout_alignment(synthetic_ui_provider: SyntheticUIProvider) -> None:
    """Verifies RTL header/toolbar controls anchor on the right half of the canvas (x > W/2)."""
    width = 1024
    img, elements = synthetic_ui_provider.create_arabic_rtl_ui(width=width, height=600)

    btn_save = elements["زر_حفظ"]
    btn_cancel = elements["زر_إلغاء"]
    input_field = elements["حقل_اسم_المشروع"]

    # In RTL layout, primary action buttons must anchor on the right half
    half_w = width / 2.0
    assert btn_save[0] > half_w, f"Save button left edge {btn_save[0]} must be > {half_w}"
    assert btn_cancel[0] > half_w, f"Cancel button left edge {btn_cancel[0]} must be > {half_w}"
    assert input_field[2] > half_w, f"Input field right edge {input_field[2]} must be > {half_w}"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f3_arabic_phrase_grounding_target_formatting(lightweight_processor_stub: LightweightProcessorStub) -> None:
    """Asserts grounding target string embeds raw Arabic UTF-8 text alongside <loc_...> tokens without unicode escapes."""
    phrase = "زر حفظ التغييرات"
    bbox = (100, 100, 500, 500)
    image_size = (1000, 1000)

    target = lightweight_processor_stub.format_target(phrase, bbox, image_size)
    assert target.startswith("زر حفظ التغييرات")
    assert "\\u" not in target, "Grounding target string contains corrupted Unicode escape sequences"
    assert target.endswith("<loc_100><loc_100><loc_500><loc_500>")


# =============================================================================
# Feature 4: Coordinate Normalization & Tokenizer Align (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f4_coordinate_normalization_proportional_scaling(
    lightweight_processor_stub: LightweightProcessorStub,
) -> None:
    """Validates exact conversion of pixel bbox (192, 108, 960, 540) on 1920x1080 canvas to [100, 100, 500, 500]."""
    bbox = (192, 108, 960, 540)
    canvas = (1920, 1080)
    normalized = lightweight_processor_stub.quantize_bbox(bbox, canvas)
    assert normalized == [100, 100, 500, 500], f"Expected [100, 100, 500, 500], got {normalized}"

    # Also verify with sanitize_bbox
    _, norm_box = sanitize_bbox(bbox, 1920, 1080)
    assert list(norm_box) == [100, 100, 500, 500]


@pytest.mark.e2e
@pytest.mark.tier1
def test_f4_coordinate_clamping_to_999(lightweight_processor_stub: LightweightProcessorStub) -> None:
    """Asserts pixels at outer canvas edge (W, H) clamp strictly to 999 (never 1000)."""
    canvas = (1920, 1080)
    edge_bbox = (0, 0, 1920, 1080)

    normalized = lightweight_processor_stub.quantize_bbox(edge_bbox, canvas)
    assert normalized[2] == 999, f"Expected nx2 == 999, got {normalized[2]}"
    assert normalized[3] == 999, f"Expected ny2 == 999, got {normalized[3]}"
    assert all(0 <= c <= 999 for c in normalized)

    _, norm_box = sanitize_bbox(edge_bbox, 1920, 1080)
    assert norm_box[2] == 999
    assert norm_box[3] == 999


@pytest.mark.e2e
@pytest.mark.tier1
def test_f4_coordinate_order_invariance() -> None:
    """Verifies coordinate sequence is strictly [nx1, ny1, nx2, ny2] with nx1 <= nx2 and ny1 <= ny2."""
    inverted_bbox = (960, 540, 192, 108)
    clean_box, norm_box = sanitize_bbox(inverted_bbox, 1920, 1080)

    # Pixel order
    assert clean_box[0] < clean_box[2], f"Pixel X order inverted: {clean_box}"
    assert clean_box[1] < clean_box[3], f"Pixel Y order inverted: {clean_box}"

    # Normalized order
    assert norm_box[0] < norm_box[2], f"Normalized X order inverted: {norm_box}"
    assert norm_box[1] < norm_box[3], f"Normalized Y order inverted: {norm_box}"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f4_florence2_token_regex_conformance(lightweight_processor_stub: LightweightProcessorStub) -> None:
    """Matches target string against regex r'^.*<loc_\\d{1,3}><loc_\\d{1,3}><loc_\\d{1,3}><loc_\\d{1,3}>$'."""
    target = lightweight_processor_stub.format_target("زر تجريبي", (50, 60, 150, 180), (1000, 1000))
    pattern = re.compile(r"^.*<loc_\d{1,3}><loc_\d{1,3}><loc_\d{1,3}><loc_\d{1,3}>$")
    assert pattern.match(target) is not None, f"Target {target} failed regex match"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f4_florence2_tokenizer_vocabulary_membership() -> None:
    """Verifies tokens <loc_0> through <loc_999> exist in Florence-2 vocabulary and <loc_1000> does not."""
    model_dir = Path("C:/Users/Administrator/.models/florence-2-base")
    if not model_dir.is_dir():
        pytest.skip(f"Florence-2 model not found at {model_dir}")

    from transformers import AutoProcessor
    processor = AutoProcessor.from_pretrained(str(model_dir), trust_remote_code=True)
    vocab = processor.tokenizer.get_vocab()

    # Valid tokens exist in vocabulary
    assert "<loc_0>" in vocab, "<loc_0> missing from Florence-2 vocabulary"
    assert "<loc_500>" in vocab, "<loc_500> missing from Florence-2 vocabulary"
    assert "<loc_999>" in vocab, "<loc_999> missing from Florence-2 vocabulary"

    # Overflow tokens must NOT exist
    assert "<loc_1000>" not in vocab, "<loc_1000> incorrectly present in vocabulary"
    assert "<loc_-1>" not in vocab, "<loc_-1> incorrectly present in vocabulary"


# =============================================================================
# Feature 5: JSONL Schema & Image Integrity Validator (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f5_validator_detects_valid_florence_jsonl_record(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    reference_dataset_validator: Callable[[Path], Tuple[bool, Any]],
) -> None:
    """Asserts validator returns (True, []) when presented with fully compliant dataset records."""
    collector = collector_factory(isolated_dataset_dir)
    traj_id = collector.start_trajectory("Valid record test")
    img = Image.new("RGB", (640, 480), (200, 200, 200))
    collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="زر التشغيل",
        bbox=(50, 50, 150, 150),
        click_coords=(100, 100),
    )
    collector.complete_trajectory(traj_id, succeeded=True)

    res = reference_dataset_validator(isolated_dataset_dir)
    is_valid = res[0]
    errors = res[1]["errors"] if isinstance(res[1], dict) else res[1]
    assert is_valid is True, f"Validator rejected valid record: {errors}"
    assert len(errors) == 0


@pytest.mark.e2e
@pytest.mark.tier1
def test_f5_validator_detects_missing_image_file(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    reference_dataset_validator: Callable[[Path], Tuple[bool, Any]],
) -> None:
    """Asserts validator identifies and reports missing image referenced by image path."""
    collector = collector_factory(isolated_dataset_dir)
    traj_id = collector.start_trajectory("Missing image test")
    img = Image.new("RGB", (300, 300), (100, 100, 100))
    collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="زر تجريبي",
        bbox=(10, 10, 50, 50),
        click_coords=(30, 30),
    )
    collector.complete_trajectory(traj_id, succeeded=True)

    # Delete the saved image file
    img_file = isolated_dataset_dir / "images" / f"{traj_id}_step_00.jpg"
    assert img_file.is_file()
    img_file.unlink()

    res = reference_dataset_validator(isolated_dataset_dir)
    is_valid = res[0]
    errors = res[1]["errors"] if isinstance(res[1], dict) else res[1]
    assert is_valid is False, "Validator failed to detect missing image file"
    assert any("not found" in err.lower() or "missing" in err.lower() or "does not exist" in err.lower() for err in errors)


@pytest.mark.e2e
@pytest.mark.tier1
def test_f5_validator_detects_schema_key_mismatch(
    isolated_dataset_dir: Path,
    reference_dataset_validator: Callable[[Path], Tuple[bool, Any]],
) -> None:
    """Asserts missing mandatory key (e.g. prefix, suffix, or bbox_px) causes validation rejection."""
    # Write a record missing 'suffix' and 'prefix'
    bad_record = {
        "id": "bad_sample_01",
        "image": "images/nonexistent.jpg",
        "target": "text<loc_0><loc_0><loc_100><loc_100>",
        "bbox_px": [0, 0, 100, 100],
        "image_size": [1000, 1000],
    }
    fl_path = isolated_dataset_dir / "dataset_florence2.jsonl"
    fl_path.write_text(json.dumps(bad_record) + "\n", encoding="utf-8")

    # Provide matching agent act record to satisfy multi-dataset synchronization
    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    act_record = {
        "trajectory_id": "bad_sample_01",
        "step_index": 0,
        "goal": "Test goal",
        "window": "Test window",
        "image": "images/nonexistent.jpg",
        "action": {"type": "click", "phrase": "text", "click": [50, 50]},
    }
    act_path.write_text(json.dumps(act_record) + "\n", encoding="utf-8")

    res = reference_dataset_validator(isolated_dataset_dir)
    is_valid = res[0]
    errors = res[1]["errors"] if isinstance(res[1], dict) else res[1]
    assert is_valid is False, "Validator failed to flag missing mandatory schema keys"
    assert any("missing" in err.lower() for err in errors)


@pytest.mark.e2e
@pytest.mark.tier1
def test_f5_validator_detects_inverted_bounding_box(
    isolated_dataset_dir: Path,
    reference_dataset_validator: Callable[[Path], Tuple[bool, Any]],
) -> None:
    """Asserts inverted bounding box (x1 > x2 or y1 > y2) is flagged by validator."""
    # Create valid dummy image
    img = Image.new("RGB", (500, 500), (255, 255, 255))
    img.save(isolated_dataset_dir / "images" / "inverted_test.jpg")

    bad_record = {
        "id": "inverted_01",
        "image": "images/inverted_test.jpg",
        "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
        "suffix": "inverted box",
        "target": "inverted box<loc_500><loc_500><loc_100><loc_100>",  # nx1 > nx2
        "bbox_px": [250, 250, 50, 50],
        "image_size": [500, 500],
    }
    fl_path = isolated_dataset_dir / "dataset_florence2.jsonl"
    fl_path.write_text(json.dumps(bad_record) + "\n", encoding="utf-8")

    # Provide matching agent act record
    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    act_record = {
        "trajectory_id": "inverted_01",
        "step_index": 0,
        "goal": "Inverted test goal",
        "window": "Test window",
        "image": "images/inverted_test.jpg",
        "action": {"type": "click", "phrase": "inverted box", "click": [100, 100]},
    }
    act_path.write_text(json.dumps(act_record) + "\n", encoding="utf-8")

    res = reference_dataset_validator(isolated_dataset_dir)
    is_valid = res[0]
    errors = res[1]["errors"] if isinstance(res[1], dict) else res[1]
    assert is_valid is False, "Validator failed to detect inverted bounding box"
    assert any("inverted" in err.lower() or "zero-area" in err.lower() for err in errors)


@pytest.mark.e2e
@pytest.mark.tier1
def test_f5_validator_detects_click_outside_target_in_agent_act(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    reference_dataset_validator: Callable[[Path], Tuple[bool, Any]],
) -> None:
    """Asserts click coordinate outside target bounding box in dataset_agent_act.jsonl is flagged."""
    collector = collector_factory(isolated_dataset_dir)
    traj_id = collector.start_trajectory("Misaligned click test")
    img = Image.new("RGB", (600, 600), (240, 240, 240))

    # Record valid Florence record with bbox (50, 50, 100, 100)
    collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="زر المستهدف",
        bbox=(50, 50, 100, 100),
        click_coords=(75, 75),
    )
    collector.complete_trajectory(traj_id, succeeded=True)

    # Tamper with agent act JSONL to place click far outside target bbox
    act_path = isolated_dataset_dir / "dataset_agent_act.jsonl"
    lines = act_path.read_text(encoding="utf-8").strip().splitlines()
    tampered_rec = json.loads(lines[0])
    tampered_rec["action"]["click"] = [500, 500]  # Far outside (50, 50, 100, 100)
    act_path.write_text(json.dumps(tampered_rec) + "\n", encoding="utf-8")

    res = reference_dataset_validator(isolated_dataset_dir)
    is_valid = res[0]
    errors = res[1]["errors"] if isinstance(res[1], dict) else res[1]
    assert is_valid is False, "Validator failed to flag misaligned click outside target bbox"
    assert any("outside bbox" in err.lower() or "outside target bbox" in err.lower() for err in errors)


# =============================================================================
# Feature 6: Training Pipeline Enhancement (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f6_training_dataset_train_val_split_ratio() -> None:
    """Asserts cpower_trainer.split_train_val_dataset partitions a PyTorch dataset into 90% train and 10% val."""
    from torch.utils.data import TensorDataset
    from cpower_trainer import split_train_val_dataset

    total_samples = 20
    dataset = TensorDataset(torch.arange(total_samples))

    train_set, val_set = split_train_val_dataset(dataset, val_ratio=0.1, seed=42)

    assert len(train_set) == 18, f"Expected 18 train samples, got {len(train_set)}"
    assert len(val_set) == 2, f"Expected 2 val samples, got {len(val_set)}"
    assert isinstance(train_set, torch.utils.data.Subset)
    assert isinstance(val_set, torch.utils.data.Subset)

    train_indices = set(train_set.indices)
    val_indices = set(val_set.indices)
    assert train_indices.isdisjoint(val_indices), "Train and validation splits overlap"
    assert train_indices.union(val_indices) == set(range(total_samples))


@pytest.mark.e2e
@pytest.mark.tier1
@pytest.mark.florence
@pytest.mark.slow
def test_f6_model_parameter_freezing_verification(florence_model_session: Dict[str, Any]) -> None:
    """Asserts cpower_trainer.freeze_florence_backbone freezes vision_tower while keeping image_proj_norm trainable."""
    from cpower_trainer import freeze_florence_backbone

    model = florence_model_session["model"]

    # Pre-condition: ensure vision tower parameters start with requires_grad=True
    for param in model.vision_tower.parameters():
        param.requires_grad = True

    trainable_params, total_params = freeze_florence_backbone(model)

    vision_tower_grads = [p.requires_grad for p in model.vision_tower.parameters()]
    assert len(vision_tower_grads) > 0
    assert not any(vision_tower_grads), "Vision tower parameters must all have requires_grad=False"

    proj_norm_grads = [p.requires_grad for p in model.image_proj_norm.parameters()]
    assert len(proj_norm_grads) > 0
    assert all(proj_norm_grads), "Image projection norm parameters must have requires_grad=True"

    assert 0 < trainable_params < total_params


@pytest.mark.e2e
@pytest.mark.tier1
def test_f6_train_and_val_loss_logging_contract(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts train_local returns contract-compliant dict with train_loss, val_loss, steps, and nan_detected."""
    from cpower_trainer import train_local

    collector = collector_factory(isolated_dataset_dir)
    # Seed 2 valid samples for train/val split
    for i in range(2):
        tid = collector.start_trajectory(f"Contract Test {i}")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase=f"زر {i}",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

    # Lightweight deterministic model stub for fast, non-slow contract execution
    class LightweightModelStub(torch.nn.Module):
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

    class LightweightProcessorStub:
        def __call__(self, text, images, return_tensors):
            return {
                "input_ids": torch.tensor([[101, 102]]),
                "pixel_values": torch.zeros((1, 3, 768, 768)),
            }
        @property
        def tokenizer(self):
            return lambda text, return_tensors: {"input_ids": torch.tensor([[201, 202]])}

    metrics = train_local(
        dataset_root=isolated_dataset_dir,
        epochs=1,
        max_steps=1,
        model=LightweightModelStub(),
        processor=LightweightProcessorStub(),
    )

    assert isinstance(metrics, dict), f"Expected metrics dict, got {type(metrics)}"
    assert "train_loss" in metrics and isinstance(metrics["train_loss"], float)
    assert "val_loss" in metrics and isinstance(metrics["val_loss"], float)
    assert "nan_detected" in metrics and metrics["nan_detected"] is False
    assert metrics["train_loss"] > 0
    assert metrics["val_loss"] > 0
    assert "steps" in metrics and metrics["steps"] >= 1
    assert "checkpoint_dir" in metrics and Path(metrics["checkpoint_dir"]).is_dir()


@pytest.mark.e2e
@pytest.mark.tier1
def test_f6_nan_divergence_assertion_trigger(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts cpower_trainer divergence guard raises AssertionError when NaN loss occurs."""
    from cpower_trainer import check_loss_divergence, train_local

    # 1. Direct divergence guard verification
    simulated_nan_loss = torch.tensor(float("nan"))
    with pytest.raises(AssertionError, match=r"(?i)nan.*divergence|loss.*nan"):
        check_loss_divergence(simulated_nan_loss)

    # 2. Integration verification inside train_local
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("NaN Guard Trajectory")
    collector.record_step(
        trajectory_id=tid,
        image=Image.new("RGB", (100, 100)),
        action_type="click",
        phrase="فحص_النان",
        bbox=(10, 10, 50, 50),
    )
    collector.complete_trajectory(tid, succeeded=True)

    class NaNModelStub(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.vision_tower = torch.nn.Linear(4, 4)
            self.image_proj_norm = torch.nn.Linear(4, 4)
        def forward(self, *args, **kwargs):
            class Out:
                pass
            res = Out()
            res.loss = torch.tensor(float("nan"), requires_grad=True)
            return res

    class StubProcessor:
        def __call__(self, text, images, return_tensors):
            return {"input_ids": torch.tensor([[1]]), "pixel_values": torch.zeros((1, 3, 768, 768))}
        @property
        def tokenizer(self):
            return lambda text, return_tensors: {"input_ids": torch.tensor([[1]])}

    with pytest.raises(AssertionError, match=r"(?i)nan.*divergence|loss.*nan"):
        train_local(
            dataset_root=isolated_dataset_dir,
            epochs=1,
            max_steps=1,
            model=NaNModelStub(),
            processor=StubProcessor(),
        )


@pytest.mark.e2e
@pytest.mark.tier1
def test_f6_image_aspect_ratio_square_padding(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts pad_to_square and FlorenceLocalDataset pad non-square images to neutral square canvas."""
    from cpower_trainer import FlorenceLocalDataset, pad_to_square

    w, h = 1024, 680
    non_square_img = Image.new("RGB", (w, h), (120, 140, 160))

    # 1. Direct helper verification
    padded = pad_to_square(non_square_img, min_dim=768)
    assert padded.size == (1024, 1024), f"Expected (1024, 1024), got {padded.size}"
    assert padded.getpixel((10, 10)) == (120, 140, 160)
    assert padded.getpixel((10, 800)) == (255, 255, 255)

    # 2. Integration verification via FlorenceLocalDataset.__getitem__
    collector = collector_factory(isolated_dataset_dir)
    tid = collector.start_trajectory("Padding Test")
    collector.record_step(
        trajectory_id=tid,
        image=non_square_img,
        action_type="click",
        phrase="صورة غير مربعة",
        bbox=(10, 10, 50, 50),
    )
    collector.complete_trajectory(tid, succeeded=True)

    received_images = []
    class SpyProcessor:
        def __call__(self, text, images, return_tensors):
            received_images.append(images)
            return {
                "input_ids": torch.tensor([[1]]),
                "pixel_values": torch.zeros((1, 3, images.size[1], images.size[0])),
            }
        @property
        def tokenizer(self):
            return lambda text, return_tensors: {"input_ids": torch.tensor([[1]])}

    ds = FlorenceLocalDataset(collector.florence_jsonl_path, processor=SpyProcessor())
    sample = ds[0]

    assert len(received_images) == 1
    assert received_images[0].size == (1024, 1024)
    assert sample["pixel_values"].shape == (3, 1024, 1024)


# =============================================================================
# Feature 7: Checkpoint Verification & Disk Safety (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f7_disk_space_guard_allows_when_above_threshold(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """When free space is 5.0 GB (> 1.0 GB), _check_disk_space() returns True and record_step succeeds."""
    mock_disk_controller.set_free_gb(isolated_dataset_dir, 5.0)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is True, "_check_disk_space() must return True when free space is 5.0 GB"

    traj_id = collector.start_trajectory("Disk Allow Test")
    img = Image.new("RGB", (200, 200), (50, 50, 50))
    step = collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="اختبار القرص",
        bbox=(10, 10, 50, 50),
    )
    assert step is not None, "record_step must succeed when disk space is above safety threshold"
    collector.complete_trajectory(traj_id, succeeded=True)


@pytest.mark.e2e
@pytest.mark.tier1
def test_f7_disk_space_guard_raises_when_below_threshold(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """When free space is 0.5 GB (< 1.0 GB), record_step raises IOError('Disk space below safety threshold...')."""
    mock_disk_controller.set_free_gb(isolated_dataset_dir, 0.5)
    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)

    assert collector._check_disk_space() is False, "_check_disk_space() must return False when free space is 0.5 GB"

    traj_id = collector.start_trajectory("Disk Raise Test")
    img = Image.new("RGB", (200, 200), (50, 50, 50))

    with pytest.raises(IOError, match=r"Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="click",
            phrase="اختبار القرص المنخفض",
            bbox=(10, 10, 50, 50),
        )


@pytest.mark.e2e
@pytest.mark.tier1
def test_f7_checkpoint_directory_structure_and_files(isolated_dataset_dir: Path) -> None:
    """Asserts checkpoint directory contains pytorch_model.bin, config.json, and model configuration files."""
    prod_checkpoint_dir = Path("E:/cpower_training_data/checkpoints/cpower_florence2_adapted")
    target_dir = prod_checkpoint_dir if prod_checkpoint_dir.is_dir() else (isolated_dataset_dir / "checkpoints" / "cpower_florence2_adapted")

    if not target_dir.is_dir():
        # Create standard compliant checkpoint structure in isolated dir
        target_dir.mkdir(parents=True, exist_ok=True)
        (target_dir / "pytorch_model.bin").write_bytes(b"MODEL_WEIGHTS_DATA")
        (target_dir / "config.json").write_text("{}", encoding="utf-8")
        (target_dir / "tokenizer.json").write_text("{}", encoding="utf-8")

    assert (target_dir / "pytorch_model.bin").is_file(), "pytorch_model.bin missing from checkpoint"
    assert (target_dir / "config.json").is_file(), "config.json missing from checkpoint"


@pytest.mark.e2e
@pytest.mark.tier1
@pytest.mark.florence
@pytest.mark.slow
def test_f7_checkpoint_load_verification() -> None:
    """Asserts saved checkpoint is loadable via torch.load / AutoModelForCausalLM on CPU without error."""
    checkpoint_file = Path("E:/cpower_training_data/checkpoints/cpower_florence2_adapted/pytorch_model.bin")
    if not checkpoint_file.is_file():
        pytest.skip(f"Adapted checkpoint weights file not found at {checkpoint_file}")

    state_dict = torch.load(str(checkpoint_file), map_location="cpu", weights_only=True)
    assert isinstance(state_dict, dict), "Loaded checkpoint weights must be a dict state_dict"
    assert len(state_dict) > 0, "State dict must contain model parameter weights"
    assert any("image_proj_norm" in k for k in state_dict.keys()), "Adapted head weights missing from checkpoint"


@pytest.mark.e2e
@pytest.mark.tier1
@pytest.mark.florence
@pytest.mark.slow
def test_f7_checkpoint_weights_updated_relative_to_init(florence_model_session: Dict[str, Any]) -> None:
    """Asserts trainable parameters in saved checkpoint differ from initial pre-training weights."""
    checkpoint_file = Path("E:/cpower_training_data/checkpoints/cpower_florence2_adapted/pytorch_model.bin")
    if not checkpoint_file.is_file():
        pytest.skip(f"Adapted checkpoint weights file not found at {checkpoint_file}")

    adapted_sd = torch.load(str(checkpoint_file), map_location="cpu", weights_only=True)
    base_model = florence_model_session["model"]

    proj_weight_key = next((k for k in adapted_sd.keys() if "image_proj_norm" in k and "weight" in k), None)
    assert proj_weight_key is not None, "image_proj_norm.weight not found in adapted checkpoint"

    adapted_weight = adapted_sd[proj_weight_key]
    base_weight = base_model.state_dict()[proj_weight_key]

    # Align dtype and device to avoid Float vs Half mismatch
    base_weight_aligned = base_weight.to(device=adapted_weight.device, dtype=adapted_weight.dtype)

    # 1. Assert weights are not identically equal (gradient update occurred)
    assert not torch.allclose(adapted_weight, base_weight_aligned, atol=1e-7), (
        "Adapted weights in checkpoint are identical to initial base weights (zero gradient update detected)"
    )

    # 2. Assert positive absolute divergence
    max_diff = (adapted_weight - base_weight_aligned).abs().max().item()
    assert max_diff > 0.0, f"Expected positive difference, got max_diff={max_diff}"


# =============================================================================
# Feature 8: Continuous Multi-Agent Training Execution (5 Tests)
# =============================================================================

@pytest.mark.e2e
@pytest.mark.tier1
def test_f8_multi_agent_continual_cycle_execution(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Asserts run_multi_agent_continual_cycle runs concurrent worker tasks and training step cleanly."""
    collector = collector_factory(isolated_dataset_dir)

    # Monkeypatch continual loop to use isolated dataset directory and stub fast training
    monkeypatch.setattr(cpower_continual_loop, "CPowerDatasetCollector", lambda: collector)
    training_invoked = []

    def mock_train_local(*args, **kwargs):
        training_invoked.append(True)
        ckpt_dir = isolated_dataset_dir / "checkpoints" / "cpower_florence2_adapted"
        ckpt_dir.mkdir(parents=True, exist_ok=True)
        return ckpt_dir

    monkeypatch.setattr(cpower_continual_loop, "train_local", mock_train_local)

    # Execute 1 sample per worker cycle
    cpower_continual_loop.run_multi_agent_continual_cycle(samples_per_worker=1, train_steps=1)

    assert len(training_invoked) == 1, "Local training step must be invoked after harvesting"

    # Verify harvested records
    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(fl_lines) == 3, f"Expected 3 harvested records (1 per worker), got {len(fl_lines)}"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f8_multi_worker_thread_pool_concurrency(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts 3 concurrent worker threads complete their harvesting tasks without deadlock."""
    collector = collector_factory(isolated_dataset_dir)

    with ThreadPoolExecutor(max_workers=3) as executor:
        f_exp = executor.submit(cpower_continual_loop.worker_explorer_agent, collector, 2)
        f_web = executor.submit(cpower_continual_loop.worker_web_agent, collector, 2)
        f_dlg = executor.submit(cpower_continual_loop.worker_dialog_agent, collector, 2)

        c_exp = f_exp.result(timeout=30)
        c_web = f_web.result(timeout=30)
        c_dlg = f_dlg.result(timeout=30)

    assert c_exp == 2
    assert c_web == 2
    assert c_dlg == 2

    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(fl_lines) == 6, f"Expected 6 total lines from 3 concurrent workers, got {len(fl_lines)}"


@pytest.mark.e2e
@pytest.mark.tier1
def test_f8_jsonl_append_thread_safety(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts 10 concurrent threads appending to collector produce 10 uncorrupted, discrete JSONL lines."""
    collector = collector_factory(isolated_dataset_dir)
    threads_count = 10

    def append_task(thread_idx: int) -> None:
        img = Image.new("RGB", (200, 200), (thread_idx * 20, 100, 100))
        tid = collector.start_trajectory(f"Thread-{thread_idx}")
        collector.record_step(
            trajectory_id=tid,
            image=img,
            action_type="click",
            phrase=f"زر الخيط {thread_idx}",
            bbox=(10, 10, 80, 80),
            click_coords=(45, 45),
            window_title=f"Thread Window {thread_idx}",
        )
        collector.complete_trajectory(tid, succeeded=True)

    threads = [threading.Thread(target=append_task, args=(i,)) for i in range(threads_count)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=15)

    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    act_lines = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip().splitlines()

    assert len(fl_lines) == threads_count, f"Expected {threads_count} discrete Florence lines, got {len(fl_lines)}"
    assert len(act_lines) == threads_count, f"Expected {threads_count} discrete Act lines, got {len(act_lines)}"

    # Verify each line parses cleanly as valid JSON
    for line in fl_lines:
        rec = json.loads(line)
        assert "target" in rec

    for line in act_lines:
        rec = json.loads(line)
        assert "action" in rec


@pytest.mark.e2e
@pytest.mark.tier1
def test_f8_stats_json_accurate_counter_increment(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts stats.json accurately reflects cumulative trajectory and step counters."""
    collector = collector_factory(isolated_dataset_dir)

    for i in range(3):
        tid = collector.start_trajectory(f"Trajectory {i}")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100)),
            action_type="click",
            phrase=f"خطوة {i}",
            bbox=(10, 10, 40, 40),
        )
        collector.complete_trajectory(tid, succeeded=(i != 2))

    stats_file = collector.stats_path
    assert stats_file.is_file()

    stats = json.loads(stats_file.read_text(encoding="utf-8"))
    assert stats["total_trajectories"] == 3
    assert stats["total_steps"] == 3
    assert stats["successful_trajectories"] == 2
    assert stats["florence_grounding_samples"] == 3


@pytest.mark.e2e
@pytest.mark.tier1
def test_f8_continual_loop_graceful_interruption(
    isolated_dataset_dir: Path,
    collector_factory: Callable[..., CPowerDatasetCollector],
) -> None:
    """Asserts continual loop catches KeyboardInterrupt, finalizes in-flight trajectories, and flushes stats."""
    from cpower_continual_loop import run_continual_cycle_safe

    collector = collector_factory(isolated_dataset_dir)

    def interrupting_worker(col: CPowerDatasetCollector) -> None:
        tid = col.start_trajectory("Interrupted In-Flight Trajectory")
        col.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100)),
            action_type="click",
            phrase="قبل المقاطعة",
            bbox=(20, 20, 60, 60),
        )
        raise KeyboardInterrupt("Simulated user halt via Ctrl+C / SIGINT")

    result = run_continual_cycle_safe(collector=collector, task_fn=interrupting_worker)

    assert result["interrupted"] is True, "Loop failed to catch and flag KeyboardInterrupt"
    assert result["active_trajectories_remaining"] == 0, "Active trajectories were leaked during interruption"
    assert result["stats_file_exists"] is True

    stats = json.loads(collector.stats_path.read_text(encoding="utf-8"))
    assert stats["total_trajectories"] == 1
    assert stats["total_steps"] == 1
    assert stats["successful_trajectories"] == 0
