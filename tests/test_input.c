/**
 * @file test_input.c
 * @brief Tier 1, Tier 2, Tier 3, and Tier 5 tests for Dual-Mode Control Engine:
 *        Minimum-jerk, cubic Bezier, Ornstein-Uhlenbeck trajectory synthesis,
 *        virtual desktop mapping, Win32 direct messaging, and UIA COM patterns.
 */

#include "test_infra.h"
#include "input.h"
#include <limits.h>
#include <wchar.h>

/* Local static UIA GUIDs to avoid linking conflicts with uuid.lib or INITGUID */
static const GUID kInputClsidCUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID kInputIidIUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
static const GUID kInputIidIUIAutomationTogglePattern =
    { 0x94cf8058, 0x9b8d, 0x4ab9, { 0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70 } };

typedef struct InputTestWin {
    HANDLE hThread;
    HANDLE hReadyEvent;
    HWND   hwnd;
    HWND   hBtn;
    HWND   hEditSingle;
    HWND   hEditReadOnly;
    HWND   hCheck;
    HWND   hListBox;
    ATOM   classAtom;
    WCHAR  className[64];
    volatile LONG clickCount;
    volatile LONG clientClickCount;
    volatile LONG lastClientX;
    volatile LONG lastClientY;
} InputTestWin;

static InputTestWin g_inWin;
static LONG g_inWinSeq = 0;

/* Tracked COM objects for teardown leak immunity on longjmp failures */
static IUIAutomation*        g_pInAuto = NULL;
static IUIAutomationElement* g_pBtnElem = NULL;
static IUIAutomationElement* g_pEditElem = NULL;
static IUIAutomationElement* g_pRoEditElem = NULL;
static IUIAutomationElement* g_pCheckElem = NULL;
static IUIAutomationElement* g_pListElem = NULL;
static IUIAutomationElement* g_pListItem0 = NULL;
static IUIAutomationElement* g_pListItem1 = NULL;
static IUIAutomationTreeWalker* g_pInWalker = NULL;
static BOOL g_inComInit = FALSE;
static SemanticTreeDiff* g_inDiff = NULL;
static BOOL g_inPercInit = FALSE;

static LRESULT CALLBACK input_test_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InputTestWin* pWin = (InputTestWin*)(uintptr_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* cs = (const CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_COMMAND:
            if (pWin && LOWORD(wParam) == 3001 && HIWORD(wParam) == BN_CLICKED) {
                InterlockedIncrement(&pWin->clickCount);
            }
            break;
        case WM_LBUTTONUP:
            if (pWin) {
                InterlockedExchange(&pWin->lastClientX, (LONG)(int16_t)LOWORD(lParam));
                InterlockedExchange(&pWin->lastClientY, (LONG)(int16_t)HIWORD(lParam));
                InterlockedIncrement(&pWin->clientClickCount);
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

static DWORD WINAPI input_ui_thread(LPVOID lpParam) {
    InputTestWin* w = (InputTestWin*)lpParam;
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = input_test_wndproc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = w->className;
    w->classAtom = RegisterClassExW(&wc);
    if (w->classAtom == 0) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hwnd = CreateWindowExW(
        0, w->className, L"CPowerInputTestWindow",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 120, 440, 360,
        NULL, NULL, hInst, w
    );
    if (!w->hwnd) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hBtn = CreateWindowExW(
        0, L"BUTTON", L"DirectInvokeBtn",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 20, 160, 32,
        w->hwnd, (HMENU)(uintptr_t)3001, hInst, NULL
    );
    w->hEditSingle = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"StartValue",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        20, 64, 240, 26,
        w->hwnd, (HMENU)(uintptr_t)3002, hInst, NULL
    );
    w->hEditReadOnly = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"ReadOnlyContent",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_READONLY,
        20, 102, 240, 26,
        w->hwnd, (HMENU)(uintptr_t)3003, hInst, NULL
    );
    w->hCheck = CreateWindowExW(
        0, L"BUTTON", L"ToggleOption",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        20, 140, 160, 28,
        w->hwnd, (HMENU)(uintptr_t)3004, hInst, NULL
    );
    w->hListBox = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY,
        20, 180, 200, 90,
        w->hwnd, (HMENU)(uintptr_t)3005, hInst, NULL
    );
    SendMessageW(w->hListBox, LB_ADDSTRING, 0, (LPARAM)L"ItemAlpha");
    SendMessageW(w->hListBox, LB_ADDSTRING, 0, (LPARAM)L"ItemBeta");

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

static BOOL start_input_test_window(void) {
    memset(&g_inWin, 0, sizeof(g_inWin));
    LONG seq = InterlockedIncrement(&g_inWinSeq);
    swprintf(g_inWin.className, 64, L"CPowerInputCls_%lu_%ld", GetCurrentProcessId(), seq);
    g_inWin.hReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_inWin.hReadyEvent) return FALSE;

    g_inWin.hThread = CreateThread(NULL, 0, input_ui_thread, &g_inWin, 0, NULL);
    if (!g_inWin.hThread) {
        CloseHandle(g_inWin.hReadyEvent);
        g_inWin.hReadyEvent = NULL;
        return FALSE;
    }

    if (WaitForSingleObject(g_inWin.hReadyEvent, 5000) != WAIT_OBJECT_0 || !g_inWin.hwnd) {
        return FALSE;
    }
    Sleep(50);
    return TRUE;
}

static void stop_input_test_window(void) {
    if (g_inWin.hwnd && IsWindow(g_inWin.hwnd)) {
        PostMessageW(g_inWin.hwnd, WM_CLOSE, 0, 0);
    }
    if (g_inWin.hThread) {
        if (WaitForSingleObject(g_inWin.hThread, 4000) == WAIT_TIMEOUT && g_inWin.hwnd) {
            PostThreadMessageW(GetThreadId(g_inWin.hThread), WM_QUIT, 0, 0);
            WaitForSingleObject(g_inWin.hThread, 2000);
        }
        CloseHandle(g_inWin.hThread);
        g_inWin.hThread = NULL;
    }
    if (g_inWin.hReadyEvent) {
        CloseHandle(g_inWin.hReadyEvent);
        g_inWin.hReadyEvent = NULL;
    }
    if (g_inWin.classAtom != 0) {
        UnregisterClassW(g_inWin.className, GetModuleHandleW(NULL));
        g_inWin.classAtom = 0;
    }
    g_inWin.hwnd = NULL;
}

static void release_input_com_objects(void) {
    CPOWER_SAFE_RELEASE(g_pListItem1);
    CPOWER_SAFE_RELEASE(g_pListItem0);
    CPOWER_SAFE_RELEASE(g_pListElem);
    CPOWER_SAFE_RELEASE(g_pCheckElem);
    CPOWER_SAFE_RELEASE(g_pRoEditElem);
    CPOWER_SAFE_RELEASE(g_pEditElem);
    CPOWER_SAFE_RELEASE(g_pBtnElem);
    CPOWER_SAFE_RELEASE(g_pInWalker);
    CPOWER_SAFE_RELEASE(g_pInAuto);
    if (g_inComInit) {
        CoUninitialize();
        g_inComInit = FALSE;
    }
}

static BOOL acquire_uia_test_elements(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        g_inComInit = TRUE;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return FALSE;
    }

    hr = CoCreateInstance(&kInputClsidCUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &kInputIidIUIAutomation, (void**)&g_pInAuto);
    if (FAILED(hr) || !g_pInAuto) return FALSE;

    hr = IUIAutomation_ElementFromHandle(g_pInAuto, (UIA_HWND)g_inWin.hBtn, &g_pBtnElem);
    if (FAILED(hr) || !g_pBtnElem) return FALSE;

    hr = IUIAutomation_ElementFromHandle(g_pInAuto, (UIA_HWND)g_inWin.hEditSingle, &g_pEditElem);
    if (FAILED(hr) || !g_pEditElem) return FALSE;

    hr = IUIAutomation_ElementFromHandle(g_pInAuto, (UIA_HWND)g_inWin.hEditReadOnly, &g_pRoEditElem);
    if (FAILED(hr) || !g_pRoEditElem) return FALSE;

    hr = IUIAutomation_ElementFromHandle(g_pInAuto, (UIA_HWND)g_inWin.hCheck, &g_pCheckElem);
    if (FAILED(hr) || !g_pCheckElem) return FALSE;

    hr = IUIAutomation_ElementFromHandle(g_pInAuto, (UIA_HWND)g_inWin.hListBox, &g_pListElem);
    if (FAILED(hr) || !g_pListElem) return FALSE;

    hr = IUIAutomation_get_ControlViewWalker(g_pInAuto, &g_pInWalker);
    if (FAILED(hr) || !g_pInWalker) return FALSE;

    hr = IUIAutomationTreeWalker_GetFirstChildElement(g_pInWalker, g_pListElem, &g_pListItem0);
    if (FAILED(hr) || !g_pListItem0) return FALSE;

    hr = IUIAutomationTreeWalker_GetNextSiblingElement(g_pInWalker, g_pListItem0, &g_pListItem1);
    if (FAILED(hr) || !g_pListItem1) return FALSE;

    return TRUE;
}

static void input_suite_setup(void) {
    memset(&g_inWin, 0, sizeof(g_inWin));
    g_pInAuto = NULL;
    g_pBtnElem = NULL;
    g_pEditElem = NULL;
    g_pRoEditElem = NULL;
    g_pCheckElem = NULL;
    g_pListElem = NULL;
    g_pListItem0 = NULL;
    g_pListItem1 = NULL;
    g_pInWalker = NULL;
    g_inComInit = FALSE;
    g_inDiff = NULL;
    g_inPercInit = FALSE;
}

static void input_suite_teardown(void) {
    Control_SetFailsafeEnabled(TRUE);
    if (g_inDiff) {
        Perception_FreeTreeDiff(g_inDiff);
        g_inDiff = NULL;
    }
    release_input_com_objects();
    stop_input_test_window();
    if (g_inPercInit) {
        Perception_Shutdown();
        g_inPercInit = FALSE;
    }
    Control_Shutdown();
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void test_input_init_configure_shutdown(void) {
    ASSERT_HRESULT_OK(Control_Init());

    ControlConfig cfg;
    cfg.defaultMode = CONTROL_MODE_DIRECT_INSTANT;
    cfg.trajectory.speedScale = 0.8;
    cfg.trajectory.tremorScale = 1.0;
    cfg.trajectory.curvatureScale = 1.0;
    cfg.trajectory.ouTheta = 15.0;
    cfg.trajectory.ouSigma = 2.5;
    cfg.trajectory.minSteps = 12;
    cfg.sendMessageTimeoutMs = 1000;
    cfg.typingWpm = 70.0;

    ASSERT_HRESULT_OK(Control_Configure(&cfg));
    ASSERT_HRESULT_FAIL(Control_Configure(NULL));

    Control_Shutdown();
    Control_Shutdown();

    ASSERT_HRESULT_OK(Control_Init());
    Control_Shutdown();
}

static void test_input_minimum_jerk_math(void) {
    ASSERT_DOUBLE_NEAR(0.0, Control_MinimumJerk(0.0), 1e-12);
    ASSERT_DOUBLE_NEAR(1.0, Control_MinimumJerk(1.0), 1e-12);
    ASSERT_DOUBLE_NEAR(0.5, Control_MinimumJerk(0.5), 1e-12);
    ASSERT_DOUBLE_NEAR(0.103515625, Control_MinimumJerk(0.25), 1e-9);
    ASSERT_DOUBLE_NEAR(0.896484375, Control_MinimumJerk(0.75), 1e-9);

    /* Monotonicity & symmetry in [0, 1] */
    double prev = -1.0;
    for (int i = 0; i <= 100; i++) {
        double tau = (double)i / 100.0;
        double s = Control_MinimumJerk(tau);
        ASSERT_TRUE(s >= 0.0 && s <= 1.0);
        if (i > 0) {
            ASSERT_TRUE(s >= prev);
        }
        prev = s;

        double sMirror = Control_MinimumJerk(1.0 - tau);
        ASSERT_DOUBLE_NEAR(1.0, s + sMirror, 1e-9);
    }

    /* Clamping outside [0, 1] */
    ASSERT_DOUBLE_NEAR(0.0, Control_MinimumJerk(-0.01), 1e-12);
    ASSERT_DOUBLE_NEAR(0.0, Control_MinimumJerk(-100.0), 1e-12);
    ASSERT_DOUBLE_NEAR(1.0, Control_MinimumJerk(1.01), 1e-12);
    ASSERT_DOUBLE_NEAR(1.0, Control_MinimumJerk(100.0), 1e-12);
}

static void test_input_cubic_bezier_eval(void) {
    BezierPoint p0 = { 10.0, 20.0 };
    BezierPoint p1 = { 40.0, 200.0 };
    BezierPoint p2 = { 180.0, -40.0 };
    BezierPoint p3 = { 310.0, 120.0 };

    BezierPoint start = Control_EvalCubicBezier(p0, p1, p2, p3, 0.0);
    BezierPoint end = Control_EvalCubicBezier(p0, p1, p2, p3, 1.0);
    ASSERT_DOUBLE_NEAR(p0.x, start.x, 1e-9);
    ASSERT_DOUBLE_NEAR(p0.y, start.y, 1e-9);
    ASSERT_DOUBLE_NEAR(p3.x, end.x, 1e-9);
    ASSERT_DOUBLE_NEAR(p3.y, end.y, 1e-9);

    /* Symmetric arch analytical midpoint */
    BezierPoint a0 = { 0.0, 0.0 };
    BezierPoint a1 = { 0.0, 100.0 };
    BezierPoint a2 = { 100.0, 100.0 };
    BezierPoint a3 = { 100.0, 0.0 };
    BezierPoint amid = Control_EvalCubicBezier(a0, a1, a2, a3, 0.5);
    ASSERT_DOUBLE_NEAR(50.0, amid.x, 1e-9);
    ASSERT_DOUBLE_NEAR(75.0, amid.y, 1e-9);

    /* Collinear control points must reproduce exact straight-line interpolation */
    BezierPoint c0 = { 50.0, 80.0 };
    BezierPoint c3 = { 350.0, 280.0 };
    BezierPoint c1 = { c0.x + (c3.x - c0.x) / 3.0, c0.y + (c3.y - c0.y) / 3.0 };
    BezierPoint c2 = { c0.x + 2.0 * (c3.x - c0.x) / 3.0, c0.y + 2.0 * (c3.y - c0.y) / 3.0 };
    for (int i = 0; i <= 20; i++) {
        double s = (double)i / 20.0;
        BezierPoint pt = Control_EvalCubicBezier(c0, c1, c2, c3, s);
        ASSERT_DOUBLE_NEAR(c0.x + s * (c3.x - c0.x), pt.x, 1e-6);
        ASSERT_DOUBLE_NEAR(c0.y + s * (c3.y - c0.y), pt.y, 1e-6);
    }
}

static double point_to_segment_perp_dist(double px, double py,
                                         double x0, double y0,
                                         double x1, double y1) {
    double dx = x1 - x0;
    double dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-9) return 0.0;
    return fabs(dy * px - dx * py + x1 * y0 - y1 * x0) / len;
}

static void test_input_trajectory_generation_and_tremor(void) {
    ASSERT_HRESULT_OK(Control_Init());

    BezierPoint pts[512];
    int count = 0;

    /* 1. Curved Bezier without tremor */
    TrajectoryConfig cfgCurve = { 1.0, 0.0, 1.0, 15.0, 2.5, 25 };
    double maxCurvatureDev = 0.0;
    for (int trial = 0; trial < 5; trial++) {
        count = 0;
        ASSERT_HRESULT_OK(Control_GenerateTrajectoryPoints(100, 100, 700, 500, &cfgCurve, pts, 512, &count));
        ASSERT_TRUE(count >= 25 && count <= 512);
        for (int i = 0; i < count; i++) {
            ASSERT_TRUE(isfinite(pts[i].x) && isfinite(pts[i].y));
            double d = point_to_segment_perp_dist(pts[i].x, pts[i].y, 100.0, 100.0, 700.0, 500.0);
            if (d > maxCurvatureDev) maxCurvatureDev = d;
        }
        ASSERT_DOUBLE_NEAR(700.0, pts[count - 1].x, 0.5);
        ASSERT_DOUBLE_NEAR(500.0, pts[count - 1].y, 0.5);
    }
    ASSERT_TRUE(maxCurvatureDev > 1.0);

    /* 2. Zero curvature + active Ornstein-Uhlenbeck tremor */
    TrajectoryConfig cfgTremor = { 1.0, 1.5, 0.0, 15.0, 3.0, 45 };
    double rmsTremor = 0.0;
    count = 0;
    ASSERT_HRESULT_OK(Control_GenerateTrajectoryPoints(50, 50, 550, 350, &cfgTremor, pts, 512, &count));
    ASSERT_TRUE(count >= 45 && count <= 512);
    for (int i = 0; i < count; i++) {
        ASSERT_TRUE(isfinite(pts[i].x) && isfinite(pts[i].y));
        double d = point_to_segment_perp_dist(pts[i].x, pts[i].y, 50.0, 50.0, 550.0, 350.0);
        rmsTremor += d * d;
    }
    rmsTremor = sqrt(rmsTremor / (double)count);
    ASSERT_TRUE(rmsTremor > 0.05);
    /* Proximity damping must land cleanly on exact target */
    ASSERT_DOUBLE_NEAR(550.0, pts[count - 1].x, 0.5);
    ASSERT_DOUBLE_NEAR(350.0, pts[count - 1].y, 0.5);
}

static void test_input_virtual_desktop_mapping(void) {
    int vLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    ASSERT_TRUE(vWidth > 0 && vHeight > 0);

    LONG nx = -1, ny = -1;
    Control_ScreenToVirtualDesktop(vLeft, vTop, &nx, &ny);
    ASSERT_EQ_INT(0, nx);
    ASSERT_EQ_INT(0, ny);

    Control_ScreenToVirtualDesktop(vLeft + vWidth - 1, vTop + vHeight - 1, &nx, &ny);
    ASSERT_EQ_INT(65535, nx);
    ASSERT_EQ_INT(65535, ny);

    Control_ScreenToVirtualDesktop(vLeft + (vWidth - 1) / 2, vTop + (vHeight - 1) / 2, &nx, &ny);
    ASSERT_TRUE(nx >= 32700 && nx <= 32836);
    ASSERT_TRUE(ny >= 32700 && ny <= 32836);

    /* Out-of-bounds clamping */
    Control_ScreenToVirtualDesktop(-50000, -50000, &nx, &ny);
    ASSERT_TRUE(nx >= 0 && nx <= 65535);
    ASSERT_TRUE(ny >= 0 && ny <= 65535);

    Control_ScreenToVirtualDesktop(50000, 50000, &nx, &ny);
    ASSERT_TRUE(nx >= 0 && nx <= 65535);
    ASSERT_TRUE(ny >= 0 && ny <= 65535);

    /* NULL output pointers must be safe */
    Control_ScreenToVirtualDesktop(100, 100, NULL, NULL);
}

static void test_input_direct_win32_messaging(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_input_test_window());

    ASSERT_EQ_INT(0, InterlockedCompareExchange(&g_inWin.clickCount, 0, 0));
    ASSERT_HRESULT_OK(Control_DirectClickButtonHwnd(g_inWin.hBtn));
    ASSERT_EQ_INT(1, InterlockedCompareExchange(&g_inWin.clickCount, 0, 0));
    ASSERT_HRESULT_OK(Control_DirectClickButtonHwnd(g_inWin.hBtn));
    ASSERT_EQ_INT(2, InterlockedCompareExchange(&g_inWin.clickCount, 0, 0));

    ASSERT_HRESULT_OK(Control_DirectSetTextHwnd(g_inWin.hEditSingle, L"DirectWin32Text_99"));
    WCHAR buf[128];
    memset(buf, 0, sizeof(buf));
    GetWindowTextW(g_inWin.hEditSingle, buf, 128);
    ASSERT_EQ_WSTR(L"DirectWin32Text_99", buf);

    /* Async client-coordinate click directly to main window client area */
    ASSERT_HRESULT_OK(Control_DirectClickHwnd(g_inWin.hwnd, 260, 220));
    DWORD t0 = GetTickCount();
    while (InterlockedCompareExchange(&g_inWin.clientClickCount, 0, 0) == 0 &&
           (GetTickCount() - t0) < 1000U) {
        Sleep(10);
    }
    ASSERT_TRUE(InterlockedCompareExchange(&g_inWin.clientClickCount, 0, 0) >= 1);
    ASSERT_EQ_INT(260, InterlockedCompareExchange(&g_inWin.lastClientX, 0, 0));
    ASSERT_EQ_INT(220, InterlockedCompareExchange(&g_inWin.lastClientY, 0, 0));

    /* Explicit (0, 0) client-coordinate click must not be dropped or treated as missing */
    LONG prevClientClicks = InterlockedCompareExchange(&g_inWin.clientClickCount, 0, 0);
    ASSERT_HRESULT_OK(Control_DirectClickHwnd(g_inWin.hwnd, 0, 0));
    DWORD tZero = GetTickCount();
    while (InterlockedCompareExchange(&g_inWin.clientClickCount, 0, 0) == prevClientClicks &&
           (GetTickCount() - tZero) < 1000U) {
        Sleep(10);
    }
    ASSERT_TRUE(InterlockedCompareExchange(&g_inWin.clientClickCount, 0, 0) > prevClientClicks);
    ASSERT_EQ_INT(0, InterlockedCompareExchange(&g_inWin.lastClientX, 0, 0));
    ASSERT_EQ_INT(0, InterlockedCompareExchange(&g_inWin.lastClientY, 0, 0));
}

static void test_input_direct_uia_com_patterns(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_input_test_window());
    ASSERT_TRUE(acquire_uia_test_elements());

    /* 1. IUIAutomationInvokePattern */
    LONG beforeClicks = InterlockedCompareExchange(&g_inWin.clickCount, 0, 0);
    ASSERT_HRESULT_OK(Control_DirectInvoke(g_pBtnElem));
    DWORD t0 = GetTickCount();
    while (InterlockedCompareExchange(&g_inWin.clickCount, 0, 0) == beforeClicks &&
           (GetTickCount() - t0) < 1000U) {
        Sleep(10);
    }
    ASSERT_EQ_INT(beforeClicks + 1, InterlockedCompareExchange(&g_inWin.clickCount, 0, 0));

    /* 2. IUIAutomationValuePattern */
    ASSERT_HRESULT_OK(Control_DirectSetValue(g_pEditElem, L"DirectUiaValue_ABC"));
    WCHAR readBack[128];
    memset(readBack, 0, sizeof(readBack));
    GetWindowTextW(g_inWin.hEditSingle, readBack, 128);
    ASSERT_EQ_WSTR(L"DirectUiaValue_ABC", readBack);

    /* 3. IUIAutomationTogglePattern (Win32 BS_AUTOCHECKBOX MSAA proxy refreshes async) */
    enum ToggleState togState = ToggleState_Indeterminate;
    ASSERT_HRESULT_OK(Control_DirectToggle(g_pCheckElem, &togState));
    ASSERT_TRUE(togState == ToggleState_Off || togState == ToggleState_On);
    ASSERT_EQ_INT(BST_CHECKED, (int)SendMessageW(g_inWin.hCheck, BM_GETCHECK, 0, 0));

    void* pRawTog = NULL;
    ASSERT_HRESULT_OK(IUIAutomationElement_GetCurrentPatternAs(
        g_pCheckElem, UIA_TogglePatternId, &kInputIidIUIAutomationTogglePattern, &pRawTog
    ));
    ASSERT_NOT_NULL(pRawTog);
    IUIAutomationTogglePattern* pTog = (IUIAutomationTogglePattern*)pRawTog;
    enum ToggleState uiaState = ToggleState_Off;
    DWORD tTog = GetTickCount();
    while ((GetTickCount() - tTog) < 500U) {
        if (SUCCEEDED(IUIAutomationTogglePattern_get_CurrentToggleState(pTog, &uiaState)) &&
            uiaState == ToggleState_On) {
            break;
        }
        Sleep(20);
    }
    IUIAutomationTogglePattern_Release(pTog);
    ASSERT_EQ_INT((int)ToggleState_On, (int)uiaState);

    /* Second toggle exercises NULL pNewState fast-path back to Unchecked */
    ASSERT_HRESULT_OK(Control_DirectToggle(g_pCheckElem, NULL));
    ASSERT_EQ_INT(BST_UNCHECKED, (int)SendMessageW(g_inWin.hCheck, BM_GETCHECK, 0, 0));

    /* 4. IUIAutomationSelectionItemPattern */
    ASSERT_HRESULT_OK(Control_DirectSelect(g_pListItem1, FALSE));
    ASSERT_EQ_INT(1, (int)SendMessageW(g_inWin.hListBox, LB_GETCURSEL, 0, 0));
}

static void test_input_safe_emulation_dispatch(void) {
    ASSERT_HRESULT_OK(Control_Init());

    /* Empty string typing injects zero keystrokes and succeeds cleanly */
    ASSERT_HRESULT_OK(Control_EmulateTypeText(L"", 1.0));

    /* Tiny 1-pixel mouse move around current cursor pos avoids cursor hijacking */
    POINT cur;
    if (GetCursorPos(&cur)) {
        ASSERT_HRESULT_OK(Control_EmulateMouseMove(cur.x + 1, cur.y + 1, 0.1));
        ASSERT_HRESULT_OK(Control_EmulateMouseMove(cur.x, cur.y, 0.1));
    }
}

static void test_input_boundary_and_invalid_args(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_input_test_window());
    ASSERT_TRUE(acquire_uia_test_elements());

    /* Null & unsupported COM pattern checks */
    ASSERT_HRESULT_FAIL(Control_DirectInvoke(NULL));
    ASSERT_HRESULT_FAIL(Control_DirectInvoke(g_pEditElem));
    ASSERT_HRESULT_FAIL(Control_DirectSetValue(NULL, L"test"));
    ASSERT_HRESULT_FAIL(Control_DirectSetValue(g_pEditElem, NULL));
    ASSERT_HRESULT_FAIL(Control_DirectSetValue(g_pRoEditElem, L"AttemptWrite"));

    /* Confirm read-only edit was NOT modified */
    WCHAR roBuf[64];
    GetWindowTextW(g_inWin.hEditReadOnly, roBuf, 64);
    ASSERT_EQ_WSTR(L"ReadOnlyContent", roBuf);

    ASSERT_HRESULT_FAIL(Control_DirectToggle(NULL, NULL));
    ASSERT_HRESULT_FAIL(Control_DirectToggle(g_pEditElem, NULL));
    ASSERT_HRESULT_FAIL(Control_DirectSelect(NULL, FALSE));
    ASSERT_HRESULT_FAIL(Control_DirectSelect(g_pEditElem, FALSE));

    /* Invalid HWNDs & NULL text */
    HWND badHwnd = (HWND)(uintptr_t)0xDEADBEEF;
    ASSERT_HRESULT_FAIL(Control_DirectClickButtonHwnd(NULL));
    ASSERT_HRESULT_FAIL(Control_DirectClickButtonHwnd(badHwnd));
    ASSERT_HRESULT_FAIL(Control_DirectSetTextHwnd(NULL, L"x"));
    ASSERT_HRESULT_FAIL(Control_DirectSetTextHwnd(badHwnd, L"x"));
    ASSERT_HRESULT_FAIL(Control_DirectSetTextHwnd(g_inWin.hEditSingle, NULL));
    ASSERT_HRESULT_FAIL(Control_DirectClickHwnd(NULL, 0, 0));
    ASSERT_HRESULT_FAIL(Control_DirectClickHwnd(badHwnd, 0, 0));

    /* Trajectory invalid arguments & canary bounds check */
    BezierPoint pts[8];
    int outCnt = 0;
    TrajectoryConfig cfg = { 1.0, 1.0, 1.0, 15.0, 2.5, 10 };
    ASSERT_HRESULT_FAIL(Control_GenerateTrajectoryPoints(0, 0, 100, 100, &cfg, NULL, 8, &outCnt));
    ASSERT_HRESULT_FAIL(Control_GenerateTrajectoryPoints(0, 0, 100, 100, &cfg, pts, 8, NULL));
    ASSERT_HRESULT_FAIL(Control_GenerateTrajectoryPoints(0, 0, 100, 100, &cfg, pts, 0, &outCnt));
    ASSERT_HRESULT_FAIL(Control_GenerateTrajectoryPoints(0, 0, 100, 100, &cfg, pts, -3, &outCnt));

    for (int i = 0; i < 8; i++) {
        pts[i].x = -9999.0;
        pts[i].y = -9999.0;
    }
    HRESULT hrTiny = Control_GenerateTrajectoryPoints(10, 20, 300, 400, &cfg, pts, 2, &outCnt);
    if (SUCCEEDED(hrTiny)) {
        ASSERT_TRUE(outCnt >= 1 && outCnt <= 2);
    }
    for (int i = 2; i < 8; i++) {
        ASSERT_DOUBLE_NEAR(-9999.0, pts[i].x, 1e-9);
        ASSERT_DOUBLE_NEAR(-9999.0, pts[i].y, 1e-9);
    }

    ASSERT_HRESULT_FAIL(Control_EmulateTypeText(NULL, 1.0));
}

static void test_input_adversarial_trajectory_stress(void) {
    ASSERT_HRESULT_OK(Control_Init());

    BezierPoint pts[512];
    for (int i = 0; i < 100; i++) {
        int sx = (i * 97) % 1920;
        int sy = (i * 53) % 1080;
        int tx = (i == 50) ? sx : ((i * 193 + 400) % 1920);
        int ty = (i == 50) ? sy : ((i * 127 + 200) % 1080);

        TrajectoryConfig cfg;
        cfg.speedScale = 0.5 + (double)(i % 5) * 0.25;
        cfg.tremorScale = (i % 3 == 0) ? 0.0 : 1.5;
        cfg.curvatureScale = (i % 4 == 0) ? 0.0 : 1.2;
        cfg.ouTheta = 15.0;
        cfg.ouSigma = 2.5;
        cfg.minSteps = 5 + (i % 30);

        int count = 0;
        ASSERT_HRESULT_OK(Control_GenerateTrajectoryPoints(sx, sy, tx, ty, &cfg, pts, 512, &count));
        ASSERT_TRUE(count >= 1 && count <= 512);
        for (int k = 0; k < count; k++) {
            ASSERT_TRUE(isfinite(pts[k].x) && isfinite(pts[k].y));
        }
        ASSERT_DOUBLE_NEAR((double)tx, pts[count - 1].x, 0.5);
        ASSERT_DOUBLE_NEAR((double)ty, pts[count - 1].y, 0.5);
    }
}

static WindowInfo g_inEnumBuf[256];

static void test_input_window_enum_focus_show(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_input_test_window());

    uint32_t winCount = 0;
    memset(g_inEnumBuf, 0, sizeof(g_inEnumBuf));
    ASSERT_HRESULT_OK(Control_EnumWindows(g_inEnumBuf, 256U, &winCount));
    ASSERT_TRUE(winCount >= 1U);

    uint32_t checkCount = (winCount > 256U) ? 256U : winCount;
    BOOL foundTarget = FALSE;
    for (uint32_t i = 0; i < checkCount; i++) {
        if ((HWND)(uintptr_t)g_inEnumBuf[i].hwnd == g_inWin.hwnd) {
            foundTarget = TRUE;
            ASSERT_EQ_UINT((uint32_t)GetCurrentProcessId(), g_inEnumBuf[i].pid);
            ASSERT_EQ_WSTR(L"CPowerInputTestWindow", g_inEnumBuf[i].title);
            ASSERT_EQ_UINT(1U, g_inEnumBuf[i].isVisible);
            ASSERT_TRUE(g_inEnumBuf[i].rect.right > g_inEnumBuf[i].rect.left);
            ASSERT_TRUE(g_inEnumBuf[i].rect.bottom > g_inEnumBuf[i].rect.top);
            break;
        }
    }
    ASSERT_TRUE(foundTarget);

    /* Minimize -> verify iconic -> Restore -> Focus */
    ASSERT_HRESULT_OK(Control_ShowWindow(g_inWin.hwnd, WND_MINIMIZE));
    Sleep(30);
    ASSERT_TRUE(IsIconic(g_inWin.hwnd));

    ASSERT_HRESULT_OK(Control_ShowWindow(g_inWin.hwnd, WND_RESTORE));
    Sleep(30);
    ASSERT_FALSE(IsIconic(g_inWin.hwnd));

    ASSERT_HRESULT_OK(Control_ShowWindow(g_inWin.hwnd, WND_MAXIMIZE));
    Sleep(30);
    ASSERT_HRESULT_OK(Control_ShowWindow(g_inWin.hwnd, WND_RESTORE));

    HRESULT hrFocus = Control_FocusWindow(g_inWin.hwnd);
    ASSERT_TRUE(SUCCEEDED(hrFocus));

    /* Boundary checks */
    uint32_t countOnly = 0;
    ASSERT_HRESULT_OK(Control_EnumWindows(NULL, 0U, &countOnly));
    ASSERT_TRUE(countOnly >= 1U);
    ASSERT_HRESULT_FAIL(Control_EnumWindows(g_inEnumBuf, 4U, NULL));
    ASSERT_HRESULT_FAIL(Control_EnumWindows(NULL, 4U, &countOnly));
    ASSERT_HRESULT_FAIL(Control_FocusWindow(NULL));
    ASSERT_HRESULT_FAIL(Control_ShowWindow(NULL, WND_RESTORE));
    ASSERT_HRESULT_FAIL(Control_ShowWindow(g_inWin.hwnd, (WindowShowAction)99));
}

static void test_input_scroll_and_failsafe_gate(void) {
    ASSERT_HRESULT_OK(Control_Init());

    /* Failsafe disabled initially for deterministic test dispatch */
    Control_SetFailsafeEnabled(FALSE);
    ASSERT_FALSE(Control_CheckFailsafeTriggered());

    /* Zero-delta fast paths (cursor untouched via INT_MIN) */
    ASSERT_HRESULT_OK(Control_EmulateMouseScroll(INT_MIN, INT_MIN, 0, FALSE));
    ASSERT_HRESULT_OK(Control_EmulateMouseScroll(INT_MIN, INT_MIN, 0, TRUE));

    /* Failsafe toggle check unless cursor is pinned at virtual screen top-left */
    Control_SetFailsafeEnabled(TRUE);
    POINT cur;
    if (GetCursorPos(&cur)) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (cur.x > vx + 4 || cur.y > vy + 4) {
            ASSERT_FALSE(Control_CheckFailsafeTriggered());
        }
    }
    Control_SetFailsafeEnabled(FALSE);
    ASSERT_FALSE(Control_CheckFailsafeTriggered());

    /* Small vertical and horizontal wheel events without cursor warp */
    HRESULT hrV = Control_EmulateMouseScroll(INT_MIN, INT_MIN, 120, FALSE);
    if (hrV == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        TEST_SKIP("SendInput scroll denied on non-interactive/headless desktop (ERROR_ACCESS_DENIED)");
        return;
    }
    ASSERT_HRESULT_OK(hrV);
    ASSERT_HRESULT_OK(Control_EmulateMouseScroll(INT_MIN, INT_MIN, -120, TRUE));
}

static HRESULT execute_verify_set_text_cb(void* pUserData) {
    HWND hEdit = (HWND)pUserData;
    return Control_DirectSetTextHwnd(hEdit, L"VerifiedDelta_110");
}

static HRESULT execute_verify_noop_cb(void* pUserData) {
    (void)pUserData;
    return S_OK;
}

static void test_input_execute_and_verify_closed_loop(void) {
    ASSERT_HRESULT_OK(Perception_Init());
    g_inPercInit = TRUE;
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_input_test_window());

    /* Closed-loop action mutating EDIT value must return S_OK and non-empty diff */
    HRESULT hr = Control_ExecuteAndVerify(
        g_inWin.hwnd,
        execute_verify_set_text_cb,
        (void*)g_inWin.hEditSingle,
        1500U,
        &g_inDiff
    );
    ASSERT_EQ_HRESULT(S_OK, hr);
    ASSERT_NOT_NULL(g_inDiff);
    ASSERT_TRUE(g_inDiff->entryCount >= 1U);

    BOOL sawEditMod = FALSE;
    for (uint32_t i = 0; i < g_inDiff->entryCount; i++) {
        const TreeDiffEntry* ent = &g_inDiff->entries[i];
        if (wcscmp(ent->node.value, L"VerifiedDelta_110") == 0) {
            sawEditMod = TRUE;
            break;
        }
    }
    ASSERT_TRUE(sawEditMod);

    Perception_FreeTreeDiff(g_inDiff);
    g_inDiff = NULL;

    /* No-op callback should succeed with S_FALSE (zero delta detected) */
    HRESULT hrNoop = Control_ExecuteAndVerify(
        g_inWin.hwnd,
        execute_verify_noop_cb,
        NULL,
        150U,
        &g_inDiff
    );
    ASSERT_EQ_HRESULT(S_FALSE, hrNoop);
    if (g_inDiff) {
        ASSERT_EQ_UINT(0U, g_inDiff->entryCount);
        Perception_FreeTreeDiff(g_inDiff);
        g_inDiff = NULL;
    }

    /* Boundary invalid args */
    ASSERT_HRESULT_FAIL(Control_ExecuteAndVerify(g_inWin.hwnd, NULL, NULL, 100U, &g_inDiff));
    ASSERT_NULL(g_inDiff);
}

void RegisterInputTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "InputSuite",
        "Minimum-jerk, cubic Bezier, O-U tremor kinematics, Win32 messaging, and COM UIA patterns",
        input_suite_setup,
        input_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_Input_InitConfigureShutdown",
                       "Verify Control_Init, Control_Configure, and Control_Shutdown lifecycle",
                       7, TIER_1_FEATURE_COVERAGE, test_input_init_configure_shutdown);

    test_case_register(suite, "Test_Input_MinimumJerkMath",
                       "Verify 5th-order minimum-jerk endpoints, monotonicity, symmetry, and clamping",
                       8, TIER_1_FEATURE_COVERAGE, test_input_minimum_jerk_math);

    test_case_register(suite, "Test_Input_CubicBezierEval",
                       "Verify cubic Bezier endpoint preservation, arch midpoint, and collinearity",
                       8, TIER_1_FEATURE_COVERAGE, test_input_cubic_bezier_eval);

    test_case_register(suite, "Test_Input_TrajectoryGenerationAndTremor",
                       "Verify curvature, O-U neuromuscular tremor presence, and exact target landing",
                       8, TIER_1_FEATURE_COVERAGE, test_input_trajectory_generation_and_tremor);

    test_case_register(suite, "Test_Input_VirtualDesktopMapping",
                       "Verify SendInput virtual desktop [0..65535] coordinate normalization and clamping",
                       8, TIER_1_FEATURE_COVERAGE, test_input_virtual_desktop_mapping);

    test_case_register(suite, "Test_Input_DirectWin32Messaging",
                       "Verify BM_CLICK, WM_SETTEXT, and client-space clicks on real Win32 controls",
                       7, TIER_1_FEATURE_COVERAGE, test_input_direct_win32_messaging);

    test_case_register(suite, "Test_Input_DirectUiaComPatterns",
                       "Verify pure C COM Invoke, Value, Toggle, and SelectionItem patterns on live controls",
                       7, TIER_3_CROSS_FEATURE, test_input_direct_uia_com_patterns);

    test_case_register(suite, "Test_Input_SafeEmulationDispatch",
                       "Verify empty-text SendInput guard and minimal 1-px mouse move return S_OK",
                       8, TIER_1_FEATURE_COVERAGE, test_input_safe_emulation_dispatch);

    test_case_register(suite, "Test_Input_BoundaryAndInvalidArgs",
                       "Verify read-only ValuePattern rejection, bad HWNDs, NULL args, and canary bounds",
                       16, TIER_2_BOUNDARY_CORNER, test_input_boundary_and_invalid_args);

    test_case_register(suite, "Test_Input_AdversarialTrajectoryStress",
                       "Stress 100 randomized trajectories (incl. zero-distance) for NaN-freedom and landing",
                       16, TIER_5_ADVERSARIAL, test_input_adversarial_trajectory_stress);

    test_case_register(suite, "Test_Input_WindowEnumFocusShow",
                       "Verify top-level WindowInfo enumeration, show states (minimize/restore/max), and focus",
                       7, TIER_1_FEATURE_COVERAGE, test_input_window_enum_focus_show);

    test_case_register(suite, "Test_Input_ScrollAndFailsafeGate",
                       "Verify vertical/horizontal SendInput scroll and top-left/Pause failsafe gate",
                       8, TIER_1_FEATURE_COVERAGE, test_input_scroll_and_failsafe_gate);

    test_case_register(suite, "Test_Input_ExecuteAndVerifyClosedLoop",
                       "Verify Control_ExecuteAndVerify closed-loop semantic delta detection (S_OK vs S_FALSE)",
                       7, TIER_3_CROSS_FEATURE, test_input_execute_and_verify_closed_loop);
}
