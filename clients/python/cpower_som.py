"""
CPower Semantic Pruning & Set-of-Marks (SoM) Engine (clients/python/cpower_som.py).

Transforms raw 4,000+ node Windows UI Automation trees into a compact,
filtered set of 15-30 interactive, actionable elements (buttons, links, inputs)
and rich content blocks, eliminating 95%+ of empty layout noise in sub-millisecond time.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import re
from typing import Any, Dict, List, Optional, Set, Tuple


INTERACTIVE_CONTROL_TYPES = {
    "Button",
    "Hyperlink",
    "Edit",
    "ComboBox",
    "CheckBox",
    "RadioButton",
    "MenuItem",
    "TabItem",
    "SplitButton",
}

CONTENT_CONTROL_TYPES = {
    "Text",
    "Header",
    "HeaderItem",
    "ListItem",
}

LAYOUT_CONTAINER_TYPES = {
    "Pane",
    "Group",
    "Window",
    "Document",
    "Separator",
    "ScrollBar",
    "Thumb",
    "TitleBar",
    "MenuBar",
    "ToolBar",
    "StatusBar",
    "Table",
    "DataGrid",
}

GENERIC_NOISE_LABELS = {
    "", "null", "none", "view", "group", "pane", "container",
    "scroll bar", "line down", "line up", "page down", "page up",
    "minimize", "maximize", "restore", "close"
}


@dataclass
class SoMElement:
    mark_id: int
    node_id: int
    control_type: str
    name: str
    value: str
    bounds: Tuple[int, int, int, int]
    center: Tuple[int, int]
    is_interactive: bool = True
    automation_id: str = ""
    class_name: str = ""

    def to_summary(self) -> str:
        label = self.name.strip()
        val = self.value.strip()
        if val and val != label:
            display = f"{label} (val: {val[:60]})" if label else val[:60]
        else:
            display = label or self.automation_id or f"Element #{self.node_id}"
        return f"[{self.mark_id}] <{self.control_type}> {display!r} @ {self.center}"


@dataclass
class SoMResult:
    interactive_elements: List[SoMElement] = field(default_factory=list)
    content_blocks: List[str] = field(default_factory=list)
    total_raw_nodes: int = 0
    pruned_nodes: int = 0

    def get_element_by_mark(self, mark_id: int) -> Optional[SoMElement]:
        for el in self.interactive_elements:
            if el.mark_id == mark_id:
                return el
        return None

    def format_interactive_list(self, max_items: int = 30) -> str:
        lines = []
        for el in self.interactive_elements[:max_items]:
            lines.append(el.to_summary())
        return "\n".join(lines)


class SemanticPruner:
    """High-speed in-memory semantic pruner and Set-of-Marks generator."""

    def __init__(self, min_width: int = 8, min_height: int = 8, titlebar_guard_px: int = 120):
        self.min_width = min_width
        self.min_height = min_height
        self.titlebar_guard_px = titlebar_guard_px

    def prune_tree(self, tree: Dict[str, Any], window_bounds: Optional[Tuple[int, int, int, int]] = None) -> SoMResult:
        nodes = tree.get("nodes", [])
        total_raw = len(nodes)
        interactive: List[SoMElement] = []
        content: List[str] = []
        seen_signatures: Set[str] = set()

        mark_counter = 1

        for n in nodes:
            # Skip disabled nodes
            if not n.get("enabled", True):
                continue

            # Skip offscreen
            if n.get("offscreen", False):
                continue

            bounds = n.get("bounds", [0, 0, 0, 0])
            left, top, right, bottom = bounds
            w = right - left
            h = bottom - top

            if w < self.min_width or h < self.min_height:
                continue

            ctrl_type = n.get("type", "")
            name = (n.get("name") or "").strip()
            val = (n.get("value") or "").strip()
            aid = (n.get("automationId") or "").strip()
            cls = (n.get("className") or "").strip()
            cx, cy = n.get("center", (left + w // 2, top + h // 2))

            # Protect window title bar and browser toolbar (tabs, back, reload, url bar, bookmark)
            if cy < self.titlebar_guard_px:
                continue
            if aid.lower() in ("view_1", "view_2", "view_3", "view_4", "close-button") and cy < 120:
                continue

            # Check if interactive
            if ctrl_type in INTERACTIVE_CONTROL_TYPES:
                # Deduplicate identical items with same name, type, and approximate position
                sig = f"{ctrl_type}:{name[:30]}:{val[:30]}:{cx//20}:{cy//20}"
                if sig in seen_signatures:
                    continue
                seen_signatures.add(sig)

                # Skip completely blank, unnamed hyperlinks or buttons without value or automationId
                if not name and not val and not aid:
                    continue

                interactive.append(SoMElement(
                    mark_id=mark_counter,
                    node_id=int(n["id"]),
                    control_type=ctrl_type,
                    name=name,
                    value=val,
                    bounds=(left, top, right, bottom),
                    center=(cx, cy),
                    is_interactive=True,
                    automation_id=aid,
                    class_name=cls,
                ))
                mark_counter += 1

            elif ctrl_type in CONTENT_CONTROL_TYPES:
                # Accumulate rich reading content (ignore 1-2 word UI tags)
                if len(name) > 30 and not any(k in name.lower() for k in ("skip to", "sign in", "log in", "cookies", "terms of use")):
                    if name not in content:
                        content.append(name)

        return SoMResult(
            interactive_elements=interactive,
            content_blocks=content,
            total_raw_nodes=total_raw,
            pruned_nodes=total_raw - len(interactive),
        )
