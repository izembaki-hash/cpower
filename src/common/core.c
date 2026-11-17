/**
 * @file core.c
 * @brief Unified cpower engine lifecycle, error strings, and versioning.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "cpower.h"
#include "common.h"
#include <objbase.h>
#include <string.h>

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE value);
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);

/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4 */
#define CPOWER_DPI_CONTEXT_PMV2 ((HANDLE)(LONG_PTR)-4)

static volatile LONG g_initState = 0; /* 0 = uninit, 1 = initializing, 2 = initialized */
static BOOL          g_comInitializedByCore = FALSE;
static DWORD         g_comInitThreadId = 0;
static QpcTimer      g_coreUptimeTimer;

static void cpower_enable_dpi_awareness(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        hUser32 = LoadLibraryW(L"user32.dll");
    }
    if (!hUser32) {
        return;
    }

    FARPROC procCtx = GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (procCtx) {
        PFN_SetProcessDpiAwarenessContext pfnSetCtx = NULL;
        memcpy(&pfnSetCtx, &procCtx, sizeof(pfnSetCtx));
        if (pfnSetCtx(CPOWER_DPI_CONTEXT_PMV2)) {
            return;
        }
    }

    FARPROC procAware = GetProcAddress(hUser32, "SetProcessDPIAware");
    if (procAware) {
        PFN_SetProcessDPIAware pfnSetAware = NULL;
        memcpy(&pfnSetAware, &procAware, sizeof(pfnSetAware));
        (void)pfnSetAware();
    }
}

cpower_status_t CPower_Init(void) {
    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_initState, 1, 0);
        if (prev == 0) {
            break;
        }
        if (prev == 2) {
            CPower_Log(CPOWER_LOG_DEBUG, "CPower_Init called while already initialized");
            return CPOWER_OK;
        }
        /* Another thread is currently initializing; yield briefly. */
        Sleep(0);
    }

    Timer_Init(&g_coreUptimeTimer);
    if (g_coreUptimeTimer.frequency.QuadPart <= 0) {
        InterlockedExchange(&g_initState, 0);
        return CPOWER_ERR_SYSTEM_FAILURE;
    }
    Timer_Start(&g_coreUptimeTimer);

    cpower_enable_dpi_awareness();

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        g_comInitializedByCore = TRUE;
        g_comInitThreadId = GetCurrentThreadId();
    } else if (hr == RPC_E_CHANGED_MODE) {
        /* Thread is already bound to an STA apartment; proceed gracefully without owning CoUninitialize. */
        g_comInitializedByCore = FALSE;
        g_comInitThreadId = 0;
        CPower_Log(CPOWER_LOG_WARN, "CPower_Init: COM already initialized in STA mode (RPC_E_CHANGED_MODE)");
    } else {
        CPower_Log(CPOWER_LOG_ERROR, "CPower_Init: CoInitializeEx failed with HRESULT 0x%08lX", (unsigned long)hr);
        InterlockedExchange(&g_initState, 0);
        return CPOWER_ERR_COM_FAILURE;
    }

    InterlockedExchange(&g_initState, 2);
    CPower_Log(CPOWER_LOG_INFO, "cpower engine v%s initialized (QPC freq=%lld Hz)",
               CPOWER_VERSION_STRING, (long long)g_coreUptimeTimer.frequency.QuadPart);
    return CPOWER_OK;
}

void CPower_Shutdown(void) {
    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_initState, 1, 2);
        if (prev == 2) {
            break;
        }
        if (prev == 0) {
            return;
        }
        Sleep(0);
    }

    Timer_Stop(&g_coreUptimeTimer);
    double uptimeMs = Timer_ElapsedMs(&g_coreUptimeTimer);

    if (g_comInitializedByCore && GetCurrentThreadId() == g_comInitThreadId) {
        CoUninitialize();
    }
    g_comInitializedByCore = FALSE;
    g_comInitThreadId = 0;

    InterlockedExchange(&g_initState, 0);
    CPower_Log(CPOWER_LOG_INFO, "cpower engine shut down (uptime=%.2f ms)", uptimeMs);
}

const char* CPower_GetErrorString(cpower_status_t status) {
    switch (status) {
        case CPOWER_OK:                        return "Success";
        case CPOWER_ERR_INVALID_ARG:           return "Invalid argument or null pointer";
        case CPOWER_ERR_OUT_OF_MEMORY:         return "Out of memory";
        case CPOWER_ERR_NOT_INITIALIZED:       return "Subsystem not initialized";
        case CPOWER_ERR_ALREADY_INITIALIZED:   return "Component already initialized";
        case CPOWER_ERR_COM_FAILURE:           return "COM runtime or CoCreateInstance failure";
        case CPOWER_ERR_ELEMENT_NOT_FOUND:     return "UI Automation element not found";
        case CPOWER_ERR_PATTERN_NOT_SUPPORTED: return "UI Automation pattern not supported";
        case CPOWER_ERR_TIMEOUT:               return "Operation timed out";
        case CPOWER_ERR_BUFFER_TOO_SMALL:      return "Destination buffer capacity too small";
        case CPOWER_ERR_IPC_FAILURE:           return "IPC shared memory or pipe failure";
        case CPOWER_ERR_SECURITY_DENIED:       return "Access denied by UIPI or missing uiAccess";
        case CPOWER_ERR_DXGI_FAILURE:          return "DXGI or D3D11 desktop capture failure";
        case CPOWER_ERR_NOT_SUPPORTED:         return "Feature or mode not supported";
        case CPOWER_ERR_SYSTEM_FAILURE:        return "Win32 system API failure";
        default:                               return "Unknown cpower error";
    }
}

const char* CPower_GetVersion(void) {
    return CPOWER_VERSION_STRING;
}
