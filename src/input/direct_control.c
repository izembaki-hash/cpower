/**
 * @file direct_control.c
 * @brief Direct Instant Mode Control Engine (Pure C COM UIA patterns & Win32 messaging).
 *
 * Implements lifecycle/configuration management, pure C COM vtable pattern
 * invocation via GetCurrentPatternAs, and deadlock-guarded Win32 messaging
 * via SendMessageTimeoutW and PostMessageW.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "input.h"
#include <math.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Explicit UIA Pattern Interface IIDs (Zero dependency on uuid.lib)
 * ------------------------------------------------------------------------- */

static const GUID CPOWER_IID_IUIAutomationInvokePattern = {
    0xfb377fbe, 0x8ea6, 0x46d5, { 0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59 }
};

static const GUID CPOWER_IID_IUIAutomationValuePattern = {
    0xa94cd8b1, 0x0844, 0x4cd6, { 0x9d, 0x2d, 0x64, 0x05, 0x37, 0xab, 0x39, 0xe9 }
};

static const GUID CPOWER_IID_IUIAutomationTogglePattern = {
    0x94cf8058, 0x9b8d, 0x4ab9, { 0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70 }
};

static const GUID CPOWER_IID_IUIAutomationSelectionItemPattern = {
    0xa8efa66a, 0x0fda, 0x421a, { 0x91, 0x94, 0x38, 0x02, 0x1f, 0x35, 0x78, 0xea }
};

/* -------------------------------------------------------------------------
 * Global Control Engine State & Synchronization
 * ------------------------------------------------------------------------- */

#define CPOWER_DEFAULT_MSG_TIMEOUT_MS 1000U
#define CPOWER_DEFAULT_TYPING_WPM     65.0
#define CPOWER_DEFAULT_OU_THETA       15.0
#define CPOWER_DEFAULT_OU_SIGMA       2.5
#define CPOWER_DEFAULT_MIN_STEPS      10

static SRWLOCK g_controlLock = SRWLOCK_INIT;
static BOOL    g_controlInitialized = FALSE;

static ControlConfig g_controlConfig = {
    CONTROL_MODE_DIRECT_INSTANT,
    {
        1.0,                     /* speedScale */
        1.0,                     /* tremorScale */
        1.0,                     /* curvatureScale */
        CPOWER_DEFAULT_OU_THETA, /* ouTheta */
        CPOWER_DEFAULT_OU_SIGMA, /* ouSigma */
        CPOWER_DEFAULT_MIN_STEPS /* minSteps */
    },
    CPOWER_DEFAULT_MSG_TIMEOUT_MS,
    CPOWER_DEFAULT_TYPING_WPM
};

static void Control_ResetDefaultsLocked(void) {
    g_controlConfig.defaultMode               = CONTROL_MODE_DIRECT_INSTANT;
    g_controlConfig.trajectory.speedScale     = 1.0;
    g_controlConfig.trajectory.tremorScale    = 1.0;
    g_controlConfig.trajectory.curvatureScale = 1.0;
    g_controlConfig.trajectory.ouTheta        = CPOWER_DEFAULT_OU_THETA;
    g_controlConfig.trajectory.ouSigma        = CPOWER_DEFAULT_OU_SIGMA;
    g_controlConfig.trajectory.minSteps       = CPOWER_DEFAULT_MIN_STEPS;
    g_controlConfig.sendMessageTimeoutMs      = CPOWER_DEFAULT_MSG_TIMEOUT_MS;
    g_controlConfig.typingWpm                 = CPOWER_DEFAULT_TYPING_WPM;
}

/**
 * @brief Internal subsystem accessor used by bezier_mouse.c and human_keyboard.c.
 */
void Control_GetActiveConfigInternal(ControlConfig* pOutConfig) {
    if (!pOutConfig) {
        return;
    }
    AcquireSRWLockShared(&g_controlLock);
    *pOutConfig = g_controlConfig;
    ReleaseSRWLockShared(&g_controlLock);
}

static HRESULT Control_Win32ErrorToHResult(DWORD fallbackError) {
    DWORD err = GetLastError();
    if (err == 0) {
        err = fallbackError;
    }
    return HRESULT_FROM_WIN32(err);
}

/* -------------------------------------------------------------------------
 * Control Engine Lifecycle
 * ------------------------------------------------------------------------- */

HRESULT Control_Init(void) {
    AcquireSRWLockExclusive(&g_controlLock);
    if (!g_controlInitialized) {
        Control_ResetDefaultsLocked();
        g_controlInitialized = TRUE;
    }
    ReleaseSRWLockExclusive(&g_controlLock);

    CPower_Log(CPOWER_LOG_INFO, "Control engine initialized.");
    return S_OK;
}

void Control_Shutdown(void) {
    AcquireSRWLockExclusive(&g_controlLock);
    Control_ResetDefaultsLocked();
    g_controlInitialized = FALSE;
    ReleaseSRWLockExclusive(&g_controlLock);

    CPower_Log(CPOWER_LOG_INFO, "Control engine shut down.");
}

HRESULT Control_Configure(const ControlConfig* pConfig) {
    if (!pConfig) {
        return E_POINTER;
    }

    if (pConfig->defaultMode != CONTROL_MODE_DIRECT_INSTANT &&
        pConfig->defaultMode != CONTROL_MODE_HUMAN_EMULATION) {
        return E_INVALIDARG;
    }

    if (!isfinite(pConfig->trajectory.speedScale)     || pConfig->trajectory.speedScale < 0.0 ||
        !isfinite(pConfig->trajectory.tremorScale)    || pConfig->trajectory.tremorScale < 0.0 ||
        !isfinite(pConfig->trajectory.curvatureScale) || pConfig->trajectory.curvatureScale < 0.0 ||
        !isfinite(pConfig->trajectory.ouTheta)        || pConfig->trajectory.ouTheta < 0.0 ||
        !isfinite(pConfig->trajectory.ouSigma)        || pConfig->trajectory.ouSigma < 0.0 ||
        pConfig->trajectory.minSteps < 0 ||
        !isfinite(pConfig->typingWpm)                 || pConfig->typingWpm < 0.0) {
        return E_INVALIDARG;
    }

    ControlConfig normalized = *pConfig;
    if (normalized.trajectory.speedScale == 0.0) {
        normalized.trajectory.speedScale = 1.0;
    }
    if (normalized.trajectory.ouTheta == 0.0) {
        normalized.trajectory.ouTheta = CPOWER_DEFAULT_OU_THETA;
    }
    if (normalized.trajectory.ouSigma == 0.0) {
        normalized.trajectory.ouSigma = CPOWER_DEFAULT_OU_SIGMA;
    }
    if (normalized.trajectory.minSteps == 0) {
        normalized.trajectory.minSteps = CPOWER_DEFAULT_MIN_STEPS;
    }
    if (normalized.sendMessageTimeoutMs == 0) {
        normalized.sendMessageTimeoutMs = CPOWER_DEFAULT_MSG_TIMEOUT_MS;
    }
    if (normalized.typingWpm == 0.0) {
        normalized.typingWpm = CPOWER_DEFAULT_TYPING_WPM;
    }

    AcquireSRWLockExclusive(&g_controlLock);
    g_controlConfig = normalized;
    ReleaseSRWLockExclusive(&g_controlLock);

    return S_OK;
}

/* -------------------------------------------------------------------------
 * Direct Instant Mode Primitives (COM UIA Patterns)
 * ------------------------------------------------------------------------- */

HRESULT Control_DirectInvoke(IUIAutomationElement* pElement) {
    if (!pElement) {
        return E_POINTER;
    }

    void* pPatternRaw = NULL;
    HRESULT hr = pElement->lpVtbl->GetCurrentPatternAs(
        pElement,
        UIA_InvokePatternId,
        &CPOWER_IID_IUIAutomationInvokePattern,
        &pPatternRaw
    );
    if (FAILED(hr)) {
        return hr;
    }
    if (!pPatternRaw) {
        return E_NOINTERFACE;
    }

    IUIAutomationInvokePattern* pInvoke = (IUIAutomationInvokePattern*)pPatternRaw;
    hr = pInvoke->lpVtbl->Invoke(pInvoke);
    CPOWER_SAFE_RELEASE(pInvoke);

    return hr;
}

HRESULT Control_DirectSetValue(IUIAutomationElement* pElement, const WCHAR* pszText) {
    if (!pElement || !pszText) {
        return E_POINTER;
    }

    void* pPatternRaw = NULL;
    HRESULT hr = pElement->lpVtbl->GetCurrentPatternAs(
        pElement,
        UIA_ValuePatternId,
        &CPOWER_IID_IUIAutomationValuePattern,
        &pPatternRaw
    );
    if (FAILED(hr)) {
        return hr;
    }
    if (!pPatternRaw) {
        return E_NOINTERFACE;
    }

    IUIAutomationValuePattern* pValue = (IUIAutomationValuePattern*)pPatternRaw;
    BOOL isReadOnly = FALSE;
    hr = pValue->lpVtbl->get_CurrentIsReadOnly(pValue, &isReadOnly);

    if (SUCCEEDED(hr)) {
        if (isReadOnly) {
            hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        } else {
            BSTR bstrValue = SysAllocString(pszText);
            if (!bstrValue) {
                hr = E_OUTOFMEMORY;
            } else {
                hr = pValue->lpVtbl->SetValue(pValue, bstrValue);
                CPOWER_SAFE_SYSFREESTRING(bstrValue);
            }
        }
    }

    CPOWER_SAFE_RELEASE(pValue);
    return hr;
}

HRESULT Control_DirectToggle(IUIAutomationElement* pElement, enum ToggleState* pNewState) {
    if (!pElement) {
        return E_POINTER;
    }

    void* pPatternRaw = NULL;
    HRESULT hr = pElement->lpVtbl->GetCurrentPatternAs(
        pElement,
        UIA_TogglePatternId,
        &CPOWER_IID_IUIAutomationTogglePattern,
        &pPatternRaw
    );
    if (FAILED(hr)) {
        return hr;
    }
    if (!pPatternRaw) {
        return E_NOINTERFACE;
    }

    IUIAutomationTogglePattern* pToggle = (IUIAutomationTogglePattern*)pPatternRaw;
    hr = pToggle->lpVtbl->Toggle(pToggle);
    if (SUCCEEDED(hr) && pNewState != NULL) {
        hr = pToggle->lpVtbl->get_CurrentToggleState(pToggle, pNewState);
    }

    CPOWER_SAFE_RELEASE(pToggle);
    return hr;
}

HRESULT Control_DirectSelect(IUIAutomationElement* pElement, BOOL bAddToSelection) {
    if (!pElement) {
        return E_POINTER;
    }

    void* pPatternRaw = NULL;
    HRESULT hr = pElement->lpVtbl->GetCurrentPatternAs(
        pElement,
        UIA_SelectionItemPatternId,
        &CPOWER_IID_IUIAutomationSelectionItemPattern,
        &pPatternRaw
    );
    if (FAILED(hr)) {
        return hr;
    }
    if (!pPatternRaw) {
        return E_NOINTERFACE;
    }

    IUIAutomationSelectionItemPattern* pSelItem = (IUIAutomationSelectionItemPattern*)pPatternRaw;
    if (bAddToSelection) {
        hr = pSelItem->lpVtbl->AddToSelection(pSelItem);
    } else {
        hr = pSelItem->lpVtbl->Select(pSelItem);
    }

    CPOWER_SAFE_RELEASE(pSelItem);
    return hr;
}

/* -------------------------------------------------------------------------
 * Direct Instant Mode Primitives (Deadlock-Guarded Win32 Messaging)
 * ------------------------------------------------------------------------- */

HRESULT Control_DirectClickHwnd(HWND hwnd, int clientX, int clientY) {
    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    LPARAM lParam = MAKELPARAM(clientX, clientY);

    SetLastError(0);
    if (!PostMessageW(hwnd, WM_MOUSEMOVE, 0, lParam)) {
        return Control_Win32ErrorToHResult(ERROR_ACCESS_DENIED);
    }

    SetLastError(0);
    if (!PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam)) {
        return Control_Win32ErrorToHResult(ERROR_ACCESS_DENIED);
    }

    SetLastError(0);
    if (!PostMessageW(hwnd, WM_LBUTTONUP, 0, lParam)) {
        return Control_Win32ErrorToHResult(ERROR_ACCESS_DENIED);
    }

    return S_OK;
}

HRESULT Control_DirectClickButtonHwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);
    DWORD timeoutMs = (cfg.sendMessageTimeoutMs > 0) ? cfg.sendMessageTimeoutMs : CPOWER_DEFAULT_MSG_TIMEOUT_MS;

    DWORD_PTR dwResult = 0;
    SetLastError(0);
    LRESULT lr = SendMessageTimeoutW(
        hwnd,
        BM_CLICK,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        timeoutMs,
        &dwResult
    );

    if (lr == 0) {
        return Control_Win32ErrorToHResult(ERROR_TIMEOUT);
    }

    return S_OK;
}

HRESULT Control_DirectSetTextHwnd(HWND hwnd, const WCHAR* pszText) {
    if (!pszText) {
        return E_POINTER;
    }
    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);
    DWORD timeoutMs = (cfg.sendMessageTimeoutMs > 0) ? cfg.sendMessageTimeoutMs : CPOWER_DEFAULT_MSG_TIMEOUT_MS;

    DWORD_PTR dwResult = 0;
    SetLastError(0);
    LRESULT lr = SendMessageTimeoutW(
        hwnd,
        WM_SETTEXT,
        0,
        (LPARAM)pszText,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        timeoutMs,
        &dwResult
    );

    if (lr == 0) {
        return Control_Win32ErrorToHResult(ERROR_TIMEOUT);
    }

    return S_OK;
}
