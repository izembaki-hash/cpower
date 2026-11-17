# Project: CPower Universal UI Pattern Dataset & Continual Florence-2 Adaptation

## Architecture
- **Perception & Live UI Capture**:
  - CPower C11 daemon (`bin/cpower_uiaccess.exe`, PID 4300) with low-latency lock-free SHM transport (`clients/python/cpower_client.py`), Per-Monitor v2 DPI awareness, MTA COM `IUIAutomation` tree traversal, and DXGI micro-region capture.
- **Multimodal Universal UI Synthesizers**:
  - **Universal Web Navigation Engine**: Headless Chrome via Playwright (`channel="chrome"`, `headless=True`) with 8 major domain archetypes (Search Engines, Encyclopedias, Code Platforms, Tech Docs, News Feeds, E-Commerce, Admin Portals, Form Services). Multi-target grounding (5-15 targets per image) with cookie consent, infinite scroll, and multi-tab patterns.
  - **Web Data Extraction Harvester**: Dedicated structured scrapers for HTML tables (headers, rows, pagination), article feeds, code snippets (syntax badges, copy triggers), and search result listings (SERPs).
  - **Complete Windows 11 OS Harvester**: Hybrid architecture combining Live Read-Only UIA extraction for authentic shell surfaces (Start menu, Taskbar, System Tray, Quick Settings) and 14 parametric PIL archetypes (Windows Settings System/Apps/Personalization/Network, File Explorer context menus & inline rename, Calculator, Notepad, Task Manager, Terminal, Snap layouts, border resizing, drag & drop) with Dark/Light mode and Arabic RTL.
  - **Universal Action Patterns & ReAct Engine**: Universal action primitives (Search, Form Fill, Filter/Sort, Download/Save, Popup Dismissal, Tab Switching, Multi-Page Traversal, Scrolling, Drag-and-Drop) mapped to full ReAct quadruplets: `(Thought, Action, Target_Coordinates, Outcome)`.
- **Dataset Collection & Integrity Validation**:
  - `cpower_dataset_collector.py` & `cpower_dataset_validator.py`: Normalizes coordinates to `[0..999]`, enforces non-degeneracy (`x2 >= x1 + 1, y2 >= y1 + 1`), sub-pixel dequantization precision (< 1.0px), click containment, and strict `E:\cpower_training_data` storage quarantine (Drive `H:` >= 2.0 GB headroom).
- **Local Florence-2 Continuous Training Loop**:
  - `cpower_trainer.py` & `cpower_continual_loop.py`: Parameter-efficient adaptation of Microsoft Florence-2 base (`C:\Users\Administrator\.models\florence-2-base`), DaViT vision tower freezing, unpadded aspect ratio alignment, AdamW optimization, train/val loss monitoring, NaN divergence protection, and verified checkpointing to `E:\cpower_training_data\checkpoints\cpower_florence2_adapted`.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Windows 11 Controls Harvester | Programmatic automated harvesting of File Explorer, Settings, and desktop controls | M1 | Completed |
| 2 | Web & YouTube Media Harvester | Programmatic automated harvesting of YouTube search, playback controls, and video cards | M1 | Completed |
| 3 | Rich Arabic RTL Synthesizer | Programmatic generation of Arabic RTL UI layouts using bi-directional and cursive reshaping | M1 | Completed |
| 4 | Coordinate Normalization & Tokenizer Align | Enforce strictly `[xmin, ymin, xmax, ymax]` tokens clamped to `[0, 999]` (`<loc_0>` to `<loc_999>`) | M2 | Completed |
| 5 | JSONL Schema & Image Integrity Validator | Automated validation of `dataset_florence2.jsonl` and `dataset_agent_act.jsonl` | M2 | Completed |
| 6 | Training Pipeline Enhancement | Validation split (90/10), train & val loss logging, NaN divergence protection | M3 | Completed |
| 7 | Checkpoint Verification & Disk Safety | Enforce checkpoint saving to `E:\cpower_training_data\checkpoints\cpower_florence2_adapted` with 1.0 GB disk safety floor | M3 | Completed |
| 8 | Continuous Multi-Agent Training Execution | Run concurrent harvesting workers alongside continual training cycles on local compute | M4 | Completed |
| 9 | Comprehensive E2E Verification & Forensic Audit | Validate 100% acceptance criteria pass, dataset integrity, and genuine non-fabricated execution | M4 | Completed |
| 10 | 8-Domain Universal Web Harvester | Implement 8 distinct web domain templates (Search, Wiki, Code, Docs, Feeds, E-commerce, Portals, Forms) with Playwright & synthetic DOM | M5 | Survey |
| 11 | Multi-Target Grounding per Screenshot | Extract and record 5-15 ground truth element bounding boxes per rendered web frame to scale training volume 10x without disk inflation | M5 | Survey |
| 12 | Web Obstruction & Multi-Tab Patterns | Synthesize Cookie consent banners, modal dismissals, infinite scroll triggers, and tab-strip navigation | M5 | Survey |
| 13 | Structured Table Data Extractor | Harvester for tabular data (table boundaries, column headers, data cells, next/previous pagination) | M6 | Survey |
| 14 | Feed & Article Content Extractor | Harvester for structured lists, news feeds, article paragraphs, author/timestamp metadata, and "Load More" | M6 | Survey |
| 15 | Code Snippet & SERP Extractor | Harvester for code block containers, language badges, copy buttons, search snippets, and ranking cards | M6 | Survey |
| 16 | 14-Archetype Win11 Controls Synthesizer | Expand parametric PIL engine to Settings (System/Apps/Personalization), File Explorer context menus & rename, Calculator, Notepad, Task Manager, Terminal, Snap layouts | M7 | Survey |
| 17 | Live Shell Read-Only Harvester | Background UIA tree walker for authentic Windows shell (Start menu, Taskbar, System Tray, Quick Settings) | M7 | Survey |
| 18 | Multi-Monitor Virtual Offset & Theme Engine | Coordinate translation for secondary monitor negative coordinates `[-1366..0]`, Per-Monitor DPI scaling, and Dark/Light themes | M7 | Survey |
| 19 | Universal Action Patterns Engine | Implement Search, Form Fill, Filter/Sort, Download/Save, Popup Dismissal, Tab Switching, Multi-Page Traversal, Scrolling, Drag-and-Drop | M8 | Survey |
| 20 | Full ReAct Quadruplet Schema | Upgrade `dataset_agent_act.jsonl` to record explicit `(Thought, Action, Target_Coordinates, Outcome)` decision pairs | M8 | Survey |
| 21 | Dataset Non-Degeneracy & Anomaly Repair | Enforce `x2 >= x1 + 1, y2 >= y1 + 1` in collector; sanitize 3 historical collapsed coordinate records on lines 1402, 1408, 1412 | M8 | Survey |
| 22 | Mass Dataset Harvesting Campaign | Execute concurrent multi-domain harvesters to generate thousands of validated, diverse UI grounding and action samples on `E:\cpower_training_data` | M9 | Survey |
| 23 | Florence-2 Aspect-Ratio Fix & Training | Remove vertical aspect distortion (`pad_to_square` target token mismatch) and execute local Florence-2 training loop | M9 | Survey |
| 24 | E2E Grounding Validation & Forensic Audit | Validate 100% test pass on dataset, verify model checkpoint, and perform non-skippable forensic integrity audit | M9 | Survey |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Multi-Domain Harvesters | Implement Windows 11, Web/YouTube, and Arabic RTL non-mouse automated harvesters | none | DONE |
| M2 | Dataset Validator & Integrity Filter | Implement automated validator enforcing schemas, normalized coordinates [0..999], and image integrity | M1 | DONE |
| M3 | Continuous Training & Checkpointing Engine | Update `cpower_trainer.py` with validation split (90/10), train/val loss logging, NaN checks, and checkpointing | M2 | DONE |
| M4 | Continual Multi-Agent Pipeline & Shadow Mode | Execute continual training verification, live phrase grounding test, and prototype passive human demonstration learner | M1, M2, M3 | DONE |
| M5 | Universal Web Navigation & 8-Domain Harvester | Implement 8 web property archetypes (Search, Wiki, Code, Docs, Feeds, E-commerce, Portals, Forms), multi-target grounding (5-15 targets/frame), cookie consent & multi-tab | M4 | IN_PROGRESS |
| M6 | Web Data Extraction & Harvesting Synthesizer | Implement structured scrapers for HTML tables (headers, rows, pagination), feeds, code snippets, and SERP result cards | M5 | PLANNED |
| M7 | Complete Windows OS Control Harvester | Expand to 14 Win11 archetypes (Settings, Explorer context menus/rename, Taskbar, Start, System Tray, Calculator, Notepad, Task Manager, Terminal, Snap layouts, virtual screen offset, Dark/Light modes) | M4 | PLANNED |
| M8 | Universal Action Patterns & Full ReAct Engine | Implement universal action primitives, full ReAct quadruplets `(Thought, Action, Coordinates, Outcome)`, collector non-degeneracy fix, and repair lines 1402/1408/1412 | M5, M6, M7 | PLANNED |
| M9 | Mass Generation, Continual Training & Audit | Massively harvest diverse datasets on `E:\cpower_training_data`, train Florence-2 with aspect ratio fix, verify checkpoint, and run Forensic Integrity Audit | M8 | PLANNED |

## Interface Contracts
### Web & OS Harvesters ↔ Dataset Collector
- Function: `collector.record_step(goal, window_title, image, action_dict, florence_grounding, thought=None, outcome=None)`
- Input Specifications:
  - `image`: PIL.Image.Image (RGB)
  - `action_dict`: `{"type": str, "phrase": str, "click": [int, int], "text": str, "keys": str}`
  - `florence_grounding`: List of dicts or single dict: `{"prefix": "<CAPTION_TO_PHRASE_GROUNDING>", "suffix": str, "bbox": [x1, y1, x2, y2]}`
  - `thought`: Optional[str] reasoning thought prior to action
  - `outcome`: Optional[str] post-action verification observation
- Invariant: Bounding boxes must have `0 <= x1 < x2 <= W` and `0 <= y1 < y2 <= H`. Drive `H:` is strictly forbidden.

### Dataset Validator ↔ JSONL Files
- Function: `validate_dataset(dataset_dir) -> Tuple[bool, List[str]]`
- Rules:
  - Verifies every line in `dataset_florence2.jsonl` has valid JSON, matching image file in `images/`, valid `<loc_...>` tokens in `[0..999]`, non-degenerate positive bounding boxes (`x2 >= x1 + 1, y2 >= y1 + 1`), and dequantization error < 1.0px.
  - Verifies every line in `dataset_agent_act.jsonl` matches trajectory and click falls within target bbox.

### Continual Trainer ↔ Florence-2 Model
- Function: `train_local(dataset_root, epochs=1, lr=1e-4, device_name="cpu", max_steps=15) -> Dict[str, float]`
- Rules:
  - Feeds unpadded images matching normalized `[0..999]` token coordinate space without aspect ratio distortion.
  - Freezes DaViT vision tower backbone, optimizing projection and language heads with AdamW.
  - Guard: `assert not torch.isnan(loss)` and `assert not torch.isinf(loss)`.
  - Checkpoint: Saves `pytorch_model.bin` and configs to `E:\cpower_training_data\checkpoints\cpower_florence2_adapted`.

## Code Layout
- `clients/python/cpower_dataset_collector.py` — Storage, trajectory recording, non-degeneracy, and disk safety logic
- `clients/python/cpower_multitask_harvester.py` — Multi-domain UI synthesis engine (Web 8 domains, Win11 14 archetypes, Arabic RTL)
- `clients/python/cpower_web_extractor.py` — Web data extraction harvesters (tables, feeds, code blocks, SERPs)
- `clients/python/cpower_pattern_engine.py` — Universal action patterns engine (Search, Navigation, Form Fill, Download, Popup, Filter, Tab, Traversal)
- `clients/python/cpower_trainer.py` — Local Florence-2 training loop with PEFT freezing and aspect ratio alignment
- `clients/python/cpower_continual_loop.py` — Multi-agent continuous harvesting and training orchestrator
- `clients/python/cpower_dataset_validator.py` — Dedicated dataset schema and integrity validator
- `tests/test_dataset_collector.py` — Collector unit tests
- `tests/test_multitask_harvester.py` — Harvester unit tests
- `tests/test_web_extractor.py` — Web extractor unit tests
- `tests/test_pattern_engine.py` — Universal action pattern unit tests
- `tests/test_vision_local.py` — Local Florence-2 inference and grounding verification
- `tests/test_e2e_continual_pipeline.py` — E2E validation test suite
