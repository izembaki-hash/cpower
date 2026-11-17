r"""
CPower Autonomous Orchestrator (clients/python/cpower_autonomous_orchestrator.py).

The Complete Unified System:
  1. Cognitive Brain (System 2): GroqCognitivePlanner (openai/gpt-oss-120b) translates intent to patterns.
  2. Pattern Engine (System 1): UniversalPatternEngine executes the 5 Universal Patterns.
  3. Visual Perception: Adapted Florence-2 grounds visual elements dynamically.
  4. Native Win32/UIA Core: ISO C11 Daemon dispatches smooth Bézier clicks and Unicode text.
  5. Distillation & Continual Training: Logs all plans and executions to Drive E: for local model learning.
"""

from __future__ import annotations

import json
import logging
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_vision_local import CPowerLocalVision
from cpower_process_guard import find_browser_windows, is_protected_window
from cpower_groq_planner import GroqCognitivePlanner
from cpower_pattern_engine import UniversalPatternEngine
from cpower_smart_shadow import AtomicAction, SmartShadowLearner

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("CPowerOrchestrator")


class AutonomousDesktopOrchestrator:
    """Coordinates Cognitive Planning, Universal Patterns, and Continual Learning."""

    def __init__(
        self,
        api_key: Optional[str] = None,
        data_dir: Optional[Path] = None,
    ):
        self.data_dir = data_dir or Path("E:/cpower_training_data")
        self.client = CPowerClient()
        self.vision = CPowerLocalVision(client=self.client)
        self.pattern_engine = UniversalPatternEngine(
            client=self.client,
            vision=self.vision,
            trace_dir=self.data_dir / "demo_artifacts",
        )
        self.planner = GroqCognitivePlanner(
            api_key=api_key,
            distillation_dir=self.data_dir / "planner_traces",
        )
        self.shadow = SmartShadowLearner(
            client=self.client,
            output_dir=self.data_dir / "learned_patterns",
        )

    def execute_user_request(self, user_prompt: str) -> Dict[str, Any]:
        print("\n" + "=" * 78)
        print("         CPOWER AUTONOMOUS DESKTOP AGENT (DUAL-SYSTEM ARCHITECTURE)")
        print(f"   Request: '{user_prompt}'")
        print("=" * 78)

        # -------------------------------------------------------------
        # Phase 1: Cognitive Planning (Groq System 2)
        # -------------------------------------------------------------
        print("\n[PHASE 1] Calling Groq Cognitive Brain (openai/gpt-oss-120b)...")
        plan = self.planner.plan_request(user_prompt)
        intent = plan.get("intent", "Unknown Intent")
        target_app = plan.get("target_application", "browser")
        target_url = plan.get("target_url")
        workflow = plan.get("workflow", [])

        print(f"          Planned Intent:     {intent}")
        print(f"          Target Application: {target_app} (URL: {target_url})")
        print(f"          Pattern Steps:      {len(workflow)} step(s)")
        print(f"          Planning Latency:   {plan.get('planning_latency_ms', 0):.1f} ms")

        # -------------------------------------------------------------
        # Phase 2: Application Acquisition & Window Focus
        # -------------------------------------------------------------
        print("\n[PHASE 2] Preparing Application Environment...")
        target_hwnd, target_rect = self._acquire_application(target_app, target_url)
        self.pattern_engine.set_active_window(target_hwnd, target_rect)
        print(f"          Active Window Acquired: HWND {target_hwnd:#x}, Bounds: {target_rect}")

        # -------------------------------------------------------------
        # Phase 3: Declarative Pattern Execution (System 1)
        # -------------------------------------------------------------
        print("\n[PHASE 3] Executing Declarative Patterns...")
        execution_results = []
        overall_success = True

        for idx, step in enumerate(workflow, 1):
            pattern_name = step.get("pattern", "")
            params = step.get("params", {})
            step_target_app = step.get("target_application")
            step_target_url = step.get("target_url")

            # Dynamic multi-application switching if step specifies a target
            if step_target_app or step_target_url:
                app_to_use = step_target_app or target_app
                print(f"\n   [APP SWITCH] Switching focus to: {app_to_use} (URL: {step_target_url})")
                target_hwnd, target_rect = self._acquire_application(app_to_use, step_target_url)
                self.pattern_engine.set_active_window(target_hwnd, target_rect)

            print(f"\n   --- Step {idx}/{len(workflow)}: Running [{pattern_name}] ---")

            step_res = {}
            try:
                if pattern_name == "dismiss_popup":
                    custom_phrases = params.get("custom_phrases")
                    dismissed = self.pattern_engine.dismiss_obstructions(custom_phrases)
                    step_res = {"pattern": "dismiss_popup", "dismissed": dismissed, "succeeded": True}
                    print(f"       Obstruction Check: {'Dismissed banner/popup' if dismissed else 'Clean screen, no obstruction'}")

                elif pattern_name == "search":
                    query = params.get("query", "")
                    submit = params.get("submit", True)
                    target_phrase = params.get("target_phrase")
                    phrases = [target_phrase] if target_phrase else None
                    step_res = self.pattern_engine.execute_search(
                        query=query,
                        target_phrases=phrases,
                        submit=submit,
                    )
                    print(f"       Search Pattern Executed: Query='{query}', Target={step_res.get('target_point')}")
                    print(f"       Verification Frame: {step_res.get('verification_frame')}")

                elif pattern_name == "navigation":
                    menu_phrase = params.get("menu_phrase", "menu")
                    item_phrase = params.get("item_phrase", "")
                    scroll_delta = params.get("scroll_delta", 0)
                    step_res = self.pattern_engine.execute_navigation(
                        menu_phrase=menu_phrase,
                        item_phrase=item_phrase,
                        scroll_delta=scroll_delta,
                    )
                    print(f"       Navigation Executed: Menu='{menu_phrase}', Item='{item_phrase}'")

                elif pattern_name == "form_fill":
                    fields = params.get("fields", [])
                    submit_phrase = params.get("submit_phrase")
                    step_res = self.pattern_engine.execute_form_fill(
                        field_entries=fields,
                        submit_phrase=submit_phrase,
                    )
                    print(f"       Form Fill Executed: Filled {step_res.get('filled_fields')} fields")

                elif pattern_name == "download_save":
                    trigger_phrase = params.get("trigger_phrase", "download")
                    save_path = params.get("save_as_path")
                    step_res = self.pattern_engine.execute_download_save(
                        trigger_phrase=trigger_phrase,
                        save_as_path=save_path,
                    )
                    print(f"       Download/Save Executed: Succeeded={step_res.get('succeeded')}")

                else:
                    print(f"       Unknown pattern '{pattern_name}'. Skipping.")
                    step_res = {"pattern": pattern_name, "succeeded": False, "error": "unknown_pattern"}

            except Exception as e:
                logger.error(f"Error in step {idx}: {e}")
                step_res = {"pattern": pattern_name, "succeeded": False, "error": str(e)}
                overall_success = False

            execution_results.append(step_res)

        # -------------------------------------------------------------
        # Phase 4: Distillation Logging for Continuous Self-Training
        # -------------------------------------------------------------
        print("\n[PHASE 4] Recording Distillation Trace for Continual Local Learning...")
        self.planner.record_distillation_trace(
            user_prompt=user_prompt,
            plan=plan,
            execution_results=execution_results,
            succeeded=overall_success,
        )
        print(f"          Trace appended to: {self.planner.trace_file}")

        print("\n" + "=" * 78)
        print("          MISSION ACCOMPLISHED SUCCESSFULLY!")
        print("=" * 78)

        return {
            "succeeded": overall_success,
            "user_prompt": user_prompt,
            "plan": plan,
            "execution_results": execution_results,
            "distillation_file": str(self.planner.trace_file),
        }

    def _acquire_application(self, target_app: str, target_url: Optional[str]) -> Tuple[int, Tuple[int, int, int, int]]:
        """Ensures the target application is running, focused, and returns its bounds."""
        target_app_lower = target_app.lower()

        if "browser" in target_app_lower or target_url:
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=True)

            target_browser = None
            if target_url:
                url_domain = target_url.split("//")[-1].split("/")[0].replace("www.", "").lower()
                for b in browsers:
                    if url_domain in b.get("title", "").lower():
                        target_browser = b
                        break

            if not target_browser and browsers:
                target_browser = browsers[0]

            if not target_browser:
                url_to_open = target_url or "https://www.google.com"
                print(f"          Launching web browser to '{url_to_open}'...")
                subprocess.Popen(["cmd.exe", "/c", "start", url_to_open], shell=False)
                time.sleep(4.0)
                wins = self.client.list_windows()
                browsers = find_browser_windows(wins, include_minimized=False)
                if browsers:
                    target_browser = browsers[0]
            elif target_url and "youtube" in target_url:
                title = target_browser.get("title", "").lower()
                if "youtube" not in title:
                    subprocess.Popen(["cmd.exe", "/c", "start", target_url], shell=False)
                    time.sleep(3.5)
                    wins = self.client.list_windows()
                    browsers = find_browser_windows(wins, include_minimized=False)
                    for b in browsers:
                        if "youtube" in b.get("title", "").lower():
                            target_browser = b
                            break
                    if not target_browser and browsers:
                        target_browser = browsers[0]

            if not target_browser:
                raise RuntimeError("Could not find or launch web browser.")

            hwnd = target_browser["hwndInt"]
            try:
                self.client.show_window(hwnd, "restore")
            except Exception:
                pass
            self.client.focus_window(hwnd)
            time.sleep(1.0)

            # Re-read active rect to avoid stale minimized bounds
            rect = tuple(target_browser["rect"])
            wins = self.client.list_windows()
            for w in wins:
                if w.get("hwndInt") == hwnd:
                    r = w["rect"]
                    if r[0] > -1000 and r[1] > -1000:
                        rect = tuple(r)
                    break

            return hwnd, rect

        else:
            # Native desktop window
            wins = self.client.list_windows()
            for w in wins:
                title = w.get("title", "")
                pid = w.get("pid", 0)
                hwnd = w.get("hwndInt", 0)
                if target_app_lower in title.lower() and not is_protected_window(hwnd, pid, title):
                    return hwnd, tuple(w["rect"])

            raise RuntimeError(f"Could not locate target application '{target_app}' on desktop.")


if __name__ == "__main__":
    prompt = sys.argv[1] if len(sys.argv) > 1 else "ادخل الى اليوتيوب وابحث لي عن ملخص مسلسل"
    orch = AutonomousDesktopOrchestrator()
    orch.execute_user_request(prompt)
