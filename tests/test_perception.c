/**
 * @file test_perception.c
 * @brief Tier 1, Tier 2, and Tier 5 tests for Perception Engine:
 *        Pure C COM UIA traversal, serialization (Binary/JSON/Indented),
 *        SetWinEventHook dispatcher, GDI fallback text, Chromium poke,
 *        in-memory BMP/PNG encoders, and DXGI micro-region capture.
 */

#include "test_infra.h"
#include "perception.h"
#include "ipc.h"
#include <wchar.h>

/* ========================================================================= */
/* Helper Win32 Test GUI Window (Dedicated UI Thread)                        */
/* ========================================================================= */

typedef struct PercTestWin {
    HANDLE hThread;
    HANDLE hReadyEvent;
    HWND   hwnd;
    HWND   hBtn;
    HWND   hEditSingle;
    HWND   hEditMulti;
    HWND   hCheck;
    HWND   hStatic;
    ATOM   classAtom;
    WCHAR  className[64];
    volatile LONG clickCount;
} PercTestWin;

static PercTestWin g_percWin;
static SemanticTree* g_percTree = NULL;
static SemanticTreeDiff* g_percDiff = NULL;
static ImageBuffer g_percImg;
static PROCESS_INFORMATION g_extEventProc;
static BOOL g_hookStarted = FALSE;
static LONG g_percWinSeq = 0;
static char* g_percJsonBuf = NULL;
static char* g_percIndentBuf = NULL;

static LRESULT CALLBACK perc_test_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PercTestWin* pWin = (PercTestWin*)(uintptr_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* cs = (const CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_COMMAND:
            if (pWin && LOWORD(wParam) == 2001 && HIWORD(wParam) == BN_CLICKED) {
                InterlockedIncrement(&pWin->clickCount);
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI perc_ui_thread(LPVOID lpParam) {
    PercTestWin* w = (PercTestWin*)lpParam;
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = perc_test_wndproc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = w->className;
    w->classAtom = RegisterClassExW(&wc);
    if (w->classAtom == 0) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hwnd = CreateWindowExW(
        0, w->className, L"CPowerPerceptionTestWindow",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 440, 340,
        NULL, NULL, hInst, w
    );
    if (!w->hwnd) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hBtn = CreateWindowExW(
        0, L"BUTTON", L"Submit Action",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 20, 150, 32,
        w->hwnd, (HMENU)(uintptr_t)2001, hInst, NULL
    );
    w->hEditSingle = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"AlphaBetaGamma",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        20, 64, 240, 26,
        w->hwnd, (HMENU)(uintptr_t)2002, hInst, NULL
    );
    w->hEditMulti = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"FirstLineText\r\nSecondLineText\r\nThirdLineText",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        20, 102, 240, 80,
        w->hwnd, (HMENU)(uintptr_t)2003, hInst, NULL
    );
    w->hCheck = CreateWindowExW(
        0, L"BUTTON", L"Enable Telemetry",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        20, 194, 180, 28,
        w->hwnd, (HMENU)(uintptr_t)2004, hInst, NULL
    );
    w->hStatic = CreateWindowExW(
        0, L"STATIC", L"Ready Status",
        WS_CHILD | WS_VISIBLE,
        190, 24, 150, 24,
        w->hwnd, (HMENU)(uintptr_t)2005, hInst, NULL
    );

    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    UpdateWindow(w->hwnd);
    SetEvent(w->hReadyEvent);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static BOOL start_perc_test_window(void) {
    memset(&g_percWin, 0, sizeof(g_percWin));
    LONG seq = InterlockedIncrement(&g_percWinSeq);
    swprintf(g_percWin.className, 64, L"CPowerPercCls_%lu_%ld", GetCurrentProcessId(), seq);
    g_percWin.hReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_percWin.hReadyEvent) return FALSE;

    g_percWin.hThread = CreateThread(NULL, 0, perc_ui_thread, &g_percWin, 0, NULL);
    if (!g_percWin.hThread) {
        CloseHandle(g_percWin.hReadyEvent);
        g_percWin.hReadyEvent = NULL;
        return FALSE;
    }

    DWORD waitRes = WaitForSingleObject(g_percWin.hReadyEvent, 5000);
    if (waitRes != WAIT_OBJECT_0 || !g_percWin.hwnd) {
        return FALSE;
    }
    Sleep(50);
    return TRUE;
}

static void stop_perc_test_window(void) {
    if (g_percWin.hwnd && IsWindow(g_percWin.hwnd)) {
        PostMessageW(g_percWin.hwnd, WM_CLOSE, 0, 0);
    }
    if (g_percWin.hThread) {
        if (WaitForSingleObject(g_percWin.hThread, 4000) == WAIT_TIMEOUT && g_percWin.hwnd) {
            PostThreadMessageW(GetThreadId(g_percWin.hThread), WM_QUIT, 0, 0);
            WaitForSingleObject(g_percWin.hThread, 2000);
        }
        CloseHandle(g_percWin.hThread);
        g_percWin.hThread = NULL;
    }
    if (g_percWin.hReadyEvent) {
        CloseHandle(g_percWin.hReadyEvent);
        g_percWin.hReadyEvent = NULL;
    }
    if (g_percWin.classAtom != 0) {
        UnregisterClassW(g_percWin.className, GetModuleHandleW(NULL));
        g_percWin.classAtom = 0;
    }
    g_percWin.hwnd = NULL;
}

static void stop_ext_event_proc(void) {
    if (g_extEventProc.hProcess) {
        if (WaitForSingleObject(g_extEventProc.hProcess, 100) == WAIT_TIMEOUT) {
            TerminateProcess(g_extEventProc.hProcess, 0);
            WaitForSingleObject(g_extEventProc.hProcess, 2000);
        }
        CloseHandle(g_extEventProc.hProcess);
        g_extEventProc.hProcess = NULL;
    }
    if (g_extEventProc.hThread) {
        CloseHandle(g_extEventProc.hThread);
        g_extEventProc.hThread = NULL;
    }
}

static void perception_suite_setup(void) {
    memset(&g_percWin, 0, sizeof(g_percWin));
    memset(&g_percImg, 0, sizeof(g_percImg));
    memset(&g_extEventProc, 0, sizeof(g_extEventProc));
    g_percTree = NULL;
    g_percDiff = NULL;
    g_hookStarted = FALSE;
}

static void perception_suite_teardown(void) {
    if (g_hookStarted) {
        Perception_StopEventHook();
        g_hookStarted = FALSE;
    }
    stop_ext_event_proc();
    stop_perc_test_window();
    if (g_percDiff) {
        Perception_FreeTreeDiff(g_percDiff);
        g_percDiff = NULL;
    }
    if (g_percTree) {
        Perception_FreeSemanticTree(g_percTree);
        g_percTree = NULL;
    }
    if (g_percImg.pixels) {
        Perception_FreeImageBuffer(&g_percImg);
    }
    if (g_percJsonBuf) {
        free(g_percJsonBuf);
        g_percJsonBuf = NULL;
    }
    if (g_percIndentBuf) {
        free(g_percIndentBuf);
        g_percIndentBuf = NULL;
    }
    Perception_Shutdown();
}

/* ========================================================================= */
/* CRC-32 & Big-Endian Helpers for PNG Verification                          */
/* ========================================================================= */

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t verify_crc32_ieee(const uint8_t* buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void test_perception_init_shutdown_idempotency(void) {
    for (int i = 0; i < 4; i++) {
        ASSERT_HRESULT_OK(Perception_Init());
        PerceptionConfig cfg;
        cfg.maxTreeDepth = 32;
        cfg.maxNodes = 1024;
        cfg.includeOffscreen = TRUE;
        cfg.enableDxgiCapture = TRUE;
        ASSERT_HRESULT_OK(Perception_Configure(&cfg));
        Perception_Shutdown();
        /* Double shutdown must be safe */
        Perception_Shutdown();
    }

    /* Repeated init while already active */
    ASSERT_HRESULT_OK(Perception_Init());
    HRESULT hr2 = Perception_Init();
    ASSERT_TRUE(SUCCEEDED(hr2) || hr2 == S_FALSE);
    Perception_Shutdown();
}

static void test_perception_capture_win32_semantic_tree(void) {
    ASSERT_TRUE(start_perc_test_window());
    ASSERT_HRESULT_OK(Perception_Init());

    PerceptionConfig cfg = { 64, 4096, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&cfg));

    ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(g_percWin.hwnd, &g_percTree));
    ASSERT_NOT_NULL(g_percTree);
    ASSERT_NOT_NULL(g_percTree->nodes);
    ASSERT_TRUE(g_percTree->nodeCount >= 5U);
    ASSERT_EQ_PTR(g_percWin.hwnd, g_percTree->rootHwnd);
    ASSERT_TRUE(g_percTree->timestampUtc > 0ULL);

    const SemanticNode* root = &g_percTree->nodes[0];
    ASSERT_EQ_UINT(0U, root->id);
    ASSERT_EQ_INT(-1, root->parentId);
    ASSERT_EQ_UINT(0U, root->depth);
    ASSERT_EQ_UINT(UIA_WindowControlTypeId, root->controlTypeId);
    ASSERT_EQ_WSTR(L"CPowerPerceptionTestWindow", root->name);
    ASSERT_TRUE(root->bounds.right > root->bounds.left);
    ASSERT_TRUE(root->bounds.bottom > root->bounds.top);

    BOOL foundBtn = FALSE;
    BOOL foundEdit = FALSE;
    BOOL foundCheck = FALSE;
    BOOL foundStatic = FALSE;

    for (uint32_t i = 0; i < g_percTree->nodeCount; i++) {
        const SemanticNode* n = &g_percTree->nodes[i];
        ASSERT_EQ_UINT(i, n->id);
        if (i > 0) {
            ASSERT_TRUE(n->parentId >= 0 && (uint32_t)n->parentId < g_percTree->nodeCount);
            ASSERT_EQ_UINT(g_percTree->nodes[n->parentId].depth + 1U, n->depth);
        }
        if (n->controlTypeId == UIA_ButtonControlTypeId && wcscmp(n->name, L"Submit Action") == 0) {
            foundBtn = TRUE;
            ASSERT_TRUE((n->flags & CPOWER_NODE_FLAG_ENABLED) != 0U);
            ASSERT_TRUE(n->bounds.right > n->bounds.left);
            ASSERT_TRUE(n->bounds.bottom > n->bounds.top);
        } else if (n->controlTypeId == UIA_EditControlTypeId &&
                   (wcscmp(n->value, L"AlphaBetaGamma") == 0 || wcscmp(n->name, L"AlphaBetaGamma") == 0)) {
            foundEdit = TRUE;
            ASSERT_TRUE(n->bounds.right > n->bounds.left);
            ASSERT_TRUE(n->bounds.bottom > n->bounds.top);
        } else if (n->controlTypeId == UIA_CheckBoxControlTypeId && wcscmp(n->name, L"Enable Telemetry") == 0) {
            foundCheck = TRUE;
        } else if (n->controlTypeId == UIA_TextControlTypeId && wcscmp(n->name, L"Ready Status") == 0) {
            foundStatic = TRUE;
        }
    }

    ASSERT_TRUE(foundBtn);
    ASSERT_TRUE(foundEdit);
    ASSERT_TRUE(foundCheck);
    ASSERT_TRUE(foundStatic);

    /* ControlTypeIdToString sanity */
    ASSERT_EQ_STR("Button", Perception_ControlTypeIdToString(UIA_ButtonControlTypeId));
    ASSERT_EQ_STR("Edit", Perception_ControlTypeIdToString(UIA_EditControlTypeId));
    ASSERT_EQ_STR("CheckBox", Perception_ControlTypeIdToString(UIA_CheckBoxControlTypeId));
    ASSERT_EQ_STR("Window", Perception_ControlTypeIdToString(UIA_WindowControlTypeId));
    ASSERT_EQ_STR("Text", Perception_ControlTypeIdToString(UIA_TextControlTypeId));
    ASSERT_NOT_NULL(Perception_ControlTypeIdToString(999999));

    Perception_FreeSemanticTree(g_percTree);
    g_percTree = NULL;
}

static void test_perception_desktop_and_node_clamping(void) {
    ASSERT_TRUE(start_perc_test_window());
    ASSERT_HRESULT_OK(Perception_Init());

    PerceptionConfig bounded = { 2, 16, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&bounded));
    ASSERT_HRESULT_OK(Perception_CaptureDesktopTree(&g_percTree));
    ASSERT_NOT_NULL(g_percTree);
    ASSERT_TRUE(g_percTree->nodeCount >= 1U && g_percTree->nodeCount <= 16U);
    ASSERT_EQ_INT(-1, g_percTree->nodes[0].parentId);
    Perception_FreeSemanticTree(g_percTree);
    g_percTree = NULL;

    /* Clamp maxNodes to 3 on multi-control test window */
    PerceptionConfig tinyMax = { 64, 3, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&tinyMax));
    ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(g_percWin.hwnd, &g_percTree));
    ASSERT_NOT_NULL(g_percTree);
    ASSERT_TRUE(g_percTree->nodeCount >= 1U && g_percTree->nodeCount <= 3U);
    Perception_FreeSemanticTree(g_percTree);
    g_percTree = NULL;

    PerceptionConfig restore = { 64, 4096, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&restore));
}

static void test_perception_serialize_binary_json_indented(void) {
    ASSERT_TRUE(start_perc_test_window());
    ASSERT_HRESULT_OK(Perception_Init());
    ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(g_percWin.hwnd, &g_percTree));

    /* 1. Binary packet serialization */
    uint8_t binBuf[32768];
    size_t binWritten = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTree(g_percTree, binBuf, sizeof(binBuf), &binWritten));
    ASSERT_TRUE(binWritten >= sizeof(TreePacketHeader));
    TreePacketHeader hdr;
    memcpy(&hdr, binBuf, sizeof(hdr));
    ASSERT_EQ_UINT(CPOWER_TREE_PACKET_MAGIC, hdr.packetMagic);
    ASSERT_EQ_UINT(g_percTree->nodeCount, hdr.nodeCount);
    ASSERT_EQ_UINT((uint32_t)binWritten, hdr.totalPayloadSize);

    /* Small buffer must fail with ERROR_INSUFFICIENT_BUFFER */
    uint8_t tinyBin[8];
    size_t tinyBinWritten = 0;
    HRESULT hrSmallBin = Perception_SerializeTree(g_percTree, tinyBin, sizeof(tinyBin), &tinyBinWritten);
    ASSERT_EQ_HRESULT(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), hrSmallBin);

    /* 2. JSON serialization on real tree + synthetic special-character escaping tree */
    enum { JSON_CAP = 65536, INDENT_CAP = 32768 };
    g_percJsonBuf = (char*)calloc(1, JSON_CAP);
    g_percIndentBuf = (char*)calloc(1, INDENT_CAP);
    ASSERT_NOT_NULL(g_percJsonBuf);
    ASSERT_NOT_NULL(g_percIndentBuf);

    size_t jsonWritten = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTreeJson(g_percTree, g_percJsonBuf, JSON_CAP, &jsonWritten));
    ASSERT_TRUE(jsonWritten > 10U && jsonWritten < JSON_CAP);
    ASSERT_EQ_INT('{', g_percJsonBuf[0]);
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "Submit Action"));
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "Button"));

    size_t indentWritten = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTreeIndented(g_percTree, g_percIndentBuf, INDENT_CAP, &indentWritten));
    ASSERT_TRUE(indentWritten > 10U && indentWritten < INDENT_CAP);
    ASSERT_NOT_NULL(strstr(g_percIndentBuf, "[0]"));
    ASSERT_NOT_NULL(strstr(g_percIndentBuf, "Submit Action"));
    ASSERT_TRUE(indentWritten < jsonWritten);

    /* Synthetic escaping check */
    SemanticNode synNodes[2];
    memset(synNodes, 0, sizeof(synNodes));
    synNodes[0].id = 0;
    synNodes[0].parentId = -1;
    synNodes[0].depth = 0;
    synNodes[0].controlTypeId = UIA_WindowControlTypeId;
    wcscpy(synNodes[0].name, L"Win \"Test\" \\ Root");
    synNodes[1].id = 1;
    synNodes[1].parentId = 0;
    synNodes[1].depth = 1;
    synNodes[1].controlTypeId = UIA_EditControlTypeId;
    wcscpy(synNodes[1].name, L"Field");
    wcscpy(synNodes[1].value, L"Line1\nLine2\t\"Q\"");

    SemanticTree synTree;
    synTree.nodeCount = 2;
    synTree.maxNodes = 2;
    synTree.rootHwnd = g_percWin.hwnd;
    synTree.timestampUtc = 999999ULL;
    synTree.nodes = synNodes;

    size_t escWritten = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTreeJson(&synTree, g_percJsonBuf, JSON_CAP, &escWritten));
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "\\\"Test\\\""));
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "\\\\"));
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "\\n"));
    ASSERT_NOT_NULL(strstr(g_percJsonBuf, "\\t"));

    /* Small buffer checks for JSON and Indented */
    char tinyTxt[8];
    size_t tinyTxtWritten = 0;
    HRESULT hrSmallJson = Perception_SerializeTreeJson(g_percTree, tinyTxt, sizeof(tinyTxt), &tinyTxtWritten);
    ASSERT_EQ_HRESULT(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), hrSmallJson);

    HRESULT hrSmallInd = Perception_SerializeTreeIndented(g_percTree, tinyTxt, sizeof(tinyTxt), &tinyTxtWritten);
    ASSERT_EQ_HRESULT(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), hrSmallInd);
}

typedef struct HookCallbackState {
    volatile LONG eventCount;
    volatile LONG hasValidRecord;
    WinEventRecord lastRecord;
} HookCallbackState;

static void perc_hook_callback(const WinEventRecord* pEvent, void* pUserData) {
    HookCallbackState* st = (HookCallbackState*)pUserData;
    if (!st || !pEvent) return;
    if (pEvent->hwnd != NULL && pEvent->event != 0U) {
        st->lastRecord = *pEvent;
        InterlockedExchange(&st->hasValidRecord, 1);
    }
    InterlockedIncrement(&st->eventCount);
}

static void test_perception_winevent_hook_realtime(void) {
    ASSERT_HRESULT_OK(Perception_Init());

    HookCallbackState cbState;
    memset(&cbState, 0, sizeof(cbState));

    ASSERT_HRESULT_OK(Perception_StartEventHook(perc_hook_callback, &cbState));
    g_hookStarted = TRUE;

    /* Spawn a short-lived external GUI process so WINEVENT_SKIPOWNPROCESS still observes real events */
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    WCHAR cmdLine[] =
        L"powershell.exe -NoProfile -NonInteractive -Command \""
        L"Add-Type -AssemblyName System.Windows.Forms; "
        L"$f=New-Object Windows.Forms.Form; $f.Text='CPowerHookTarget'; $f.Width=240; $f.Height=120; "
        L"$b=New-Object Windows.Forms.Button; $b.Text='Ping'; $f.Controls.Add($b); "
        L"$t=New-Object Windows.Forms.Timer; $t.Interval=180; $t.Add_Tick({$t.Stop();$f.Close()}); "
        L"$f.Add_Shown({$f.Activate();$b.Focus();$t.Start()}); "
        L"[Windows.Forms.Application]::Run($f)\"";

    BOOL started = CreateProcessW(
        NULL, cmdLine, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &g_extEventProc
    );
    ASSERT_TRUE(started);

    /* Wait up to 5000ms for real-time WinEvents via callback or poll queue */
    DWORD t0 = GetTickCount();
    WinEventRecord polled;
    BOOL polledAny = FALSE;
    while ((GetTickCount() - t0) < 5000U) {
        if (Perception_PollEvent(&polled)) {
            if (polled.hwnd != NULL && polled.event != 0U) {
                polledAny = TRUE;
            }
        }
        if (InterlockedCompareExchange(&cbState.eventCount, 0, 0) > 0 &&
            InterlockedCompareExchange(&cbState.hasValidRecord, 0, 0) == 1) {
            break;
        }
        Sleep(20);
    }

    /* Drain any queued events to exercise Perception_PollEvent */
    while (Perception_PollEvent(&polled)) {
        if (polled.hwnd != NULL && polled.event != 0U) {
            polledAny = TRUE;
        }
    }
    (void)polledAny;

    ASSERT_TRUE(InterlockedCompareExchange(&cbState.eventCount, 0, 0) > 0);
    ASSERT_EQ_INT(1, InterlockedCompareExchange(&cbState.hasValidRecord, 0, 0));
    ASSERT_NOT_NULL(cbState.lastRecord.hwnd);
    ASSERT_NE_INT(0, (int64_t)cbState.lastRecord.event);

    /* Verify PollEvent(NULL) is safe */
    ASSERT_FALSE(Perception_PollEvent(NULL));

    Perception_StopEventHook();
    /* Double stop must be safe */
    Perception_StopEventHook();
    g_hookStarted = FALSE;

    stop_ext_event_proc();
}

static void test_perception_fallback_text_and_poke(void) {
    ASSERT_TRUE(start_perc_test_window());

    WCHAR textBuf[256];
    memset(textBuf, 0, sizeof(textBuf));
    ASSERT_HRESULT_OK(Perception_FallbackExtractText(g_percWin.hEditSingle, textBuf, 256));
    ASSERT_EQ_WSTR(L"AlphaBetaGamma", textBuf);

    memset(textBuf, 0, sizeof(textBuf));
    ASSERT_HRESULT_OK(Perception_FallbackExtractText(g_percWin.hEditMulti, textBuf, 256));
    ASSERT_NOT_NULL(wcsstr(textBuf, L"FirstLineText"));
    ASSERT_NOT_NULL(wcsstr(textBuf, L"SecondLineText"));

    memset(textBuf, 0, sizeof(textBuf));
    ASSERT_HRESULT_OK(Perception_FallbackExtractText(g_percWin.hBtn, textBuf, 256));
    ASSERT_EQ_WSTR(L"Submit Action", textBuf);

    /* Canary truncation test: maxChars = 6 must not overwrite past index 5 */
    WCHAR tinyBuf[12];
    for (int i = 0; i < 12; i++) tinyBuf[i] = (WCHAR)0xBEEF;
    HRESULT hrTiny = Perception_FallbackExtractText(g_percWin.hEditSingle, tinyBuf, 6);
    ASSERT_TRUE(SUCCEEDED(hrTiny) || hrTiny == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    for (int i = 6; i < 12; i++) {
        ASSERT_EQ_UINT(0xBEEFU, (unsigned int)tinyBuf[i]);
    }

    /* Chromium accessibility poke safety on real window and invalid HWNDs */
    Perception_PokeChromiumAccessibility(g_percWin.hwnd);
    Perception_PokeChromiumAccessibility(NULL);
    Perception_PokeChromiumAccessibility((HWND)(uintptr_t)0xDEADBEEF);
}

static void test_perception_bmp_and_png_encoders(void) {
    enum { W = 10, H = 8 };
    uint8_t pixels[W * H * 4];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t idx = (size_t)(y * W + x) * 4U;
            pixels[idx + 0] = (uint8_t)(x * 25);        /* B */
            pixels[idx + 1] = (uint8_t)(y * 30);        /* G */
            pixels[idx + 2] = (uint8_t)(255 - x * 20);  /* R */
            pixels[idx + 3] = 255U;                     /* A */
        }
    }

    ImageBuffer img;
    img.width = W;
    img.height = H;
    img.stride = W * 4;
    img.dataSize = sizeof(pixels);
    img.pixels = pixels;

    /* 1. Verify 32-bit top-down BMP encoder */
    uint8_t bmpBuf[2048];
    size_t bmpBytes = 0;
    ASSERT_HRESULT_OK(Perception_EncodeBmpInMemory(&img, bmpBuf, sizeof(bmpBuf), &bmpBytes));
    ASSERT_EQ_UINT(54U + sizeof(pixels), bmpBytes);
    ASSERT_EQ_INT('B', bmpBuf[0]);
    ASSERT_EQ_INT('M', bmpBuf[1]);
    ASSERT_EQ_UINT((uint32_t)bmpBytes, read_le32(bmpBuf + 2));
    ASSERT_EQ_UINT(54U, read_le32(bmpBuf + 10));
    ASSERT_EQ_UINT(40U, read_le32(bmpBuf + 14));
    ASSERT_EQ_INT(W, (int32_t)read_le32(bmpBuf + 18));
    ASSERT_EQ_INT(-H, (int32_t)read_le32(bmpBuf + 22)); /* Negative height = top-down */
    ASSERT_EQ_UINT(1U, read_le16(bmpBuf + 26));
    ASSERT_EQ_UINT(32U, read_le16(bmpBuf + 28));
    ASSERT_EQ_UINT(0U, read_le32(bmpBuf + 30));
    ASSERT_EQ_INT(0, memcmp(bmpBuf + 54, pixels, sizeof(pixels)));

    /* 2. Verify RFC 2083 PNG signature, chunks, zlib header, and IEEE CRC-32 */
    uint8_t pngBuf[4096];
    size_t pngBytes = 0;
    ASSERT_HRESULT_OK(Perception_EncodePngInMemory(&img, pngBuf, sizeof(pngBuf), &pngBytes));
    ASSERT_TRUE(pngBytes > 8U + 25U + 12U);

    static const uint8_t expectedSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    ASSERT_EQ_INT(0, memcmp(pngBuf, expectedSig, 8));

    size_t offset = 8;
    BOOL sawIhdr = FALSE;
    BOOL sawIdat = FALSE;
    BOOL sawIend = FALSE;

    while (offset + 12U <= pngBytes) {
        uint32_t chunkLen = read_be32(pngBuf + offset);
        const uint8_t* chunkType = pngBuf + offset + 4;
        ASSERT_TRUE(offset + 12U + (size_t)chunkLen <= pngBytes);

        uint32_t actualCrc = verify_crc32_ieee(chunkType, 4U + (size_t)chunkLen);
        uint32_t storedCrc = read_be32(pngBuf + offset + 8U + (size_t)chunkLen);
        ASSERT_EQ_UINT(actualCrc, storedCrc);

        if (memcmp(chunkType, "IHDR", 4) == 0) {
            ASSERT_FALSE(sawIhdr);
            ASSERT_EQ_UINT(13U, chunkLen);
            const uint8_t* d = pngBuf + offset + 8;
            ASSERT_EQ_UINT((uint32_t)W, read_be32(d));
            ASSERT_EQ_UINT((uint32_t)H, read_be32(d + 4));
            ASSERT_EQ_UINT(8U, d[8]);  /* 8-bit depth */
            ASSERT_EQ_UINT(0U, d[10]); /* DEFLATE compression */
            ASSERT_EQ_UINT(0U, d[11]); /* Adaptive filter */
            ASSERT_EQ_UINT(0U, d[12]); /* Non-interlaced */
            sawIhdr = TRUE;
        } else if (memcmp(chunkType, "IDAT", 4) == 0) {
            ASSERT_TRUE(sawIhdr);
            if (!sawIdat) {
                ASSERT_TRUE(chunkLen >= 2U);
                const uint8_t* z = pngBuf + offset + 8;
                /* Zlib CMF & FLG header check: (CMF*256 + FLG) % 31 == 0 and CM == 8 */
                uint32_t cm = (uint32_t)(z[0] & 0x0FU);
                uint32_t fcheck = (((uint32_t)z[0] << 8) | z[1]) % 31U;
                ASSERT_EQ_UINT(8U, cm);
                ASSERT_EQ_UINT(0U, fcheck);
            }
            sawIdat = TRUE;
        } else if (memcmp(chunkType, "IEND", 4) == 0) {
            ASSERT_EQ_UINT(0U, chunkLen);
            sawIend = TRUE;
            offset += 12U;
            break;
        }

        offset += 12U + (size_t)chunkLen;
    }

    ASSERT_TRUE(sawIhdr);
    ASSERT_TRUE(sawIdat);
    ASSERT_TRUE(sawIend);
    ASSERT_EQ_UINT(pngBytes, offset);
}

static void test_perception_dxgi_micro_region_capture(void) {
    ASSERT_HRESULT_OK(Perception_Init());
    PerceptionConfig cfg = { 64, 4096, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&cfg));

    memset(&g_percImg, 0, sizeof(g_percImg));
    HRESULT hr = Perception_CaptureMicroRegion(0, 0, 64, 64, &g_percImg);

    if (hr == E_ACCESSDENIED ||
        hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_UNSUPPORTED ||
        hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
        hr == DXGI_ERROR_WAIT_TIMEOUT) {
        TEST_SKIP("DXGI Desktop Duplication unavailable on current/headless desktop session");
    }

    ASSERT_HRESULT_OK(hr);
    ASSERT_EQ_INT(64, g_percImg.width);
    ASSERT_EQ_INT(64, g_percImg.height);
    ASSERT_TRUE(g_percImg.stride >= 64 * 4);
    ASSERT_TRUE(g_percImg.dataSize >= (size_t)(64 * 64 * 4));
    ASSERT_NOT_NULL(g_percImg.pixels);

    uint8_t outBmp[64 * 64 * 4 + 128];
    size_t outBytes = 0;
    ASSERT_HRESULT_OK(Perception_EncodeBmpInMemory(&g_percImg, outBmp, sizeof(outBmp), &outBytes));
    ASSERT_EQ_INT('B', outBmp[0]);
    ASSERT_EQ_INT('M', outBmp[1]);

    Perception_FreeImageBuffer(&g_percImg);
    ASSERT_NULL(g_percImg.pixels);
}

static void test_perception_boundary_and_nulls(void) {
    ASSERT_HRESULT_OK(Perception_Init());

    ASSERT_HRESULT_FAIL(Perception_Configure(NULL));
    ASSERT_HRESULT_FAIL(Perception_CaptureSemanticTree(g_percWin.hwnd, NULL));
    ASSERT_HRESULT_FAIL(Perception_CaptureSemanticTree((HWND)(uintptr_t)0xDEADBEEF, &g_percTree));
    ASSERT_NULL(g_percTree);
    ASSERT_HRESULT_FAIL(Perception_CaptureDesktopTree(NULL));
    Perception_FreeSemanticTree(NULL);

    uint8_t dummyBuf[64];
    char txtBuf[64];
    size_t written = 0;
    ASSERT_HRESULT_FAIL(Perception_SerializeTree(NULL, dummyBuf, sizeof(dummyBuf), &written));
    ASSERT_HRESULT_FAIL(Perception_SerializeTreeJson(NULL, txtBuf, sizeof(txtBuf), &written));
    ASSERT_HRESULT_FAIL(Perception_SerializeTreeIndented(NULL, txtBuf, sizeof(txtBuf), &written));

    WCHAR wbuf[32];
    ASSERT_HRESULT_FAIL(Perception_FallbackExtractText(NULL, wbuf, 32));
    ASSERT_HRESULT_FAIL(Perception_FallbackExtractText((HWND)(uintptr_t)0xDEADBEEF, wbuf, 32));
    ASSERT_HRESULT_FAIL(Perception_FallbackExtractText(GetDesktopWindow(), NULL, 32));
    ASSERT_HRESULT_FAIL(Perception_FallbackExtractText(GetDesktopWindow(), wbuf, 0));
    ASSERT_HRESULT_FAIL(Perception_FallbackExtractText(GetDesktopWindow(), wbuf, -5));

    /* Inverted and zero-area micro-regions must fail cleanly */
    ASSERT_HRESULT_FAIL(Perception_CaptureMicroRegion(0, 0, 64, 64, NULL));
    ASSERT_HRESULT_FAIL(Perception_CaptureMicroRegion(100, 100, 50, 50, &g_percImg));
    ASSERT_HRESULT_FAIL(Perception_CaptureMicroRegion(50, 50, 50, 100, &g_percImg));
    ASSERT_HRESULT_FAIL(Perception_CaptureMicroRegion(50, 50, 100, 50, &g_percImg));
    Perception_FreeImageBuffer(NULL);

    /* Encoder invalid arg / insufficient buffer */
    uint8_t tiny[8];
    uint8_t dummyPx[16] = {0};
    ImageBuffer validImg = { 2, 2, 8, 16, dummyPx };
    ASSERT_HRESULT_FAIL(Perception_EncodeBmpInMemory(NULL, tiny, sizeof(tiny), &written));
    ASSERT_HRESULT_FAIL(Perception_EncodeBmpInMemory(&validImg, NULL, 100, &written));
    ASSERT_HRESULT_FAIL(Perception_EncodeBmpInMemory(&validImg, tiny, sizeof(tiny), &written));
    ASSERT_HRESULT_FAIL(Perception_EncodePngInMemory(NULL, tiny, sizeof(tiny), &written));
    ASSERT_HRESULT_FAIL(Perception_EncodePngInMemory(&validImg, NULL, 200, &written));
    ASSERT_HRESULT_FAIL(Perception_EncodePngInMemory(&validImg, tiny, sizeof(tiny), &written));

    ImageBuffer badDimImg = { 0, 0, 0, 0, dummyPx };
    ASSERT_HRESULT_FAIL(Perception_EncodeBmpInMemory(&badDimImg, dummyBuf, sizeof(dummyBuf), &written));
    ASSERT_HRESULT_FAIL(Perception_EncodePngInMemory(&badDimImg, dummyBuf, sizeof(dummyBuf), &written));

    ASSERT_FALSE(Perception_PollEvent(NULL));
}

static void test_perception_find_nodes_locator(void) {
    SemanticNode nodes[4];
    memset(nodes, 0, sizeof(nodes));

    nodes[0].id = 0; nodes[0].parentId = -1; nodes[0].depth = 0;
    nodes[0].controlTypeId = UIA_WindowControlTypeId;
    nodes[0].bounds = (RECT){ 0, 0, 800, 600 };
    nodes[0].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(nodes[0].name, L"Main Window");
    wcscpy(nodes[0].className, L"CPowerTestWnd");

    nodes[1].id = 1; nodes[1].parentId = 0; nodes[1].depth = 1;
    nodes[1].controlTypeId = UIA_ButtonControlTypeId;
    nodes[1].bounds = (RECT){ 100, 50, 220, 90 };
    nodes[1].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(nodes[1].name, L"Submit Form Action");
    wcscpy(nodes[1].automationId, L"Btn_Submit_01");
    wcscpy(nodes[1].className, L"Button");

    nodes[2].id = 2; nodes[2].parentId = 0; nodes[2].depth = 1;
    nodes[2].controlTypeId = UIA_ButtonControlTypeId;
    nodes[2].bounds = (RECT){ 100, 120, 220, 160 };
    nodes[2].flags = CPOWER_NODE_FLAG_OFFSCREEN; /* Disabled + Offscreen */
    wcscpy(nodes[2].name, L"Submit Disabled");
    wcscpy(nodes[2].automationId, L"Btn_Disabled");
    wcscpy(nodes[2].className, L"Button");

    nodes[3].id = 3; nodes[3].parentId = 0; nodes[3].depth = 1;
    nodes[3].controlTypeId = UIA_EditControlTypeId;
    nodes[3].bounds = (RECT){ 100, 200, 100, 220 }; /* Zero width */
    nodes[3].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(nodes[3].name, L"HiddenBox");
    wcscpy(nodes[3].className, L"Edit");

    SemanticTree tree = { 4U, 4U, (HWND)(uintptr_t)0x1234, 111111ULL, nodes };

    /* 1. Case-insensitive substring + controlTypeId */
    SemanticQuery q1;
    memset(&q1, 0, sizeof(q1));
    wcscpy(q1.nameSubstr, L"submit");
    q1.controlTypeId = UIA_ButtonControlTypeId;

    SemanticMatch matches[4];
    uint32_t matchCount = 0;
    ASSERT_HRESULT_OK(Perception_FindNodes(&tree, &q1, matches, 4U, &matchCount));
    ASSERT_EQ_UINT(2U, matchCount);
    ASSERT_EQ_UINT(1U, matches[0].nodeId);
    ASSERT_EQ_INT(160, matches[0].center.x);
    ASSERT_EQ_INT(70, matches[0].center.y);

    /* 2. onlyEnabled + onlyVisible filter out disabled/offscreen node 2 */
    q1.onlyEnabled = TRUE;
    q1.onlyVisible = TRUE;
    ASSERT_HRESULT_OK(Perception_FindNodes(&tree, &q1, matches, 4U, &matchCount));
    ASSERT_EQ_UINT(1U, matchCount);
    ASSERT_EQ_UINT(1U, matches[0].nodeId);

    /* 3. Exact automationId (case-insensitive) + maxMatches=1 clamp */
    SemanticQuery q2;
    memset(&q2, 0, sizeof(q2));
    wcscpy(q2.automationId, L"btn_submit_01");
    ASSERT_HRESULT_OK(Perception_FindNodes(&tree, &q2, matches, 1U, &matchCount));
    ASSERT_EQ_UINT(1U, matchCount);
    ASSERT_EQ_UINT(1U, matches[0].nodeId);

    /* 4. onlyVisible excludes zero-area node 3 */
    SemanticQuery q3;
    memset(&q3, 0, sizeof(q3));
    wcscpy(q3.className, L"edit");
    q3.onlyVisible = TRUE;
    ASSERT_HRESULT_OK(Perception_FindNodes(&tree, &q3, matches, 4U, &matchCount));
    ASSERT_EQ_UINT(0U, matchCount);

    /* 5. Count-only query (maxMatches == 0) + NULL & invalid argument boundaries */
    matchCount = 99U;
    ASSERT_HRESULT_OK(Perception_FindNodes(&tree, &q1, NULL, 0U, &matchCount));
    ASSERT_EQ_UINT(1U, matchCount);

    ASSERT_HRESULT_FAIL(Perception_FindNodes(NULL, &q1, matches, 4U, &matchCount));
    ASSERT_HRESULT_FAIL(Perception_FindNodes(&tree, NULL, matches, 4U, &matchCount));
    ASSERT_HRESULT_FAIL(Perception_FindNodes(&tree, &q1, NULL, 4U, &matchCount));
    ASSERT_HRESULT_FAIL(Perception_FindNodes(&tree, &q1, matches, 4U, NULL));
}

static void test_perception_tree_diff_and_serialization(void) {
    SemanticNode prevNodes[3];
    memset(prevNodes, 0, sizeof(prevNodes));
    prevNodes[0].id = 0; prevNodes[0].parentId = -1; prevNodes[0].depth = 0;
    prevNodes[0].controlTypeId = UIA_WindowControlTypeId;
    prevNodes[0].bounds = (RECT){ 0, 0, 500, 400 };
    prevNodes[0].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(prevNodes[0].name, L"AppHost");

    prevNodes[1].id = 1; prevNodes[1].parentId = 0; prevNodes[1].depth = 1;
    prevNodes[1].controlTypeId = UIA_TextControlTypeId;
    prevNodes[1].bounds = (RECT){ 10, 10, 120, 30 };
    prevNodes[1].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(prevNodes[1].automationId, L"LblStatus");
    wcscpy(prevNodes[1].name, L"Status:0");

    prevNodes[2].id = 2; prevNodes[2].parentId = 0; prevNodes[2].depth = 1;
    prevNodes[2].controlTypeId = UIA_ButtonControlTypeId;
    prevNodes[2].bounds = (RECT){ 10, 50, 100, 80 };
    prevNodes[2].flags = CPOWER_NODE_FLAG_ENABLED;
    wcscpy(prevNodes[2].automationId, L"BtnOld");
    wcscpy(prevNodes[2].name, L"RemoveMe");

    SemanticTree prevTree = { 3U, 3U, (HWND)(uintptr_t)0x5678, 10000ULL, prevNodes };

    /* 1. Identical trees -> 0 diff entries */
    ASSERT_HRESULT_OK(Perception_DiffTrees(&prevTree, &prevTree, &g_percDiff));
    ASSERT_NOT_NULL(g_percDiff);
    ASSERT_EQ_UINT(0U, g_percDiff->entryCount);
    ASSERT_EQ_UINT(0U, g_percDiff->addedCount);
    ASSERT_EQ_UINT(0U, g_percDiff->removedCount);
    ASSERT_EQ_UINT(0U, g_percDiff->modifiedCount);
    Perception_FreeTreeDiff(g_percDiff);
    g_percDiff = NULL;

    /* 2. prev == NULL -> all nodes marked ADDED */
    ASSERT_HRESULT_OK(Perception_DiffTrees(NULL, &prevTree, &g_percDiff));
    ASSERT_NOT_NULL(g_percDiff);
    ASSERT_EQ_UINT(3U, g_percDiff->entryCount);
    ASSERT_EQ_UINT(3U, g_percDiff->addedCount);
    Perception_FreeTreeDiff(g_percDiff);
    g_percDiff = NULL;

    /* 3. Mixed MODIFIED (Status:0 -> Status:1 + bounds) + REMOVED (BtnOld) + ADDED (BtnNew) */
    SemanticNode currNodes[3];
    memcpy(currNodes, prevNodes, sizeof(prevNodes));
    wcscpy(currNodes[1].name, L"Status:1");
    currNodes[1].bounds.right = 140;
    currNodes[2].id = 2;
    wcscpy(currNodes[2].automationId, L"BtnNew");
    wcscpy(currNodes[2].name, L"AddedControl");
    currNodes[2].bounds = (RECT){ 20, 90, 150, 120 };

    SemanticTree currTree = { 3U, 3U, (HWND)(uintptr_t)0x5678, 20000ULL, currNodes };
    ASSERT_HRESULT_OK(Perception_DiffTrees(&prevTree, &currTree, &g_percDiff));
    ASSERT_NOT_NULL(g_percDiff);
    ASSERT_EQ_UINT(1U, g_percDiff->addedCount);
    ASSERT_EQ_UINT(1U, g_percDiff->removedCount);
    ASSERT_EQ_UINT(1U, g_percDiff->modifiedCount);
    ASSERT_EQ_UINT(3U, g_percDiff->entryCount);

    BOOL sawMod = FALSE, sawAdd = FALSE, sawRem = FALSE;
    for (uint32_t i = 0; i < g_percDiff->entryCount; i++) {
        const TreeDiffEntry* e = &g_percDiff->entries[i];
        if (e->op == DIFF_OP_MODIFIED) {
            sawMod = TRUE;
            ASSERT_TRUE((e->changedMask & CPOWER_DIFF_CHANGE_NAME) != 0U);
            ASSERT_TRUE((e->changedMask & CPOWER_DIFF_CHANGE_BOUNDS) != 0U);
        } else if (e->op == DIFF_OP_ADDED) {
            sawAdd = TRUE;
            ASSERT_EQ_WSTR(L"AddedControl", e->node.name);
        } else if (e->op == DIFF_OP_REMOVED) {
            sawRem = TRUE;
            ASSERT_EQ_WSTR(L"RemoveMe", e->node.name);
        }
    }
    ASSERT_TRUE(sawMod && sawAdd && sawRem);

    /* 4. JSON & Binary DIFF serialization + buffer insufficiency */
    char diffJson[16384];
    size_t jsonBytes = 0;
    ASSERT_HRESULT_OK(Perception_SerializeDiffJson(g_percDiff, diffJson, sizeof(diffJson), &jsonBytes));
    ASSERT_TRUE(jsonBytes > 20U);
    ASSERT_NOT_NULL(strstr(diffJson, "\"modifiedCount\":1"));
    ASSERT_NOT_NULL(strstr(diffJson, "AddedControl"));

    char tinyTxt[8];
    size_t tinyNeed = 0;
    ASSERT_EQ_HRESULT(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                      Perception_SerializeDiffJson(g_percDiff, tinyTxt, sizeof(tinyTxt), &tinyNeed));

    uint8_t diffBin[8192];
    size_t binBytes = 0;
    ASSERT_HRESULT_OK(Perception_SerializeDiffBinary(g_percDiff, diffBin, sizeof(diffBin), &binBytes));
    ASSERT_EQ_UINT(sizeof(DiffPacketHeader) + g_percDiff->entryCount * sizeof(TreeDiffEntry), binBytes);
    DiffPacketHeader dh;
    memcpy(&dh, diffBin, sizeof(dh));
    ASSERT_EQ_UINT(CPOWER_DIFF_PACKET_MAGIC, dh.packetMagic);
    ASSERT_EQ_UINT(g_percDiff->entryCount, dh.entryCount);
    ASSERT_EQ_UINT((uint32_t)binBytes, dh.totalPayloadSize);

    uint8_t tinyBin[16];
    ASSERT_EQ_HRESULT(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                      Perception_SerializeDiffBinary(g_percDiff, tinyBin, sizeof(tinyBin), &tinyNeed));

    /* Boundary nulls */
    ASSERT_HRESULT_FAIL(Perception_DiffTrees(&prevTree, NULL, &g_percDiff));
    ASSERT_HRESULT_FAIL(Perception_DiffTrees(&prevTree, &currTree, NULL));
    Perception_FreeTreeDiff(NULL);
}

void RegisterPerceptionTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "PerceptionSuite",
        "UIA COM tree traversal, serialization, WinEvent hook, GDI fallback, BMP/PNG, DXGI, locator, and diff",
        perception_suite_setup,
        perception_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_Perception_InitShutdownIdempotency",
                       "Verify repeated Perception_Init/Configure/Shutdown lifecycle cycles",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_init_shutdown_idempotency);

    test_case_register(suite, "Test_Perception_CaptureWin32SemanticTree",
                       "Capture SemanticTree from live Win32 GUI window and verify nodes, bounds, and types",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_capture_win32_semantic_tree);

    test_case_register(suite, "Test_Perception_DesktopAndNodeClamping",
                       "Verify bounded desktop tree capture and maxNodes limit clamping",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_desktop_and_node_clamping);

    test_case_register(suite, "Test_Perception_SerializeBinaryJsonIndented",
                       "Verify binary TREE packet, JSON escaping, indented format, and ERROR_INSUFFICIENT_BUFFER",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_serialize_binary_json_indented);

    test_case_register(suite, "Test_Perception_WinEventHookRealTime",
                       "Verify real-time SetWinEventHook callback and PollEvent on external GUI window events",
                       4, TIER_1_FEATURE_COVERAGE, test_perception_winevent_hook_realtime);

    test_case_register(suite, "Test_Perception_FallbackTextAndPoke",
                       "Verify WM_GETTEXT, multiline EM_GETLINE extraction, canary truncation, and Chromium poke",
                       5, TIER_1_FEATURE_COVERAGE, test_perception_fallback_text_and_poke);

    test_case_register(suite, "Test_Perception_BmpAndPngEncoders",
                       "Verify 32-bit top-down BMP header and RFC 2083 PNG signature, chunks, zlib header, and CRC-32",
                       6, TIER_1_FEATURE_COVERAGE, test_perception_bmp_and_png_encoders);

    test_case_register(suite, "Test_Perception_DxgiMicroRegionCapture",
                       "Verify DXGI micro-region capture and graceful handling on non-default/headless desktops",
                       6, TIER_3_CROSS_FEATURE, test_perception_dxgi_micro_region_capture);

    test_case_register(suite, "Test_Perception_FindNodesSemanticLocator",
                       "Verify case-insensitive Perception_FindNodes queries, visibility filters, centers, and boundaries",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_find_nodes_locator);

    test_case_register(suite, "Test_Perception_TreeDiffAndSerialization",
                       "Verify Perception_DiffTrees delta accuracy, JSON/Binary serialization, and buffer boundaries",
                       3, TIER_1_FEATURE_COVERAGE, test_perception_tree_diff_and_serialization);

    test_case_register(suite, "Test_Perception_BoundaryAndNulls",
                       "Verify NULL pointers, invalid HWNDs, inverted rects, and zero-byte buffer errors",
                       16, TIER_2_BOUNDARY_CORNER, test_perception_boundary_and_nulls);
}
