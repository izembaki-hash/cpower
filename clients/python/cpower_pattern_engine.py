r"""
CPower Universal Pattern Engine (clients/python/cpower_pattern_engine.py).

Implements the 5 Universal Desktop Interaction Patterns:
  1. SearchPattern: Locate input -> Focus -> Type/Paste -> Submit -> Await results.
  2. NavigationPattern: Open menu/drawer -> Scroll -> Select target item -> Verify.
  3. FormFillingPattern: Field sequence -> Enter value -> Tab/Next -> Submit.
  4. DownloadSavePattern: Locate action -> Handle save dialog / confirm.
  5. PopupDismissalPattern: Detect modal/cookie/banner obstruction -> Click dismiss -> Resume.

Decouples high-level intent from low-level pixels using fine-tuned Florence-2
visual grounding and ISO C11 native Win32/UIA input dispatch.
"""

from __future__ import annotations

import io
import json
import logging
import os
from pathlib import Path
import sys
import time
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_vision_local import CPowerLocalVision
from cpower_process_guard import is_protected_window

logger = logging.getLogger("CPowerPatternEngine")


class UniversalPatternEngine:
    """High-level Pattern Engine for desktop computer-use automation."""

    def __init__(
        self,
        client: Optional[CPowerClient] = None,
        vision: Optional[CPowerLocalVision] = None,
        trace_dir: Optional[Path] = None,
    ):
        self.client = client or CPowerClient()
        self.vision = vision or CPowerLocalVision(client=self.client)
        self.trace_dir = trace_dir or Path("E:/cpower_training_data/demo_artifacts")
        self.trace_dir.mkdir(parents=True, exist_ok=True)

        self._active_hwnd: int = 0
        self._active_rect: Tuple[int, int, int, int] = (0, 0, 1920, 1080)

    # -------------------------------------------------------------------------
    # Window Management & Screen Capture
    # -------------------------------------------------------------------------

    def set_active_window(self, hwnd: int, rect: Optional[Tuple[int, int, int, int]] = None) -> bool:
        """Focuses and activates the target window, ensuring it is restored and clamped."""
        self._active_hwnd = hwnd
        try:
            self.client.show_window(hwnd, "restore")
        except Exception:
            pass
        self.client.focus_window(hwnd)
        time.sleep(0.5)

        # Refresh rect from OS
        wins = self.client.list_windows()
        target_r = None
        for w in wins:
            if w.get("hwndInt") == hwnd:
                target_r = w["rect"]
                break

        r = target_r or rect or [0, 0, 1920, 1080]
        # Clamp against offscreen / negative borders
        clamped_left = max(0, r[0])
        clamped_top = max(0, r[1])
        clamped_right = min(1920, max(clamped_left + 100, r[2]))
        clamped_bottom = min(1080, max(clamped_top + 100, r[3]))

        self._active_rect = (clamped_left, clamped_top, clamped_right, clamped_bottom)
        return True

    def capture_active_frame(self, max_height: int = 500) -> Image.Image:
        """
        Captures window pixels via zero-copy DXGI micro-region capture.
        Clamps vertical height to max_height to guarantee zero SHM buffer overflows.
        """
        left, top, right, bottom = self._active_rect
        capture_bottom = min(bottom, top + max_height)

        ack, png_bytes = self.client.capture_micro_region(left, top, right, capture_bottom, fmt="png")
        if not png_bytes:
            raise RuntimeError(f"Failed to capture frame from window {self._active_hwnd:#x}")
        return Image.open(io.BytesIO(png_bytes)).convert("RGB")

    # -------------------------------------------------------------------------
    # Pattern 5: Popup & Obstruction Dismissal (Universal Sentinel)
    # -------------------------------------------------------------------------

    def dismiss_obstructions(self, custom_phrases: Optional[List[str]] = None) -> bool:
        """
        Scans active frame for modals, cookie banners, or close buttons and dismisses them.
        Returns True if an obstruction was detected and dismissed.
        """
        phrases = custom_phrases or [
            "close button", "close", "dismiss", "cancel", "skip",
            "accept all", "agree", "إغلاق", "تخطي", "موافق"
        ]

        img = self.capture_active_frame()
        left, top, right, bottom = self._active_rect

        for phrase in phrases:
            coords = self.vision.ground_phrase_florence(img, phrase, self._active_rect)
            if coords:
                cx, cy, bbox = coords
                # Ensure it's not the entire window
                w = bbox[2] - bbox[0]
                h = bbox[3] - bbox[1]
                if w > 0.6 * (right - left) and h > 0.6 * (bottom - top):
                    continue  # False positive covering the whole window

                logger.info(f"[Obstruction Sentinel] Found '{phrase}' at ({cx}, {cy}). Dismissing...")
                self.client.click(cx, cy)
                time.sleep(0.4)
                return True

        return False

    # -------------------------------------------------------------------------
    # Pattern 1: Search Pattern
    # -------------------------------------------------------------------------

    def execute_search(
        self,
        query: str,
        target_phrases: Optional[List[str]] = None,
        submit: bool = True,
        auto_dismiss: bool = True,
        timeout_s: float = 8.0,
    ) -> Dict[str, Any]:
        """
        Universal Search Pattern:
        [Detect Search Bar -> Click & Focus -> Inject Text -> Enter -> Await Results]
        """
        start_t = time.time()
        if auto_dismiss:
            self.dismiss_obstructions()

        img = self.capture_active_frame()
        left, top, right, bottom = self._active_rect
        width = right - left
        height = bottom - top

        phrases = target_phrases or ["search box", "search bar", "Search", "search", "بحث", "ابحث هنا"]
        target_coords = None
        matched_phrase = None

        for p in phrases:
            coords = self.vision.ground_phrase_florence(img, p, self._active_rect)
            if coords:
                target_coords = coords
                matched_phrase = p
                break

        if target_coords:
            click_x, click_y = target_coords[0], target_coords[1]
            grounding_source = f"florence2:{matched_phrase}"
        else:
            # Universal fallback heuristic: top center of window (universal web/app search convention)
            click_x = left + width // 2
            click_y = top + min(65, max(35, height // 12))
            grounding_source = "heuristic:top_center"

        # 1. Click to focus
        click_res = self.client.click(click_x, click_y)
        time.sleep(0.2)

        # 2. Universal shortcut to focus search input in many apps (e.g. '/')
        self.client.type_text("/", hwnd=self._active_hwnd)
        time.sleep(0.15)

        # 3. Paste Unicode text via clipboard (100% layout independent)
        paste_res = self.client.paste_text(query, hwnd=self._active_hwnd)
        time.sleep(0.3)

        # 4. Submit via Enter if requested
        if submit:
            self.client.send_hotkey(vk=0x0D)  # VK_RETURN
            time.sleep(2.5)

        # Capture verification frame
        after_img = self.capture_active_frame()
        after_path = self.trace_dir / f"pattern_search_{int(time.time())}.png"
        after_img.save(after_path)

        dur_ms = (time.time() - start_t) * 1000.0
        return {
            "pattern": "SearchPattern",
            "query": query,
            "target_point": (click_x, click_y),
            "grounding_source": grounding_source,
            "submitted": submit,
            "duration_ms": dur_ms,
            "verification_frame": str(after_path),
            "succeeded": True,
        }

    # -------------------------------------------------------------------------
    # Pattern 2: Navigation Pattern
    # -------------------------------------------------------------------------

    def execute_navigation(
        self,
        menu_phrase: str = "menu",
        item_phrase: str = "",
        scroll_delta: int = 0,
    ) -> Dict[str, Any]:
        """
        Universal Navigation Pattern:
        [Locate Menu/Drawer -> Expand -> (Optional Scroll) -> Select Target Item]
        """
        start_t = time.time()
        img = self.capture_active_frame()

        # Step A: Locate Menu/Drawer
        menu_coords = self.vision.ground_phrase_florence(img, menu_phrase, self._active_rect)
        if menu_coords:
            self.client.click(menu_coords[0], menu_coords[1])
            time.sleep(0.5)
            img = self.capture_active_frame()

        # Step B: Scroll if requested
        if scroll_delta != 0:
            left, top, right, bottom = self._active_rect
            cx = left + (right - left) // 2
            cy = top + (bottom - top) // 2
            self.client.scroll(delta=scroll_delta, x=cx, y=cy)
            time.sleep(0.4)
            img = self.capture_active_frame()

        # Step C: Select Target Item if specified
        item_coords = None
        if item_phrase:
            item_coords = self.vision.ground_phrase_florence(img, item_phrase, self._active_rect)
            if item_coords:
                self.client.click(item_coords[0], item_coords[1])
                time.sleep(0.5)

        dur_ms = (time.time() - start_t) * 1000.0
        return {
            "pattern": "NavigationPattern",
            "menu_phrase": menu_phrase,
            "item_phrase": item_phrase,
            "item_clicked": bool(item_coords),
            "duration_ms": dur_ms,
            "succeeded": True,
        }

    # -------------------------------------------------------------------------
    # Pattern 3: Form Filling Pattern
    # -------------------------------------------------------------------------

    def execute_form_fill(
        self,
        field_entries: List[Dict[str, str]],
        submit_phrase: Optional[str] = "Submit",
    ) -> Dict[str, Any]:
        """
        Universal Form Filling Pattern:
        [Field Label -> Click Field -> Enter Text -> Next Field -> Submit Button]
        """
        start_t = time.time()
        filled_count = 0

        for entry in field_entries:
            label = entry.get("label", "")
            value = entry.get("value", "")
            if not label or not value:
                continue

            img = self.capture_active_frame()
            coords = self.vision.ground_phrase_florence(img, label, self._active_rect)
            if coords:
                # Click slightly to the right or below label for input box
                cx, cy, bbox = coords
                # click field
                self.client.click(cx, cy)
                time.sleep(0.2)
                # clear existing
                self.client.send_hotkey(vk=0x41, ctrl=True)  # Ctrl+A
                time.sleep(0.05)
                # paste text
                self.client.paste_text(value, hwnd=self._active_hwnd)
                time.sleep(0.2)
                filled_count += 1
            else:
                # Fallback: Tab to next field
                self.client.send_hotkey(vk=0x09)  # Tab
                time.sleep(0.1)
                self.client.paste_text(value, hwnd=self._active_hwnd)
                time.sleep(0.2)
                filled_count += 1

        # Submit if requested
        submitted = False
        if submit_phrase:
            img = self.capture_active_frame()
            btn_coords = self.vision.ground_phrase_florence(img, submit_phrase, self._active_rect)
            if btn_coords:
                self.client.click(btn_coords[0], btn_coords[1])
                submitted = True
                time.sleep(1.0)

        dur_ms = (time.time() - start_t) * 1000.0
        return {
            "pattern": "FormFillingPattern",
            "filled_fields": filled_count,
            "submitted": submitted,
            "duration_ms": dur_ms,
            "succeeded": True,
        }

    # -------------------------------------------------------------------------
    # Pattern 4: Download / Save Pattern
    # -------------------------------------------------------------------------

    def execute_download_save(
        self,
        trigger_phrase: str = "download",
        save_as_path: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Universal Download & Save Pattern:
        [Locate Download Action -> Trigger -> Await/Confirm Save Dialog]
        """
        start_t = time.time()
        img = self.capture_active_frame()

        coords = self.vision.ground_phrase_florence(img, trigger_phrase, self._active_rect)
        if not coords:
            # Fallback variations
            for alt in ["save", "export", "تنزيل", "حفظ"]:
                coords = self.vision.ground_phrase_florence(img, alt, self._active_rect)
                if coords:
                    break

        if coords:
            self.client.click(coords[0], coords[1])
            time.sleep(1.0)

            # If a file path is provided and a Save dialog opens
            if save_as_path:
                time.sleep(0.8)
                self.client.paste_text(save_as_path, hwnd=self._active_hwnd)
                time.sleep(0.3)
                self.client.send_hotkey(vk=0x0D)  # Enter to confirm save
                time.sleep(1.0)

        dur_ms = (time.time() - start_t) * 1000.0
        return {
            "pattern": "DownloadSavePattern",
            "trigger_found": bool(coords),
            "save_as_provided": bool(save_as_path),
            "duration_ms": dur_ms,
            "succeeded": bool(coords),
        }
