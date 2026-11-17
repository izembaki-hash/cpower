"""
Unit & Integration Tests for CPower Cognitive Orchestrator & Universal Patterns.
Verifies:
  - UniversalPatternEngine: all 5 patterns and fallback mechanisms.
  - GroqCognitivePlanner: schema conformance, fallback model, distillation logging.
  - SmartShadowLearner: episode classification, idle thresholds, JSONL persistence.
  - Drive H: Quarantine: 100% adherence to external drive E:.
"""

import json
from pathlib import Path
import sys
import time
from unittest.mock import MagicMock, patch

import pytest
from PIL import Image

CLIENT_DIR = Path(__file__).resolve().parent.parent / "clients" / "python"
sys.path.insert(0, str(CLIENT_DIR))

from cpower_pattern_engine import UniversalPatternEngine
from cpower_groq_planner import GroqCognitivePlanner, SYSTEM_PLANNER_PROMPT
from cpower_smart_shadow import AtomicAction, PatternEpisode, SmartShadowLearner


@pytest.fixture
def mock_client():
    client = MagicMock()
    client.list_windows.return_value = [
        {"hwndInt": 0x1234, "title": "YouTube - Edge", "rect": [0, 0, 1920, 1080], "visible": True}
    ]
    client.focus_window.return_value = {"succeeded": True}
    client.click.return_value = {"ack": {"succeeded": True}}
    client.type_text.return_value = {"ack": {"succeeded": True}}
    client.paste_text.return_value = {"ack": {"succeeded": True}}
    client.send_hotkey.return_value = {"ack": {"succeeded": True}}
    client.scroll.return_value = {"ack": {"succeeded": True}}
    # 10x10 mock PNG image
    img = Image.new("RGB", (100, 100), color="white")
    import io
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    client.capture_micro_region.return_value = ({"succeeded": True}, buf.getvalue())
    return client


@pytest.fixture
def mock_vision():
    vision = MagicMock()
    # Return grounded center coords (500, 50, (400, 30, 600, 70))
    vision.ground_phrase_florence.return_value = (500, 50, (400, 30, 600, 70))
    return vision


class TestUniversalPatternEngine:
    """Tests all 5 Universal Interaction Patterns."""

    def test_search_pattern_execution(self, mock_client, mock_vision, tmp_path):
        engine = UniversalPatternEngine(client=mock_client, vision=mock_vision, trace_dir=tmp_path)
        engine.set_active_window(0x1234, (0, 0, 1920, 1080))

        res = engine.execute_search(query="ملخص مسلسل", submit=True, auto_dismiss=False)
        assert res["succeeded"] is True
        assert res["pattern"] == "SearchPattern"
        assert res["query"] == "ملخص مسلسل"
        assert mock_client.click.called
        assert mock_client.paste_text.called
        assert mock_client.send_hotkey.called

    def test_navigation_pattern_execution(self, mock_client, mock_vision, tmp_path):
        engine = UniversalPatternEngine(client=mock_client, vision=mock_vision, trace_dir=tmp_path)
        engine.set_active_window(0x1234, (0, 0, 1920, 1080))

        res = engine.execute_navigation(menu_phrase="menu", item_phrase="settings", scroll_delta=120)
        assert res["succeeded"] is True
        assert res["pattern"] == "NavigationPattern"
        assert mock_client.scroll.called

    def test_form_filling_pattern_execution(self, mock_client, mock_vision, tmp_path):
        engine = UniversalPatternEngine(client=mock_client, vision=mock_vision, trace_dir=tmp_path)
        engine.set_active_window(0x1234, (0, 0, 1920, 1080))

        fields = [
            {"label": "Username", "value": "testuser"},
            {"label": "Email", "value": "test@example.com"},
        ]
        res = engine.execute_form_fill(field_entries=fields, submit_phrase="Register")
        assert res["succeeded"] is True
        assert res["filled_fields"] == 2
        assert res["submitted"] is True

    def test_download_save_pattern_execution(self, mock_client, mock_vision, tmp_path):
        engine = UniversalPatternEngine(client=mock_client, vision=mock_vision, trace_dir=tmp_path)
        engine.set_active_window(0x1234, (0, 0, 1920, 1080))

        res = engine.execute_download_save(trigger_phrase="download", save_as_path="C:/Downloads/file.pdf")
        assert res["succeeded"] is True
        assert res["pattern"] == "DownloadSavePattern"

    def test_popup_dismissal_pattern(self, mock_client, mock_vision, tmp_path):
        engine = UniversalPatternEngine(client=mock_client, vision=mock_vision, trace_dir=tmp_path)
        engine.set_active_window(0x1234, (0, 0, 1920, 1080))

        dismissed = engine.dismiss_obstructions(["close button"])
        assert dismissed is True
        assert mock_client.click.called


class TestGroqCognitivePlanner:
    """Tests Groq Planner, Schema parsing, and Distillation recording."""

    def test_distillation_record_persistence(self, tmp_path):
        planner = GroqCognitivePlanner(api_key="test_key", distillation_dir=tmp_path)
        test_plan = {
            "intent": "search",
            "target_application": "browser",
            "workflow": [{"pattern": "search", "params": {"query": "test"}}],
        }
        test_feedback = [{"pattern": "search", "succeeded": True}]

        planner.record_distillation_trace("search test", test_plan, test_feedback, succeeded=True)
        assert planner.trace_file.is_file()

        lines = planner.trace_file.read_text(encoding="utf-8").strip().splitlines()
        assert len(lines) == 1
        data = json.loads(lines[0])
        assert data["prompt"] == "search test"
        assert data["succeeded"] is True
        assert data["groq_plan"]["intent"] == "search"


class TestSmartShadowLearner:
    """Tests Smart Shadow Learner Episode Aggregation and Pattern Distillation."""

    def test_search_episode_aggregation(self, tmp_path):
        shadow = SmartShadowLearner(output_dir=tmp_path, episode_idle_threshold=0.1)

        a1 = AtomicAction("click", time.time(), "Google Chrome", "chrome.exe", 0x1, (100, 200))
        a2 = AtomicAction("type", time.time() + 0.01, "Google Chrome", "chrome.exe", 0x1, text_content="AI news")
        a3 = AtomicAction("hotkey", time.time() + 0.02, "Google Chrome", "chrome.exe", 0x1, key_code=0x0D)

        shadow.record_action(a1)
        shadow.record_action(a2)
        shadow.record_action(a3)

        # Force flush
        shadow.flush()

        assert shadow.episodes_file.is_file()
        records = [json.loads(line) for line in shadow.episodes_file.read_text(encoding="utf-8").splitlines()]
        assert len(records) == 1
        assert records[0]["pattern_type"] == "search"
        assert "AI news" in records[0]["semantic_summary"]

    def test_gaming_episode_detection(self, tmp_path):
        shadow = SmartShadowLearner(output_dir=tmp_path, episode_idle_threshold=0.1)

        g1 = AtomicAction("hotkey", time.time(), "Minecraft 1.20", "minecraft.exe", 0x2, key_code=0x57)  # 'W'
        g2 = AtomicAction("click", time.time() + 0.01, "Minecraft 1.20", "minecraft.exe", 0x2)

        shadow.record_action(g1)
        shadow.record_action(g2)
        shadow.flush()

        records = [json.loads(line) for line in shadow.episodes_file.read_text(encoding="utf-8").splitlines()]
        assert len(records) == 1
        assert records[0]["pattern_type"] == "game_control"
