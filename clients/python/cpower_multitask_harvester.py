"""
CPower Multi-Task UI Harvester & Dataset Generator (clients/python/cpower_multitask_harvester.py).

Produces diverse, high-fidelity multimodal training samples for Microsoft Florence-2 fine-tuning
and ReAct agent action training across three primary domains:
  1. Windows 11 Explorer & System Controls (5 Parametric Archetypes + Live Background Perception).
  2. Web & YouTube Media Controls (Playwright Headless Chrome + In-Memory Pillow Fallback >100 fps).
  3. Rich Arabic RTL Layouts (Dual-Engine: Headless Chromium RTL DOM + Pillow BiDi Reshaped Canvas).

Key Architecture Guarantees:
  - 100% Non-Mouse Operation: Zero cursor movement, zero focus stealing, off-screen background execution.
  - Strict Florence-2 Grounding Alignment: [xmin, ymin, xmax, ymax] clamped strictly to [0..999] (<loc_0>..<loc_999>).
  - Degenerate box repair (x2 >= x1 + 1, y2 >= y1 + 1) and sub-pixel accuracy (< 1.0px error with AutoProcessor).
  - Storage safety on high-capacity drive (E:/cpower_training_data).
"""

from __future__ import annotations

import io
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import sys
import time
from typing import Any, Dict, List, Optional, Tuple, Union

from PIL import Image, ImageDraw, ImageFont

# Ensure imports from python client directory
CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector  # noqa: E402

# Optional imports for Arabic reshaping
try:
    import arabic_reshaper
    from bidi.algorithm import get_display

    HAS_ARABIC_RESHAPER = True
except ImportError:
    HAS_ARABIC_RESHAPER = False

# Optional import for Playwright
try:
    from playwright.sync_api import sync_playwright, Browser, Page, Playwright

    HAS_PLAYWRIGHT = True
except ImportError:
    HAS_PLAYWRIGHT = False

# Optional import for CPower Client (Live Perception)
try:
    from cpower_client import CPowerClient

    HAS_CPOWER_CLIENT = True
except ImportError:
    HAS_CPOWER_CLIENT = False


# ---------------------------------------------------------------------------
# Font & Text Utilities
# ---------------------------------------------------------------------------

def get_ui_font(size: int = 14) -> ImageFont.ImageFont:
    """Robust multi-tier font cascade for Windows 11 UI rendering."""
    font_candidates = [
        "segoeui.ttf",   # Windows 11 Primary UI
        "seguisym.ttf",  # Windows 11 Symbols & Icons
        "tahoma.ttf",    # Standard Arabic & Multi-language
        "arial.ttf",     # Fallback TrueType
        "calibri.ttf",   # Windows TrueType
    ]
    fonts_dir = Path("C:/Windows/Fonts")
    for f in font_candidates:
        p = fonts_dir / f
        if p.is_file():
            try:
                font = ImageFont.truetype(str(p), size=size)
                return font
            except Exception:
                continue
    return ImageFont.load_default()


def get_default_font(size: int = 16) -> ImageFont.ImageFont:
    """Backwards-compatible font loader alias."""
    return get_ui_font(size=size)


def reshape_arabic(text: str) -> str:
    """
    Transforms raw Unicode Arabic strings into connected cursive Presentation Form-B glyphs
    and applies Unicode Bidirectional (BiDi) reordering for Pillow rendering on Windows.
    """
    if not HAS_ARABIC_RESHAPER or not text:
        return text
    try:
        reshaped = arabic_reshaper.reshape(text)
        return get_display(reshaped)
    except Exception:
        return text


# ---------------------------------------------------------------------------
# Coordinate Normalization & Bounding Box Utilities
# ---------------------------------------------------------------------------

def sanitize_bbox(
    bbox: Union[Tuple[int, int, int, int], List[int]],
    img_w: int,
    img_h: int,
) -> Tuple[Tuple[int, int, int, int], Tuple[int, int, int, int]]:
    """
    Ensures pixel coordinates are sorted, positive, bounded, and non-degenerate.
    Uses integer floor quantization aligned with Microsoft Florence-2 AutoProcessor.
    Returns:
        (x1, y1, x2, y2): sanitized pixel bounding box (0 <= x1 < x2 <= img_w, 0 <= y1 < y2 <= img_h)
        (nx1, ny1, nx2, ny2): normalized integer tokens strictly in [0..999] (0 <= nx1 < nx2 <= 999)
    """
    x1, y1, x2, y2 = bbox
    # Ensure correct Cartesian ordering
    if x1 > x2:
        x1, x2 = x2, x1
    if y1 > y2:
        y1, y2 = y2, y1

    img_w = max(1, int(img_w))
    img_h = max(1, int(img_h))

    # Clamp to image bounds
    x1 = max(0, min(img_w, int(round(x1))))
    y1 = max(0, min(img_h, int(round(y1))))
    x2 = max(0, min(img_w, int(round(x2))))
    y2 = max(0, min(img_h, int(round(y2))))

    # Guarantee minimum physical dimensions so box spans at least 1 bin difference
    min_w = max(2, int(math.ceil(img_w / 1000.0)) + 1)
    min_h = max(2, int(math.ceil(img_h / 1000.0)) + 1)
    if (x2 - x1) < min_w:
        if x1 + min_w <= img_w:
            x2 = x1 + min_w
        elif x2 - min_w >= 0:
            x1 = x2 - min_w
        else:
            x1 = 0
            x2 = min(img_w, min_w)

    if (y2 - y1) < min_h:
        if y1 + min_h <= img_h:
            y2 = y1 + min_h
        elif y2 - min_h >= 0:
            y1 = y2 - min_h
        else:
            y1 = 0
            y2 = min(img_h, min_h)

    # Compute normalized Florence-2 bin coordinates [0..999] using floor quantization
    nx1 = max(0, min(999, int(1000.0 * x1 / img_w)))
    ny1 = max(0, min(999, int(1000.0 * y1 / img_h)))
    nx2 = max(0, min(999, int(1000.0 * x2 / img_w)))
    ny2 = max(0, min(999, int(1000.0 * y2 / img_h)))

    # Non-degeneracy repair: strictly guarantee 0 <= nx1 < nx2 <= 999 and 0 <= ny1 < ny2 <= 999
    # Specifically if nx1 >= 999, set nx1 = 998, nx2 = 999, else nx2 = nx1 + 1. Never output <loc_1000>!
    if nx1 >= 999:
        nx1 = 998
        nx2 = 999
    elif nx2 <= nx1:
        nx2 = nx1 + 1

    if ny1 >= 999:
        ny1 = 998
        ny2 = 999
    elif ny2 <= ny1:
        ny2 = ny1 + 1

    # Mirror back to pixel coordinates to ensure clean_box matches the normalized bins
    # and remains non-degenerate
    def bin_min_px(b: int, dim: int) -> int:
        return max(0, min(dim, int(math.ceil(b * dim / 1000.0))))

    def bin_max_px(b: int, dim: int) -> int:
        if b >= 999:
            return dim
        return max(0, min(dim, int(math.floor(((b + 1) * dim - 1e-5) / 1000.0))))

    max_x1 = bin_max_px(nx1, img_w)
    min_x2 = bin_min_px(nx2, img_w)
    x1 = min(x1, max_x1)
    x2 = max(x2, min_x2)

    max_y1 = bin_max_px(ny1, img_h)
    min_y2 = bin_min_px(ny2, img_h)
    y1 = min(y1, max_y1)
    y2 = max(y2, min_y2)

    x2 = max(x1 + 1, min(img_w, x2))
    x1 = min(x2 - 1, max(0, x1))
    y2 = max(y1 + 1, min(img_h, y2))
    y1 = min(y2 - 1, max(0, y1))

    return (x1, y1, x2, y2), (nx1, ny1, nx2, ny2)


# ---------------------------------------------------------------------------
# Domain 1: Windows 11 Explorer & System Controls Synthesizer
# ---------------------------------------------------------------------------

class Win11ControlsSynthesizer:
    """
    Parametric synthetic generator for Windows 11 Explorer & System Controls.
    Generates 5 distinct archetypes with high visual realism and exact bounding boxes:
      1. Explorer Ribbon & Toolbar
      2. Explorer Navigation & Breadcrumbs
      3. Explorer Folder Grid & File List
      4. Common File Dialogs (Save As / Open)
      5. System Confirmation & Alert Dialogs
    """

    def __init__(self):
        self.font_title = get_ui_font(18)
        self.font_body = get_ui_font(14)
        self.font_small = get_ui_font(12)
        self.font_large = get_ui_font(22)

    def generate_ribbon(self, dark_mode: bool = False) -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        """Archetype 1: Explorer Ribbon & Toolbar."""
        w, h = 1024, 680
        bg_col = (32, 32, 32) if dark_mode else (249, 250, 252)
        ribbon_bg = (40, 40, 40) if dark_mode else (243, 245, 249)
        border_col = (60, 60, 60) if dark_mode else (225, 228, 235)
        text_col = (240, 240, 240) if dark_mode else (35, 35, 35)

        img = Image.new("RGB", (w, h), bg_col)
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        # Title bar
        title_bg = (28, 28, 28) if dark_mode else (240, 243, 249)
        draw.rectangle([0, 0, w, 32], fill=title_bg)
        draw.text((16, 7), "المستكشف — File Explorer", fill=text_col, font=self.font_small)

        # Min/Max/Close
        close_btn = (w - 45, 0, w, 32)
        draw.rectangle(close_btn, fill=(232, 17, 35))
        draw.text((w - 28, 8), "✕", fill=(255, 255, 255), font=self.font_small)
        components.append(("close-btn", "زر إغلاق النافذة", close_btn))

        # Command Ribbon
        draw.rectangle([0, 32, w, 82], fill=ribbon_bg)
        draw.line([(0, 82), (w, 82)], fill=border_col, width=1)

        ribbon_buttons = [
            ("new-btn", "زر جديد (New)", (16, 42, 96, 74)),
            ("cut-btn", "زر قص (Cut)", (104, 42, 168, 74)),
            ("copy-btn", "زر نسخ (Copy)", (176, 42, 248, 74)),
            ("paste-btn", "زر لصق (Paste)", (256, 42, 328, 74)),
            ("rename-btn", "زر إعادة تسمية (Rename)", (336, 42, 424, 74)),
            ("share-btn", "زر مشاركة (Share)", (432, 42, 508, 74)),
            ("delete-btn", "زر حذف (Delete)", (516, 42, 592, 74)),
            ("sort-btn", "زر فرز (Sort)", (600, 42, 672, 74)),
            ("view-btn", "زر عرض (View)", (680, 42, 752, 74)),
            ("details-btn", "زر تفاصيل (Details)", (760, 42, 836, 74)),
        ]

        for cid, phrase, box in ribbon_buttons:
            btn_bg = (50, 50, 50) if dark_mode else (255, 255, 255)
            draw.rectangle(box, fill=btn_bg, outline=border_col, width=1)
            label = phrase.split()[1] if len(phrase.split()) > 1 else phrase
            draw.text((box[0] + 8, box[1] + 8), label, fill=text_col, font=self.font_small)
            components.append((cid, phrase, box))

        # Address & Search placeholder bar
        draw.rectangle([0, 83, w, 128], fill=bg_col)
        draw.line([(0, 128), (w, 128)], fill=border_col, width=1)

        addr_box = (130, 90, 720, 120)
        draw.rectangle(addr_box, fill=(25, 25, 25) if dark_mode else (255, 255, 255), outline=border_col, width=1)
        draw.text((142, 96), "هذا الكمبيوتر الشخصي > القرص المحلي (E:) > بيانات التدريب CPower", fill=text_col, font=self.font_small)
        components.append(("address-bar", "شريط العناوين", addr_box))

        search_box = (732, 90, 1008, 120)
        draw.rectangle(search_box, fill=(25, 25, 25) if dark_mode else (255, 255, 255), outline=border_col, width=1)
        draw.text((744, 96), "بحث في بيانات التدريب CPower...", fill=(140, 140, 140), font=self.font_small)
        components.append(("search-box", "مربع البحث", search_box))

        return img, components

    def generate_breadcrumb(self, drive: str = "E:") -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        """Archetype 2: Explorer Navigation & Breadcrumbs."""
        w, h = 1024, 680
        img = Image.new("RGB", (w, h), (255, 255, 255))
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        # Top navigation area
        draw.rectangle([0, 0, w, 60], fill=(245, 246, 248))
        draw.line([(0, 60), (w, 60)], fill=(220, 224, 230), width=1)

        # Nav buttons
        nav_back = (16, 14, 52, 46)
        nav_fwd = (58, 14, 94, 46)
        nav_up = (100, 14, 136, 46)
        draw.rectangle(nav_back, fill=(255, 255, 255), outline=(215, 220, 228))
        draw.text((28, 20), "←", fill=(30, 30, 30), font=self.font_body)
        components.append(("nav-back", "زر الرجوع للخلف", nav_back))

        draw.rectangle(nav_fwd, fill=(255, 255, 255), outline=(215, 220, 228))
        draw.text((70, 20), "→", fill=(160, 160, 160), font=self.font_body)
        components.append(("nav-fwd", "زر التقدم للأمام", nav_fwd))

        draw.rectangle(nav_up, fill=(255, 255, 255), outline=(215, 220, 228))
        draw.text((112, 20), "↑", fill=(30, 30, 30), font=self.font_body)
        components.append(("nav-up", "زر الانتقال للمجلد الأعلى", nav_up))

        # Breadcrumb address bar
        addr_box = (148, 14, 750, 46)
        draw.rectangle(addr_box, fill=(255, 255, 255), outline=(190, 195, 205), width=1)
        path_str = f"هذا الكمبيوتر الشخصي > القرص المحلي ({drive}) > بيانات التدريب CPower > النماذج"
        draw.text((160, 20), path_str, fill=(35, 35, 35), font=self.font_small)
        components.append(("breadcrumb-address", "شريط مسار التنقل", addr_box))

        # Search box
        search_box = (762, 14, 1008, 46)
        draw.rectangle(search_box, fill=(255, 255, 255), outline=(190, 195, 205), width=1)
        draw.text((774, 20), f"بحث في ({drive})...", fill=(120, 120, 120), font=self.font_small)
        components.append(("search-input", "مربع البحث في المجلد", search_box))

        # Content area
        draw.rectangle([0, 61, 220, h], fill=(248, 249, 251))
        draw.line([(220, 61), (220, h)], fill=(225, 228, 235), width=1)

        return img, components

    def generate_grid(self, folder_count: int = 8) -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        """Archetype 3: Explorer Folder Grid & File List."""
        w, h = 1024, 680
        img = Image.new("RGB", (w, h), (255, 255, 255))
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        # Left Sidebar (Navigation pane)
        draw.rectangle([0, 0, 220, h], fill=(245, 247, 250))
        draw.line([(220, 0), (220, h)], fill=(225, 228, 235), width=1)

        sidebar_items = [
            ("quick-access", "الوصول السريع (Quick Access)", (12, 24, 208, 52)),
            ("desktop-node", "سطح المكتب (Desktop)", (12, 60, 208, 88)),
            ("downloads-node", "التنزيلات (Downloads)", (12, 96, 208, 124)),
            ("docs-node", "المستندات (Documents)", (12, 132, 208, 160)),
            ("drive-c", "القرص المحلي (C:)", (12, 180, 208, 208)),
            ("drive-e", "القرص المحلي (E:)", (12, 216, 208, 244)),
            ("drive-d", "القرص المحلي (D:)", (12, 252, 208, 280)),
        ]
        for cid, label, box in sidebar_items:
            draw.text((box[0] + 8, box[1] + 6), label, fill=(45, 45, 45), font=self.font_small)
            components.append((cid, label, box))

        # Folder & File items grid
        items = [
            ("المشاريع البرمجية", (240, 40, 410, 120), False),
            ("أغاني الشاب بلال", (430, 40, 600, 120), False),
            ("المستندات الهامة", (620, 40, 790, 120), False),
            ("الصور العائلية", (810, 40, 980, 120), False),
            ("بيانات التدريب CPower", (240, 140, 410, 220), False),
            ("cpower_florence2_adapted", (430, 140, 610, 220), False),
            ("dataset_florence2.jsonl", (620, 140, 800, 220), True),
            ("dataset_agent_act.jsonl", (810, 140, 990, 220), True),
            ("model_weights.bin", (240, 240, 410, 320), True),
            ("stats.json", (430, 240, 600, 320), True),
            ("README_MODEL.txt", (620, 240, 790, 320), True),
            ("backup_archive.zip", (810, 240, 980, 320), True),
        ]

        target_idx = random.randint(0, min(folder_count, len(items)) - 1)
        for i, (name, box, is_file) in enumerate(items[:folder_count]):
            is_target = (i == target_idx)
            bg = (235, 243, 255) if is_target else (255, 255, 255)
            outline = (0, 103, 192) if is_target else (225, 228, 235)
            draw.rectangle(box, fill=bg, outline=outline, width=2 if is_target else 1)

            # Icon indicator
            icon_col = (80, 140, 230) if is_file else (245, 185, 45)
            draw.rectangle([box[0] + 12, box[1] + 12, box[0] + 36, box[1] + 36], fill=icon_col)
            draw.text((box[0] + 44, box[1] + 16), name[:18], fill=(30, 30, 30), font=self.font_small)
            phrase = f"ملف {name}" if is_file else f"مجلد {name}"
            components.append((f"item-{i+1}", phrase, box))

        return img, components

    def generate_save_open_dialog(self, is_open_dialog: bool = False) -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        """Archetype 4: Common File Dialogs (Save As / Open)."""
        w, h = 800, 520
        img = Image.new("RGB", (w, h), (242, 242, 242))
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        # Window Header
        draw.rectangle([0, 0, w, 36], fill=(255, 255, 255))
        title = "فتح ملف — Open File" if is_open_dialog else "حفظ باسم — Save As"
        draw.text((16, 8), title, fill=(20, 20, 20), font=self.font_body)

        # File path & name section
        draw.text((36, 120), "اسم الملف (File name):", fill=(30, 30, 30), font=self.font_small)
        file_input_box = (200, 115, 660, 147)
        draw.rectangle(file_input_box, fill=(255, 255, 255), outline=(180, 180, 180), width=1)
        draw.text((210, 122), "cpower_model_weights.bin", fill=(0, 0, 0), font=self.font_small)
        components.append(("file-name-input", "حقل إدخال اسم الملف", file_input_box))

        browse_box = (672, 115, 764, 147)
        draw.rectangle(browse_box, fill=(230, 230, 230), outline=(160, 160, 160), width=1)
        draw.text((684, 122), "استعراض...", fill=(20, 20, 20), font=self.font_small)
        components.append(("browse-btn", "زر استعراض...", browse_box))

        # File type dropdown
        draw.text((36, 165), "نوع الملف (Save as type):", fill=(30, 30, 30), font=self.font_small)
        type_dropdown_box = (200, 160, 660, 192)
        draw.rectangle(type_dropdown_box, fill=(255, 255, 255), outline=(180, 180, 180), width=1)
        draw.text((210, 167), "ملفات النموذج (*.bin; *.pt)", fill=(0, 0, 0), font=self.font_small)
        components.append(("file-type-dropdown", "قائمة نوع الملف", type_dropdown_box))

        # Bottom buttons
        primary_btn_name = "فتح" if is_open_dialog else "حفظ"
        primary_btn_phrase = f"زر {primary_btn_name}"
        primary_btn_box = (480, 440, 590, 480)
        draw.rectangle(primary_btn_box, fill=(0, 120, 215), outline=(0, 100, 190), width=1)
        draw.text((520, 450), primary_btn_name, fill=(255, 255, 255), font=self.font_body)
        components.append(("primary-btn", primary_btn_phrase, primary_btn_box))

        cancel_btn_box = (606, 440, 720, 480)
        draw.rectangle(cancel_btn_box, fill=(230, 230, 230), outline=(170, 170, 170), width=1)
        draw.text((630, 450), "إلغاء الأمر", fill=(20, 20, 20), font=self.font_body)
        components.append(("cancel-btn", "زر إلغاء الأمر", cancel_btn_box))

        return img, components

    def generate_confirm_dialog(self, dialog_type: str = "delete") -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        """Archetype 5: System Confirmation & Alert Dialogs."""
        w, h = 600, 320
        img = Image.new("RGB", (w, h), (245, 245, 245))
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        # Title bar
        draw.rectangle([0, 0, w, 32], fill=(255, 255, 255))
        close_btn = (w - 40, 0, w, 32)
        draw.rectangle(close_btn, fill=(232, 17, 35))
        draw.text((w - 26, 8), "✕", fill=(255, 255, 255), font=self.font_small)
        components.append(("close-btn", "زر إغلاق", close_btn))

        if dialog_type == "delete":
            draw.text((16, 7), "تأكيد حذف المجلد", fill=(30, 30, 30), font=self.font_small)
            # Warning icon
            draw.ellipse([40, 80, 80, 120], fill=(235, 170, 40))
            draw.text((56, 86), "!", fill=(255, 255, 255), font=self.font_large)
            draw.text((100, 85), "هل أنت متأكد من رغبتك في حذف هذا المجلد نهائياً؟", fill=(30, 30, 30), font=self.font_body)
            draw.text((100, 115), "بيانات التدريب CPower (لا يمكن استعادة الملفات المحذوفة)", fill=(100, 100, 100), font=self.font_small)

            # Yes / No buttons
            yes_box = (340, 250, 440, 290)
            draw.rectangle(yes_box, fill=(210, 40, 40), outline=(180, 30, 30))
            draw.text((375, 260), "نعم", fill=(255, 255, 255), font=self.font_body)
            components.append(("yes-btn", "زر نعم للحذف", yes_box))

            no_box = (456, 250, 556, 290)
            draw.rectangle(no_box, fill=(230, 230, 230), outline=(170, 170, 170))
            draw.text((495, 260), "لا", fill=(30, 30, 30), font=self.font_body)
            components.append(("no-btn", "زر لا للإلغاء", no_box))

        elif dialog_type == "replace":
            draw.text((16, 7), "استبدال الملفات أو تخطيها", fill=(30, 30, 30), font=self.font_small)
            draw.text((40, 70), "يوجد بالفعل ملف بنفس الاسم في الوجهة:", fill=(30, 30, 30), font=self.font_body)
            draw.text((40, 100), "cpower_model_weights.bin (الحجم: 840 MB)", fill=(100, 100, 100), font=self.font_small)

            rep_box = (40, 160, 560, 205)
            draw.rectangle(rep_box, fill=(255, 255, 255), outline=(0, 103, 192), width=2)
            draw.text((60, 172), "استبدال الملف في الوجهة (Replace)", fill=(0, 103, 192), font=self.font_body)
            components.append(("replace-btn", "زر استبدال الملف في الوجهة", rep_box))

            skip_box = (40, 215, 560, 260)
            draw.rectangle(skip_box, fill=(255, 255, 255), outline=(180, 180, 180), width=1)
            draw.text((60, 227), "تخطي هذا الملف (Skip)", fill=(40, 40, 40), font=self.font_body)
            components.append(("skip-btn", "زر تخطي هذا الملف", skip_box))

        else:  # UAC
            draw.text((16, 7), "التحكم في حساب المستخدم (UAC)", fill=(30, 30, 30), font=self.font_small)
            draw.rectangle([0, 32, w, 140], fill=(0, 103, 192))
            draw.text((40, 55), "هل ترغب في السماح لهذا التطبيق بإجراء تغييرات على جهازك؟", fill=(255, 255, 255), font=self.font_body)
            draw.text((40, 95), "CPower AI Desktop Daemon (Authenticode Signed)", fill=(220, 235, 255), font=self.font_small)

            yes_box = (340, 250, 440, 290)
            draw.rectangle(yes_box, fill=(0, 103, 192), outline=(0, 80, 160))
            draw.text((375, 260), "نعم", fill=(255, 255, 255), font=self.font_body)
            components.append(("uac-yes", "زر نعم للموافقة", yes_box))

            no_box = (456, 250, 556, 290)
            draw.rectangle(no_box, fill=(230, 230, 230), outline=(170, 170, 170))
            draw.text((495, 260), "لا", fill=(30, 30, 30), font=self.font_body)
            components.append(("uac-no", "زر لا للرفض", no_box))

        return img, components


# ---------------------------------------------------------------------------
# Domain 2: High-Speed In-Memory Pillow YouTube Synthesizer (>100 fps)
# ---------------------------------------------------------------------------

class PillowYouTubeSynthesizer:
    """
    High-speed in-memory synthetic generator for YouTube Media controls (>100 fps).
    Produces 26 distinct interactive components with pixel-precise ground truth bounding boxes.
    """

    def __init__(self):
        self.font_logo = get_ui_font(20)
        self.font_title = get_ui_font(18)
        self.font_regular = get_ui_font(14)
        self.font_small = get_ui_font(12)
        self.font_tiny = get_ui_font(10)

    def generate(
        self,
        width: int = 1280,
        height: int = 800,
        dark_theme: bool = True,
        locale: str = "en",
    ) -> Tuple[Image.Image, List[Tuple[str, str, Tuple[int, int, int, int]]]]:
        bg_col = (15, 15, 15) if dark_theme else (249, 249, 249)
        text_primary = (241, 241, 241) if dark_theme else (15, 15, 15)
        text_secondary = (170, 170, 170) if dark_theme else (96, 96, 96)
        border_col = (45, 45, 45) if dark_theme else (220, 220, 220)

        img = Image.new("RGB", (width, height), bg_col)
        draw = ImageDraw.Draw(img)
        components: List[Tuple[str, str, Tuple[int, int, int, int]]] = []

        is_ar = (locale == "ar")

        # 1. Top Navigation Bar
        draw.rectangle([0, 0, width, 56], fill=(15, 15, 15) if dark_theme else (255, 255, 255))
        draw.line([(0, 56), (width, 56)], fill=border_col, width=1)

        # YouTube Logo
        logo_box = (width - 140, 12, width - 20, 44) if is_ar else (20, 12, 140, 44)
        draw.rectangle([logo_box[0], logo_box[1] + 4, logo_box[0] + 28, logo_box[3] - 4], fill=(255, 0, 0))
        draw.polygon([(logo_box[0] + 10, logo_box[1] + 10), (logo_box[0] + 10, logo_box[3] - 10), (logo_box[0] + 22, (logo_box[1] + logo_box[3]) // 2)], fill=(255, 255, 255))
        draw.text((logo_box[0] + 34, logo_box[1] + 4), "YouTube", fill=text_primary, font=self.font_logo)
        components.append(("yt-logo", "شعار يوتيوب" if is_ar else "YouTube logo", logo_box))

        # Search Container
        sb_x1, sb_y1, sb_x2, sb_y2 = 340, 9, 820, 47
        search_btn_w = 64
        draw.rectangle([sb_x1, sb_y1, sb_x2 - search_btn_w, sb_y2], fill=(18, 18, 18) if dark_theme else (255, 255, 255), outline=border_col, width=1)
        search_prompt = "بحث في يوتيوب..." if is_ar else "Search YouTube..."
        draw.text((sb_x1 + 16, sb_y1 + 10), reshape_arabic(search_prompt), fill=text_secondary, font=self.font_regular)
        search_input_box = (sb_x1, sb_y1, sb_x2 - search_btn_w, sb_y2)
        components.append(("search-input", "حقل البحث في يوتيوب" if is_ar else "Search input box", search_input_box))

        # Search Button
        search_btn_box = (sb_x2 - search_btn_w, sb_y1, sb_x2, sb_y2)
        draw.rectangle(search_btn_box, fill=(35, 35, 35) if dark_theme else (242, 242, 242), outline=border_col, width=1)
        draw.text((sb_x2 - 42, sb_y1 + 10), "🔍", fill=text_primary, font=self.font_regular)
        components.append(("search-btn", "زر البحث" if is_ar else "Search button", search_btn_box))

        # Voice Search Button
        voice_btn_box = (sb_x2 + 12, sb_y1, sb_x2 + 50, sb_y2)
        draw.ellipse(voice_btn_box, fill=(35, 35, 35) if dark_theme else (242, 242, 242))
        draw.text((voice_btn_box[0] + 12, voice_btn_box[1] + 9), "🎤", fill=text_primary, font=self.font_regular)
        components.append(("voice-search-btn", "زر البحث الصوتي" if is_ar else "Voice search button", voice_btn_box))

        # 2. Main Video Player Box
        pw, ph = 840, 472
        px1, py1 = 24, 76
        px2, py2 = px1 + pw, py1 + ph
        player_box = (px1, py1, px2, py2)
        draw.rectangle(player_box, fill=(0, 0, 0))
        draw.text((px1 + pw // 2 - 90, py1 + ph // 2 - 20), "▶ Video Stream 1080p", fill=(80, 80, 80), font=self.font_title)
        components.append(("video-player", "مشغل الفيديو" if is_ar else "Video player screen", player_box))

        # Player Controls Bar
        ctrl_h = 44
        ctrl_y1 = py2 - ctrl_h
        seek_y1 = ctrl_y1 - 6
        seek_y2 = seek_y1 + 4
        seek_bar_box = (px1 + 10, seek_y1, px2 - 10, seek_y2)
        draw.rectangle(seek_bar_box, fill=(80, 80, 80))
        played_w = int((seek_bar_box[2] - seek_bar_box[0]) * 0.35)
        draw.rectangle([seek_bar_box[0], seek_y1, seek_bar_box[0] + played_w, seek_y2], fill=(255, 0, 0))
        scrubber_x = seek_bar_box[0] + played_w
        scrubber_box = (scrubber_x - 5, seek_y1 - 4, scrubber_x + 5, seek_y2 + 4)
        draw.ellipse(scrubber_box, fill=(255, 0, 0))
        components.append(("seek-bar", "شريط تمرير الوقت" if is_ar else "Playback seek bar", seek_bar_box))
        components.append(("seek-scrubber", "مؤشر وقت الفيديو" if is_ar else "Timeline seek scrubber", scrubber_box))

        # Left Player Controls
        btn_y1 = ctrl_y1 + 4
        play_btn_box = (px1 + 12, btn_y1, px1 + 44, btn_y1 + 32)
        draw.text((play_btn_box[0] + 6, play_btn_box[1] + 4), "⏯", fill=(255, 255, 255), font=self.font_regular)
        components.append(("play-pause-btn", "زر تشغيل أو إيقاف الفيديو" if is_ar else "Play/Pause toggle button", play_btn_box))

        next_btn_box = (px1 + 52, btn_y1, px1 + 84, btn_y1 + 32)
        draw.text((next_btn_box[0] + 6, next_btn_box[1] + 4), "⏭", fill=(255, 255, 255), font=self.font_regular)
        components.append(("next-video-btn", "زر الفيديو التالي" if is_ar else "Next video button", next_btn_box))

        vol_btn_box = (px1 + 92, btn_y1, px1 + 124, btn_y1 + 32)
        draw.text((vol_btn_box[0] + 6, vol_btn_box[1] + 4), "🔊", fill=(255, 255, 255), font=self.font_regular)
        components.append(("volume-btn", "زر كتم وتعديل الصوت" if is_ar else "Volume mute button", vol_btn_box))

        time_box = (px1 + 134, btn_y1 + 6, px1 + 220, btn_y1 + 26)
        draw.text((time_box[0], time_box[1]), "04:15 / 12:30", fill=(220, 220, 220), font=self.font_small)
        components.append(("time-display", "مؤشر مدة الفيديو" if is_ar else "Playback time display", time_box))

        # Right Player Controls
        fs_btn_box = (px2 - 44, btn_y1, px2 - 12, btn_y1 + 32)
        draw.text((fs_btn_box[0] + 6, fs_btn_box[1] + 4), "⛶", fill=(255, 255, 255), font=self.font_regular)
        components.append(("fullscreen-btn", "زر ملء الشاشة" if is_ar else "Fullscreen toggle button", fs_btn_box))

        settings_btn_box = (px2 - 84, btn_y1, px2 - 52, btn_y1 + 32)
        draw.text((settings_btn_box[0] + 6, settings_btn_box[1] + 4), "⚙", fill=(255, 255, 255), font=self.font_regular)
        components.append(("settings-btn", "زر إعدادات المشغل" if is_ar else "Settings menu button", settings_btn_box))

        cc_btn_box = (px2 - 124, btn_y1, px2 - 92, btn_y1 + 32)
        draw.rectangle(cc_btn_box, outline=(200, 200, 200), width=1)
        draw.text((cc_btn_box[0] + 6, cc_btn_box[1] + 6), "CC", fill=(255, 255, 255), font=self.font_tiny)
        components.append(("subtitles-btn", "زر الترجمة وشرح الفيديو" if is_ar else "Subtitles closed captions button", cc_btn_box))

        # 3. Video Metadata
        meta_y1 = py2 + 14
        title_box = (px1, meta_y1, px2, meta_y1 + 28)
        title_text = "تطوير وكيل حاسوب ذكي ذاتي في ويندوز 11 مع CPower" if is_ar else "Microsoft Florence-2 Local Vision-Language Fine Tuning in Windows 11"
        draw.text((title_box[0], title_box[1]), reshape_arabic(title_text), fill=text_primary, font=self.font_title)
        components.append(("video-title", "عنوان مقطع الفيديو" if is_ar else "Video title headline", title_box))

        # Channel & Subscribe
        chan_y1 = meta_y1 + 34
        chan_avatar = (px1, chan_y1, px1 + 40, chan_y1 + 40)
        draw.ellipse(chan_avatar, fill=(225, 29, 72))
        components.append(("channel-avatar", "صورة القناة" if is_ar else "Channel profile avatar", chan_avatar))

        chan_text_box = (px1 + 48, chan_y1, px1 + 240, chan_y1 + 38)
        draw.text((px1 + 48, chan_y1 + 2), reshape_arabic("مختبر CPower للذكاء الاصطناعي" if is_ar else "CPower AI Lab"), fill=text_primary, font=self.font_regular)
        components.append(("channel-name", "اسم القناة" if is_ar else "Channel name banner", chan_text_box))

        sub_btn_box = (px1 + 250, chan_y1 + 2, px1 + 355, chan_y1 + 38)
        draw.rectangle(sub_btn_box, fill=(241, 241, 241) if dark_theme else (15, 15, 15))
        draw.text((sub_btn_box[0] + 16, sub_btn_box[1] + 8), reshape_arabic("اشتراك" if is_ar else "Subscribe"), fill=(15, 15, 15) if dark_theme else (255, 255, 255), font=self.font_regular)
        components.append(("subscribe-btn", "زر الاشتراك في القناة" if is_ar else "Subscribe button", sub_btn_box))

        # Like / Share
        like_btn_box = (px2 - 280, chan_y1 + 2, px2 - 180, chan_y1 + 38)
        draw.rectangle(like_btn_box, fill=(35, 35, 35) if dark_theme else (240, 240, 240))
        draw.text((like_btn_box[0] + 16, like_btn_box[1] + 8), "👍 15K | 👎", fill=text_primary, font=self.font_small)
        components.append(("like-btn", "زر الإعجاب بالفيديو" if is_ar else "Like video button", like_btn_box))

        share_btn_box = (px2 - 170, chan_y1 + 2, px2 - 90, chan_y1 + 38)
        draw.rectangle(share_btn_box, fill=(35, 35, 35) if dark_theme else (240, 240, 240))
        draw.text((share_btn_box[0] + 14, share_btn_box[1] + 8), reshape_arabic("مشاركة" if is_ar else "Share"), fill=text_primary, font=self.font_small)
        components.append(("share-btn", "زر مشاركة الفيديو" if is_ar else "Share video button", share_btn_box))

        # 4. Comments Section
        comm_y1 = chan_y1 + 52
        comm_header_box = (px1, comm_y1, px1 + 260, comm_y1 + 24)
        comm_header_text = "1,240 تعليق   ☰ ترتيب حسب" if is_ar else "1,240 Comments   ☰ Sort by"
        draw.text((px1, comm_y1), reshape_arabic(comm_header_text), fill=text_primary, font=self.font_regular)
        components.append(("comments-header", "شريط عدد وترتيب التعليقات" if is_ar else "Comments count and sort header", comm_header_box))

        add_comm_y = comm_y1 + 30
        add_comm_input = (px1 + 44, add_comm_y, px2, add_comm_y + 32)
        draw.line([(add_comm_input[0], add_comm_y + 30), (add_comm_input[2], add_comm_y + 30)], fill=border_col, width=1)
        draw.text((add_comm_input[0] + 4, add_comm_y + 6), reshape_arabic("إضافة تعليق علني..." if is_ar else "Add a comment..."), fill=text_secondary, font=self.font_small)
        components.append(("add-comment-input", "حقل كتابة تعليق علني" if is_ar else "Add public comment input", add_comm_input))

        # 5. Right Sidebar Recommendation Cards
        rx1, ry1 = px2 + 24, 76
        rw = width - rx1 - 24
        for idx in range(3):
            card_y1 = ry1 + idx * 105
            card_box = (rx1, card_y1, rx1 + rw, card_y1 + 95)
            tw, th = 140, 80
            thumb_box = (rx1, card_y1, rx1 + tw, card_y1 + th)
            draw.rectangle(thumb_box, fill=(50 + idx * 15, 60 + idx * 10, 80 + idx * 5))
            draw.text((rx1 + tw + 10, card_y1 + 4), f"Autonomous Agent #{idx+1}", fill=text_primary, font=self.font_small)
            components.append((f"rec-card-{idx+1}", f"بطاقة الفيديو المقترح {idx+1}" if is_ar else f"Recommended video card {idx+1}", card_box))

        return img, components


# ---------------------------------------------------------------------------
# HTML5/CSS3 Templates for Playwright Headless Chrome Engine
# ---------------------------------------------------------------------------

YOUTUBE_WATCH_HTML = """<!DOCTYPE html>
<html lang="{lang}" dir="{dir}">
<head>
<meta charset="utf-8">
<style>
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{ background: #0f0f0f; color: #f1f1f1; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; }}
  .yt-header {{ display: flex; align-items: center; justify-content: space-between; height: 56px; padding: 0 16px; background: #0f0f0f; border-bottom: 1px solid #272727; }}
  .header-start {{ display: flex; align-items: center; gap: 16px; }}
  .yt-logo {{ display: flex; align-items: center; gap: 6px; font-weight: 700; font-size: 19px; color: #fff; cursor: pointer; }}
  .yt-logo span.red {{ color: #ff0000; font-size: 22px; }}
  .header-center {{ display: flex; align-items: center; gap: 12px; max-width: 720px; flex: 1; margin: 0 40px; }}
  .search-container {{ display: flex; flex: 1; height: 40px; }}
  .search-box {{ display: flex; flex: 1; border: 1px solid #303030; background: #121212; padding: 0 16px; align-items: center; border-radius: 4px; }}
  .search-input {{ width: 100%; background: transparent; border: none; color: #fff; font-size: 16px; outline: none; }}
  .search-btn {{ width: 64px; height: 40px; background: #222222; border: 1px solid #303030; color: #fff; cursor: pointer; display: flex; align-items: center; justify-content: center; }}
  .voice-btn {{ width: 40px; height: 40px; border-radius: 50%; background: #222222; border: none; color: #fff; cursor: pointer; display: flex; align-items: center; justify-content: center; font-size: 16px; }}
  .header-end {{ display: flex; align-items: center; gap: 16px; }}
  .user-avatar {{ width: 34px; height: 34px; border-radius: 50%; background: #7c4dff; color: #fff; display: flex; align-items: center; justify-content: center; font-weight: bold; font-size: 14px; }}

  .main-layout {{ display: flex; max-width: 1320px; margin: 16px auto; padding: 0 16px; gap: 24px; }}
  .primary-content {{ flex: 1; min-width: 0; }}
  .secondary-content {{ width: 360px; flex-shrink: 0; }}

  .player-box {{ width: 100%; height: 460px; background: #000; border-radius: 8px; position: relative; overflow: hidden; }}
  .player-screen {{ width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; color: #6b7280; font-size: 22px; }}
  .player-controls {{ position: absolute; bottom: 0; left: 0; right: 0; height: 50px; background: linear-gradient(transparent, rgba(0, 0, 0, 0.9)); display: flex; flex-direction: column; justify-content: flex-end; padding: 0 12px 6px; }}
  .seek-bar-track {{ width: 100%; height: 4px; background: rgba(255, 255, 255, 0.3); position: relative; cursor: pointer; margin-bottom: 6px; }}
  .seek-bar-played {{ position: absolute; left: 0; top: 0; height: 100%; width: 35%; background: #ff0000; }}
  .seek-scrubber {{ position: absolute; left: 35%; top: -4px; width: 12px; height: 12px; border-radius: 50%; background: #ff0000; transform: translateX(-50%); }}
  .controls-row {{ display: flex; align-items: center; justify-content: space-between; }}
  .ctrl-group {{ display: flex; align-items: center; gap: 10px; }}
  .player-btn {{ background: transparent; border: none; color: #fff; width: 36px; height: 36px; font-size: 16px; cursor: pointer; display: flex; align-items: center; justify-content: center; }}
  .time-badge {{ font-size: 12px; color: #e5e5e5; font-variant-numeric: tabular-nums; }}

  .video-info {{ margin-top: 14px; }}
  .video-title {{ font-size: 18px; font-weight: 600; line-height: 24px; }}
  .channel-row {{ display: flex; align-items: center; justify-content: space-between; margin-top: 10px; }}
  .channel-meta {{ display: flex; align-items: center; gap: 12px; }}
  .channel-pfp {{ width: 40px; height: 40px; border-radius: 50%; background: #e11d48; }}
  .sub-btn {{ background: #fff; color: #0f0f0f; border: none; border-radius: 18px; font-weight: 600; font-size: 14px; height: 36px; padding: 0 16px; cursor: pointer; }}
  .action-buttons {{ display: flex; align-items: center; gap: 8px; }}
  .act-btn {{ background: #272727; color: #fff; border: none; border-radius: 18px; height: 36px; padding: 0 14px; font-size: 13px; cursor: pointer; }}

  .comments-container {{ margin-top: 20px; }}
  .comments-header {{ display: flex; align-items: center; gap: 24px; margin-bottom: 16px; font-size: 16px; font-weight: 700; }}
  .add-comment-box {{ display: flex; gap: 12px; }}
  .comment-avatar {{ width: 36px; height: 36px; border-radius: 50%; background: #2563eb; flex-shrink: 0; }}
  .comment-input {{ flex: 1; background: transparent; border: none; border-bottom: 1px solid #303030; color: #fff; font-size: 14px; padding-bottom: 6px; outline: none; }}

  .rec-card {{ display: flex; gap: 8px; margin-bottom: 12px; cursor: pointer; }}
  .rec-thumb {{ width: 140px; height: 80px; border-radius: 6px; background: #374151; flex-shrink: 0; }}
  .rec-meta {{ display: flex; flex-direction: column; gap: 4px; }}
  .rec-title {{ font-size: 13px; font-weight: 600; line-height: 18px; }}
</style>
</head>
<body>
  <header class="yt-header">
    <div class="header-start">
      <div id="yt-logo" class="yt-logo"><span class="red">▶</span> YouTube</div>
    </div>
    <div class="header-center">
      <div class="search-container">
        <div class="search-box">
          <input id="search-input" class="search-input" type="text" value="{search_val}">
        </div>
        <button id="search-btn" class="search-btn">🔍</button>
      </div>
      <button id="voice-search-btn" class="voice-btn">🎤</button>
    </div>
    <div class="header-end">
      <div id="user-avatar" class="user-avatar">C</div>
    </div>
  </header>

  <main class="main-layout">
    <div class="primary-content">
      <div id="video-player" class="player-box">
        <div class="player-screen">▶ Video Stream 1080p</div>
        <div class="player-controls">
          <div id="seek-bar" class="seek-bar-track">
            <div class="seek-bar-played"></div>
            <div id="seek-scrubber" class="seek-scrubber"></div>
          </div>
          <div class="controls-row">
            <div class="ctrl-group">
              <button id="play-pause-btn" class="player-btn">⏯</button>
              <button id="next-video-btn" class="player-btn">⏭</button>
              <button id="volume-btn" class="player-btn">🔊</button>
              <span id="time-display" class="time-badge">04:15 / 12:30</span>
            </div>
            <div class="ctrl-group">
              <button id="subtitles-btn" class="player-btn">CC</button>
              <button id="settings-btn" class="player-btn">⚙</button>
              <button id="fullscreen-btn" class="player-btn">⛶</button>
            </div>
          </div>
        </div>
      </div>

      <div class="video-info">
        <h1 id="video-title" class="video-title">{video_title}</h1>
        <div class="channel-row">
          <div class="channel-meta">
            <div id="channel-avatar" class="channel-pfp"></div>
            <span id="channel-name" style="font-weight:600;font-size:15px;">{channel_name}</span>
            <button id="subscribe-btn" class="sub-btn">{sub_text}</button>
          </div>
          <div class="action-buttons">
            <button id="like-btn" class="act-btn">👍 18K</button>
            <button id="share-btn" class="act-btn">↗ {share_text}</button>
          </div>
        </div>
      </div>

      <section class="comments-container">
        <div id="comments-header" class="comments-header">1,842 {comments_text}</div>
        <div class="add-comment-box">
          <div class="comment-avatar"></div>
          <input id="add-comment-input" class="comment-input" type="text" placeholder="{comment_placeholder}">
        </div>
      </section>
    </div>

    <aside class="secondary-content">
      <div id="rec-video-1" class="rec-card">
        <div class="rec-thumb"></div>
        <div class="rec-meta">
          <div class="rec-title">{rec1_title}</div>
        </div>
      </div>
      <div id="rec-video-2" class="rec-card">
        <div class="rec-thumb" style="background:#1e3a8a;"></div>
        <div class="rec-meta">
          <div class="rec-title">{rec2_title}</div>
        </div>
      </div>
      <div id="rec-video-3" class="rec-card">
        <div class="rec-thumb" style="background:#701a75;"></div>
        <div class="rec-meta">
          <div class="rec-title">{rec3_title}</div>
        </div>
      </div>
    </aside>
  </main>
</body>
</html>
"""

ARABIC_WEB_FORM_HTML = """<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="utf-8">
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #f0f2f5; font-family: "Segoe UI", Tahoma, sans-serif; display: flex; align-items: center; justify-content: center; height: 100vh; }
  .form-card { width: 520px; background: #fff; padding: 36px; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
  .form-title { font-size: 22px; font-weight: 700; color: #1a1a1a; margin-bottom: 24px; text-align: center; }
  .form-group { margin-bottom: 18px; }
  .form-label { display: block; font-size: 14px; font-weight: 600; color: #333; margin-bottom: 6px; }
  .form-input { width: 100%; height: 42px; border: 1px solid #ccc; border-radius: 4px; padding: 0 12px; font-size: 14px; outline: none; }
  .form-input:focus { border-color: #0067c0; }
  .checkbox-row { display: flex; align-items: center; gap: 8px; margin: 16px 0; }
  .submit-btn { width: 100%; height: 44px; background: #0067c0; color: #fff; border: none; border-radius: 4px; font-size: 16px; font-weight: 600; cursor: pointer; }
  .forgot-link { display: block; text-align: center; margin-top: 14px; color: #0067c0; text-decoration: none; font-size: 14px; }
</style>
</head>
<body>
  <div class="form-card">
    <h2 class="form-title">تسجيل الدخول إلى حساب CPower السحابي</h2>
    <div class="form-group">
      <label class="form-label" for="user-input">اسم المستخدم أو البريد الإلكتروني</label>
      <input id="user-input" class="form-input" type="text" value="administrator@cpower.local">
    </div>
    <div class="form-group">
      <label class="form-label" for="password-input">كلمة المرور</label>
      <input id="password-input" class="form-input" type="password" value="••••••••••••">
    </div>
    <div class="form-group">
      <label class="form-label" for="confirm-password-input">تأكيد كلمة المرور</label>
      <input id="confirm-password-input" class="form-input" type="password" value="••••••••••••">
    </div>
    <div class="checkbox-row">
      <input id="remember-checkbox" type="checkbox" checked style="width:18px;height:18px;">
      <label for="remember-checkbox" style="font-size:14px;color:#444;">تذكرني في هذه الجلسة</label>
    </div>
    <button id="submit-btn" class="submit-btn">تسجيل الدخول</button>
    <a id="forgot-pwd-link" class="forgot-link" href="#">هل نسيت كلمة المرور؟</a>
  </div>
</body>
</html>
"""

ARABIC_PORTAL_DIALOG_HTML = """<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="utf-8">
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #202020; font-family: "Segoe UI", Tahoma, sans-serif; display: flex; align-items: center; justify-content: center; height: 100vh; }
  .dialog-box { width: 680px; background: #2b2b2b; color: #fff; border-radius: 8px; border: 1px solid #444; padding: 24px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); }
  .dialog-header { font-size: 18px; font-weight: 700; border-bottom: 1px solid #444; padding-bottom: 12px; margin-bottom: 20px; }
  .input-row { display: flex; align-items: center; gap: 12px; margin-bottom: 16px; }
  .input-label { width: 140px; font-size: 14px; color: #ccc; }
  .input-field { flex: 1; height: 38px; background: #1a1a1a; border: 1px solid #555; color: #fff; border-radius: 4px; padding: 0 10px; font-size: 14px; }
  .browse-btn { height: 38px; padding: 0 16px; background: #3a3a3a; color: #fff; border: 1px solid #555; border-radius: 4px; cursor: pointer; }
  .btn-bar { display: flex; justify-content: flex-end; gap: 12px; margin-top: 24px; border-top: 1px solid #444; padding-top: 16px; }
  .action-btn { height: 38px; padding: 0 20px; border-radius: 4px; font-size: 14px; font-weight: 600; cursor: pointer; }
  .btn-primary { background: #0067c0; color: #fff; border: none; }
  .btn-secondary { background: #3a3a3a; color: #fff; border: 1px solid #555; }
</style>
</head>
<body>
  <div class="dialog-box">
    <div class="dialog-header">تصدير وحفظ نتائج تدريب نموذج Florence-2</div>
    <div class="input-row">
      <span class="input-label">اسم الملف:</span>
      <input id="file-name-input" class="input-field" type="text" value="cpower_florence2_checkpoint_m1.bin">
      <button id="browse-btn" class="browse-btn">استعراض...</button>
    </div>
    <div class="input-row">
      <span class="input-label">دليل الحفظ:</span>
      <input id="save-path-input" class="input-field" type="text" value="E:\\cpower_training_data\\checkpoints">
    </div>
    <div class="btn-bar">
      <button id="save-btn" class="action-btn btn-primary">حفظ</button>
      <button id="apply-btn" class="action-btn btn-secondary">تطبيق</button>
      <button id="cancel-btn" class="action-btn btn-secondary">إلغاء الأمر</button>
    </div>
  </div>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# Domain 2: Web & YouTube Media Harvester
# ---------------------------------------------------------------------------

class WebMediaHarvester:
    """
    Automated non-mouse Web & YouTube UI harvester with dual Playwright / Pillow engines.
    Persistently reuses background Chrome page to achieve 30-50ms latency per frame.
    """

    def __init__(self, collector: CPowerDatasetCollector, headless: bool = True):
        self.collector = collector
        self.headless = headless
        self._playwright: Optional[Playwright] = None
        self._browser: Optional[Browser] = None
        self._page: Optional[Page] = None
        self._pillow_engine = PillowYouTubeSynthesizer()
        self._init_playwright()

    def _init_playwright(self) -> bool:
        if not HAS_PLAYWRIGHT:
            return False
        try:
            self._playwright = sync_playwright().start()
            self._browser = self._playwright.chromium.launch(channel="chrome", headless=self.headless)
            self._page = self._browser.new_page(viewport={"width": 1280, "height": 800})
            return True
        except Exception:
            self.close()
            return False

    def close(self):
        if self._browser:
            try:
                self._browser.close()
            except Exception:
                pass
            self._browser = None
        if self._playwright:
            try:
                self._playwright.stop()
            except Exception:
                pass
            self._playwright = None
        self._page = None

    def generate_youtube_task(
        self,
        traj_id: str,
        step_idx: int = 1,
        locale: str = "en",
        use_playwright: bool = True,
    ) -> None:
        """Generates a complete YouTube media sample and records it into the dataset."""
        if use_playwright and self._page:
            try:
                self._generate_playwright_sample(traj_id, step_idx, locale=locale)
                return
            except Exception:
                pass

        # High-speed Pillow fallback
        self._generate_pillow_sample(traj_id, step_idx, locale=locale)

    def _generate_playwright_sample(self, traj_id: str, step_idx: int, locale: str = "en") -> None:
        is_ar = (locale == "ar")
        lang = "ar" if is_ar else "en"
        direction = "rtl" if is_ar else "ltr"

        params = {
            "lang": lang,
            "dir": direction,
            "search_val": "تطوير وكيل CPower محلي في ويندوز" if is_ar else "Florence-2 autonomous agent UI navigation",
            "video_title": "تطوير وكيل حاسوب ذاتي بلغة C ونماذج محلية في ويندوز 11" if is_ar else "Building Autonomous Local AI Agents on Windows 11 with CPower",
            "channel_name": "مختبر CPower للذكاء الاصطناعي" if is_ar else "CPower Architecture Lab",
            "sub_text": "اشتراك" if is_ar else "Subscribe",
            "share_text": "مشاركة" if is_ar else "Share",
            "comments_text": "تعليق" if is_ar else "Comments",
            "comment_placeholder": "إضافة تعليق علني..." if is_ar else "Add a comment...",
            "rec1_title": "ضبط نموذج Florence-2 محلياً: LoRA مقابل الأوزان الكاملة" if is_ar else "Microsoft Florence-2 Fine Tuning: LoRA vs Full Weights",
            "rec2_title": "أتمتة واجهة ويندوز 11 باستخدام C11 و UIA" if is_ar else "Windows 11 UI Automation with C11 & UIA Deep Dive",
            "rec3_title": "محرك توليد واجهات العربية RTL المزدوج" if is_ar else "Dual-Engine Arabic RTL UI Generation in Python",
        }

        html = YOUTUBE_WATCH_HTML.format(**params)
        self._page.set_content(html)
        self._page.wait_for_load_state("domcontentloaded")

        # Element targets map
        targets = [
            ("yt-logo", "شعار يوتيوب" if is_ar else "YouTube logo", "click", ""),
            ("search-input", "حقل البحث في يوتيوب" if is_ar else "Search input box", "type", "/"),
            ("search-btn", "زر البحث" if is_ar else "Search button", "click", "enter"),
            ("voice-search-btn", "زر البحث الصوتي" if is_ar else "Voice search button", "click", ""),
            ("video-player", "مشغل الفيديو" if is_ar else "Video player screen", "click", "k"),
            ("seek-bar", "شريط تمرير الوقت" if is_ar else "Playback seek bar", "click", ""),
            ("seek-scrubber", "مؤشر وقت الفيديو" if is_ar else "Timeline seek scrubber", "click", ""),
            ("play-pause-btn", "زر تشغيل أو إيقاف الفيديو" if is_ar else "Play/Pause toggle button", "click", "k"),
            ("next-video-btn", "زر الفيديو التالي" if is_ar else "Next video button", "click", "shift+n"),
            ("volume-btn", "زر كتم وتعديل الصوت" if is_ar else "Volume mute button", "click", "m"),
            ("time-display", "مؤشر مدة الفيديو" if is_ar else "Playback time display", "click", ""),
            ("subtitles-btn", "زر الترجمة وشرح الفيديو" if is_ar else "Subtitles closed captions button", "click", "c"),
            ("settings-btn", "زر إعدادات المشغل" if is_ar else "Settings menu button", "click", ""),
            ("fullscreen-btn", "زر ملء الشاشة" if is_ar else "Fullscreen toggle button", "click", "f"),
            ("video-title", "عنوان مقطع الفيديو" if is_ar else "Video title headline", "click", ""),
            ("channel-avatar", "صورة القناة" if is_ar else "Channel profile avatar", "click", ""),
            ("channel-name", "اسم القناة" if is_ar else "Channel name", "click", ""),
            ("subscribe-btn", "زر الاشتراك في القناة" if is_ar else "Subscribe button", "click", ""),
            ("like-btn", "زر الإعجاب بالفيديو" if is_ar else "Like video button", "click", ""),
            ("share-btn", "زر مشاركة الفيديو" if is_ar else "Share video button", "click", ""),
            ("comments-header", "شريط عدد التعليقات" if is_ar else "Comments count and header", "click", ""),
            ("add-comment-input", "حقل كتابة تعليق علني" if is_ar else "Add public comment input", "type", ""),
            ("rec-video-1", "بطاقة الفيديو المقترح الأول" if is_ar else "Recommended video card 1", "click", ""),
            ("rec-video-2", "بطاقة الفيديو المقترح الثاني" if is_ar else "Recommended video card 2", "click", ""),
            ("rec-video-3", "بطاقة الفيديو المقترح الثالث" if is_ar else "Recommended video card 3", "click", ""),
        ]

        extracted_targets = []
        for elem_id, phrase, act_type, hotkey in targets:
            try:
                loc = self._page.locator(f"#{elem_id}")
                if loc.count() > 0:
                    box = loc.bounding_box()
                    if box and box["width"] > 0 and box["height"] > 0:
                        x1 = int(round(box["x"]))
                        y1 = int(round(box["y"]))
                        x2 = int(round(box["x"] + box["width"]))
                        y2 = int(round(box["y"] + box["height"]))
                        extracted_targets.append((elem_id, phrase, act_type, hotkey, (x1, y1, x2, y2)))
            except Exception:
                continue

        if not extracted_targets:
            raise RuntimeError("No targets extracted from Playwright DOM.")

        screenshot_bytes = self._page.screenshot(type="jpeg", quality=90)
        img = Image.open(io.BytesIO(screenshot_bytes)).convert("RGB")
        img_w, img_h = img.size

        elem_id, phrase, act_type, hotkey, raw_box = random.choice(extracted_targets)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type=act_type,
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            keys=hotkey,
            window_title="YouTube - Google Chrome",
            metadata={"domain": "web_media", "engine": "playwright", "component": elem_id, "locale": locale},
        )

    def _generate_pillow_sample(self, traj_id: str, step_idx: int, locale: str = "en") -> None:
        dark_theme = random.choice([True, False])
        img, comps = self._pillow_engine.generate(dark_theme=dark_theme, locale=locale)
        img_w, img_h = img.size

        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="type" if "input" in cid else "click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title="YouTube - Google Chrome",
            metadata={"domain": "web_media", "engine": "pillow", "component": cid, "locale": locale},
        )


# ---------------------------------------------------------------------------
# Domain 3: Rich Arabic RTL Layout Synthesizer
# ---------------------------------------------------------------------------

class ArabicRTLHarvester:
    """
    Dual-Engine Harvester for Rich Arabic RTL UI Grounding & Action Data:
      - Engine A: Headless Chromium RTL DOM with native HarfBuzz and CSS flexbox mirroring.
      - Engine B: High-speed In-Memory Pillow with arabic_reshaper and python-bidi.
    """

    def __init__(self, collector: CPowerDatasetCollector, headless: bool = True):
        self.collector = collector
        self.headless = headless
        self._browser: Optional[Browser] = None
        self._playwright: Optional[Playwright] = None
        self._page: Optional[Page] = None
        self._win11_synth = Win11ControlsSynthesizer()

    def _get_page(self) -> Optional[Page]:
        if not HAS_PLAYWRIGHT:
            return None
        if self._page is None:
            try:
                self._playwright = sync_playwright().start()
                self._browser = self._playwright.chromium.launch(channel="chrome", headless=self.headless)
                self._page = self._browser.new_page(viewport={"width": 1024, "height": 680})
            except Exception:
                self.close()
                return None
        return self._page

    def close(self):
        if self._browser:
            try:
                self._browser.close()
            except Exception:
                pass
            self._browser = None
        if self._playwright:
            try:
                self._playwright.stop()
            except Exception:
                pass
            self._playwright = None
        self._page = None

    def generate_arabic_task(
        self,
        traj_id: str,
        step_idx: int = 1,
        engine: str = "auto",
        template: str = "auto",
    ) -> None:
        """Generates an authentic Arabic RTL training sample."""
        if engine == "auto":
            engine = "engine_a" if (HAS_PLAYWRIGHT and random.random() > 0.4) else "engine_b"

        if engine == "engine_a":
            page = self._get_page()
            if page:
                try:
                    self._generate_engine_a_sample(page, traj_id, step_idx, template=template)
                    return
                except Exception:
                    pass

        # Fallback to Engine B (Pillow)
        self._generate_engine_b_sample(traj_id, step_idx, template=template)

    def _generate_engine_a_sample(self, page: Page, traj_id: str, step_idx: int, template: str = "auto") -> None:
        if template == "auto":
            template = random.choice(["form", "portal_dialog"])

        if template == "form":
            html = ARABIC_WEB_FORM_HTML
            targets = [
                ("user-input", "حقل اسم المستخدم", "type"),
                ("password-input", "حقل كلمة المرور", "type"),
                ("confirm-password-input", "حقل تأكيد كلمة المرور", "type"),
                ("remember-checkbox", "مربع اختيار تذكرني", "click"),
                ("submit-btn", "زر تسجيل الدخول", "click"),
                ("forgot-pwd-link", "رابط هل نسيت كلمة المرور؟", "click"),
            ]
            win_title = "تسجيل الدخول — CPower Cloud"
        else:
            html = ARABIC_PORTAL_DIALOG_HTML
            targets = [
                ("file-name-input", "حقل إدخال اسم الملف", "type"),
                ("save-path-input", "حقل دليل حفظ النموذج", "type"),
                ("browse-btn", "زر استعراض...", "click"),
                ("save-btn", "زر حفظ", "click"),
                ("apply-btn", "زر تطبيق", "click"),
                ("cancel-btn", "زر إلغاء الأمر", "click"),
            ]
            win_title = "حفظ وتصدير النموذج — Save Model"

        page.set_content(html)
        page.wait_for_load_state("domcontentloaded")

        extracted_targets = []
        for elem_id, phrase, act_type in targets:
            try:
                loc = page.locator(f"#{elem_id}")
                if loc.count() > 0:
                    box = loc.bounding_box()
                    if box and box["width"] > 0 and box["height"] > 0:
                        x1 = int(round(box["x"]))
                        y1 = int(round(box["y"]))
                        x2 = int(round(box["x"] + box["width"]))
                        y2 = int(round(box["y"] + box["height"]))
                        extracted_targets.append((elem_id, phrase, act_type, (x1, y1, x2, y2)))
            except Exception:
                continue

        if not extracted_targets:
            raise RuntimeError("No targets extracted in Engine A Arabic sample.")

        screenshot_bytes = page.screenshot(type="jpeg", quality=90)
        img = Image.open(io.BytesIO(screenshot_bytes)).convert("RGB")
        img_w, img_h = img.size

        elem_id, phrase, act_type, raw_box = random.choice(extracted_targets)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type=act_type,
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title=win_title,
            metadata={"domain": "arabic_rtl", "engine": "engine_a_chromium", "component": elem_id},
        )

    def _generate_engine_b_sample(self, traj_id: str, step_idx: int, template: str = "auto") -> None:
        """Engine B: Mirrored Windows 11 Explorer with right-aligned navigation pane or Arabic Dialog."""
        if template in ("dialog", "save_as"):
            img, comps = self._win11_synth.generate_save_open_dialog(is_open_dialog=False)
            win_title = "حفظ باسم — Save As"
        elif template == "confirm":
            img, comps = self._win11_synth.generate_confirm_dialog(dialog_type="delete")
            win_title = "تأكيد حذف المجلد"
        else:
            # Mirrored Arabic Windows 11 Explorer
            w, h = 1024, 680
            img = Image.new("RGB", (w, h), (248, 249, 250))
            draw = ImageDraw.Draw(img)
            comps = []

            # Explorer Header
            draw.rectangle([0, 0, w, 50], fill=(238, 240, 244))
            draw.line([(0, 50), (w, 50)], fill=(210, 215, 222), width=1)

            # Mirrored Navigation Pane on the RIGHT (w-220 to w)
            draw.rectangle([w - 220, 51, w, h], fill=(242, 244, 248))
            draw.line([(w - 220, 51), (w - 220, h)], fill=(215, 220, 228), width=1)

            sidebar_items = [
                ("الوصول السريع", (w - 200, 70)),
                ("سطح المكتب", (w - 200, 105)),
                ("التنزيلات", (w - 200, 140)),
                ("المستندات", (w - 200, 175)),
                ("القرص المحلي (C:)", (w - 200, 220)),
                ("القرص المحلي (E:)", (w - 200, 255)),
            ]
            for s_name, (sx, sy) in sidebar_items:
                draw.text((sx, sy), reshape_arabic(s_name), fill=(40, 40, 40), font=self._win11_synth.font_small)
                s_box = (sx - 10, sy - 4, w - 10, sy + 24)
                comps.append((f"side-{s_name}", f"عنصر {s_name}", s_box))

            # Breadcrumb & Search on Top Bar
            addr_box = (280, 10, w - 240, 40)
            draw.rectangle(addr_box, fill=(255, 255, 255), outline=(190, 195, 205), width=1)
            draw.text((addr_box[0] + 15, addr_box[1] + 6), reshape_arabic("هذا الكمبيوتر الشخصي > القرص المحلي (E:)"), fill=(30, 30, 30), font=self._win11_synth.font_small)
            comps.append(("addr-bar", "شريط مسار المجلدات", addr_box))

            search_box = (20, 10, 260, 40)
            draw.rectangle(search_box, fill=(255, 255, 255), outline=(190, 195, 205), width=1)
            draw.text((search_box[0] + 15, search_box[1] + 6), reshape_arabic("بحث في القرص E..."), fill=(120, 120, 120), font=self._win11_synth.font_small)
            comps.append(("search-box", "مربع البحث", search_box))

            # Folder items on LEFT content area
            folders = [
                ("المشاريع البرمجية", (40, 80, 180, 160)),
                ("بيانات التدريب CPower", (210, 80, 350, 160)),
                ("النماذج المدربة", (380, 80, 520, 160)),
                ("النسخ الاحتياطية", (550, 80, 690, 160)),
            ]
            for f_name, box in folders:
                draw.rectangle(box, fill=(255, 255, 255), outline=(220, 225, 235), width=1)
                draw.rectangle([box[0] + 15, box[1] + 15, box[0] + 45, box[1] + 40], fill=(245, 180, 50))
                draw.text((box[0] + 10, box[3] - 25), reshape_arabic(f_name[:12]), fill=(20, 20, 20), font=self._win11_synth.font_small)
                comps.append((f"folder-{f_name}", f"مجلد {f_name}", box))

            win_title = "مستكشف الملفات — القرص E:"

        img_w, img_h = img.size
        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="double_click" if "مجلد" in phrase or "ملف" in phrase else "click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title=win_title,
            metadata={"domain": "arabic_rtl", "engine": "engine_b_pillow", "component": cid},
        )


# ---------------------------------------------------------------------------
# Unified Multi-Task Harvester Controller
# ---------------------------------------------------------------------------

class MultiTaskUIHarvester:
    """
    Unified multimodal automated non-mouse harvesting suite across:
      1. Windows 11 Explorer & System Controls (5 Archetypes + Live Background Perception).
      2. Web & YouTube Media Controls (Playwright Chrome + In-Memory Pillow Fallback).
      3. Rich Arabic RTL Layouts (Chromium RTL DOM + Mirrored Pillow Synthesizer).
    """

    def __init__(self, collector: CPowerDatasetCollector, headless: bool = True):
        self.collector = collector
        self.headless = headless
        self.win11_synth = Win11ControlsSynthesizer()
        self.web_harvester = WebMediaHarvester(collector=collector, headless=headless)
        self.arabic_harvester = ArabicRTLHarvester(collector=collector, headless=headless)
        self.font_large = get_ui_font(22)
        self.font_medium = get_ui_font(16)
        self.font_small = get_ui_font(13)

    def close(self):
        """Releases browser and client resources cleanly."""
        self.web_harvester.close()
        self.arabic_harvester.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    # --- Windows 11 Explorer & System Controls ---

    def generate_explorer_task(self, traj_id: str, step_idx: int = 1, archetype: Optional[str] = None) -> None:
        """Generates an Explorer task across one of the 5 archetypes."""
        if archetype is None:
            archetype = random.choice(["ribbon", "breadcrumb", "grid", "dialog", "confirm"])

        if archetype == "ribbon":
            self.generate_explorer_ribbon_task(traj_id, step_idx)
        elif archetype == "breadcrumb":
            self.generate_explorer_breadcrumb_task(traj_id, step_idx)
        elif archetype == "grid":
            self.generate_explorer_grid_task(traj_id, step_idx)
        elif archetype == "dialog":
            self.generate_save_open_dialog_task(traj_id, step_idx)
        else:
            self.generate_confirm_dialog_task(traj_id, step_idx)

    def generate_explorer_ribbon_task(self, traj_id: str, step_idx: int = 1) -> None:
        """Archetype 1: Explorer Ribbon & Toolbar."""
        dark_mode = random.choice([True, False])
        img, comps = self.win11_synth.generate_ribbon(dark_mode=dark_mode)
        img_w, img_h = img.size

        # Pick one ribbon button or navigation element
        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title="File Explorer - Ribbon",
            metadata={"domain": "win11_explorer", "archetype": "ribbon", "component": cid},
        )

    def generate_explorer_breadcrumb_task(self, traj_id: str, step_idx: int = 1) -> None:
        """Archetype 2: Explorer Navigation & Breadcrumbs."""
        drive = random.choice(["C:", "E:", "D:"])
        img, comps = self.win11_synth.generate_breadcrumb(drive=drive)
        img_w, img_h = img.size

        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="type" if "search" in cid else "click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title=f"File Explorer - {drive}",
            metadata={"domain": "win11_explorer", "archetype": "breadcrumb", "component": cid},
        )

    def generate_explorer_grid_task(self, traj_id: str, step_idx: int = 1) -> None:
        """Archetype 3: Explorer Folder Grid & File List."""
        folder_count = random.choice([6, 8, 10, 12])
        img, comps = self.win11_synth.generate_grid(folder_count=folder_count)
        img_w, img_h = img.size

        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        action_type = "double_click" if "مجلد" in phrase or "ملف" in phrase else "click"
        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type=action_type,
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title="File Explorer - Items Grid",
            metadata={"domain": "win11_explorer", "archetype": "grid", "component": cid},
        )

    def generate_save_open_dialog_task(self, traj_id: str, step_idx: int = 1) -> None:
        """Archetype 4: Common File Dialogs (Save As / Open)."""
        is_open = random.choice([True, False])
        img, comps = self.win11_synth.generate_save_open_dialog(is_open_dialog=is_open)
        img_w, img_h = img.size

        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="type" if "input" in cid else "click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title="Open File Dialog" if is_open else "Save As Dialog",
            metadata={"domain": "win11_controls", "archetype": "dialog", "component": cid},
        )

    def generate_confirm_dialog_task(self, traj_id: str, step_idx: int = 1) -> None:
        """Archetype 5: System Confirmation & Alert Dialogs."""
        d_type = random.choice(["delete", "replace", "uac"])
        img, comps = self.win11_synth.generate_confirm_dialog(dialog_type=d_type)
        img_w, img_h = img.size

        cid, phrase, raw_box = random.choice(comps)
        clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
        click_x = (clean_box[0] + clean_box[2]) // 2
        click_y = (clean_box[1] + clean_box[3]) // 2

        self.collector.record_step(
            trajectory_id=traj_id,
            image=img,
            action_type="click",
            phrase=phrase,
            bbox=clean_box,
            click_coords=(click_x, click_y),
            normalized_bbox=norm_box,
            window_title="Confirm Dialog",
            metadata={"domain": "win11_controls", "archetype": "confirm", "type": d_type, "component": cid},
        )

    def harvest_live_explorer_step(self, traj_id: str, client: Optional[Any] = None) -> bool:
        """
        Extracts live background Explorer / Dialog nodes and micro-regions via CPowerClient
        with zero mouse cursor movement or focus stealing.
        """
        if not HAS_CPOWER_CLIENT:
            return False

        owns_client = False
        if client is None:
            try:
                client = CPowerClient(transport="shm")
                owns_client = True
            except Exception:
                return False

        try:
            windows = client.list_windows()
            explorer_wnds = [
                w for w in windows
                if w.get("className") in ("CabinetWClass", "#32770")
                or "Explorer" in w.get("title", "")
                or "مستكشف" in w.get("title", "")
            ]
            if not explorer_wnds:
                return False

            wnd = explorer_wnds[0]
            hwnd = wnd["hwnd"]
            tree = client.capture_tree(hwnd=hwnd)
            nodes = tree.get("nodes", [])
            if len(nodes) < 5:
                return False

            root = nodes[0]
            rx1, ry1, rx2, ry2 = root.get("bounds", [0, 0, 0, 0])
            rw, rh = rx2 - rx1, ry2 - ry1
            if rw < 150 or rh < 100:
                return False

            # Capture micro-region via DXGI/GDI (zero mouse disruption)
            ack, png_bytes = client.capture_micro_region(rx1, ry1, rx2, ry2, fmt="png")
            if not ack.get("succeeded") or not png_bytes:
                return False

            img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
            img_w, img_h = img.size

            candidates = []
            for n in nodes[1:]:
                if n.get("offscreen", False):
                    continue
                name = (n.get("name") or "").strip()
                if not name or len(name) < 2:
                    continue
                b = n.get("bounds", [0, 0, 0, 0])
                lx1 = max(0, min(img_w, b[0] - rx1))
                ly1 = max(0, min(img_h, b[1] - ry1))
                lx2 = max(0, min(img_w, b[2] - rx1))
                ly2 = max(0, min(img_h, b[3] - ry1))
                if (lx2 - lx1) >= 8 and (ly2 - ly1) >= 8:
                    candidates.append((name, (lx1, ly1, lx2, ly2)))

            if not candidates:
                return False

            target_name, raw_box = random.choice(candidates)
            clean_box, norm_box = sanitize_bbox(raw_box, img_w, img_h)
            click_x = (clean_box[0] + clean_box[2]) // 2
            click_y = (clean_box[1] + clean_box[3]) // 2

            self.collector.record_step(
                trajectory_id=traj_id,
                image=img,
                action_type="click",
                phrase=target_name,
                bbox=clean_box,
                click_coords=(click_x, click_y),
                normalized_bbox=norm_box,
                window_title=wnd.get("title", "File Explorer"),
                metadata={"domain": "live_explorer", "hwnd": str(hwnd)},
            )
            return True
        except Exception:
            return False
        finally:
            if owns_client:
                try:
                    client.close()
                except Exception:
                    pass

    # --- Domain 2: Web & YouTube Media ---

    def generate_youtube_task(
        self,
        traj_id: str,
        step_idx: int = 1,
        locale: str = "en",
        use_playwright: bool = True,
    ) -> None:
        """Generates a complete YouTube media sample (English LTR or Arabic RTL)."""
        self.web_harvester.generate_youtube_task(
            traj_id=traj_id,
            step_idx=step_idx,
            locale=locale,
            use_playwright=use_playwright,
        )

    # --- Domain 3: Rich Arabic RTL ---

    def generate_arabic_rtl_task(
        self,
        traj_id: str,
        step_idx: int = 1,
        engine: str = "auto",
        template: str = "auto",
    ) -> None:
        """Generates an Arabic RTL task using Dual-Engine architecture."""
        self.arabic_harvester.generate_arabic_task(
            traj_id=traj_id,
            step_idx=step_idx,
            engine=engine,
            template=template,
        )

    # Backwards compatibility alias
    def generate_system_dialog_task(self, traj_id: str, step_idx: int = 1) -> None:
        self.generate_save_open_dialog_task(traj_id, step_idx)


# ---------------------------------------------------------------------------
# High-Level Batch Harvesting Driver
# ---------------------------------------------------------------------------

def harvest_all_tasks(total_samples: int = 60, distribution: Optional[Dict[str, float]] = None) -> int:
    """
    Generates a balanced multi-domain dataset across:
      - Windows 11 Explorer & Controls (Ribbon, Breadcrumbs, Grid, Dialog, Confirm, Live Perception)
      - Web & YouTube Media (English LTR & Arabic RTL)
      - Rich Arabic RTL (Dual-Engine: Headless Chromium + Mirrored Pillow)
    """
    collector = CPowerDatasetCollector()
    harvester = MultiTaskUIHarvester(collector)

    print(f"[HARVESTER] Generating {total_samples} multi-domain training samples into {collector.base_dir}...")
    start_time = time.time()

    categories = [
        ("win11_explorer_ribbon", lambda tid, s: harvester.generate_explorer_ribbon_task(tid, s)),
        ("win11_explorer_breadcrumb", lambda tid, s: harvester.generate_explorer_breadcrumb_task(tid, s)),
        ("win11_explorer_grid", lambda tid, s: harvester.generate_explorer_grid_task(tid, s)),
        ("win11_save_dialog", lambda tid, s: harvester.generate_save_open_dialog_task(tid, s)),
        ("win11_confirm_dialog", lambda tid, s: harvester.generate_confirm_dialog_task(tid, s)),
        ("web_youtube_en", lambda tid, s: harvester.generate_youtube_task(tid, s, locale="en")),
        ("web_youtube_ar", lambda tid, s: harvester.generate_youtube_task(tid, s, locale="ar")),
        ("arabic_rtl_form", lambda tid, s: harvester.generate_arabic_rtl_task(tid, s, template="form")),
        ("arabic_rtl_portal", lambda tid, s: harvester.generate_arabic_rtl_task(tid, s, template="portal_dialog")),
        ("arabic_rtl_explorer", lambda tid, s: harvester.generate_arabic_rtl_task(tid, s, engine="engine_b")),
    ]

    live_success = 0
    for i in range(1, total_samples + 1):
        # Periodically attempt live desktop capture
        if i % 5 == 0 and HAS_CPOWER_CLIENT:
            traj_id = collector.start_trajectory(goal=f"Live Windows 11 Perception Step #{i}")
            if harvester.harvest_live_explorer_step(traj_id):
                collector.complete_trajectory(traj_id, succeeded=True)
                live_success += 1
                if i % 10 == 0 or i == total_samples:
                    print(f"  Progress: [{i}/{total_samples}] samples harvested (Live: {live_success}).")
                continue
            else:
                collector.complete_trajectory(traj_id, succeeded=False, notes="Live capture inactive, fell back")

        cat_name, task_fn = random.choice(categories)
        traj_id = collector.start_trajectory(goal=f"Autonomous UI Grounding: {cat_name} task #{i}")
        task_fn(traj_id, 1)
        collector.complete_trajectory(traj_id, succeeded=True)

        if i % 10 == 0 or i == total_samples:
            print(f"  Progress: [{i}/{total_samples}] samples harvested (Live: {live_success}).")

    harvester.close()
    elapsed = time.time() - start_time
    summary = collector.get_dataset_summary()
    print(f"[HARVESTER] Completed {total_samples} samples in {elapsed:.2f}s (Live captures: {live_success})!")
    print(f"[HARVESTER] Current Dataset Stats: {summary}")
    return total_samples


if __name__ == "__main__":
    harvest_all_tasks(total_samples=60)
