"""
CPower Standalone MCP Server over stdio (mcp/cpower_mcp_server.py).

Implements Model Context Protocol (JSON-RPC 2.0 over stdio) using zero third-party
Python packages, backed by `clients/python/cpower_client.py` and `bin/cpower.exe`.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
from pathlib import Path
import sys
from typing import Any, Dict, List, Optional, Tuple, Union


_CLIENT_DIR = Path(__file__).resolve().parents[1] / "clients" / "python"
if str(_CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(_CLIENT_DIR))

from cpower_client import (  # noqa: E402
    CPowerClient,
    CPowerError,
    EXEC_MODE_DIRECT_INSTANT,
    EXEC_MODE_HUMAN_EMULATION,
)


TOOLS_SCHEMA: List[Dict[str, Any]] = [
    {
        "name": "list_windows",
        "description": "List visible top-level desktop windows with HWND, PID, title, className, and bounding rect.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "focus_window",
        "description": "Bring a window to foreground or change its state (focus, restore, minimize, maximize, close).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": ["integer", "string"], "description": "Window handle (int or hex string like 0x1234)."},
                "action": {
                    "type": "string",
                    "enum": ["focus", "restore", "minimize", "maximize", "close"],
                    "default": "focus",
                },
            },
            "required": ["hwnd"],
            "additionalProperties": False,
        },
    },
    {
        "name": "get_ui_tree",
        "description": "Extract compact semantic UI Automation tree nodes for a window (or active foreground window).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "desktop": {"type": "boolean", "default": False},
                "max_nodes": {"type": "integer", "default": 200, "minimum": 1, "maximum": 4096},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "get_ui_diff",
        "description": "Capture current UI tree and compute semantic delta diff (added/removed/modified nodes) since the previous capture.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "desktop": {"type": "boolean", "default": False},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "find_ui_elements",
        "description": "Search UI Automation tree nodes by name substring, automationId, className, or controlType.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "desktop": {"type": "boolean", "default": False},
                "name": {"type": "string"},
                "automation_id": {"type": "string"},
                "class_name": {"type": "string"},
                "control_type": {"type": ["string", "integer"]},
                "only_enabled": {"type": "boolean", "default": False},
                "only_visible": {"type": "boolean", "default": False},
                "limit": {"type": "integer", "default": 50, "minimum": 1, "maximum": 500},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "click_at",
        "description": "Click at screen coordinates (or window client coordinates in direct mode).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "button": {"type": "string", "enum": ["left", "right", "middle", "double"], "default": "left"},
                "mode": {"type": "string", "enum": ["human", "direct"], "default": "human"},
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "speed": {"type": "number", "exclusiveMinimum": 0, "default": 1.0},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "click_element",
        "description": "Resolve target node from captured tree (by node_id or semantic query) and click it.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "node_id": {"type": "integer", "minimum": 0},
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "name": {"type": "string"},
                "automation_id": {"type": "string"},
                "control_type": {"type": ["string", "integer"]},
                "button": {"type": "string", "enum": ["left", "right", "middle", "double"], "default": "left"},
                "mode": {"type": "string", "enum": ["human", "direct"], "default": "human"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "type_text",
        "description": "Type Unicode text using human-timed SendInput or direct WM_SETTEXT if hwnd+direct mode specified.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string"},
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "mode": {"type": "string", "enum": ["human", "direct"], "default": "human"},
                "speed": {"type": "number", "exclusiveMinimum": 0, "default": 1.0},
            },
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "paste_text",
        "description": "Instantly inject Unicode text via Windows clipboard and paste (Ctrl+V). 100% immune to active keyboard layout (Arabic/Russian/etc.), instant for long URLs and code.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string"},
                "hwnd": {"type": ["integer", "string"], "default": 0},
            },
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "press_hotkey",
        "description": "Send a virtual key chord (e.g. vk=0x43 with ctrl=true for Ctrl+C).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "vk": {"type": "integer", "minimum": 1, "maximum": 65535},
                "ctrl": {"type": "boolean", "default": False},
                "shift": {"type": "boolean", "default": False},
                "alt": {"type": "boolean", "default": False},
                "win": {"type": "boolean", "default": False},
            },
            "required": ["vk"],
            "additionalProperties": False,
        },
    },
    {
        "name": "scroll_at",
        "description": "Scroll vertically or horizontally via mouse wheel (positive=up/right, negative=down/left, 120 per notch).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "delta": {"type": "integer", "default": -120},
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "horizontal": {"type": "boolean", "default": False},
            },
            "required": ["delta"],
            "additionalProperties": False,
        },
    },
    {
        "name": "uia_invoke",
        "description": "Invoke, toggle, select, or set value on a UIA element directly via COM pattern without moving the mouse.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["invoke", "toggle", "select", "set_value"], "default": "invoke"},
                "node_id": {"type": "integer", "minimum": 0, "default": 0},
                "hwnd": {"type": ["integer", "string"], "default": 0},
                "x": {"type": "integer", "default": 0},
                "y": {"type": "integer", "default": 0},
                "text": {"type": "string"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "capture_micro_region",
        "description": "Capture a screen bounding box via DXGI VRAM (supports multi-monitor stitching) and return a base64 PNG image.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "left": {"type": "integer"},
                "top": {"type": "integer"},
                "right": {"type": "integer"},
                "bottom": {"type": "integer"},
            },
            "required": ["left", "top", "right", "bottom"],
            "additionalProperties": False,
        },
    },
]


_TOOL_SCHEMA_MAP: Dict[str, Dict[str, Any]] = {
    tool["name"]: tool["inputSchema"] for tool in TOOLS_SCHEMA
}


class _InvalidParamsError(ValueError):
    """Raised when JSON-RPC / MCP tool parameters violate schema."""


class _RpcParseError(ValueError):
    """Raised when incoming stdio JSON-RPC payload cannot be parsed."""

    def __init__(self, message: str, framed: bool = False) -> None:
        super().__init__(message)
        self.framed = framed


def _is_valid_json_type(val: Any, expected: Union[str, List[str]]) -> bool:
    types = [expected] if isinstance(expected, str) else expected
    for t in types:
        if t == "integer":
            if isinstance(val, int) and not isinstance(val, bool):
                return True
        elif t == "number":
            if isinstance(val, (int, float)) and not isinstance(val, bool) and math.isfinite(float(val)):
                return True
        elif t == "boolean":
            if isinstance(val, bool):
                return True
        elif t == "string":
            if isinstance(val, str):
                return True
        elif t == "object":
            if isinstance(val, dict):
                return True
        elif t == "array":
            if isinstance(val, list):
                return True
        elif t == "null":
            if val is None:
                return True
    return False


def _validate_tool_arguments(tool_name: str, arguments: Any) -> Dict[str, Any]:
    schema = _TOOL_SCHEMA_MAP.get(tool_name)
    if schema is None:
        raise _InvalidParamsError(f"Unknown tool: {tool_name}")

    if arguments is None:
        args: Dict[str, Any] = {}
    elif isinstance(arguments, dict):
        args = arguments
    else:
        raise _InvalidParamsError("Tool 'arguments' must be an object")

    props: Dict[str, Any] = schema.get("properties", {})
    required: List[str] = schema.get("required", [])

    for req_key in required:
        if req_key not in args:
            raise _InvalidParamsError(f"Missing required argument: '{req_key}'")

    if schema.get("additionalProperties") is False:
        for key in args:
            if key not in props:
                raise _InvalidParamsError(f"Unknown argument '{key}' for tool '{tool_name}'")

    for key, val in args.items():
        ps = props.get(key)
        if ps is None:
            continue

        expected_type = ps.get("type")
        if expected_type is not None and not _is_valid_json_type(val, expected_type):
            raise _InvalidParamsError(
                f"Argument '{key}' has invalid type {type(val).__name__}; expected {expected_type}"
            )

        if "enum" in ps and val not in ps["enum"]:
            raise _InvalidParamsError(
                f"Argument '{key}' value {val!r} must be one of {ps['enum']}"
            )

        if isinstance(val, (int, float)) and not isinstance(val, bool):
            if "minimum" in ps and val < ps["minimum"]:
                raise _InvalidParamsError(
                    f"Argument '{key}' ({val}) must be >= {ps['minimum']}"
                )
            if "maximum" in ps and val > ps["maximum"]:
                raise _InvalidParamsError(
                    f"Argument '{key}' ({val}) must be <= {ps['maximum']}"
                )
            if "exclusiveMinimum" in ps and val <= ps["exclusiveMinimum"]:
                raise _InvalidParamsError(
                    f"Argument '{key}' ({val}) must be > {ps['exclusiveMinimum']}"
                )
            if "exclusiveMaximum" in ps and val >= ps["exclusiveMaximum"]:
                raise _InvalidParamsError(
                    f"Argument '{key}' ({val}) must be < {ps['exclusiveMaximum']}"
                )

        if key == "hwnd" and isinstance(val, str):
            stripped = val.strip()
            if not stripped:
                raise _InvalidParamsError("Argument 'hwnd' string cannot be empty")
            try:
                int(stripped, 0)
            except ValueError as exc:
                raise _InvalidParamsError(f"Invalid 'hwnd' string: {val!r}") from exc

    if tool_name == "click_element":
        has_node = "node_id" in args and args["node_id"] is not None
        has_name = bool(args.get("name"))
        has_auto_id = bool(args.get("automation_id"))
        has_ctype = args.get("control_type") not in (None, "")
        if not (has_node or has_name or has_auto_id or has_ctype):
            raise _InvalidParamsError(
                "click_element requires at least one locator: node_id, name, automation_id, or control_type"
            )

    return args


def _compact_nodes(nodes: List[Dict[str, Any]], max_nodes: int) -> Dict[str, Any]:
    sliced = nodes[:max_nodes]
    compact = []
    for n in sliced:
        item: Dict[str, Any] = {
            "id": n["id"],
            "parentId": n["parentId"],
            "type": n["type"],
            "bounds": n["bounds"],
            "center": n["center"],
        }
        if n["name"]:
            item["name"] = n["name"]
        if n["automationId"]:
            item["automationId"] = n["automationId"]
        if n["value"]:
            item["value"] = n["value"]
        if not n["enabled"]:
            item["enabled"] = False
        if n["offscreen"]:
            item["offscreen"] = True
        compact.append(item)
    return {"returnedNodes": len(compact), "totalNodes": len(nodes), "nodes": compact}


class CPowerMcpServer:
    def __init__(
        self,
        transport: str = "shm",
        name: Optional[str] = None,
        exe_path: Optional[str] = None,
    ) -> None:
        self.transport = transport
        self.name = name
        self.exe_path = exe_path
        self._client: Optional[CPowerClient] = None

    def _get_client(self) -> CPowerClient:
        if self._client is None or self._client._closed:
            self._client = CPowerClient(
                transport=self.transport,
                name=self.name,
                exe_path=self.exe_path,
                auto_spawn=True,
                timeout_ms=30000,
            )
        return self._client

    def close(self) -> None:
        if self._client is not None:
            self._client.close()
            self._client = None

    def _call_tool(self, tool_name: str, args: Dict[str, Any]) -> Dict[str, Any]:
        client = self._get_client()

        if tool_name == "list_windows":
            wins = client.list_windows()
            return {
                "content": [{"type": "text", "text": json.dumps({"count": len(wins), "windows": wins}, ensure_ascii=False)}],
                "isError": False,
            }

        if tool_name == "focus_window":
            hwnd = args["hwnd"]
            action = args.get("action", "focus")
            ack = client.focus_window(hwnd) if action == "focus" else client.show_window(hwnd, action)
            return {
                "content": [{"type": "text", "text": json.dumps({"action": action, "ack": ack})}],
                "isError": not ack["succeeded"],
            }

        if tool_name == "get_ui_tree":
            tree = client.capture_tree(hwnd=args.get("hwnd", 0), desktop=bool(args.get("desktop", False)))
            summary = _compact_nodes(tree["nodes"], int(args.get("max_nodes", 200)))
            summary["ack"] = tree["ack"]
            return {
                "content": [{"type": "text", "text": json.dumps(summary, ensure_ascii=False)}],
                "isError": False,
            }

        if tool_name == "get_ui_diff":
            diff = client.diff_tree(hwnd=args.get("hwnd", 0), desktop=bool(args.get("desktop", False)))
            return {
                "content": [{"type": "text", "text": json.dumps(diff, ensure_ascii=False)}],
                "isError": False,
            }

        if tool_name == "find_ui_elements":
            matches = client.find_nodes(
                name=args.get("name"),
                automation_id=args.get("automation_id"),
                class_name=args.get("class_name"),
                control_type=args.get("control_type"),
                only_enabled=bool(args.get("only_enabled", False)),
                only_visible=bool(args.get("only_visible", False)),
                hwnd=args.get("hwnd", 0),
                desktop=bool(args.get("desktop", False)),
            )
            limit = int(args.get("limit", 50))
            payload = {"totalMatches": len(matches), "matches": matches[:limit]}
            return {
                "content": [{"type": "text", "text": json.dumps(payload, ensure_ascii=False)}],
                "isError": False,
            }

        if tool_name == "click_at":
            x, y = int(args["x"]), int(args["y"])
            btn = args.get("button", "left")
            mode = EXEC_MODE_DIRECT_INSTANT if args.get("mode") == "direct" else EXEC_MODE_HUMAN_EMULATION
            hwnd = args.get("hwnd", 0)
            speed = float(args.get("speed", 1.0))
            if btn == "double":
                res = client.double_click(x=x, y=y, hwnd=hwnd)
            elif btn == "right":
                res = client.right_click(x=x, y=y, hwnd=hwnd)
            elif btn == "middle":
                res = client.click(x=x, y=y, hwnd=hwnd, button=2, mode=mode, speed=speed)
            else:
                res = client.click(x=x, y=y, hwnd=hwnd, button=0, mode=mode, speed=speed)
            return {
                "content": [{"type": "text", "text": json.dumps({"ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "click_element":
            hwnd = args.get("hwnd", 0)
            node_id = args.get("node_id")
            if node_id is None:
                matches = client.find_nodes(
                    name=args.get("name"),
                    automation_id=args.get("automation_id"),
                    control_type=args.get("control_type"),
                    hwnd=hwnd,
                )
                if not matches:
                    return {
                        "content": [{"type": "text", "text": "No matching UI element found."}],
                        "isError": True,
                    }
                node_id = matches[0]["id"]

            btn = args.get("button", "left")
            mode = EXEC_MODE_DIRECT_INSTANT if args.get("mode") == "direct" else EXEC_MODE_HUMAN_EMULATION
            if btn == "double":
                res = client.double_click(node_id=int(node_id), hwnd=hwnd)
            elif btn == "right":
                res = client.right_click(node_id=int(node_id), hwnd=hwnd)
            elif btn == "middle":
                res = client.click(node_id=int(node_id), hwnd=hwnd, button=2, mode=mode)
            else:
                res = client.click(node_id=int(node_id), hwnd=hwnd, button=0, mode=mode)
            return {
                "content": [{"type": "text", "text": json.dumps({"nodeId": int(node_id), "ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "type_text":
            mode = EXEC_MODE_DIRECT_INSTANT if args.get("mode") == "direct" else EXEC_MODE_HUMAN_EMULATION
            res = client.type_text(
                text=str(args["text"]),
                hwnd=args.get("hwnd", 0),
                mode=mode,
                speed=float(args.get("speed", 1.0)),
            )
            return {
                "content": [{"type": "text", "text": json.dumps({"ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "paste_text":
            res = client.paste_text(
                text=str(args["text"]),
                hwnd=args.get("hwnd", 0),
            )
            return {
                "content": [{"type": "text", "text": json.dumps({"pasted": True, "ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "press_hotkey":
            res = client.send_hotkey(
                vk=int(args["vk"]),
                ctrl=bool(args.get("ctrl", False)),
                shift=bool(args.get("shift", False)),
                alt=bool(args.get("alt", False)),
                win=bool(args.get("win", False)),
            )
            return {
                "content": [{"type": "text", "text": json.dumps({"ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "scroll_at":
            res = client.scroll(
                delta=int(args["delta"]),
                x=args.get("x"),
                y=args.get("y"),
                horizontal=bool(args.get("horizontal", False)),
            )
            return {
                "content": [{"type": "text", "text": json.dumps({"ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "uia_invoke":
            action = args.get("action", "invoke")
            node_id = int(args["node_id"]) if "node_id" in args else None
            hwnd = args.get("hwnd", 0)
            x = int(args["x"]) if "x" in args else None
            y = int(args["y"]) if "y" in args else None
            if action == "set_value":
                res = client.uia_set_value(str(args.get("text", "")), node_id=node_id, hwnd=hwnd, x=x, y=y)
            elif action == "toggle":
                res = client.uia_toggle(node_id=node_id, hwnd=hwnd, x=x, y=y)
            elif action == "select":
                res = client.uia_select(node_id=node_id, hwnd=hwnd, x=x, y=y)
            else:
                res = client.uia_invoke(node_id=node_id, hwnd=hwnd, x=x, y=y)
            return {
                "content": [{"type": "text", "text": json.dumps({"action": action, "ack": res["ack"]})}],
                "isError": not res["ack"]["succeeded"],
            }

        if tool_name == "capture_micro_region":
            l, t, r, b = int(args["left"]), int(args["top"]), int(args["right"]), int(args["bottom"])
            ack, png_bytes = client.capture_micro_region(l, t, r, b, fmt="png")
            b64 = base64.b64encode(png_bytes).decode("ascii")
            return {
                "content": [
                    {"type": "image", "data": b64, "mimeType": "image/png"},
                    {"type": "text", "text": json.dumps({"bytes": len(png_bytes), "rect": [l, t, r, b], "ack": ack})},
                ],
                "isError": False,
            }

        raise _InvalidParamsError(f"Unknown tool: {tool_name}")

    def handle_request(self, req: Any) -> Optional[Dict[str, Any]]:
        if not isinstance(req, dict):
            return {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32600, "message": "Invalid Request: expected JSON object"},
            }

        has_id = "id" in req
        raw_id = req.get("id")
        req_id = raw_id if (isinstance(raw_id, (str, int, type(None))) and not isinstance(raw_id, bool)) else None

        if "jsonrpc" in req and req.get("jsonrpc") != "2.0":
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32600, "message": "Invalid Request: jsonrpc must be '2.0'"},
            }

        method = req.get("method")
        if not isinstance(method, str) or not method:
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32600, "message": "Invalid Request: 'method' must be a non-empty string"},
            }

        # Notifications have no "id" member and expect no response
        if not has_id:
            return None

        raw_params = req.get("params")
        if raw_params is None:
            params: Dict[str, Any] = {}
        elif isinstance(raw_params, dict):
            params = raw_params
        else:
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32602, "message": "Invalid params: 'params' must be an object"},
            }

        try:
            if method == "initialize":
                proto = params.get("protocolVersion") or "2024-11-05"
                if not isinstance(proto, str):
                    return {
                        "jsonrpc": "2.0",
                        "id": req_id,
                        "error": {"code": -32602, "message": "Invalid params: 'protocolVersion' must be a string"},
                    }
                return {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {
                        "protocolVersion": proto,
                        "capabilities": {"tools": {"listChanged": False}},
                        "serverInfo": {"name": "cpower-mcp", "version": "1.1.0"},
                    },
                }

            if method == "ping":
                return {"jsonrpc": "2.0", "id": req_id, "result": {}}

            if method == "tools/list":
                return {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {"tools": TOOLS_SCHEMA},
                }

            if method == "tools/call":
                tool_name = params.get("name")
                if not isinstance(tool_name, str) or not tool_name:
                    return {
                        "jsonrpc": "2.0",
                        "id": req_id,
                        "error": {"code": -32602, "message": "Invalid params: 'name' must be a non-empty string"},
                    }
                try:
                    tool_args = _validate_tool_arguments(tool_name, params.get("arguments"))
                except (_InvalidParamsError, TypeError) as exc:
                    return {
                        "jsonrpc": "2.0",
                        "id": req_id,
                        "error": {"code": -32602, "message": f"Invalid params: {exc}"},
                    }

                try:
                    res = self._call_tool(tool_name, tool_args)
                except TypeError as exc:
                    return {
                        "jsonrpc": "2.0",
                        "id": req_id,
                        "error": {"code": -32602, "message": f"Invalid params: {exc}"},
                    }
                except (CPowerError, ValueError, KeyError) as exc:
                    res = {
                        "content": [{"type": "text", "text": f"Tool '{tool_name}' error: {exc}"}],
                        "isError": True,
                    }
                return {"jsonrpc": "2.0", "id": req_id, "result": res}

            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"},
            }
        except Exception as exc:
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32603, "message": f"Internal error: {exc}"},
            }


def _read_message() -> Optional[Tuple[Any, bool]]:
    """Read either newline-delimited JSON-RPC or Content-Length framed JSON-RPC."""
    stdin = sys.stdin.buffer
    while True:
        line = stdin.readline()
        if not line:
            return None
        stripped = line.strip()
        if not stripped:
            continue

        lower = stripped.lower()
        if lower.startswith(b"content-length:"):
            raw_len = stripped.split(b":", 1)[1].strip()
            length = -1
            bad_len_msg: Optional[str] = None
            try:
                length = int(raw_len)
                if length < 0 or length > (64 * 1024 * 1024):
                    bad_len_msg = f"Invalid Content-Length value: {raw_len!r}"
            except ValueError:
                bad_len_msg = f"Invalid Content-Length header: {raw_len!r}"

            while True:
                hdr_line = stdin.readline()
                if not hdr_line or hdr_line in (b"\r\n", b"\n"):
                    break

            if bad_len_msg is not None:
                raise _RpcParseError(bad_len_msg, framed=True)

            body = stdin.read(length)
            if len(body) < length:
                raise _RpcParseError(
                    f"Truncated Content-Length body (expected {length} bytes, got {len(body)})",
                    framed=True,
                )
            try:
                return json.loads(body.decode("utf-8")), True
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise _RpcParseError(str(exc), framed=True) from exc

        try:
            return json.loads(stripped.decode("utf-8")), False
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise _RpcParseError(str(exc), framed=False) from exc


def _write_message(msg: Dict[str, Any], framed: bool) -> None:
    encoded = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    stdout = sys.stdout.buffer
    if framed:
        header = f"Content-Length: {len(encoded)}\r\n\r\n".encode("ascii")
        stdout.write(header)
        stdout.write(encoded)
    else:
        stdout.write(encoded + b"\n")
    stdout.flush()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="CPower Standalone MCP stdio server")
    parser.add_argument("--transport", choices=["shm", "pipe", "auto"], default="auto")
    parser.add_argument("--name", default=None, help="Optional custom SHM or Named Pipe name")
    parser.add_argument("--exe", default=None, help="Optional path to cpower.exe")
    args = parser.parse_args(argv)

    server = CPowerMcpServer(transport=args.transport, name=args.name, exe_path=args.exe)
    try:
        while True:
            try:
                item = _read_message()
            except _RpcParseError as exc:
                _write_message(
                    {
                        "jsonrpc": "2.0",
                        "id": None,
                        "error": {"code": -32700, "message": f"Parse error: {exc}"},
                    },
                    framed=exc.framed,
                )
                continue
            if item is None:
                break
            req, framed = item
            resp = server.handle_request(req)
            if resp is not None:
                _write_message(resp, framed)
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
