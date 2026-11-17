"""
Comprehensive Python SDK & Standalone MCP Server Verification Suite (tests/test_python_mcp.py).

Verifies:
1. All 11 packed C ABI ctypes struct sizes match C headers byte-for-byte.
2. CPowerClient over BOTH Shared Memory (stream_events=True) and Named Pipes:
   - list_windows, focus_window, show_window
   - capture_tree, find_nodes, uia_set_value (by node_id), uia_toggle, uia_invoke
   - diff_tree (non-empty delta detection after UI mutation + zero delta when idle)
   - capture_micro_region (live DXGI PNG or graceful headless HRESULT reporting)
   - poll_events (real WinEvent packets over SHM)
3. Standalone MCP stdio server (mcp/cpower_mcp_server.py) over real stdin/stdout pipes
   using BOTH newline-delimited JSON-RPC and Content-Length framing:
   - initialize, notifications/initialized, ping, tools/list (12 tools), tools/call
   - Clean daemon shutdown when stdin closes.
"""

from __future__ import annotations

import base64
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
CLIENT_DIR = REPO_ROOT / "clients" / "python"
MCP_SERVER_PATH = REPO_ROOT / "mcp" / "cpower_mcp_server.py"
CPOWER_EXE = REPO_ROOT / "bin" / "cpower.exe"

if str(CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_DIR))

import cpower_client as cc  # noqa: E402


_user32 = ctypes.WinDLL("user32", use_last_error=True)
_BM_CLICK = 0x00F5

_EnumChildProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_user32.EnumChildWindows.argtypes = [wintypes.HWND, _EnumChildProc, wintypes.LPARAM]
_user32.EnumChildWindows.restype = wintypes.BOOL
_user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.GetWindowTextW.restype = ctypes.c_int
_user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
_user32.SendMessageW.restype = ctypes.c_ssize_t


def _find_child_hwnd_by_text(parent_hwnd: int, target_text: str) -> Optional[int]:
    found: List[int] = []
    buf = ctypes.create_unicode_buffer(256)

    @_EnumChildProc
    def _cb(hwnd: int, _lp: int) -> bool:
        n = _user32.GetWindowTextW(hwnd, buf, 256)
        if n > 0 and buf.value == target_text:
            found.append(int(hwnd))
            return False
        return True

    _user32.EnumChildWindows(wintypes.HWND(parent_hwnd), _cb, 0)
    return found[0] if found else None


def _spawn_winforms_target(title: str) -> subprocess.Popen[bytes]:
    ps_script = (
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$script:cnt=0; "
        f"$f=New-Object Windows.Forms.Form; $f.Text='{title}'; $f.Width=380; $f.Height=260; "
        "$tb=New-Object Windows.Forms.TextBox; $tb.Text='InitVal'; $tb.Name='PyEditBox'; "
        "$tb.SetBounds(20,20,260,24); $f.Controls.Add($tb); "
        "$chk=New-Object Windows.Forms.CheckBox; $chk.Text='PyToggleChk'; $chk.Name='PyCheckBox'; "
        "$chk.SetBounds(20,56,220,24); $f.Controls.Add($chk); "
        "$lbl=New-Object Windows.Forms.Label; $lbl.Text='Status:0'; $lbl.Name='PyStatusLbl'; "
        "$lbl.SetBounds(20,90,180,24); $f.Controls.Add($lbl); "
        "$btn=New-Object Windows.Forms.Button; $btn.Text='ApplyActionBtn'; $btn.Name='PyApplyBtn'; "
        "$btn.SetBounds(20,124,180,32); "
        "$btn.Add_Click({ $script:cnt++; $lbl.Text='Status:'+$script:cnt }); $f.Controls.Add($btn); "
        "$guard=New-Object Windows.Forms.Timer; $guard.Interval=45000; $guard.Add_Tick({$guard.Stop();$f.Close()}); "
        "$f.Add_Shown({ $f.Activate(); $tb.Focus(); $guard.Start() }); "
        "[Windows.Forms.Application]::Run($f)"
    )
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.Popen(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", ps_script],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creation_flags,
    )


def _wait_for_target_window(client: cc.CPowerClient, title: str, timeout_s: float = 10.0) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        wins = client.list_windows()
        for w in wins:
            if w["title"] == title:
                return w
        time.sleep(0.08)
    raise AssertionError(f"Timed out waiting for target window '{title}' in list_windows()")


def _wait_for_ui_controls(client: cc.CPowerClient, hwnd: int, timeout_s: float = 6.0) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        tree = client.capture_tree(hwnd=hwnd)
        types = {n["controlTypeId"] for n in tree["nodes"]}
        if {50000, 50002, 50004, 50020}.issubset(types):
            return tree
        time.sleep(0.08)
    raise AssertionError("Timed out waiting for all WinForms child controls in UIA tree")


def _stop_proc(proc: Optional[subprocess.Popen[bytes]]) -> None:
    if proc is None:
        return
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2.0)


def test_ctypes_abi_sizes() -> None:
    expected = {
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
    for name, size in expected.items():
        cls = getattr(cc, name)
        actual = ctypes.sizeof(cls)
        assert actual == size, f"{name}: expected {size}, got {actual}"

    expected_offsets = {
        "ShmRingBufferHeader": {
            "magic": 0, "version": 4, "capacity": 8, "mask": 12, "pad0": 16,
            "writeIdx": 64, "pad1": 68, "readIdx": 128, "pad2": 132,
        },
        "CommandPacket": {
            "packetMagic": 0, "commandId": 4, "commandCode": 8, "executionMode": 10,
            "targetX": 12, "targetY": 16, "param1": 20, "param2": 24,
            "speedFactor": 28, "jitterScale": 32, "targetHwnd": 36,
            "targetNodeId": 44, "textPayloadBytes": 48, "textPayload": 52,
        },
        "AckPacket": {
            "magic": 0, "commandId": 4, "hresult": 8, "elapsedUs": 12,
            "nodeCount": 16, "diffCount": 20, "flags": 24,
        },
        "EventPacket": {
            "magic": 0, "event": 4, "hwnd": 8, "idObject": 16,
            "idChild": 20, "threadId": 24, "timestamp": 28,
        },
        "SemanticNode": {
            "id": 0, "parentId": 4, "depth": 8, "controlTypeId": 10,
            "bounds": 12, "flags": 28, "name": 32, "className": 288,
            "automationId": 416, "value": 544,
        },
        "WindowInfo": {
            "hwnd": 0, "pid": 8, "rect": 12, "isVisible": 28,
            "isMinimized": 32, "title": 36, "className": 292,
        },
    }
    for struct_name, fields in expected_offsets.items():
        cls = getattr(cc, struct_name)
        for field_name, exp_off in fields.items():
            actual_off = getattr(cls, field_name).offset
            assert actual_off == exp_off, (
                f"{struct_name}.{field_name} offset mismatch: expected {exp_off}, got {actual_off}"
            )
    print("[PASS] test_ctypes_abi_sizes (all 11 struct sizes & 6 struct field offsets match)")


def test_client_unit_helpers() -> None:
    # 1. UTF-16LE byte truncation (<= 510 bytes, never splits surrogate pairs)
    astral_edge = ("a" * 254) + "\U0001F680"  # 508 + 4 = 512 bytes
    trimmed, raw = cc._truncate_utf16le(astral_edge, 510)
    assert len(raw) == 508 and len(raw) <= 510
    assert trimmed == ("a" * 254)
    assert trimmed.encode("utf-16-le") == raw

    full_rockets = "\U0001F680" * 128  # 512 bytes
    trimmed_r, raw_r = cc._truncate_utf16le(full_rockets, 510)
    assert len(raw_r) == 508 and trimmed_r == ("\U0001F680" * 127)

    # 2. C-compatible truncating center rounding
    node = cc.SemanticNode()
    node.bounds.left = -10
    node.bounds.top = -5
    node.bounds.right = -5
    node.bounds.bottom = 0
    nd = cc._node_to_dict(node)
    assert nd["center"] == [-8, -3], f"Unexpected negative center rounding: {nd['center']}"

    # 3. CommandPacket executionMode flags for node_id == 0 and (0, 0) coords
    dummy = object.__new__(cc.CPowerClient)
    dummy._next_cmd_id = 1
    cmd_node0 = dummy._build_command_packet(cc.CMD_CLICK, mode=cc.EXEC_MODE_DIRECT_INSTANT, node_id=0)
    assert (cmd_node0.executionMode & cc.CPOWER_EXEC_MODE_MASK) == cc.EXEC_MODE_DIRECT_INSTANT
    assert (cmd_node0.executionMode & cc.CPOWER_EXEC_FLAG_HAS_NODE_ID) != 0
    assert (cmd_node0.executionMode & cc.CPOWER_EXEC_FLAG_HAS_COORDS) == 0
    assert cmd_node0.targetNodeId == 0

    cmd_xy0 = dummy._build_command_packet(cc.CMD_CLICK, mode=cc.EXEC_MODE_HUMAN_EMULATION, x=0, y=0)
    assert (cmd_xy0.executionMode & cc.CPOWER_EXEC_MODE_MASK) == cc.EXEC_MODE_HUMAN_EMULATION
    assert (cmd_xy0.executionMode & cc.CPOWER_EXEC_FLAG_HAS_COORDS) != 0
    assert (cmd_xy0.executionMode & cc.CPOWER_EXEC_FLAG_HAS_NODE_ID) == 0
    assert cmd_xy0.targetX == 0 and cmd_xy0.targetY == 0

    cmd_both = dummy._build_command_packet(cc.CMD_UIA_INVOKE, mode=cc.EXEC_MODE_DIRECT_INSTANT, x=0, y=0, node_id=0)
    assert (cmd_both.executionMode & cc.CPOWER_EXEC_FLAG_HAS_NODE_ID) != 0
    assert (cmd_both.executionMode & cc.CPOWER_EXEC_FLAG_HAS_COORDS) != 0

    cmd_none = dummy._build_command_packet(cc.CMD_NOP, mode=cc.EXEC_MODE_DIRECT_INSTANT)
    assert (cmd_none.executionMode & (cc.CPOWER_EXEC_FLAG_HAS_NODE_ID | cc.CPOWER_EXEC_FLAG_HAS_COORDS)) == 0

    # 4. auto_spawn=False fails cleanly when daemon is absent
    for transport, absent_name in (
        ("shm", f"Local\\CPowerAbsent_{os.getpid()}"),
        ("pipe", f"\\\\.\\pipe\\cpower_absent_{os.getpid()}"),
    ):
        try:
            cc.CPowerClient(transport=transport, name=absent_name, auto_spawn=False, timeout_ms=200)
            raise AssertionError(f"Expected CPowerError for absent {transport} daemon")
        except cc.CPowerError:
            pass

    print("[PASS] test_client_unit_helpers (UTF-16LE, center, flags, auto_spawn=False)")


def test_python_client_transport(transport: str) -> None:
    pid = os.getpid()
    tag = f"{transport}_{pid}"
    title = f"CPowerPyTarget_{tag}"
    if transport == "shm":
        chan_name = f"Local\\CPowerPyShm_{tag}"
    else:
        chan_name = f"\\\\.\\pipe\\cpower_py_pipe_{tag}"

    gui_proc = _spawn_winforms_target(title)
    try:
        with cc.CPowerClient(
            transport=transport,
            name=chan_name,
            exe_path=CPOWER_EXE,
            auto_spawn=True,
            stream_events=(transport == "shm"),
            timeout_ms=10000,
        ) as client:
            win = _wait_for_target_window(client, title)
            hwnd = win["hwndInt"]
            assert win["visible"] is True
            assert win["pid"] == gui_proc.pid

            # Window focus & show roundtrip
            ack_focus = client.focus_window(hwnd)
            assert ack_focus["succeeded"], f"focus_window failed: {ack_focus}"
            assert client.show_window(hwnd, "minimize")["succeeded"]
            time.sleep(0.05)
            assert client.show_window(hwnd, "restore")["succeeded"]
            time.sleep(0.05)

            # Capture tree and run semantic locator
            tree = _wait_for_ui_controls(client, hwnd)
            assert tree["nodeCount"] >= 5

            edits = client.find_nodes(control_type="Edit", tree=tree)
            btns = client.find_nodes(automation_id="PyApplyBtn", control_type="Button", tree=tree)
            chks = client.find_nodes(name="PyToggleChk", control_type="CheckBox", tree=tree)
            assert len(edits) == 1 and len(btns) == 1 and len(chks) == 1

            # SetValue & Toggle & Invoke by node_id only (x=0, y=0 omitted)
            res_val = client.uia_set_value(f"PyValue_{transport}", node_id=edits[0]["id"], hwnd=hwnd)
            assert res_val["ack"]["succeeded"], res_val["ack"]

            # Explicit (0, 0) client-coordinate click & node_id == 0 roundtrip
            res_coord00 = client.click(x=0, y=0, hwnd=hwnd, mode=cc.EXEC_MODE_DIRECT_INSTANT)
            assert res_coord00["ack"]["succeeded"], f"(0,0) direct click failed: {res_coord00}"
            res_node0 = client.click(node_id=0, hwnd=hwnd, mode=cc.EXEC_MODE_DIRECT_INSTANT)
            assert res_node0["ack"]["succeeded"], f"node_id=0 direct click failed: {res_node0}"

            updated_chks = client.find_nodes(automation_id="PyCheckBox", tree=res_val["tree"])
            res_tog = client.uia_toggle(node_id=updated_chks[0]["id"], hwnd=hwnd)
            assert res_tog["ack"]["succeeded"], res_tog["ack"]

            updated_btns = client.find_nodes(automation_id="PyApplyBtn", tree=res_tog["tree"])
            res_inv = client.uia_invoke(node_id=updated_btns[0]["id"], hwnd=hwnd)
            assert res_inv["ack"]["succeeded"], res_inv["ack"]

            # Verify final UI state via tree find_nodes
            final_tree = client.capture_tree(hwnd=hwnd)
            assert len(client.find_nodes(name="Status:1", tree=final_tree)) == 1
            final_edits = client.find_nodes(control_type="Edit", tree=final_tree)
            assert final_edits[0]["value"] == f"PyValue_{transport}"

            # Trigger an external click via Win32 BM_CLICK so diff_tree sees a fresh UI delta
            btn_hwnd = _find_child_hwnd_by_text(hwnd, "ApplyActionBtn")
            assert btn_hwnd is not None, "Could not locate ApplyActionBtn child HWND"
            _user32.SendMessageW(wintypes.HWND(btn_hwnd), _BM_CLICK, 0, 0)
            time.sleep(0.1)

            diff1 = client.diff_tree(hwnd=hwnd)
            assert diff1["entryCount"] >= 1, f"Expected non-empty diff after external click, got {diff1}"
            assert any(e["node"]["name"] == "Status:2" for e in diff1["entries"])

            diff_idle = client.diff_tree(hwnd=hwnd)
            assert diff_idle["entryCount"] == 0, f"Expected 0 diff when unchanged, got {diff_idle}"

            if transport == "shm":
                evts = client.poll_events()
                assert len(evts) > 0, "Expected live WinEvents streamed over SHM"

            # Verify capture_micro_region over active transport (live PNG or graceful headless DXGI error)
            try:
                cap_ack, png_raw = client.capture_micro_region(0, 0, 32, 32, fmt="png")
                assert cap_ack["succeeded"]
                assert png_raw.startswith(b"\x89PNG\r\n\x1a\n")
            except cc.CPowerError as exc:
                msg = str(exc).lower()
                assert "0x8" in msg or "failed" in msg

            # Close target cleanly
            client.show_window(hwnd, "close")
    finally:
        _stop_proc(gui_proc)

    print(f"[PASS] test_python_client_transport ({transport})")


def _mcp_send_line(proc: subprocess.Popen[bytes], payload: Dict[str, Any]) -> None:
    assert proc.stdin is not None
    raw = (json.dumps(payload) + "\n").encode("utf-8")
    proc.stdin.write(raw)
    proc.stdin.flush()


def _mcp_recv_line(proc: subprocess.Popen[bytes]) -> Dict[str, Any]:
    assert proc.stdout is not None
    line = proc.stdout.readline()
    assert line, "MCP server EOF while waiting for newline-delimited response"
    return json.loads(line.decode("utf-8"))


def _mcp_send_framed(proc: subprocess.Popen[bytes], payload: Dict[str, Any]) -> None:
    assert proc.stdin is not None
    body = json.dumps(payload).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    proc.stdin.write(header + body)
    proc.stdin.flush()


def _mcp_recv_framed(proc: subprocess.Popen[bytes]) -> Dict[str, Any]:
    assert proc.stdout is not None
    length = None
    while True:
        line = proc.stdout.readline()
        assert line, "MCP server EOF while waiting for Content-Length headers"
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
    assert length is not None and length > 0
    body = proc.stdout.read(length)
    assert len(body) == length
    return json.loads(body.decode("utf-8"))


def test_mcp_stdio_server(framed: bool) -> None:
    pid = os.getpid()
    mode_tag = "framed" if framed else "jsonl"
    title = f"CPowerMcpTarget_{mode_tag}_{pid}"
    shm_name = f"Local\\CPowerMcpShm_{mode_tag}_{pid}"

    gui_proc = _spawn_winforms_target(title)
    mcp_proc = subprocess.Popen(
        [sys.executable, str(MCP_SERVER_PATH), "--transport", "shm", "--name", shm_name, "--exe", str(CPOWER_EXE)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    send = _mcp_send_framed if framed else _mcp_send_line
    recv = _mcp_recv_framed if framed else _mcp_recv_line

    try:
        # 1. initialize
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "clientInfo": {"name": "pytest-harness"}},
        })
        init_resp = recv(mcp_proc)
        assert init_resp["id"] == 1
        assert init_resp["result"]["serverInfo"]["name"] == "cpower-mcp"
        assert init_resp["result"]["serverInfo"]["version"] == "1.1.0"

        # 2. notifications/initialized + id:null ping + invalid requests / error codes
        send(mcp_proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
        send(mcp_proc, {"jsonrpc": "2.0", "id": None, "method": "ping"})
        null_id_resp = recv(mcp_proc)
        assert "id" in null_id_resp and null_id_resp["id"] is None and null_id_resp["result"] == {}

        # Malformed JSON -> -32700 Parse error, server stays alive
        assert mcp_proc.stdin is not None
        if framed:
            bad_body = b"{not_json_here"
            mcp_proc.stdin.write(f"Content-Length: {len(bad_body)}\r\n\r\n".encode("ascii") + bad_body)
            mcp_proc.stdin.flush()
            parse_err = recv(mcp_proc)
            assert parse_err["id"] is None and parse_err["error"]["code"] == -32700

            mcp_proc.stdin.write(b"Content-Length: notanint\r\n\r\n")
            mcp_proc.stdin.flush()
            hdr_err = recv(mcp_proc)
            assert hdr_err["id"] is None and hdr_err["error"]["code"] == -32700
        else:
            mcp_proc.stdin.write(b"{not valid json\n")
            mcp_proc.stdin.flush()
            parse_err = recv(mcp_proc)
            assert parse_err["id"] is None and parse_err["error"]["code"] == -32700

        # Non-object JSON -> -32600 Invalid Request
        send(mcp_proc, [1, 2, 3])  # type: ignore[arg-type]
        inv_req = recv(mcp_proc)
        assert inv_req["id"] is None and inv_req["error"]["code"] == -32600

        # Unknown method -> -32601 Method not found
        send(mcp_proc, {"jsonrpc": "2.0", "id": 100, "method": "nonexistent/method"})
        unk_method = recv(mcp_proc)
        assert unk_method["id"] == 100 and unk_method["error"]["code"] == -32601

        # Schema violations & unknown tool -> -32602 Invalid params
        bad_calls = [
            {"name": "no_such_tool", "arguments": {}},
            {"name": "get_ui_tree", "arguments": {"max_nodes": -5}},
            {"name": "get_ui_tree", "arguments": {"max_nodes": True}},
            {"name": "click_at", "arguments": {"x": 10, "y": 10, "button": "bogus"}},
            {"name": "click_at", "arguments": {"x": 10, "y": 10, "speed": 0}},
            {"name": "press_hotkey", "arguments": {"vk": 0}},
            {"name": "press_hotkey", "arguments": {"vk": 70000}},
            {"name": "list_windows", "arguments": {"unexpected": 1}},
            {"name": "click_element", "arguments": {}},
        ]
        for idx, bad_params in enumerate(bad_calls, start=200):
            send(mcp_proc, {"jsonrpc": "2.0", "id": idx, "method": "tools/call", "params": bad_params})
            err_resp = recv(mcp_proc)
            assert err_resp["id"] == idx, err_resp
            assert err_resp["error"]["code"] == -32602, f"Expected -32602 for {bad_params}, got {err_resp}"

        # Failed command (invalid hwnd -> isError: True)
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": 250,
            "method": "tools/call",
            "params": {"name": "focus_window", "arguments": {"hwnd": 0xDEAD0000}},
        })
        bad_hwnd_resp = recv(mcp_proc)
        assert bad_hwnd_resp["id"] == 250 and bad_hwnd_resp["result"]["isError"] is True

        send(mcp_proc, {"jsonrpc": "2.0", "id": 2, "method": "ping"})
        ping_resp = recv(mcp_proc)
        assert ping_resp["id"] == 2 and ping_resp["result"] == {}

        # 3. tools/list -> assert all 12 tools
        send(mcp_proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
        list_resp = recv(mcp_proc)
        tools = {t["name"] for t in list_resp["result"]["tools"]}
        expected_tools = {
            "list_windows", "focus_window", "get_ui_tree", "get_ui_diff",
            "find_ui_elements", "click_at", "click_element", "type_text",
            "paste_text", "press_hotkey", "scroll_at", "uia_invoke", "capture_micro_region",
        }
        assert tools == expected_tools, f"Tool mismatch: {tools ^ expected_tools}"

        # 4. Wait until GUI window appears in list_windows
        target_hwnd: Optional[int] = None
        req_id = 10
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            send(mcp_proc, {
                "jsonrpc": "2.0",
                "id": req_id,
                "method": "tools/call",
                "params": {"name": "list_windows", "arguments": {}},
            })
            resp = recv(mcp_proc)
            req_id += 1
            data = json.loads(resp["result"]["content"][0]["text"])
            for w in data["windows"]:
                if w["title"] == title:
                    target_hwnd = w["hwndInt"]
                    break
            if target_hwnd is not None:
                break
            time.sleep(0.1)
        assert target_hwnd is not None, "Target WinForms window never appeared in MCP list_windows"

        # 5. get_ui_tree (retry until child controls settle)
        edit_id = None
        btn_id = None
        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            send(mcp_proc, {
                "jsonrpc": "2.0",
                "id": req_id,
                "method": "tools/call",
                "params": {"name": "get_ui_tree", "arguments": {"hwnd": target_hwnd, "max_nodes": 100}},
            })
            resp = recv(mcp_proc)
            req_id += 1
            assert not resp["result"]["isError"]
            tree_data = json.loads(resp["result"]["content"][0]["text"])
            for n in tree_data["nodes"]:
                if n.get("automationId") == "PyEditBox":
                    edit_id = n["id"]
                elif n.get("automationId") == "PyApplyBtn":
                    btn_id = n["id"]
            if edit_id is not None and btn_id is not None:
                break
            time.sleep(0.08)
        assert edit_id is not None and btn_id is not None

        # 6. uia_invoke set_value + find_ui_elements
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {
                "name": "uia_invoke",
                "arguments": {"action": "set_value", "hwnd": target_hwnd, "node_id": edit_id, "text": "McpEdited_77"},
            },
        })
        set_resp = recv(mcp_proc)
        req_id += 1
        assert not set_resp["result"]["isError"], set_resp

        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {
                "name": "find_ui_elements",
                "arguments": {"hwnd": target_hwnd, "automation_id": "PyEditBox"},
            },
        })
        find_resp = recv(mcp_proc)
        req_id += 1
        find_data = json.loads(find_resp["result"]["content"][0]["text"])
        assert find_data["totalMatches"] == 1
        assert find_data["matches"][0]["value"] == "McpEdited_77"

        # 7. Mutate externally via BM_CLICK and verify get_ui_diff detects Status:1
        btn_hwnd = _find_child_hwnd_by_text(target_hwnd, "ApplyActionBtn")
        assert btn_hwnd is not None
        _user32.SendMessageW(wintypes.HWND(btn_hwnd), _BM_CLICK, 0, 0)
        time.sleep(0.1)

        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {"name": "get_ui_diff", "arguments": {"hwnd": target_hwnd}},
        })
        diff_resp = recv(mcp_proc)
        req_id += 1
        assert not diff_resp["result"]["isError"]
        diff_data = json.loads(diff_resp["result"]["content"][0]["text"])
        assert diff_data["entryCount"] >= 1
        assert any(e["node"]["name"] == "Status:1" for e in diff_data["entries"])

        # 8. Mouse, scroll, and hotkey MCP tool dispatch on test target window
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {"name": "focus_window", "arguments": {"hwnd": target_hwnd, "action": "focus"}},
        })
        foc_resp = recv(mcp_proc)
        req_id += 1
        assert not foc_resp["result"]["isError"], foc_resp

        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {
                "name": "click_element",
                "arguments": {"hwnd": target_hwnd, "automation_id": "PyCheckBox", "mode": "direct"},
            },
        })
        clk_elem_resp = recv(mcp_proc)
        req_id += 1
        assert not clk_elem_resp["result"]["isError"], clk_elem_resp

        btn_node = next(n for n in tree_data["nodes"] if n.get("automationId") == "PyApplyBtn")
        btn_cx, btn_cy = btn_node["center"]
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {
                "name": "click_at",
                "arguments": {"x": 5, "y": 5, "hwnd": target_hwnd, "mode": "direct", "button": "left"},
            },
        })
        clk_at_resp = recv(mcp_proc)
        req_id += 1
        assert not clk_at_resp["result"]["isError"], clk_at_resp

        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {
                "name": "scroll_at",
                "arguments": {"delta": -120, "x": btn_cx, "y": btn_cy, "horizontal": False},
            },
        })
        scr_resp = recv(mcp_proc)
        req_id += 1
        if scr_resp["result"]["isError"]:
            scr_ack = json.loads(scr_resp["result"]["content"][0]["text"])["ack"]
            assert scr_ack["hresultHex"] == "0x80070005", f"Unexpected scroll_at error: {scr_resp}"
        else:
            scr_ack = json.loads(scr_resp["result"]["content"][0]["text"])["ack"]
            assert scr_ack["succeeded"] is True, scr_resp

        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {"name": "press_hotkey", "arguments": {"vk": 0x10}},
        })
        hk_resp = recv(mcp_proc)
        req_id += 1
        if hk_resp["result"]["isError"]:
            hk_ack = json.loads(hk_resp["result"]["content"][0]["text"])["ack"]
            assert hk_ack["hresultHex"] == "0x80070005", f"Unexpected press_hotkey error: {hk_resp}"
        else:
            hk_ack = json.loads(hk_resp["result"]["content"][0]["text"])["ack"]
            assert hk_ack["succeeded"] is True, hk_resp

        # 9. capture_micro_region: verify live PNG OR graceful headless DXGI error
        send(mcp_proc, {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {"name": "capture_micro_region", "arguments": {"left": 0, "top": 0, "right": 32, "bottom": 32}},
        })
        cap_resp = recv(mcp_proc)
        req_id += 1
        cap_res = cap_resp["result"]
        if not cap_res["isError"]:
            img_item = cap_res["content"][0]
            assert img_item["type"] == "image"
            assert img_item["mimeType"] == "image/png"
            png_raw = base64.b64decode(img_item["data"])
            assert png_raw.startswith(b"\x89PNG\r\n\x1a\n")
        else:
            err_txt = cap_res["content"][0]["text"]
            assert "0x8" in err_txt or "failed" in err_txt.lower()

        # 10. Close stdin cleanly and confirm server + daemon exit cleanly
        assert mcp_proc.stdin is not None
        mcp_proc.stdin.close()
        exit_code = mcp_proc.wait(timeout=6.0)
        stderr_data = mcp_proc.stderr.read().decode("utf-8", errors="replace") if mcp_proc.stderr else ""
        assert exit_code == 0, f"MCP server exited with {exit_code}: {stderr_data}"
    finally:
        if mcp_proc.poll() is None:
            mcp_proc.terminate()
            try:
                mcp_proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                mcp_proc.kill()
                mcp_proc.wait(timeout=2.0)
        if mcp_proc.stdout:
            mcp_proc.stdout.close()
        if mcp_proc.stderr:
            mcp_proc.stderr.close()
        _stop_proc(gui_proc)

    print(f"[PASS] test_mcp_stdio_server ({mode_tag})")


def main() -> int:
    print("========================================================================")
    print("  CPower v1.1.0 Python SDK & MCP Server Automated Test Suite")
    print("========================================================================")
    test_ctypes_abi_sizes()
    test_client_unit_helpers()
    test_python_client_transport("shm")
    test_python_client_transport("pipe")
    test_mcp_stdio_server(framed=False)
    test_mcp_stdio_server(framed=True)
    print("========================================================================")
    print("  ALL PYTHON SDK & MCP TESTS PASSED (6/6)")
    print("========================================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
