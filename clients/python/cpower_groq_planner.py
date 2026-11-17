r"""
CPower Groq Cognitive Planner & Distillation Engine (clients/python/cpower_groq_planner.py).

Provides the "Cognitive Brain" (System 2):
  1. Translates high-level natural language user goals into declarative Pattern Workflows.
  2. Powered by ultra-fast Groq LLM API (openai/gpt-oss-120b & qwen/qwen3.8-27b).
  3. Continuous Self-Distillation: Logs every reasoning trace and execution outcome
     to E:\cpower_training_data\planner_traces for local model fine-tuning.
"""

from __future__ import annotations

import json
import logging
import os
from pathlib import Path
import sys
import time
from typing import Any, Dict, List, Optional
import urllib.request
import urllib.error

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

logger = logging.getLogger("CPowerGroqPlanner")

DEFAULT_GROQ_KEY = os.environ.get("GROQ_API_KEY", "")
GROQ_API_URL = "https://api.groq.com/openai/v1/chat/completions"

SYSTEM_PLANNER_PROMPT = """You are the Cognitive Brain of an autonomous desktop computer-use agent.
Your job is to translate the user's natural language request (Arabic or English) into an executable declarative JSON Workflow composed of Universal Interaction Patterns.

The 5 available Universal Patterns:
1. "search": Locate input box, focus, paste query, and press Enter.
   Params: {"query": string, "target_phrase": string (optional), "submit": boolean}
2. "navigation": Open menu/drawer, scroll, select item.
   Params: {"menu_phrase": string, "item_phrase": string, "scroll_delta": int (optional)}
3. "form_fill": Sequence of form fields.
   Params: {"fields": [{"label": string, "value": string}], "submit_phrase": string (optional)}
4. "download_save": Trigger download and handle save dialog.
   Params: {"trigger_phrase": string, "save_as_path": string (optional)}
5. "dismiss_popup": Close any modal/cookie/banner obstruction.
   Params: {"custom_phrases": [string] (optional)}

Output Schema:
You MUST respond with a single JSON object (no markdown, no code block fences, just raw JSON) matching this schema:
{
  "intent": string (short summary of user intent),
  "target_application": string (e.g. "browser", "settings", "notepad"),
  "target_url": string or null (if target_application is browser, specify full URL like https://www.youtube.com),
  "workflow": [
    {
      "pattern": "search" | "navigation" | "form_fill" | "download_save" | "dismiss_popup",
      "target_application": string (optional, specify if switching app in this step),
      "target_url": string or null (optional, specify if navigating to URL in this step),
      "params": object
    }
  ]
}
"""


class GroqCognitivePlanner:
    """Plans high-level desktop tasks via Groq API and logs traces for local distillation."""

    def __init__(
        self,
        api_key: Optional[str] = None,
        model_name: str = "qwen/qwen3.8-27b",
        distillation_dir: Optional[Path] = None,
    ):
        self.api_key = api_key or os.environ.get("GROQ_API_KEY", DEFAULT_GROQ_KEY)
        self.model_name = model_name
        self.distillation_dir = distillation_dir or Path("E:/cpower_training_data/planner_traces")
        self.distillation_dir.mkdir(parents=True, exist_ok=True)
        self.trace_file = self.distillation_dir / "distillation_dataset.jsonl"

    def plan_request(self, user_prompt: str) -> Dict[str, Any]:
        """Queries Groq to generate a declarative Pattern Workflow for user_prompt."""
        start_t = time.time()
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
        }

        body = {
            "model": self.model_name,
            "messages": [
                {"role": "system", "content": SYSTEM_PLANNER_PROMPT},
                {"role": "user", "content": user_prompt},
            ],
            "temperature": 0.1,
            "max_tokens": 600,
        }

        req = urllib.request.Request(
            GROQ_API_URL,
            data=json.dumps(body).encode("utf-8"),
            headers=headers,
        )

        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                raw_resp = json.loads(resp.read().decode("utf-8"))
                content = raw_resp["choices"][0]["message"]["content"].strip()
                # Clean possible markdown fences
                if content.startswith("```"):
                    lines = content.splitlines()
                    if lines[0].startswith("```"):
                        lines = lines[1:]
                    if lines and lines[-1].startswith("```"):
                        lines = lines[:-1]
                    content = "\n".join(lines).strip()

                plan = json.loads(content)
                dur_ms = (time.time() - start_t) * 1000.0
                plan["planner_model"] = self.model_name
                plan["planning_latency_ms"] = dur_ms
                plan["raw_prompt"] = user_prompt
                return plan

        except Exception as e:
            logger.warning(f"Primary planner model {self.model_name} failed: {e}. Trying fallback...")
            # Fallback to qwen/qwen3.8-27b
            if self.model_name != "qwen/qwen3.8-27b":
                self.model_name = "qwen/qwen3.8-27b"
                return self.plan_request(user_prompt)
            raise RuntimeError(f"Groq Cognitive Planning failed: {e}")

    def record_distillation_trace(
        self,
        user_prompt: str,
        plan: Dict[str, Any],
        execution_results: List[Dict[str, Any]],
        succeeded: bool = True,
    ) -> None:
        """
        Records the (Prompt -> Reasoning Plan -> Execution Feedback) tuple
        into the Distillation Dataset on Drive E: for future local fine-tuning.
        """
        record = {
            "timestamp": time.time(),
            "prompt": user_prompt,
            "system_prompt": SYSTEM_PLANNER_PROMPT,
            "groq_plan": plan,
            "execution_feedback": execution_results,
            "succeeded": succeeded,
        }

        with open(self.trace_file, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
        logger.info(f"Distillation trace logged to {self.trace_file}")
