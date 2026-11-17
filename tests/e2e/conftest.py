"""
CPower E2E Testing Suite Configuration & Fixture Harness (tests/e2e/conftest.py).

Provides:
  - isolated_dataset_dir: temporary sandboxed dataset hierarchy
  - mock_disk_controller: dynamic shutil.disk_usage monkeypatching
  - synthetic_ui_provider: deterministic PIL RGB generators across UI domains
  - arabic_sample_corpus: canonical Arabic test strings and reshaping helpers
  - florence_model_session: session-scoped singleton for Florence-2 processor and model on CPU
  - lightweight_processor_stub: fast quantization and token parsing stub
  - collector_factory: factory for CPowerDatasetCollector instances
  - reference_dataset_validator: contract-compliant reference dataset validator
  - smoke test cases validating all harness fixtures
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

from PIL import Image
import pytest

# Ensure clients/python and tests/e2e are on sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CLIENTS_PYTHON = PROJECT_ROOT / "clients" / "python"
TESTS_E2E = PROJECT_ROOT / "tests" / "e2e"

for p in (str(CLIENTS_PYTHON), str(TESTS_E2E), str(PROJECT_ROOT)):
    if p not in sys.path:
        sys.path.insert(0, p)

from mock_hardware import (
    ArabicSampleCorpus,
    DiskUsage,
    LightweightProcessorStub,
    MockDiskController,
    SyntheticUIProvider,
    ZeroMouseTracker,
)


# -----------------------------------------------------------------------------
# Storage & Isolation Fixtures
# -----------------------------------------------------------------------------

@pytest.fixture
def isolated_dataset_dir(tmp_path: Path) -> Path:
    """
    Yields an isolated temporary directory configured for CPower dataset operations.
    Pre-creates standard subdirectories: images/, trajectories/, checkpoints/.
    Purges automatically when the test concludes.
    """
    dataset_dir = tmp_path / "cpower_isolated_dataset"
    (dataset_dir / "images").mkdir(parents=True, exist_ok=True)
    (dataset_dir / "trajectories").mkdir(parents=True, exist_ok=True)
    (dataset_dir / "checkpoints").mkdir(parents=True, exist_ok=True)
    return dataset_dir


@pytest.fixture
def mock_disk_controller(monkeypatch) -> MockDiskController:
    """
    Provides a dynamic disk usage controller monkeypatching shutil.disk_usage.
    Allows testing disk space above (e.g. 5.0 GB) and below (e.g. 0.5 GB) safety thresholds.
    """
    controller = MockDiskController(monkeypatch=monkeypatch)
    return controller


# -----------------------------------------------------------------------------
# Multimodal UI & Arabic Fixtures
# -----------------------------------------------------------------------------

@pytest.fixture(scope="session")
def synthetic_ui_provider() -> SyntheticUIProvider:
    """Session-scoped deterministic PIL image and bounding box generator."""
    return SyntheticUIProvider()


@pytest.fixture(scope="session")
def arabic_sample_corpus() -> ArabicSampleCorpus:
    """Session-scoped canonical Arabic sample corpus."""
    return ArabicSampleCorpus()


@pytest.fixture
def lightweight_processor_stub() -> LightweightProcessorStub:
    """Provides fast Florence-2 location token quantization and parsing stub."""
    return LightweightProcessorStub()


@pytest.fixture
def zero_mouse_tracker() -> ZeroMouseTracker:
    """Spy fixture to verify zero physical mouse movement or click events."""
    return ZeroMouseTracker()


# -----------------------------------------------------------------------------
# Florence-2 Model Session Singleton (CPU)
# -----------------------------------------------------------------------------

@pytest.fixture(scope="session")
def florence_model_session() -> Dict[str, Any]:
    """
    Session-scoped singleton loading Microsoft Florence-2 base model and processor on CPU.
    Loads from C:/Users/Administrator/.models/florence-2-base once per test session.
    """
    model_dir = Path("C:/Users/Administrator/.models/florence-2-base")
    if not model_dir.is_dir() or not (model_dir / "pytorch_model.bin").is_file():
        pytest.skip(f"Florence-2 base model weights not present at {model_dir}")

    import torch
    from transformers import AutoModelForCausalLM, AutoProcessor

    processor = AutoProcessor.from_pretrained(str(model_dir), trust_remote_code=True)
    model = AutoModelForCausalLM.from_pretrained(
        str(model_dir),
        trust_remote_code=True,
        torch_dtype=torch.float32,
    )
    model.to("cpu")
    model.eval()

    return {
        "processor": processor,
        "model": model,
        "model_dir": model_dir,
    }


# -----------------------------------------------------------------------------
# Collector & Validator Factories
# -----------------------------------------------------------------------------

@pytest.fixture
def collector_factory(isolated_dataset_dir: Path, mock_disk_controller: MockDiskController) -> Callable[..., Any]:
    """
    Factory function returning a pre-configured CPowerDatasetCollector.
    Defaults to isolated_dataset_dir and sets mock disk space to 50 GB if not already configured.
    """
    from cpower_dataset_collector import CPowerDatasetCollector

    def _create_collector(
        base_dir: Optional[Union[str, Path]] = None,
        min_free_gb: float = 1.0,
        mock_free_gb: Optional[float] = None,
    ) -> CPowerDatasetCollector:
        target_dir = Path(base_dir) if base_dir else isolated_dataset_dir
        if mock_free_gb is not None:
            mock_disk_controller.set_free_gb(target_dir, mock_free_gb)
        else:
            resolved_key = str(target_dir.resolve()).lower()
            if resolved_key not in mock_disk_controller._overrides:
                mock_disk_controller.set_free_gb(target_dir, 50.0)

        collector = CPowerDatasetCollector(base_dir=target_dir, min_free_gb=min_free_gb)
        return collector

    return _create_collector


def contract_validate_dataset(dataset_dir: Union[str, Path]) -> Tuple[bool, List[str]]:
    """
    Reference specification-compliant implementation of validate_dataset.
    Adheres strictly to PROJECT.md § Interface Contracts:
      - Verifies every line in dataset_florence2.jsonl has valid JSON, matching image file in images/,
        valid <loc_...> tokens in [0..999], and positive non-inverted bounding boxes.
      - Verifies every line in dataset_agent_act.jsonl matches trajectory and click falls within target bbox.
    """
    root = Path(dataset_dir)
    errors: List[str] = []

    florence_path = root / "dataset_florence2.jsonl"
    agent_act_path = root / "dataset_agent_act.jsonl"
    images_dir = root / "images"

    if not florence_path.exists():
        errors.append(f"Missing florence dataset file: {florence_path}")
        return False, errors

    loc_regex = re.compile(
        r"^.*<loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})>$",
        re.DOTALL,
    )
    mandatory_florence_keys = {"id", "image", "prefix", "suffix", "target", "bbox_px", "image_size"}

    florence_records: Dict[str, Dict[str, Any]] = {}

    with open(florence_path, "r", encoding="utf-8") as f:
        for line_num, line in enumerate(f, start=1):
            line_str = line.strip()
            if not line_str:
                continue
            try:
                rec = json.loads(line_str)
            except Exception as e:
                errors.append(f"Line {line_num} in {florence_path.name} is invalid JSON: {e}")
                continue

            missing = mandatory_florence_keys - set(rec.keys())
            if missing:
                errors.append(f"Line {line_num}: Missing mandatory keys {missing}")
                continue

            # Verify image file exists
            img_rel = rec["image"]
            img_file = root / img_rel
            if not img_file.is_file():
                errors.append(f"Line {line_num}: Referenced image file not found: {img_file}")
            elif img_file.stat().st_size == 0:
                errors.append(f"Line {line_num}: Image file is empty (0 bytes): {img_file}")
            else:
                try:
                    with Image.open(img_file) as img:
                        img.verify()
                except Exception as img_err:
                    errors.append(f"Line {line_num}: Corrupted image {img_file}: {img_err}")

            # Verify location tokens in target
            target = rec["target"]
            m = loc_regex.match(target)
            if not m:
                errors.append(f"Line {line_num}: Target does not match Florence-2 loc pattern: {target}")
            else:
                nx1, ny1, nx2, ny2 = (int(x) for x in m.groups())
                for coord in (nx1, ny1, nx2, ny2):
                    if coord < 0 or coord > 999:
                        errors.append(f"Line {line_num}: Coordinate {coord} out of range [0, 999]")

                if nx2 <= nx1 or ny2 <= ny1:
                    errors.append(f"Line {line_num}: Inverted or zero-area bounding box: [{nx1}, {ny1}, {nx2}, {ny2}]")

            # Check pixel bbox
            bbox_px = rec.get("bbox_px")
            if not isinstance(bbox_px, (list, tuple)) or len(bbox_px) != 4:
                errors.append(f"Line {line_num}: Invalid bbox_px (expected 4-element list/tuple): {bbox_px}")
            else:
                px1, py1, px2, py2 = bbox_px
                if px2 <= px1 or py2 <= py1:
                    errors.append(f"Line {line_num}: Inverted pixel bounding box: {bbox_px}")

            florence_records[rec["id"]] = rec

    # Validate dataset_agent_act.jsonl if present
    if agent_act_path.exists():
        mandatory_act_keys = {"trajectory_id", "step_index", "goal", "image", "action", "window"}
        with open(agent_act_path, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, start=1):
                line_str = line.strip()
                if not line_str:
                    continue
                try:
                    rec = json.loads(line_str)
                except Exception as e:
                    errors.append(f"Line {line_num} in {agent_act_path.name} is invalid JSON: {e}")
                    continue

                missing = mandatory_act_keys - set(rec.keys())
                if missing:
                    errors.append(f"Line {line_num} in {agent_act_path.name}: Missing keys {missing}")
                    continue

                action = rec.get("action", {})
                click = action.get("click")

                # Cross-reference with matching Florence record if available
                rec_id = f"{rec['trajectory_id']}_{rec['step_index']:02d}"
                fl_rec = florence_records.get(rec_id)

                if click is not None:
                    if not isinstance(click, (list, tuple)) or len(click) != 2:
                        errors.append(
                            f"Line {line_num} in {agent_act_path.name}: Malformed click coordinates {click}"
                        )
                        continue
                    cx, cy = click
                    if fl_rec:
                        fl_bbox = fl_rec.get("bbox_px")
                        if isinstance(fl_bbox, (list, tuple)) and len(fl_bbox) == 4:
                            bx1, by1, bx2, by2 = fl_bbox
                            if not (bx1 <= cx <= bx2 and by1 <= cy <= by2):
                                errors.append(
                                    f"Line {line_num} in {agent_act_path.name}: Click ({cx}, {cy}) outside bbox [{bx1}, {by1}, {bx2}, {by2}]"
                                )
                        else:
                            errors.append(
                                f"Line {line_num} in {agent_act_path.name}: Referenced Florence record {rec_id} has invalid bbox_px: {fl_bbox}"
                            )
                    else:
                        errors.append(
                            f"Line {line_num} in {agent_act_path.name}: No matching Florence record for id {rec_id}"
                        )

    return (len(errors) == 0, errors)


@pytest.fixture
def reference_dataset_validator() -> Callable[[Union[str, Path]], Tuple[bool, List[str]]]:
    """Returns the authoritative dataset validator function."""
    try:
        from cpower_dataset_validator import validate_dataset
        return validate_dataset
    except ImportError:
        return contract_validate_dataset


# =============================================================================
# Smoke Tests for Test Infrastructure Verification (Milestone TM1)
# =============================================================================

@pytest.mark.e2e
def test_smoke_isolated_dataset_and_collector_factory(isolated_dataset_dir: Path, collector_factory: Callable) -> None:
    """Verifies isolated temporary storage creation and collector factory initialization."""
    assert isolated_dataset_dir.is_dir()
    assert (isolated_dataset_dir / "images").is_dir()
    assert (isolated_dataset_dir / "trajectories").is_dir()
    assert (isolated_dataset_dir / "checkpoints").is_dir()

    collector = collector_factory(isolated_dataset_dir)
    assert collector.base_dir == isolated_dataset_dir
    assert collector.images_dir == isolated_dataset_dir / "images"

    # Record a test step
    traj_id = collector.start_trajectory(goal="Smoke test trajectory")
    img = Image.new("RGB", (200, 200), (255, 255, 255))
    step = collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="زر تجريبي",
        bbox=(20, 20, 80, 80),
        click_coords=(50, 50),
        window_title="Smoke Window",
    )
    assert step["normalized_bbox"] == [100, 100, 400, 400]
    collector.complete_trajectory(traj_id, succeeded=True)

    assert (isolated_dataset_dir / "dataset_florence2.jsonl").is_file()
    assert (isolated_dataset_dir / "dataset_agent_act.jsonl").is_file()
    assert (isolated_dataset_dir / "images" / f"{traj_id}_step_00.jpg").is_file()


@pytest.mark.e2e
def test_smoke_mock_disk_controller(
    isolated_dataset_dir: Path,
    mock_disk_controller: MockDiskController,
    collector_factory: Callable,
) -> None:
    """Verifies dynamic disk space manipulation and safety floor enforcement."""
    # Set space below 1.0 GB floor
    mock_disk_controller.set_free_gb(isolated_dataset_dir, 0.5)
    import shutil
    usage = shutil.disk_usage(str(isolated_dataset_dir))
    assert usage.free < 1024 * 1024 * 1024

    collector = collector_factory(isolated_dataset_dir, min_free_gb=1.0)
    traj_id = collector.start_trajectory(goal="Disk guard test")
    img = Image.new("RGB", (100, 100), (0, 0, 0))

    with pytest.raises(IOError, match="Disk space below safety threshold"):
        collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="click",
            phrase="test",
            bbox=(10, 10, 50, 50),
        )

    # Restore space above threshold
    mock_disk_controller.set_free_gb(isolated_dataset_dir, 5.0)
    usage_restored = shutil.disk_usage(str(isolated_dataset_dir))
    assert usage_restored.free >= 5 * 1024 * 1024 * 1024

    # Now step succeeds
    step = collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="test",
        bbox=(10, 10, 50, 50),
    )
    assert step is not None


@pytest.mark.e2e
def test_smoke_synthetic_ui_provider(synthetic_ui_provider: SyntheticUIProvider) -> None:
    """Verifies deterministic UI canvas generators across all supported domains."""
    # Explorer
    exp_img, exp_bbox, exp_phrase = synthetic_ui_provider.create_explorer_ui()
    assert exp_img.size == (1024, 680)
    assert exp_bbox[2] > exp_bbox[0] and exp_bbox[3] > exp_bbox[1]
    assert "مجلد" in exp_phrase

    # YouTube
    yt_img, yt_bbox, yt_phrase = synthetic_ui_provider.create_youtube_ui()
    assert yt_img.size == (1100, 700)
    assert yt_bbox[2] > yt_bbox[0] and yt_bbox[3] > yt_bbox[1]

    # Dialog
    dlg_img, dlg_bbox, dlg_phrase = synthetic_ui_provider.create_dialog_ui(target_button="حفظ")
    assert dlg_img.size == (800, 500)
    assert "حفظ" in dlg_phrase

    # Arabic RTL
    rtl_img, rtl_elements = synthetic_ui_provider.create_arabic_rtl_ui()
    assert rtl_img.size == (1024, 600)
    assert "زر_حفظ" in rtl_elements
    assert "حقل_اسم_المشروع" in rtl_elements


@pytest.mark.e2e
def test_smoke_arabic_sample_corpus(arabic_sample_corpus: ArabicSampleCorpus) -> None:
    """Verifies canonical Arabic corpus and cursive bidi shaping."""
    raw_rtl = arabic_sample_corpus.PURE_RTL[0]
    shaped_rtl = arabic_sample_corpus.reshape_and_bidi(raw_rtl)
    assert len(shaped_rtl) > 0

    mixed = arabic_sample_corpus.MIXED_BIDI[0]
    shaped_mixed = arabic_sample_corpus.reshape_and_bidi(mixed)
    assert len(shaped_mixed) > 0

    assert len(arabic_sample_corpus.DIACRITICS_HEAVY) > 0
    assert len(arabic_sample_corpus.EASTERN_NUMERALS) > 0


@pytest.mark.e2e
def test_smoke_lightweight_processor_stub(lightweight_processor_stub: LightweightProcessorStub) -> None:
    """Verifies quantization math and token formatting stub."""
    img_size = (1920, 1080)
    bbox = (192, 108, 960, 540)
    norm = lightweight_processor_stub.quantize_bbox(bbox, img_size)
    assert norm == [100, 100, 500, 500]

    target = lightweight_processor_stub.format_target("زر حفظ", bbox, img_size)
    assert target == "زر حفظ<loc_100><loc_100><loc_500><loc_500>"

    parsed = lightweight_processor_stub.parse_target(target, img_size)
    assert parsed["valid"] is True
    assert parsed["phrase"] == "زر حفظ"
    assert parsed["positive_area"] is True
    assert parsed["normalized_bbox"] == [100, 100, 500, 500]

    assert lightweight_processor_stub.is_valid_loc_token("<loc_0>") is True
    assert lightweight_processor_stub.is_valid_loc_token("<loc_999>") is True
    assert lightweight_processor_stub.is_valid_loc_token("<loc_1000>") is False
    assert lightweight_processor_stub.is_valid_loc_token("<loc_-1>") is False


@pytest.mark.e2e
def test_smoke_reference_dataset_validator(
    isolated_dataset_dir: Path,
    collector_factory: Callable,
    reference_dataset_validator: Callable,
) -> None:
    """Verifies dataset validator validation on compliant dataset."""
    collector = collector_factory(isolated_dataset_dir)
    traj_id = collector.start_trajectory(goal="Validator smoke goal")
    img = Image.new("RGB", (800, 600), (200, 200, 200))
    collector.record_step(
        trajectory_id=traj_id,
        image=img,
        action_type="click",
        phrase="زر تجريبي",
        bbox=(50, 50, 150, 150),
        click_coords=(100, 100),
        window_title="Smoke Window",
    )
    collector.complete_trajectory(traj_id, succeeded=True)

    valid, errors = reference_dataset_validator(isolated_dataset_dir)
    assert valid is True, f"Validator failed on compliant dataset: {errors}"
    assert len(errors) == 0


@pytest.mark.e2e
@pytest.mark.florence
@pytest.mark.slow
def test_smoke_florence_model_session(florence_model_session: Dict[str, Any]) -> None:
    """Verifies that Florence-2 base model and processor load cleanly on CPU and execute a forward pass."""
    import torch
    processor = florence_model_session["processor"]
    model = florence_model_session["model"]
    assert processor is not None
    assert model is not None
    assert "<loc_999>" in processor.tokenizer.get_vocab()
    assert "<loc_1000>" not in processor.tokenizer.get_vocab()

    # Empirical CPU forward pass verification
    dummy_img = Image.new("RGB", (768, 768), (255, 255, 255))
    inputs = processor(text="<OD>", images=dummy_img, return_tensors="pt")
    labels = processor.tokenizer(text="button<loc_0><loc_0><loc_100><loc_100>", return_tensors="pt")["input_ids"]
    with torch.no_grad():
        out = model(
            input_ids=inputs["input_ids"],
            pixel_values=inputs["pixel_values"].to(dtype=torch.float32),
            labels=labels,
        )
    assert not torch.isnan(out.loss), "Forward pass loss returned NaN"
    assert out.loss.item() > 0, "Forward pass loss must be positive"



if __name__ == "__main__":
    print("[INFRA SMOKE] Executing conftest self-diagnostics directly...")
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        t_path = Path(td)
        ctrl = MockDiskController()
        ui = SyntheticUIProvider()
        corpus = ArabicSampleCorpus()
        stub = LightweightProcessorStub()

        print("1. Testing MockDiskController...")
        ctrl.set_free_gb(t_path, 50.0)
        assert ctrl._mocked_disk_usage(t_path).free == 50 * 1024 * 1024 * 1024

        print("2. Testing SyntheticUIProvider...")
        img, bbox, phrase = ui.create_explorer_ui()
        assert img.size == (1024, 680)

        print("3. Testing ArabicSampleCorpus...")
        res = corpus.reshape_and_bidi("حفظ الملف")
        assert len(res) > 0

        print("4. Testing LightweightProcessorStub...")
        norm = stub.quantize_bbox((0, 0, 100, 100), (1000, 1000))
        assert norm == [0, 0, 100, 100]

        print("5. Testing reference_dataset_validator contract...")
        from cpower_dataset_collector import CPowerDatasetCollector
        col = CPowerDatasetCollector(base_dir=t_path, min_free_gb=1.0)
        tid = col.start_trajectory("smoke")
        col.record_step(tid, Image.new("RGB", (200, 200)), "click", "btn", (10, 10, 50, 50), click_coords=(25, 25))
        valid, errs = contract_validate_dataset(t_path)
        assert valid is True, f"Validation errors: {errs}"

    print("[INFRA SMOKE] All self-diagnostic smoke tests PASSED successfully!")
