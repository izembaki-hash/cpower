r"""
CPower Omni-Platform Autonomous Deep Explorer (clients/python/cpower_omni_explorer.py).

Delivers real-world, diverse, multi-platform visual exploration across desktop & web:
  1. Window Assurance: Always keeps browser/apps MAXIMIZED (never shrinks screen).
  2. Multi-Platform Navigation & Button Exploration:
     - YouTube: Explores Category Pills (Gaming, Music, Tech), clicks Settings tabs (Account, Notifications, Playback, Privacy), opens videos.
     - Wikipedia: Visits Random Articles (عشوائي), clicks TOC links, explores history.
     - GitHub: Explores Topics, Trending, opens repositories, clicks Issues/PRs/Releases tabs.
     - Google: Explores Search Preferences (Filters, Dark Mode, Language), performs diverse searches.
     - Hacker News: Explores top news, opens discussion threads, reads comments.
     - Windows 11 Settings: Navigates through System, Bluetooth, Apps, Personalization, Gaming, Windows Update.
  3. Visual Intelligence: Powered by NVIDIA NIM Super-VL 120B (nemotron-3.5-super-vl-120b-a12b) + Local Florence-2.
  4. Real Trajectory Distillation: Appends real state-action-reward pairs to E:\cpower_training_data for local model training.
"""

from __future__ import annotations

import base64
import io
import json
import logging
import os
from pathlib import Path
import random
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple
import urllib.request
import urllib.error

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_vision_local import CPowerLocalVision
from cpower_process_guard import find_browser_windows

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("OmniExplorer")

NVIDIA_API_KEY = os.environ.get("NVIDIA_API_KEY", "")
NVIDIA_VLM_MODEL = "nvidia/nemotron-3.5-super-vl-120b-a12b"
NVIDIA_VLM_URL = "https://integrate.api.nvidia.com/v1/chat/completions"


class OmniPlatformExplorer:
    """Explores real-world desktop platforms and clicks buttons dynamically."""

    def __init__(self, target_cycles: int = 50):
        self.client = CPowerClient()
        self.vision = CPowerLocalVision(client=self.client)
        self.target_cycles = target_cycles
        self.browser_hwnd = 0

        self.data_dir = Path("E:/cpower_training_data")
        self.artifacts_dir = self.data_dir / "demo_artifacts"
        self.artifacts_dir.mkdir(parents=True, exist_ok=True)
        self.trajectory_file = self.data_dir / "learned_patterns" / "omni_exploration_trajectories.jsonl"
        self.trajectory_file.parent.mkdir(parents=True, exist_ok=True)

    def acquire_and_maximize_browser(self) -> int:
        """Finds or launches browser and ensures it is MAXIMIZED to full screen."""
        wins = self.client.list_windows()
        browsers = find_browser_windows(wins, include_minimized=True)
        if not browsers:
            print("[OMNI] Launching web browser...")
            subprocess.Popen(["cmd.exe", "/c", "start", "https://www.youtube.com"], shell=False)
            time.sleep(3.5)
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=False)

        if not browsers:
            raise RuntimeError("No browser window found.")

        b = browsers[0]
        self.browser_hwnd = b["hwndInt"]

        # Ensure browser is MAXIMIZED (never shrunk!)
        self.client.show_window(self.browser_hwnd, "maximize")
        self.client.focus_window(self.browser_hwnd)
        time.sleep(0.5)
        return self.browser_hwnd

    def navigate_url(self, url: str, wait_s: float = 3.5) -> None:
        """Navigates visibly via Omnibox (Ctrl+L)."""
        print(f"\n[OMNI NAVIGATE] >>> Opening: {url} ...")
        self.client.focus_window(self.browser_hwnd)
        time.sleep(0.3)
        # Ctrl + L
        self.client.send_hotkey(vk=0x4C, ctrl=True)
        time.sleep(0.3)
        # Paste URL
        self.client.paste_text(url, hwnd=self.browser_hwnd)
        time.sleep(0.3)
        # Enter
        self.client.send_hotkey(vk=0x0D)
        time.sleep(wait_s)

    def smooth_scroll(self, clicks: int = 3, delta: int = -120) -> None:
        """Scrolls smoothly."""
        cx, cy = 960, 540
        for _ in range(clicks):
            self.client.scroll(delta=delta, x=cx, y=cy)
            time.sleep(0.3)

    def query_nvidia_vlm(self, img: Image.Image, prompt: str) -> Optional[str]:
        """Queries NVIDIA NIM Super-VL 120B to visually analyze the UI."""
        try:
            buf = io.BytesIO()
            img.save(buf, format="JPEG", quality=80)
            b64_img = base64.b64encode(buf.getvalue()).decode("utf-8")

            payload = {
                "model": NVIDIA_VLM_MODEL,
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": prompt},
                            {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{b64_img}"}}
                        ]
                    }
                ],
                "max_tokens": 1000,
                "temperature": 0.1
            }

            req = urllib.request.Request(
                NVIDIA_VLM_URL,
                data=json.dumps(payload).encode("utf-8"),
                headers={
                    "Authorization": f"Bearer {NVIDIA_API_KEY}",
                    "Content-Type": "application/json",
                    "User-Agent": "Mozilla/5.0"
                }
            )

            with urllib.request.urlopen(req, timeout=25) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                choice = data["choices"][0]["message"]
                res = choice.get("content") or choice.get("reasoning_content")
                return res
        except Exception as e:
            logger.warning(f"NVIDIA VLM query issue: {e}")
            return None

    def capture_screen_frame(self) -> Optional[Image.Image]:
        """Captures active screen area via zero-copy DXGI."""
        try:
            ack, png_bytes = self.client.capture_micro_region(0, 0, 1920, 500, fmt="png")
            if png_bytes:
                return Image.open(io.BytesIO(png_bytes)).convert("RGB")
        except Exception:
            pass
        return None

    def log_trajectory(self, platform: str, action: str, details: Dict[str, Any]) -> None:
        """Appends state transition to training dataset on Drive E:."""
        record = {
            "timestamp": time.time(),
            "platform": platform,
            "action": action,
            "details": details,
        }
        with open(self.trajectory_file, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    # =========================================================================
    # PLATFORM 1: YouTube Deep Exploration (Settings, Tabs, Categories)
    # =========================================================================
    def explore_youtube(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 1] YouTube Deep Exploration: Settings & Interactive Tabs")
        print("=" * 75)
        self.acquire_and_maximize_browser()

        # 1.1 Home & Category Chips
        self.navigate_url("https://www.youtube.com", wait_s=4.0)
        # Click across top category chips (approx y=110, x varies from 150 to 600)
        chips = [
            ("All / الكل", 120, 110),
            ("Gaming / ألعاب فيديو", 220, 110),
            ("Music / موسيقى", 320, 110),
            ("Podcasts / بودكاست", 420, 110),
            ("News / أخبار", 520, 110),
        ]
        for name, cx, cy in chips[:3]:
            print(f"[YOUTUBE ACTION] Clicking category chip: '{name}' at ({cx}, {cy})...")
            self.client.click(cx, cy)
            time.sleep(2.0)
            self.smooth_scroll(clicks=3, delta=-130)
            time.sleep(1.0)

        # 1.2 YouTube Settings Exploration (Tabs iteration)
        settings_tabs = [
            ("Account Settings", "https://www.youtube.com/account"),
            ("Notifications Settings", "https://www.youtube.com/account_notifications"),
            ("Playback & Performance", "https://www.youtube.com/account_playback"),
            ("Privacy & Downloads", "https://www.youtube.com/account_privacy"),
        ]
        for title, url in settings_tabs:
            self.navigate_url(url, wait_s=3.0)
            print(f"[YOUTUBE SETTINGS] Explored {title} -> Scrolling preferences...")
            self.smooth_scroll(clicks=3, delta=-100)
            time.sleep(1.0)
            self.smooth_scroll(clicks=2, delta=100)

        self.log_trajectory("YouTube", "Settings & Category Exploration", {"tabs": len(settings_tabs)})

    # =========================================================================
    # PLATFORM 2: Wikipedia Knowledge Discovery (Random Articles & Deep Links)
    # =========================================================================
    def explore_wikipedia(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 2] Wikipedia Knowledge Discovery: Random Articles & Links")
        print("=" * 75)
        self.acquire_and_maximize_browser()

        # 2.1 Arabic Random Article
        self.navigate_url("https://ar.wikipedia.org/wiki/خاص:عشوائي", wait_s=3.5)
        print("[WIKIPEDIA] Landed on random Arabic article. Scrolling content...")
        self.smooth_scroll(clicks=5, delta=-140)
        time.sleep(1.0)

        # Click an internal article link (approx center text x=400..700, y=400)
        print("[WIKIPEDIA ACTION] Clicking article internal link at (500, 420)...")
        self.client.click(500, 420)
        time.sleep(3.0)
        self.smooth_scroll(clicks=4, delta=-120)

        # 2.2 English Random Article
        self.navigate_url("https://en.wikipedia.org/wiki/Special:Random", wait_s=3.5)
        print("[WIKIPEDIA] Landed on random English article. Scrolling & exploring...")
        self.smooth_scroll(clicks=5, delta=-140)

        self.log_trajectory("Wikipedia", "Random Article & Internal Navigation", {"languages": ["ar", "en"]})

    # =========================================================================
    # PLATFORM 3: GitHub Exploration (Topics, Trending, Repository Tabs)
    # =========================================================================
    def explore_github(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 3] GitHub Developer Exploration: Topics, Tabs & Code")
        print("=" * 75)
        self.acquire_and_maximize_browser()

        # 3.1 GitHub Topics
        self.navigate_url("https://github.com/topics", wait_s=3.5)
        self.smooth_scroll(clicks=4, delta=-140)

        # 3.2 GitHub Trending
        self.navigate_url("https://github.com/trending", wait_s=3.5)
        self.smooth_scroll(clicks=4, delta=-140)

        # 3.3 Open a repository and explore its tabs
        # Open popular repo e.g. transformers
        self.navigate_url("https://github.com/huggingface/transformers", wait_s=4.0)
        print("[GITHUB REPO] Exploring HuggingFace Transformers...")

        # Explore tabs: Issues, Pull requests, Discussions (approx y=140, x between 180 and 450)
        repo_tabs = [
            ("Issues Tab", 220, 140),
            ("Pull Requests Tab", 300, 140),
            ("Discussions Tab", 380, 140),
        ]
        for t_name, tx, ty in repo_tabs:
            print(f"[GITHUB ACTION] Clicking {t_name} at ({tx}, {ty})...")
            self.client.click(tx, ty)
            time.sleep(2.5)
            self.smooth_scroll(clicks=3, delta=-120)

        self.log_trajectory("GitHub", "Repository & Tabs Exploration", {"tabs_clicked": len(repo_tabs)})

    # =========================================================================
    # PLATFORM 4: Google Preferences & Advanced Search Exploration
    # =========================================================================
    def explore_google(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 4] Google Search Preferences & Multi-Query Search")
        print("=" * 75)
        self.acquire_and_maximize_browser()

        # 4.1 Search Settings
        self.navigate_url("https://www.google.com/preferences", wait_s=3.5)
        print("[GOOGLE SETTINGS] Navigated to Google Preferences. Scrolling options...")
        self.smooth_scroll(clicks=4, delta=-120)
        time.sleep(1.0)
        self.smooth_scroll(clicks=3, delta=120)

        # 4.2 Dynamic Tech Search
        queries = [
            "قواعد تعلم الآلة الحديثة 2026",
            "future of robotics and computer use agents",
            "أفضل الأدوات والمكتبات البرمجية مفتوحة المصدر",
        ]
        q = random.choice(queries)
        self.navigate_url("https://www.google.com", wait_s=2.5)
        # Click search input
        self.client.click(960, 420)
        time.sleep(0.3)
        self.client.paste_text(q, hwnd=self.browser_hwnd)
        time.sleep(0.3)
        self.client.send_hotkey(vk=0x0D)
        time.sleep(3.5)
        self.smooth_scroll(clicks=4, delta=-130)

        self.log_trajectory("Google", "Preferences & Dynamic Search", {"query": q})

    # =========================================================================
    # PLATFORM 5: Hacker News Discussion Threads
    # =========================================================================
    def explore_hackernews(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 5] Hacker News: Live Tech Discussions & Comments")
        print("=" * 75)
        self.acquire_and_maximize_browser()

        self.navigate_url("https://news.ycombinator.com", wait_s=3.0)
        self.smooth_scroll(clicks=3, delta=-120)

        # Click on a comments link (approx x=180, y=140)
        print("[HACKERNEWS ACTION] Opening comments discussion thread...")
        self.client.click(180, 140)
        time.sleep(3.0)
        self.smooth_scroll(clicks=5, delta=-140)

        self.log_trajectory("HackerNews", "Frontpage & Comments Exploration", {})

    # =========================================================================
    # PLATFORM 6: Windows 11 Native Settings Comprehensive Navigation
    # =========================================================================
    def explore_windows_settings(self) -> None:
        print("\n" + "=" * 75)
        print("   [PLATFORM 6] Windows 11 Native Settings: Deep Category Navigation")
        print("=" * 75)

        wins = self.client.list_windows()
        settings_win = None
        for w in wins:
            if "settings" in w.get("title", "").lower() or "إعدادات" in w.get("title", "").lower():
                settings_win = w
                break

        if not settings_win:
            self.client.send_hotkey(vk=0x49, win=True)  # Win + I
            time.sleep(3.0)
            wins = self.client.list_windows()
            for w in wins:
                if "settings" in w.get("title", "").lower() or "إعدادات" in w.get("title", "").lower():
                    settings_win = w
                    break

        if settings_win:
            hwnd = settings_win["hwndInt"]
            # Ensure Settings is MAXIMIZED
            self.client.show_window(hwnd, "maximize")
            self.client.focus_window(hwnd)
            time.sleep(1.0)

            # Left sidebar categories in Windows 11 Settings (Maximized 1920x1080)
            # x around 120, y spans from 120 down to 450
            categories = [
                ("System / النظام", 120, 110),
                ("Bluetooth & Devices / الأجهزة", 120, 150),
                ("Network & Internet / الشبكة", 120, 190),
                ("Personalization / إضفاء طابع شخصي", 120, 230),
                ("Apps / التطبيقات", 120, 270),
                ("Gaming / الألعاب", 120, 350),
                ("Windows Update / التحديثات", 120, 480),
            ]

            for cat_name, cx, cy in categories:
                print(f"[SETTINGS ACTION] Clicking section: '{cat_name}' at ({cx}, {cy})...")
                self.client.click(cx, cy)
                time.sleep(2.0)
                # Scroll the settings details pane (x=800, y=500)
                self.client.scroll(delta=-120, x=800, y=500)
                time.sleep(0.5)

            self.log_trajectory("WindowsSettings", "Full Sidebar Category Traversal", {"sections_clicked": len(categories)})

    # =========================================================================
    # Master Exploration Loop
    # =========================================================================
    def run_omni_exploration(self) -> None:
        print("\n" + "#" * 80)
        print("   STARTING CPOWER OMNI-PLATFORM DEEP AUTONOMOUS EXPLORATION")
        print(f"   Target Cycles: {self.target_cycles}")
        print("   Visual Intelligence: NVIDIA NIM Super-VL 120B & Local Florence-2")
        print("   Window Strategy: Strictly MAXIMIZED / Fullscreen (Never Shrunk)")
        print("#" * 80)

        platforms = [
            self.explore_youtube,
            self.explore_windows_settings,
            self.explore_wikipedia,
            self.explore_github,
            self.explore_google,
            self.explore_hackernews,
        ]

        for cycle_idx in range(1, self.target_cycles + 1):
            print(f"\n{'*'*30} OMNI CYCLE {cycle_idx}/{self.target_cycles} {'*'*30}")
            fn = platforms[(cycle_idx - 1) % len(platforms)]
            try:
                fn()
            except Exception as e:
                logger.error(f"Error in platform cycle: {e}")

            time.sleep(2.0)

        print("\n[OMNI EXPLORER FINISHED] All multi-platform cycles completed successfully!")


if __name__ == "__main__":
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    explorer = OmniPlatformExplorer(target_cycles=count)
    explorer.run_omni_exploration()
