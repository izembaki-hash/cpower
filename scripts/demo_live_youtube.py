"""
Live Interactive Demo for CPower (Windows-Native C11 Computer-Use Agent).

Demonstrates CPower controlling a real Windows desktop without screenshots:
  1. Enumerates live desktop windows via CPower IPC (`list_windows`).
  2. Performs a human-like Bezier cursor glide (`move` in EXEC_MODE_HUMAN_EMULATION).
  3. Launches Microsoft Edge (or Chrome) on the user's interactive desktop,
     focuses & maximizes it, reads YouTube's UI Automation tree, searches for
     a song, invokes the result, verifies playback controls, and keeps Edge open.

Usage:
  python scripts/demo_live_youtube.py
  python scripts/demo_live_youtube.py --query "Fairuz Kifak Inta"
  python scripts/demo_live_youtube.py --auto-close
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "clients" / "python"))

from cpower_client import CPowerClient  # noqa: E402


BROWSER_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]


def find_browser() -> str:
    for path in BROWSER_CANDIDATES:
        if os.path.isfile(path):
            return path
    raise FileNotFoundError("Could not find Microsoft Edge or Google Chrome.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Live CPower YouTube & desktop demo")
    parser.add_argument("--query", default="Fairuz Kifak Inta", help="Song query to search on YouTube")
    parser.add_argument("--transport", choices=("shm", "pipe"), default="shm", help="IPC transport")
    parser.add_argument("--auto-close", action="store_true", help="Close spawned browser at the end")
    args = parser.parse_args()

    exe_path = REPO_ROOT / "bin" / "cpower.exe"
    browser_exe = find_browser()
    tmp_profile = tempfile.mkdtemp(prefix="cpower_live_yt_") if args.auto_close else None
    proc: subprocess.Popen[bytes] | None = None

    try:
        with CPowerClient(exe_path=str(exe_path), transport=args.transport, timeout_ms=10000) as c:
            desktop_wins = c.list_windows()
            existing_hwnds = {w["hwndInt"] for w in desktop_wins}
            print(f"[1] CPower connected ({args.transport.upper()}). Visible windows on current desktop: {len(desktop_wins)}")
            for w in desktop_wins[:8]:
                print(f"    - hwnd={w['hwnd']} pid={w['pid']} title={w['title'][:55]!r}")

            print("[2] Testing human-like Bezier mouse move to (640, 420)...")
            mv = c.move(640, 420, speed=0.85)
            mv_ack = mv["ack"]
            if mv_ack["succeeded"]:
                print(f"    Mouse moved smoothly in {mv_ack['elapsedUs'] / 1000.0:.1f} ms (hr={mv_ack['hresultHex']})")
            else:
                print(
                    f"    Mouse SendInput returned {mv_ack['hresultHex']} "
                    "(expected inside an isolated non-active WinStation; works on active desktop)"
                )

            launch_args = [
                browser_exe,
                "--new-window",
                "--force-renderer-accessibility",
                "--window-size=1280,900",
                "https://www.youtube.com",
            ]
            if tmp_profile:
                launch_args[1:1] = [
                    f"--user-data-dir={tmp_profile}",
                    "--no-first-run",
                    "--no-default-browser-check",
                    "--disable-features=msEdgeFirstRun",
                ]

            print(f"[3] Launching browser: {Path(browser_exe).name} -> https://www.youtube.com")
            proc = subprocess.Popen(launch_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            hwnd = 0
            target_win = None
            for _ in range(45):
                wins_now = c.list_windows()
                new_wins = [w for w in wins_now if w["hwndInt"] not in existing_hwnds and "Chrome_WidgetWin" in w.get("className", "")]
                for w in new_wins:
                    if w.get("pid") == proc.pid or "YouTube" in w.get("title", "") or "Edge" in w.get("title", "") or "Chrome" in w.get("title", "") or w.get("title", ""):
                        hwnd = int(w["hwndInt"])
                        target_win = w
                        break
                if not hwnd:
                    for w in wins_now:
                        if "Chrome_WidgetWin" in w.get("className", "") and "YouTube" in w.get("title", ""):
                            hwnd = int(w["hwndInt"])
                            target_win = w
                            break
                if hwnd:
                    break
                time.sleep(0.3)

            if not hwnd or target_win is None:
                raise RuntimeError("Browser window did not appear in time.")

            print(f"[4] Focusing & maximizing browser hwnd={target_win['hwnd']} title={target_win['title'][:60]!r}")
            c.focus_window(hwnd)
            c.show_window(hwnd, "maximize")

            tree = None
            search_box = None
            for _ in range(35):
                tree = c.capture_tree(hwnd=hwnd)
                for btn in c.find_nodes(control_type="Button", tree=tree):
                    bn = btn["name"].lower()
                    if "reject all" in bn or "accept all" in bn:
                        c.uia_invoke(node_id=btn["id"], hwnd=hwnd)
                        time.sleep(1.2)
                        break

                candidates = c.find_nodes(control_type="Edit", tree=tree) + c.find_nodes(control_type="ComboBox", tree=tree)
                for cand in candidates:
                    nm = (cand.get("name", "") or "").lower()
                    aid = (cand.get("automationId", "") or "").lower()
                    if ("search" in nm and "address" not in nm) or aid == "search" or "بحث" in nm:
                        search_box = cand
                        break
                if search_box and len(tree["nodes"]) > 40:
                    break
                time.sleep(0.7)

            if not search_box or tree is None:
                raise RuntimeError("YouTube search box was not detected in UIA tree.")

            print(f"[5] Zero-screenshot UIA captured {len(tree['nodes'])} nodes. Setting query={args.query!r} into [{search_box['id']}]...")
            c.uia_set_value(text=args.query, node_id=search_box["id"], hwnd=hwnd)

            tree_after = c.capture_tree(hwnd=hwnd)
            search_btns = [
                b for b in c.find_nodes(control_type="Button", tree=tree_after)
                if b["name"].strip().lower() == "search" or "بحث" in b["name"].lower()
            ]
            if not search_btns:
                raise RuntimeError("YouTube Search submit button not found in UIA tree.")

            print(f"[6] Invoking Search button [{search_btns[0]['id']}]...")
            c.uia_invoke(node_id=search_btns[0]["id"], hwnd=hwnd)

            watch_links = []
            for _ in range(25):
                time.sleep(0.8)
                res_tree = c.capture_tree(hwnd=hwnd)
                watch_links = [
                    lk for lk in c.find_nodes(control_type="Hyperlink", tree=res_tree)
                    if "youtube.com/watch?v=" in (lk.get("value") or "")
                    and len((lk.get("name") or "").strip()) > 3
                ]
                if watch_links:
                    break

            if not watch_links:
                raise RuntimeError("No YouTube watch links found in UIA results.")

            chosen = watch_links[0]
            print(f"[7] Invoking top result [{chosen['id']}]: {chosen['name'][:65]!r}")
            c.uia_invoke(node_id=chosen["id"], hwnd=hwnd)

            for _ in range(25):
                time.sleep(0.8)
                watch_tree = c.capture_tree(hwnd=hwnd)
                addrs = [e for e in c.find_nodes(control_type="Edit", tree=watch_tree) if "address" in e["name"].lower()]
                url_now = addrs[0].get("value", "") if addrs else ""
                pbtns = [
                    b for b in c.find_nodes(control_type="Button", tree=watch_tree)
                    if any(k in b["name"].lower() for k in ("pause", "play (k)", "mute"))
                ]
                if "watch?v=" in url_now and pbtns:
                    sliders = c.find_nodes(control_type="Slider", tree=watch_tree)
                    s_val = sliders[0].get("value", "") if sliders else ""
                    print(f"[8] Playing now: {url_now}")
                    print(f"    Controls: {[(b['id'], b['name']) for b in pbtns]} | Seek: {s_val!r}")
                    break

        return 0
    finally:
        if args.auto_close:
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except Exception:
                    proc.kill()
            if tmp_profile:
                shutil.rmtree(tmp_profile, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
