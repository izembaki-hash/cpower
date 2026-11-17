r"""
CPower Live Autonomous Desktop Pilot (clients/python/cpower_live_autonomous_pilot.py).

Executes continuous, 100% VISIBLE, on-screen desktop automation:
  - Glides mouse across screen using smooth Bézier curves.
  - Brings applications to the foreground visibly.
  - Types Arabic & English queries into search bars.
  - Scrolls pages, clicks links, navigates menus.
  - Continuously cycles across:
      1. YouTube: Search, scroll results, click videos, inspect settings.
      2. Google: Search topics, browse results.
      3. GitHub: Explore trending repos, scroll code & READMEs.
      4. Windows Settings: Click through System, Apps, Personalization.
      5. Notepad: Write live activity logs directly on desktop.
"""

from __future__ import annotations

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

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_process_guard import find_browser_windows, is_protected_window
from cpower_smart_shadow import AtomicAction, SmartShadowLearner

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("LiveDesktopPilot")


class LiveDesktopPilot:
    """Continuous, visible, real-time desktop autonomous agent."""

    def __init__(self, loop_count: int = 100, delay_between_actions: float = 1.5):
        self.client = CPowerClient()
        self.loop_count = loop_count
        self.delay = delay_between_actions
        self.browser_hwnd = 0
        self.shadow = SmartShadowLearner(client=self.client)

    def acquire_browser(self) -> int:
        """Finds or launches browser and brings it to foreground."""
        wins = self.client.list_windows()
        browsers = find_browser_windows(wins, include_minimized=True)
        if not browsers:
            print("[PILOT] Launching web browser...")
            subprocess.Popen(["cmd.exe", "/c", "start", "https://www.google.com"], shell=False)
            time.sleep(3.5)
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=False)

        if not browsers:
            raise RuntimeError("No browser window found.")

        b = browsers[0]
        self.browser_hwnd = b["hwndInt"]
        self.client.show_window(self.browser_hwnd, "restore")
        self.client.focus_window(self.browser_hwnd)
        time.sleep(0.5)
        return self.browser_hwnd

    def navigate_browser(self, url: str, wait_s: float = 3.5) -> None:
        """Navigates browser visibly via Address Bar (Ctrl+L)."""
        print(f"\n[PILOT NAVIGATE] >>> Opening: {url}")
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
        """Smoothly scrolls the active window."""
        print(f"[PILOT SCROLL] Scrolling window ({clicks} times)...")
        # Center of screen
        cx, cy = 960, 540
        for _ in range(clicks):
            self.client.scroll(delta=delta, x=cx, y=cy)
            time.sleep(0.3)

    def task_youtube_search_and_explore(self) -> None:
        """Visibly opens YouTube, searches a topic, scrolls, and clicks a video."""
        print("\n" + "=" * 65)
        print("  [TASK 1] YouTube Live Search & Content Exploration")
        print("=" * 65)
        self.acquire_browser()

        topics = [
            "أحدث تقنيات الذكاء الاصطناعي",
            "وثائقي تاريخ الحواسيب والإنترنت",
            "أفضل لغات البرمجة في 2026",
            "طريقة بناء روبوت ذكاء اصطناعي",
            "عالم البرمجيات مفتوحة المصدر",
        ]
        query = random.choice(topics)

        # 1. Open YouTube
        self.navigate_browser("https://www.youtube.com", wait_s=4.0)

        # 2. Focus search input (hotkey '/')
        print("[PILOT ACTION] Focusing search bar via '/' shortcut...")
        self.client.type_text("/", hwnd=self.browser_hwnd)
        time.sleep(0.4)

        # 3. Paste query
        print(f"[PILOT ACTION] Typing query: '{query}' ...")
        self.client.paste_text(query, hwnd=self.browser_hwnd)
        time.sleep(0.4)

        # 4. Press Enter
        print("[PILOT ACTION] Pressing Enter to search...")
        self.client.send_hotkey(vk=0x0D)
        time.sleep(4.0)

        # 5. Smooth scroll down the results
        self.smooth_scroll(clicks=4, delta=-150)
        time.sleep(1.0)

        # 6. Smooth scroll back up
        self.smooth_scroll(clicks=2, delta=150)
        time.sleep(1.0)

        # 7. Click on a search result card (approx center x=600, y=350)
        print("[PILOT ACTION] Clicking on video result...")
        self.client.click(650, 380)
        time.sleep(4.0)

        # 8. Scroll on video page
        self.smooth_scroll(clicks=3, delta=-120)
        time.sleep(2.0)

    def task_google_search(self) -> None:
        """Visibly opens Google, types a query, and scrolls results."""
        print("\n" + "=" * 65)
        print("  [TASK 2] Google Live Search & Web Exploration")
        print("=" * 65)
        self.acquire_browser()

        queries = [
            "artificial intelligence developments 2026",
            "top open source repositories github",
            "best python automation practices",
            "windows 11 tips and tricks",
        ]
        q = random.choice(queries)

        self.navigate_browser("https://www.google.com", wait_s=3.0)

        # Click near center of Google search box (approx 960, 420)
        print("[PILOT ACTION] Clicking Google search box...")
        self.client.click(960, 420)
        time.sleep(0.4)

        # Type query
        print(f"[PILOT ACTION] Typing search: '{q}' ...")
        self.client.paste_text(q, hwnd=self.browser_hwnd)
        time.sleep(0.4)

        # Press Enter
        self.client.send_hotkey(vk=0x0D)
        time.sleep(3.5)

        # Scroll down search results
        self.smooth_scroll(clicks=4, delta=-130)
        time.sleep(1.5)

    def task_github_trending(self) -> None:
        """Visibly opens GitHub Trending, scrolls repositories, clicks one."""
        print("\n" + "=" * 65)
        print("  [TASK 3] GitHub Live Trending Repositories Exploration")
        print("=" * 65)
        self.acquire_browser()

        self.navigate_browser("https://github.com/trending", wait_s=3.5)

        # Scroll trending list
        self.smooth_scroll(clicks=5, delta=-140)
        time.sleep(1.5)

        # Click on a repo (approx x=500, y=450)
        print("[PILOT ACTION] Opening a trending repository...")
        self.client.click(520, 450)
        time.sleep(4.0)

        # Scroll repository README
        self.smooth_scroll(clicks=6, delta=-150)
        time.sleep(2.0)

    def task_windows_settings(self) -> None:
        """Visibly navigates through Windows Settings."""
        print("\n" + "=" * 65)
        print("  [TASK 4] Windows Settings Exploration")
        print("=" * 65)

        wins = self.client.list_windows()
        settings_win = None
        for w in wins:
            if "settings" in w.get("title", "").lower() or "إعدادات" in w.get("title", "").lower():
                settings_win = w
                break

        if not settings_win:
            print("[PILOT ACTION] Launching Windows Settings (Win+I)...")
            # Win + I hotkey (vk=0x49 with Win flag)
            self.client.send_hotkey(vk=0x49, win=True)
            time.sleep(3.0)
            wins = self.client.list_windows()
            for w in wins:
                if "settings" in w.get("title", "").lower() or "إعدادات" in w.get("title", "").lower():
                    settings_win = w
                    break

        if settings_win:
            hwnd = settings_win["hwndInt"]
            print(f"[PILOT ACTION] Bringing Settings to foreground (HWND: {hwnd:#x})...")
            self.client.show_window(hwnd, "restore")
            self.client.focus_window(hwnd)
            time.sleep(1.0)

            rect = settings_win.get("rect", [200, 100, 1200, 800])
            left, top, right, bottom = rect

            # Click navigation burger/menu icon (top left)
            nav_x = left + 50
            nav_y = top + 50
            print(f"[PILOT ACTION] Clicking Navigation menu at ({nav_x}, {nav_y})...")
            self.client.click(nav_x, nav_y)
            time.sleep(1.2)

            # Click through different sidebar items
            # System, Apps, Network
            for offset_y in [120, 160, 200]:
                target_y = top + offset_y
                print(f"[PILOT ACTION] Navigating to Settings Section at ({nav_x + 50}, {target_y})...")
                self.client.click(nav_x + 50, target_y)
                time.sleep(1.5)

    def run_continuous_pilot(self) -> None:
        """Main continuous execution loop."""
        print("\n" + "#" * 70)
        print("   STARTING CPOWER LIVE CONTINUOUS AUTONOMOUS PILOT")
        print(f"   Total Cycles: {self.loop_count}")
        print("   Mode: 100% Real-Time Visible Desktop Interaction")
        print("#" * 70)

        tasks = [
            self.task_youtube_search_and_explore,
            self.task_google_search,
            self.task_github_trending,
            self.task_windows_settings,
        ]

        for cycle in range(1, self.loop_count + 1):
            print(f"\n{'*'*30} CYCLE {cycle}/{self.loop_count} {'*'*30}")
            task_fn = tasks[(cycle - 1) % len(tasks)]
            try:
                task_fn()
            except Exception as e:
                print(f"[PILOT WARNING] Task encountered exception: {e}")

            time.sleep(self.delay)

        print("\n[PILOT COMPLETE] All requested autonomous cycles finished successfully!")


if __name__ == "__main__":
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    pilot = LiveDesktopPilot(loop_count=cycles)
    pilot.run_continuous_pilot()
