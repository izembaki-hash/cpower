"""
CPower Local Vision System (clients/python/cpower_vision_local.py).

Autonomous Computer-Use Vision & Visual Grounding Module for CPower.
Empowers the agent to visually perceive, detect, and interact with non-accessible UI:
  1. Fast DXGI/GDI Micro-Region & Window Image Acquisition via CPower C11 daemon.
  2. Sub-50ms Computer Vision Contour & Interactive Region Detector (OpenCV).
  3. Visual Set-of-Marks ([V1], [V2], [V3]) with Screen Coordinate Mapping.
  4. Neural Grounding via Florence-2 (Local SLM-V) & NVIDIA NIM Super-VL Fallback.
  5. Episodic Visual Memory Integration (cpower_memory.py).
"""

from __future__ import annotations

import base64
import io
import json
import os
from pathlib import Path
import re
from typing import Any, Dict, List, Optional, Tuple, Union

try:
    import cv2
    import numpy as np
    _CV2_AVAILABLE = True
except ImportError:
    _CV2_AVAILABLE = False

try:
    from PIL import Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

import urllib.request
import urllib.error

from cpower_client import CPowerClient
from cpower_process_guard import is_protected_window, get_process_name_by_pid, _user32
import ctypes
from ctypes import wintypes


def get_window_bounds(hwnd: int) -> Optional[Tuple[int, int, int, int]]:
    """Returns (left, top, right, bottom) for hwnd."""
    rect = wintypes.RECT()
    if _user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return (rect.left, rect.top, rect.right, rect.bottom)
    return None


class VisualMark:
    """Represents a visually identified interactive candidate element."""
    def __init__(self, mark_id: int, x: int, y: int, w: int, h: int, label: str = "visual_element", confidence: float = 1.0):
        self.mark_id = mark_id
        self.x = x
        self.y = y
        self.w = w
        self.h = h
        self.cx = x + w // 2
        self.cy = y + h // 2
        self.label = label
        self.confidence = confidence

    def to_dict(self) -> Dict[str, Any]:
        return {
            "mark_id": self.mark_id,
            "cx": self.cx,
            "cy": self.cy,
            "bbox": [self.x, self.y, self.w, self.h],
            "label": self.label,
            "confidence": self.confidence,
        }

    def __repr__(self) -> str:
        return f"[V{self.mark_id}] <{self.label}> center=({self.cx}, {self.cy}) size={self.w}x{self.h}"


class CPowerLocalVision:
    """Autonomous Local Vision Engine for CPower Computer-Use."""

    def __init__(
        self,
        client: Optional[CPowerClient] = None,
        model_dir: str = r"C:\Users\Administrator\.models\florence-2-base",
        nvidia_api_key: Optional[str] = None,
    ):
        self.client = client or CPowerClient()
        adapted_dir = Path("E:/cpower_training_data/checkpoints/cpower_florence2_adapted")
        if (adapted_dir / "pytorch_model.bin").is_file():
            self.model_dir = adapted_dir
        else:
            self.model_dir = Path(model_dir)
        self.nvidia_api_key = nvidia_api_key or os.environ.get("NVIDIA_API_KEY", "")
        self._florence_model = None
        self._florence_processor = None
        self._florence_attempted = False

    def is_local_model_ready(self) -> bool:
        """Check if local Florence-2 model weights exist and are ready."""
        weights_file = self.model_dir / "pytorch_model.bin"
        return weights_file.is_file() and weights_file.stat().st_size > 400 * 1024 * 1024

    def load_local_model(self) -> bool:
        """Lazy-loads local Florence-2 model when weights are present."""
        if self._florence_model is not None:
            return True
        if self._florence_attempted:
            return False

        self._florence_attempted = True
        if not self.is_local_model_ready():
            return False

        try:
            from transformers import AutoProcessor, AutoModelForCausalLM
            import torch

            print(f"[VISION] Loading local Florence-2 model from {self.model_dir}...")
            self._florence_processor = AutoProcessor.from_pretrained(str(self.model_dir), trust_remote_code=True)
            self._florence_model = AutoModelForCausalLM.from_pretrained(
                str(self.model_dir),
                trust_remote_code=True,
                torch_dtype=torch.float32,
            )
            self._florence_model.eval()
            print("[VISION] Local Florence-2 vision model loaded successfully into RAM/CPU.")
            return True
        except Exception as e:
            print(f"[VISION] Warning: Could not load local Florence-2 model: {e}")
            return False

    def capture_window_image(self, hwnd: int) -> Tuple[Optional[Any], Tuple[int, int, int, int]]:
        """
        Captures the visible window pixels safely via CPower C11 daemon.
        Guarantees that protected windows (Antigravity, IDEs) are NEVER captured.
        """
        if not hwnd:
            raise RuntimeError(f"[VISION] Invalid window handle: {hwnd}")
        if isinstance(hwnd, str):
            hwnd = int(hwnd, 16) if (hwnd.startswith("0x") or hwnd.startswith("0X")) else int(hwnd)
        else:
            hwnd = int(hwnd)

        pid = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        buf = ctypes.create_unicode_buffer(512)
        _user32.GetWindowTextW(hwnd, buf, 512)

        if is_protected_window(hwnd, pid.value, buf.value):
            raise PermissionError(f"[VISION GUARD] Refusing to capture protected window {hwnd:#x} ('{buf.value}')")

        bounds = get_window_bounds(hwnd)
        if not bounds:
            raise RuntimeError(f"[VISION] Cannot acquire window rect for hwnd {hwnd:#x}")

        left, top, right, bottom = bounds
        if right <= left or bottom <= top:
            raise RuntimeError(f"[VISION] Window {hwnd:#x} has zero or negative dimensions")

        # Capture via CPower native C11 daemon (high-speed DXGI/GDI)
        ack, png_bytes = self.client.capture_micro_region(left, top, right, bottom, fmt="png")
        if not png_bytes or not _PIL_AVAILABLE:
            return None, bounds

        image = Image.open(io.BytesIO(png_bytes)).convert("RGB")
        return image, bounds

    def detect_visual_elements(
        self,
        image: Any,
        window_bounds: Tuple[int, int, int, int],
        titlebar_guard_px: int = 125,
    ) -> List[VisualMark]:
        """
        Fast Computer Vision Contour & Interactive Region Detector (sub-30ms).
        Identifies clickable boxes, icon buttons, search bars, and controls
        even if Accessibility (UIA) tree is completely empty (e.g. Canvas/WebGL).
        """
        if not _CV2_AVAILABLE or not _PIL_AVAILABLE:
            return []

        win_left, win_top, win_right, win_bottom = window_bounds
        win_w = win_right - win_left
        win_h = win_bottom - win_top

        # Convert PIL to OpenCV BGR
        cv_img = cv2.cvtColor(np.array(image), cv2.COLOR_RGB2BGR)
        gray = cv2.cvtColor(cv_img, cv2.COLOR_BGR2GRAY)

        # Bilateral filter to smooth noise while keeping crisp UI edges
        filtered = cv2.bilateralFilter(gray, 7, 50, 50)
        edges = cv2.Canny(filtered, 50, 150)

        # Morphological close to bridge text characters and button borders into single contours
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (9, 5))
        closed = cv2.morphologyEx(edges, cv2.MORPH_CLOSE, kernel)

        contours, _ = cv2.findContours(closed, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        marks: List[VisualMark] = []
        mark_idx = 1

        for cnt in contours:
            x, y, w, h = cv2.boundingRect(cnt)

            # Filter out browser toolbar / titlebar region
            screen_y = win_top + y
            if screen_y < win_top + titlebar_guard_px:
                continue

            # UI element size heuristics
            if w < 18 or h < 14:
                continue  # Too small (noise/dot)
            if w > win_w * 0.85 or h > win_h * 0.85:
                continue  # Too large (entire viewport/panel)

            aspect = w / float(h)
            label = "button_or_control"
            if aspect > 4.0 and h < 45:
                label = "input_field_or_banner"
            elif 0.7 <= aspect <= 1.4 and w <= 48 and h <= 48:
                label = "icon_button"

            screen_x = win_left + x
            marks.append(VisualMark(mark_idx, screen_x, screen_y, w, h, label=label))
            mark_idx += 1

        # Sort top-to-bottom, left-to-right
        marks.sort(key=lambda m: (m.cy // 30, m.cx))
        # Re-index marks cleanly
        for i, m in enumerate(marks, start=1):
            m.mark_id = i

        return marks

    def ground_phrase_florence(
        self,
        image: Any,
        phrase: str,
        window_bounds: Tuple[int, int, int, int],
    ) -> Optional[Tuple[int, int]]:
        """
        Uses local Florence-2 model for visual grounding (<CAPTION_TO_PHRASE_GROUNDING>).
        Finds the exact (cx, cy) screen coordinates of the described phrase/icon.
        """
        if not self.load_local_model():
            return None

        import torch

        orig_w, orig_h = image.size
        square_dim = max(orig_w, orig_h, 768)
        square_img = Image.new("RGB", (square_dim, square_dim), (255, 255, 255))
        square_img.paste(image, (0, 0))
        resized_img = square_img.resize((768, 768), Image.Resampling.BILINEAR)

        prompt = f"<CAPTION_TO_PHRASE_GROUNDING> {phrase}"
        inputs = self._florence_processor(text=prompt, images=resized_img, return_tensors="pt")

        with torch.no_grad():
            generated_ids = self._florence_model.generate(
                input_ids=inputs["input_ids"],
                pixel_values=inputs["pixel_values"],
                max_new_tokens=128,
                num_beams=1,
                do_sample=False,
                use_cache=False,
            )

        generated_text = self._florence_processor.batch_decode(generated_ids, skip_special_tokens=False)[0]
        parsed = self._florence_processor.post_process_generation(
            generated_text,
            task="<CAPTION_TO_PHRASE_GROUNDING>",
            image_size=(768, 768),
        )

        grounding = parsed.get("<CAPTION_TO_PHRASE_GROUNDING>", {})
        bboxes = grounding.get("bboxes", [])
        if not bboxes:
            return None

        # Take first matched bounding box and scale back to original coordinate frame
        scale = square_dim / 768.0
        box = bboxes[0]
        x1, y1, x2, y2 = [c * scale for c in box]
        win_left, win_top, _, _ = window_bounds
        cx = win_left + int((x1 + x2) / 2)
        cy = win_top + int((y1 + y2) / 2)
        return (cx, cy)

    def query_vlm_zero_shot(
        self,
        image: Any,
        prompt: str,
    ) -> Optional[str]:
        """
        Deep Multimodal Zero-Shot Fallback using NVIDIA NIM Super-VL (Nemotron 3.5 Super VL 120B).
        Delivers world-class visual comprehension when exploring totally unfamiliar interfaces.
        """
        if not self.nvidia_api_key:
            return None

        # Compress image to JPEG base64
        buf = io.BytesIO()
        image.save(buf, format="JPEG", quality=85)
        b64_img = base64.b64encode(buf.getvalue()).decode("utf-8")
        image_data_uri = f"data:image/jpeg;base64,{b64_img}"

        url = "https://integrate.api.nvidia.com/v1/chat/completions"
        payload = {
            "model": "nvidia/nemotron-3.5-super-vl-120b-a12b",
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {"type": "image_url", "image_url": {"url": image_data_uri}},
                    ],
                }
            ],
            "max_tokens": 512,
            "temperature": 0.1,
        }

        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {self.nvidia_api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )

        try:
            with urllib.request.urlopen(req, timeout=25.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            choice = data["choices"][0]["message"]
            return choice.get("content") or choice.get("reasoning_content")
        except Exception as e:
            print(f"[VISION VLM] NVIDIA NIM Super-VL query error: {e}")
            return None
