# Architectural Blueprint & Engineering Specification
## Pure C Windows-Native Computer-Use Agent Subsystem

**Document Version:** 1.0.0  
**Target Platform:** Windows 10 / Windows 11 (x86_64)  
**Language Standard:** ISO C99 / C11  
**Runtime Dependencies:** None (Zero third-party DLLs; standard Windows System DLLs only)  
**Binary Output:** `bin/cpower.exe` (~25 KB - 150 KB standalone native executable)  

---

## 1. Executive Summary & Architectural Overview

The **cpower** engine is an ultra-low-latency, zero-external-dependency Windows-native Computer-Use agent foundation designed for autonomous AI agents and programmatic robotic desktop control. Traditional desktop automation tools rely on either resource-heavy periodic full-screen raster captures (consuming gigabytes of memory bandwidth per second and hundreds of milliseconds of vision model processing) or fragile accessibility wrappers with significant overhead. 

**cpower** operates on an inverted perception-control paradigm:
1. **Zero-Screenshot Semantic Perception**: Interfaces directly with the Windows accessibility subsystem via native COM `IUIAutomation` vtables in pure C, retrieving structured semantic UI trees in 1–15 milliseconds without taking screenshots. Window state transitions and focus changes are streamed in real time via an out-of-context `SetWinEventHook` dispatcher thread with zero polling overhead. Micro-region raster capture via DXGI Direct3D 11 staging textures is invoked solely on-demand for unidentifiable custom canvases or icon verification.
2. **Native Dual-Mode Control Engine**: Combines **Direct Instant Mode** (sub-millisecond programmatic COM pattern invocation and deadlock-guarded Win32 window message dispatch) with **Human Emulation Mode** (anatomically curved cubic Bézier paths, 5th-order minimum-jerk velocity profiles, Ornstein-Uhlenbeck mean-reverting neuromuscular tremor, and Box-Muller log-normal cognitive delay distributions) dispatched via low-level `SendInput`.
3. **Privilege Handling & UIPI Bypass**: Leverages the official Microsoft `uiAccess="true"` accessibility privilege architecture coupled with automated Authenticode local root certificate signing and development registry bypassing (`EnableSecureUIAPaths = 0`). This allows standard-rights processes to inspect, manipulate, and send synthetic input to elevated (High Integrity) administrative windows without triggering intrusive UAC consent dialogs on every launch.
4. **Sub-Millisecond Lock-Free IPC Architecture**: Communicates with external AI runtimes (Python, Rust, Go, Node.js) via a dual Single-Producer Single-Consumer (SPSC) circular shared memory ring buffer with 64-byte cache-line isolation, power-of-two bitmask indexing, cross-integrity SDDL security descriptors (`D:(A;;GA;;;WD)S:(ML;;NW;;;LW)`), and flat contiguous binary semantic tree serialization for zero-copy deserialization. A message-mode Named Pipe server provides an out-of-box cross-container fallback.
5. **Zero-Dependency Compilation Model**: 100% pure C99/C11 adhering strictly to standard Windows OS system imports (`KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `ole32.dll`, `OLEAUT32.dll`, `UIAutomationCore.dll`, `dxgi.dll`, `d3d11.dll`, `dwrite.dll`, `ADVAPI32.dll`, and native Universal C Runtime). Multi-toolchain build system supports MinGW-w64 GCC and MSVC `cl.exe`.

---

## 2. Global Architecture & Subsystem Layout

```
+===================================================================================================+
|                                    cpower System Architecture                                     |
+===================================================================================================+
|                                                                                                   |
|  +---------------------------------------------------------------------------------------------+  |
|  |                                  External AI Runtime / Client                               |  |
|  |                            (Python / Rust / TypeScript / Go / REST / RPC)                   |  |
|  +------------------------------+----------------------------------^---------------------------+  |
|                                 | Action Commands                  | Semantic Trees & Events      |
|                                 | (IPC_CommandPacket)              | (IPC_SemanticTreePacket)     |
|                                 v                                  |                              |
|  +=================================================================+===========================+  |
|  |                             Pillar 4: Low-Latency IPC Subsystem                             |  |
|  |  +----------------------------------------------------+  +-------------------------------+  |  |
|  |  |      SPSC Dual Shared Memory Ring Buffer           |  |      Named Pipes Fallback     |  |  |
|  |  |   - 64-byte Cache Line Padding (False Sharing)     |  |   - Message-Mode Framing      |  |  |
|  |  |   - Power-of-Two Bitmask Wrapping (idx & mask)     |  |   - FILE_FLAG_WRITE_THROUGH   |  |  |
|  |  |   - SDDL Cross-Integrity: D:(A;;GA;;;WD)S:(ML;;NW;;;LW) | - Zero Stream Framing Lag   |  |  |
|  |  +----------------------------------------------------+  +-------------------------------+  |  |
|  +=============================================================================================+  |
|                                 |                                  ^                              |
|                                 v                                  |                              |
|  +=================================================================+===========================+  |
|  |                                cpower Core Dispatcher & Daemon                              |  |
|  +=============================================================================================+  |
|              |                                                             |                      |
|              v                                                             v                      |
|  +=======================================+     +===============================================+  |
|  | Pillar 2: Dual-Mode Control Engine    |     | Pillar 1: Zero-Screenshot Perception Engine   |  |
|  |                                       |     |                                               |  |
|  | [Direct Instant Mode]                 |     | [UIA Semantic Tree Extractor]                 |  |
|  | - Pure C COM Pattern Dispatch:        |     | - Pure C COM vtable via COBJMACROS            |  |
|  |   Invoke, Value, Toggle, Selection    |     | - MTA Threading (CoInitializeEx Multithreaded)|  |
|  | - Deadlock-Guarded Win32 Messaging:   |     | - Bounded Iterative Traversal (Stack Depth 64)|  |
|  |   SendMessageTimeoutW (SMTO_ABORT)    |     | - get_ControlViewWalker & Targeted HWND Root  |  |
|  |   PostMessageW                        |     | - Flat Binary Serialization (CompactUiTree)   |  |
|  |                                       |     |                                               |  |
|  | [Human Emulation Mode]                |     | [Real-Time WinEvent Hook Dispatcher]          |  |
|  | - Cubic Bézier Path Synthesis         |     | - Out-of-Context SetWinEventHook Pump (STA)   |  |
|  | - 5th-Order Minimum-Jerk Velocity:    |     | - MsgWaitForMultipleObjectsEx Message Pump    |  |
|  |   s(tau) = 10*tau^3 - 15*tau^4 + 6*tau^5 | - Real-time Focus, State, Window Events   |  |
|  | - Ornstein-Uhlenbeck Motor Tremor:    |     |                                               |  |
|  |   dJ_t = -theta*J_t*dt + sigma*dW_t   |     | [Fallback Text Extraction]                    |  |
|  |   with Target Proximity Damping       |     | - GDI WM_GETTEXT & EM_GETLINE (Word-Sized)    |  |
|  | - Box-Muller Log-Normal Timing Delays |     | - Cross-Process SysListView32 Memory Reading  |  |
|  | - SendInput Physical Event Synthesis  |     | - Chromium / Electron Accessibility Poke      |  |
|  |   with Virtual Desktop Coord Normal.  |     |                                               |  |
|  +=======================================+     | [Micro-Region DXGI VRAM Capture]              |  |
|                                                | - Direct3D 11 & DXGI 1.2 Output Duplication   |  |
|                                                | - GPU-Side Subresource Blit (CopySubresource) |  |
|                                                | - Zero-Dependency In-Memory BMP & PNG Encoder |  |
|                                                +===============================================+  |
|                                                                                                   |
|  +=============================================================================================+  |
|  | Pillar 3: Privilege Handling & UIPI Bypass                                                  |  |
|  | - Embedded Application Manifest: requestedExecutionLevel level="asInvoker" uiAccess="true"  |  |
|  | - Authenticode Digital Signature chained to LocalMachine\Root & TrustedPublisher            |  |
|  | - Local Self-Signed Certificate Generation & Automated PowerShell Signing Pipeline          |  |
|  | - Development Security Bypass: HKLM\...\System\EnableSecureUIAPaths = 0                     |  |
|  | - Token Integrity Inspection: TOKEN_UIACCESS & Mandatory Integrity Label Validation         |  |
|  +=============================================================================================+  |
|                                                                                                   |
|  +=============================================================================================+  |
|  | Pillar 5: Zero-Dependency Compilation & Build Architecture                                  |  |
|  | - Pure C99/C11 Source Tree: src/perception, src/input, src/ipc, src/security, src/common     |  |
|  | - Multi-Toolchain Support: MinGW-w64 GCC 16.1.0 & MSVC cl.exe Auto-Detection                |  |
|  | - Unified Windows Headers & Link Imports: kernel32, user32, gdi32, ole32, oleaut32,         |  |
|  |   uiautomationcore, dxgi, d3d11, dwrite, advapi32                                          |  |
|  +=============================================================================================+  |
+===================================================================================================+
```

---

## 3. Pillar 1: Zero-Screenshot Semantic Perception Engine

### 3.1 Pure C COM vtable Architecture & `COBJMACROS`
COM interfaces in pure C are structured as typed pointers to virtual function tables (`lpVtbl`). To invoke a COM method without C++ compiler mangling, the interface pointer (`This`) is passed explicitly as the first argument.

```c
/* Manual Vtable Invocation */
IUIAutomation *pAutomation = NULL;
IUIAutomationElement *pRoot = NULL;
HRESULT hr = pAutomation->lpVtbl->GetRootElement(pAutomation, &pRoot);
```

To optimize developer ergonomics and eliminate boilerplate, `cpower` enforces `#define COBJMACROS` before including `<uiautomationclient.h>`:

```c
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <uiautomationclient.h>

/* Expands directly to (pAutomation)->lpVtbl->GetRootElement(pAutomation, &pRoot) */
HRESULT hr = IUIAutomation_GetRootElement(pAutomation, &pRoot);
```

#### Standalone GUID Resolution (Zero Linkage on `uuid.lib`)
To avoid linker dependencies on SDK static libraries (`uuid.lib`), the engine includes `<initguid.h>` or provides static definitions:
```c
static const GUID CLSID_CUIAutomation_Manual = 
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID IID_IUIAutomation_Manual = 
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
```

### 3.2 COM Threading Architecture: MTA Traversal vs. STA WinEvent Pump
The perception engine utilizes a multi-threaded architecture with decoupled COM apartments:
- **MTA (Multi-Threaded Apartment) for Tree Walking**:
  `CoInitializeEx(NULL, COINIT_MULTITHREADED)`. Windows UI Automation runs via an out-of-process COM server (`uiautomationcore.dll`). Traversal calls execute concurrently without requiring a Windows message pump and cannot be deadlocked by target window message stalls.
- **STA (Single-Threaded Apartment) for WinEvent Hook Thread**:
  `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`. Windows WinEvent hooks (`SetWinEventHook`) with `WINEVENT_OUTOFCONTEXT` deliver event notifications via thread message queues (`WM_WINEVENT`). The hook thread executes a standard `MsgWaitForMultipleObjectsEx` message loop, pushing events into an SPSC lock-free queue.

### 3.3 Targeted Bounded Iterative Traversal
Walking the entire Desktop element (`GetRootElement`) takes 300ms–1200ms and returns thousands of irrelevant background nodes. `cpower` targets the active foreground window (`GetForegroundWindow()`) and resolves the root element via `ElementFromHandle`:

```c
HWND hwndForeground = GetForegroundWindow();
IUIAutomationElement *pTargetRoot = NULL;
HRESULT hr = IUIAutomation_ElementFromHandle(pAutomation, (UIA_HWND)hwndForeground, &pTargetRoot);
```

#### Tree Walker Selection: `get_ControlViewWalker`
- `RawViewWalker`: Contains structural layout boxes, non-semantic containers, and invisible clipping nodes.
- `ContentViewWalker`: Strips decorative nodes but frequently omits interactive controls lacking textual labels.
- `ControlViewWalker`: **Optimal.** Preserves all interactive controls (Buttons, Checkboxes, Sliders, Edits, Trees, Menus) while discarding layout wrappers.

#### Bounded Stack-Overflow Proof Iteration
Deep DOM trees in Chromium and Electron applications (VS Code, Slack) can exceed 60 nesting levels. A recursive C function risks thread stack overflow. The engine implements an explicit bounded iterative stack:

```c
#define CPOWER_MAX_TREE_DEPTH 64
#define CPOWER_MAX_NODES_PER_WINDOW 4096

typedef struct {
    IUIAutomationElement *pElement;
    int depth;
    int parentIndex;
} TraversalStackFrame;

typedef struct {
    TraversalStackFrame frames[CPOWER_MAX_TREE_DEPTH];
    int top;
} TraversalStack;
```

```
[Iterative Tree Traversal State Machine]
  
  +------------------+
  | Push Target Root |
  +--------+---------+
           |
           v
  +--------+---------+      Stack Empty
  |  Is Stack Empty? |-------------------------> [Complete: Return SemanticTree]
  +--------+---------+
           | No
           v
  +------------------+
  |  Pop Frame curr  |
  +--------+---------+
           |
           v
  +-------------------------------------------------------+
  | Extract Properties (Name, Type, Bounds, Flags, Value) |
  | Append to SemanticNode Array -> Assign Node Index     |
  +------------------------+------------------------------+
                           |
                           v
              +------------+------------+
              | Depth + 1 < MAX_DEPTH?  |
              +------------+------------+
                           | Yes
                           v
  +-------------------------------------------------------+
  | Query FirstChild -> Enumerate Siblings                |
  | Push Children to Stack in Reverse Order (Natural DFS) |
  +------------------------+------------------------------+
                           |
                           v
  +-------------------------------------------------------+
  | IUIAutomationElement_Release(curr.pElement)           |
  +------------------------+------------------------------+
                           |
                           +---------> [Loop back to Is Stack Empty?]
```

### 3.4 In-Memory Semantic Tree & Binary Serialization
The semantic tree is stored in a compact format designed for zero-allocation serialization:

```c
#pragma pack(push, 1)

typedef struct {
    uint32_t id;               /* Unique node index */
    int32_t  parentId;         /* -1 if root */
    uint16_t depth;            /* Nesting level [0..63] */
    uint16_t controlTypeId;    /* UIA_*ControlTypeId */
    RECT     bounds;           /* Bounding rectangle in screen coordinates */
    uint32_t flags;            /* Bit 0: Enabled, Bit 1: Offscreen, Bit 2: Focused, Bit 3: Password */
    WCHAR    name[128];        /* Friendly element name */
    WCHAR    className[64];    /* Win32 window class / UIA class */
    WCHAR    automationId[64]; /* Developer-assigned ID */
    WCHAR    value[128];       /* Input value or text content */
} SemanticNode;

typedef struct {
    uint32_t     nodeCount;
    uint32_t     maxNodes;
    HWND         rootHwnd;
    uint64_t     timestampUtc;
    SemanticNode *nodes;
} SemanticTree;

#pragma pack(pop)
```

### 3.5 Real-Time WinEvent Hook Dispatcher
Event-driven perception eliminates polling. The engine installs an out-of-context hook via `SetWinEventHook`:

```c
HWINEVENTHOOK hHook = SetWinEventHook(
    EVENT_SYSTEM_FOREGROUND,
    EVENT_OBJECT_VALUECHANGE,
    NULL,
    WinEventProcCallback,
    0,
    0,
    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
);
```

#### Event Action Matrix
| Event Constant | Hex ID | Action Triggered in Perception Engine |
|---|---|---|
| `EVENT_SYSTEM_FOREGROUND` | `0x0003` | Foreground window switched. Invalidate cached tree; capture active window root; emit new tree packet. |
| `EVENT_OBJECT_FOCUS` | `0x8005` | Input focus changed (`idChild`, `idObject`). Update focused node ID in memory without full tree re-traversal. |
| `EVENT_SYSTEM_MOVESIZEEND` | `0x000B` | Target window repositioned or resized. Refresh bounding boxes of active window elements. |
| `EVENT_OBJECT_CREATE` | `0x8000` | New window, dialog, or popup menu opened. Flag active tree as dirty. |
| `EVENT_OBJECT_DESTROY` | `0x8001` | Window or popup closed. Prune orphaned subtree. |
| `EVENT_OBJECT_VALUECHANGE` | `0x800E` | User typed in edit box or changed slider. Update cached value attribute for target node. |

High-frequency cursor movements (`OBJID_CURSOR = 0xFFFFFFF7`) and audio alerts are dropped immediately inside the callback.

### 3.6 Fallback Text Extraction for Non-UIA Applications
When `IUIAutomationTreeWalker::GetFirstChildElement` returns `NULL` on an active window, or standard controls return empty UIA names, fallback text extraction activates:
1. **Standard Win32 Controls**: `WM_GETTEXT` for single-line edits, buttons, and static labels.
2. **Multiline Edit & RichEdit Controls**: `EM_GETLINE`.
   *Critical Win32 Quirk*: `EM_GETLINE` mandates that the very first `WORD` (2 bytes) of the destination buffer holds the capacity of the buffer in characters. Failure to set `*((WORD*)buf) = maxChars;` causes memory corruption or failure.
3. **ListBox & ComboBox**: `LB_GETTEXT` / `CB_GETTEXT`.
4. **Cross-Process `SysListView32` Memory Reading**: Uses `VirtualAllocEx` in the target process address space to allocate a remote `LVITEMW` struct, sends `LVM_GETITEMTEXTW`, reads back the extracted string via `ReadProcessMemory`, and releases the remote memory with `VirtualFreeEx`.
5. **Chromium / Electron Accessibility Poke**:
   Chromium engines run their accessibility subsystem in a dormant state until requested. To force Chromium to instantiate its full internal Blink accessibility DOM, `cpower` sends a synchronous `WM_GETOBJECT` message with `(LPARAM)OBJID_CLIENT` (`0xFFFFFFFC`) using `SendMessageTimeoutW` (200 ms timeout).

### 3.7 Micro-Region DXGI VRAM Capture
Full-screen periodic screenshots at 4K (3840x2160x4 bytes = 33.18 MB) waste gigabytes of PCIe and CPU bandwidth. `cpower` activates DXGI Desktop Duplication exclusively on-demand for small sub-regions (e.g., 64x64 pixels = 16 KB):

```c
/* Direct3D 11 Subresource Hardware Blit */
D3D11_BOX box;
box.left = (UINT)region.left;
box.top = (UINT)region.top;
box.front = 0;
box.right = (UINT)region.right;
box.bottom = (UINT)region.bottom;
box.back = 1;

ID3D11DeviceContext_CopySubresourceRegion(
    pContext,
    (ID3D11Resource*)pStagingTexture,
    0, 0, 0, 0,
    (ID3D11Resource*)pDesktopTexture,
    0, &box
);
```

#### Zero-Dependency In-Memory BMP & PNG Encoders
- **BMP Encoder**: Emits a standard 54-byte DIB header (`BITMAPFILEHEADER` + `BITMAPINFOHEADER` with negative height for top-down scanlines matching DXGI memory order), followed immediately by BGRA32 pixel bytes. Zero memory allocations.
- **PNG Encoder**: Formats the pixel array into uncompressed RFC 1950/1951 DEFLATE store blocks (`BTYPE=00`), computing Adler-32 and CRC-32 directly in C without linking `zlib` or `libpng`.

---

## 4. Pillar 2: Native Dual-Mode Control Engine

### 4.1 Mode Classifier & Routing
The control subsystem exposes two execution strategies:
1. **Direct Instant Mode**: Programmatic dispatch bypassing the system cursor and physical input event queues. Sub-millisecond latency.
2. **Human Emulation Mode**: Synthetic motor trajectories governed by biological kinematic models dispatched through low-level `SendInput`.

### 4.2 Direct Instant Mode: COM Patterns & Win32 Messaging
- **`IUIAutomationInvokePattern`**: Dispatched via `GetCurrentPatternAs(UIA_InvokePatternId, ...)`, followed by `Invoke()`.
- **`IUIAutomationValuePattern`**: Checks `get_CurrentIsReadOnly()`. If false, writes BSTR via `SetValue()`.
- **`IUIAutomationTogglePattern`**: Cycles checkboxes and toggles via `Toggle()`.
- **`IUIAutomationSelectionItemPattern`**: Selects tabs, radio buttons, and list items via `Select()`.
- **Deadlock-Guarded Win32 Messaging**:
  Direct `SendMessageW` calls block indefinitely if the target UI thread hangs or displays a modal dialog. All direct window messages use `SendMessageTimeoutW` with `SMTO_ABORTIFHUNG | SMTO_NORMAL` and a strict timeout (500 ms – 1500 ms). Coordinate translation from Screen space to Window Client space is executed via `ScreenToClient(hwnd, &pt)`.

### 4.3 Human Emulation Mode: Kinematics & Mathematical Formulations

```
[Human Emulation Kinematic Pipeline]

  P0 (Current Pos) -----------------------------------------------------> P3 (Target Pos)
         |                                                                      ^
         | 1. Control Point Synthesis (Longitudinal + Curvature Bias)          |
         v                                                                      |
  [P1, P2 Control Points]                                                       |
         |                                                                      |
         | 2. Cubic Bézier Curve: B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + ...       |
         v                                                                      |
  [Ideal Geometric Path]                                                        |
         |                                                                      |
         | 3. Minimum-Jerk Velocity Profile: s(tau) = 10*tau^3 - 15*tau^4 + 6*tau^5 |
         v                                                                      |
  [Time-Parametric Trajectory]                                                  |
         |                                                                      |
         | 4. Ornstein-Uhlenbeck Tremor: dJ = -theta*J*dt + sigma*dW            |
         |    with Proximity Damping Envelope Omega(s)                          |
         v                                                                      |
  [Perturbed Coordinates: P_actual(s) = B(s) + Omega(s)*J_s]                    |
         |                                                                      |
         | 5. Box-Muller Log-Normal Timing Dynamics: T = exp(mu + sigma*Z)      |
         v                                                                      |
  [SendInput Event Stream: Normalized Virtual Desktop Coordinates [0, 65535]] --+
```

#### 1. Cubic Bézier Trajectory Generation
Straight-line mouse movements exhibit zero curvature and infinite acceleration, instantly flagging programmatic automation. The trajectory between start $P_0$ and destination $P_3$ is defined by:
$$B(t) = (1-t)^3 P_0 + 3(1-t)^2 t P_1 + 3(1-t) t^2 P_2 + t^3 P_3, \quad t \in [0, 1]$$

Control points $P_1$ and $P_2$ are derived from displacement vector $\vec{D} = P_3 - P_0$, distance $L = \|\vec{D}\|$, unit tangent $\hat{u}$, and unit normal $\hat{n} = (-\hat{u}_y, \hat{u}_x)$:
$$P_1 = P_0 + u_1 \vec{D} + w_1 \hat{n}$$
$$P_2 = P_0 + u_2 \vec{D} + w_2 \hat{n}$$
Where $u_1 \in [0.20, 0.35]$, $u_2 \in [0.65, 0.85]$, and $w_1, w_2$ introduce anatomical curvature bias scaled to $L$.

#### 2. Minimum-Jerk Velocity Profile
Biological arm movement minimizes the time derivative of acceleration (Flash & Hogan, 1985):
$$\min \int_0^T \left( \left(\frac{d^3 x}{dt^3}\right)^2 + \left(\frac{d^3 y}{dt^3}\right)^2 \right) dt$$
The closed-form 5th-order polynomial solution maps normalized time $\tau = \frac{t}{T} \in [0, 1]$ to path progress $s(\tau) \in [0, 1]$:
$$s(\tau) = 10\tau^3 - 15\tau^4 + 6\tau^5$$
Properties:
- Boundary displacement: $s(0) = 0, \quad s(1) = 1$.
- Boundary velocity: $\dot{s}(0) = 0, \quad \dot{s}(1) = 0$ (smooth start and stop).
- Bell-shaped velocity profile: $\dot{s}(\tau) = 30\tau^2(1-\tau)^2$, peaking at $\tau = 0.5$.

#### 3. Neuromuscular Tremor (Ornstein-Uhlenbeck SDE)
Pure Gaussian noise produces high-frequency "jitter buzz", while Brownian random walks drift indefinitely. Human motor tremor is **mean-reverting** (8–12 Hz physiological tremor), governed by:
$$dJ_t = -\theta J_t dt + \sigma dW_t$$
Euler-Maruyama discrete formulation:
$$J_{k+1} = J_k \cdot (1 - \theta \Delta t) + \sigma \sqrt{\Delta t} \cdot \xi_k, \quad \xi_k \sim \mathcal{N}(0, 1)$$
To prevent the cursor from shaking off small interactive targets upon arrival, tremor is damped by a proximity envelope $\Omega(s)$:
$$\Omega(s) = (1 - s^{1.2}) \cdot \min\left(1.0, \frac{\text{dist\_to\_target}}{20.0}\right)$$
$$P_{\text{actual}}(s) = B(s) + \Omega(s) \cdot \vec{J}_s$$

#### 4. Cognitive Timing Dynamics (Box-Muller Log-Normal Distribution)
Human keystroke intervals and dwell times are log-normally distributed with heavy right tails:
$$T = \exp(\mu + \sigma Z), \quad Z \sim \mathcal{N}(0, 1)$$
Box-Muller transform:
$$Z_0 = \sqrt{-2 \ln U_1} \cos(2\pi U_2), \quad Z_1 = \sqrt{-2 \ln U_1} \sin(2\pi U_2)$$

#### Empirical Physiological Parameters
| Action | Median $\tilde{T}$ | Shape Parameter $\sigma$ | Effective 95% Range |
|---|---|---|---|
| Inter-Keystroke Interval (Same Hand) | 140 ms | 0.38 | 65 ms – 290 ms |
| Inter-Keystroke Interval (Cross Hand) | 95 ms | 0.32 | 50 ms – 180 ms |
| Key Press Dwell Time (Down $\to$ Up) | 65 ms | 0.25 | 40 ms – 105 ms |
| Mouse Click Dwell Time (Down $\to$ Up) | 75 ms | 0.22 | 50 ms – 115 ms |
| Mouse Double-Click Interval | 110 ms | 0.28 | 65 ms – 190 ms |

#### 5. Virtual Desktop Coordinate Mapping in `SendInput`
`SendInput` with `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` normalizes coordinates to $[0, 65535]$ across multi-monitor virtual desktops:
$$\text{normX} = \text{round}\left( \frac{(X - \text{SM\_XVIRTUALSCREEN}) \times 65535.0}{\text{SM\_CXVIRTUALSCREEN} - 1} \right)$$
$$\text{normY} = \text{round}\left( \frac{(Y - \text{SM\_YVIRTUALSCREEN}) \times 65535.0}{\text{SM\_CYVIRTUALSCREEN} - 1} \right)$$

---

## 5. Pillar 3: Privilege Handling & UIPI Bypass (`uiAccess="true"`)

### 5.1 The UIPI Security Barrier
Windows User Interface Privilege Isolation (UIPI) blocks lower-integrity processes from sending window messages or injecting synthetic input (`SendInput`) into higher-integrity (Administrator) windows:
- If `cpower` runs with standard Medium integrity, opening Task Manager, Registry Editor, or an elevated installer freezes and paralyzes the agent.
- Running `cpower` as Administrator is insufficient: enterprise environments restrict admin access, UAC prompts block headless automation, and elevated processes cannot interact with specific secure desktop components.

### 5.2 The `uiAccess="true"` Architecture
Microsoft created `uiAccess="true"` for accessibility and assistive technologies. When granted:
1. The process runs with standard user rights (no administrative privileges required).
2. The kernel sets `TokenUIAccess = TRUE` on the access token.
3. The process is **immune to UIPI**: it can dispatch `SendInput` and window messages to windows of **any** integrity level (Medium, High, System) on the current interactive desktop.

```
+-----------------------------------------------------------------------------+
|                         Windows uiAccess Validation Gate                    |
+-----------------------------------------------------------------------------+
|                                                                             |
|  1. Manifest Validation:                                                    |
|     <requestedExecutionLevel level="asInvoker" uiAccess="true"/>            |
|                                                                             |
|  2. Authenticode Digital Signature:                                         |
|     Binary must be signed with a certificate chaining to:                   |
|     - LocalMachine\Root (Trusted Root Certification Authorities)             |
|     - LocalMachine\TrustedPublisher                                         |
|                                                                             |
|  3. Secure Directory Location:                                              |
|     Binary must execute from %ProgramFiles% or %SystemRoot%\System32        |
|     --> DEVELOPMENT BYPASS: HKLM\...\System\EnableSecureUIAPaths = 0        |
|                                                                             |
+--------------------------------------+--------------------------------------+
                                       | Pass
                                       v
                     +==================================+
                     | TOKEN_UIACCESS Privilege Granted |
                     | UIPI Barrier Completely Bypassed |
                     +==================================+
```

### 5.3 Automated PowerShell PKI & Signing Pipeline
`cpower` provides two self-contained PowerShell scripts in `scripts/`:
1. `scripts/setup_cert.ps1`: Creates a local self-signed code-signing root certificate via `New-SelfSignedCertificate` and imports it into `Cert:\LocalMachine\Root` and `Cert:\LocalMachine\TrustedPublisher`.
2. `scripts/sign_app.ps1`: Automates Authenticode signing using `signtool.exe` or `Set-AuthenticodeSignature` with SHA-256.

---

## 6. Pillar 4: Low-Latency IPC Architecture

### 6.1 Dual SPSC Shared Memory Ring Buffer
The IPC subsystem provides sub-microsecond, lock-free communication between `cpower` and external AI runtimes using Windows shared memory (`CreateFileMappingW`).

```
+===================================================================================================+
|                              IPC Shared Memory Architecture (64-byte padded)                       |
+===================================================================================================+
|                                                                                                   |
|  [Header Cache Line - 64 bytes]                                                                   |
|  +----------------+---------------+----------------+---------------+---------------------------+  |
|  | magic (uint32) | ver (uint32)  | capacity (u32) | mask (uint32) | pad0 (48 bytes)           |  |
|  | 0x41475043     | 1             | 8388608 (8MB)  | 8388607       | Alignment Padding         |  |
|  +----------------+---------------+----------------+---------------+---------------------------+  |
|                                                                                                   |
|  [Producer Cache Line - 64 bytes]                                                                 |
|  +--------------------------------+------------------------------------------------------------+  |
|  | write_idx (volatile uint32)    | pad1 (60 bytes)                                            |  |
|  | Monotonic byte offset          | Eliminates false sharing against read_idx                  |  |
|  +--------------------------------+------------------------------------------------------------+  |
|                                                                                                   |
|  [Consumer Cache Line - 64 bytes]                                                                 |
|  +--------------------------------+------------------------------------------------------------+  |
|  | read_idx (volatile uint32)     | pad2 (60 bytes)                                            |  |
|  | Monotonic byte offset          | Eliminates false sharing against write_idx                 |  |
|  +--------------------------------+------------------------------------------------------------+  |
|                                                                                                   |
|  [Data Payload Ring Buffer - capacity bytes (Power of Two)]                                       |
|  +---------------------------------------------------------------------------------------------+  |
|  | Offset wrapping via bitwise AND: buffer_offset = index & mask                               |  |
|  | Memory Barriers: MemoryBarrier() on write commit and read consumption                       |  |
|  +---------------------------------------------------------------------------------------------+  |
+===================================================================================================+
```

#### Cross-Integrity SDDL Security Descriptors
When an elevated or `uiAccess="true"` process creates a shared memory section, standard medium-integrity external runtimes receive `ERROR_ACCESS_DENIED` (5). `cpower` resolves this by assigning an explicit SDDL descriptor:
- `D:(A;;GA;;;WD)`: Discretionary ACL granting `GENERIC_ALL` (`GA`) to Everyone/World (`WD`).
- `S:(ML;;NW;;;LW)`: Mandatory Label (`ML`) allowing write access from Low Mandatory Level (`LW`) processes without write-up restrictions.

### 6.2 Flat Contiguous Binary Semantic Tree Packets
Instead of serializing trees to JSON strings, the engine packs trees into contiguous binary buffers requiring **zero pointer fixups**:

```c
#pragma pack(push, 4)

typedef struct {
    uint32_t packet_magic;       /* 0x54524545 "TREE" */
    uint32_t sequence_id;        /* Monotonic sequence number */
    uint64_t timestamp_us;       /* QPC microsecond timestamp */
    uint32_t total_payload_size; /* Header + Nodes + Strings */
    uint32_t node_count;         /* Number of SemanticNode structs */
    uint32_t root_node_index;    /* Root node index (0) */
    uint32_t string_table_bytes; /* Byte length of trailing string table */
} TreePacketHeader;

/* Followed immediately in memory by:
   SemanticNode nodes[node_count];
   char string_table[string_table_bytes];
*/

#pragma pack(pop)
```

External runtimes deserialize the tree in $O(1)$ time by casting memory offsets directly.

### 6.3 Named Pipes Fallback Architecture
For environments where shared memory is restricted, the engine hosts a message-mode Named Pipe server (`\\.\pipe\cpower_ipc`):
- `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`: Guarantees message boundaries without stream delimiter parsing.
- `FILE_FLAG_WRITE_THROUGH`: Bypasses internal caching, transmitting packets immediately to the kernel pipe buffer.

---

## 7. Pillar 5: Zero-Dependency Compilation Model

### 7.1 Multi-Toolchain Support & Flag Matrix
The project compiles under both standard MinGW-w64 GCC and MSVC `cl.exe`.

| Configuration | MinGW-w64 GCC (Active) | MSVC `cl.exe` |
|---|---|---|
| C Standard | `-std=c11` | `/std:c11` |
| Warning Level | `-Wall -Wextra -Werror -pedantic` | `/W4 /WX` |
| Unicode Mode | `-municode -DUNICODE -D_UNICODE` | `/DUNICODE /D_UNICODE` |
| Windows Defs | `-DWIN32_LEAN_AND_MEAN -DCOBJMACROS` | `/DWIN32_LEAN_AND_MEAN /DCOBJMACROS` |
| Optimization | `-O2 -s -static-libgcc` | `/O2 /MD` |
| Link Libraries | `-lole32 -loleaut32 -luiautomationcore -luser32 -lgdi32 -ldwrite -ld3d11 -ldxgi -ladvapi32` | `ole32.lib oleaut32.lib uiautomationcore.lib user32.lib gdi32.lib dwrite.lib d3d11.lib dxgi.lib advapi32.lib` |

### 7.2 Standalone Executable Structure & CLI Daemon
The resulting `bin/cpower.exe` binary links exclusively to standard Windows system DLLs. Dependency audits confirm **zero** third-party DLL dependencies.

The CLI exposes modular subcommands:
- `cpower.exe tree [--hwnd <id>] [--format json|compact|indent]`: Dumps active or target window semantic tree.
- `cpower.exe events [--duration <sec>]`: Streams real-time WinEvent notifications.
- `cpower.exe action --cmd <type> [--x <val>] [--y <val>] [--text <str>] [--mode direct|emulated]`: Executes an action.
- `cpower.exe daemon [--transport shm|pipe]`: Starts the high-speed IPC daemon for external runtimes.
- `cpower.exe verify`: Executes hardware and environment verification diagnostics.

---

## 8. Concrete Interface Contracts & Module Specifications

### 8.1 Header Inventory & Responsibilities
- `include/cpower.h`: Unified master definitions, versioning macros, unified error codes (`CPOWER_OK`, `CPOWER_ERR_*`).
- `include/common.h`: High-resolution QPC timers, logging, memory safety macros, Box-Muller RNG.
- `include/perception.h`: Semantic tree data structures, UIA traversal, WinEvent dispatcher, fallback GDI, DXGI micro-capture.
- `include/input.h`: Dual-mode control, cubic Bézier kinematics, tremor simulation, SendInput wrappers.
- `include/ipc.h`: Shared memory ring buffer, named pipe protocol, binary tree packet serialization.
- `include/security.h`: `uiAccess` verification, token integrity inspection, security descriptors.

---

*End of Architectural Specification.*
