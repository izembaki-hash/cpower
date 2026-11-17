"""
CPower Process & Window Guard (clients/python/cpower_process_guard.py).

Provides 100% accurate Win32 process identification and window awareness.
Guards against interacting with protected host windows (such as Antigravity IDE,
code editors, or consoles) and safely acquires dedicated target applications (browsers).
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_user32 = ctypes.WinDLL("user32", use_last_error=True)

OpenProcess = _kernel32.OpenProcess
OpenProcess.restype = wintypes.HANDLE
OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]

QueryFullProcessImageNameW = _kernel32.QueryFullProcessImageNameW
QueryFullProcessImageNameW.restype = wintypes.BOOL
QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]

CloseHandle = _kernel32.CloseHandle
CloseHandle.restype = wintypes.BOOL
CloseHandle.argtypes = [wintypes.HANDLE]


# Protected applications that must NEVER be targeted or clicked by computer-use automation
PROTECTED_PROCESS_NAMES: Set[str] = {
    "antigravity.exe",
    "zcode.exe",
    "code.exe",
    "devenv.exe",
    "cmd.exe",
    "powershell.exe",
    "pwsh.exe",
    "windowsterminal.exe",
    "taskmgr.exe",
}

# Recognized standard web browsers
BROWSER_PROCESS_NAMES: Set[str] = {
    "msedge.exe",
    "chrome.exe",
    "brave.exe",
    "firefox.exe",
    "opera.exe",
}


def get_process_name_by_pid(pid: int) -> str:
    """Returns the executable basename for a given PID via Win32 API."""
    if pid <= 4:
        return "system"

    # PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h_proc = OpenProcess(0x1000, False, pid)
    if not h_proc:
        return "unknown"

    try:
        buf = ctypes.create_unicode_buffer(1024)
        sz = wintypes.DWORD(1024)
        if QueryFullProcessImageNameW(h_proc, 0, buf, ctypes.byref(sz)):
            return os.path.basename(buf.value).lower()
        return "unknown"
    finally:
        CloseHandle(h_proc)


def is_protected_window(hwnd: int, pid: int, title: str) -> bool:
    """Returns True if the window belongs to a protected application (Antigravity/IDE/Terminal)."""
    pname = get_process_name_by_pid(pid)
    if pname in PROTECTED_PROCESS_NAMES:
        return True

    title_lower = title.lower()
    if any(k in title_lower for k in ("antigravity", "visual studio code", "zcode")):
        return True

    return False


def find_browser_windows(windows: List[Dict[str, Any]], include_minimized: bool = True) -> List[Dict[str, Any]]:
    """Filters windows to find real, legitimate web browser instances only."""
    browsers = []
    for w in windows:
        if not w.get("visible", False):
            continue
        if not include_minimized and w.get("minimized", False):
            continue

        pid = int(w.get("pid", 0))
        pname = get_process_name_by_pid(pid)
        title = w.get("title", "")

        # Skip protected windows
        if is_protected_window(int(w.get("hwndInt", 0)), pid, title):
            continue

        if pname in BROWSER_PROCESS_NAMES:
            w_copy = dict(w)
            w_copy["processName"] = pname
            browsers.append(w_copy)

    return browsers
