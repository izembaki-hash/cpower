"""
Reconciles dataset_florence2.jsonl, dataset_agent_act.jsonl, and stats.json on E:\\cpower_training_data:
  1. Creates safety backups (.bak).
  2. Removes orphan line 61 (traj_1794616824_9c5cea_00) in dataset_florence2.jsonl.
  3. Aligns swapped lines 69/70 in dataset_agent_act.jsonl to match Florence order.
  4. Re-quantizes all Florence-2 ground truth tokens with floor quantization + non-degenerate repair.
  5. Repairs null click in row 151 of dataset_agent_act.jsonl with center coordinates.
  6. Synchronizes stats.json (total_steps and florence_grounding_samples).
"""

import json
from pathlib import Path
import shutil
import time

DATASET_DIR = Path(r"E:\cpower_training_data")
FLORENCE_PATH = DATASET_DIR / "dataset_florence2.jsonl"
ACT_PATH = DATASET_DIR / "dataset_agent_act.jsonl"
STATS_PATH = DATASET_DIR / "stats.json"

# 0. Create safety backups
shutil.copy2(FLORENCE_PATH, DATASET_DIR / "dataset_florence2.jsonl.bak")
shutil.copy2(ACT_PATH, DATASET_DIR / "dataset_agent_act.jsonl.bak")
shutil.copy2(STATS_PATH, DATASET_DIR / "stats.json.bak")
print("Backups created successfully.")

# Read existing records
with open(FLORENCE_PATH, "r", encoding="utf-8") as f:
    florence_records = [json.loads(line) for line in f if line.strip()]

with open(ACT_PATH, "r", encoding="utf-8") as f:
    act_records = [json.loads(line) for line in f if line.strip()]

print(f"Initial counts: Florence={len(florence_records)}, Act={len(act_records)}")

# 1. Remove orphan line 61 (0-indexed 60)
reconciled_florence = [r for idx, r in enumerate(florence_records) if idx != 60]
reconciled_act = list(act_records)

# 2. Align swapped lines 69 & 70 (0-indexed) in Act to match Florence
if reconciled_act[69]["image"] != reconciled_florence[69]["image"]:
    print(f"Swapping Act[69] and Act[70]")
    reconciled_act[69], reconciled_act[70] = reconciled_act[70], reconciled_act[69]

# Verify strict 1-to-1 image alignment
for i in range(len(reconciled_florence)):
    assert reconciled_florence[i]["image"] == reconciled_act[i]["image"], (
        f"Image mismatch at index {i}: {reconciled_florence[i]['image']} vs {reconciled_act[i]['image']}"
    )

# 3. Re-quantize Florence records with floor-based non-degenerate tokens
for entry in reconciled_florence:
    w, h = entry["image_size"]
    x1, y1, x2, y2 = entry["bbox_px"]
    phrase = entry["suffix"]

    raw_nx1 = int(1000.0 * x1 / max(1, w))
    raw_ny1 = int(1000.0 * y1 / max(1, h))
    raw_nx2 = int(1000.0 * x2 / max(1, w))
    raw_ny2 = int(1000.0 * y2 / max(1, h))

    nx1 = max(0, min(998, raw_nx1))
    nx2 = max(nx1 + 1, min(999, raw_nx2))
    ny1 = max(0, min(998, raw_ny1))
    ny2 = max(ny1 + 1, min(999, raw_ny2))

    entry["target"] = f"{phrase}<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"

# 4. Repair null click in Act
florence_map = {e["image"]: e for e in reconciled_florence}
repaired_clicks = 0
for act in reconciled_act:
    if act.get("action", {}).get("type") in ("click", "double_click", "right_click"):
        if act["action"].get("click") is None:
            f_match = florence_map[act["image"]]
            bx = f_match["bbox_px"]
            act["action"]["click"] = [(bx[0] + bx[2]) // 2, (bx[1] + bx[3]) // 2]
            repaired_clicks += 1
print(f"Repaired clicks count: {repaired_clicks}")

# 5. Overwrite JSONL files with reconciled data
with open(FLORENCE_PATH, "w", encoding="utf-8") as f:
    for r in reconciled_florence:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")

with open(ACT_PATH, "w", encoding="utf-8") as f:
    for r in reconciled_act:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")

# 6. Update stats.json
if STATS_PATH.exists():
    stats = json.loads(STATS_PATH.read_text(encoding="utf-8"))
    stats["total_steps"] = len(reconciled_florence)
    stats["florence_grounding_samples"] = len(reconciled_florence)
    stats["last_updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    STATS_PATH.write_text(json.dumps(stats, indent=2), encoding="utf-8")

print(f"Dataset reconciled successfully: Florence={len(reconciled_florence)}, Act={len(reconciled_act)}, stats={len(reconciled_florence)}")
