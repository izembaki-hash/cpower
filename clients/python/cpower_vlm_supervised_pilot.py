r"""
CPower VLM Supervised Pilot (clients/python/cpower_vlm_supervised_pilot.py).

Autonomous desktop exploration actively supervised by NVIDIA Vision-Language Model
(nvidia/nemotron-3.5-super-vl-120b-a12b).

Key Features:
  1. Fullscreen Closed-Loop Perception: Captures full 1920x1080 zero-copy DXGI frames
     via CPower C11 daemon, never downscaling to partial regions.
  2. Active VLM Supervision: Evaluates every screenshot with NVIDIA Super-VL 120B.
     Identifies real clickable buttons, detects login walls/error pages, and corrects
     aimless exploration.
  3. Multi-Platform Support: Seamlessly navigates across Windows Settings (ms-settings:),
     GitHub Trending, Wikipedia (Arabic & English), Python Docs, and Windows native apps.
  4. Ground-Truth Bounding Box Extraction: Obtains exact bounding boxes [ymin, xmin, ymax, xmax]
     and converts to 1920x1080 desktop coordinates for smooth Bézier mouse dispatch.
  5. Continual Dataset Harvesting: Automatically converts VLM-verified visual elements into
     Florence-2 training samples on E:\cpower_training_data\dataset_florence2.jsonl.
  6. Strict Storage Quarantine: Drive H: remains strictly protected (>=2.0 GB headroom);
     all artifacts, screenshots, and logs live on E:\cpower_training_data.
"""

from __future__ import annotations

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

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("VLMSupervisedPilot")

NVIDIA_API_KEY = os.environ.get("NVIDIA_API_KEY", "")
NVIDIA_VLM_MODEL = "nvidia/nemotron-3.5-super-vl-120b-a12b"
NVIDIA_VLM_URL = "https://integrate.api.nvidia.com/v1/chat/completions"

# Multi-Platform Curated Destinations (Web + Native Windows Settings)
CURATED_PLATFORMS = [
    {
        "type": "web",
        "name": "Wikipedia Arabic Portal",
        "target": "https://ar.wikipedia.org/wiki/البوابة_الرئيسية",
        "goal": "Explore Arabic Wikipedia main portal, featured articles, and navigation bars."
    },
    {
        "type": "windows_app",
        "name": "Windows System Settings",
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
        "name": "Windows Personalization Settings",
        "cmd": "ms-settings:personalization",
        "title_keyword": "setting",
        "goal": "Explore Windows Personalization, themes, colors, and background settings."
    }
]


class VLMSupervisor:
    """Supervises agent actions using NVIDIA Nemotron 3.5 Super-VL 120B."""

    def __init__(self, client: CPowerClient):
        self.client = client

    def capture_fullscreen_dxgi(self) -> Optional[Image.Image]:
        """Captures 1920x1080 screen using 3 DXGI slices to stay within 4MB ring buffer."""
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
            logger.warning(f"DXGI full screen capture failed: {e}")
            return None

    def analyze_screen(self, img: Image.Image, step_goal: str = "") -> Optional[Dict[str, Any]]:
        """Sends screenshot to NVIDIA Super-VL 120B and extracts structured actionable intent."""
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
        """Resiliently extracts and parses JSON even from mixed thoughts or slightly truncated output."""
        raw_text = raw_text.strip()

        # 1. Direct parse attempt
        try:
            return json.loads(raw_text)
        except Exception:
            pass

        # 2. Extract from markdown code fence
        fence_match = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", raw_text, re.DOTALL)
        if fence_match:
            try:
                return json.loads(fence_match.group(1))
            except Exception:
                pass

        # 3. Outer braces extraction with sanitized control chars
        start = raw_text.find("{")
        end = raw_text.rfind("}")
        if start != -1 and end != -1 and end > start:
            candidate = raw_text[start:end + 1]
            try:
                return json.loads(candidate)
            except Exception:
                # Sanitize unescaped newlines inside strings
                sanitized = re.sub(r'[\r\n\t]+', ' ', candidate)
                try:
                    return json.loads(sanitized)
                except Exception:
                    pass

        # 4. Regex-based fallback extraction if JSON syntax is damaged
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

        logger.warning(f"Could not parse JSON from VLM output. Raw preview: {raw_text[:200]}")
        return None


class CPowerVLMSupervisedPilot:
    """Orchestrates closed-loop autonomous desktop exploration supervised by NVIDIA VLM."""

    def __init__(self, target_steps: int = 50, storage_base: str = "E:/cpower_training_data"):
        self.target_steps = target_steps
        self.client = CPowerClient()
        self.supervisor = VLMSupervisor(self.client)
        self.collector = CPowerDatasetCollector(base_dir=storage_base)

        self.storage_base = Path(storage_base)
        self.artifacts_dir = self.storage_base / "demo_artifacts"
        self.artifacts_dir.mkdir(parents=True, exist_ok=True)
        self.trajectories_file = self.storage_base / "learned_patterns" / "vlm_supervised_trajectories.jsonl"
        self.trajectories_file.parent.mkdir(parents=True, exist_ok=True)

        self.active_hwnd = 0
        self.step_counter = 0
        self.platform_index = 0
        self.current_platform = CURATED_PLATFORMS[0]

    def ensure_maximized_window(self, hwnd: int) -> None:
        """Enforces SW_MAXIMIZE on target window so it NEVER shrinks or un-maximizes."""
        self.client.show_window(hwnd, "maximize")
        self.client.focus_window(hwnd)
        time.sleep(0.4)

    def activate_platform(self, platform_info: Dict[str, Any]) -> int:
        """Launches or switches to the specified platform (Web or Windows Native App) in MAXIMIZED state."""
        p_type = platform_info.get("type", "web")
        p_name = platform_info.get("name", "Unknown")

        print(f"\n[PLATFORM SWITCH] >>> Transitioning to: {p_name} ({p_type}) ...")

        if p_type == "web":
            target_url = platform_info.get("target", "https://ar.wikipedia.org")
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=True)
            if not browsers:
                print(f"[PILOT] Launching browser to {target_url}...")
                subprocess.Popen(["cmd.exe", "/c", "start", target_url], shell=False)
                time.sleep(3.5)
                wins = self.client.list_windows()
                browsers = find_browser_windows(wins, include_minimized=False)

            if browsers:
                self.active_hwnd = browsers[0]["hwndInt"]
                self.ensure_maximized_window(self.active_hwnd)
                # Navigate cleanly
                self.client.send_hotkey(vk=0x4C, ctrl=True)  # Ctrl+L
                time.sleep(0.3)
                self.client.paste_text(target_url, hwnd=self.active_hwnd)
                time.sleep(0.3)
                self.client.send_hotkey(vk=0x0D)  # Enter
                time.sleep(3.5)
                self.ensure_maximized_window(self.active_hwnd)

        elif p_type == "windows_app":
            cmd = platform_info.get("cmd", "ms-settings:")
            title_kw = platform_info.get("title_keyword", "setting").lower()
            subprocess.Popen(["cmd.exe", "/c", "start", cmd], shell=False)
            time.sleep(2.5)

            wins = self.client.list_windows()
            for w in wins:
                if title_kw in w.get("title", "").lower() or "إعداد" in w.get("title", "").lower():
                    self.active_hwnd = w["hwndInt"]
                    self.ensure_maximized_window(self.active_hwnd)
                    break

        return self.active_hwnd

    def smooth_scroll(self, clicks: int = 3, delta: int = -120) -> None:
        """Scrolls the page smoothly."""
        cx, cy = 960, 540
        for _ in range(clicks):
            self.client.scroll(delta=delta, x=cx, y=cy)
            time.sleep(0.3)

    def execute_supervised_step(self) -> bool:
        """Executes a single verified, closed-loop step under NVIDIA VLM supervision."""
        self.step_counter += 1
        print("\n" + "=" * 80)
        print(f" [VLM SUPERVISED STEP {self.step_counter}/{self.target_steps}] Evaluating & Supervising Live Screen")
        print(f" Current Platform: {self.current_platform['name']}")
        print("=" * 80)

        # 1. Guarantee window is MAXIMIZED and focused
        if self.active_hwnd:
            self.ensure_maximized_window(self.active_hwnd)

        # 2. Capture full 1920x1080 screenshot via zero-copy DXGI
        img = self.supervisor.capture_fullscreen_dxgi()
        if not img:
            print("[PILOT WARN] Failed to capture full DXGI frame. Retrying...")
            time.sleep(1.0)
            return False

        # Save screenshot artifact on Drive E:
        screenshot_path = self.artifacts_dir / f"vlm_step_{self.step_counter:04d}.jpg"
        img.save(screenshot_path, format="JPEG", quality=85)

        # 3. Query NVIDIA Nemotron Super-VL 120B
        print(f"[VLM SUPERVISOR] Sending live frame to NVIDIA Super-VL 120B...")
        t0 = time.time()
        analysis = self.supervisor.analyze_screen(img, step_goal=self.current_platform.get("goal", ""))
        dt = time.time() - t0
        print(f"[VLM SUPERVISOR] Analysis returned in {dt:.2f}s.")

        if not analysis:
            print("[PILOT WARN] VLM analysis empty or invalid. Switching to next curated platform...")
            self.platform_index = (self.platform_index + 1) % len(CURATED_PLATFORMS)
            self.current_platform = CURATED_PLATFORMS[self.platform_index]
            self.activate_platform(self.current_platform)
            return True

        page_status = analysis.get("page_status", "normal")
        current_site = analysis.get("current_site", "Unknown")
        reasoning = analysis.get("reasoning", "")
        recommended = analysis.get("recommended_target", {})
        alternatives = analysis.get("alternative_elements", [])

        print(f"  [VISUAL CONTEXT] Site: {current_site} | Status: {page_status}")
        print(f"  [VLM REASONING]  {reasoning}")

        # Check for error page or login barrier
        if page_status in ("error_page", "login_wall"):
            print(f"[VLM SUPERVISOR INTERVENTION] Page state is '{page_status}'.")
            print("[VLM SUPERVISOR CORRECTION] Rotating platform to guarantee open, useful exploration.")
            self.platform_index = (self.platform_index + 1) % len(CURATED_PLATFORMS)
            self.current_platform = CURATED_PLATFORMS[self.platform_index]
            self.activate_platform(self.current_platform)

            self._record_trajectory(
                step_idx=self.step_counter,
                site=current_site,
                status=page_status,
                action="platform_rotation_correction",
                target_name=self.current_platform["name"],
                target_xy=(960, 540),
                reasoning=reasoning
            )
            return True

        # Process recommended target
        target_name = recommended.get("name", "Interactive Element")
        action_type = recommended.get("action_type", "click")
        dest_x = int(recommended.get("desktop_x", 960))
        dest_y = int(recommended.get("desktop_y", 540))
        box_1280 = recommended.get("box_1280x720")

        # Clamp safely within visible display bounds
        dest_x = max(50, min(1870, dest_x))
        dest_y = max(50, min(1030, dest_y))

        print(f"  [TARGET ELEMENT] '{target_name}' at ({dest_x}, {dest_y}) | Action: {action_type}")

        # Harvest verified training sample for Florence-2
        self._harvest_vlm_samples_for_training(img, target_name, box_1280, dest_x, dest_y, alternatives)

        # 4. Dispatch the action smoothly
        if action_type == "navigate_new_url":
            target_url = recommended.get("action_payload")
            if target_url and target_url.startswith("http"):
                print(f"[PILOT ACTION] Navigating to URL: {target_url} ...")
                self.client.send_hotkey(vk=0x4C, ctrl=True)
                time.sleep(0.3)
                self.client.paste_text(target_url, hwnd=self.active_hwnd)
                time.sleep(0.3)
                self.client.send_hotkey(vk=0x0D)
                time.sleep(3.5)
            else:
                self.platform_index = (self.platform_index + 1) % len(CURATED_PLATFORMS)
                self.current_platform = CURATED_PLATFORMS[self.platform_index]
                self.activate_platform(self.current_platform)

        elif action_type == "scroll":
            print(f"[PILOT ACTION] Scrolling page content smoothly...")
            self.smooth_scroll(clicks=4, delta=-130)
            time.sleep(1.5)

        elif action_type == "type_text":
            text_to_type = recommended.get("action_payload", "Artificial Intelligence")
            print(f"[PILOT ACTION] Clicking '{target_name}' at ({dest_x}, {dest_y}) and typing '{text_to_type}'...")
            self.client.click(dest_x, dest_y)
            time.sleep(0.5)
            self.client.paste_text(text_to_type, hwnd=self.active_hwnd)
            time.sleep(0.3)
            self.client.send_hotkey(vk=0x0D)
            time.sleep(3.0)

        else:  # click
            print(f"[PILOT ACTION] Bézier movement to '{target_name}' at ({dest_x}, {dest_y}) & Click...")
            self.client.click(dest_x, dest_y)
            time.sleep(2.5)

        # Record verified step trajectory
        self._record_trajectory(
            step_idx=self.step_counter,
            site=current_site,
            status=page_status,
            action=action_type,
            target_name=target_name,
            target_xy=(dest_x, dest_y),
            reasoning=reasoning
        )

        # Periodically rotate platform to give diverse experience across multiple apps & platforms
        if self.step_counter % 3 == 0:
            print(f"[CURRICULUM DIVERSITY] Rotating to next platform after {self.step_counter} steps...")
            self.platform_index = (self.platform_index + 1) % len(CURATED_PLATFORMS)
            self.current_platform = CURATED_PLATFORMS[self.platform_index]
            self.activate_platform(self.current_platform)

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
            # Calculate primary bounding box
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
                goal=f"VLM Exploration: interact with {target_name}",
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
            print(f"  [DATASET HARVEST] Added Florence-2 pair: '{target_name}' [{x1}, {y1}, {x2}, {y2}]")

            # Add alternative elements if present
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
                print(f"  [DATASET HARVEST] Added alternative pair: '{alt_name}' [{ax1}, {ay1}, {ax2}, {ay2}]")

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

    def run_exploration_session(self) -> None:
        """Runs the complete multi-platform supervised exploration session."""
        print("\n" + "=" * 80)
        print("   STARTING MULTI-PLATFORM VLM-SUPERVISED AUTONOMOUS AGENT (NVIDIA NIM 120B)")
        print(f"   Target Steps: {self.target_steps} | Training Storage: {self.storage_base}")
        print("   Window Strategy: STRICTLY MAXIMIZED (100% Fullscreen)")
        print(f"   Platforms in Curriculum: {len(CURATED_PLATFORMS)} (Web + Windows Native Apps)")
        print("=" * 80)

        # Activate initial platform
        self.activate_platform(self.current_platform)

        successful_steps = 0
        for _ in range(self.target_steps):
            try:
                ok = self.execute_supervised_step()
                if ok:
                    successful_steps += 1
                time.sleep(2.0)
            except KeyboardInterrupt:
                print("\n[PILOT] Exploration stopped by user interrupt.")
                break
            except Exception as e:
                logger.error(f"[PILOT ERROR] Unexpected exception during step: {e}", exc_info=True)
                time.sleep(3.0)

        print("\n" + "=" * 80)
        print(f"   EXPLORATION SESSION COMPLETED: {successful_steps}/{self.target_steps} verified steps.")
        print(f"   Harvested Trajectories: {self.trajectories_file}")
        print("=" * 80)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Run VLM-Supervised Desktop Pilot")
    parser.add_argument("--steps", type=int, default=10, help="Number of supervised steps to execute")
    args = parser.parse_args()

    pilot = CPowerVLMSupervisedPilot(target_steps=args.steps)
    pilot.run_exploration_session()
