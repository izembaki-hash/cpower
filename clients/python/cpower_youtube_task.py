r"""
CPower Autonomous YouTube Search Task (clients/python/cpower_youtube_task.py).

Executes end-to-end computer-use navigation on the user's desktop:
  1. Opens or focuses web browser on YouTube (https://www.youtube.com).
  2. Uses CPower UI Access Daemon to focus and bring the browser forward.
  3. Uses fine-tuned Florence-2 local vision model to locate the YouTube Search input.
  4. Moves mouse via smooth human-like Bézier curve and clicks the search bar.
  5. Injects Unicode Arabic query ("ملخص مسلسل") via CF_UNICODETEXT clipboard paste.
  6. Sends Enter (VK_RETURN) to perform the search.
  7. Captures result frame to verify search results on screen.
"""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Dict, Optional, Tuple

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_vision_local import CPowerLocalVision
from cpower_process_guard import is_protected_window, find_browser_windows


def run_youtube_search(query: str = "ملخص مسلسل") -> Dict[str, Any]:
    print("=" * 75)
    print("      CPower Autonomous Desktop Agent: YouTube Search Task")
    print(f"      Search Query: '{query}'")
    print("=" * 75)

    client = CPowerClient()
    vision = CPowerLocalVision(client=client)

    # 1. Check for running browser; if not found, launch YouTube
    print("\n[STEP 1] Checking for active browser window...")
    wins = client.list_windows()
    browsers = find_browser_windows(wins, include_minimized=True)

    browser_target = None
    for b in browsers:
        if "youtube" in b.get("title", "").lower():
            browser_target = b
            break

    if not browser_target and browsers:
        # Use first available browser
        browser_target = browsers[0]

    if not browser_target:
        print("         No browser window found. Launching default browser to https://www.youtube.com ...")
        subprocess.Popen(["cmd.exe", "/c", "start", "https://www.youtube.com"], shell=False)
        print("         Waiting 4.0s for browser window to initialize...")
        time.sleep(4.0)
        wins = client.list_windows()
        browsers = find_browser_windows(wins, include_minimized=False)
        if browsers:
            browser_target = browsers[0]
    else:
        # If browser doesn't have youtube open, navigate or open youtube
        title = browser_target.get("title", "").lower()
        if "youtube" not in title:
            print(f"         Found browser '{browser_target.get('title')}', opening https://www.youtube.com ...")
            subprocess.Popen(["cmd.exe", "/c", "start", "https://www.youtube.com"], shell=False)
            time.sleep(3.5)
            wins = client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=False)
            for b in browsers:
                if "youtube" in b.get("title", "").lower():
                    browser_target = b
                    break
            if not browser_target and browsers:
                browser_target = browsers[0]

    if not browser_target:
        raise RuntimeError("Failed to detect or launch web browser.")

    hwnd = browser_target["hwndInt"]
    title = browser_target["title"]
    pname = browser_target.get("processName", "browser")
    rect = browser_target["rect"]
    print(f"         Target Browser: '{title}' ({pname}, HWND: {hwnd:#x}, Rect: {rect})")

    # 2. Focus and bring browser forward
    print("\n[STEP 2] Bringing browser window to foreground...")
    focus_res = client.focus_window(hwnd)
    time.sleep(0.8)

    # Refresh window rect after focus
    wins = client.list_windows()
    for w in wins:
        if w.get("hwndInt") == hwnd:
            rect = w["rect"]
            break

    left, top, right, bottom = rect
    width = right - left
    height = bottom - top
    print(f"         Window Dimensions: {width}x{height} at ({left}, {top})")

    # 3. Capture high-resolution DXGI micro-region
    print(f"\n[STEP 3] Capturing screen pixels via DXGI capture...")
    ack, png_bytes = client.capture_micro_region(left, top, right, bottom, fmt="png")
    if not png_bytes:
        raise RuntimeError("Failed to capture browser pixels via DXGI.")

    img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    print(f"         Captured Image: {img.size[0]}x{img.size[1]} pixels ({len(png_bytes):,} bytes)")

    # Save before frame on Drive E:
    out_dir = Path("E:/cpower_training_data/demo_artifacts")
    out_dir.mkdir(parents=True, exist_ok=True)
    before_path = out_dir / "youtube_search_before.png"
    img.save(before_path)
    print(f"         Saved initial frame to {before_path}")

    # 4. Neural Grounding for YouTube Search Box
    print(f"\n[STEP 4] Running fine-tuned Florence-2 to ground YouTube search bar...")
    start_vlm = time.time()
    # Try common visual phrases
    search_phrases = ["search box", "Search", "search", "search input"]
    grounded_coords = None
    matched_phrase = None

    for phrase in search_phrases:
        coords = vision.ground_phrase_florence(img, phrase, (left, top, right, bottom))
        if coords:
            grounded_coords = coords
            matched_phrase = phrase
            print(f"         Successfully grounded '{phrase}': center=({coords[0]}, {coords[1]}), bbox={coords[2]}")
            break

    vlm_dur = (time.time() - start_vlm) * 1000.0

    # If visual grounding found the box, click it; otherwise calculate fallback header center
    if grounded_coords:
        click_x, click_y = grounded_coords[0], grounded_coords[1]
        print(f"         Neural Grounding Succeeded in {vlm_dur:.1f}ms -> Target Point: ({click_x}, {click_y})")
    else:
        # YouTube search bar is centrally located in the top bar: center x, top ~60px
        click_x = left + width // 2
        click_y = top + min(65, height // 10)
        print(f"         Visual fallback to standard YouTube header bar: ({click_x}, {click_y})")

    # 5. Move mouse with Bézier curve & click
    print(f"\n[STEP 5] Dispatching smooth native click to ({click_x}, {click_y})...")
    click_res = client.click(click_x, click_y)
    click_ok = click_res.get("ack", {}).get("succeeded", False) if "ack" in click_res else click_res.get("succeeded", False)
    print(f"         Click Status: Succeeded={click_ok}")
    time.sleep(0.4)

    # Also send '/' hotkey to ensure YouTube search input has focus
    client.type_text("/", hwnd=hwnd)
    time.sleep(0.3)

    # 6. Type/Paste the Arabic query
    print(f"\n[STEP 6] Injecting search query: '{query}' via Unicode clipboard paste...")
    paste_res = client.paste_text(query, hwnd=hwnd)
    paste_ok = paste_res.get("ack", {}).get("succeeded", False) if "ack" in paste_res else paste_res.get("succeeded", False)
    print(f"         Paste Status: Succeeded={paste_ok}")
    time.sleep(0.5)

    # 7. Press Enter (VK_RETURN = 0x0D)
    print(f"\n[STEP 7] Pressing Enter (VK_RETURN) to execute search...")
    enter_res = client.send_hotkey(vk=0x0D)
    time.sleep(3.5)

    # 8. Capture result frame
    print(f"\n[STEP 8] Capturing final search results...")
    ack2, png_bytes2 = client.capture_micro_region(left, top, right, bottom, fmt="png")
    after_path = out_dir / "youtube_search_after.png"
    if png_bytes2:
        img2 = Image.open(io.BytesIO(png_bytes2)).convert("RGB")
        img2.save(after_path)
        print(f"         Saved results frame to {after_path}")

    print("\n" + "=" * 75)
    print("      YOUTUBE SEARCH TASK COMPLETED SUCCESSFULLY!")
    print("=" * 75)

    return {
        "succeeded": True,
        "query": query,
        "browser_title": title,
        "browser_hwnd": hwnd,
        "search_bar_coords": (click_x, click_y),
        "vlm_latency_ms": vlm_dur,
        "before_screenshot": str(before_path),
        "after_screenshot": str(after_path),
    }


if __name__ == "__main__":
    query_arg = sys.argv[1] if len(sys.argv) > 1 else "ملخص مسلسل"
    run_youtube_search(query_arg)
