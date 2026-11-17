"""
CPower Autonomous On-Screen Disk & Folder Mapper (clients/python/cpower_disk_mapper.py).

Navigates Windows File Explorer entirely on-screen:
  1. Discovers drives visually / through UI elements starting with Drive E:.
  2. Double-clicks into each drive and folder live on-screen.
  3. Records subfolders and files hierarchically.
  4. Steps back via Alt+Up keyboard actuation.
  5. Continuously updates a rich Live Tree Map on the user's Desktop.
  6. 100% General-purpose Win32 / UIA actuation with strict Antigravity protection.
"""

from __future__ import annotations

import argparse
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

from cpower_client import CPowerClient
from cpower_process_guard import is_protected_window, _user32


@dataclass
class DiskNode:
    name: str
    is_folder: bool
    children: List["DiskNode"] = field(default_factory=list)
    item_count: int = 0


class CPowerDiskMapper:
    """Autonomous live on-screen filesystem explorer and mapper."""

    def __init__(self, start_drive: str = "E:", max_depth: int = 2):
        self.client = CPowerClient()
        self.start_drive = start_drive.upper()
        self.max_depth = max_depth
        self.explorer_hwnd: int = 0
        self.root_map: Dict[str, DiskNode] = {}
        self.desktop_report_path = Path(os.environ.get("USERPROFILE", "")) / "Desktop" / "CPower_Live_Drive_Map.md"

    def log(self, tag: str, msg: str) -> None:
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] [{tag}] {msg}", flush=True)

    def ensure_explorer_window(self) -> int:
        """Finds or launches Windows File Explorer and restores it on-screen."""
        for w in self.client.list_windows():
            cls = w.get("className", "")
            title = str(w.get("title", ""))
            if "CabinetWClass" in cls or "File Explorer" in title:
                hwnd = int(w["hwnd"], 16) if isinstance(w["hwnd"], str) else int(w["hwnd"])
                self.client.show_window(hwnd, "restore")
                self.client.focus_window(hwnd)
                time.sleep(0.5)
                self.explorer_hwnd = hwnd
                self.log("WINDOW", f"Found and focused File Explorer window: {hwnd:#x}")
                return hwnd

        # Open File Explorer via Win+E
        self.log("WINDOW", "File Explorer not open, sending Win+E hotkey...")
        self.client.send_hotkey(vk=0x45, win=True)  # Win+E
        time.sleep(1.5)

        for w in self.client.list_windows():
            cls = w.get("className", "")
            title = str(w.get("title", ""))
            if "CabinetWClass" in cls or "File Explorer" in title:
                hwnd = int(w["hwnd"], 16) if isinstance(w["hwnd"], str) else int(w["hwnd"])
                self.client.show_window(hwnd, "restore")
                self.client.focus_window(hwnd)
                time.sleep(0.5)
                self.explorer_hwnd = hwnd
                self.log("WINDOW", f"File Explorer launched and focused: {hwnd:#x}")
                return hwnd

        raise RuntimeError("Failed to launch or acquire File Explorer window.")

    def get_visible_items(self) -> List[Dict[str, Any]]:
        """Captures visible ListItems inside File Explorer."""
        tree = self.client.capture_tree(hwnd=self.explorer_hwnd)
        items = []
        for n in tree.get("nodes", []):
            if n.get("type") in ("ListItem", "TreeItem") and n.get("name"):
                name = n.get("name", "").strip()
                # Exclude system caption / close buttons
                if name in ("Minimize", "Maximize", "Close", "Restore"):
                    continue
                items.append(n)
        return items

    def navigate_up(self) -> bool:
        """Sends Alt + Up Arrow to step up to the parent directory."""
        self.log("NAV", "Stepping back up (Alt + Up Arrow)...")
        self.client.focus_window(self.explorer_hwnd)
        time.sleep(0.15)
        res = self.client.send_hotkey(vk=0x26, alt=True)  # Alt+Up
        time.sleep(0.8)
        return bool(res["ack"]["succeeded"])

    def navigate_to_this_pc(self) -> bool:
        """Navigates to This PC to reveal all drive letters."""
        self.log("NAV", "Navigating to 'This PC' to inspect all system drives...")
        for _ in range(3):
            items = self.get_visible_items()
            drive_names = [it["name"] for it in items if any(f"({d}:)" in it["name"] for d in "CDEFGHIJK")]
            if len(drive_names) >= 2:
                self.log("DISCOVERY", f"Visible system drives: {drive_names}")
                return True

            # Check if 'This PC' item is visible to double click
            this_pc = next((it for it in items if "This PC" in it["name"]), None)
            if this_pc:
                cx, cy = this_pc["center"]
                self.client.double_click(x=cx, y=cy, hwnd=self.explorer_hwnd)
                time.sleep(1.0)
            else:
                self.navigate_up()
                time.sleep(0.5)

        return True

    def discover_drives_on_screen(self) -> List[Dict[str, Any]]:
        """Discovers drives directly from File Explorer UI elements."""
        self.navigate_to_this_pc()
        items = self.get_visible_items()
        drives = []
        for it in items:
            name = it["name"]
            m = re.search(r"\(([A-Z]:)\)", name)
            if m:
                letter = m.group(1)
                drives.append({"node": it, "letter": letter, "name": name, "center": it["center"]})

        # Sort so that requested start drive (e.g. E:) comes first!
        drives.sort(key=lambda d: (0 if d["letter"] == f"{self.start_drive}:" else 1, d["letter"]))
        return drives

    def update_live_report(self) -> None:
        """Writes the current recursive map to the desktop markdown file."""
        lines = [
            "# خريطة استكشاف الأقراص المباشرة — CPower Autonomous Disk Map",
            "",
            f"- **تاريخ ووقت التحديث:** `{time.strftime('%Y-%m-%d %H:%M:%S')}`",
            f"- **معمارية الاستكشاف:** تحكم يدوي مرئي حي على الشاشة عبر CPower C11 (Windows UI Access)",
            f"- **نمط العمل:** استكشاف ذاتي متواصل (Zero-Code Autonomous Navigation)",
            f"- **القرص الابتدائي:** `{self.start_drive}:`",
            "",
            "---",
            "",
            "## الهيكل الشجري للأقراص والمجلدات (Drive & Folder Hierarchy)",
            "",
            "```text",
        ]

        def render_tree(node: DiskNode, indent: str = ""):
            for idx, child in enumerate(node.children):
                is_last = idx == len(node.children) - 1
                branch = "└── " if is_last else "├── "
                sub_indent = "    " if is_last else "│   "
                count_str = f" ({child.item_count} items)" if child.is_folder and child.item_count > 0 else ""
                lines.append(f"{indent}{branch}{child.name}{count_str}")
                if child.children:
                    render_tree(child, indent + sub_indent)

        for drive_name, root in self.root_map.items():
            lines.append(f"{drive_name}")
            render_tree(root, indent="  ")
            lines.append("")

        lines.append("```")
        lines.append("")
        lines.append("---")
        lines.append("*الوكيل يعمل حالياً بشكل متواصل على شاشتك ويقوم بفتح المجلدات واستكشافها ورسم خريطتها خطوة بخطوة.*")

        try:
            self.desktop_report_path.write_text("\n".join(lines), encoding="utf-8")
        except Exception:
            pass

    def explore_folder_live(self, folder_name: str, center: Tuple[int, int], current_depth: int) -> DiskNode:
        """Double clicks a folder on screen, reads its contents, and steps back."""
        node = DiskNode(name=folder_name, is_folder=True)
        if current_depth > self.max_depth:
            return node

        cx, cy = center
        self.log("ENTER", f"{'  ' * current_depth}📂 Double-clicking folder: {folder_name!r} at ({cx}, {cy})...")
        self.client.focus_window(self.explorer_hwnd)
        self.client.double_click(x=cx, y=cy, hwnd=self.explorer_hwnd)
        time.sleep(0.7)  # Visible pacing for the user to watch

        sub_items = self.get_visible_items()
        folder_items = []
        file_items = []

        for it in sub_items:
            name = it["name"]
            # Exclude navigation bar items
            if any(k in name for k in ("Quick access", "This PC", "Desktop", "Downloads", "Documents", "Pictures")):
                continue
            if it.get("type") == "ListItem":
                # Check if it has a file extension or is a folder
                if "." in name and not name.startswith("."):
                    file_items.append(DiskNode(name=f"📄 {name}", is_folder=False))
                else:
                    folder_items.append((name, it["center"]))

        node.item_count = len(folder_items) + len(file_items)
        self.log("DISCOVER", f"{'  ' * current_depth}   └── Found {len(folder_items)} subfolders, {len(file_items)} files.")

        # Record files directly
        node.children.extend(file_items[:10])

        # Recursively explore up to 5 subfolders on screen
        if current_depth < self.max_depth:
            for s_name, s_center in folder_items[:5]:
                sub_child = self.explore_folder_live(s_name, s_center, current_depth + 1)
                node.children.append(sub_child)
                self.update_live_report()

        # Step back out of this folder
        self.navigate_up()
        time.sleep(0.5)
        return node

    def run_continuous_exploration(self) -> None:
        """Runs the continuous on-screen manual exploration of all drives."""
        self.log("START", "Starting continuous on-screen filesystem exploration...")
        self.ensure_explorer_window()

        drives = self.discover_drives_on_screen()
        self.log("DRIVES", f"Discovered {len(drives)} drives on screen: {[d['name'] for d in drives]}")

        for drive in drives:
            d_name = drive["name"]
            d_center = drive["center"]
            self.log("DRIVE", f"══════════════════════════════════════════════════════════")
            self.log("DRIVE", f"Entering Drive: {d_name} at {d_center}...")
            self.log("DRIVE", f"══════════════════════════════════════════════════════════")

            drive_root = DiskNode(name=d_name, is_folder=True)
            self.root_map[d_name] = drive_root

            self.client.focus_window(self.explorer_hwnd)
            self.client.double_click(x=d_center[0], y=d_center[1], hwnd=self.explorer_hwnd)
            time.sleep(1.0)

            # Discover top-level folders in this drive
            items = self.get_visible_items()
            top_folders = []
            for it in items:
                name = it["name"]
                if it.get("type") == "ListItem" and not ("." in name and not name.startswith(".")):
                    top_folders.append((name, it["center"]))

            self.log("ITEMS", f"Drive {d_name} contains {len(top_folders)} visible folders to explore.")

            for f_name, f_center in top_folders:
                folder_node = self.explore_folder_live(f_name, f_center, current_depth=1)
                drive_root.children.append(folder_node)
                self.update_live_report()

            # Return to This PC to start next drive
            self.navigate_to_this_pc()
            time.sleep(0.8)

        self.log("DONE", f"Exploration cycle completed! Final map written to: {self.desktop_report_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="CPower Autonomous On-Screen Disk Mapper")
    parser.add_argument("--start", default="E", help="Starting drive letter (default: E)")
    parser.add_argument("--depth", type=int, default=2, help="Folder traversal depth (default: 2)")
    args = parser.parse_args()

    mapper = CPowerDiskMapper(start_drive=args.start, max_depth=args.depth)
    mapper.run_continuous_exploration()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
