"""
CPower Autonomous Trajectory & Dataset Collector (clients/python/cpower_dataset_collector.py).

Harvests multimodal computer-use trajectories from live CPower agent executions:
  1. Screenshots before and after each action.
  2. Sub-pixel bounding box grounding labels for Florence-2 fine-tuning (<CAPTION_TO_PHRASE_GROUNDING>).
  3. Action sequences and state transitions for ReAct agent policy training.
  4. Auto-manages storage on high-capacity drives (E: > C:) with strict disk space guards.
"""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import re
import shutil
import threading
import time
from typing import Any, Dict, List, Optional, Tuple, Union
import uuid

from PIL import Image


class CPowerDatasetCollector:
    """
    Production-grade harvester for offline multimodal UI datasets and agent trajectories.
    Generates training-ready pairs for Florence-2 LoRA fine-tuning and ReAct policy models.
    """

    def __init__(self, base_dir: Optional[Union[str, Path]] = None, min_free_gb: float = 1.0):
        self._lock = threading.RLock()
        self.min_free_bytes = int(min_free_gb * 1024 * 1024 * 1024)
        self.base_dir = self._resolve_storage_directory(base_dir)
        self.images_dir = self.base_dir / "images"
        self.trajectories_dir = self.base_dir / "trajectories"

        # Create directory hierarchy
        self.images_dir.mkdir(parents=True, exist_ok=True)
        self.trajectories_dir.mkdir(parents=True, exist_ok=True)

        self.florence_jsonl_path = self.base_dir / "dataset_florence2.jsonl"
        self.agent_act_jsonl_path = self.base_dir / "dataset_agent_act.jsonl"
        self.stats_path = self.base_dir / "stats.json"

        # Active trajectories in memory: traj_id -> state dict
        self._active_trajectories: Dict[str, Dict[str, Any]] = {}
        self._ensure_stats_file()

    @staticmethod
    def _is_drive_h(path: Union[str, Path]) -> bool:
        """
        Determines if a given path targets Drive H: under any representation:
          - Standard drive paths: H:\..., h:/...
          - Drive-relative paths: H:..., h:...
          - Extended-length syntax: \\?\H:\..., //?/H:/...
          - Win32 device namespace: \\.\H:\..., //./H:/...
          - NT Object Manager syntax: \??\H:\...
          - UNC administrative shares: \\server\H$\...
          - Relative paths resolving to Drive H:
        """
        raw_str = os.fspath(path).strip()
        raw_norm = raw_str.replace("/", "\\").upper()

        # 1. Direct lexical inspection of raw input
        if (
            raw_norm.startswith("H:")
            or raw_norm.startswith("\\\\?\\H:")
            or raw_norm.startswith("\\\\.\\H:")
            or raw_norm.startswith("\\??\\H:")
            or "\\H:" in raw_norm
            or bool(re.search(r"(?:^|[\\/])H:", raw_str, re.IGNORECASE))
            or bool(re.search(r"[\\/]H\$(?:[\\/]|$)", raw_str, re.IGNORECASE))
        ):
            return True

        # 2. Canonicalized and resolved path inspection
        try:
            p = Path(path).resolve()
            res_str = os.fspath(p).strip()
            res_norm = res_str.replace("/", "\\").upper()
            drive_norm = p.drive.replace("/", "\\").upper()
            split_drive, _ = os.path.splitdrive(res_norm)

            if (
                drive_norm.endswith("H:")
                or "H:" in drive_norm
                or split_drive.upper().endswith("H:")
                or res_norm.startswith("H:")
                or res_norm.startswith("\\\\?\\H:")
                or res_norm.startswith("\\\\.\\H:")
                or res_norm.startswith("\\??\\H:")
                or "\\H:" in res_norm
                or bool(re.search(r"(?:^|[\\/])H:", res_str, re.IGNORECASE))
                or bool(re.search(r"[\\/]H\$(?:[\\/]|$)", res_str, re.IGNORECASE))
            ):
                return True
        except Exception:
            pass

        return False

    def _resolve_storage_directory(self, preferred_path: Optional[Union[str, Path]]) -> Path:
        """Resolves the safest storage path, favoring large secondary drives (E: > C:)."""
        if preferred_path:
            if self._is_drive_h(preferred_path):
                raise PermissionError(
                    f"Drive H: storage quarantine violation: {preferred_path} is on Drive H:. "
                    "Storing datasets on Drive H: is strictly forbidden to protect working directory headroom."
                )
            p = Path(preferred_path).resolve()
            p.mkdir(parents=True, exist_ok=True)
            return p

        # Check Drive E: first (typically large storage drive on Windows)
        drive_e = Path("E:/cpower_training_data")
        try:
            drive_e_root = Path("E:/")
            if drive_e_root.is_dir() and not self._is_drive_h(drive_e):
                usage = shutil.disk_usage(str(drive_e_root))
                if usage.free > self.min_free_bytes:
                    drive_e.mkdir(parents=True, exist_ok=True)
                    return drive_e
        except Exception:
            pass

        # Fallback to User Profile directory on C:
        user_home = Path.home()
        fallback = user_home / ".cpower_dataset"
        if self._is_drive_h(fallback):
            raise PermissionError("Fallback directory cannot be on quarantined Drive H:")
        fallback.mkdir(parents=True, exist_ok=True)
        return fallback

    def _ensure_stats_file(self) -> None:
        if not self.stats_path.exists():
            initial_stats = {
                "version": "1.0",
                "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "total_trajectories": 0,
                "total_steps": 0,
                "successful_trajectories": 0,
                "florence_grounding_samples": 0,
            }
            self.stats_path.write_text(json.dumps(initial_stats, indent=2), encoding="utf-8")

    def _check_disk_space(self) -> bool:
        """Verifies that storage has not fallen below the safety floor."""
        try:
            usage = shutil.disk_usage(str(self.base_dir))
            return usage.free >= self.min_free_bytes
        except Exception:
            return True

    def start_trajectory(self, goal: str, window_title: str = "") -> str:
        """Initializes a new recording session for a multi-step user goal."""
        with self._lock:
            traj_id = f"traj_{int(time.time())}_{uuid.uuid4().hex[:6]}"
            self._active_trajectories[traj_id] = {
                "trajectory_id": traj_id,
                "goal": goal,
                "start_time": time.strftime("%Y-%m-%d %H:%M:%S"),
                "initial_window": window_title,
                "steps": [],
                "status": "in_progress",
            }
            return traj_id

    def record_step(
        self,
        trajectory_id: str,
        image: Union[Image.Image, Any],
        action_type: str,
        phrase: str,
        bbox: Tuple[int, int, int, int],
        click_coords: Optional[Tuple[int, int]] = None,
        text_input: str = "",
        keys: str = "",
        window_title: str = "",
        metadata: Optional[Dict[str, Any]] = None,
        normalized_bbox: Optional[Tuple[int, int, int, int]] = None,
    ) -> Dict[str, Any]:
        """
        Records a single decision step:
          - Saves screenshot.
          - Normalizes bounding box for Florence-2 fine-tuning (<loc_x1><loc_y1><loc_x2><loc_y2>).
          - Appends to dataset_florence2.jsonl and dataset_agent_act.jsonl with thread-safety and explicit flush.
        """
        with self._lock:
            if not self._check_disk_space():
                raise IOError(f"Disk space below safety threshold on {self.base_dir}")

            traj = self._active_trajectories.get(trajectory_id)
            step_idx = len(traj["steps"]) if traj else 0

            # Ensure image is PIL Image
            if not isinstance(image, Image.Image):
                if hasattr(image, "convert"):
                    image = image.convert("RGB")
                else:
                    raise TypeError(f"Expected PIL Image, got {type(image)}")

            img_w, img_h = image.size
            img_filename = f"{trajectory_id}_step_{step_idx:02d}.jpg"
            img_abs_path = self.images_dir / img_filename
            img_rel_path = f"images/{img_filename}"

            # Save image compressed to preserve storage
            image.save(img_abs_path, format="JPEG", quality=88, optimize=True)

            # Cartesian coordinate bounds sanitization
            x1, y1, x2, y2 = bbox
            if x1 > x2:
                x1, x2 = x2, x1
            if y1 > y2:
                y1, y2 = y2, y1
            x1 = max(0, min(img_w, int(round(x1))))
            y1 = max(0, min(img_h, int(round(y1))))
            x2 = max(0, min(img_w, int(round(x2))))
            y2 = max(0, min(img_h, int(round(y2))))

            # Coordinate normalization to Florence-2 [0..999] space using floor quantization
            if normalized_bbox is not None:
                raw_nx1, raw_ny1, raw_nx2, raw_ny2 = normalized_bbox
                raw_nx1 = int(raw_nx1)
                raw_ny1 = int(raw_ny1)
                raw_nx2 = int(raw_nx2)
                raw_ny2 = int(raw_ny2)
                if raw_nx1 > raw_nx2:
                    raw_nx1, raw_nx2 = raw_nx2, raw_nx1
                if raw_ny1 > raw_ny2:
                    raw_ny1, raw_ny2 = raw_ny2, raw_ny1
                nx1 = max(0, min(998, raw_nx1))
                nx2 = max(nx1 + 1, min(999, raw_nx2))
                ny1 = max(0, min(998, raw_ny1))
                ny2 = max(ny1 + 1, min(999, raw_ny2))
            else:
                raw_nx1 = int(1000.0 * x1 / max(1, img_w))
                raw_ny1 = int(1000.0 * y1 / max(1, img_h))
                raw_nx2 = int(1000.0 * x2 / max(1, img_w))
                raw_ny2 = int(1000.0 * y2 / max(1, img_h))

                nx1 = max(0, min(998, raw_nx1))
                nx2 = max(nx1 + 1, min(999, raw_nx2))
                ny1 = max(0, min(998, raw_ny1))
                ny2 = max(ny1 + 1, min(999, raw_ny2))

            # Format Florence-2 ground truth token string
            florence_target = f"{phrase}<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"

            # Enforce non-null click coordinates for click actions
            click_actions = ("click", "double_click", "right_click")
            if action_type in click_actions:
                if click_coords is None:
                    cx = (x1 + x2) // 2
                    cy = (y1 + y2) // 2
                    click_coords = (cx, cy)
                else:
                    cx = max(x1, min(x2, int(click_coords[0])))
                    cy = max(y1, min(y2, int(click_coords[1])))
                    click_coords = (cx, cy)

            step_record = {
                "trajectory_id": trajectory_id,
                "step_index": step_idx,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "image_path": img_rel_path,
                "image_size": [img_w, img_h],
                "action_type": action_type,
                "phrase": phrase,
                "bbox_px": [x1, y1, x2, y2],
                "normalized_bbox": [nx1, ny1, nx2, ny2],
                "click_coords": list(click_coords) if click_coords else None,
                "text_input": text_input,
                "keys": keys,
                "window_title": window_title,
                "florence_sample": {
                    "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
                    "suffix": phrase,
                    "target": florence_target,
                },
                "metadata": metadata or {},
            }

            if traj:
                traj["steps"].append(step_record)

            # Append to dataset_florence2.jsonl with buffer flush
            florence_entry = {
                "id": f"{trajectory_id}_{step_idx:02d}",
                "image": img_rel_path,
                "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
                "suffix": phrase,
                "target": florence_target,
                "bbox_px": [x1, y1, x2, y2],
                "image_size": [img_w, img_h],
            }
            with open(self.florence_jsonl_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(florence_entry, ensure_ascii=False) + "\n")
                f.flush()

            # Append to dataset_agent_act.jsonl with buffer flush
            act_entry = {
                "trajectory_id": trajectory_id,
                "step_index": step_idx,
                "goal": traj["goal"] if traj else "",
                "image": img_rel_path,
                "action": {
                    "type": action_type,
                    "phrase": phrase,
                    "click": list(click_coords) if click_coords else None,
                    "text": text_input,
                    "keys": keys,
                },
                "window": window_title,
            }
            with open(self.agent_act_jsonl_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(act_entry, ensure_ascii=False) + "\n")
                f.flush()

            self._increment_stats(steps=1, samples=1)
            return step_record

    def complete_trajectory(
        self,
        trajectory_id: str,
        succeeded: bool = True,
        notes: str = "",
    ) -> Path:
        """Finalizes trajectory, saves full JSON file, and updates metrics."""
        with self._lock:
            traj = self._active_trajectories.pop(trajectory_id, None)
            if not traj:
                traj = {
                    "trajectory_id": trajectory_id,
                    "goal": "Unspecified",
                    "start_time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "steps": [],
                }

            traj["end_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
            traj["succeeded"] = succeeded
            traj["notes"] = notes
            traj["step_count"] = len(traj["steps"])

            out_path = self.trajectories_dir / f"{trajectory_id}.json"
            out_path.write_text(json.dumps(traj, indent=2, ensure_ascii=False), encoding="utf-8")

            self._increment_stats(trajectories=1, successful_trajectories=1 if succeeded else 0)
            return out_path

    def _increment_stats(
        self,
        trajectories: int = 0,
        steps: int = 0,
        successful_trajectories: int = 0,
        samples: int = 0,
    ) -> None:
        with self._lock:
            try:
                stats = json.loads(self.stats_path.read_text(encoding="utf-8"))
                stats["total_trajectories"] = stats.get("total_trajectories", 0) + trajectories
                stats["total_steps"] = stats.get("total_steps", 0) + steps
                stats["successful_trajectories"] = stats.get("successful_trajectories", 0) + successful_trajectories
                stats["florence_grounding_samples"] = stats.get("florence_grounding_samples", 0) + samples
                stats["last_updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
                self.stats_path.write_text(json.dumps(stats, indent=2), encoding="utf-8")
            except Exception:
                pass

    def get_dataset_summary(self) -> Dict[str, Any]:
        """Returns storage metrics, counts, and directory paths."""
        with self._lock:
            usage = shutil.disk_usage(str(self.base_dir))
            stats = {}
            if self.stats_path.exists():
                try:
                    stats = json.loads(self.stats_path.read_text(encoding="utf-8"))
                except Exception:
                    pass

            return {
                "storage_root": str(self.base_dir),
                "free_space_gb": round(usage.free / (1024**3), 2),
                "total_trajectories": stats.get("total_trajectories", 0),
                "total_steps": stats.get("total_steps", 0),
                "florence_samples": stats.get("florence_grounding_samples", 0),
                "florence_jsonl": str(self.florence_jsonl_path),
                "agent_act_jsonl": str(self.agent_act_jsonl_path),
            }

    def generate_training_loader_script(self) -> Path:
        """
        Emits a standalone Python dataset loader script directly into the dataset root,
        ready for PyTorch / HuggingFace LoRA fine-tuning workflows.
        """
        loader_code = '''"""
Autogenerated Dataset Loader for CPower Local LoRA Fine-Tuning.
Loads dataset_florence2.jsonl pairs into PyTorch Tensors.
"""
import json
from pathlib import Path
from PIL import Image
from torch.utils.data import Dataset

class CPowerFlorenceDataset(Dataset):
    def __init__(self, root_dir: str, processor):
        self.root = Path(root_dir)
        self.processor = processor
        self.samples = []
        jsonl_path = self.root / "dataset_florence2.jsonl"
        if jsonl_path.exists():
            with open(jsonl_path, "r", encoding="utf-8") as f:
                for line in f:
                    if line.strip():
                        self.samples.append(json.loads(line))

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):
        item = self.samples[idx]
        img_path = self.root / item["image"]
        image = Image.open(img_path).convert("RGB")
        prompt = f"{item['prefix']} {item['suffix']}"
        target = item["target"]
        inputs = self.processor(text=prompt, images=image, return_tensors="pt")
        labels = self.processor.tokenizer(text=target, return_tensors="pt")["input_ids"]
        return {
            "input_ids": inputs["input_ids"].squeeze(0),
            "pixel_values": inputs["pixel_values"].squeeze(0),
            "labels": labels.squeeze(0),
        }
'''
        out_script = self.base_dir / "florence2_dataset_loader.py"
        out_script.write_text(loader_code, encoding="utf-8")
        return out_script
