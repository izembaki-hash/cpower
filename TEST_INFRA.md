# CPower End-to-End Test Infrastructure & Verification Specification

**Version:** `2.0.0`  
**Language / Framework:** Python 3.12 (`pytest >= 8.0`, `PyTorch 2.13+cpu`, `Transformers 5.22+`, `Pillow 12.3+`)  
**Target Repository:** `h:\c power`  
**Root Path & Test Path:** `tests/e2e/` (configured via `pytest.ini`)  

---

## 1. Executive Summary & Test Philosophy

This specification establishes the official End-to-End (E2E) testing framework, verification methodology, test inventory, and quality gates for the CPower Autonomous Multi-Agent Harvesting and Continual Florence-2 Training system.

### Core Testing Principles
1. **Opaque-Box Requirement-Driven Testing**:  
   Tests evaluate the system strictly through its public APIs (`cpower_dataset_collector.CPowerDatasetCollector`, `cpower_multitask_harvester.MultiTaskUIHarvester`, `cpower_dataset_validator.validate_dataset`, `cpower_trainer.train_local`, `cpower_continual_loop.ContinualTrainingLoop`) and persisted filesystem artifacts (`dataset_florence2.jsonl`, `dataset_agent_act.jsonl`, `images/*.jpg`, `stats.json`, `pytorch_model.bin`). Tests never tamper with internal private variables.
2. **Zero-Fabrication Integrity Policy**:  
   All tests execute authentic software pipelines. No facade tests, no artificial passes, no hardcoded synthetic assertions matching mock implementations rather than specifications. Every test failure halts CI/CD promotion.
3. **Zero-Mouse, Non-Intrusive Execution**:  
   In compliance with `ORIGINAL_REQUEST.md § Acceptance Criteria` ("No manual on-screen mouse interaction or manual training demonstration is required for dataset generation"), testing relies on in-memory PIL canvas generation, headless Chromium DOM rendering (`headless=True`), and synthetic accessibility injection. The physical Windows cursor is never hijacked, preserving host interactivity.
4. **Hardware Decoupling & CPU-Only Execution**:  
   The test suite executes entirely on local CPU architecture (`torch 2.13.0+cpu`, no CUDA required). Deep neural network tests employ Parameter-Efficient Fine-Tuning (freezing DaViT vision backbone) and calibrated micro-steps (`max_steps=1` or `max_steps=2`) to achieve full loss and tensor execution verification within strict runtime budgets.
5. **Storage Isolation & Disk Protection**:  
   All test writes are strictly quarantined within pytest `tmp_path` sandboxes on drive `C:` or directed to `E:\cpower_training_data`. Under no circumstances are volatile image datasets or multi-gigabyte checkpoints written to drive `H:`, preserving its limited 2.01 GB headroom.

---

## 2. Test Architecture & Directory Layout

The E2E test harness resides under `tests/e2e/`, structured into modular test tiers alongside shared isolation fixtures:

```
h:\c power\
├── pytest.ini                            # Master pytest configuration
├── TEST_INFRA.md                         # This authoritative specification
├── tests/
│   ├── e2e/
│   │   ├── __init__.py                   # Package initializer
│   │   ├── conftest.py                   # Master pytest fixtures and test isolation harness
│   │   ├── mock_hardware.py              # Synthetic UI generators and hardware/disk mock controller
│   │   ├── test_tier1_features.py        # Tier 1: Feature Coverage (40 tests, 5 per feature)
│   │   ├── test_tier2_boundaries.py      # Tier 2: Boundary & Extreme Conditions (40 tests)
│   │   ├── test_tier3_combinations.py    # Tier 3: Cross-Feature Integration (10 pairwise tests)
│   │   └── test_tier4_scenarios.py       # Tier 4: Real-World Continual Scenarios (6 scenarios)
```

### Pytest Configuration (`pytest.ini`)
```ini
[pytest]
minversion = 8.0
testpaths = tests/e2e
python_files = test_*.py
python_classes = Test*
python_functions = test_*
norecursedirs = .git .agents bin obj build dist scratch external third_party clients/python/__pycache__
markers =
    e2e: Master marker for all End-to-End pipeline verification tests
    tier1: Tier 1 Feature Coverage tests (happy path, isolated verification)
    tier2: Tier 2 Boundary & Corner Cases tests (limits, errors, disk safety)
    tier3: Tier 3 Cross-Feature Combination tests (dataflow integration)
    tier4: Tier 4 Real-World Continual Scenario tests (multi-agent, checkpoints)
    slow: Tests executing PyTorch CPU training or multi-step continual loops
    florence: Tests requiring local Florence-2 model weights at C:\Users\Administrator\.models\florence-2-base
addopts = -v --strict-markers -ra --tb=short
filterwarnings =
    ignore::FutureWarning:transformers.*
    ignore::DeprecationWarning
```

---

## 3. Test Fixtures & Isolation Harness Specification

Located in `tests/e2e/conftest.py` and `tests/e2e/mock_hardware.py`, these fixtures provide isolated execution environments:

| Fixture Name | Scope | Purpose & Interface Contract |
|---|---|---|
| `isolated_dataset_dir` | `function` | Yields a pristine temporary `Path` under pytest's `tmp_path`. Pre-creates standard subdirectories (`images/`, `trajectories/`, `checkpoints/`). Automatically purged upon test completion. |
| `mock_disk_controller` | `function` | Provides dynamic monkeypatching of `shutil.disk_usage` via a callable `set_free_space(path, free_bytes, total_bytes)`. Enables simulation of low disk space (<1.0 GB) and high disk space (>10 GB). |
| `synthetic_ui_provider` | `session` | Generates calibrated PIL RGB images and exact ground-truth bounding boxes for: Windows 11 Explorer (1024x680), YouTube Media (1100x700), System Dialogs (800x500), and Arabic RTL interfaces. |
| `arabic_sample_corpus` | `session` | Authoritative corpus of Arabic test strings: pure RTL, mixed LTR/RTL bidirectional paths (`C:\بيانات\config.json`), heavy tashkeel diacritics, Eastern Arabic numerals (`١٢٣٤٥`), and Lam-Alif ligatures (`لا إله إلا الله`). |
| `florence_model_session` | `session` | Scoped singleton loading `AutoProcessor` and `AutoModelForCausalLM` from `C:\Users\Administrator\.models\florence-2-base` on CPU once per session, avoiding repeated 15s model loading overhead. |
| `lightweight_processor_stub` | `function` | Fast stub processor that performs exact Florence-2 quantization math ($[xmin, ymin, xmax, ymax] \to <\text{loc\_}0> \dots <\text{loc\_}999>$) and tokenizer validation without model tensor overhead. |
| `collector_factory` | `function` | Factory function returning fresh `CPowerDatasetCollector` instances configured with `isolated_dataset_dir` and customizable `min_free_gb`. |

---

## 4. Feature Coverage Matrix (Tier 1: 40 Tests)

Tier 1 validates the primary behavior (happy path) of each feature in isolation. 5 comprehensive tests per feature across all 8 features defined in `PROJECT.md`.

### Feature 1: Windows 11 Controls Harvester (5 Tests)
- **`test_f1_explorer_task_generation_structure`**: Verifies `MultiTaskUIHarvester.generate_explorer_task` generates a 1024x680 RGB image, step record with action `double_click`, and phrase `مجلد ...`.
- **`test_f1_explorer_bounding_box_within_image_bounds`**: Asserts target folder bounding box satisfies $0 \le x_1 < x_2 \le 1024$ and $0 \le y_1 < y_2 \le 680$, with positive width and height.
- **`test_f1_explorer_click_coords_inside_bbox`**: Asserts synthesized click coordinates fall strictly inside target folder bounding box: $x_1 \le cx \le x_2$ and $y_1 \le cy \le y_2$.
- **`test_f1_system_dialog_task_generation`**: Verifies `generate_system_dialog_task` generates Save/Cancel/Browse buttons with valid window title and Arabic action labels.
- **`test_f1_windows11_harvester_zero_hardware_mouse_calls`**: Asserts Win32 hardware mouse APIs (`SetCursorPos`, `mouse_event`) are never called during harvesting.

### Feature 2: Web & YouTube Media Harvester (5 Tests)
- **`test_f2_youtube_task_generation_dark_theme`**: Verifies YouTube canvas has dark theme aesthetics (mean RGB < 60) and window title `"YouTube - Google Chrome"`.
- **`test_f2_youtube_search_bar_and_button_geometry`**: Asserts search button has width $\ge 30$, height $\ge 20$, and non-overlapping alignment with search input box.
- **`test_f2_youtube_video_card_metadata`**: Asserts metadata contains `domain: "youtube"` and non-empty search query string.
- **`test_f2_youtube_action_type_validity`**: Asserts action is `click` and target coordinates correspond to video thumbnail or search button.
- **`test_f2_web_harvester_headless_mode_enforcement`**: Asserts web harvesting operates strictly in headless mode without spawning interactive OS browser windows.

### Feature 3: Rich Arabic RTL Synthesizer (5 Tests)
- **`test_f3_arabic_text_reshaping_cursive_connection`**: Verifies `arabic_reshaper.reshape` connects isolated Arabic characters into connected cursive presentation forms.
- **`test_f3_arabic_bidi_visual_reordering`**: Verifies `bidi.algorithm.get_display` correctly positions bidirectional mixed LTR/RTL substrings into visual display order.
- **`test_f3_arabic_diacritics_handling`**: Asserts Arabic tashkeel diacritics (Fatha, Damma, Kasra, Shadda, Sukun) render without unicode decoding errors.
- **`test_f3_arabic_rtl_layout_alignment`**: Verifies RTL header/toolbar controls anchor on the right half of the canvas ($x > W/2$).
- **`test_f3_arabic_phrase_grounding_target_formatting`**: Asserts grounding target string embeds raw Arabic UTF-8 text alongside `<loc_...>` tokens without unicode escape corruption.

### Feature 4: Coordinate Normalization & Tokenizer Align (5 Tests)
- **`test_f4_coordinate_normalization_proportional_scaling`**: Validates exact conversion of pixel bbox `(192, 108, 960, 540)` on 1920x1080 canvas to `[100, 100, 500, 500]`.
- **`test_f4_coordinate_clamping_to_999`**: Asserts pixels at outer canvas edge $(W, H)$ clamp strictly to 999 (never 1000).
- **`test_f4_coordinate_order_invariance`**: Verifies coordinate sequence is strictly $[nx_1, ny_1, nx_2, ny_2]$ with $nx_1 \le nx_2$ and $ny_1 \le ny_2$.
- **`test_f4_florence2_token_regex_conformance`**: Matches target string against regex `r"^.*<loc_\d{1,3}><loc_\d{1,3}><loc_\d{1,3}><loc_\d{1,3}>$"`.
- **`test_f4_florence2_tokenizer_vocabulary_membership`**: Verifies tokens `<loc_0>` through `<loc_999>` exist as atomic tokens in the Florence-2 vocabulary and `<loc_1000>` does not.

### Feature 5: JSONL Schema & Image Integrity Validator (5 Tests)
- **`test_f5_validator_detects_valid_florence_jsonl_record`**: Asserts validator returns `(True, [])` when presented with fully compliant dataset records.
- **`test_f5_validator_detects_missing_image_file`**: Asserts validator identifies and reports missing image referenced by `image` path.
- **`test_f5_validator_detects_schema_key_mismatch`**: Asserts missing mandatory key (e.g. `prefix`, `suffix`, or `bbox_px`) causes validation rejection with descriptive error.
- **`test_f5_validator_detects_inverted_bounding_box`**: Asserts inverted bounding box ($x_1 > x_2$ or $y_1 > y_2$) is flagged by validator.
- **`test_f5_validator_detects_click_outside_target_in_agent_act`**: Asserts click coordinate outside target bounding box in `dataset_agent_act.jsonl` is flagged as misaligned.

### Feature 6: Training Pipeline Enhancement (5 Tests)
- **`test_f6_training_dataset_train_val_split_ratio`**: Asserts 20-sample dataset splits into 18 training (90%) and 2 validation (10%) samples.
- **`test_f6_model_parameter_freezing_verification`**: Asserts `model.vision_tower.parameters()` has `requires_grad=False` and `model.image_proj_norm.parameters()` has `requires_grad=True`.
- **`test_f6_train_and_val_loss_logging_contract`**: Asserts training output dictionary contains `train_loss: float`, `val_loss: float`, and `nan_detected: False`.
- **`test_f6_nan_divergence_assertion_trigger`**: Asserts simulated NaN loss triggers `AssertionError` with divergence warning message.
- **`test_f6_image_aspect_ratio_square_padding`**: Asserts non-square image is padded to square canvas with neutral background without distortion.

### Feature 7: Checkpoint Verification & Disk Safety (5 Tests)
- **`test_f7_disk_space_guard_allows_when_above_threshold`**: When free space is 5.0 GB ($>1.0\text{ GB}$), `_check_disk_space()` returns True and `record_step` succeeds.
- **`test_f7_disk_space_guard_raises_when_below_threshold`**: When free space is 0.5 GB ($<1.0\text{ GB}$), `record_step` raises `IOError("Disk space below safety threshold...")`.
- **`test_f7_checkpoint_directory_structure_and_files`**: Asserts checkpoint directory contains `pytorch_model.bin`, `config.json`, and model configuration files.
- **`test_f7_checkpoint_load_verification`**: Asserts saved checkpoint is loadable via `AutoModelForCausalLM.from_pretrained` on CPU without error.
- **`test_f7_checkpoint_weights_updated_relative_to_init`**: Asserts trainable parameters in saved checkpoint differ from initial pre-training weights.

### Feature 8: Continuous Multi-Agent Training Execution (5 Tests)
- **`test_f8_multi_agent_continual_cycle_execution`**: Asserts `run_multi_agent_continual_cycle` runs concurrent worker tasks and 1 training step cleanly.
- **`test_f8_multi_worker_thread_pool_concurrency`**: Asserts 3 concurrent worker threads complete their harvesting tasks without deadlock.
- **`test_f8_jsonl_append_thread_safety`**: Asserts 10 concurrent threads appending to collector produce 10 uncorrupted, discrete JSONL lines.
- **`test_f8_stats_json_accurate_counter_increment`**: Asserts `stats.json` accurately reflects cumulative trajectory and step counters.
- **`test_f8_continual_loop_graceful_interruption`**: Asserts loop terminates cleanly on interrupt signal, flushing memory and persisting stats.

---

## 5. Boundary & Extreme Conditions (Tier 2: 40 Tests)

Tier 2 pushes the pipeline to its mathematical, structural, and environmental limits.

### Category 2.1: Coordinate & Bounding Box Limits (10 Tests)
1. **`test_b1_coord_origin_zero_zero`**: Target bbox at exact origin `(0, 0, 10, 10)` normalizes to `<loc_0><loc_0>...`.
2. **`test_b2_coord_maximum_999`**: Target bbox at bottom-right corner `(w-1, h-1, w, h)` normalizes to `<loc_...><loc_999><loc_999>`.
3. **`test_b3_coord_fullscreen_bounding_box`**: Entire canvas `(0, 0, w, h)` normalizes to `<loc_0><loc_0><loc_999><loc_999>`.
4. **`test_b4_coord_negative_inputs_clamped_to_zero`**: Negative input coordinates `(-50, -20, 100, 100)` clamp safely to `<loc_0><loc_0>`.
5. **`test_b5_coord_overflow_inputs_clamped_to_999`**: Overflow coordinates `(100, 100, w+500, h+500)` clamp safely to `<loc_999><loc_999>`.
6. **`test_b6_coord_single_pixel_bounding_box`**: 1-pixel target `(500, 500, 501, 501)` generates valid non-zero normalized area.
7. **`test_b7_coord_zero_width_or_height_rejected`**: Bounding box with width=0 or height=0 is rejected by validator as degenerate.
8. **`test_b8_coord_inverted_bbox_handling`**: Inverted bounding box `(400, 300, 200, 100)` is flagged and rejected by validator.
9. **`test_b9_coord_float_inputs_rounded`**: Floating point coordinates `(100.4, 200.7, 300.2, 400.9)` round cleanly to integer tokens.
10. **`test_b10_coord_extreme_resolution_1x1`**: Micro-resolution image 1x1 with bbox `(0, 0, 1, 1)` yields `<loc_0><loc_0><loc_999><loc_999>`.

### Category 2.2: Empty & Corrupted Inputs (10 Tests)
11. **`test_b11_empty_string_phrase`**: Empty phrase string `""` is handled with descriptive fallback or rejection.
12. **`test_b12_empty_string_goal`**: Empty goal string `""` defaults safely to `"Unspecified"`.
13. **`test_b13_whitespace_only_phrase`**: Whitespace-only phrase `"   \t\n"` is stripped or rejected.
14. **`test_b14_corrupted_jsonl_line_skipped_by_dataset_loader`**: Corrupted JSON line in JSONL file is skipped without crashing dataset loader.
15. **`test_b15_truncated_jsonl_line`**: Truncated JSON line at EOF is skipped gracefully.
16. **`test_b16_missing_target_key_in_florence_jsonl`**: Record missing `"target"` key is flagged with line number.
17. **`test_b17_zero_byte_image_file`**: 0-byte image file is reported by validator as corrupted.
18. **`test_b18_truncated_jpeg_image`**: Truncated JPEG file is reported by validator as unreadable.
19. **`test_b19_extreme_aspect_ratio_panorama`**: Ultra-wide image 8000x200 is padded to square canvas without out-of-memory errors.
20. **`test_b20_extreme_aspect_ratio_tall`**: Ultra-tall image 200x8000 is padded to square canvas without out-of-memory errors.

### Category 2.3: Arabic Bi-directional & Typography Extremes (10 Tests)
21. **`test_b21_pure_arabic_diacritics_heavy`**: Full tashkeel text is rendered without character truncation.
22. **`test_b22_mixed_ltr_rtl_triple_alternation`**: String with multiple RTL/LTR transitions (`"المجلد C:\CPower\Data رقم 1 تم حفظه بنجاح"`) orders correctly.
23. **`test_b23_eastern_arabic_numerals`**: Eastern Arabic numerals (`"١٢٣٤٥٦٧٨٩٠"`) render with glyphs preserved.
24. **`test_b24_arabic_ligatures_lam_alif`**: Text containing Lam-Alif ligatures (`"لا إله إلا الله"`) renders connected glyphs.
25. **`test_b25_zero_width_joiner_zwnj`**: Strings containing ZWJ (`\u200D`) and ZWNJ (`\u200C`) process cleanly.
26. **`test_b26_very_long_arabic_string`**: 1,000-character Arabic paragraph processes without buffer overflow.
27. **`test_b27_special_arabic_punctuation`**: Arabic punctuation marks (؛, ؟, ،) preserve reading order.
28. **`test_b28_arabic_tatweel_kashida`**: Arabic kashida elongation (`"تـــــدريــــب"`) renders correctly.
29. **`test_b29_arabic_text_bounding_box_tightness`**: Text bounding box tightly encloses rendered glyph extents.
30. **`test_b30_arabic_empty_and_single_character`**: Single character `"ع"` processes without index errors.

### Category 2.4: Disk Safety & Storage Boundary Limits (5 Tests)
31. **`test_b31_disk_exact_threshold_boundary`**: Free space exactly 1.0 GB ($1,073,741,824\text{ bytes}$) passes safety check.
32. **`test_b32_disk_one_byte_below_threshold`**: Free space $1.0\text{ GB} - 1\text{ byte}$ raises `IOError`.
33. **`test_b33_disk_one_byte_above_threshold`**: Free space $1.0\text{ GB} + 1\text{ byte}$ succeeds.
34. **`test_b34_disk_zero_free_space`**: Free space 0 bytes immediately raises `IOError`.
35. **`test_b35_dataset_collector_recovers_after_disk_freed`**: After simulated disk expansion, subsequent writes succeed without process restart.

### Category 2.5: Trainer Divergence & Extreme Hyperparameters (5 Tests)
36. **`test_b36_trainer_zero_samples_dataset`**: Dataset with 0 lines returns early with warning without raising unhandled exception.
37. **`test_b37_trainer_single_sample_dataset`**: Dataset with 1 sample handles 90/10 split safely without zero-division error.
38. **`test_b38_trainer_nan_loss_detection_cpu`**: Simulated NaN loss is caught by `assert not torch.isnan(loss)`.
39. **`test_b39_trainer_infinite_loss_detection`**: Simulated Inf loss is caught by divergence assertion.
40. **`test_b40_trainer_max_steps_one`**: `max_steps=1` terminates training loop strictly after step 1.

---

## 6. Cross-Feature Integration (Tier 3: 10 Tests)

Tier 3 validates pairwise module interactions and dataflow integrity across the pipeline:

1. **`test_p1_explorer_harvester_to_collector_to_validator`**: Windows 11 Explorer task output is saved by collector and validated by validator with 100% pass rate.
2. **`test_p2_youtube_harvester_to_collector_to_validator`**: YouTube harvester output is saved by collector and validated by validator, verifying image file existence and token format.
3. **`test_p3_arabic_rtl_synthesizer_to_collector_to_validator`**: Arabic RTL output flows through collector and validator, verifying UTF-8 encoding integrity.
4. **`test_p4_collector_to_training_dataset_loader`**: Collector JSONL and images load directly into `FlorenceLocalDataset` and PyTorch DataLoader, verifying tensor shapes `[1, seq_len]`, `[1, 3, 768, 768]`, `[1, label_len]`.
5. **`test_p5_validator_filtering_before_training`**: Validator identifies and filters out corrupted line; trainer proceeds cleanly on certified valid subset.
6. **`test_p6_collector_disk_safety_triggers_before_image_write`**: Disk safety breach raises `IOError` before saving image to disk, leaving zero orphan files.
7. **`test_p7_trainer_to_checkpoint_and_reloading`**: `train_local` writes checkpoint to disk; `AutoModelForCausalLM.from_pretrained` reloads checkpoint successfully.
8. **`test_p8_concurrent_harvesters_to_single_collector`**: 3 concurrent worker threads writing 10 steps each to single collector produce 30 valid lines without JSON corruption.
9. **`test_p9_harvester_step_to_agent_act_trajectory_consistency`**: Completed trajectory JSON step count matches entries in `dataset_agent_act.jsonl`.
10. **`test_p10_full_pipeline_harvest_validate_train_checkpoint`**: Complete sequential 4-stage pipeline execution (harvest $\to$ validate $\to$ train $\to$ checkpoint reload).

---

## 7. Real-World Application Scenarios (Tier 4: 6 Scenarios)

Tier 4 tests validate end-to-end production lifecycles and recovery workflows:

1. **`test_s1_multi_agent_continual_harvesting_cycle`**: Multi-agent concurrent cycle harvesting 15 diverse samples across Explorer, YouTube, and Dialog domains; verifies stats and images.
2. **`test_s2_incremental_training_with_checkpoint_chaining`**: Continual learning across 2 successive cycles (Cycle 1 $\to$ Checkpoint A $\to$ Cycle 2 fine-tuning from Checkpoint A $\to$ Checkpoint B); verifies weight progression.
3. **`test_s3_disk_exhaustion_protection_and_graceful_halt`**: Sustained harvesting until simulated disk drop below 1.0 GB; verifies safe halting, zero data loss, and trainer refusal.
4. **`test_s4_corrupted_dataset_recovery_and_resumption`**: Injected mid-write crash (truncated JSONL line + missing image); recovery mechanism quarantines corrupted lines and resumes training.
5. **`test_s5_multi_domain_stress_and_concurrency`**: Rapid interleaved generation of 30 tasks across threads under tight memory limits; verifies thread safety and zero memory leaks.
6. **`test_s6_e2e_forensic_audit_and_zero_fabrication`**: Comprehensive forensic audit: distinct image SHA256 hashes, exact line count alignment, non-empty `pytorch_model.bin` (> 400MB), non-mocked floating-point loss values.

---

## 8. Test Runner & CI/CD Gate Policies

### Master Execution Command
```powershell
pytest tests/e2e -v
```

### Selective Tier Execution
```powershell
# Tier 1 Feature Coverage (40 tests)
pytest tests/e2e/test_tier1_features.py -v

# Tier 2 Boundary & Extreme Conditions (40 tests)
pytest tests/e2e/test_tier2_boundaries.py -v

# Tier 3 Cross-Feature Integration (10 tests)
pytest tests/e2e/test_tier3_combinations.py -v

# Tier 4 Real-World Scenarios (6 scenarios)
pytest tests/e2e/test_tier4_scenarios.py -v

# Fast Suite (excluding PyTorch training loops)
pytest tests/e2e -m "not slow" -v
```

### Quality Gate Pass Criteria
- **Total Test Count**: Exactly 96 tests across Tiers 1–4.
- **Pass Rate**: 100% required (0 failures, 0 errors).
- **Execution Budget**: Fast suite $< 30\text{ seconds}$; full suite (including CPU training steps) $< 4\text{ minutes}$.
- **Artifact Verification**: Any failure prevents merging or deployment.

---

## Appendix: Dual Harness Architecture

CPower maintains two specialized test harnesses:
1. **Pure C11 Core Harness (`tests/test_runner.c`)**: Validates low-level Win32 IPC, shared memory rings, and Direct3D/DXGI micro-region capture via direct Windows APIs.
2. **Python Multimodal E2E Harness (`tests/e2e/`)**: Validates multi-agent UI harvesting, coordinate normalization, dataset schema integrity, Florence-2 continual learning loops, and storage safety guards as specified herein.
