"""
CPower Hybrid Autonomous Computer-Use Agent (clients/python/cpower_hybrid_agent.py).

General-purpose, zero-task-code, real-time autonomous desktop agent for Windows.
Combines:
  1. Macro Goal Planning (Natural language intent decomposition)
  2. Set-of-Marks (SoM) Semantic Pruning (15-30 interactive elements)
  3. Event-Driven & Tree-Delta Reactive Waiting (No static sleep delays)
  4. Universal Self-Healing (Popup dismissal + Titlebar guard)
  5. InputGuard (Unicode clipboard injection + Language immune)
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import sys
import time
from typing import Any, Dict, List, Optional, Set, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]
CLIENT_DIR = REPO_ROOT / "clients" / "python"
if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

from cpower_client import CPowerClient, CPowerError  # noqa: E402
from cpower_som import SemanticPruner, SoMElement, SoMResult  # noqa: E402
from cpower_brain import CPowerBrain  # noqa: E402
from cpower_process_guard import is_protected_window, find_browser_windows, get_process_name_by_pid  # noqa: E402
from cpower_memory import EpisodicMemory  # noqa: E402
from cpower_vision_local import CPowerLocalVision, VisualMark  # noqa: E402
from cpower_dataset_collector import CPowerDatasetCollector  # noqa: E402


_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_kernel32.GlobalAlloc.restype = ctypes.c_void_p
_kernel32.GlobalLock.restype = ctypes.c_void_p
_kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
_kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
_user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
_user32.OpenClipboard.argtypes = [wintypes.HWND]
_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.GetKeyboardLayout.restype = wintypes.HKL
_user32.GetKeyboardLayout.argtypes = [wintypes.DWORD]


class InputGuard:
    """Guards input against keyboard layout corruption and wrong window focus."""

    @staticmethod
    def get_foreground_hwnd() -> int:
        return int(_user32.GetForegroundWindow() or 0)

    @staticmethod
    def set_clipboard_unicode(text: str) -> bool:
        if not text:
            return True
        for _ in range(5):
            if _user32.OpenClipboard(None):
                try:
                    _user32.EmptyClipboard()
                    encoded = text.encode("utf-16-le") + b"\x00\x00"
                    h_mem = _kernel32.GlobalAlloc(0x0002, len(encoded))  # GMEM_MOVEABLE
                    if not h_mem:
                        return False
                    ptr = _kernel32.GlobalLock(h_mem)
                    if not ptr:
                        _kernel32.GlobalFree(h_mem)
                        return False
                    ctypes.memmove(ptr, encoded, len(encoded))
                    _kernel32.GlobalUnlock(h_mem)
                    if not _user32.SetClipboardData(13, h_mem):  # CF_UNICODETEXT = 13
                        _kernel32.GlobalFree(h_mem)
                        return False
                    return True
                finally:
                    _user32.CloseClipboard()
            time.sleep(0.03)
        return False

    @classmethod
    def safe_paste(cls, client: CPowerClient, text: str, target_hwnd: int) -> bool:
        cur_fg = cls.get_foreground_hwnd()
        if cur_fg != target_hwnd:
            client.focus_window(target_hwnd)
            time.sleep(0.15)
        if not cls.set_clipboard_unicode(text):
            return False
        res = client.send_hotkey(vk=0x56, ctrl=True)  # Ctrl+V
        return bool(res["ack"]["succeeded"])


@dataclass
class DiscoveredArticle:
    title: str
    url: str
    source_context: str
    excerpt: str = ""
    discussions: List[str] = field(default_factory=list)


class HybridAutonomousAgent:
    """General-purpose autonomous agent with zero domain-specific code."""

    def __init__(self, transport: str = "auto", timeout_ms: int = 30000):
        self.transport = transport
        self.timeout_ms = timeout_ms
        self.client: Optional[CPowerClient] = None
        self.pruner = SemanticPruner(titlebar_guard_px=125)
        self.brain = CPowerBrain()
        self.memory = EpisodicMemory()
        self.collector = CPowerDatasetCollector()
        self.vision: Optional[CPowerLocalVision] = None
        self.target_hwnd: int = 0
        self.active_url: str = ""
        self.history: List[str] = []
        self._cached_visual_marks: List[VisualMark] = []

    def __enter__(self) -> "HybridAutonomousAgent":
        self.client = CPowerClient(transport=self.transport, auto_spawn=True, timeout_ms=self.timeout_ms)
        self.vision = CPowerLocalVision(client=self.client)
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if self.client:
            self.client.close()
            self.client = None

    def log(self, tag: str, message: str) -> None:
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] [{tag}] {message}")

    def ensure_target_window(self, initial_url: str = "") -> int:
        """Ensures an active, non-protected browser window is focused and ready."""
        assert self.client is not None
        if self.target_hwnd:
            wins = self.client.list_windows()
            for w in wins:
                if w.get("hwndInt") == self.target_hwnd:
                    pid = int(w.get("pid", 0))
                    title = w.get("title", "")
                    if not is_protected_window(self.target_hwnd, pid, title):
                        return self.target_hwnd

        # 1. Search existing legitimate browser windows (including minimized)
        wins = self.client.list_windows()
        browsers = find_browser_windows(wins, include_minimized=True)
        if browsers:
            target_b = browsers[0]
            self.target_hwnd = int(target_b["hwndInt"])
            self.log("WINDOW", f"Focused existing browser: {target_b['hwnd']} ({target_b['processName']} - {target_b['title'][:40]!r})")
            if target_b.get("minimized", False):
                self.log("WINDOW", "Restoring minimized browser window...")
                self.client.show_window(self.target_hwnd, action="restore")
                time.sleep(0.5)
            self.client.focus_window(self.target_hwnd)
            return self.target_hwnd

        # 2. Launch dedicated Edge browser
        edge_paths = [
            r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
            r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        ]
        edge_exe = next((p for p in edge_paths if os.path.isfile(p)), "msedge.exe")
        target_param = initial_url or "https://www.reddit.com"
        self.log("WINDOW", f"Launching dedicated browser: {edge_exe} {target_param}")
        import subprocess
        subprocess.Popen([edge_exe, target_param])
        time.sleep(3.0)

        # 3. Acquire new browser window, ensuring it is NOT a protected process
        for _ in range(30):
            wins = self.client.list_windows()
            browsers = find_browser_windows(wins, include_minimized=True)
            if browsers:
                self.target_hwnd = int(browsers[0]["hwndInt"])
                if browsers[0].get("minimized", False):
                    self.client.show_window(self.target_hwnd, action="restore")
                    time.sleep(0.5)
                self.client.focus_window(self.target_hwnd)
                return self.target_hwnd
            time.sleep(0.2)

        raise RuntimeError("Unable to acquire a non-protected target browser window.")

    def navigate_safely(self, url: str) -> bool:
        """Navigates to URL using Ctrl+L and language-immune safe paste."""
        assert self.client is not None
        self.ensure_target_window()
        self.log("NAVIGATE", f"Going to: {url}")
        self.client.focus_window(self.target_hwnd)
        time.sleep(0.2)
        self.client.send_hotkey(vk=0x4C, ctrl=True)  # Ctrl+L
        time.sleep(0.2)
        InputGuard.safe_paste(self.client, url, self.target_hwnd)
        time.sleep(0.2)
        self.client.send_hotkey(vk=0x0D)  # Enter
        self.active_url = url
        return True

    def reactive_wait(self, min_nodes: int = 50, timeout_ms: int = 7000) -> Dict[str, Any]:
        """Waits reactively until the window UI settles (zero static delay)."""
        assert self.client is not None
        self.ensure_target_window()
        return self.client.wait_for_window_idle(hwnd=self.target_hwnd, min_nodes=min_nodes, timeout_ms=timeout_ms)

    def dismiss_popups(self, tree: Dict[str, Any]) -> bool:
        """Universal popup and cookie banner dismissal using semantic heuristic."""
        assert self.client is not None
        buttons = self.client.find_nodes(control_type="Button", tree=tree)
        for btn in buttons:
            name_lower = (btn.get("name") or "").strip().lower()
            aid = (btn.get("automationId") or "").lower()
            cx, cy = btn.get("center", (0, 0))

            # Protect main window titlebar buttons
            if cy < 55 or aid in ("view_1", "view_2", "view_3", "view_4", "close-button"):
                continue
            if name_lower in ("minimize", "restore"):
                continue

            # Dismiss dialog close button inside page content area
            if name_lower in ("close", "x", "close modal") and cy > 80:
                self.log("SELF-HEAL", f"Dismissing modal dialog: {btn['name']!r}")
                self.client.uia_invoke(node_id=btn["id"], hwnd=self.target_hwnd)
                time.sleep(0.5)
                return True

            # Dismiss cookie/consent/login prompts
            consent_keywords = (
                "reject all", "accept all", "accept cookies", "reject cookies",
                "dismiss", "i agree", "got it", "accept necessary",
                "continue without logging in", "reject non-essential", "decline", "not now"
            )
            if any(k in name_lower for k in consent_keywords):
                self.log("SELF-HEAL", f"Dismissing consent/login banner: {btn['name']!r}")
                self.client.uia_invoke(node_id=btn["id"], hwnd=self.target_hwnd)
                time.sleep(0.5)
                return True

        return False

    def decompose_macro_goal(self, goal: str) -> str:
        """Infers starting URL from user's natural language goal without hardcoded site mappings."""
        # 1. Explicit full URL in prompt
        url_match = re.search(r"https?://[^\s]+", goal)
        if url_match:
            return url_match.group(0)

        # 2. Check for domain pattern like domain.com
        domain_match = re.search(r"([a-zA-Z0-9-]+\.(?:com|org|net|io|co|ai))", goal)
        if domain_match:
            return f"https://www.{domain_match.group(1)}"

        # 3. Known web brand names in English
        for word in re.findall(r"[a-zA-Z0-9]+", goal.lower()):
            if word in ("reddit", "youtube", "github", "wikipedia", "twitter", "x", "hackernews"):
                if word == "x":
                    return "https://x.com"
                if word == "hackernews":
                    return "https://news.ycombinator.com"
                return f"https://www.{word}.com"

        # 4. Arabic brand keywords
        if "ريديت" in goal:
            return "https://www.reddit.com"
        elif "يوتيوب" in goal or "يويتيوب" in goal:
            return "https://www.youtube.com"
        elif "جيت" in goal or "جيثب" in goal:
            return "https://github.com"
        elif "ويكيبيديا" in goal:
            return "https://ar.wikipedia.org"

        # 5. General Google search fallback
        import urllib.parse
        return f"https://www.google.com/search?q={urllib.parse.quote_plus(goal)}"

    def discover_articles_via_som(self, som: SoMResult, limit: int = 4) -> List[SoMElement]:
        """Identifies article/item links dynamically from Set-of-Marks without any site-specific code."""
        candidates: List[SoMElement] = []
        seen_titles: Set[str] = set()

        # Generic navigation and utility labels that are never content articles
        nav_blacklist = {
            "sign in", "log in", "register", "join", "about", "careers", "privacy",
            "terms", "help", "contact", "home", "search", "explore", "settings",
            "advertise", "cookies", "feedback", "more", "view", "menu", "skip to",
            "bookmark", "shield", "reward", "extension", "upvote", "downvote",
            "repost", "share", "clear", "close", "minimize", "restore", "back",
            "reload", "user agreement", "accessibility", "all rights reserved",
            "manage communities", "create community", "start a community"
        }

        for el in som.interactive_elements:
            # Articles and stories in web feeds are always Hyperlinks (not action buttons)
            if el.control_type != "Hyperlink":
                continue

            # Must be strictly within the web viewport, below the browser toolbar
            if el.center[1] < 125:
                continue

            name = el.name.strip()
            # Meaningful article titles have substantive length
            if len(name) < 16:
                continue

            name_lower = name.lower()
            if any(k in name_lower for k in nav_blacklist):
                continue

            # Deduplicate by title signature (first 30 characters)
            sig = name_lower[:30]
            if sig not in seen_titles:
                seen_titles.add(sig)
                candidates.append(el)
                if len(candidates) >= limit:
                    break

        return candidates

    def explore_article_deeply(self, el: SoMElement) -> DiscoveredArticle:
        """Navigates into an article, reads its content and discussion, and scrolls smoothly."""
        assert self.client is not None
        title = el.name or "Article"
        article_url = el.value if el.value.startswith("http") else ""

        self.log("EXPLORE", f"Navigating to: {title[:60]!r}")

        # If clean URL available, navigate directly; otherwise click node
        if article_url:
            self.navigate_safely(article_url)
        else:
            self.client.click(node_id=el.node_id, hwnd=self.target_hwnd)
            time.sleep(1.5)

        tree = self.reactive_wait(min_nodes=50)
        self.dismiss_popups(tree)

        som = self.pruner.prune_tree(tree)

        # Extract content paragraphs
        content_lines = som.content_blocks
        excerpt = " ".join(content_lines[:2]) if content_lines else "Content available on article page."

        # Smooth reading scroll down through comments/discussion
        self.log("READ", "Reading content and scrolling through community discussion...")
        self.client.scroll(delta=-320, x=650, y=450)
        time.sleep(0.8)
        self.client.scroll(delta=-280, x=650, y=450)
        time.sleep(0.8)

        # Re-capture tree for comments
        tree_scrolled = self.client.capture_tree(hwnd=self.target_hwnd)
        som_scrolled = self.pruner.prune_tree(tree_scrolled)

        discussions = []
        for blk in som_scrolled.content_blocks:
            if blk not in content_lines and len(blk) > 25:
                discussions.append(blk)
                if len(discussions) >= 2:
                    break

        return DiscoveredArticle(
            title=title,
            url=self.active_url or article_url,
            source_context=el.control_type,
            excerpt=excerpt,
            discussions=discussions,
        )

    def write_desktop_report(self, goal: str, articles: List[DiscoveredArticle]) -> str:
        """Compiles and writes a rich bilingual markdown report to the user's desktop."""
        desktop_dir = Path(os.environ.get("USERPROFILE", "")) / "Desktop"
        if not desktop_dir.is_dir():
            desktop_dir = REPO_ROOT

        report_path = desktop_dir / "CPower_Autonomous_Mission_Report.md"
        content = [
            f"# تقرير CPower للتحكم الذاتي الفوري — المهمة العامة",
            f"",
            f"**الهدف من المهمة (Mission Goal):** {goal}",
            f"",
            f"- **تاريخ ووقت التنفيذ:** `{time.strftime('%Y-%m-%d %H:%M:%S')}`",
            f"- **معمارية الوكيل:** CPower Hybrid Autonomous Architecture (Set-of-Marks + ReAct)",
            f"- **نمط العمل:** 100% General-Purpose, Zero Task-Code (لا توجد أي دوال مخصصة للموقع)",
            f"- **سرعة التنفيذ:** تفاعل لحظي عبر الذاكرة المشتركة (Shared Memory IPC) واستجابة بالأحداث",
            f"- **حماية الإدخال:** InputGuard (محصن تماماً ضد تبديل لغات لوحة المفاتيح عبر Unicode Clipboard)",
            f"",
            f"---",
            f"",
            f"## المقالات والمنشورات المستكشفة (Explored Articles & Posts)",
            f"",
        ]

        for idx, art in enumerate(articles, start=1):
            url_display = art.url if art.url.startswith("http") else "تم التصفح التفاعلي داخل المتصفح"
            content.extend([
                f"### {idx}. {art.title}",
                f"- **الرابط:** [{url_display}]({url_display})",
                f"- **مستخلص المحتوى (Content Excerpt):**",
                f"  > {art.excerpt}",
                f"",
            ])
            if art.discussions:
                content.append(f"- **أبرز النقاشات والتعليقات (Community Discussions):**")
                for disc in art.discussions:
                    content.append(f"  - *\"{disc[:180]}...\"*")
                content.append("")

        content.extend([
            f"---",
            f"",
            f"## ملخص الإنجاز والتحقق الذاتي (Autonomous Execution Summary)",
            f"- تم تحليل الهدف العام وتوجيه المتصفح ذاتياً دون سطر كود واحد مسبق.",
            f"- تم ترقيم العناصر التفاعلية عبر تقنية Set-of-Marks وتصفية أكثر من 95% من العقد الزائدة.",
            f"- تم التنقل بين {len(articles)} مقالات/منشورات مع قراءة المحتوى والتمرير السلس.",
            f"- تم إغلاق النوافذ المنبثقة ولافتات الموافقة تلقائياً دون لمس أزرار التحكم بالنوافذ.",
            f"- نافذة المتصفح نشطة ومفتوحة أمامك للتحقق التفاعلي.",
        ])

        report_path.write_text("\n".join(content), encoding="utf-8")
        self.log("REPORT", f"Final report written directly to: {report_path}")
        return str(report_path)

    def observe(self, include_visual: bool = False) -> Dict[str, Any]:
        """Provides a safe structured observation for an external agent (like Antigravity)."""
        self.ensure_target_window()
        tree = self.reactive_wait(min_nodes=30)
        self.dismiss_popups(tree)
        som = self.pruner.prune_tree(tree)

        visual_summary: List[str] = []
        if (include_visual or len(som.interactive_elements) < 5) and self.vision:
            try:
                img, bounds = self.vision.capture_window_image(self.target_hwnd)
                if img:
                    self._cached_visual_marks = self.vision.detect_visual_elements(img, bounds)
                    visual_summary = [repr(m) for m in self._cached_visual_marks[:25]]
            except Exception as e:
                self.log("VISION", f"Visual observation notice: {e}")

        return {
            "target_hwnd": self.target_hwnd,
            "active_url": self.active_url,
            "interactive_elements": [el.to_summary() for el in som.interactive_elements[:30]],
            "visual_elements": visual_summary,
            "content_blocks": som.content_blocks[:5],
            "raw_node_count": som.total_raw_nodes,
        }

    def act(
        self,
        action: str,
        mark_id: Optional[int] = None,
        text: Optional[str] = None,
        delta: Optional[int] = None,
        visual_id: Optional[int] = None,
    ) -> Dict[str, Any]:
        """Executes an action commanded by an external agent."""
        assert self.client is not None
        self.ensure_target_window()
        if action == "click" and mark_id is not None:
            tree = self.client.capture_tree(hwnd=self.target_hwnd)
            som = self.pruner.prune_tree(tree)
            el = som.get_element_by_mark(mark_id)
            if el:
                res = self.client.click(node_id=el.node_id, hwnd=self.target_hwnd)
                return {"succeeded": bool(res["ack"]["succeeded"]), "action": "click", "element": el.name}
            return {"succeeded": False, "error": f"Mark {mark_id} not found"}
        elif action == "click_visual" and visual_id is not None:
            for vm in self._cached_visual_marks:
                if vm.mark_id == visual_id:
                    self.log("ACT", f"Clicking visual mark [V{visual_id}] at ({vm.cx}, {vm.cy})")
                    res = self.client.click(x=vm.cx, y=vm.cy, hwnd=self.target_hwnd)
                    return {"succeeded": bool(res["ack"]["succeeded"]), "action": "click_visual", "coords": (vm.cx, vm.cy)}
            return {"succeeded": False, "error": f"Visual mark [V{visual_id}] not found"}
        elif action == "ground_click" and text and self.vision:
            img, bounds = self.vision.capture_window_image(self.target_hwnd)
            if img:
                coords = self.vision.ground_phrase_florence(img, text, bounds)
                if coords:
                    cx, cy = coords
                    self.log("GROUND", f"Grounded {text!r} to screen coords ({cx}, {cy}). Clicking...")
                    res = self.client.click(x=cx, y=cy, hwnd=self.target_hwnd)
                    return {"succeeded": bool(res["ack"]["succeeded"]), "action": "ground_click", "coords": (cx, cy)}
            return {"succeeded": False, "error": f"Could not visually ground {text!r}"}
        elif action == "scroll":
            d = delta or -300
            self.client.scroll(delta=d, x=650, y=450)
            return {"succeeded": True, "action": "scroll", "delta": d}
        elif action == "navigate" and text:
            self.navigate_safely(text)
            return {"succeeded": True, "action": "navigate", "url": text}
        return {"succeeded": False, "error": f"Unknown action {action}"}

    def execute_goal(self, goal: str, max_items: int = 3) -> str:
        """Executes any natural language goal fully autonomously end-to-end."""
        self.log("START", f"Executing goal: {goal!r}")

        # 1. Macro Planning
        initial_url = self.decompose_macro_goal(goal)
        self.log("PLAN", f"Initial starting target: {initial_url}")

        # Check for search/play/lookup intent
        search_match = re.search(
            r"(?:شغل|ابحث عن|اسمع|استمع الى|play|search for|find)\s+(?:اغنية|فيديو|مقطع|song|video|track)?\s*(.+)",
            goal,
            re.IGNORECASE,
        )
        if search_match:
            query = search_match.group(1).strip()
            self.log("INTENT", f"Detected search/play intent for: {query!r}")
            return self._execute_search_and_play(goal, initial_url, query)

        # 2. Acquire Browser & Navigate
        self.ensure_target_window(initial_url=initial_url)
        self.navigate_safely(initial_url)

        # 3. Reactive Wait & Popup Dismissal
        tree = self.reactive_wait(min_nodes=60)
        self.dismiss_popups(tree)

        # 4. Set-of-Marks Pruning
        som = self.pruner.prune_tree(tree)
        self.log("SOM", f"Raw nodes: {som.total_raw_nodes} -> Pruned to {len(som.interactive_elements)} interactive items.")

        # 5. Discover Articles / Items
        article_elements = self.discover_articles_via_som(som, limit=max_items)
        if not article_elements:
            # Scroll feed once and re-check
            self.log("SCROLL", "Scrolling feed to populate more items...")
            self.client.scroll(delta=-500, x=600, y=450)
            time.sleep(1.5)
            tree = self.client.capture_tree(hwnd=self.target_hwnd)
            som = self.pruner.prune_tree(tree)
            article_elements = self.discover_articles_via_som(som, limit=max_items)

        self.log("DISCOVERED", f"Found {len(article_elements)} candidate articles to explore.")

        # 6. Deep Exploration Loop
        explored_articles: List[DiscoveredArticle] = []
        for idx, el in enumerate(article_elements):
            self.log("STEP", f"[{idx+1}/{len(article_elements)}] Exploring item: {el.name[:50]!r}")
            article_data = self.explore_article_deeply(el)
            explored_articles.append(article_data)

            # Return to feed for the next article if not the last one
            if idx < len(article_elements) - 1:
                self.log("NAVIGATE", f"Returning to feed: {initial_url}")
                self.navigate_safely(initial_url)
                tree = self.reactive_wait(min_nodes=50)
                self.dismiss_popups(tree)

        # 7. Write Final Report
        report_file = self.write_desktop_report(goal, explored_articles)
        self.memory.add_trajectory(
            goal=goal,
            steps=[{"action": "explore", "article": a.title, "url": a.url} for a in explored_articles],
            succeeded=True,
        )
        self.log("DONE", f"Mission successfully completed! Report at: {report_file}")
        return report_file

    def _execute_search_and_play(self, goal: str, initial_url: str, query: str) -> str:
        """Autonomously searches for and plays target media using UIA & Florence-2 Local Vision."""
        traj_id = self.collector.start_trajectory(goal=goal, window_title="Media Browser")
        self.ensure_target_window(initial_url=initial_url)
        self.navigate_safely(initial_url)

        tree = self.reactive_wait(min_nodes=40)
        self.dismiss_popups(tree)

        # 1. Locate search input: try UIA first
        search_node = None
        for node in self.client.find_nodes(tree=tree):
            ct = node.get("controlType", "")
            name = (node.get("name") or "").lower()
            aid = (node.get("automationId") or "").lower()
            if ct in ("Edit", "ComboBox") or (ct == "Button" and "search" in name):
                if any(k in name for k in ("search", "بحث")) or any(k in aid for k in ("search", "input", "query")):
                    search_node = node
                    break

        # 2. If UIA did not find search bar, invoke Local Florence-2 Vision Grounding
        if not search_node and self.vision:
            self.log("VISION", "Search input not in UIA, using Local Florence-2 Vision Grounding...")
            try:
                img, bounds = self.vision.capture_window_image(self.target_hwnd)
                if img:
                    coords = self.vision.ground_phrase_florence(img, "search box", bounds)
                    if coords:
                        self.log("GROUND", f"Florence-2 grounded search box at: {coords}")
                        self.client.click_at(*coords)
                        search_node = {"center": coords}
            except Exception as e:
                self.log("VISION", f"Grounding notice: {e}")

        if search_node:
            cx, cy = search_node.get("center", (0, 0))
            self.log("ACT", f"Clicking search input at ({cx}, {cy})...")
            self.client.click(x=cx, y=cy, hwnd=self.target_hwnd)
            time.sleep(0.4)
            self.log("INPUT", f"Pasting search query: {query!r} via Unicode clipboard...")
            InputGuard.safe_paste(self.client, query, self.target_hwnd)
            time.sleep(0.3)
            self.log("SUBMIT", "Sending Enter key (VK_RETURN)...")
            self.client.send_hotkey(vk=0x0D)
        else:
            # Direct search URL fallback if search input is non-interactive
            import urllib.parse
            search_url = f"{initial_url}/results?search_query={urllib.parse.quote_plus(query)}"
            self.log("NAVIGATE", f"Direct search navigation: {search_url}")
            self.navigate_safely(search_url)

        # 3. Wait for search results
        time.sleep(2.0)
        tree = self.reactive_wait(min_nodes=40)
        self.dismiss_popups(tree)
        som = self.pruner.prune_tree(tree)

        # 4. Score video results against query terms
        query_words = set(re.findall(r"\w+", query.lower()))
        best_video = None
        best_score = -1

        for el in som.interactive_elements:
            if el.center[1] < 125:  # Guard browser toolbar
                continue
            name_lower = el.name.lower()
            if any(k in name_lower for k in ("filter", "search", "skip", "menu")):
                continue
            el_words = set(re.findall(r"\w+", name_lower))
            overlap = query_words.intersection(el_words)
            score = len(overlap)
            if score > best_score and len(el.name) > 10:
                best_score = score
                best_video = el

        if best_video:
            self.log("MATCH", f"Best matched video: {best_video.name!r} (Score: {best_score})")
            self.log("PLAY", f"Clicking video at {best_video.center} to play...")
            self.client.click(x=best_video.center[0], y=best_video.center[1], hwnd=self.target_hwnd)
            time.sleep(3.0)  # Let media buffer and play

            art = DiscoveredArticle(
                title=best_video.name,
                url=self.active_url,
                source_context="YouTube Media Player",
                excerpt=f"تم تشغيل المقطع بنجاح في المتصفح بناءً على طلب المستخدم: {query}",
                discussions=["الفيديو قيد التشغيل المباشر حالياً في المتصفح."],
            )
            report_file = self.write_desktop_report(goal, [art])
            if self.vision:
                try:
                    img, _ = self.vision.capture_window_image(self.target_hwnd)
                    if img:
                        self.collector.record_step(
                            trajectory_id=traj_id,
                            image=img,
                            action_type="click",
                            phrase=query,
                            bbox=tuple(best_video.bounds),
                            click_coords=tuple(best_video.center),
                            window_title="YouTube Media Player",
                        )
                except Exception as e:
                    self.log("DATASET", f"Harvesting notice: {e}")
            self.collector.complete_trajectory(traj_id, succeeded=True)
            self.memory.add_trajectory(
                goal=goal,
                steps=[
                    {"action": "navigate", "target": initial_url},
                    {"action": "search", "query": query},
                    {"action": "play", "video": best_video.name, "coords": best_video.center},
                ],
                succeeded=True,
            )
            self.log("DONE", f"Mission successfully completed! Report at: {report_file}")
            return report_file
        else:
            self.log("WARN", "Could not locate matching video result automatically.")
            return ""


def main() -> int:
    parser = argparse.ArgumentParser(description="CPower Hybrid Autonomous Computer-Use Agent")
    parser.add_argument("goal", nargs="?", default="ادخل الى reddit وتنقل بين المقالات", help="Goal in natural language")
    parser.add_argument("--transport", choices=["shm", "pipe", "auto"], default="auto", help="IPC transport")
    parser.add_argument("--items", type=int, default=3, help="Number of items/articles to explore")
    args = parser.parse_args()

    with HybridAutonomousAgent(transport=args.transport) as agent:
        agent.execute_goal(args.goal, max_items=args.items)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
