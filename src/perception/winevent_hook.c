/**
 * @file winevent_hook.c
 * @brief Real-time out-of-context SetWinEventHook dispatcher on a dedicated STA thread.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "perception.h"
#include <objbase.h>

#define CPOWER_EVENT_QUEUE_CAP  1024U
#define CPOWER_EVENT_QUEUE_MASK (CPOWER_EVENT_QUEUE_CAP - 1U)

typedef struct WinEventThreadCtx {
    volatile LONG refCount;
    HANDLE        hStopEvent;
    HANDLE        hInitEvent;
    HRESULT       initHr;
} WinEventThreadCtx;

static INIT_ONCE               g_hookInitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION        g_ctrlCs;
static CRITICAL_SECTION        g_queueCs;

static HANDLE                  g_hThread = NULL;
static DWORD                   g_threadId = 0;
static WinEventThreadCtx*      g_pActiveCtx = NULL;
static volatile LONG           g_hookRunning = 0;

static PerceptionEventCallback g_pfnCallback = NULL;
static void*                   g_pUserData = NULL;

static WinEventRecord          g_ring[CPOWER_EVENT_QUEUE_CAP];
static uint32_t                g_ringHead = 0; /* Next write slot */
static uint32_t                g_ringTail = 0; /* Next read slot */
static uint32_t                g_ringCount = 0;

static void ctx_release(WinEventThreadCtx* ctx) {
    if (!ctx) {
        return;
    }
    if (InterlockedDecrement(&ctx->refCount) == 0L) {
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hStopEvent);
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hInitEvent);
        free(ctx);
    }
}

static BOOL CALLBACK init_winevent_cs(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    InitializeCriticalSection(&g_ctrlCs);
    InitializeCriticalSection(&g_queueCs);
    return TRUE;
}

static void queue_clear(void) {
    EnterCriticalSection(&g_queueCs);
    g_ringHead = 0;
    g_ringTail = 0;
    g_ringCount = 0;
    LeaveCriticalSection(&g_queueCs);
}

static void queue_push(const WinEventRecord* pRec) {
    EnterCriticalSection(&g_queueCs);
    if (g_ringCount == CPOWER_EVENT_QUEUE_CAP) {
        /* Overwrite oldest event when queue is full */
        g_ringTail = (g_ringTail + 1U) & CPOWER_EVENT_QUEUE_MASK;
        g_ringCount--;
    }
    g_ring[g_ringHead] = *pRec;
    g_ringHead = (g_ringHead + 1U) & CPOWER_EVENT_QUEUE_MASK;
    g_ringCount++;
    LeaveCriticalSection(&g_queueCs);
}

static void CALLBACK WinEventProcCallback(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime
) {
    (void)hWinEventHook;

    if (hwnd == NULL) {
        return;
    }

    if (idObject == (LONG)OBJID_CURSOR ||
        idObject == (LONG)OBJID_SOUND ||
        idObject == (LONG)OBJID_ALERT) {
        return;
    }

    if (InterlockedCompareExchange(&g_hookRunning, 0, 0) == 0) {
        return;
    }

    WinEventRecord rec;
    rec.event = event;
    rec.hwnd = hwnd;
    rec.idObject = idObject;
    rec.idChild = idChild;
    rec.threadId = idEventThread;
    rec.timestamp = dwmsEventTime;

    queue_push(&rec);

    PerceptionEventCallback cb = NULL;
    void* userData = NULL;
    EnterCriticalSection(&g_queueCs);
    cb = g_pfnCallback;
    userData = g_pUserData;
    LeaveCriticalSection(&g_queueCs);

    if (cb != NULL && InterlockedCompareExchange(&g_hookRunning, 0, 0) == 1) {
        cb(&rec, userData);
    }
}

static DWORD WINAPI WinEventDispatcherThreadProc(LPVOID lpParam) {
    WinEventThreadCtx* ctx = (WinEventThreadCtx*)lpParam;
    if (!ctx) {
        return 1;
    }

    BOOL comOwned = FALSE;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hrCom == S_OK || hrCom == S_FALSE) {
        comOwned = TRUE;
    } else if (hrCom != RPC_E_CHANGED_MODE) {
        ctx->initHr = hrCom;
        SetEvent(ctx->hInitEvent);
        ctx_release(ctx);
        return 1;
    }

    /* Force creation of this thread's message queue before installing the hook */
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    HWINEVENTHOOK hHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_OBJECT_VALUECHANGE,
        NULL,
        WinEventProcCallback,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (!hHook) {
        DWORD err = GetLastError();
        ctx->initHr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
        if (comOwned) {
            CoUninitialize();
        }
        SetEvent(ctx->hInitEvent);
        ctx_release(ctx);
        return 1;
    }

    ctx->initHr = S_OK;
    InterlockedExchange(&g_hookRunning, 1);
    SetEvent(ctx->hInitEvent);

    for (;;) {
        if (WaitForSingleObject(ctx->hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        DWORD waitRes = MsgWaitForMultipleObjectsEx(
            1,
            &ctx->hStopEvent,
            INFINITE,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE
        );

        if (waitRes == WAIT_OBJECT_0) {
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            BOOL shouldQuit = FALSE;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    shouldQuit = TRUE;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (WaitForSingleObject(ctx->hStopEvent, 0) == WAIT_OBJECT_0) {
                    shouldQuit = TRUE;
                    break;
                }
            }
            if (shouldQuit) {
                break;
            }
        } else if (waitRes == WAIT_FAILED) {
            break;
        }
    }

    InterlockedExchange(&g_hookRunning, 0);
    UnhookWinEvent(hHook);
    if (comOwned) {
        CoUninitialize();
    }
    ctx_release(ctx);
    return 0;
}

HRESULT Perception_StartEventHook(EventCallback pfnCallback, void* pUserData) {
    InitOnceExecuteOnce(&g_hookInitOnce, init_winevent_cs, NULL, NULL);
    EnterCriticalSection(&g_ctrlCs);

    if (g_hThread != NULL) {
        if (WaitForSingleObject(g_hThread, 0) == WAIT_OBJECT_0) {
            CPOWER_SAFE_CLOSE_HANDLE(g_hThread);
            g_threadId = 0;
            ctx_release(g_pActiveCtx);
            g_pActiveCtx = NULL;
            InterlockedExchange(&g_hookRunning, 0);
        } else if (InterlockedCompareExchange(&g_hookRunning, 0, 0) == 1) {
            EnterCriticalSection(&g_queueCs);
            g_pfnCallback = pfnCallback;
            g_pUserData = pUserData;
            LeaveCriticalSection(&g_queueCs);
            LeaveCriticalSection(&g_ctrlCs);
            return S_FALSE;
        } else {
            if (GetCurrentThreadId() == g_threadId) {
                LeaveCriticalSection(&g_ctrlCs);
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            HANDLE hWaitPrev = NULL;
            if (!DuplicateHandle(GetCurrentProcess(), g_hThread, GetCurrentProcess(),
                                 &hWaitPrev, SYNCHRONIZE, FALSE, 0)) {
                LeaveCriticalSection(&g_ctrlCs);
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            if (g_pActiveCtx != NULL && g_pActiveCtx->hStopEvent != NULL) {
                SetEvent(g_pActiveCtx->hStopEvent);
            }
            if (g_threadId != 0) {
                PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
            }
            LeaveCriticalSection(&g_ctrlCs);

            WaitForSingleObject(hWaitPrev, 5000);
            CloseHandle(hWaitPrev);

            EnterCriticalSection(&g_ctrlCs);
            if (g_hThread != NULL) {
                if (WaitForSingleObject(g_hThread, 0) == WAIT_OBJECT_0) {
                    CPOWER_SAFE_CLOSE_HANDLE(g_hThread);
                    g_threadId = 0;
                    ctx_release(g_pActiveCtx);
                    g_pActiveCtx = NULL;
                    InterlockedExchange(&g_hookRunning, 0);
                } else {
                    LeaveCriticalSection(&g_ctrlCs);
                    return HRESULT_FROM_WIN32(ERROR_BUSY);
                }
            }
        }
    }

    ctx_release(g_pActiveCtx);
    g_pActiveCtx = NULL;
    g_threadId = 0;

    queue_clear();

    EnterCriticalSection(&g_queueCs);
    g_pfnCallback = pfnCallback;
    g_pUserData = pUserData;
    LeaveCriticalSection(&g_queueCs);

    WinEventThreadCtx* ctx = (WinEventThreadCtx*)calloc(1, sizeof(WinEventThreadCtx));
    if (!ctx) {
        LeaveCriticalSection(&g_ctrlCs);
        return E_OUTOFMEMORY;
    }
    ctx->refCount = 2L; /* 1 for controller, 1 for worker thread */
    ctx->initHr = E_FAIL;
    ctx->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx->hInitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ctx->hStopEvent || !ctx->hInitEvent) {
        DWORD err = GetLastError();
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hStopEvent);
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hInitEvent);
        free(ctx);
        LeaveCriticalSection(&g_ctrlCs);
        return HRESULT_FROM_WIN32(err ? err : ERROR_OUTOFMEMORY);
    }

    DWORD tid = 0;
    HANDLE hThread = CreateThread(NULL, 0, WinEventDispatcherThreadProc, ctx, 0, &tid);
    if (!hThread) {
        DWORD err = GetLastError();
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hStopEvent);
        CPOWER_SAFE_CLOSE_HANDLE(ctx->hInitEvent);
        free(ctx);
        LeaveCriticalSection(&g_ctrlCs);
        return HRESULT_FROM_WIN32(err ? err : ERROR_GEN_FAILURE);
    }

    HANDLE hWaitThread = NULL;
    (void)DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(),
                          &hWaitThread, SYNCHRONIZE, FALSE, 0);

    g_hThread = hThread;
    g_threadId = tid;
    g_pActiveCtx = ctx;
    HANDLE hInitEvent = ctx->hInitEvent;

    /* Drop g_ctrlCs before waiting to avoid callback reentrancy deadlock */
    LeaveCriticalSection(&g_ctrlCs);

    DWORD waitRes = WaitForSingleObject(hInitEvent, 5000);

    EnterCriticalSection(&g_ctrlCs);
    if (g_pActiveCtx != ctx) {
        /* Stopped or replaced concurrently while initializing */
        if (hWaitThread) {
            CloseHandle(hWaitThread);
        }
        LeaveCriticalSection(&g_ctrlCs);
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    if (waitRes != WAIT_OBJECT_0 || FAILED(ctx->initHr)) {
        HRESULT failHr = (waitRes != WAIT_OBJECT_0) ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : ctx->initHr;
        InterlockedExchange(&g_hookRunning, 0);
        SetEvent(ctx->hStopEvent);
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        LeaveCriticalSection(&g_ctrlCs);

        if (hWaitThread) {
            WaitForSingleObject(hWaitThread, 2000);
            CloseHandle(hWaitThread);
            hWaitThread = NULL;
        }

        EnterCriticalSection(&g_ctrlCs);
        if (g_hThread == hThread && WaitForSingleObject(g_hThread, 0) == WAIT_OBJECT_0) {
            CPOWER_SAFE_CLOSE_HANDLE(g_hThread);
            g_threadId = 0;
            if (g_pActiveCtx == ctx) {
                ctx_release(g_pActiveCtx);
                g_pActiveCtx = NULL;
            }
        }
        LeaveCriticalSection(&g_ctrlCs);

        CPower_Log(CPOWER_LOG_ERROR, "Perception_StartEventHook failed (0x%08lX)", (unsigned long)failHr);
        return failHr;
    }

    if (hWaitThread) {
        CloseHandle(hWaitThread);
    }
    LeaveCriticalSection(&g_ctrlCs);
    CPower_Log(CPOWER_LOG_INFO, "WinEvent hook dispatcher started (tid=%lu)", (unsigned long)tid);
    return S_OK;
}

void Perception_StopEventHook(void) {
    InitOnceExecuteOnce(&g_hookInitOnce, init_winevent_cs, NULL, NULL);
    EnterCriticalSection(&g_ctrlCs);

    EnterCriticalSection(&g_queueCs);
    g_pfnCallback = NULL;
    g_pUserData = NULL;
    LeaveCriticalSection(&g_queueCs);

    InterlockedExchange(&g_hookRunning, 0);

    if (g_hThread == NULL) {
        ctx_release(g_pActiveCtx);
        g_pActiveCtx = NULL;
        g_threadId = 0;
        LeaveCriticalSection(&g_ctrlCs);
        return;
    }

    if (g_pActiveCtx != NULL && g_pActiveCtx->hStopEvent != NULL) {
        SetEvent(g_pActiveCtx->hStopEvent);
    }
    if (g_threadId != 0) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }

    if (GetCurrentThreadId() == g_threadId) {
        /* Stopped from inside the hook/callback thread: cannot self-wait; thread exits asynchronously */
        LeaveCriticalSection(&g_ctrlCs);
        CPower_Log(CPOWER_LOG_INFO, "WinEvent hook dispatcher stop requested from callback thread");
        return;
    }

    HANDLE hExpectedThread = g_hThread;
    HANDLE hWaitThread = NULL;
    (void)DuplicateHandle(GetCurrentProcess(), g_hThread, GetCurrentProcess(),
                          &hWaitThread, SYNCHRONIZE, FALSE, 0);

    /* Drop g_ctrlCs before waiting so callbacks calling Stop/Start cannot deadlock */
    LeaveCriticalSection(&g_ctrlCs);

    if (hWaitThread != NULL) {
        WaitForSingleObject(hWaitThread, 5000);
        CloseHandle(hWaitThread);
    }

    EnterCriticalSection(&g_ctrlCs);
    if (g_hThread != NULL && g_hThread == hExpectedThread) {
        if (WaitForSingleObject(g_hThread, 0) == WAIT_OBJECT_0) {
            CPOWER_SAFE_CLOSE_HANDLE(g_hThread);
            g_threadId = 0;
            ctx_release(g_pActiveCtx);
            g_pActiveCtx = NULL;
        } else {
            CPower_Log(CPOWER_LOG_WARN, "Perception_StopEventHook: worker thread still alive after timeout");
        }
    }
    LeaveCriticalSection(&g_ctrlCs);
    CPower_Log(CPOWER_LOG_INFO, "WinEvent hook dispatcher stopped");
}

BOOL Perception_PollEvent(WinEventRecord* pEventOut) {
    if (!pEventOut) {
        return FALSE;
    }

    InitOnceExecuteOnce(&g_hookInitOnce, init_winevent_cs, NULL, NULL);
    EnterCriticalSection(&g_queueCs);

    if (g_ringCount == 0U) {
        LeaveCriticalSection(&g_queueCs);
        return FALSE;
    }

    *pEventOut = g_ring[g_ringTail];
    g_ringTail = (g_ringTail + 1U) & CPOWER_EVENT_QUEUE_MASK;
    g_ringCount--;

    LeaveCriticalSection(&g_queueCs);
    return TRUE;
}
