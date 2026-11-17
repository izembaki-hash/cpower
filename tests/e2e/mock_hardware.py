"""
Hardware & Environment Mocking Utilities for CPower E2E Testing.

Provides:
  - MockDiskController: Dynamic monkeypatching of shutil.disk_usage for disk safety testing.
  - SyntheticUIProvider: Deterministic PIL RGB generators for Explorer, YouTube, Dialog, and Arabic RTL interfaces.
  - ArabicSampleCorpus: Canonical Arabic test strings across pure RTL, mixed bidi, diacritics, and numerals.
  - LightweightProcessorStub: Fast quantization and token formatting stub for Florence-2.
  - ZeroMouseTracker: Verification spy asserting zero physical mouse cursor movement APIs are called.
"""

from __future__ import annotations

import collections
import os
from pathlib import Path
import re
import shutil
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Tuple, Union

from PIL import Image, ImageDraw, ImageFont
import arabic_reshaper
from bidi.algorithm import get_display

DiskUsage = collections.namedtuple("usage", ["total", "used", "free"])


def get_system_font(size: int = 16) -> ImageFont.ImageFont:
    """Loads a crisp system font (Segoe UI / Arial / Tahoma) or falls back to default."""
    for font_name in ("segoeui.ttf", "arial.ttf", "tahoma.ttf"):
        font_path = Path("C:/Windows/Fonts") / font_name
        if font_path.is_file():
            try:
                return ImageFont.truetype(str(font_path), size=size)
            except Exception:
                pass
    return ImageFont.load_default()


class MockDiskController:
    """
    Dynamic monkeypatching controller for shutil.disk_usage.
    Allows tests to simulate arbitrary storage conditions (e.g. 500 MB, 1.0 GB, 50 GB)
    for specific paths or globally.
    """

    def __init__(self, monkeypatch=None):
        self._monkeypatch = monkeypatch
        self._overrides: Dict[str, DiskUsage] = {}
        self._default_override: Optional[DiskUsage] = None
        self._original_disk_usage = shutil.disk_usage
        self._installed = False

        if monkeypatch:
            self.install(monkeypatch)

    def install(self, monkeypatch) -> None:
        """Installs the patched disk_usage function using pytest's monkeypatch."""
        self._monkeypatch = monkeypatch
        self._monkeypatch.setattr(shutil, "disk_usage", self._mocked_disk_usage)
        self._installed = True

    def set_free_space(
        self,
        target_path: Optional[Union[str, Path]],
        free_bytes: int,
        total_bytes: Optional[int] = None,
    ) -> None:
        """Sets the free space reported for target_path, or globally if target_path is None."""
        if total_bytes is None:
            total_bytes = max(free_bytes * 2, 100 * 1024 * 1024 * 1024)  # Default 100 GB
        used_bytes = max(0, total_bytes - free_bytes)
        usage = DiskUsage(total=total_bytes, used=used_bytes, free=free_bytes)

        if target_path is None:
            self._default_override = usage
        else:
            resolved = str(Path(target_path).resolve()).lower()
            self._overrides[resolved] = usage

    def set_free_gb(
        self,
        target_path: Optional[Union[str, Path]],
        free_gb: float,
        total_gb: float = 100.0,
    ) -> None:
        """Convenience method to set free space in gigabytes."""
        free_b = int(free_gb * 1024 * 1024 * 1024)
        total_b = int(total_gb * 1024 * 1024 * 1024)
        self.set_free_space(target_path, free_b, total_b)

    def clear(self) -> None:
        """Clears all configured overrides."""
        self._overrides.clear()
        self._default_override = None

    def _mocked_disk_usage(self, path: Union[str, Path]) -> DiskUsage:
        path_str = str(Path(path).resolve()).lower()

        # Check exact or prefix path match (longest specific match first)
        sorted_keys = sorted(self._overrides.keys(), key=len, reverse=True)
        for key in sorted_keys:
            norm_key = key.rstrip("\\").rstrip("/")
            prefix = norm_key + "\\"
            if (
                path_str == key
                or path_str == norm_key
                or path_str.startswith(prefix)
                or path_str.startswith(norm_key + "/")
            ):
                return self._overrides[key]

        if self._default_override is not None:
            return self._default_override

        return self._original_disk_usage(path)


class ArabicSampleCorpus:
    """Canonical Arabic corpus for UI synthesis, testing, and boundary stress."""

    # Pure Arabic strings (RTL)
    PURE_RTL = [
        "حفظ",
        "إلغاء الأمر",
        "استعراض الملفات",
        "المشاريع البرمجية",
        "مجلد التدريب الذاتي",
        "تشغيل الفيديو بجودة عالية",
        "تأكيد العملية ومتابعة الحفظ",
    ]

    # Mixed Bidirectional strings (Arabic + LTR numbers or English paths)
    MIXED_BIDI = [
        "القرص المحلي (C:)",
        "القرص المحلي (E:)",
        "المجلد C:\\CPower\\Data رقم 1 تم حفظه بنجاح",
        "تقرير شهر مارس 2026 النهائي",
        "حفظ باسم — Save As",
        "بحث في Google Chrome عن Cheb Bilal",
        "ملف configuration.json الخاص بالنظام",
    ]

    # Diacritics Heavy (Tashkeel)
    DIACRITICS_HEAVY = [
        "بِسْمِ اللَّهِ الرَّحْمَٰنِ الرَّحِيمِ",
        "تَدْرِيبُ النَّمُوذَجِ عَلَى بَيَانَاتِ الْوَاجِهَةِ",
        "حِفْظُ الْمِلَفِّ فِي الْقُرْصِ الْمَحَلِّيِّ",
        "الْفَحْصُ الشَّامِلُ لِلْمَنْظُومَةِ",
    ]

    # Eastern Arabic Numerals (Hindi numerals)
    EASTERN_NUMERALS = [
        "١٢٣٤٥٦٧٨٩٠",
        "التقرير رقم ٤٥٦ لسنة ٢٠٢٦",
        "الصفحة رقم ١٢ من أصل ٩٩",
    ]

    # Ligatures & Special Unicode Sequences
    LIGATURES = [
        "لا إله إلا الله",
        "الإعلانات والمقالات الإسلامية",
        "لإلغاء الأمر اضغط هنا",
    ]

    # Extremes & Edge Cases
    TATWEEL_KASHIDA = "تـــــدريــــب"
    SINGLE_CHAR = "ع"
    EMPTY = ""
    WHITESPACE = "   \t\n  "
    PUNCTUATION = "هل تم الحفظ بنجاح؟ نعم؛ تم ذلك، شكراً."
    LONG_PARAGRAPH = "يقوم وكيل CPower بجمع بيانات واجهات المستخدم لنظام ويندوز 11 " * 20

    @staticmethod
    def reshape_and_bidi(text: str) -> str:
        """Transforms raw Arabic Unicode into cursive connected and visually reordered glyphs."""
        if not text:
            return ""
        reshaped = arabic_reshaper.reshape(text)
        return get_display(reshaped)


class SyntheticUIProvider:
    """Deterministic PIL RGB Canvas generator for diverse UI domains."""

    def __init__(self):
        self.font_large = get_system_font(22)
        self.font_medium = get_system_font(16)
        self.font_small = get_system_font(13)

    def create_explorer_ui(
        self,
        width: int = 1024,
        height: int = 680,
        target_folder: str = "بيانات التدريب CPower",
    ) -> Tuple[Image.Image, Tuple[int, int, int, int], str]:
        """
        Synthesizes a Windows 11 Explorer interface.
        Returns: (image, target_bbox_px, phrase)
        """
        img = Image.new("RGB", (width, height), (248, 249, 250))
        draw = ImageDraw.Draw(img)

        # Header / Ribbon
        draw.rectangle([0, 0, width, 50], fill=(238, 240, 244))
        draw.line([(0, 50), (width, 50)], fill=(210, 215, 222), width=1)

        # Address bar
        draw.rectangle([60, 10, 680, 40], fill=(255, 255, 255), outline=(190, 195, 205), width=1)
        addr_text = ArabicSampleCorpus.reshape_and_bidi("هذا الكمبيوتر الشخصي > القرص المحلي (E:)")
        draw.text((75, 16), addr_text, fill=(30, 30, 30), font=self.font_small)

        # Sidebar
        draw.rectangle([0, 51, 220, height], fill=(242, 244, 248))
        draw.line([(220, 51), (220, height)], fill=(215, 220, 228), width=1)

        # Target folder card in main view
        target_bbox = (260, 190, 390, 270)
        fx1, fy1, fx2, fy2 = target_bbox
        draw.rectangle([fx1, fy1, fx2, fy2], fill=(225, 235, 255), outline=(60, 120, 240), width=2)
        draw.rectangle([fx1 + 15, fy1 + 15, fx1 + 45, fy1 + 40], fill=(245, 180, 50))
        folder_display = ArabicSampleCorpus.reshape_and_bidi(target_folder[:12])
        draw.text((fx1 + 10, fy2 - 25), folder_display, fill=(20, 20, 20), font=self.font_small)

        phrase = f"مجلد {target_folder}"
        return img, target_bbox, phrase

    def create_youtube_ui(
        self,
        width: int = 1100,
        height: int = 700,
        query: str = "الشاب بلال جديد",
    ) -> Tuple[Image.Image, Tuple[int, int, int, int], str]:
        """
        Synthesizes a YouTube media interface with dark theme aesthetics.
        Returns: (image, target_bbox_px, phrase)
        """
        img = Image.new("RGB", (width, height), (15, 15, 15))
        draw = ImageDraw.Draw(img)

        # Top Bar
        draw.rectangle([0, 0, width, 56], fill=(33, 33, 33))
        draw.text((30, 15), "YouTube", fill=(255, 0, 0), font=self.font_large)

        # Search Bar
        draw.rectangle([280, 10, 750, 46], fill=(18, 18, 18), outline=(60, 60, 60), width=1)
        search_btn_bbox = (750, 10, 810, 46)
        draw.rectangle(search_btn_bbox, fill=(45, 45, 45), outline=(60, 60, 60), width=1)
        btn_label = ArabicSampleCorpus.reshape_and_bidi("بحث")
        draw.text((765, 18), btn_label, fill=(220, 220, 220), font=self.font_small)

        # Video Card Target
        video_card_bbox = (40, 90, 360, 310)
        cx1, cy1, cx2, cy2 = video_card_bbox
        draw.rectangle([cx1, cy1, cx2, cy1 + 160], fill=(50, 50, 60))
        draw.rectangle([cx2 - 50, cy1 + 135, cx2 - 10, cy1 + 155], fill=(0, 0, 0))
        draw.text((cx2 - 45, cy1 + 138), "04:15", fill=(255, 255, 255), font=self.font_small)
        vid_title = ArabicSampleCorpus.reshape_and_bidi(f"Cheb Bilal - {query}")
        draw.text((cx1, cy1 + 170), vid_title[:35], fill=(240, 240, 240), font=self.font_small)

        phrase = f"فيديو {query}"
        return img, video_card_bbox, phrase

    def create_dialog_ui(
        self,
        width: int = 800,
        height: int = 500,
        target_button: str = "حفظ",
    ) -> Tuple[Image.Image, Tuple[int, int, int, int], str]:
        """
        Synthesizes a Windows System Save As Dialog.
        Returns: (image, target_bbox_px, phrase)
        """
        img = Image.new("RGB", (width, height), (240, 240, 240))
        draw = ImageDraw.Draw(img)

        # Header
        draw.rectangle([0, 0, width, 36], fill=(255, 255, 255))
        header_text = ArabicSampleCorpus.reshape_and_bidi("حفظ باسم — Save As")
        draw.text((15, 8), header_text, fill=(0, 0, 0), font=self.font_medium)

        buttons = {
            "حفظ": ((480, 420, 580, 460), (0, 120, 215), (255, 255, 255)),
            "إلغاء الأمر": ((600, 420, 720, 460), (230, 230, 230), (0, 0, 0)),
            "استعراض...": ((690, 115, 760, 145), (230, 230, 230), (0, 0, 0)),
        }

        target_bbox = buttons.get(target_button, buttons["حفظ"])[0]

        for name, ((bx1, by1, bx2, by2), bg, tc) in buttons.items():
            draw.rectangle([bx1, by1, bx2, by2], fill=bg, outline=(160, 160, 160), width=1)
            b_label = ArabicSampleCorpus.reshape_and_bidi(name)
            draw.text((bx1 + 15, by1 + 10), b_label, fill=tc, font=self.font_small)

        phrase = f"زر {target_button}"
        return img, target_bbox, phrase

    def create_arabic_rtl_ui(
        self,
        width: int = 1024,
        height: int = 600,
        title: str = "نظام إدارة الملفات الذكي",
    ) -> Tuple[Image.Image, Dict[str, Tuple[int, int, int, int]]]:
        """
        Synthesizes a full Arabic RTL desktop application with right-anchored ribbons and controls.
        Returns: (image, dict of element_name -> bbox)
        """
        img = Image.new("RGB", (width, height), (250, 250, 252))
        draw = ImageDraw.Draw(img)

        # RTL Navigation Ribbon (Right-Anchored)
        draw.rectangle([0, 0, width, 60], fill=(41, 128, 185))
        title_shaped = ArabicSampleCorpus.reshape_and_bidi(title)
        draw.text((width - 250, 18), title_shaped, fill=(255, 255, 255), font=self.font_medium)

        elements: Dict[str, Tuple[int, int, int, int]] = {}

        # RTL Buttons aligned from right to left
        btn_save = (width - 120, 10, width - 20, 50)
        draw.rectangle(btn_save, fill=(39, 174, 96), outline=(255, 255, 255), width=1)
        draw.text((width - 95, 20), ArabicSampleCorpus.reshape_and_bidi("حفظ"), fill=(255, 255, 255), font=self.font_small)
        elements["زر_حفظ"] = btn_save

        btn_cancel = (width - 240, 10, width - 130, 50)
        draw.rectangle(btn_cancel, fill=(192, 57, 43), outline=(255, 255, 255), width=1)
        draw.text((width - 215, 20), ArabicSampleCorpus.reshape_and_bidi("إلغاء"), fill=(255, 255, 255), font=self.font_small)
        elements["زر_إلغاء"] = btn_cancel

        # RTL Form Input
        lbl_x = width - 150
        draw.text((lbl_x, 120), ArabicSampleCorpus.reshape_and_bidi("اسم المشروع:"), fill=(40, 40, 40), font=self.font_small)
        input_bbox = (width - 550, 115, width - 160, 145)
        draw.rectangle(input_bbox, fill=(255, 255, 255), outline=(180, 180, 180), width=1)
        draw.text((width - 530, 122), ArabicSampleCorpus.reshape_and_bidi("مشروع_ذكاء_اصطناعي_2026"), fill=(0, 0, 0), font=self.font_small)
        elements["حقل_اسم_المشروع"] = input_bbox

        return img, elements


class LightweightProcessorStub:
    """
    Fast tokenizer & quantization stub adhering exactly to Florence-2 specifications.
    Performs exact coordinate quantization and dequantization without requiring PyTorch model weights.
    """

    LOC_TOKEN_PATTERN = re.compile(
        r"^.*<loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})><loc_(0|[1-9]\d{0,2})>$",
        re.DOTALL,
    )
    SINGLE_LOC_PATTERN = re.compile(r"<loc_(0|[1-9]\d{0,2})>")

    @classmethod
    def quantize_bbox(
        cls,
        bbox: Tuple[int, int, int, int],
        image_size: Tuple[int, int],
    ) -> List[int]:
        """Quantizes pixel bbox [x1, y1, x2, y2] to discrete [0..999] Florence-2 space."""
        x1, y1, x2, y2 = bbox
        img_w, img_h = max(1, image_size[0]), max(1, image_size[1])

        nx1 = max(0, min(999, int(round(1000.0 * x1 / img_w))))
        ny1 = max(0, min(999, int(round(1000.0 * y1 / img_h))))
        nx2 = max(0, min(999, int(round(1000.0 * x2 / img_w))))
        ny2 = max(0, min(999, int(round(1000.0 * y2 / img_h))))

        return [nx1, ny1, nx2, ny2]

    @classmethod
    def format_target(
        cls,
        phrase: str,
        bbox: Tuple[int, int, int, int],
        image_size: Tuple[int, int],
    ) -> str:
        """Formats grounding target string with <loc_...> tokens."""
        nx1, ny1, nx2, ny2 = cls.quantize_bbox(bbox, image_size)
        return f"{phrase}<loc_{nx1}><loc_{ny1}><loc_{nx2}><loc_{ny2}>"

    @classmethod
    def parse_target(
        cls,
        target_str: str,
        image_size: Tuple[int, int],
    ) -> Dict[str, Any]:
        """Parses target string into phrase, normalized coordinates, and dequantized pixel bbox."""
        match = cls.LOC_TOKEN_PATTERN.match(target_str)
        if not match:
            return {"valid": False, "error": "Target does not match Florence-2 loc pattern"}

        nx1, ny1, nx2, ny2 = (int(x) for x in match.groups())

        # Check bounds
        for c in (nx1, ny1, nx2, ny2):
            if c < 0 or c > 999:
                return {"valid": False, "error": f"Coordinate {c} out of [0, 999] range"}

        # Extract phrase prefix using regex group 1 start index
        phrase = target_str[: match.start(1) - len("<loc_")]

        # Dequantize back to pixel coordinates
        img_w, img_h = image_size
        px1 = (nx1 + 0.5) * img_w / 1000.0
        py1 = (ny1 + 0.5) * img_h / 1000.0
        px2 = (nx2 + 0.5) * img_w / 1000.0
        py2 = (ny2 + 0.5) * img_h / 1000.0

        return {
            "valid": True,
            "phrase": phrase,
            "normalized_bbox": [nx1, ny1, nx2, ny2],
            "pixel_bbox": [px1, py1, px2, py2],
            "positive_area": (nx2 > nx1) and (ny2 > ny1),
        }

    @classmethod
    def is_valid_loc_token(cls, token: str) -> bool:
        """Verifies if token is a valid Florence-2 location token (<loc_0> to <loc_999>)."""
        m = re.fullmatch(r"<loc_(0|[1-9]\d{0,2})>", token)
        return m is not None


class ZeroMouseTracker:
    """
    Verification spy that tracks calls to physical OS mouse APIs.
    Asserts zero physical mouse events are dispatched.
    """

    def __init__(self):
        self.cursor_moves = 0
        self.mouse_clicks = 0
        self.send_input_calls = 0

    def record_move(self, x: int, y: int) -> None:
        self.cursor_moves += 1

    def record_click(self) -> None:
        self.mouse_clicks += 1

    def assert_zero_mouse_interactions(self) -> None:
        assert self.cursor_moves == 0, f"Hardware mouse moved {self.cursor_moves} times!"
        assert self.mouse_clicks == 0, f"Hardware mouse clicked {self.mouse_clicks} times!"
        assert self.send_input_calls == 0, f"SendInput dispatched {self.send_input_calls} times!"
