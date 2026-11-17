"""
Adversarial Stress Test Suite for Milestone 3 Gate Verification.
Executed by teamwork_challenger_m3_1.

Tests the resilience, mathematical edge cases, and failure modes of:
  1. check_loss_divergence
  2. split_train_val_dataset
  3. pad_to_square
  4. freeze_florence_backbone
  5. verify_checkpoint
  6. train_local (max_steps=0, low disk space, quarantine)
"""

import collections
import json
import os
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PIL import Image
import torch
import torch.nn as nn
from torch.utils.data import Dataset, TensorDataset

import sys
CLIENT_DIR = Path(__file__).resolve().parent.parent / "clients" / "python"
if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

from cpower_trainer import (
    check_disk_space,
    check_loss_divergence,
    freeze_florence_backbone,
    pad_to_square,
    split_train_val_dataset,
    train_local,
    verify_checkpoint,
)
from cpower_dataset_collector import CPowerDatasetCollector


class MockLinearFlorenceModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.vision_tower = nn.Linear(4, 4)
        self.image_proj_norm = nn.Linear(4, 4)
        self.language_model = nn.Linear(4, 4)
        self.image_pos_embed = nn.Linear(4, 4)
        self.head = nn.Linear(4, 1)

    def forward(self, input_ids=None, pixel_values=None, labels=None, use_cache=False):
        pred = self.head(self.image_proj_norm(torch.ones(1, 4)))
        loss = (pred - 0.5).pow(2).mean()
        return SimpleNamespace(loss=loss)


class MockProcessor:
    class MockTokenizer:
        def __call__(self, text=None, return_tensors="pt"):
            return {"input_ids": torch.tensor([[101, 102]])}

    def __init__(self):
        self.tokenizer = self.MockTokenizer()

    def __call__(self, text=None, images=None, return_tensors="pt"):
        return {
            "input_ids": torch.tensor([[101, 102]]),
            "pixel_values": torch.zeros(1, 3, 768, 768),
        }


class TestCheckLossDivergence(unittest.TestCase):
    """Adversarial testing of check_loss_divergence."""

    def test_nan_loss_raises_assertion(self):
        with self.assertRaises(AssertionError) as ctx:
            check_loss_divergence(torch.tensor(float("nan")))
        self.assertIn("NaN or Inf detected", str(ctx.exception))

    def test_inf_loss_raises_assertion(self):
        with self.assertRaises(AssertionError) as ctx:
            check_loss_divergence(torch.tensor(float("inf")))
        self.assertIn("NaN or Inf detected", str(ctx.exception))

    def test_negative_inf_loss_raises_assertion(self):
        with self.assertRaises(AssertionError) as ctx:
            check_loss_divergence(torch.tensor(float("-inf")))
        self.assertIn("NaN or Inf detected", str(ctx.exception))

    def test_zero_loss_passes(self):
        # 0.0 is finite and non-nan
        check_loss_divergence(torch.tensor(0.0))

    def test_negative_finite_loss_passes(self):
        # Empirical observation: check_loss_divergence only checks isnan/isinf,
        # so negative finite loss does not raise AssertionError.
        check_loss_divergence(torch.tensor(-1.0))
        check_loss_divergence(torch.tensor(-1e5))

    def test_large_finite_loss_passes(self):
        # Empirical observation: 1e20 is finite (not inf), so it passes check_loss_divergence.
        check_loss_divergence(torch.tensor(1e20))

    def test_multi_element_tensor_raises_runtime_error(self):
        # Empirical finding: torch.isnan(tensor([1.0, nan])) returns a boolean tensor,
        # triggering Python RuntimeError: Boolean value of Tensor with more than one value is ambiguous.
        multi_nan = torch.tensor([1.0, float("nan")])
        with self.assertRaises(RuntimeError):
            check_loss_divergence(multi_nan)

    def test_python_float_nan_raises_type_error(self):
        # Empirical finding: Passing raw Python float nan raises TypeError because torch.isnan expects Tensor.
        with self.assertRaises(TypeError):
            check_loss_divergence(float("nan"))


class TestSplitTrainValDataset(unittest.TestCase):
    """Adversarial stress testing of split_train_val_dataset across sample sizes and ratios."""

    def test_empty_dataset_0_samples(self):
        ds = TensorDataset(torch.arange(0))
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.1)
        self.assertEqual(len(t_sub), 0)
        self.assertEqual(len(v_sub), 0)

    def test_single_sample_dataset_1_sample(self):
        ds = TensorDataset(torch.arange(1))
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.1)
        # For N=1, split_train_val_dataset returns (ds, ds) as identity
        self.assertEqual(len(t_sub), 1)
        self.assertEqual(len(v_sub), 1)

    def test_small_dataset_2_samples_extreme_ratios(self):
        ds = TensorDataset(torch.arange(2))
        for r in [0.0, 0.1, 0.5, 0.99, 1.0, 1.5]:
            t_sub, v_sub = split_train_val_dataset(ds, val_ratio=r)
            self.assertEqual(len(t_sub), 1, f"Failed train_len for ratio {r}")
            self.assertEqual(len(v_sub), 1, f"Failed val_len for ratio {r}")
            self.assertTrue(set(t_sub.indices).isdisjoint(set(v_sub.indices)))
            self.assertEqual(len(t_sub) + len(v_sub), 2)

    def test_dataset_3_samples_extreme_ratios(self):
        ds = TensorDataset(torch.arange(3))
        # ratio 0.0 -> train=2, val=1
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.0)
        self.assertEqual((len(t_sub), len(v_sub)), (2, 1))
        self.assertTrue(set(t_sub.indices).isdisjoint(set(v_sub.indices)))

        # ratio 0.1 -> train=2, val=1
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.1)
        self.assertEqual((len(t_sub), len(v_sub)), (2, 1))

        # ratio 1.0 -> train=1, val=2 (val capped at total-1)
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=1.0)
        self.assertEqual((len(t_sub), len(v_sub)), (1, 2))

    def test_large_dataset_1000_samples_extreme_ratios(self):
        ds = TensorDataset(torch.arange(1000))
        # ratio 0.0 -> val=1 (due to max(1, ...)), train=999
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.0)
        self.assertEqual((len(t_sub), len(v_sub)), (999, 1))

        # ratio 0.1 -> 900 train, 100 val
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.1)
        self.assertEqual((len(t_sub), len(v_sub)), (900, 100))

        # ratio 0.5 -> 500 train, 500 val
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.5)
        self.assertEqual((len(t_sub), len(v_sub)), (500, 500))

        # ratio 0.99 -> 10 train, 990 val
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=0.99)
        self.assertEqual((len(t_sub), len(v_sub)), (10, 990))

        # ratio 1.0 -> 1 train, 999 val (capped at total-1)
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=1.0)
        self.assertEqual((len(t_sub), len(v_sub)), (1, 999))

        # ratio 1.5 -> 1 train, 999 val
        t_sub, v_sub = split_train_val_dataset(ds, val_ratio=1.5)
        self.assertEqual((len(t_sub), len(v_sub)), (1, 999))

    def test_split_seed_determinism(self):
        ds = TensorDataset(torch.arange(100))
        t1, v1 = split_train_val_dataset(ds, val_ratio=0.2, seed=42)
        t2, v2 = split_train_val_dataset(ds, val_ratio=0.2, seed=42)
        self.assertEqual(list(t1.indices), list(t2.indices))
        self.assertEqual(list(v1.indices), list(v2.indices))


class TestPadToSquare(unittest.TestCase):
    """Adversarial testing of pad_to_square with extreme resolutions and image modes."""

    def test_1x1_micro_image(self):
        im = Image.new("RGB", (1, 1), (255, 0, 0))
        res = pad_to_square(im, min_dim=768)
        self.assertEqual(res.size, (768, 768))
        self.assertEqual(res.mode, "RGB")
        self.assertEqual(res.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(res.getpixel((767, 767)), (255, 255, 255))

    def test_extreme_aspect_ratio_1x10000(self):
        im = Image.new("RGB", (1, 10000), (0, 255, 0))
        res = pad_to_square(im)
        self.assertEqual(res.size, (10000, 10000))
        self.assertEqual(res.mode, "RGB")

    def test_extreme_aspect_ratio_10000x1(self):
        im = Image.new("RGB", (10000, 1), (0, 0, 255))
        res = pad_to_square(im)
        self.assertEqual(res.size, (10000, 10000))
        self.assertEqual(res.mode, "RGB")

    def test_rgba_image_mode(self):
        im = Image.new("RGBA", (100, 200), (255, 128, 0, 200))
        res = pad_to_square(im, min_dim=768)
        self.assertEqual(res.size, (768, 768))
        self.assertEqual(res.mode, "RGB")

    def test_grayscale_image_mode(self):
        im = Image.new("L", (300, 100), 128)
        res = pad_to_square(im, min_dim=768)
        self.assertEqual(res.size, (768, 768))
        self.assertEqual(res.mode, "RGB")

    def test_palette_image_mode(self):
        im = Image.new("P", (150, 150))
        res = pad_to_square(im, min_dim=768)
        self.assertEqual(res.size, (768, 768))
        self.assertEqual(res.mode, "RGB")

    def test_binary_1bit_image_mode(self):
        im = Image.new("1", (200, 200), 1)
        res = pad_to_square(im, min_dim=768)
        self.assertEqual(res.size, (768, 768))
        self.assertEqual(res.mode, "RGB")

    def test_custom_bg_color_and_min_dim(self):
        im = Image.new("RGB", (100, 100), (0, 0, 0))
        res = pad_to_square(im, min_dim=512, bg_color=(128, 128, 128))
        self.assertEqual(res.size, (512, 512))
        self.assertEqual(res.getpixel((500, 500)), (128, 128, 128))


class TestFreezeFlorenceBackbone(unittest.TestCase):
    """Stress testing of freeze_florence_backbone for re-entrancy and pre-frozen model recovery."""

    def test_re_entrancy_lightweight_model(self):
        m = MockLinearFlorenceModel()
        t1, tot1 = freeze_florence_backbone(m)
        t2, tot2 = freeze_florence_backbone(m)
        t3, tot3 = freeze_florence_backbone(m)
        self.assertEqual(t1, t2)
        self.assertEqual(t2, t3)
        self.assertEqual(tot1, tot2)
        self.assertEqual(tot2, tot3)

    def test_pre_frozen_model_recovery_lightweight_model(self):
        m = MockLinearFlorenceModel()
        for p in m.parameters():
            p.requires_grad = False
        self.assertEqual(sum(p.numel() for p in m.parameters() if p.requires_grad), 0)

        t, tot = freeze_florence_backbone(m)
        self.assertGreater(t, 0)
        self.assertFalse(all(p.requires_grad for p in m.vision_tower.parameters()))
        self.assertTrue(all(p.requires_grad for p in m.image_proj_norm.parameters()))
        self.assertTrue(all(p.requires_grad for p in m.language_model.parameters()))
        self.assertTrue(all(p.requires_grad for p in m.image_pos_embed.parameters()))


class TestVerifyCheckpoint(unittest.TestCase):
    """Stress testing of verify_checkpoint against corrupted, missing, and valid artifacts."""

    def setUp(self):
        self.tmp_dir = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def test_non_existent_directory(self):
        res = verify_checkpoint(self.tmp_dir / "does_not_exist_xyz")
        self.assertFalse(res["valid"])
        self.assertTrue(any("Directory not found" in e for e in res["errors"]))

    def test_empty_directory(self):
        empty_dir = self.tmp_dir / "empty"
        empty_dir.mkdir()
        res = verify_checkpoint(empty_dir)
        self.assertFalse(res["valid"])
        self.assertTrue(any("Weights file missing" in e for e in res["errors"]))

    def test_corrupted_zero_byte_bin(self):
        c_dir = self.tmp_dir / "corrupted_zero"
        c_dir.mkdir()
        (c_dir / "pytorch_model.bin").write_bytes(b"")
        res = verify_checkpoint(c_dir)
        self.assertFalse(res["valid"])
        self.assertTrue(any("Failed to load weights" in e or "Invalid or empty" in e for e in res["errors"]))

    def test_corrupted_garbage_bin(self):
        c_dir = self.tmp_dir / "corrupted_garbage"
        c_dir.mkdir()
        (c_dir / "pytorch_model.bin").write_bytes(b"GARBAGE_NOT_A_PICKLE_FILE_12345")
        res = verify_checkpoint(c_dir)
        self.assertFalse(res["valid"])
        self.assertTrue(any("Failed to load weights" in e for e in res["errors"]))

    def test_missing_config_json_still_passes_verify_checkpoint(self):
        # Empirical finding: verify_checkpoint does NOT enforce config.json,
        # so a directory with only pytorch_model.bin is reported as valid=True.
        # However, HuggingFace AutoModelForCausalLM.from_pretrained fails without config.json.
        v_dir = self.tmp_dir / "only_bin"
        v_dir.mkdir()
        torch.save({"weight": torch.randn(4, 4)}, v_dir / "pytorch_model.bin")
        res = verify_checkpoint(v_dir)
        self.assertTrue(res["valid"])
        self.assertNotIn("config.json", res["files"])

    def test_valid_checkpoint_and_weight_divergence(self):
        base_dir = self.tmp_dir / "base"
        base_dir.mkdir()
        adapted_dir = self.tmp_dir / "adapted"
        adapted_dir.mkdir()

        t1 = torch.tensor([1.0, 2.0, 3.0])
        t2 = torch.tensor([10.0, 20.0, 30.0])
        torch.save({"layer1": t1, "layer2": t2}, base_dir / "pytorch_model.bin")

        # Adapted with layer2 modified
        torch.save({"layer1": t1.clone(), "layer2": t2 + 0.5}, adapted_dir / "pytorch_model.bin")

        res = verify_checkpoint(adapted_dir, base_model_dir=base_dir)
        self.assertTrue(res["valid"])
        self.assertTrue(res["diverged"])
        self.assertAlmostEqual(res["max_diff"], 0.5, places=5)
        self.assertEqual(res["changed_tensors"], 1)


class TestTrainLocalAdversarial(unittest.TestCase):
    """Stress testing of train_local edge cases: max_steps=0, low disk space, Drive H quarantine."""

    def setUp(self):
        self.tmp_dir = Path(tempfile.mkdtemp())
        self.img_dir = self.tmp_dir / "images"
        self.img_dir.mkdir(parents=True)
        img = Image.new("RGB", (64, 64), (100, 150, 200))
        img.save(self.img_dir / "test.jpg")

        self.jsonl = self.tmp_dir / "dataset_florence2.jsonl"
        rec = {
            "image": "images/test.jpg",
            "prefix": "<CAPTION_TO_PHRASE_GROUNDING>",
            "suffix": "button",
            "target": "button<loc_10><loc_10><loc_90><loc_90>",
        }
        self.jsonl.write_text(json.dumps(rec) + "\n", encoding="utf-8")
        self.model = MockLinearFlorenceModel()
        self.processor = MockProcessor()

    def tearDown(self):
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def test_train_local_max_steps_zero(self):
        res = train_local(
            dataset_root=self.tmp_dir,
            epochs=1,
            max_steps=0,
            model=self.model,
            processor=self.processor,
        )
        self.assertEqual(res["steps"], 0)
        self.assertEqual(res["train_loss"], 0.0)
        self.assertEqual(res["val_loss"], 0.0)
        self.assertFalse(res["nan_detected"])
        ckpt_dir = res["checkpoint_dir"]
        self.assertTrue((ckpt_dir / "pytorch_model.bin").is_file())
        self.assertTrue((ckpt_dir / "training_metrics.json").is_file())

    def test_train_local_simulated_low_disk_space_preflight(self):
        Usage = collections.namedtuple("usage", ["total", "used", "free"])
        mock_low = Usage(total=100 * 1024**3, used=99.5 * 1024**3, free=int(0.5 * 1024**3))
        with patch("shutil.disk_usage", return_value=mock_low):
            with self.assertRaises(IOError) as ctx:
                train_local(
                    dataset_root=self.tmp_dir,
                    epochs=1,
                    max_steps=1,
                    model=self.model,
                    processor=self.processor,
                )
            self.assertIn("Insufficient disk space", str(ctx.exception))

    def test_train_local_simulated_low_disk_space_at_checkpoint_save(self):
        Usage = collections.namedtuple("usage", ["total", "used", "free"])
        normal_usage = Usage(total=100 * 1024**3, used=50 * 1024**3, free=50 * 1024**3)
        low_usage = Usage(total=100 * 1024**3, used=99.5 * 1024**3, free=int(0.5 * 1024**3))

        call_count = [0]
        def dynamic_disk_usage(path):
            call_count[0] += 1
            # First call is pre-flight (return normal), second call is pre-save (return low)
            if call_count[0] <= 1:
                return normal_usage
            return low_usage

        with patch("shutil.disk_usage", side_effect=dynamic_disk_usage):
            with self.assertRaises(IOError) as ctx:
                train_local(
                    dataset_root=self.tmp_dir,
                    epochs=1,
                    max_steps=1,
                    model=self.model,
                    processor=self.processor,
                )
            self.assertIn("Insufficient disk space to save checkpoint", str(ctx.exception))

    def test_drive_h_storage_quarantine_enforcement(self):
        with self.assertRaises(PermissionError) as ctx:
            CPowerDatasetCollector(base_dir="H:/quarantine_challenge_dir")
        self.assertIn("Drive H: storage quarantine violation", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
