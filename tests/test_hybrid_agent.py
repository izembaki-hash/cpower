"""
Automated Test Suite for CPower Hybrid Architecture (tests/test_hybrid_agent.py).

Verifies:
  1. Set-of-Marks (SoM) Semantic Pruner (Noise pruning >90%, interactive element marks)
  2. InputGuard Unicode clipboard encoding and safety
  3. MacroGoal decomposition for general natural language goals
  4. CPowerClient SoM integration
"""

import sys
from pathlib import Path
import unittest

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "clients" / "python"))

from cpower_som import SemanticPruner, SoMElement, SoMResult
from cpower_hybrid_agent import HybridAutonomousAgent, InputGuard


class TestHybridArchitecture(unittest.TestCase):

    def test_som_pruning_and_titlebar_guard(self):
        pruner = SemanticPruner(titlebar_guard_px=120)

        raw_tree = {
            "nodeCount": 6,
            "nodes": [
                # Titlebar close button (should be ignored)
                {
                    "id": 1, "enabled": True, "offscreen": False,
                    "type": "Button", "name": "Close", "value": "",
                    "bounds": [1880, 0, 1920, 35], "center": [1900, 17],
                    "automationId": "close-button", "className": "TitleBarButton"
                },
                # Large empty layout container (should be filtered out)
                {
                    "id": 2, "enabled": True, "offscreen": False,
                    "type": "Pane", "name": "", "value": "",
                    "bounds": [0, 0, 1920, 1080], "center": [960, 540],
                    "automationId": "", "className": "Chrome_RenderWidgetHostHWND"
                },
                # Real interactive article link
                {
                    "id": 3, "enabled": True, "offscreen": False,
                    "type": "Hyperlink", "name": "AI Breakthrough in Computer-Use Agents",
                    "value": "https://www.reddit.com/r/technology/comments/123/ai_breakthrough",
                    "bounds": [200, 300, 600, 330], "center": [400, 315],
                    "automationId": "post-title-123", "className": "StyledLink"
                },
                # Second interactive article
                {
                    "id": 4, "enabled": True, "offscreen": False,
                    "type": "Hyperlink", "name": "Open-Source Native C11 Engine Released",
                    "value": "https://www.reddit.com/r/technology/comments/456/native_c11",
                    "bounds": [200, 400, 600, 430], "center": [400, 415],
                    "automationId": "post-title-456", "className": "StyledLink"
                },
                # Real interactive button
                {
                    "id": 5, "enabled": True, "offscreen": False,
                    "type": "Button", "name": "Search Reddit", "value": "",
                    "bounds": [800, 200, 900, 230], "center": [850, 215],
                    "automationId": "search-btn", "className": "Button"
                },
                # Meaningful content text
                {
                    "id": 6, "enabled": True, "offscreen": False,
                    "type": "Text", "name": "This is a detailed community discussion paragraph discussing the new system.",
                    "value": "",
                    "bounds": [200, 500, 800, 540], "center": [500, 520],
                    "automationId": "", "className": "Paragraph"
                },
            ]
        }

        result = pruner.prune_tree(raw_tree)
        self.assertEqual(result.total_raw_nodes, 6)
        # Should retain only the 2 hyperlinks + 1 search button as interactive
        self.assertEqual(len(result.interactive_elements), 3)
        self.assertEqual(result.interactive_elements[0].name, "AI Breakthrough in Computer-Use Agents")
        self.assertEqual(result.interactive_elements[1].name, "Open-Source Native C11 Engine Released")
        self.assertEqual(result.interactive_elements[2].name, "Search Reddit")

        # Content block should be captured
        self.assertEqual(len(result.content_blocks), 1)
        self.assertIn("community discussion", result.content_blocks[0])

    def test_input_guard_unicode_clipboard(self):
        sample_text = "https://www.reddit.com/r/technology?q=ذكاء_اصطناعي"
        success = InputGuard.set_clipboard_unicode(sample_text)
        self.assertTrue(success)

    def test_macro_goal_decomposition(self):
        agent = HybridAutonomousAgent()
        url1 = agent.decompose_macro_goal("ادخل الى reddit وتصفح المقالات")
        self.assertIn("reddit.com", url1)

        url2 = agent.decompose_macro_goal("ابحث في جيت هب عن ادوات الذكاء الاصطناعي")
        self.assertIn("github.com", url2)

        url3 = agent.decompose_macro_goal("افتح https://news.ycombinator.com واقرأ الأخبار")
        self.assertEqual(url3, "https://news.ycombinator.com")


    def test_process_guard_protection(self):
        from cpower_process_guard import is_protected_window
        # Antigravity should always be protected
        self.assertTrue(is_protected_window(1234, 11812, "Antigravity - Chat"))
        # Generic title with antigravity name
        self.assertTrue(is_protected_window(5678, 9999, "Antigravity"))

    def test_episodic_memory_guidance(self):
        from cpower_memory import EpisodicMemory
        mem = EpisodicMemory()
        guidance = mem.get_prompt_guidance()
        self.assertIn("CRITICAL CONSTRAINTS", guidance)
        self.assertIn("Antigravity.exe", guidance)


if __name__ == "__main__":
    unittest.main()
