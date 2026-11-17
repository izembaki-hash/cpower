"""
CPower Trainer & Continual Loop Unit Tests (tests/test_trainer.py).

Comprehensive unit test suite for:
  1. Parameter-efficient fine-tuning freezing (freeze_florence_backbone)
  2. Canvas aspect ratio padding (pad_to_square)
  3. Loss divergence and NaN guards (check_loss_divergence)
  4. Train/validation dataset splitting (split_train_val_dataset)
  5. Dataset loading and tokenization (FlorenceLocalDataset)
  6. train_local contract metrics and JSON reporting
  7. train_local zero-steps execution (max_steps=0)
  8. train_local pre-flight disk safety threshold
  9. Checkpoint verification helper (verify_checkpoint)
  10. ContinualTrainingLoop initialization and defaults
  11. ContinualTrainingLoop single-cycle 4-worker execution (including Arabic RTL)
  12. ContinualTrainingLoop disk guard halt and trajectory cleanup
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from unittest import TestCase
from unittest.mock import patch

from PIL import Image
import torch
from torch.utils.data import Dataset, TensorDataset

REPO_ROOT = Path(__file__).resolve().parent.parent
CLIENTS_DIR = REPO_ROOT / "clients" / "python"
if str(CLIENTS_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENTS_DIR))

from cpower_dataset_collector import CPowerDatasetCollector
from cpower_trainer import (
    FlorenceLocalDataset,
    check_disk_space,
    check_loss_divergence,
    freeze_florence_backbone,
    pad_to_square,
    split_train_val_dataset,
    train_local,
    verify_checkpoint,
)
from cpower_continual_loop import (
    ContinualTrainingLoop,
    worker_arabic_agent,
    worker_dialog_agent,
    worker_explorer_agent,
    worker_web_agent,
)


class LightweightStubModel(torch.nn.Module):
    """Fast neural model stub for non-slow training loop verification."""

    def __init__(self):
        super().__init__()
        self.vision_tower = torch.nn.Linear(4, 4)
        self.image_proj_norm = torch.nn.Linear(4, 4)
        self.language_model = torch.nn.Linear(4, 4)
        self.image_pos_embed = torch.nn.Linear(4, 4)
        self.head = torch.nn.Linear(4, 1)

    def forward(self, input_ids=None, pixel_values=None, labels=None, use_cache=False):
        pred = self.head(self.image_proj_norm(torch.ones(1, 4)))
        loss = torch.nn.functional.mse_loss(pred, torch.ones_like(pred) * 2.0) + 0.1

        class Output:
            pass

        res = Output()
        res.loss = loss
        return res


class LightweightStubProcessor:
    """Fast processor stub returning properly formatted tensors."""

    def __call__(self, text, images, return_tensors):
        return {
            "input_ids": torch.tensor([[101, 102]]),
            "pixel_values": torch.zeros((1, 3, 768, 768)),
        }

    @property
    def tokenizer(self):
        return lambda text, return_tensors: {"input_ids": torch.tensor([[201, 202]])}


class TestCPowerTrainer(TestCase):
    """Unit test suite for cpower_trainer.py and cpower_continual_loop.py."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.isolated_path = Path(self.temp_dir.name)

    def tearDown(self):
        try:
            self.temp_dir.cleanup()
        except Exception:
            pass

    def test_freeze_florence_backbone(self):
        """1. freeze_florence_backbone freezes vision_tower and enables image_proj_norm and language heads."""
        model = LightweightStubModel()

        # Pre-freeze all parameters to verify re-entrancy safety
        for param in model.parameters():
            param.requires_grad = False

        trainable, total = freeze_florence_backbone(model)

        # vision_tower must be frozen
        for p in model.vision_tower.parameters():
            self.assertFalse(p.requires_grad)

        # image_proj_norm, language_model, image_pos_embed must be trainable
        for p in model.image_proj_norm.parameters():
            self.assertTrue(p.requires_grad)
        for p in model.language_model.parameters():
            self.assertTrue(p.requires_grad)
        for p in model.image_pos_embed.parameters():
            self.assertTrue(p.requires_grad)

        self.assertGreater(trainable, 0)
        self.assertLess(trainable, total)
        self.assertEqual(trainable, sum(p.numel() for p in model.parameters() if p.requires_grad))

    def test_pad_to_square_aspect_ratios(self):
        """2. pad_to_square pads rectangular images to square >= 768 without stretching."""
        # Wide image: 1280x720
        wide_img = Image.new("RGB", (1280, 720), (10, 20, 30))
        padded_wide = pad_to_square(wide_img, min_dim=768)
        self.assertEqual(padded_wide.size, (1280, 1280))
        self.assertEqual(padded_wide.getpixel((10, 10)), (10, 20, 30))
        self.assertEqual(padded_wide.getpixel((10, 800)), (255, 255, 255))

        # Tall image: 600x900
        tall_img = Image.new("RGB", (600, 900), (40, 50, 60))
        padded_tall = pad_to_square(tall_img, min_dim=768)
        self.assertEqual(padded_tall.size, (900, 900))
        self.assertEqual(padded_tall.getpixel((10, 10)), (40, 50, 60))
        self.assertEqual(padded_tall.getpixel((700, 10)), (255, 255, 255))

        # Small image: 200x200 padded to min_dim=768
        small_img = Image.new("RGB", (200, 200), (70, 80, 90))
        padded_small = pad_to_square(small_img, min_dim=768)
        self.assertEqual(padded_small.size, (768, 768))

    def test_check_loss_divergence_guards(self):
        """3. check_loss_divergence raises AssertionError on NaN, Inf, and -Inf."""
        with self.assertRaises(AssertionError):
            check_loss_divergence(torch.tensor(float("nan")))

        with self.assertRaises(AssertionError):
            check_loss_divergence(torch.tensor(float("inf")))

        with self.assertRaises(AssertionError):
            check_loss_divergence(torch.tensor(float("-inf")))

        # Finite loss must pass cleanly
        check_loss_divergence(torch.tensor(1.2345))

    def test_split_train_val_dataset_ratios(self):
        """4. split_train_val_dataset produces deterministic 90/10 disjoint split with boundary guards."""
        # N = 20
        ds_20 = TensorDataset(torch.arange(20))
        train_set, val_set = split_train_val_dataset(ds_20, val_ratio=0.1, seed=42)
        self.assertEqual(len(train_set), 18)
        self.assertEqual(len(val_set), 2)

        # Disjoint verification
        train_idx = set(train_set.indices)
        val_idx = set(val_set.indices)
        self.assertTrue(train_idx.isdisjoint(val_idx))

        # Determinism verification
        t2, v2 = split_train_val_dataset(ds_20, val_ratio=0.1, seed=42)
        self.assertEqual(train_set.indices, t2.indices)

        # Boundary N = 0
        ds_0 = TensorDataset(torch.empty(0))
        t0, v0 = split_train_val_dataset(ds_0)
        self.assertEqual(len(t0), 0)

        # Boundary N = 1
        ds_1 = TensorDataset(torch.arange(1))
        t1, v1 = split_train_val_dataset(ds_1)
        self.assertEqual(len(t1), 1)
        self.assertEqual(len(v1), 1)

        # Boundary N = 2
        ds_2 = TensorDataset(torch.arange(2))
        t2, v2 = split_train_val_dataset(ds_2)
        self.assertEqual(len(t2), 1)
        self.assertEqual(len(v2), 1)

        # Defensive guard: val_ratio=1.0 on N=10 still guarantees train_size >= 1
        ds_10 = TensorDataset(torch.arange(10))
        t10, v10 = split_train_val_dataset(ds_10, val_ratio=1.0)
        self.assertEqual(len(t10), 1)
        self.assertEqual(len(v10), 9)

    def test_florence_local_dataset_loader(self):
        """5. FlorenceLocalDataset loads JSONL records, skips corrupted lines, and pads images."""
        collector = CPowerDatasetCollector(base_dir=self.isolated_path)
        tid = collector.start_trajectory("Loader Test")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase="زر اختبار",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

        # Inject a corrupted JSON line into dataset
        with open(collector.florence_jsonl_path, "a", encoding="utf-8") as f:
            f.write("INVALID JSON LINE <<>>\n")

        ds = FlorenceLocalDataset(collector.florence_jsonl_path, processor=LightweightStubProcessor())
        self.assertEqual(len(ds), 1)

        sample = ds[0]
        self.assertIn("input_ids", sample)
        self.assertIn("pixel_values", sample)
        self.assertIn("labels", sample)
        self.assertIsInstance(sample["pixel_values"], torch.Tensor)

    def test_train_local_contract_metrics(self):
        """6. train_local executes training pass and returns full metrics contract."""
        collector = CPowerDatasetCollector(base_dir=self.isolated_path)
        for i in range(2):
            tid = collector.start_trajectory(f"Metrics Contract {i}")
            collector.record_step(
                trajectory_id=tid,
                image=Image.new("RGB", (100, 100), (30 * i, 30 * i, 30 * i)),
                action_type="click",
                phrase=f"زر_{i}",
                bbox=(10, 10, 50, 50),
            )
            collector.complete_trajectory(tid, succeeded=True)

        metrics = train_local(
            dataset_root=self.isolated_path,
            epochs=1,
            max_steps=1,
            model=LightweightStubModel(),
            processor=LightweightStubProcessor(),
        )

        self.assertIsInstance(metrics, dict)
        self.assertIn("train_loss", metrics)
        self.assertIn("val_loss", metrics)
        self.assertIn("steps", metrics)
        self.assertIn("nan_detected", metrics)
        self.assertIn("checkpoint_dir", metrics)
        self.assertIn("elapsed_sec", metrics)
        self.assertIn("total_samples", metrics)

        self.assertGreater(metrics["train_loss"], 0.0)
        self.assertGreater(metrics["val_loss"], 0.0)
        self.assertEqual(metrics["steps"], 1)
        self.assertFalse(metrics["nan_detected"])
        self.assertEqual(metrics["total_samples"], 2)

        # Assert metrics file written to disk
        metrics_file = Path(metrics["checkpoint_dir"]) / "training_metrics.json"
        self.assertTrue(metrics_file.is_file())
        file_data = json.loads(metrics_file.read_text(encoding="utf-8"))
        self.assertIn("elapsed_sec", file_data)
        self.assertEqual(file_data["total_samples"], 2)

    def test_train_local_zero_steps_execution(self):
        """7. train_local with max_steps=0 skips training cleanly without errors."""
        collector = CPowerDatasetCollector(base_dir=self.isolated_path)
        tid = collector.start_trajectory("Zero Steps Test")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase="صفر_خطوة",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

        metrics = train_local(
            dataset_root=self.isolated_path,
            epochs=3,
            max_steps=0,
            model=LightweightStubModel(),
            processor=LightweightStubProcessor(),
        )

        self.assertEqual(metrics["steps"], 0)
        self.assertEqual(metrics["train_loss"], 0.0)
        self.assertEqual(metrics["val_loss"], 0.0)
        self.assertFalse(metrics["nan_detected"])

    def test_train_local_disk_safety_threshold(self):
        """8. train_local raises IOError before training when disk space is below safety threshold."""
        collector = CPowerDatasetCollector(base_dir=self.isolated_path)
        tid = collector.start_trajectory("Disk Safety Test")
        collector.record_step(
            trajectory_id=tid,
            image=Image.new("RGB", (100, 100), (50, 50, 50)),
            action_type="click",
            phrase="قرص_منخفض",
            bbox=(10, 10, 50, 50),
        )
        collector.complete_trajectory(tid, succeeded=True)

        # Mock check_disk_space to return False
        with patch("cpower_trainer.check_disk_space", return_value=False):
            with self.assertRaises(IOError):
                train_local(
                    dataset_root=self.isolated_path,
                    epochs=1,
                    max_steps=1,
                    model=LightweightStubModel(),
                    processor=LightweightStubProcessor(),
                )

    def test_verify_checkpoint_helper(self):
        """9. verify_checkpoint validates directory structure and computes weight divergence."""
        # Non-existent directory
        invalid_res = verify_checkpoint(self.isolated_path / "nonexistent")
        self.assertFalse(invalid_res["valid"])
        self.assertTrue(any("Directory not found" in e for e in invalid_res["errors"]))

        # Missing pytorch_model.bin
        empty_ckpt = self.isolated_path / "empty_ckpt"
        empty_ckpt.mkdir()
        missing_bin_res = verify_checkpoint(empty_ckpt)
        self.assertFalse(missing_bin_res["valid"])
        self.assertTrue(any("Weights file missing" in e for e in missing_bin_res["errors"]))

        # Valid checkpoint
        valid_ckpt = self.isolated_path / "valid_ckpt"
        valid_ckpt.mkdir()
        adapted_weights = {
            "image_proj_norm.weight": torch.tensor([1.5, 2.5]),
            "image_proj_norm.bias": torch.tensor([0.1, 0.2]),
        }
        torch.save(adapted_weights, valid_ckpt / "pytorch_model.bin")

        res_valid = verify_checkpoint(valid_ckpt)
        self.assertTrue(res_valid["valid"])
        self.assertEqual(res_valid["num_tensors"], 2)
        self.assertEqual(res_valid["total_parameters"], 4)

        # Base model comparison with divergence
        base_dir = self.isolated_path / "base_model"
        base_dir.mkdir()
        base_weights = {
            "image_proj_norm.weight": torch.tensor([1.0, 2.0]),
            "image_proj_norm.bias": torch.tensor([0.1, 0.2]),
        }
        torch.save(base_weights, base_dir / "pytorch_model.bin")

        diff_res = verify_checkpoint(valid_ckpt, base_model_dir=base_dir)
        self.assertTrue(diff_res["valid"])
        self.assertAlmostEqual(diff_res["max_diff"], 0.5, places=5)
        self.assertTrue(diff_res["diverged"])
        self.assertEqual(diff_res["changed_tensors"], 1)

    def test_continual_training_loop_initialization(self):
        """10. ContinualTrainingLoop initializes with proper attribute defaults and collector binding."""
        collector = CPowerDatasetCollector(base_dir=self.isolated_path)
        loop = ContinualTrainingLoop(
            collector=collector,
            samples_per_worker=3,
            train_steps=2,
            epochs=2,
            lr=5e-5,
            device="cpu",
            val_ratio=0.15,
            min_free_gb=1.5,
            enable_arabic_worker=True,
        )

        self.assertEqual(loop.samples_per_worker, 3)
        self.assertEqual(loop.train_steps, 2)
        self.assertEqual(loop.epochs, 2)
        self.assertEqual(loop.lr, 5e-5)
        self.assertEqual(loop.device, "cpu")
        self.assertEqual(loop.val_ratio, 0.15)
        self.assertEqual(loop.min_free_gb, 1.5)
        self.assertTrue(loop.enable_arabic_worker)
        self.assertIs(loop.collector, collector)
        self.assertEqual(loop.cycle_count, 0)
        self.assertEqual(loop.total_samples_harvested, 0)

    def test_continual_training_loop_single_cycle_execution(self):
        """11. ContinualTrainingLoop single cycle harvests across all 4 workers (including Arabic) and trains."""
        loop = ContinualTrainingLoop(
            base_dir=self.isolated_path,
            samples_per_worker=1,
            train_steps=1,
            enable_arabic_worker=True,
        )

        def mock_train(*args, **kwargs):
            return {
                "train_loss": 0.5,
                "val_loss": 0.45,
                "steps": 1,
                "nan_detected": False,
                "checkpoint_dir": self.isolated_path / "checkpoints",
            }

        with patch("cpower_continual_loop.train_local", side_effect=mock_train):
            cycle_summary = loop.run_single_cycle()

        self.assertEqual(loop.cycle_count, 1)
        self.assertEqual(cycle_summary["cycle"], 1)
        # 4 workers (Explorer, Web, Dialog, Arabic) * 1 sample = 4 samples
        self.assertEqual(cycle_summary["samples_harvested"], 4)
        workers_run = cycle_summary["harvest_details"]["by_worker"]
        self.assertIn("worker_explorer_agent", workers_run)
        self.assertIn("worker_web_agent", workers_run)
        self.assertIn("worker_dialog_agent", workers_run)
        self.assertIn("worker_arabic_agent", workers_run)
        self.assertEqual(len(loop.cycle_history), 1)

    def test_continual_training_loop_disk_guard_stop(self):
        """12. ContinualTrainingLoop halts cleanly when disk space drops and cleans up active trajectories."""
        loop = ContinualTrainingLoop(
            base_dir=self.isolated_path,
            samples_per_worker=1,
            train_steps=0,
            enable_arabic_worker=True,
        )

        # Allow first cycle to complete (1 loop check + 4 worker checks = 5), then return False for cycle 2
        calls = {"count": 0}

        def mock_disk():
            calls["count"] += 1
            return calls["count"] <= 5

        def mock_train(*args, **kwargs):
            return {"steps": 0, "checkpoint_dir": self.isolated_path / "checkpoints"}

        # Leave an active trajectory to verify cleanup
        tid = loop.collector.start_trajectory("Dangling Active Trajectory")

        with patch.object(loop.collector, "_check_disk_space", side_effect=mock_disk):
            with patch("cpower_continual_loop.train_local", side_effect=mock_train):
                result = loop.run_continual(max_cycles=5)

        self.assertEqual(result["cycles_completed"], 1)
        self.assertTrue(result["stopped_cleanly"])
        # Trajectory cleanup must have finalized the dangling trajectory
        self.assertEqual(len(loop.collector._active_trajectories), 0)


if __name__ == "__main__":
    import unittest

    unittest.main()
