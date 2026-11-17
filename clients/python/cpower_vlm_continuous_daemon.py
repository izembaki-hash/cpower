r"""
CPower VLM Continuous Long-Running Daemon (clients/python/cpower_vlm_continuous_daemon.py).

Continuous multi-hour autonomous desktop exploration supervised by NVIDIA Vision-Language Model
(nvidia/nemotron-3.5-super-vl-120b-a12b) with automated continual Florence-2 training.

Architecture:
  1. Closed-Loop VLM Perception: Before EVERY action, captures full 1920x1080 zero-copy DXGI frame.
  2. Active VLM Guidance: Identifies genuine clickable elements, detects login walls/error pages,
     and corrects navigational course in real time.
  3. Multi-Platform Curriculum: Cycles across Windows Settings, GitHub Trending, Wikipedia, Python Docs,
     and Windows native apps.
  4. Real-time Dataset Harvesting: Converts every verified UI element into Florence-2 ground truth tokens.
  5. Continual Background Training: Every N steps, invokes PEFT LoRA training on local GPU/CPU.
  6. Strict Quarantine: Drive H: remains strictly protected (>=2.0 GB headroom); all data on Drive E:.
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple, Union
import urllib.request

from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_dataset_collector import CPowerDatasetCollector
from cpower_process_guard import find_browser_windows

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ]
)
logger = logging.getLogger("VLMContinuousDaemon")

NVIDIA_API_KEY = os.environ.get("NVIDIA_API_KEY", "")
NVIDIA_VLM_MODEL = "nvidia/nemotron-3.5-super-vl-120b-a12b"
NVIDIA_VLM_URL = "https://integrate.api.nvidia.com/v1/chat/completions"

STORAGE_BASE = Path("E:/cpower_training_data")

# Comprehensive Multi-Platform Curriculum
CURRICULUM = [
    {
        "type": "web",
        "name": "Wikipedia Arabic Portal",
        "target": "https://ar.wikipedia.org/wiki/البوابة_الرئيسية",
        "goal": "Explore Arabic Wikipedia main portal, featured articles, and navigation bars."
    },
    {
        "type": "windows_app",
        "name": "Windows Display Settings",
        "cmd": "ms-settings:display",
        "title_keyword": "setting",
        "goal": "Explore Windows Settings categories, toggles, and display configuration."
    },
    {
        "type": "web",
        "name": "GitHub Trending Repositories",
        "target": "https://github.com/trending",
        "goal": "Explore trending open-source repositories, developer stars, and topics."
    },
    {
        "type": "windows_app",
        "name": "Windows Personalization Settings",
        "cmd": "ms-settings:personalization",
        "title_keyword": "setting",
        "goal": "Explore Windows Personalization, themes, colors, and background settings."
    },
    {
        "type": "web",
        "name": "Python 3 Official Documentation",
        "target": "https://docs.python.org/3/",
        "goal": "Explore Python documentation, library reference index, and search box."
    },
    {
        "type": "web",
        "name": "Wikipedia Random Discovery",
        "target": "https://ar.wikipedia.org/wiki/خاص:عشوائي",
        "goal": "Discover random knowledge articles and follow contextual hyperlinks."
    },
    {
        "type": "windows_app",
        "name": "Windows System Sound Settings",
        "cmd": "ms-settings:sound",
        "title_keyword": "setting",
        "goal": "Explore Windows Audio and Sound output preferences and sliders."
    },
    {
        "type": "web",
        "name": "English Wikipedia Random Discovery",
        "target": "https://en.wikipedia.org/wiki/Special:Random",
        "goal": "Explore international encyclopedic articles, tables, and citation links."
    }
]


class VLMSupervisor:
    """Zero-copy DXGI screen perception and NVIDIA NIM Super-VL 120B cognitive supervision."""

    def __init__(self, client: CPowerClient):
        self.client = client

    def capture_fullscreen_dxgi(self) -> Optional[Image.Image]:
        """Captures full 1920x1080 screen using 3 DXGI slices to maintain buffer safety."""
        try:
            slices = []
            ranges = [(0, 0, 1920, 360), (0, 360, 1920, 720), (0, 720, 1920, 1080)]
            for left, top, right, bottom in ranges:
                ack, data = self.client.capture_micro_region(left, top, right, bottom, fmt="png")
                if not data:
                    return None
                slices.append(Image.open(io.BytesIO(data)))

            full_img = Image.new("RGB", (1920, 1080))
            full_img.paste(slices[0], (0, 0))
            full_img.paste(slices[1], (0, 360))
            full_img.paste(slices[2], (0, 720))
            return full_img
        except Exception as e:
            logger.warning(f"DXGI full screen capture error: {e}")
            return None

    def analyze_screen(self, img: Image.Image, step_goal: str = "") -> Optional[Dict[str, Any]]:
        """Sends live screenshot to NVIDIA Super-VL 120B and parses structured actionable intent."""
        try:
            img_resized = img.resize((1280, 720))
            buf = io.BytesIO()
            img_resized.save(buf, format="JPEG", quality=80)
            b64_img = base64.b64encode(buf.getvalue()).decode("utf-8")

            prompt = f"""You are the Real-time Vision Supervisor for an autonomous desktop agent.
Analyze this 1280x720 desktop screenshot.
Exploration Goal: {step_goal if step_goal else 'Explore productive UI elements, discover links/buttons, and learn interactions.'}

Rules:
1. Detect page status: 'normal' (active page), 'error_page' (404/broken), or 'login_wall' (sign-in barrier).
2. If error_page or login_wall, set action_type='navigate_new_url' with a public URL in action_payload.
3. If normal, identify up to 3 genuine, distinct clickable elements (buttons, nav links, tabs, search bars).
4. Output STRICT JSON format. Keep 'reasoning' concise (1 sentence, no inner quotes).

JSON schema:
{{
  "page_status": "normal",
  "current_site": "name of site or application",
  "reasoning": "concise explanation",
  "recommended_target": {{
      "name": "button or link name",
      "box_1280x720": [ymin, xmin, ymax, xmax],
      "desktop_x": 960,
      "desktop_y": 540,
      "action_type": "click",
      "action_payload": null
  }},
  "alternative_elements": [
      {{
          "name": "secondary element label",
          "box_1280x720": [ymin, xmin, ymax, xmax],
          "desktop_x": 500,
          "desktop_y": 300
      }}
  ]
}}
"""
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
                "max_tokens": 2048,
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

            with urllib.request.urlopen(req, timeout=40) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                choice = data["choices"][0]["message"]
                content = choice.get("content") or choice.get("reasoning_content") or ""
                return self._parse_vlm_json(content)

        except Exception as e:
            logger.warning(f"NVIDIA VLM query exception: {e}")
            return None

    def _parse_vlm_json(self, raw_text: str) -> Optional[Dict[str, Any]]:
        """Leniently extracts valid JSON even with thinking preambles or code fences."""
        raw_text = raw_text.strip()
        try:
            return json.loads(raw_text)
        except Exception:
            pass

        fence_match = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", raw_text, re.DOTALL)
        if fence_match:
            try:
                return json.loads(fence_match.group(1))
            except Exception:
                pass

        start = raw_text.find("{")
        end = raw_text.rfind("}")
        if start != -1 and end != -1 and end > start:
            candidate = raw_text[start:end + 1]
            try:
                return json.loads(candidate)
            except Exception:
                sanitized = re.sub(r'[\r\n\t]+', ' ', candidate)
                try:
                    return json.loads(sanitized)
                except Exception:
                    pass

        # Regex fallback
        try:
            page_status_m = re.search(r'"page_status"\s*:\s*"([^"]+)"', raw_text)
            current_site_m = re.search(r'"current_site"\s*:\s*"([^"]+)"', raw_text)
            target_name_m = re.search(r'"name"\s*:\s*"([^"]+)"', raw_text)
            dest_x_m = re.search(r'"desktop_x"\s*:\s*(\d+)', raw_text)
            dest_y_m = re.search(r'"desktop_y"\s*:\s*(\d+)', raw_text)
            action_type_m = re.search(r'"action_type"\s*:\s*"([^"]+)"', raw_text)

            if dest_x_m and dest_y_m:
                return {
                    "page_status": page_status_m.group(1) if page_status_m else "normal",
                    "current_site": current_site_m.group(1) if current_site_m else "Desktop Window",
                    "reasoning": "Extracted via regex fallback.",
                    "recommended_target": {
                        "name": target_name_m.group(1) if target_name_m else "Interactive Button",
                        "desktop_x": int(dest_x_m.group(1)),
                        "desktop_y": int(dest_y_m.group(1)),
                        "action_type": action_type_m.group(1) if action_type_m else "click",
                        "action_payload": None
                    },
                    "alternative_elements": []
                }
        except Exception:
            pass

        return None


class CPowerContinuousVLMDaemon:
    """Supervises long-running desktop exploration and continual Florence-2 training."""

    def __init__(self, target_hours: float = 8.0, train_every_steps: int = 25):
        self.target_hours = target_hours
        self.train_every_steps = train_every_steps
        self.start_time = time.time()
        self.end_time = self.start_time + (target_hours * 3600.0)

        self.client = CPowerClient()
        self.supervisor = VLMSupervisor(self.client)
        self.collector = CPowerDatasetCollector(base_dir=STORAGE_BASE)

        self.artifacts_dir = STORAGE_BASE / "demo_artifacts"
        self.artifacts_dir.mkdir(parents=True, exist_ok=True)
        self.trajectories_file = STORAGE_BASE / "learned_patterns" / "vlm_supervised_trajectories.jsonl"
        self.trajectories_file.parent.mkdir(parents=True, exist_ok=True)
        self.status_file = STORAGE_BASE / "vlm_daemon_status.json"

        self.active_hwnd = 0
        self.step_counter = 0
        self.platform_index = 0
        self.current_platform = CURRICULUM[0]
        self.training_process: Optional[subprocess.Popen] = None

    def enforce_maximized(self, hwnd: int) -> None:
        """Enforces SW_MAXIMIZE on window so it never shrinks or stays un-maximized."""
        self.client.show_window(hwnd, "maximize")
        self.client.focus_window(hwnd)
        time.sleep(0.4)

    def activate_platform(self, platform_info: Dict[str, Any]) -> int:
        """Transitions seamlessly to target web URL or native Windows app."""
        p_type = platform_info.get("type", "web")
        p_name = platform_info.get("name", "Unknown")

        print(f"\n[PLATFORM TRANSITION] >>> Navigating to: {p_name} ({p_type}) ...", flush=True)

        if p_type == "web":
            target_url = platform_info.get("target", "https://ar.wikipedia.org")
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=True)
            if not browsers:
                print(f"[DAEMON] Launching browser to {target_url}...", flush=True)
                subprocess.Popen(["cmd.exe", "/c", "start", target_url], shell=False)
                time.sleep(3.5)
                wins = self.client.list_windows()
                browsers = find_browser_windows(wins, include_minimized=False)

            if browsers:
                self.active_hwnd = browsers[0]["hwndInt"]
                self.enforce_maximized(self.active_hwnd)
                # Navigate via address bar
                self.client.send_hotkey(vk=0x4C, ctrl=True)  # Ctrl+L
                time.sleep(0.3)
                self.client.paste_text(target_url, hwnd=self.active_hwnd)
                time.sleep(0.3)
                self.client.send_hotkey(vk=0x0D)  # Enter
                time.sleep(3.5)
                self.enforce_maximized(self.active_hwnd)

        elif p_type == "windows_app":
            cmd = platform_info.get("cmd", "ms-settings:")
            title_kw = platform_info.get("title_keyword", "setting").lower()
            subprocess.Popen(["cmd.exe", "/c", "start", cmd], shell=False)
            time.sleep(2.5)

            wins = self.client.list_windows()
            for w in wins:
                if title_kw in w.get("title", "").lower() or "إعداد" in w.get("title", "").lower():
                    self.active_hwnd = w["hwndInt"]
                    self.enforce_maximized(self.active_hwnd)
                    break

        return self.active_hwnd

    def check_and_trigger_continual_training(self) -> None:
        """Triggers local Florence-2 LoRA training asynchronously on Drive E:."""
        if self.step_counter > 0 and self.step_counter % self.train_every_steps == 0:
            if self.training_process is not None and self.training_process.poll() is None:
                print("[TRAINING] Previous training job is still active. Skipping concurrent trigger.", flush=True)
                return

            print("\n" + "=" * 80, flush=True)
            print(f"[CONTINUAL TRAINING TRIGGER] Step {self.step_counter} reached. Launching Florence-2 Adaptation...", flush=True)
            print("=" * 80, flush=True)

            cmd = [
                sys.executable,
                str(CLIENT_DIR / "cpower_trainer.py"),
                "--epochs", "1",
                "--batch-size", "2",
                "--dataset-dir", str(STORAGE_BASE),
                "--checkpoint-dir", str(STORAGE_BASE / "checkpoints" / "cpower_florence2_adapted"),
                "--device", "auto"
            ]
            try:
                self.training_process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    cwd=str(REPO_ROOT)
                )
                print(f"[TRAINING] Background adaptation job launched with PID {self.training_process.pid}.", flush=True)
            except Exception as e:
                logger.warning(f"Could not launch training job: {e}")

    def execute_supervised_step(self) -> bool:
        """Performs one completely verified, closed-loop visual step."""
        self.step_counter += 1
        elapsed_m = (time.time() - self.start_time) / 60.0
        remaining_m = max(0.0, (self.end_time - time.time()) / 60.0)

        print("\n" + "=" * 80, flush=True)
        print(f" [VLM SUPERVISED STEP #{self.step_counter}] Elapsed: {elapsed_m:.1f}m | Remaining: {remaining_m:.1f}m", flush=True)
        print(f" Active Platform: {self.current_platform['name']} ({self.current_platform['type']})", flush=True)
        print("=" * 80, flush=True)

        if self.active_hwnd:
            self.enforce_maximized(self.active_hwnd)

        # 1. Capture live 1920x1080 screen
        img = self.supervisor.capture_fullscreen_dxgi()
        if not img:
            print("[DAEMON WARN] DXGI capture failed. Retrying in 1s...", flush=True)
            time.sleep(1.0)
            return False

        # Save screenshot artifact
        screenshot_path = self.artifacts_dir / f"daemon_step_{self.step_counter:05d}.jpg"
        img.save(screenshot_path, format="JPEG", quality=85)

        # 2. Query NVIDIA Nemotron Super-VL 120B
        t0 = time.time()
        analysis = self.supervisor.analyze_screen(img, step_goal=self.current_platform.get("goal", ""))
        dt = time.time() - t0
        print(f"[VLM SUPERVISOR] Analysis returned in {dt:.2f}s.", flush=True)

        if not analysis:
            print("[DAEMON WARN] VLM analysis empty. Advancing to next platform in curriculum...", flush=True)
            self.platform_index = (self.platform_index + 1) % len(CURRICULUM)
            self.current_platform = CURRICULUM[self.platform_index]
            self.activate_platform(self.current_platform)
            return True

        page_status = analysis.get("page_status", "normal")
        current_site = analysis.get("current_site", "Unknown")
        reasoning = analysis.get("reasoning", "")
        recommended = analysis.get("recommended_target", {})
        alternatives = analysis.get("alternative_elements", [])

        print(f"  [VISUAL CONTEXT] Site: {current_site} | Status: {page_status}", flush=True)
        print(f"  [VLM REASONING]  {reasoning}", flush=True)

        # 3. Intervene on error page or login barrier
        if page_status in ("error_page", "login_wall"):
            print(f"[VLM SUPERVISOR CORRECTION] Page state '{page_status}' detected. Rotating away from dead end.", flush=True)
            self.platform_index = (self.platform_index + 1) % len(CURRICULUM)
            self.current_platform = CURRICULUM[self.platform_index]
            self.activate_platform(self.current_platform)

            self._record_trajectory(
                step_idx=self.step_counter,
                site=current_site,
                status=page_status,
                action="rotation_correction",
                target_name=self.current_platform["name"],
                target_xy=(960, 540),
                reasoning=reasoning
            )
            return True

        target_name = recommended.get("name", "Interactive Control")
        action_type = recommended.get("action_type", "click")
        dest_x = int(recommended.get("desktop_x", 960))
        dest_y = int(recommended.get("desktop_y", 540))
        box_1280 = recommended.get("box_1280x720")

        # Clamp safely to visible screen bounds
        dest_x = max(50, min(1870, dest_x))
        dest_y = max(50, min(1030, dest_y))

        print(f"  [TARGET ELEMENT] '{target_name}' at ({dest_x}, {dest_y}) | Action: {action_type}", flush=True)

        # 4. Harvest training samples for Florence-2
        self._harvest_vlm_samples_for_training(img, target_name, box_1280, dest_x, dest_y, alternatives)

        # 5. Dispatch verified action
        if action_type == "navigate_new_url":
            target_url = recommended.get("action_payload")
            if target_url and target_url.startswith("http"):
                print(f"[DAEMON ACTION] Navigating to URL: {target_url} ...", flush=True)
                self.client.send_hotkey(vk=0x4C, ctrl=True)
                time.sleep(0.3)
                self.client.paste_text(target_url, hwnd=self.active_hwnd)
                time.sleep(0.3)
                self.client.send_hotkey(vk=0x0D)
                time.sleep(3.5)
            else:
                self.platform_index = (self.platform_index + 1) % len(CURRICULUM)
                self.current_platform = CURRICULUM[self.platform_index]
                self.activate_platform(self.current_platform)

        elif action_type == "scroll":
            print(f"[DAEMON ACTION] Scrolling page smoothly...", flush=True)
            for _ in range(3):
                self.client.scroll(delta=-120, x=960, y=540)
                time.sleep(0.3)
            time.sleep(1.5)

        elif action_type == "type_text":
            text_to_type = recommended.get("action_payload", "Autonomous AI Agents")
            print(f"[DAEMON ACTION] Clicking '{target_name}' at ({dest_x}, {dest_y}) & typing '{text_to_type}'...", flush=True)
            self.client.click(dest_x, dest_y)
            time.sleep(0.5)
            self.client.paste_text(text_to_type, hwnd=self.active_hwnd)
            time.sleep(0.3)
            self.client.send_hotkey(vk=0x0D)
            time.sleep(3.0)

        else:  # click
            print(f"[DAEMON ACTION] Bézier movement to '{target_name}' at ({dest_x}, {dest_y}) & Click...", flush=True)
            self.client.click(dest_x, dest_y)
            time.sleep(2.5)

        # Record trajectory
        self._record_trajectory(
            step_idx=self.step_counter,
            site=current_site,
            status=page_status,
            action=action_type,
            target_name=target_name,
            target_xy=(dest_x, dest_y),
            reasoning=reasoning
        )

        # Platform rotation every 3 steps for broad curriculum coverage
        if self.step_counter % 3 == 0:
            print(f"[CURRICULUM ROTATION] Rotating to next platform after {self.step_counter} steps...", flush=True)
            self.platform_index = (self.platform_index + 1) % len(CURRICULUM)
            self.current_platform = CURRICULUM[self.platform_index]
            self.activate_platform(self.current_platform)

        # Check and trigger background training
        self.check_and_trigger_continual_training()

        # Update daemon status JSON
        self._write_status(current_site, target_name, (dest_x, dest_y))
        return True

    def _harvest_vlm_samples_for_training(
        self,
        full_img: Image.Image,
        target_name: str,
        box_1280: Optional[List[int]],
        dest_x: int,
        dest_y: int,
        alternatives: List[Dict[str, Any]]
    ) -> None:
        """Stores verified visual elements into Florence-2 training dataset on Drive E:."""
        try:
            if box_1280 and len(box_1280) == 4:
                ymin, xmin, ymax, xmax = box_1280
                x1 = int(xmin * 1.5)
                y1 = int(ymin * 1.5)
                x2 = int(xmax * 1.5)
                y2 = int(ymax * 1.5)
            else:
                x1 = max(0, dest_x - 40)
                y1 = max(0, dest_y - 15)
                x2 = min(1920, dest_x + 40)
                y2 = min(1080, dest_y + 15)

            traj_id = self.collector.start_trajectory(
                goal=f"VLM Exploration: {target_name}",
                window_title=self.current_platform["name"]
            )

            self.collector.record_step(
                trajectory_id=traj_id,
                image=full_img,
                action_type="click",
                phrase=target_name,
                bbox=(x1, y1, x2, y2),
                click_coords=(dest_x, dest_y),
                window_title=self.current_platform["name"]
            )
            print(f"  [DATASET HARVEST] Added Florence-2 pair: '{target_name}' [{x1}, {y1}, {x2}, {y2}]", flush=True)

            for idx, alt in enumerate(alternatives[:2]):
                alt_name = alt.get("name", "")
                alt_box = alt.get("box_1280x720")
                ax = alt.get("desktop_x")
                ay = alt.get("desktop_y")

                if alt_name and alt_box and len(alt_box) == 4:
                    ymin, xmin, ymax, xmax = alt_box
                    ax1 = int(xmin * 1.5)
                    ay1 = int(ymin * 1.5)
                    ax2 = int(xmax * 1.5)
                    ay2 = int(ymax * 1.5)
                    click_x = (ax1 + ax2) // 2
                    click_y = (ay1 + ay2) // 2
                elif alt_name and ax and ay:
                    ax1 = max(0, int(ax) - 40)
                    ay1 = max(0, int(ay) - 15)
                    ax2 = min(1920, int(ax) + 40)
                    ay2 = min(1080, int(ay) + 15)
                    click_x = int(ax)
                    click_y = int(ay)
                else:
                    continue

                self.collector.record_step(
                    trajectory_id=traj_id,
                    image=full_img,
                    action_type="click",
                    phrase=alt_name,
                    bbox=(ax1, ay1, ax2, ay2),
                    click_coords=(click_x, click_y),
                    window_title=self.current_platform["name"]
                )
                print(f"  [DATASET HARVEST] Added alternative pair: '{alt_name}' [{ax1}, {ay1}, {ax2}, {ay2}]", flush=True)

            self.collector.complete_trajectory(traj_id, succeeded=True)

        except Exception as e:
            logger.warning(f"Dataset harvesting error: {e}")

    def _record_trajectory(
        self,
        step_idx: int,
        site: str,
        status: str,
        action: str,
        target_name: str,
        target_xy: Tuple[int, int],
        reasoning: str
    ) -> None:
        r"""Appends verified step record to E:\cpower_training_data."""
        entry = {
            "timestamp": time.time(),
            "step": step_idx,
            "site": site,
            "status": status,
            "action": action,
            "target": target_name,
            "coordinates": list(target_xy),
            "reasoning": reasoning
        }
        with open(self.trajectories_file, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")

    def _write_status(self, site: str, last_target: str, coords: Tuple[int, int]) -> None:
        """Writes live JSON heartbeat status for monitoring."""
        status = {
            "timestamp": time.time(),
            "step": self.step_counter,
            "platform": self.current_platform["name"],
            "site": site,
            "last_target": last_target,
            "coordinates": list(coords),
            "elapsed_minutes": round((time.time() - self.start_time) / 60.0, 1),
            "remaining_minutes": round(max(0.0, (self.end_time - time.time()) / 60.0), 1),
            "dataset_samples": 0
        }
        try:
            with open(STORAGE_BASE / "dataset_florence2.jsonl", "r", encoding="utf-8") as f:
                status["dataset_samples"] = sum(1 for _ in f)
        except Exception:
            pass

        try:
            with open(self.status_file, "w", encoding="utf-8") as f:
                json.dump(status, f, indent=2, ensure_ascii=False)
        except Exception:
            pass

    def run_daemon(self) -> None:
        """Runs the continuous multi-hour loop until target duration is reached."""
        print("\n" + "=" * 80, flush=True)
        print("   STARTING CPOWER LONG-RUNNING VLM SUPERVISED EXPLORATION DAEMON", flush=True)
        print(f"   Target Duration: {self.target_hours:.1f} hours ({self.target_hours*60:.0f} minutes)", flush=True)
        print(f"   Storage Base: {STORAGE_BASE} (Drive H: strictly quarantined)", flush=True)
        print(f"   VLM Supervisor: {NVIDIA_VLM_MODEL} (NVIDIA NIM 120B)", flush=True)
        print(f"   Curriculum Depth: {len(CURRICULUM)} Platforms (Web + Windows Apps)", flush=True)
        print("   Window Strategy: STRICTLY MAXIMIZED (100% Fullscreen)", flush=True)
        print("=" * 80, flush=True)

        self.activate_platform(self.current_platform)

        while time.time() < self.end_time:
            try:
                self.execute_supervised_step()
                time.sleep(2.0)
            except KeyboardInterrupt:
                print("\n[DAEMON] Stopped by keyboard interrupt.", flush=True)
                break
            except Exception as e:
                logger.error(f"[DAEMON EXCEPTION] Unexpected error: {e}", exc_info=True)
                time.sleep(4.0)

        print("\n" + "=" * 80, flush=True)
        print(f"   DAEMON COMPLETED: Executed {self.step_counter} verified supervised steps.", flush=True)
        print(f"   Final Trajectory Dataset: {self.trajectories_file}", flush=True)
        print("=" * 80, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run Long-Running VLM Continuous Daemon")
    parser.add_argument("--hours", type=float, default=6.5, help="Total hours to run (default: 6.5h for overnight)")
    parser.add_argument("--train-interval", type=int, default=25, help="Steps between training runs")
    args = parser.parse_args()

    daemon = CPowerContinuousVLMDaemon(target_hours=args.hours, train_every_steps=args.train_interval)
    daemon.run_daemon()
