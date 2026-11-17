"""
CPower Python 3.12 Zero-Dependency SDK (clients/python/cpower_client.py).

Communicates directly with `cpower.exe daemon` (or `cpower_uiaccess.exe daemon`)
over lock-free Shared Memory (SPSC dual circular ring buffers) or message-mode
Named Pipes using only the Python standard library and Win32 ctypes.
"""

from __future__ import annotations

import collections
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import platform
import subprocess
import threading
import time
from typing import Any, Deque, Dict, List, Optional, Tuple, Union


# ---------------------------------------------------------------------------
# Protocol Constants & Magic Numbers (Mirrors include/ipc.h & include/perception.h)
# ---------------------------------------------------------------------------
CPOWER_SHM_MAGIC = 0x41475043  # "AGPC"
CPOWER_SHM_VERSION = 1
CPOWER_TREE_PACKET_MAGIC = 0x54524545  # "TREE"
CPOWER_CMD_PACKET_MAGIC = 0x434D4E44   # "CMND"
CPOWER_EVENT_PACKET_MAGIC = 0x45564E54  # "EVNT"
CPOWER_ACK_PACKET_MAGIC = 0x41434B50   # "ACKP"
CPOWER_DIFF_PACKET_MAGIC = 0x44494646  # "DIFF"
CPOWER_WINDOWS_PACKET_MAGIC = 0x574E4453  # "WNDS"

CPOWER_DEFAULT_SHM_NAME = "Local\\CPowerAgentIPC"
CPOWER_DEFAULT_PIPE_NAME = "\\\\.\\pipe\\cpower_ipc"

CPOWER_ACK_FLAG_NONE = 0
CPOWER_ACK_FLAG_HAS_TREE = 1 << 0
CPOWER_ACK_FLAG_HAS_DIFF = 1 << 1
CPOWER_ACK_FLAG_HAS_IMAGE = 1 << 2
CPOWER_ACK_FLAG_HAS_WINDOWS = 1 << 3

CPOWER_NODE_FLAG_ENABLED = 1 << 0
CPOWER_NODE_FLAG_OFFSCREEN = 1 << 1
CPOWER_NODE_FLAG_FOCUSED = 1 << 2
CPOWER_NODE_FLAG_KEYBOARD_FOCUSABLE = 1 << 3
CPOWER_NODE_FLAG_PASSWORD = 1 << 4

CPOWER_DIFF_CHANGE_BOUNDS = 1 << 0
CPOWER_DIFF_CHANGE_VALUE = 1 << 1
CPOWER_DIFF_CHANGE_FLAGS = 1 << 2
CPOWER_DIFF_CHANGE_NAME = 1 << 3

IPC_PKT_UNKNOWN = 0
IPC_PKT_TREE = 1
IPC_PKT_COMMAND = 2
IPC_PKT_EVENT = 3
IPC_PKT_ACK = 4
IPC_PKT_IMAGE = 5
IPC_PKT_TREE_DIFF = 6
IPC_PKT_WINDOWS = 7

CMD_NOP = 0
CMD_CLICK = 1
CMD_DOUBLE_CLICK = 2
CMD_RIGHT_CLICK = 3
CMD_MOUSE_DOWN = 4
CMD_MOUSE_UP = 5
CMD_MOUSE_MOVE = 6
CMD_DRAG = 7
CMD_KEY_DOWN = 8
CMD_KEY_UP = 9
CMD_TYPE_TEXT = 10
CMD_HOTKEY = 11
CMD_SCROLL = 12
CMD_UIA_INVOKE = 13
CMD_UIA_SET_VALUE = 14
CMD_UIA_TOGGLE = 15
CMD_UIA_SELECT = 16
CMD_WIN32_CLICK = 17
CMD_WIN32_SET_TEXT = 18
CMD_CAPTURE_MICRO_REGION = 19
CMD_CAPTURE_TREE = 20
CMD_SHUTDOWN = 21
CMD_FOCUS_WINDOW = 22
CMD_SHOW_WINDOW = 23
CMD_DIFF_TREE = 24
CMD_LIST_WINDOWS = 25
CMD_HSCROLL = 26

EXEC_MODE_DIRECT_INSTANT = 0
EXEC_MODE_HUMAN_EMULATION = 1
CPOWER_EXEC_MODE_MASK = 0x00FF
CPOWER_EXEC_FLAG_HAS_NODE_ID = 0x0100
CPOWER_EXEC_FLAG_HAS_COORDS = 0x0200

WND_RESTORE = 0
WND_MINIMIZE = 1
WND_MAXIMIZE = 2
WND_CLOSE = 3

KEY_MOD_NONE = 0
KEY_MOD_CTRL = 1 << 0
KEY_MOD_SHIFT = 1 << 1
KEY_MOD_ALT = 1 << 2
KEY_MOD_WIN = 1 << 3

INT_MIN = -2147483648

CONTROL_TYPE_NAMES: Dict[int, str] = {
    50000: "Button",
    50001: "Calendar",
    50002: "CheckBox",
    50003: "ComboBox",
    50004: "Edit",
    50005: "Hyperlink",
    50006: "Image",
    50007: "ListItem",
    50008: "List",
    50009: "Menu",
    50010: "MenuBar",
    50011: "MenuItem",
    50012: "ProgressBar",
    50013: "RadioButton",
    50014: "ScrollBar",
    50015: "Slider",
    50016: "Spinner",
    50017: "StatusBar",
    50018: "Tab",
    50019: "TabItem",
    50020: "Text",
    50021: "ToolBar",
    50022: "ToolTip",
    50023: "Tree",
    50024: "TreeItem",
    50025: "Custom",
    50026: "Group",
    50027: "Thumb",
    50028: "DataGrid",
    50029: "DataItem",
    50030: "Document",
    50031: "SplitButton",
    50032: "Window",
    50033: "Pane",
    50034: "Header",
    50035: "HeaderItem",
    50036: "Table",
    50037: "TitleBar",
    50038: "Separator",
    50039: "SemanticZoom",
    50040: "AppBar",
}

CONTROL_NAME_TO_ID: Dict[str, int] = {v.lower(): k for k, v in CONTROL_TYPE_NAMES.items()}


# ---------------------------------------------------------------------------
# Packed C ABI ctypes Structures (Verified 1:1 with _Static_assert in C headers)
# ---------------------------------------------------------------------------
class RECT(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("left", ctypes.c_int32),
        ("top", ctypes.c_int32),
        ("right", ctypes.c_int32),
        ("bottom", ctypes.c_int32),
    ]


class ShmRingBufferHeader(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("capacity", ctypes.c_uint32),
        ("mask", ctypes.c_uint32),
        ("pad0", ctypes.c_uint8 * 48),
        ("writeIdx", ctypes.c_uint32),
        ("pad1", ctypes.c_uint8 * 60),
        ("readIdx", ctypes.c_uint32),
        ("pad2", ctypes.c_uint8 * 60),
    ]


class IPC_FrameHeader(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("payloadSize", ctypes.c_uint32),
        ("packetType", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
    ]


class TreePacketHeader(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("packetMagic", ctypes.c_uint32),
        ("sequenceId", ctypes.c_uint32),
        ("timestampUs", ctypes.c_uint64),
        ("totalPayloadSize", ctypes.c_uint32),
        ("nodeCount", ctypes.c_uint32),
        ("rootNodeIndex", ctypes.c_uint32),
        ("stringTableBytes", ctypes.c_uint32),
    ]


class SemanticNode(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("parentId", ctypes.c_int32),
        ("depth", ctypes.c_uint16),
        ("controlTypeId", ctypes.c_uint16),
        ("bounds", RECT),
        ("flags", ctypes.c_uint32),
        ("name", ctypes.c_wchar * 128),
        ("className", ctypes.c_wchar * 64),
        ("automationId", ctypes.c_wchar * 64),
        ("value", ctypes.c_wchar * 128),
    ]


class CommandPacket(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("packetMagic", ctypes.c_uint32),
        ("commandId", ctypes.c_uint32),
        ("commandCode", ctypes.c_uint16),
        ("executionMode", ctypes.c_uint16),
        ("targetX", ctypes.c_int32),
        ("targetY", ctypes.c_int32),
        ("param1", ctypes.c_int32),
        ("param2", ctypes.c_int32),
        ("speedFactor", ctypes.c_float),
        ("jitterScale", ctypes.c_float),
        ("targetHwnd", ctypes.c_uint64),
        ("targetNodeId", ctypes.c_uint32),
        ("textPayloadBytes", ctypes.c_uint32),
        ("textPayload", ctypes.c_wchar * 256),
    ]


class AckPacket(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("commandId", ctypes.c_uint32),
        ("hresult", ctypes.c_int32),
        ("elapsedUs", ctypes.c_uint32),
        ("nodeCount", ctypes.c_uint32),
        ("diffCount", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
    ]


class EventPacket(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("event", ctypes.c_uint32),
        ("hwnd", ctypes.c_uint64),
        ("idObject", ctypes.c_int32),
        ("idChild", ctypes.c_int32),
        ("threadId", ctypes.c_uint32),
        ("timestamp", ctypes.c_uint32),
    ]


class DiffPacketHeader(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("packetMagic", ctypes.c_uint32),
        ("sequenceId", ctypes.c_uint32),
        ("prevTimestampUtc", ctypes.c_uint64),
        ("currTimestampUtc", ctypes.c_uint64),
        ("totalPayloadSize", ctypes.c_uint32),
        ("entryCount", ctypes.c_uint32),
        ("addedCount", ctypes.c_uint32),
        ("removedCount", ctypes.c_uint32),
        ("modifiedCount", ctypes.c_uint32),
    ]


class TreeDiffEntry(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("op", ctypes.c_uint32),
        ("nodeId", ctypes.c_uint32),
        ("prevNodeId", ctypes.c_uint32),
        ("changedMask", ctypes.c_uint32),
        ("fingerprint", ctypes.c_uint64),
        ("node", SemanticNode),
    ]


class WindowPacketHeader(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("packetMagic", ctypes.c_uint32),
        ("windowCount", ctypes.c_uint32),
        ("totalPayloadSize", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class WindowInfo(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("hwnd", ctypes.c_uint64),
        ("pid", ctypes.c_uint32),
        ("rect", RECT),
        ("isVisible", ctypes.c_uint32),
        ("isMinimized", ctypes.c_uint32),
        ("title", ctypes.c_wchar * 128),
        ("className", ctypes.c_wchar * 64),
    ]


EXPECTED_STRUCT_SIZES: Dict[str, int] = {
    "ShmRingBufferHeader": 192,
    "IPC_FrameHeader": 8,
    "TreePacketHeader": 32,
    "SemanticNode": 800,
    "CommandPacket": 564,
    "AckPacket": 28,
    "EventPacket": 32,
    "DiffPacketHeader": 44,
    "TreeDiffEntry": 824,
    "WindowPacketHeader": 16,
    "WindowInfo": 420,
}

for _cls in (
    ShmRingBufferHeader,
    IPC_FrameHeader,
    TreePacketHeader,
    SemanticNode,
    CommandPacket,
    AckPacket,
    EventPacket,
    DiffPacketHeader,
    TreeDiffEntry,
    WindowPacketHeader,
    WindowInfo,
):
    assert ctypes.sizeof(_cls) == EXPECTED_STRUCT_SIZES[_cls.__name__], (
        f"{_cls.__name__} size mismatch: {ctypes.sizeof(_cls)} != {EXPECTED_STRUCT_SIZES[_cls.__name__]}"
    )


# ---------------------------------------------------------------------------
# Win32 Kernel32 ctypes Bindings
# ---------------------------------------------------------------------------
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_FILE_MAP_ALL_ACCESS = 0x000F001F
_EVENT_MODIFY_STATE = 0x0002
_SYNCHRONIZE = 0x00100000
_WAIT_OBJECT_0 = 0x00000000
_WAIT_TIMEOUT = 0x00000102
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_OPEN_EXISTING = 3
_PIPE_READMODE_MESSAGE = 0x00000002
_ERROR_MORE_DATA = 234
_ERROR_PIPE_BUSY = 231


class _MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("PartitionId", wintypes.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


_kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_kernel32.OpenFileMappingW.restype = wintypes.HANDLE

_kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
_kernel32.MapViewOfFile.restype = ctypes.c_void_p

_kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_kernel32.UnmapViewOfFile.restype = wintypes.BOOL

_kernel32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.POINTER(_MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
_kernel32.VirtualQuery.restype = ctypes.c_size_t

_kernel32.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_kernel32.OpenEventW.restype = wintypes.HANDLE

_kernel32.SetEvent.argtypes = [wintypes.HANDLE]
_kernel32.SetEvent.restype = wintypes.BOOL

_kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_kernel32.WaitForSingleObject.restype = wintypes.DWORD

_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.restype = wintypes.BOOL

_kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
_kernel32.CreateFileW.restype = wintypes.HANDLE

_kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_kernel32.WaitNamedPipeW.restype = wintypes.BOOL

_kernel32.SetNamedPipeHandleState.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD)
]
_kernel32.SetNamedPipeHandleState.restype = wintypes.BOOL

_kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_kernel32.ReadFile.restype = wintypes.BOOL

_kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_kernel32.PeekNamedPipe.restype = wintypes.BOOL

_kernel32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_kernel32.WriteFile.restype = wintypes.BOOL

_IS_X86_ARCH = platform.machine().lower() in ("amd64", "x86_64", "x86", "i386", "i686")
_HAS_FLUSH_WRITE_BUFFERS = hasattr(_kernel32, "FlushProcessWriteBuffers")
if _HAS_FLUSH_WRITE_BUFFERS:
    _kernel32.FlushProcessWriteBuffers.argtypes = []
    _kernel32.FlushProcessWriteBuffers.restype = None


def _memory_barrier() -> None:
    """Explicit memory ordering helper for lock-free SPSC SHM ring cursors.

    Note on ctypes volatile semantics:
      Each `ctypes.Structure.from_address(...)` field read/write and `memmove`/
      `string_at` call crosses the CPython -> `_ctypes` C extension boundary,
      preventing register caching across Python statements (compiler barrier).
      On x86/x86-64 (TSO), Store-Store, Load-Load, and Load-Store ordering is
      guaranteed in hardware; on weak-memory architectures (e.g. ARM64), we
      invoke `FlushProcessWriteBuffers` for full cross-core visibility.
    """
    if not _IS_X86_ARCH and _HAS_FLUSH_WRITE_BUFFERS:
        _kernel32.FlushProcessWriteBuffers()


def _truncate_utf16le(text: str, max_bytes: int = 510) -> Tuple[str, bytes]:
    """Truncate a Python str so its UTF-16LE encoding is <= max_bytes without splitting surrogate pairs."""
    if not text or max_bytes <= 0:
        return "", b""
    even_max = max_bytes - (max_bytes % 2)
    raw = text.encode("utf-16-le", errors="replace")
    if len(raw) <= even_max:
        return text, raw
    raw = raw[:even_max]
    if len(raw) >= 2:
        last_unit = int.from_bytes(raw[-2:], "little")
        if 0xD800 <= last_unit <= 0xDBFF:
            raw = raw[:-2]
    trimmed = raw.decode("utf-16-le")
    return trimmed, raw


# ---------------------------------------------------------------------------
# Decoding Helpers
# ---------------------------------------------------------------------------
def _align64(val: int) -> int:
    return (val + 63) & ~63


def _node_to_dict(n: SemanticNode) -> Dict[str, Any]:
    l, t, r, b = int(n.bounds.left), int(n.bounds.top), int(n.bounds.right), int(n.bounds.bottom)
    return {
        "id": int(n.id),
        "parentId": int(n.parentId),
        "depth": int(n.depth),
        "controlTypeId": int(n.controlTypeId),
        "type": CONTROL_TYPE_NAMES.get(int(n.controlTypeId), f"ControlType({n.controlTypeId})"),
        "bounds": [l, t, r, b],
        "center": [l + int((r - l) / 2), t + int((b - t) / 2)],
        "flags": int(n.flags),
        "enabled": bool(n.flags & CPOWER_NODE_FLAG_ENABLED),
        "offscreen": bool(n.flags & CPOWER_NODE_FLAG_OFFSCREEN),
        "focused": bool(n.flags & CPOWER_NODE_FLAG_FOCUSED),
        "name": str(n.name),
        "className": str(n.className),
        "automationId": str(n.automationId),
        "value": str(n.value),
    }


def _decode_tree_payload(data: bytes) -> Dict[str, Any]:
    if len(data) < ctypes.sizeof(TreePacketHeader):
        raise ValueError(f"Tree packet too small: {len(data)} bytes")
    hdr = TreePacketHeader.from_buffer_copy(data[:ctypes.sizeof(TreePacketHeader)])
    if hdr.packetMagic != CPOWER_TREE_PACKET_MAGIC:
        raise ValueError(f"Invalid TreePacket magic: 0x{hdr.packetMagic:08X}")
    node_sz = ctypes.sizeof(SemanticNode)
    offset = ctypes.sizeof(TreePacketHeader)
    nodes: List[Dict[str, Any]] = []
    for i in range(hdr.nodeCount):
        end = offset + node_sz
        if end > len(data):
            break
        node = SemanticNode.from_buffer_copy(data[offset:end])
        nodes.append(_node_to_dict(node))
        offset = end
    return {
        "sequenceId": int(hdr.sequenceId),
        "timestampUs": int(hdr.timestampUs),
        "nodeCount": int(hdr.nodeCount),
        "rootNodeIndex": int(hdr.rootNodeIndex),
        "nodes": nodes,
    }


def _decode_diff_payload(data: bytes) -> Dict[str, Any]:
    if len(data) < ctypes.sizeof(DiffPacketHeader):
        raise ValueError(f"Diff packet too small: {len(data)} bytes")
    hdr = DiffPacketHeader.from_buffer_copy(data[:ctypes.sizeof(DiffPacketHeader)])
    if hdr.packetMagic != CPOWER_DIFF_PACKET_MAGIC:
        raise ValueError(f"Invalid DiffPacket magic: 0x{hdr.packetMagic:08X}")
    op_names = {1: "added", 2: "removed", 3: "modified"}
    entry_sz = ctypes.sizeof(TreeDiffEntry)
    offset = ctypes.sizeof(DiffPacketHeader)
    entries: List[Dict[str, Any]] = []
    for _ in range(hdr.entryCount):
        end = offset + entry_sz
        if end > len(data):
            break
        ent = TreeDiffEntry.from_buffer_copy(data[offset:end])
        mask = int(ent.changedMask)
        changed = []
        if mask & CPOWER_DIFF_CHANGE_BOUNDS:
            changed.append("bounds")
        if mask & CPOWER_DIFF_CHANGE_VALUE:
            changed.append("value")
        if mask & CPOWER_DIFF_CHANGE_FLAGS:
            changed.append("flags")
        if mask & CPOWER_DIFF_CHANGE_NAME:
            changed.append("name")
        entries.append({
            "op": op_names.get(int(ent.op), "unknown"),
            "nodeId": int(ent.nodeId),
            "prevNodeId": None if ent.prevNodeId == 0xFFFFFFFF else int(ent.prevNodeId),
            "changedMask": mask,
            "changed": changed,
            "fingerprint": f"0x{int(ent.fingerprint):016X}",
            "node": _node_to_dict(ent.node),
        })
        offset = end
    return {
        "sequenceId": int(hdr.sequenceId),
        "prevTimestampUtc": int(hdr.prevTimestampUtc),
        "currTimestampUtc": int(hdr.currTimestampUtc),
        "entryCount": int(hdr.entryCount),
        "addedCount": int(hdr.addedCount),
        "removedCount": int(hdr.removedCount),
        "modifiedCount": int(hdr.modifiedCount),
        "entries": entries,
    }


def _decode_windows_payload(data: bytes) -> List[Dict[str, Any]]:
    if len(data) < ctypes.sizeof(WindowPacketHeader):
        raise ValueError(f"Window packet too small: {len(data)} bytes")
    hdr = WindowPacketHeader.from_buffer_copy(data[:ctypes.sizeof(WindowPacketHeader)])
    if hdr.packetMagic != CPOWER_WINDOWS_PACKET_MAGIC:
        raise ValueError(f"Invalid WindowPacket magic: 0x{hdr.packetMagic:08X}")
    win_sz = ctypes.sizeof(WindowInfo)
    offset = ctypes.sizeof(WindowPacketHeader)
    windows: List[Dict[str, Any]] = []
    for _ in range(hdr.windowCount):
        end = offset + win_sz
        if end > len(data):
            break
        w = WindowInfo.from_buffer_copy(data[offset:end])
        windows.append({
            "hwnd": f"0x{int(w.hwnd):X}",
            "hwndInt": int(w.hwnd),
            "pid": int(w.pid),
            "rect": [int(w.rect.left), int(w.rect.top), int(w.rect.right), int(w.rect.bottom)],
            "visible": bool(w.isVisible),
            "minimized": bool(w.isMinimized),
            "title": str(w.title),
            "className": str(w.className),
        })
        offset = end
    return windows


def _ack_to_dict(ack: AckPacket) -> Dict[str, Any]:
    hr_u32 = int(ack.hresult) & 0xFFFFFFFF
    return {
        "commandId": int(ack.commandId),
        "hresult": int(ack.hresult),
        "hresultHex": f"0x{hr_u32:08X}",
        "succeeded": int(ack.hresult) >= 0,
        "elapsedUs": int(ack.elapsedUs),
        "nodeCount": int(ack.nodeCount),
        "diffCount": int(ack.diffCount),
        "flags": int(ack.flags),
    }


def _event_to_dict(evt: EventPacket) -> Dict[str, Any]:
    return {
        "event": int(evt.event),
        "hwnd": f"0x{int(evt.hwnd):X}",
        "hwndInt": int(evt.hwnd),
        "idObject": int(evt.idObject),
        "idChild": int(evt.idChild),
        "threadId": int(evt.threadId),
        "timestamp": int(evt.timestamp),
    }


def _parse_hwnd(hwnd: Union[int, str, None]) -> int:
    if hwnd is None:
        return 0
    if isinstance(hwnd, int):
        return hwnd
    s = str(hwnd).strip()
    if not s:
        return 0
    return int(s, 0)


# ---------------------------------------------------------------------------
# Client Implementation
# ---------------------------------------------------------------------------
class CPowerError(RuntimeError):
    """Raised when IPC communication or command execution fails."""


class CPowerClient:
    """High-level zero-dependency client for the cpower native Windows daemon."""

    def __init__(
        self,
        transport: str = "shm",
        name: Optional[str] = None,
        exe_path: Optional[Union[str, Path]] = None,
        auto_spawn: bool = True,
        stream_events: bool = False,
        timeout_ms: int = 8000,
    ) -> None:
        self._closed = False
        self._proc: Optional[subprocess.Popen[bytes]] = None
        self._stderr_lines: Deque[str] = collections.deque(maxlen=64)
        self._stderr_thread: Optional[threading.Thread] = None
        self._h_map: Optional[int] = None
        self._base_ptr: Optional[int] = None
        self._out_ring: Optional[ShmRingBufferHeader] = None
        self._out_data_addr: int = 0
        self._in_ring: Optional[ShmRingBufferHeader] = None
        self._in_data_addr: int = 0
        self._ring_cap: int = 0
        self._ring_mask: int = 0
        self._h_out_evt: Optional[int] = None
        self._h_in_evt: Optional[int] = None
        self._h_pipe: Optional[int] = None

        self.transport = transport.lower()
        if self.transport not in ("shm", "pipe", "auto"):
            self._closed = True
            raise ValueError("transport must be 'shm', 'pipe', or 'auto'")

        self._custom_name = name
        self.channel_name = name or (
            CPOWER_DEFAULT_SHM_NAME if self.transport != "pipe" else CPOWER_DEFAULT_PIPE_NAME
        )
        self.timeout_ms = int(timeout_ms)
        self.stream_events = bool(stream_events)
        self._next_cmd_id = 1
        self.events: Deque[Dict[str, Any]] = collections.deque(maxlen=1024)
        self.last_tree: Optional[Dict[str, Any]] = None

        if exe_path is not None:
            self._exe_path = Path(exe_path)
        else:
            repo_root = Path(__file__).resolve().parents[2]
            self._exe_path = repo_root / "bin" / "cpower.exe"

        self._connect(auto_spawn=auto_spawn)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "CPowerClient":
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close()

    def _try_open_shm(self) -> bool:
        h_map = _kernel32.OpenFileMappingW(_FILE_MAP_ALL_ACCESS, False, self.channel_name)
        if not h_map:
            return False
        base = _kernel32.MapViewOfFile(h_map, _FILE_MAP_ALL_ACCESS, 0, 0, 0)
        if not base:
            _kernel32.CloseHandle(h_map)
            return False

        mbi = _MEMORY_BASIC_INFORMATION()
        hdr_sz = ctypes.sizeof(ShmRingBufferHeader)
        if _kernel32.VirtualQuery(base, ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0 or mbi.RegionSize < hdr_sz:
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        _memory_barrier()
        out_ring = ShmRingBufferHeader.from_address(base)
        if out_ring.magic != CPOWER_SHM_MAGIC or out_ring.version != CPOWER_SHM_VERSION:
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        cap = int(out_ring.capacity)
        if cap == 0 or (cap & (cap - 1)) != 0 or out_ring.mask != cap - 1:
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        in_offset = _align64(hdr_sz + cap)
        total_sz = in_offset + hdr_sz + cap
        if mbi.RegionSize < total_sz:
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        in_ring = ShmRingBufferHeader.from_address(base + in_offset)
        if in_ring.magic != CPOWER_SHM_MAGIC or in_ring.capacity != cap:
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        h_out_evt = _kernel32.OpenEventW(_EVENT_MODIFY_STATE | _SYNCHRONIZE, False, self.channel_name + "_OutEvt")
        h_in_evt = _kernel32.OpenEventW(_EVENT_MODIFY_STATE | _SYNCHRONIZE, False, self.channel_name + "_InEvt")
        if not h_out_evt or not h_in_evt:
            if h_out_evt:
                _kernel32.CloseHandle(h_out_evt)
            if h_in_evt:
                _kernel32.CloseHandle(h_in_evt)
            _kernel32.UnmapViewOfFile(base)
            _kernel32.CloseHandle(h_map)
            return False

        self._h_map = h_map
        self._base_ptr = base
        self._out_ring = out_ring
        self._out_data_addr = base + hdr_sz
        self._in_ring = in_ring
        self._in_data_addr = base + in_offset + hdr_sz
        self._ring_cap = cap
        self._ring_mask = cap - 1
        self._h_out_evt = h_out_evt
        self._h_in_evt = h_in_evt
        return True

    def _try_open_pipe(self) -> bool:
        handle = _kernel32.CreateFileW(
            self.channel_name,
            _GENERIC_READ | _GENERIC_WRITE,
            0,
            None,
            _OPEN_EXISTING,
            0,
            None,
        )
        if handle == _INVALID_HANDLE_VALUE or not handle:
            err = ctypes.get_last_error()
            if err == _ERROR_PIPE_BUSY:
                wait_ms = min(max(1, self.timeout_ms), 1000)
                if _kernel32.WaitNamedPipeW(self.channel_name, wait_ms):
                    handle = _kernel32.CreateFileW(
                        self.channel_name,
                        _GENERIC_READ | _GENERIC_WRITE,
                        0,
                        None,
                        _OPEN_EXISTING,
                        0,
                        None,
                    )
            if handle == _INVALID_HANDLE_VALUE or not handle:
                return False

        mode = wintypes.DWORD(_PIPE_READMODE_MESSAGE)
        _kernel32.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None)
        self._h_pipe = handle
        return True

    def _drain_stderr(self, proc: subprocess.Popen[bytes]) -> None:
        stream = proc.stderr
        if stream is None:
            return
        try:
            for raw_line in iter(stream.readline, b""):
                text = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
                self._stderr_lines.append(text)
        except Exception:
            pass

    def _connect(self, auto_spawn: bool) -> None:
        if self.transport == "auto":
            # 1. Try attaching to existing SHM daemon
            self.channel_name = self._custom_name or CPOWER_DEFAULT_SHM_NAME
            if self._try_open_shm():
                self.transport = "shm"
                return
            # 2. Try attaching to existing Named Pipe daemon
            self.channel_name = self._custom_name or CPOWER_DEFAULT_PIPE_NAME
            if self._try_open_pipe():
                self.transport = "pipe"
                return
            # 3. Default to SHM for auto-spawn
            self.transport = "shm"
            self.channel_name = self._custom_name or CPOWER_DEFAULT_SHM_NAME

        opened = self._try_open_shm() if self.transport == "shm" else self._try_open_pipe()
        if opened:
            return

        if not auto_spawn:
            self.close()
            raise CPowerError(f"Could not attach to cpower daemon ({self.transport}: {self.channel_name})")

        if not self._exe_path.is_file():
            self.close()
            raise CPowerError(f"cpower executable not found at {self._exe_path}")

        args = [str(self._exe_path), "daemon", "--transport", self.transport, "--name", self.channel_name]
        if self.stream_events:
            args.append("--stream-events")

        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self._proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
        )
        self._stderr_thread = threading.Thread(
            target=self._drain_stderr,
            args=(self._proc,),
            name="cpower-stderr-drain",
            daemon=True,
        )
        self._stderr_thread.start()

        deadline = time.monotonic() + (self.timeout_ms / 1000.0)
        while time.monotonic() < deadline:
            if self._proc.poll() is not None:
                rc = self._proc.returncode
                if self._stderr_thread is not None:
                    self._stderr_thread.join(timeout=0.5)
                stderr_txt = "\n".join(self._stderr_lines)
                self.close()
                raise CPowerError(f"cpower daemon exited early ({rc}): {stderr_txt}")
            opened = self._try_open_shm() if self.transport == "shm" else self._try_open_pipe()
            if opened:
                return
            time.sleep(0.015)

        stderr_txt = "\n".join(self._stderr_lines)
        self.close()
        suffix = f" ({stderr_txt})" if stderr_txt else ""
        raise CPowerError(f"Timed out waiting for cpower daemon ({self.transport}: {self.channel_name}){suffix}")

    # -----------------------------------------------------------------------
    # Low-Level Ring & Pipe Frame Transport
    # -----------------------------------------------------------------------
    def _shm_write_bytes(self, start_idx: int, data: bytes) -> None:
        if not data:
            return
        cap = self._ring_cap
        offset = start_idx & self._ring_mask
        first = cap - offset
        if first >= len(data):
            ctypes.memmove(self._in_data_addr + offset, data, len(data))
        else:
            ctypes.memmove(self._in_data_addr + offset, data[:first], first)
            ctypes.memmove(self._in_data_addr, data[first:], len(data) - first)

    def _shm_read_bytes(self, start_idx: int, length: int) -> bytes:
        if length <= 0:
            return b""
        cap = self._ring_cap
        offset = start_idx & self._ring_mask
        first = cap - offset
        if first >= length:
            return ctypes.string_at(self._out_data_addr + offset, length)
        part1 = ctypes.string_at(self._out_data_addr + offset, first)
        part2 = ctypes.string_at(self._out_data_addr, length - first)
        return part1 + part2

    def _shm_send_command(self, cmd: CommandPacket) -> None:
        assert self._in_ring is not None and self._h_in_evt is not None
        payload = bytes(cmd)
        hdr_sz = ctypes.sizeof(IPC_FrameHeader)
        if self._ring_cap <= hdr_sz or len(payload) > (self._ring_cap - hdr_sz):
            raise CPowerError(
                f"Serialized command ({len(payload)} bytes) exceeds SHM ring capacity ({max(0, self._ring_cap - hdr_sz)} bytes)"
            )
        hdr = IPC_FrameHeader(payloadSize=len(payload), packetType=IPC_PKT_COMMAND, flags=0)
        total = hdr_sz + len(payload)

        deadline = time.monotonic() + (self.timeout_ms / 1000.0)
        while True:
            w_idx = int(self._in_ring.writeIdx)
            r_idx = int(self._in_ring.readIdx)
            _memory_barrier()
            used = (w_idx - r_idx) & 0xFFFFFFFF
            if used > self._ring_cap:
                raise CPowerError("Corrupted SHM InRing cursor state")
            if (self._ring_cap - used) >= total:
                self._shm_write_bytes(w_idx, bytes(hdr))
                self._shm_write_bytes(w_idx + hdr_sz, payload)
                _memory_barrier()
                self._in_ring.writeIdx = (w_idx + total) & 0xFFFFFFFF
                _memory_barrier()
                _kernel32.SetEvent(self._h_in_evt)
                return
            if time.monotonic() >= deadline:
                raise CPowerError("Timed out waiting for SHM InRing capacity")
            time.sleep(0.002)

    def _shm_read_frame(self, timeout_ms: int) -> Tuple[int, bytes]:
        assert self._out_ring is not None and self._h_out_evt is not None
        hdr_sz = ctypes.sizeof(IPC_FrameHeader)
        deadline = time.monotonic() + (timeout_ms / 1000.0)

        while True:
            w_idx = int(self._out_ring.writeIdx)
            r_idx = int(self._out_ring.readIdx)
            _memory_barrier()
            avail = (w_idx - r_idx) & 0xFFFFFFFF
            if avail > self._ring_cap:
                raise CPowerError("Corrupted SHM OutRing cursor state")
            if avail >= hdr_sz:
                hdr_bytes = self._shm_read_bytes(r_idx, hdr_sz)
                hdr = IPC_FrameHeader.from_buffer_copy(hdr_bytes)
                total = hdr_sz + int(hdr.payloadSize)
                if total > self._ring_cap:
                    raise CPowerError(f"Corrupted SHM frame payloadSize: {hdr.payloadSize}")
                if avail >= total:
                    payload = self._shm_read_bytes(r_idx + hdr_sz, int(hdr.payloadSize))
                    _memory_barrier()
                    self._out_ring.readIdx = (r_idx + total) & 0xFFFFFFFF
                    return int(hdr.packetType), payload

            rem_ms = int((deadline - time.monotonic()) * 1000.0)
            if rem_ms <= 0:
                raise CPowerError("Timed out reading frame from SHM OutRing")
            _kernel32.WaitForSingleObject(self._h_out_evt, min(rem_ms, 50))

    def _pipe_write_msg(self, data: bytes) -> None:
        assert self._h_pipe is not None
        written = wintypes.DWORD(0)
        ok = _kernel32.WriteFile(self._h_pipe, data, len(data), ctypes.byref(written), None)
        if not ok or written.value != len(data):
            err = ctypes.get_last_error()
            raise CPowerError(f"Named Pipe write failed (winerr={err})")

    def _pipe_read_msg(self, timeout_ms: Optional[int] = None) -> bytes:
        assert self._h_pipe is not None
        t_ms = self.timeout_ms if timeout_ms is None else int(timeout_ms)
        deadline = time.monotonic() + (max(0, t_ms) / 1000.0)
        avail = wintypes.DWORD(0)
        while True:
            ok = _kernel32.PeekNamedPipe(
                self._h_pipe, None, 0, None, ctypes.byref(avail), None
            )
            if not ok:
                err = ctypes.get_last_error()
                raise CPowerError(f"Named Pipe peek failed (winerr={err})")
            if avail.value > 0:
                break
            if time.monotonic() >= deadline:
                raise CPowerError(f"Timed out reading from Named Pipe after {t_ms} ms")
            time.sleep(0.002)

        chunks: List[bytes] = []
        buf = (ctypes.c_ubyte * 65536)()
        while True:
            n_read = wintypes.DWORD(0)
            ok = _kernel32.ReadFile(self._h_pipe, buf, 65536, ctypes.byref(n_read), None)
            chunks.append(bytes(buf[:n_read.value]))
            if ok:
                return b"".join(chunks)
            err = ctypes.get_last_error()
            if err == _ERROR_MORE_DATA:
                continue
            raise CPowerError(f"Named Pipe read failed (winerr={err})")

    def _pipe_read_frame(self, timeout_ms: Optional[int] = None) -> Tuple[int, bytes]:
        raw = self._pipe_read_msg(timeout_ms=timeout_ms)
        hdr_sz = ctypes.sizeof(IPC_FrameHeader)
        if len(raw) < hdr_sz:
            raise CPowerError(f"Named Pipe frame too small: {len(raw)} bytes")
        hdr = IPC_FrameHeader.from_buffer_copy(raw[:hdr_sz])
        expected_total = hdr_sz + int(hdr.payloadSize)
        if len(raw) < expected_total:
            raise CPowerError(
                f"Truncated Named Pipe frame: expected {expected_total} bytes, got {len(raw)}"
            )
        return int(hdr.packetType), raw[hdr_sz:expected_total]

    def _build_command_packet(
        self,
        code: int,
        mode: int = EXEC_MODE_HUMAN_EMULATION,
        x: Optional[int] = None,
        y: Optional[int] = None,
        param1: int = 0,
        param2: int = 0,
        speed: float = 1.0,
        jitter: float = 1.0,
        hwnd: Union[int, str, None] = 0,
        node_id: Optional[int] = None,
        text: Optional[str] = None,
        default_x: int = 0,
        default_y: int = 0,
    ) -> CommandPacket:
        cmd = CommandPacket()
        cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC
        cmd.commandId = self._next_cmd_id
        self._next_cmd_id += 1
        cmd.commandCode = code

        exec_mode = int(mode) & CPOWER_EXEC_MODE_MASK
        if node_id is not None:
            exec_mode |= CPOWER_EXEC_FLAG_HAS_NODE_ID
        if x is not None or y is not None:
            exec_mode |= CPOWER_EXEC_FLAG_HAS_COORDS
        cmd.executionMode = exec_mode

        cmd.targetX = default_x if x is None else int(x)
        cmd.targetY = default_y if y is None else int(y)
        cmd.param1 = int(param1)
        cmd.param2 = int(param2)
        cmd.speedFactor = float(speed)
        cmd.jitterScale = float(jitter)
        cmd.targetHwnd = _parse_hwnd(hwnd)
        cmd.targetNodeId = 0 if node_id is None else int(node_id)

        if text:
            trimmed, utf16_bytes = _truncate_utf16le(text, max_bytes=510)
            cmd.textPayload = trimmed
            cmd.textPayloadBytes = len(utf16_bytes)
        else:
            cmd.textPayloadBytes = 0
        return cmd

    def _execute(
        self,
        code: int,
        mode: int = EXEC_MODE_HUMAN_EMULATION,
        x: Optional[int] = None,
        y: Optional[int] = None,
        param1: int = 0,
        param2: int = 0,
        speed: float = 1.0,
        jitter: float = 1.0,
        hwnd: Union[int, str, None] = 0,
        node_id: Optional[int] = None,
        text: Optional[str] = None,
        default_x: int = 0,
        default_y: int = 0,
    ) -> Tuple[Dict[str, Any], Optional[int], Optional[bytes]]:
        if self._closed:
            raise CPowerError("Client is closed")

        cmd = self._build_command_packet(
            code=code,
            mode=mode,
            x=x,
            y=y,
            param1=param1,
            param2=param2,
            speed=speed,
            jitter=jitter,
            hwnd=hwnd,
            node_id=node_id,
            text=text,
            default_x=default_x,
            default_y=default_y,
        )

        if self.transport == "shm":
            self._shm_send_command(cmd)
            read_frame = self._shm_read_frame
        else:
            self._pipe_write_msg(bytes(cmd))
            read_frame = self._pipe_read_frame

        while True:
            pkt_type, payload = read_frame(self.timeout_ms)
            if pkt_type == IPC_PKT_EVENT and len(payload) >= ctypes.sizeof(EventPacket):
                evt = EventPacket.from_buffer_copy(payload[:ctypes.sizeof(EventPacket)])
                if evt.magic == CPOWER_EVENT_PACKET_MAGIC:
                    self.events.append(_event_to_dict(evt))
                continue
            if pkt_type == IPC_PKT_ACK and len(payload) >= ctypes.sizeof(AckPacket):
                ack = AckPacket.from_buffer_copy(payload[:ctypes.sizeof(AckPacket)])
                if ack.magic == CPOWER_ACK_PACKET_MAGIC and ack.commandId == cmd.commandId:
                    ack_dict = _ack_to_dict(ack)
                    if ack.flags == 0:
                        return ack_dict, None, None
                    while True:
                        p_type, p_data = read_frame(self.timeout_ms)
                        if p_type == IPC_PKT_EVENT and len(p_data) >= ctypes.sizeof(EventPacket):
                            evt = EventPacket.from_buffer_copy(p_data[:ctypes.sizeof(EventPacket)])
                            if evt.magic == CPOWER_EVENT_PACKET_MAGIC:
                                self.events.append(_event_to_dict(evt))
                            continue
                        return ack_dict, p_type, p_data

    # -----------------------------------------------------------------------
    # High-Level Agent API
    # -----------------------------------------------------------------------
    def poll_events(self) -> List[Dict[str, Any]]:
        """Return and clear any buffered WinEvent notifications."""
        if self.transport == "shm" and self._out_ring is not None:
            hdr_sz = ctypes.sizeof(IPC_FrameHeader)
            while True:
                w_idx = int(self._out_ring.writeIdx)
                r_idx = int(self._out_ring.readIdx)
                _memory_barrier()
                avail = (w_idx - r_idx) & 0xFFFFFFFF
                if avail < hdr_sz or avail > self._ring_cap:
                    break
                hdr = IPC_FrameHeader.from_buffer_copy(self._shm_read_bytes(r_idx, hdr_sz))
                total = hdr_sz + int(hdr.payloadSize)
                if total > self._ring_cap or avail < total:
                    break
                payload = self._shm_read_bytes(r_idx + hdr_sz, int(hdr.payloadSize))
                _memory_barrier()
                self._out_ring.readIdx = (r_idx + total) & 0xFFFFFFFF
                if int(hdr.packetType) == IPC_PKT_EVENT and len(payload) >= ctypes.sizeof(EventPacket):
                    evt = EventPacket.from_buffer_copy(payload[:ctypes.sizeof(EventPacket)])
                    if evt.magic == CPOWER_EVENT_PACKET_MAGIC:
                        self.events.append(_event_to_dict(evt))
        elif self.transport == "pipe" and self._h_pipe is not None:
            avail = wintypes.DWORD(0)
            while _kernel32.PeekNamedPipe(self._h_pipe, None, 0, None, ctypes.byref(avail), None) and avail.value > 0:
                pkt_type, payload = self._pipe_read_frame(timeout_ms=0)
                if pkt_type == IPC_PKT_EVENT and len(payload) >= ctypes.sizeof(EventPacket):
                    evt = EventPacket.from_buffer_copy(payload[:ctypes.sizeof(EventPacket)])
                    if evt.magic == CPOWER_EVENT_PACKET_MAGIC:
                        self.events.append(_event_to_dict(evt))
        out = list(self.events)
        self.events.clear()
        return out

    def list_windows(self) -> List[Dict[str, Any]]:
        ack, pkt_type, payload = self._execute(CMD_LIST_WINDOWS)
        if not ack["succeeded"]:
            raise CPowerError(f"CMD_LIST_WINDOWS failed: {ack['hresultHex']}")
        if pkt_type != IPC_PKT_WINDOWS or payload is None:
            return []
        return _decode_windows_payload(payload)

    def focus_window(self, hwnd: Union[int, str]) -> Dict[str, Any]:
        ack, _, _ = self._execute(CMD_FOCUS_WINDOW, hwnd=hwnd)
        return ack

    def show_window(self, hwnd: Union[int, str], action: Union[int, str] = WND_RESTORE) -> Dict[str, Any]:
        act_map = {
            "restore": WND_RESTORE,
            "minimize": WND_MINIMIZE,
            "maximize": WND_MAXIMIZE,
            "close": WND_CLOSE,
        }
        act_code = act_map[action.lower()] if isinstance(action, str) else int(action)
        ack, _, _ = self._execute(CMD_SHOW_WINDOW, hwnd=hwnd, param1=act_code)
        return ack

    def capture_tree(self, hwnd: Union[int, str, None] = 0, desktop: bool = False) -> Dict[str, Any]:
        ack, pkt_type, payload = self._execute(CMD_CAPTURE_TREE, hwnd=hwnd, param1=1 if desktop else 0)
        if not ack["succeeded"]:
            raise CPowerError(f"CMD_CAPTURE_TREE failed: {ack['hresultHex']}")
        if pkt_type != IPC_PKT_TREE or payload is None:
            raise CPowerError("Expected TREE payload from daemon")
        tree = _decode_tree_payload(payload)
        tree["ack"] = ack
        self.last_tree = tree
        return tree

    def diff_tree(self, hwnd: Union[int, str, None] = 0, desktop: bool = False) -> Dict[str, Any]:
        ack, pkt_type, payload = self._execute(CMD_DIFF_TREE, hwnd=hwnd, param1=1 if desktop else 0)
        if not ack["succeeded"]:
            raise CPowerError(f"CMD_DIFF_TREE failed: {ack['hresultHex']}")
        if pkt_type != IPC_PKT_TREE_DIFF or payload is None:
            raise CPowerError("Expected DIFF payload from daemon")
        diff = _decode_diff_payload(payload)
        diff["ack"] = ack
        return diff

    def find_nodes(
        self,
        name: Optional[str] = None,
        automation_id: Optional[str] = None,
        class_name: Optional[str] = None,
        control_type: Union[int, str, None] = None,
        only_enabled: bool = False,
        only_visible: bool = False,
        hwnd: Union[int, str, None] = 0,
        desktop: bool = False,
        tree: Optional[Dict[str, Any]] = None,
    ) -> List[Dict[str, Any]]:
        src_tree = tree if tree is not None else self.capture_tree(hwnd=hwnd, desktop=desktop)
        target_type_id: Optional[int] = None
        if control_type is not None:
            if isinstance(control_type, int):
                target_type_id = control_type
            else:
                ct_str = str(control_type).strip()
                if ct_str.isdigit():
                    target_type_id = int(ct_str)
                else:
                    target_type_id = CONTROL_NAME_TO_ID.get(ct_str.lower())
                    if target_type_id is None:
                        raise ValueError(f"Unknown control_type: {control_type}")

        name_q = name.lower() if name else None
        id_q = automation_id.lower() if automation_id else None
        cls_q = class_name.lower() if class_name else None

        matches: List[Dict[str, Any]] = []
        for n in src_tree["nodes"]:
            if only_enabled and not n["enabled"]:
                continue
            if only_visible:
                l, t, r, b = n["bounds"]
                if n["offscreen"] or r <= l or b <= t:
                    continue
            if target_type_id is not None and n["controlTypeId"] != target_type_id:
                continue
            if name_q is not None and name_q not in n["name"].lower():
                continue
            if id_q is not None and n["automationId"].lower() != id_q:
                continue
            if cls_q is not None and cls_q not in n["className"].lower():
                continue
            matches.append(n)
        return matches

    def _action_with_tree(
        self,
        code: int,
        mode: int = EXEC_MODE_HUMAN_EMULATION,
        x: Optional[int] = None,
        y: Optional[int] = None,
        param1: int = 0,
        param2: int = 0,
        speed: float = 1.0,
        hwnd: Union[int, str, None] = 0,
        node_id: Optional[int] = None,
        text: Optional[str] = None,
        default_x: int = 0,
        default_y: int = 0,
    ) -> Dict[str, Any]:
        ack, pkt_type, payload = self._execute(
            code=code, mode=mode, x=x, y=y, param1=param1, param2=param2,
            speed=speed, hwnd=hwnd, node_id=node_id, text=text,
            default_x=default_x, default_y=default_y,
        )
        result: Dict[str, Any] = {"ack": ack}
        if pkt_type == IPC_PKT_TREE and payload is not None:
            tree = _decode_tree_payload(payload)
            self.last_tree = tree
            result["tree"] = tree
        return result

    def click(
        self,
        x: Optional[int] = None,
        y: Optional[int] = None,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        button: int = 0,
        mode: int = EXEC_MODE_HUMAN_EMULATION,
        speed: float = 1.0,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_CLICK, mode=mode, x=x, y=y, param1=button, speed=speed, hwnd=hwnd, node_id=node_id)

    def double_click(
        self,
        x: Optional[int] = None,
        y: Optional[int] = None,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        button: int = 0,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_DOUBLE_CLICK, mode=EXEC_MODE_HUMAN_EMULATION, x=x, y=y, param1=button, hwnd=hwnd, node_id=node_id)

    def right_click(
        self,
        x: Optional[int] = None,
        y: Optional[int] = None,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_RIGHT_CLICK, mode=EXEC_MODE_HUMAN_EMULATION, x=x, y=y, hwnd=hwnd, node_id=node_id)

    def move(
        self,
        x: Optional[int] = None,
        y: Optional[int] = None,
        node_id: Optional[int] = None,
        speed: float = 1.0,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_MOUSE_MOVE, mode=EXEC_MODE_HUMAN_EMULATION, x=x, y=y, speed=speed, node_id=node_id)

    def drag(self, start_x: int, start_y: int, end_x: int, end_y: int, speed: float = 1.0) -> Dict[str, Any]:
        return self._action_with_tree(CMD_DRAG, mode=EXEC_MODE_HUMAN_EMULATION, x=start_x, y=start_y, param1=end_x, param2=end_y, speed=speed)

    def scroll(self, delta: int, x: Optional[int] = None, y: Optional[int] = None, horizontal: bool = False) -> Dict[str, Any]:
        code = CMD_HSCROLL if horizontal else CMD_SCROLL
        if x is None and y is None:
            return self._action_with_tree(
                code, mode=EXEC_MODE_HUMAN_EMULATION, x=None, y=None,
                param1=int(delta), param2=1 if horizontal else 0,
                default_x=INT_MIN, default_y=INT_MIN,
            )
        sx = INT_MIN if x is None else int(x)
        sy = INT_MIN if y is None else int(y)
        return self._action_with_tree(code, mode=EXEC_MODE_HUMAN_EMULATION, x=sx, y=sy, param1=int(delta), param2=1 if horizontal else 0)

    def type_text(
        self,
        text: str,
        hwnd: Union[int, str, None] = 0,
        mode: int = EXEC_MODE_HUMAN_EMULATION,
        speed: float = 1.0,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_TYPE_TEXT, mode=mode, speed=speed, hwnd=hwnd, text=text)

    def set_clipboard_text(self, text: str) -> bool:
        """Injects text into Windows Clipboard as CF_UNICODETEXT using 64-bit Win32 APIs."""
        if not text:
            return True
        k = _kernel32
        u = ctypes.WinDLL("user32", use_last_error=True)
        k.GlobalAlloc.restype = ctypes.c_void_p
        k.GlobalLock.restype = ctypes.c_void_p
        k.GlobalLock.argtypes = [ctypes.c_void_p]
        k.GlobalUnlock.argtypes = [ctypes.c_void_p]
        u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
        u.OpenClipboard.argtypes = [wintypes.HWND]

        for _ in range(5):
            if u.OpenClipboard(None):
                try:
                    u.EmptyClipboard()
                    encoded = text.encode("utf-16-le") + b"\x00\x00"
                    h_mem = k.GlobalAlloc(0x0002, len(encoded))
                    if not h_mem:
                        return False
                    ptr = k.GlobalLock(h_mem)
                    if not ptr:
                        k.GlobalFree(h_mem)
                        return False
                    ctypes.memmove(ptr, encoded, len(encoded))
                    k.GlobalUnlock(h_mem)
                    if not u.SetClipboardData(13, h_mem):
                        k.GlobalFree(h_mem)
                        return False
                    return True
                finally:
                    u.CloseClipboard()
            time.sleep(0.05)
        return False

    def paste_text(self, text: str, hwnd: Union[int, str, None] = 0) -> Dict[str, Any]:
        """Injects Unicode text via clipboard and triggers Ctrl+V. 100% immune to keyboard language."""
        if hwnd:
            self.focus_window(hwnd)
            time.sleep(0.1)
        ok = self.set_clipboard_text(text)
        if not ok:
            raise CPowerError("Failed to set clipboard text")
        return self.send_hotkey(vk=0x56, ctrl=True)

    def send_hotkey(self, vk: int, ctrl: bool = False, shift: bool = False, alt: bool = False, win: bool = False) -> Dict[str, Any]:
        mods = 0
        if ctrl:
            mods |= KEY_MOD_CTRL
        if shift:
            mods |= KEY_MOD_SHIFT
        if alt:
            mods |= KEY_MOD_ALT
        if win:
            mods |= KEY_MOD_WIN
        return self._action_with_tree(CMD_HOTKEY, mode=EXEC_MODE_HUMAN_EMULATION, param1=int(vk), param2=mods)

    def uia_invoke(
        self,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        x: Optional[int] = None,
        y: Optional[int] = None,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_UIA_INVOKE, mode=EXEC_MODE_DIRECT_INSTANT, x=x, y=y, hwnd=hwnd, node_id=node_id)

    def uia_set_value(
        self,
        text: str,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        x: Optional[int] = None,
        y: Optional[int] = None,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_UIA_SET_VALUE, mode=EXEC_MODE_DIRECT_INSTANT, x=x, y=y, hwnd=hwnd, node_id=node_id, text=text)

    def uia_toggle(
        self,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        x: Optional[int] = None,
        y: Optional[int] = None,
    ) -> Dict[str, Any]:
        return self._action_with_tree(CMD_UIA_TOGGLE, mode=EXEC_MODE_DIRECT_INSTANT, x=x, y=y, hwnd=hwnd, node_id=node_id)

    def uia_select(
        self,
        node_id: Optional[int] = None,
        hwnd: Union[int, str, None] = 0,
        x: Optional[int] = None,
        y: Optional[int] = None,
        add_to_selection: bool = False,
    ) -> Dict[str, Any]:
        return self._action_with_tree(
            CMD_UIA_SELECT, mode=EXEC_MODE_DIRECT_INSTANT, x=x, y=y,
            param1=1 if add_to_selection else 0, hwnd=hwnd, node_id=node_id,
        )

    def capture_micro_region(
        self,
        left: int,
        top: int,
        right: int,
        bottom: int,
        fmt: str = "png",
    ) -> Tuple[Dict[str, Any], bytes]:
        ack, pkt_type, payload = self._execute(
            CMD_CAPTURE_MICRO_REGION,
            x=int(left),
            y=int(top),
            param1=int(right),
            param2=int(bottom),
            text=fmt,
        )
        if not ack["succeeded"] or pkt_type != IPC_PKT_IMAGE or payload is None:
            raise CPowerError(f"CMD_CAPTURE_MICRO_REGION failed: {ack['hresultHex']}")
        return ack, payload

    def get_som_elements(self, hwnd: Union[int, str, None] = 0) -> Any:
        """Capture UIA tree and prune it into numbered Set-of-Marks elements."""
        tree = self.capture_tree(hwnd=hwnd)
        try:
            from cpower_som import SemanticPruner
            pruner = SemanticPruner()
            return pruner.prune_tree(tree)
        except ImportError:
            return tree

    def wait_for_window_idle(
        self,
        hwnd: Union[int, str, None] = 0,
        min_nodes: int = 40,
        timeout_ms: int = 8000,
        stable_checks: int = 2,
    ) -> Dict[str, Any]:
        """Reactively wait until window tree node count stabilizes without static sleep."""
        deadline = time.monotonic() + (timeout_ms / 1000.0)
        last_count = -1
        stable_streak = 0
        last_tree = None

        while time.monotonic() < deadline:
            try:
                tree = self.capture_tree(hwnd=hwnd)
                last_tree = tree
                cnt = tree.get("nodeCount", 0)
                if cnt >= min_nodes:
                    if cnt == last_count:
                        stable_streak += 1
                        if stable_streak >= stable_checks:
                            return tree
                    else:
                        stable_streak = 0
                    last_count = cnt
            except Exception:
                pass
            time.sleep(0.15)
        return last_tree or {"nodes": [], "nodeCount": 0}

    def close(self) -> None:
        if getattr(self, "_closed", True):
            return
        self._closed = True

        proc = getattr(self, "_proc", None)
        if proc is not None and proc.poll() is None:
            graceful_sent = False
            try:
                cmd = self._build_command_packet(CMD_SHUTDOWN, mode=EXEC_MODE_DIRECT_INSTANT)
                if self.transport == "shm" and self._in_ring is not None and self._h_in_evt is not None:
                    self._shm_send_command(cmd)
                    graceful_sent = True
                elif self.transport == "pipe" and self._h_pipe is not None:
                    self._pipe_write_msg(bytes(cmd))
                    graceful_sent = True
            except Exception:
                graceful_sent = False

            if graceful_sent:
                try:
                    proc.wait(timeout=2.0)
                except Exception:
                    pass

            if proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=1.5)
                except Exception:
                    try:
                        proc.kill()
                        proc.wait(timeout=1.5)
                    except Exception:
                        pass

        if self._h_pipe:
            try:
                _kernel32.CloseHandle(self._h_pipe)
            except Exception:
                pass
            self._h_pipe = None
        if self._h_out_evt:
            try:
                _kernel32.CloseHandle(self._h_out_evt)
            except Exception:
                pass
            self._h_out_evt = None
        if self._h_in_evt:
            try:
                _kernel32.CloseHandle(self._h_in_evt)
            except Exception:
                pass
            self._h_in_evt = None
        if self._base_ptr:
            try:
                _kernel32.UnmapViewOfFile(self._base_ptr)
            except Exception:
                pass
            self._base_ptr = None
        if self._h_map:
            try:
                _kernel32.CloseHandle(self._h_map)
            except Exception:
                pass
            self._h_map = None
        self._out_ring = None
        self._in_ring = None

        if proc is not None:
            try:
                if proc.stdout:
                    proc.stdout.close()
            except Exception:
                pass
            try:
                if proc.stderr:
                    proc.stderr.close()
            except Exception:
                pass
            self._proc = None

        stderr_thread = getattr(self, "_stderr_thread", None)
        if stderr_thread is not None and stderr_thread is not threading.current_thread():
            try:
                stderr_thread.join(timeout=0.2)
            except Exception:
                pass
        self._stderr_thread = None
