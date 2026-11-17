r"""
CPower Smart Shadow Learner (clients/python/cpower_smart_shadow.py).

Converts raw desktop human interactions into abstract, reusable Behavioral Patterns:
  - Observes active window context, control types, and user gestures.
  - Distills raw clicks/keystrokes into Semantic Pattern Episodes:
      * SearchPattern: Input focus -> text injection -> submission.
      * NavigationPattern: Menu open -> item selection.
      * FormFillingPattern: Sequential multi-field entries.
      * GameControlPattern: Rapid WASD / hotkey / mouse delta streams.
  - Appends to E:\cpower_training_data\learned_patterns\user_behavioral_patterns.jsonl.
  - Generates training pairs for offline model fine-tuning.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
import logging
from pathlib import Path
import sys
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient
from cpower_process_guard import get_process_name_by_pid, is_protected_window

logger = logging.getLogger("CPowerSmartShadow")


@dataclass
class AtomicAction:
    action_type: str  # "click", "type", "hotkey", "scroll", "focus"
    timestamp: float
    window_title: str
    process_name: str
    hwnd: int
    coords: Tuple[int, int] = (0, 0)
    text_content: Optional[str] = None
    key_code: Optional[int] = None
    element_role: Optional[str] = None
    element_name: Optional[str] = None


@dataclass
class PatternEpisode:
    pattern_type: str  # "search", "navigation", "form_fill", "game_control", "general"
    start_time: float
    end_time: float
    window_title: str
    process_name: str
    actions: List[Dict[str, Any]] = field(default_factory=list)
    semantic_summary: str = ""


class SmartShadowLearner:
    """Passively observes user desktop actions and distills them into pattern episodes."""

    def __init__(
        self,
        client: Optional[CPowerClient] = None,
        output_dir: Optional[Path] = None,
        episode_idle_threshold: float = 3.5,
    ):
        self.client = client or CPowerClient()
        self.output_dir = output_dir or Path("E:/cpower_training_data/learned_patterns")
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.episodes_file = self.output_dir / "user_behavioral_patterns.jsonl"
        self.episode_idle_threshold = episode_idle_threshold

        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._current_actions: List[AtomicAction] = []
        self._last_action_time = time.time()
        self._lock = threading.Lock()

    def record_action(self, action: AtomicAction) -> None:
        """Adds an observed user action and checks for episode completion."""
        with self._lock:
            now = time.time()
            if self._current_actions and (now - self._last_action_time > self.episode_idle_threshold):
                self._flush_current_episode()

            self._current_actions.append(action)
            self._last_action_time = now

    def _flush_current_episode(self) -> Optional[PatternEpisode]:
        """Classifies accumulated actions into a pattern and commits to disk."""
        if not self._current_actions:
            return None

        actions = list(self._current_actions)
        self._current_actions.clear()

        # Classify the episode into one of the 5 Universal Patterns
        pattern_type = self._classify_episode(actions)
        summary = self._summarize_episode(pattern_type, actions)

        episode = PatternEpisode(
            pattern_type=pattern_type,
            start_time=actions[0].timestamp,
            end_time=actions[-1].timestamp,
            window_title=actions[0].window_title,
            process_name=actions[0].process_name,
            actions=[asdict(a) for a in actions],
            semantic_summary=summary,
        )

        # Write to JSONL
        with open(self.episodes_file, "a", encoding="utf-8") as f:
            f.write(json.dumps(asdict(episode), ensure_ascii=False) + "\n")

        logger.info(f"Learned Episode '{pattern_type}' ({len(actions)} actions): {summary}")
        return episode

    def _classify_episode(self, actions: List[AtomicAction]) -> str:
        """Infers the high-level pattern from the action sequence."""
        types = [a.action_type for a in actions]
        texts = [a.text_content for a in actions if a.text_content]

        # Gaming detection: rapid movement / WASD / key repeat in non-editor apps
        pname = actions[0].process_name.lower()
        if any(g in pname for g in ["game", "steam", "unreal", "unity", "roblox", "minecraft"]):
            return "game_control"

        # Search Pattern: text entered followed by Enter hotkey
        if "type" in types and any(a.action_type == "hotkey" and a.key_code == 0x0D for a in actions):
            return "search"

        # Form Filling: multiple type actions across distinct fields
        if types.count("type") >= 2:
            return "form_fill"

        # Navigation: menu/button click followed by selection
        if types.count("click") >= 2 and not texts:
            return "navigation"

        return "general_interaction"

    def _summarize_episode(self, ptype: str, actions: List[AtomicAction]) -> str:
        win = actions[0].window_title
        proc = actions[0].process_name
        if ptype == "search":
            query = " ".join([a.text_content for a in actions if a.text_content])
            return f"Searched for '{query}' in '{win}' ({proc})"
        elif ptype == "form_fill":
            return f"Filled form with {len([a for a in actions if a.text_content])} entries in '{win}'"
        elif ptype == "navigation":
            return f"Navigated through {len(actions)} UI elements in '{win}'"
        elif ptype == "game_control":
            return f"Executed real-time game inputs in '{proc}'"
        return f"Interacted with {len(actions)} controls in '{win}'"

    def flush(self) -> None:
        """Explicitly flushes any pending actions."""
        with self._lock:
            self._flush_current_episode()
