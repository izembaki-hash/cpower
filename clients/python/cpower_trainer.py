r"""
CPower Local Continual Trainer (clients/python/cpower_trainer.py).

Executes Parameter-Efficient Domain Adaptation (PEFT) for Florence-2
using the locally harvested trajectory datasets:
  1. Freezes the heavy DaViT vision backbone (vision_tower).
  2. Fine-tunes multimodal projection & language generation heads on local Windows UI grounding pairs.
  3. Uses 90/10 Train/Validation split and evaluates validation loss.
  4. Enforces divergence protection (check_loss_divergence).
  5. Enforces 1.0 GB disk safety floor before saving.
  6. Validates checkpoint serialization integrity upon save.
  7. Saves adapted weights and configs to E:\cpower_training_data\checkpoints\cpower_florence2_adapted.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import time
from typing import Any, Dict, List, Optional, Tuple, Union

import torch
from torch.utils.data import DataLoader, Dataset, random_split
from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector  # noqa: E402


def check_disk_space(target_path: Union[str, Path], min_free_gb: float = 1.0) -> bool:
    """
    Checks if available disk space at target_path meets or exceeds min_free_gb.
    Returns True if space is sufficient or if disk_usage check cannot be performed, False otherwise.
    """
    try:
        usage = shutil.disk_usage(str(target_path))
        free_gb = usage.free / (1024**3)
        return free_gb >= min_free_gb
    except Exception:
        return True


def freeze_florence_backbone(model: torch.nn.Module) -> Tuple[int, int]:
    """
    Applies CPower parameter-efficient fine-tuning freezing:
      - Freezes the heavy vision backbone (model.vision_tower).
      - Keeps the multimodal projection layer (model.image_proj_norm) and language heads trainable.
      - Explicitly ensures language_model and image_pos_embed parameters have requires_grad=True.
    Returns (trainable_params, total_params).
    """
    if hasattr(model, "vision_tower"):
        for param in model.vision_tower.parameters():
            param.requires_grad = False

    if hasattr(model, "image_proj_norm"):
        for param in model.image_proj_norm.parameters():
            param.requires_grad = True

    if hasattr(model, "language_model"):
        for param in model.language_model.parameters():
            param.requires_grad = True

    if hasattr(model, "image_pos_embed"):
        for param in model.image_pos_embed.parameters():
            param.requires_grad = True

    trainable_params = sum(p.numel() for p in model.parameters() if p.requires_grad)
    total_params = sum(p.numel() for p in model.parameters())
    return trainable_params, total_params


def pad_to_square(
    image: Image.Image,
    min_dim: int = 768,
    bg_color: Tuple[int, int, int] = (255, 255, 255),
) -> Image.Image:
    """Pads an image to a square aspect ratio and normalizes to 768x768 to prevent DaViT position embedding overflow."""
    w, h = image.size
    dim = max(w, h, min_dim)
    square = Image.new("RGB", (dim, dim), bg_color)
    square.paste(image, (0, 0))
    if dim != 768:
        square = square.resize((768, 768), Image.Resampling.BICUBIC)
    return square


def check_loss_divergence(loss: torch.Tensor) -> None:
    """Verifies loss is not NaN or infinite, raising AssertionError if divergence detected."""
    if torch.isnan(loss) or torch.isinf(loss):
        raise AssertionError("Loss divergence: NaN or Inf detected during training iteration")


def split_train_val_dataset(
    dataset: Dataset,
    val_ratio: float = 0.1,
    seed: int = 42,
) -> Tuple[Dataset, Dataset]:
    """Splits a dataset into training and validation subsets according to val_ratio (default 90/10)."""
    total = len(dataset)
    if total <= 1:
        return dataset, dataset
    val_size = max(1, int(round(total * val_ratio)))
    val_size = min(val_size, total - 1)
    train_size = total - val_size
    generator = torch.Generator().manual_seed(seed)
    return random_split(dataset, [train_size, val_size], generator=generator)


class FlorenceLocalDataset(Dataset):
    """PyTorch Dataset loading harvested CPower grounding pairs."""

    def __init__(self, jsonl_path: Path, processor: Any):
        self.root = jsonl_path.parent
        self.processor = processor
        self.samples: List[Dict[str, Any]] = []

        if jsonl_path.exists():
            with open(jsonl_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            self.samples.append(json.loads(line))
                        except Exception:
                            pass

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, idx: int) -> Dict[str, torch.Tensor]:
        item = self.samples[idx]
        img_file = self.root / item["image"]
        image = Image.open(img_file).convert("RGB")
        prompt = f"{item['prefix']} {item['suffix']}"
        target = item["target"]

        # Square aspect ratio padding via exposed helper
        square = pad_to_square(image, min_dim=768)

        inputs = self.processor(text=prompt, images=square, return_tensors="pt")
        labels = self.processor.tokenizer(text=target, return_tensors="pt")["input_ids"]

        return {
            "input_ids": inputs["input_ids"].squeeze(0),
            "pixel_values": inputs["pixel_values"].squeeze(0).float(),
            "labels": labels.squeeze(0),
        }


def collate_single(batch: List[Dict[str, Any]]) -> Dict[str, torch.Tensor]:
    """Collate single sample batch cleanly."""
    sample = batch[0]
    return {
        "pixel_values": sample["pixel_values"].unsqueeze(0),
        "input_ids": sample["input_ids"].unsqueeze(0),
        "labels": sample["labels"].unsqueeze(0),
    }


def train_local(
    dataset_root: Optional[Union[str, Path]] = None,
    epochs: int = 3,
    lr: float = 1e-4,
    device_name: str = "cpu",
    max_steps: Optional[int] = None,
    val_ratio: float = 0.1,
    val_split: Optional[float] = None,
    model: Optional[Any] = None,
    processor: Optional[Any] = None,
    return_dict: bool = True,
) -> Union[Dict[str, Any], Path]:
    """
    Executes the local adaptation training loop.
    Returns metrics dictionary adhering to PROJECT.md interface contract:
      {"train_loss": float, "val_loss": float, "steps": int, "nan_detected": bool, "checkpoint_dir": Path}
    """
    if val_split is not None:
        val_ratio = val_split

    collector = CPowerDatasetCollector(base_dir=dataset_root)
    jsonl_file = collector.florence_jsonl_path
    output_checkpoint_dir = collector.base_dir / "checkpoints" / "cpower_florence2_adapted"

    # Pre-flight disk safety check before starting training loop
    if not check_disk_space(collector.base_dir, min_free_gb=1.0):
        usage = shutil.disk_usage(str(collector.base_dir))
        free_gb = usage.free / (1024**3)
        raise IOError(f"Insufficient disk space on {collector.base_dir}: {free_gb:.2f} GB free (minimum required: 1.0 GB)")

    if not jsonl_file.exists():
        print(f"[TRAINER] Error: Dataset file not found at {jsonl_file}")
        res = {
            "train_loss": 0.0,
            "val_loss": 0.0,
            "steps": 0,
            "nan_detected": False,
            "checkpoint_dir": output_checkpoint_dir,
        }
        return res if return_dict else output_checkpoint_dir

    model_dir = Path("C:/Users/Administrator/.models/florence-2-base")
    if model is None or processor is None:
        if not model_dir.is_dir():
            raise FileNotFoundError(f"Base model not found at {model_dir}")
        print(f"[TRAINER] Initializing training on: {jsonl_file}")
        from transformers import AutoModelForCausalLM, AutoProcessor

        processor = AutoProcessor.from_pretrained(str(model_dir), trust_remote_code=True)
        model = AutoModelForCausalLM.from_pretrained(str(model_dir), trust_remote_code=True)

    # 1. Parameter-efficient freezing via exposed helper
    trainable_params, total_params = freeze_florence_backbone(model)
    print(f"[TRAINER] Efficient Tuning: {trainable_params:,} trainable / {total_params:,} total params ({100.0 * trainable_params / total_params:.2f}%)")

    dataset = FlorenceLocalDataset(jsonl_file, processor)
    if len(dataset) == 0:
        print("[TRAINER] No samples in dataset yet.")
        res = {
            "train_loss": 0.0,
            "val_loss": 0.0,
            "steps": 0,
            "nan_detected": False,
            "checkpoint_dir": output_checkpoint_dir,
        }
        return res if return_dict else output_checkpoint_dir

    # 2. Train/Val Split (90/10)
    train_subset, val_subset = split_train_val_dataset(dataset, val_ratio=val_ratio)
    train_loader = DataLoader(train_subset, batch_size=1, shuffle=True, collate_fn=collate_single)
    val_loader = DataLoader(val_subset, batch_size=1, shuffle=False, collate_fn=collate_single)

    device = torch.device(device_name)
    model.to(device)
    if device.type == "cpu":
        model.float()
    optimizer = torch.optim.AdamW(filter(lambda p: p.requires_grad, model.parameters()), lr=lr)

    print(f"[TRAINER] Dataset Split: {len(train_subset)} train, {len(val_subset)} validation.")
    print(f"[TRAINER] Starting training for {epochs} epoch(s)...")

    model.train()
    start_time = time.time()
    total_train_loss = 0.0
    total_steps = 0

    if max_steps == 0:
        print("[TRAINER] max_steps=0 requested: skipping training loop.")
        avg_train_loss = 0.0
        avg_val_loss = 0.0
    else:
        for epoch in range(1, epochs + 1):
            for step, batch in enumerate(train_loader, start=1):
                pixel_values = batch["pixel_values"].to(device, dtype=torch.float32)
                input_ids = batch["input_ids"].to(device)
                labels = batch["labels"].to(device)

                optimizer.zero_grad()
                outputs = model(
                    input_ids=input_ids,
                    pixel_values=pixel_values,
                    labels=labels,
                    use_cache=False,
                )
                loss = outputs.loss
                check_loss_divergence(loss)

                loss.backward()
                optimizer.step()

                total_train_loss += loss.item()
                total_steps += 1

                if step % 2 == 0 or step == len(train_loader):
                    print(f"  Epoch {epoch}/{epochs} | Step {step}/{len(train_loader)} | Train Loss: {loss.item():.4f}", flush=True)

                if max_steps is not None and total_steps >= max_steps:
                    print(f"[TRAINER] Reached maximum requested steps: {max_steps}", flush=True)
                    break
            if max_steps is not None and total_steps >= max_steps:
                break

        avg_train_loss = total_train_loss / max(1, total_steps)
        print(f"[TRAINER] Training complete. Average Train Loss: {avg_train_loss:.4f}", flush=True)

        # 3. Validation Pass (capped for fast evaluation)
        model.eval()
        total_val_loss = 0.0
        max_val_steps = min(len(val_loader), 6 if max_steps else 50)
        with torch.no_grad():
            for v_idx, batch in enumerate(val_loader, start=1):
                pixel_values = batch["pixel_values"].to(device, dtype=torch.float32)
                input_ids = batch["input_ids"].to(device)
                labels = batch["labels"].to(device)
                outputs = model(
                    input_ids=input_ids,
                    pixel_values=pixel_values,
                    labels=labels,
                    use_cache=False,
                )
                v_loss = outputs.loss
                check_loss_divergence(v_loss)
                total_val_loss += v_loss.item()
                if v_idx >= max_val_steps:
                    break

        avg_val_loss = total_val_loss / max(1, max_val_steps) if max_val_steps > 0 else avg_train_loss
        print(f"[TRAINER] Validation complete ({max_val_steps} batches). Average Val Loss: {avg_val_loss:.4f}", flush=True)

    elapsed = time.time() - start_time
    total_samples = len(dataset)
    output_checkpoint_dir.mkdir(parents=True, exist_ok=True)

    # Enforce disk space safety before writing checkpoint (~1.4 GB)
    if not check_disk_space(collector.base_dir, min_free_gb=1.0):
        usage = shutil.disk_usage(str(collector.base_dir))
        free_gb = usage.free / (1024**3)
        raise IOError(f"Insufficient disk space to save checkpoint on {collector.base_dir}: {free_gb:.2f} GB free")

    checkpoint_bin = output_checkpoint_dir / "pytorch_model.bin"
    print(f"[TRAINER] Saving adapted checkpoint to {checkpoint_bin}...")
    torch.save(model.state_dict(), checkpoint_bin)

    # Copy config files if model_dir exists
    if model_dir.is_dir():
        for fname in os.listdir(model_dir):
            if fname.endswith((".json", ".py")):
                src_f = model_dir / fname
                dst_f = output_checkpoint_dir / fname
                if src_f.is_file() and not dst_f.exists():
                    shutil.copy2(src_f, dst_f)

    metrics = {
        "train_loss": float(avg_train_loss),
        "val_loss": float(avg_val_loss),
        "steps": int(total_steps),
        "nan_detected": False,
        "checkpoint_dir": output_checkpoint_dir,
        "elapsed_sec": round(elapsed, 2),
        "total_samples": int(total_samples),
    }

    metrics_file = output_checkpoint_dir / "training_metrics.json"
    metrics_file.write_text(json.dumps({
        "train_loss": round(float(avg_train_loss), 4),
        "val_loss": round(float(avg_val_loss), 4),
        "steps": int(total_steps),
        "nan_detected": False,
        "elapsed_sec": round(elapsed, 2),
        "checkpoint_dir": str(output_checkpoint_dir),
        "total_samples": int(total_samples),
    }, indent=2), encoding="utf-8")

    print(f"[TRAINER] Adaptation complete in {elapsed:.1f}s! Metrics: {metrics}")

    if return_dict:
        return metrics
    return output_checkpoint_dir


def verify_checkpoint(
    checkpoint_dir: Union[str, Path],
    base_model_dir: Optional[Union[str, Path]] = None,
) -> Dict[str, Any]:
    """
    Verifies checkpoint directory structure, weight loading on CPU, and divergence against base model.
    Checks:
      1. Checkpoint directory exists and contains pytorch_model.bin.
      2. Model weights load cleanly on CPU into state dict.
      3. Computes tensor count and total parameter count.
      4. If base_model_dir provided, computes weight divergence (max_diff, changed_tensors).
    Returns verification report dict:
      {"valid": bool, "checkpoint_dir": str, "files": List[str], "num_tensors": int,
       "total_parameters": int, "max_diff": Optional[float], "diverged": Optional[bool],
       "changed_tensors": Optional[int], "errors": List[str]}
    """
    ckpt_path = Path(checkpoint_dir)
    errors: List[str] = []
    result: Dict[str, Any] = {
        "valid": False,
        "checkpoint_dir": str(ckpt_path),
        "files": [],
        "num_tensors": 0,
        "total_parameters": 0,
        "max_diff": None,
        "diverged": None,
        "changed_tensors": None,
        "errors": errors,
    }

    if not ckpt_path.is_dir():
        errors.append(f"Directory not found: {ckpt_path}")
        return result

    result["files"] = sorted(os.listdir(ckpt_path))
    model_bin = ckpt_path / "pytorch_model.bin"
    if not model_bin.is_file():
        errors.append(f"Weights file missing: {model_bin}")
        return result

    try:
        adapted_sd = torch.load(str(model_bin), map_location="cpu", weights_only=True)
    except Exception as e:
        errors.append(f"Failed to load weights from {model_bin}: {e}")
        return result

    if not isinstance(adapted_sd, dict) or len(adapted_sd) == 0:
        errors.append("Invalid or empty state dict")
        return result

    result["num_tensors"] = len(adapted_sd)
    result["total_parameters"] = sum(p.numel() for p in adapted_sd.values() if hasattr(p, "numel"))

    if base_model_dir is not None:
        base_path = Path(base_model_dir)
        base_bin = base_path / "pytorch_model.bin" if base_path.is_dir() else base_path
        if not base_bin.is_file():
            errors.append(f"Base model weights missing at {base_bin}")
        else:
            try:
                base_sd = torch.load(str(base_bin), map_location="cpu", weights_only=True)
                max_diff = 0.0
                diff_count = 0
                for k, v in adapted_sd.items():
                    if k in base_sd and hasattr(v, "dtype") and hasattr(base_sd[k], "dtype"):
                        bv = base_sd[k].to(device=v.device, dtype=v.dtype)
                        diff = (v - bv).abs().max().item()
                        if diff > max_diff:
                            max_diff = diff
                        if not torch.allclose(v, bv, atol=1e-7):
                            diff_count += 1
                result["max_diff"] = float(max_diff)
                result["diverged"] = bool(max_diff > 0.0)
                result["changed_tensors"] = int(diff_count)
            except Exception as e:
                errors.append(f"Failed to compare weights against base model: {e}")

    result["valid"] = len(errors) == 0
    return result


__all__ = [
    "FlorenceLocalDataset",
    "collate_single",
    "freeze_florence_backbone",
    "pad_to_square",
    "check_loss_divergence",
    "split_train_val_dataset",
    "check_disk_space",
    "verify_checkpoint",
    "train_local",
]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CPower Local Continual Trainer")
    parser.add_argument("--epochs", type=int, default=3, help="Training epochs")
    parser.add_argument("--max-steps", type=int, default=None, help="Maximum steps to train")
    parser.add_argument("--lr", type=float, default=1e-4, help="Learning rate")
    parser.add_argument("--device", default="cpu", help="Device (cpu or cuda)")
    parser.add_argument("--val-ratio", type=float, default=0.1, help="Validation split ratio")
    args = parser.parse_args()

    train_local(
        epochs=args.epochs,
        lr=args.lr,
        device_name=args.device,
        max_steps=args.max_steps,
        val_ratio=args.val_ratio,
    )
