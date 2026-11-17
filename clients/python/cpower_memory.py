"""
CPower Episodic Memory & Trajectory Learner (clients/python/cpower_memory.py).

Provides persistent learned rules, few-shot training trajectories, and
experience replay for CPower Computer-Use agents.
"""

from __future__ import annotations

import json
from pathlib import Path
import time
from typing import Any, Dict, List, Optional


DEFAULT_LEARNED_KNOWLEDGE: Dict[str, Any] = {
    "version": "1.0",
    "negative_rules": [
        "NEVER target or click inside protected windows (Antigravity.exe, ZCode.exe, VS Code, terminals).",
        "NEVER click main window title bar buttons (Minimize, Maximize, Close at the top edge of screen).",
        "NEVER click generic UI links like 'Sign In', 'Log In', 'Join', 'Share', 'Report', 'Terms', 'Privacy'.",
    ],
    "positive_heuristics": [
        "To navigate to a new site, focus address bar via Ctrl+L, safe paste URL via Unicode clipboard, press Enter.",
        "To dismiss cookie banners, look for buttons labeled 'Accept all', 'Reject all', 'Dismiss', 'I agree'.",
        "To read an article/post, click on the prominent headline hyperlink (text length > 20 chars).",
        "To read discussions, perform smooth mouse wheel scroll down (delta -300), wait 0.8s, and collect text.",
        "To return to the main feed, press Alt+Left Arrow (Back) or re-navigate to the feed URL.",
    ],
    "trajectories": [],
}


class EpisodicMemory:
    """Persistent learned memory buffer for CPower agents."""

    def __init__(self, storage_path: Optional[Path] = None):
        self.storage_path = storage_path or (Path.home() / ".cpower_memory.json")
        self.data: Dict[str, Any] = self._load()

    def _load(self) -> Dict[str, Any]:
        if self.storage_path.is_file():
            try:
                content = json.loads(self.storage_path.read_text(encoding="utf-8"))
                return content
            except Exception:
                pass
        return dict(DEFAULT_LEARNED_KNOWLEDGE)

    def save(self) -> None:
        try:
            self.storage_path.write_text(json.dumps(self.data, indent=2, ensure_ascii=False), encoding="utf-8")
        except Exception as e:
            print(f"[MEMORY] Warning: Could not save memory: {e}")

    def add_trajectory(self, goal: str, steps: List[Dict[str, Any]], succeeded: bool) -> None:
        """Records an execution trajectory for future reference."""
        record = {
            "goal": goal,
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "succeeded": succeeded,
            "step_count": len(steps),
            "steps": steps,
        }
        self.data.setdefault("trajectories", []).append(record)
        # Keep last 50 trajectories
        if len(self.data["trajectories"]) > 50:
            self.data["trajectories"] = self.data["trajectories"][-50:]
        self.save()

    def get_prompt_guidance(self) -> str:
        """Returns the learned rules formatted as guidance for AI decision-making."""
        rules = "\n".join(f"- {r}" for r in self.data.get("negative_rules", []))
        heuristics = "\n".join(f"- {h}" for h in self.data.get("positive_heuristics", []))
        return f"CRITICAL CONSTRAINTS (Learned from experience):\n{rules}\n\nEFFECTIVE PATTERNS:\n{heuristics}"
