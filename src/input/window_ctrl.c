/**
 * @file window_ctrl.c
 * @brief Top-level window enumeration, focus/show control, mouse wheel scroll,
 *        emergency failsafe, and closed-loop post-action semantic verification.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "input.h"
#include <limits.h>
#include <string.h>

static volatile LONG g_failsafeEnabled = 0L;

void Control_SetFailsafeEnabled(BOOL enabled) {
    InterlockedExchange(&g_failsafeEnabled, enabled ? 1L : 0L);
}

BOOL Control_CheckFailsafeTriggered(void) {
    if (InterlockedCompareExchange(&g_failsafeEnabled, 0L, 0L) == 0L) {
        return FALSE;
    }

    if ((GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0) {
        return TRUE;
    }

    POINT pt;
    if (GetCursorPos(&pt)) {
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (hMon != NULL) {
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                if (pt.x <= mi.rcMonitor.left + 2 && pt.y <= mi.rcMonitor.top + 2) {
                    return TRUE;
                }
                return FALSE;
            }
        }
        if (pt.x >= 0 && pt.x <= 2 && pt.y >= 0 && pt.y <= 2) {
            return TRUE;
        }
    }
    return FALSE;
}

/* -------------------------------------------------------------------------
 * Window Enumeration & Focus / Show Management
 * ------------------------------------------------------------------------- */

typedef struct EnumWindowsCtx {
    WindowInfo* pOut;
    uint32_t    maxCount;
    uint32_t    totalCount;
} EnumWindowsCtx;

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam) {
    EnumWindowsCtx* ctx = (EnumWindowsCtx*)(uintptr_t)lParam;
    if (!ctx) {
        return FALSE;
    }

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0L && (exStyle & WS_EX_APPWINDOW) == 0L) {
        return TRUE;
    }

    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != NULL && (exStyle & WS_EX_APPWINDOW) == 0L) {
        return TRUE;
    }

    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) {
        return TRUE;
    }

    RECT rc;
    ZeroMemory(&rc, sizeof(rc));
    if (!GetWindowRect(hwnd, &rc)) {
        return TRUE;
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return TRUE;
    }

    if (ctx->pOut && ctx->totalCount < ctx->maxCount) {
        WindowInfo* info = &ctx->pOut[ctx->totalCount];
        ZeroMemory(info, sizeof(*info));

        info->hwnd = (uint64_t)(uintptr_t)hwnd;
        DWORD pid = 0U;
        (void)GetWindowThreadProcessId(hwnd, &pid);
        info->pid = (uint32_t)pid;
        info->rect = rc;
        info->isVisible = 1U;
        info->isMinimized = IsIconic(hwnd) ? 1U : 0U;
        (void)GetWindowTextW(hwnd, info->title, (int)(sizeof(info->title) / sizeof(info->title[0])));
        (void)GetClassNameW(hwnd, info->className, (int)(sizeof(info->className) / sizeof(info->className[0])));
    }

    ctx->totalCount++;
    return TRUE;
}

HRESULT Control_EnumWindows(WindowInfo* pOut, uint32_t maxCount, uint32_t* pOutCount) {
    if (!pOutCount) {
        return E_POINTER;
    }
    if (maxCount > 0U && !pOut) {
        return E_POINTER;
    }
    *pOutCount = 0U;

    EnumWindowsCtx ctx;
    ctx.pOut = pOut;
    ctx.maxCount = maxCount;
    ctx.totalCount = 0U;

    SetLastError(0);
    if (!EnumWindows(enum_windows_proc, (LPARAM)(uintptr_t)&ctx)) {
        DWORD err = GetLastError();
        if (err != 0U) {
            return HRESULT_FROM_WIN32(err);
        }
    }

    *pOutCount = ctx.totalCount;
    return S_OK;
}

HRESULT Control_FocusWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    (void)AllowSetForegroundWindow(ASFW_ANY);

    if (SetForegroundWindow(hwnd)) {
        (void)BringWindowToTop(hwnd);
        return S_OK;
    }

    HWND fgHwnd = GetForegroundWindow();
    DWORD curTid = GetCurrentThreadId();
    DWORD fgTid = fgHwnd ? GetWindowThreadProcessId(fgHwnd, NULL) : 0U;
    DWORD targetTid = GetWindowThreadProcessId(hwnd, NULL);

    BOOL attachedFg = FALSE;
    BOOL attachedTarget = FALSE;

    if (fgTid != 0U && fgTid != curTid) {
        attachedFg = AttachThreadInput(curTid, fgTid, TRUE);
    }
    if (targetTid != 0U && targetTid != curTid && targetTid != fgTid) {
        attachedTarget = AttachThreadInput(curTid, targetTid, TRUE);
    }

    (void)BringWindowToTop(hwnd);
    BOOL ok = SetForegroundWindow(hwnd);
    if (!ok) {
        (void)ShowWindow(hwnd, SW_SHOW);
        ok = SetForegroundWindow(hwnd);
    }

    if (attachedTarget) {
        AttachThreadInput(curTid, targetTid, FALSE);
    }
    if (attachedFg) {
        AttachThreadInput(curTid, fgTid, FALSE);
    }

    if (ok || GetForegroundWindow() == hwnd) {
        return S_OK;
    }
    return S_FALSE;
}

HRESULT Control_ShowWindow(HWND hwnd, WindowShowAction action) {
    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    switch (action) {
        case WND_RESTORE:
            (void)ShowWindow(hwnd, SW_RESTORE);
            return S_OK;
        case WND_MINIMIZE:
            (void)ShowWindow(hwnd, SW_MINIMIZE);
            return S_OK;
        case WND_MAXIMIZE:
            (void)ShowWindow(hwnd, SW_MAXIMIZE);
            return S_OK;
        case WND_CLOSE:
            if (!PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
                DWORD err = GetLastError();
                return HRESULT_FROM_WIN32(err ? err : ERROR_ACCESS_DENIED);
            }
            return S_OK;
        default:
            return E_INVALIDARG;
    }
}

/* -------------------------------------------------------------------------
 * Vertical & Horizontal Mouse Scroll Emulation
 * ------------------------------------------------------------------------- */

HRESULT Control_EmulateMouseScroll(int x, int y, int delta, BOOL horizontal) {
    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (delta == 0) {
        return S_OK;
    }

    if (x != INT_MIN && y != INT_MIN) {
        POINT cur;
        if (!GetCursorPos(&cur) || cur.x != x || cur.y != y) {
            HRESULT hrMove = Control_EmulateMouseMove(x, y, 0.25);
            if (FAILED(hrMove)) {
                return hrMove;
            }
        }
    }

    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    inp.mi.mouseData = (DWORD)(int32_t)delta;

    SetLastError(0);
    if (SendInput(1, &inp, sizeof(INPUT)) != 1U) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err ? err : ERROR_ACCESS_DENIED);
    }
    return S_OK;
}

/* -------------------------------------------------------------------------
 * Closed-Loop Post-Action Semantic Verification
 * ------------------------------------------------------------------------- */

HRESULT Control_ExecuteAndVerify(
    HWND targetHwnd,
    ControlActionCallback pfnAction,
    void* pUserData,
    DWORD verifyTimeoutMs,
    SemanticTreeDiff** ppDiffOut
) {
    if (ppDiffOut) {
        *ppDiffOut = NULL;
    }
    if (!pfnAction) {
        return E_POINTER;
    }

    SemanticTree* pBefore = NULL;
    HRESULT hrCap = Perception_CaptureSemanticTree(targetHwnd, &pBefore);
    if (FAILED(hrCap) || !pBefore) {
        return FAILED(hrCap) ? hrCap : E_FAIL;
    }

    HWND verifyHwnd = (targetHwnd != NULL) ? targetHwnd : pBefore->rootHwnd;

    HRESULT hrAct = pfnAction(pUserData);
    if (FAILED(hrAct)) {
        Perception_FreeSemanticTree(pBefore);
        return hrAct;
    }

    DWORD timeout = (verifyTimeoutMs > 0U) ? verifyTimeoutMs : 300U;
    ULONGLONG startMs = GetTickCount64();
    SemanticTreeDiff* pLastDiff = NULL;
    BOOL changed = FALSE;

    for (;;) {
        SemanticTree* pAfter = NULL;
        HRESULT hrAfter = Perception_CaptureSemanticTree(verifyHwnd, &pAfter);
        if (SUCCEEDED(hrAfter) && pAfter) {
            SemanticTreeDiff* pDiff = NULL;
            if (SUCCEEDED(Perception_DiffTrees(pBefore, pAfter, &pDiff)) && pDiff) {
                if (pLastDiff) {
                    Perception_FreeTreeDiff(pLastDiff);
                }
                pLastDiff = pDiff;
                if (pDiff->entryCount > 0U) {
                    changed = TRUE;
                    Perception_FreeSemanticTree(pAfter);
                    break;
                }
            }
            Perception_FreeSemanticTree(pAfter);
        }

        if ((GetTickCount64() - startMs) >= (ULONGLONG)timeout) {
            break;
        }
        Sleep(25U);
    }

    Perception_FreeSemanticTree(pBefore);

    if (ppDiffOut) {
        *ppDiffOut = pLastDiff;
    } else if (pLastDiff) {
        Perception_FreeTreeDiff(pLastDiff);
    }

    return changed ? S_OK : S_FALSE;
}
