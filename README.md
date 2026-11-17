# CPower v1.1.0 — Native Pure C11 Windows Computer-Use Platform

**CPower** هو محرك تحكم وإدراك أصيل لنظام ويندوز مكتوب بلغة **Pure ISO C11** بدون أي مكتبات خارجية (Zero Third-Party Dependencies)، ومزود بـ **Python 3.12 Zero-Dependency SDK** و **Standalone MCP Server** عبر `stdio`.

Pure C11 Windows-native Computer-Use Agent platform featuring lock-free Shared Memory SPSC & Named Pipes IPC, UIA COM perception + semantic delta diffing, bio-realistic human emulation + direct COM control, multi-monitor DXGI VRAM screen capture, optional opt-in **GDI/User32 IAT** text-capture hook, zero-dependency Python 3.12 SDK, and a standalone Model Context Protocol (`stdio`) server.

---

## Architecture / البنية المعمارية

| Subsystem | Files | Capabilities |
| :--- | :--- | :--- |
| **Perception Engine** | `src/perception/uia_tree.c`, `tree_diff.c`, `winevent_hook.c`, `fallback_text.c`, `dxgi_capture.c` | Multi-threaded COM (`COINIT_MULTITHREADED`) UIA extraction, `Perception_FindNodes` semantic locator, FNV-1a tree delta diffing (`Perception_DiffTrees`), real-time WinEvent streaming, GDI/WM_GETTEXT fallback, and multi-monitor virtual-desktop stitched **DXGI Desktop Duplication** capture with pure-C in-memory BMP & lossless PNG (RFC 1950/1951/CRC-32) encoding. |
| **Dual-Mode Control Engine** | `src/input/direct_control.c`, `bezier_mouse.c`, `human_keyboard.c`, `window_ctrl.c` | Direct Instant Mode (COM UIA `Invoke`, `Value`, `Toggle`, `SelectionItem` patterns + deadlock-guarded `SendMessageTimeoutW`), Human Emulation Mode (cubic Bezier + 5th-order minimum-jerk + Ornstein-Uhlenbeck tremor + Box-Muller keystroke timing + `SendInput` scroll/hscroll), top-level window enum/focus/show, emergency top-left/Pause failsafe, and closed-loop `Control_ExecuteAndVerify`. |
| **Low-Latency IPC** | `src/ipc/shm_transport.c`, `pipe_transport.c`, `include/ipc.h` | Lock-free SPSC dual circular Shared Memory rings (64-byte cache-line isolation, power-of-2 bitmask wrapping, cross-integrity `D:(A;;GA;;;WD)S:(ML;;NW;;;LW)` SDDL), message-mode Named Pipes fallback, plus `AckPacket` (`ACKP`), `EventPacket` (`EVNT`), `DiffPacketHeader` (`DIFF`), and `WindowPacketHeader` (`WNDS`). |
| **Security & UIPI Bypass** | `src/security/privilege.c`, `res/app.manifest`, `build.ps1` | Self-signed local root CA (`CN=CPowerLocalRoot`) + PE Authenticode signing for `bin/cpower_uiaccess.exe` (`uiAccess="true"`). Inspects token integrity RID, UIAccess privilege, `EnableSecureUIAPaths`, and UIPI boundary before interacting with elevated windows. |
| **Opt-In Text Hook (x64)** | `src/hooks/gdi_dwrite_hook.c`, `src/hooks/hook_injector.c` | Strictly opt-in (`cpower hook --pid <PID>`) x64 **GDI/User32 IAT** hook DLL (`bin/cpower_hook64.dll`) intercepting `gdi32.dll` (`ExtTextOutW`, `TextOutW`) and `user32.dll` (`DrawTextW`, `DrawTextExW`) into `Local\CPowerTextHook_<pid>` with deterministic IAT unpatching + `FreeLibrary` unload on `--detach`. |
| **Python 3.12 SDK & MCP** | `clients/python/cpower_client.py`, `mcp/cpower_mcp_server.py` | Pure stdlib + `ctypes` binary protocol client (`shm` and `pipe`) and 12-tool JSON-RPC 2.0 MCP stdio server (supports both newline-delimited JSON-RPC and `Content-Length` headers). |

---

## Build, Code Signing & Verification / البناء والتوقيع والفحص

### 1. Build with Make (MinGW-w64 GCC `-Wall -Wextra -Werror -pedantic -std=c11`)
```powershell
mingw32-make clean all
```
Produces:
- `bin/cpower.exe` (Standard development CLI & daemon, `uiAccess="false"`)
- `bin/cpower_uiaccess.exe` (`uiAccess="true"` UIPI-capable binary; must be signed before launch)
- `bin/cpower_hook64.dll` (Opt-in GDI/User32 IAT text-capture DLL)
- `tests/test_runner.exe` (Native C test harness)

### 2. Sign Binaries & Run Verification Audit
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Sign -Verify
```
Checks:
- Zero non-OS DLL imports (`objdump -p` import audit).
- Valid Authenticode signatures on `bin\cpower.exe`, `bin\cpower_uiaccess.exe`, and `bin\cpower_hook64.dll` against `Cert:\LocalMachine\Root` (`CN=CPowerLocalRoot`).
- Runs `bin\cpower.exe verify`.

> **Important Note on Launching `bin\cpower_uiaccess.exe` (خطأ 740 / UIAccess):**
> Executables with `uiAccess="true"` **must** be launched via `ShellExecute` / `ShellExecuteW` (e.g. Windows Explorer, `Start-Process` in PowerShell, or `ShellExecuteExW`), **not** via raw `CreateProcess` directly from a non-UI-access process — direct execution may fail with **Win32 Error 740 (`ERROR_ELEVATION_REQUIRED`)** or strip `TokenUIAccess`. Furthermore, when `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\EnableSecureUIAPaths` is `1` (Windows default), Windows only grants `TokenUIAccess=TRUE` if the signed binary resides in a trusted secure directory such as `C:\Program Files\CPower\`. Setting `EnableSecureUIAPaths=0` allows development testing directly from `bin\cpower_uiaccess.exe`.

---

## CLI Reference / أوامر سطر الأوامر

```text
cpower.exe <subcommand> [options]

  tree     [--hwnd <HEX|DEC>] [--desktop] [--format indent|json|binary]
  find     [--hwnd H] [--desktop] [--name SUB] [--id AUTO_ID] [--class CLS]
           [--type Button|Edit|CheckBox|Text|Window|...] [--enabled] [--visible] [--json]
  diff     [--hwnd H] [--desktop] [--delay <ms>] [--format json|text]
  windows  [--json]
  focus    --hwnd <HEX|DEC> [--action focus|restore|minimize|maximize|close]
  events   [--duration <ms>]
  action   --cmd <click|double_click|right_click|move|drag|scroll|hscroll|
                  type|key|invoke|set_value|toggle|select|direct_click|direct_text>
           [--x N] [--y N] [--to-x N] [--to-y N] [--delta N] [--node ID]
           [--hwnd H] [--text STR] [--vk N] [--mods ctrl+shift]
           [--mode direct|human] [--speed F] [--verify]
  capture  --left L --top T --right R --bottom B --out <path.bmp|path.png>
  hook     --pid <PID> [--inject] [--read] [--detach] [--duration <ms>] [--dll <path>]
  daemon   [--transport shm|pipe] [--name <name>] [--duration <ms>] [--stream-events]
  verify        Run OS, COM/UIA, Control, Security/UIPI, and IPC self-checks
  version       Display cpower engine version (1.1.0) (aliases: --version, -v)
  --help        Display CLI usage and subcommand options (aliases: -h, help)
```

### Opt-In GDI/User32 IAT Text Hook Example (`cpower_hook64.dll`)
`cpower_hook64.dll` provides strictly opt-in **GDI/User32 IAT** text capture (`ExtTextOutW`, `TextOutW`, `DrawTextW`, `DrawTextExW`). It is **never** loaded globally (`AppInit_DLLs` is never used) and only attaches to an explicit PID when requested:
```powershell
.\bin\cpower.exe hook --pid 12345 --inject
.\bin\cpower.exe hook --pid 12345 --read --duration 3000
.\bin\cpower.exe hook --pid 12345 --detach
```

---

## Python 3.12 Zero-Dependency SDK

> **Import Note (Repo-Root Independent):** When importing from outside the repository root, add `clients/python` to `PYTHONPATH` (or `sys.path.insert(0, r"h:\c power\clients\python")`) and pass `exe_path=r"h:\c power\bin\cpower.exe"` if `cpower.exe` is not in the default relative `bin/` directory.

```python
import sys
from pathlib import Path

# Works regardless of current working directory
sys.path.insert(0, str(Path(r"h:\c power\clients\python")))
from cpower_client import CPowerClient

with CPowerClient(transport="shm", exe_path=r"h:\c power\bin\cpower.exe", stream_events=True) as client:
    windows = client.list_windows()
    if not windows:
        print("No visible top-level windows found.")
    else:
        hwnd = windows[0]["hwndInt"]
        matches = client.find_nodes(control_type="Edit", hwnd=hwnd)
        if matches:
            client.uia_set_value("Hello from Python 3.12", node_id=matches[0]["id"], hwnd=hwnd)
        diff = client.diff_tree(hwnd=hwnd)
        print(diff["entryCount"], diff["entries"])
```

---

## Standalone MCP Server (`stdio`)

Add to your MCP client configuration (Antigravity / Claude Desktop / Cursor / VS Code):

```json
{
  "mcpServers": {
    "cpower": {
      "command": "python",
      "args": [
        "h:\\c power\\mcp\\cpower_mcp_server.py",
        "--transport", "auto"
      ]
    }
  }
}
```

The server automatically detects whether an interactive `cpower` daemon is already running (via lock-free Shared Memory or Named Pipes) or auto-spawns one on demand.

Available MCP tools (12):
`list_windows`, `focus_window`, `get_ui_tree`, `get_ui_diff`, `find_ui_elements`, `click_at`, `click_element`, `type_text`, `press_hotkey`, `scroll_at`, `uia_invoke`, `capture_micro_region`.

---

## Interactive Desktop Daemon / خدمة سطح المكتب التفاعلية

To control the active user desktop (Session 1) with full `uiAccess="true"` token privileges (enabling seamless control over elevated and administrative windows):

```powershell
# Check daemon status and IPC connectivity
.\scripts\manage_interactive_daemon.ps1 -Action Status

# Start interactive daemon in background (Session 1)
.\scripts\manage_interactive_daemon.ps1 -Action Start

# Optional: Install as a scheduled task to run automatically at user logon
.\scripts\manage_interactive_daemon.ps1 -Action InstallTask

# Stop running daemon
.\scripts\manage_interactive_daemon.ps1 -Action Stop
```

Quick-launch batch shortcuts are also provided in `scripts/`:
- `scripts\start_daemon.bat`: Double-click to start daemon.
- `scripts\status_daemon.bat`: Double-click to inspect status & active sessions.
- `scripts\stop_daemon.bat`: Double-click to stop daemon.

---

## Automated Tests / الاختبارات التلقائية

```powershell
# C11 Native Unit, Stress, Security, Architecture & E2E Tests
.\tests\test_runner.exe --all

# Python 3.12 ABI, SHM/Pipe SDK & MCP stdio Server Tests
python .\tests\test_python_mcp.py

# Interactive Desktop Smoke Verification
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_live_smoke.ps1
```
