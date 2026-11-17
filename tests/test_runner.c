/**
 * @file test_runner.c
 * @brief Standalone Test Runner CLI for pure C Windows-native Computer-Use Agent
 *
 * Provides a unified command-line entry point to execute:
 *   - The entire test suite or specific suites
 *   - Individual test tiers (Tier 1 to Tier 5)
 *   - Specific feature test suites (Feature 01 to Feature 16)
 *   - Substring filter matches on suite or test names
 *   - Structured JSON report export for CI and auditing
 *   - Interactive list of registered suites and test cases
 */

#include "test_infra.h"

/* Singleton Test Context Instance */
TestContext g_test_ctx;

/* Forward declarations for suite registrars */
void RegisterInfraSelfTestSuite(void);

#if defined(ENABLE_PERCEPTION_TESTS)
void RegisterPerceptionTestSuite(void);
#endif

#if defined(ENABLE_INPUT_TESTS)
void RegisterInputTestSuite(void);
#endif

#if defined(ENABLE_IPC_TESTS)
void RegisterIpcTestSuite(void);
#endif

#if defined(ENABLE_SECURITY_TESTS)
void RegisterSecurityTestSuite(void);
#endif

#if defined(ENABLE_ARCH_TESTS)
void RegisterArchTestSuite(void);
#endif

#if defined(ENABLE_E2E_TESTS)
void RegisterE2ETestSuite(void);
#endif

/* ========================================================================= */
/* Self-Test Suite: Validates Test Infrastructure & Assertion Engine         */
/* ========================================================================= */

static void test_infra_qpc_timer_monotonic(void) {
    int64_t t1 = test_infra_timer_now();
    Sleep(5); /* Sleep 5ms */
    int64_t t2 = test_infra_timer_now();
    ASSERT_TRUE(t2 > t1);
    double elapsed = test_infra_timer_ms(t1, t2);
    /* Expect elapsed to be at least 3ms and less than 100ms */
    ASSERT_TRUE(elapsed >= 3.0);
    ASSERT_TRUE(elapsed < 100.0);
}

static void test_infra_integer_assertions(void) {
    int a = 42;
    int b = 42;
    int c = 99;
    ASSERT_EQ_INT(a, b);
    ASSERT_NE_INT(a, c);
    ASSERT_EQ_UINT(100ULL, 100ULL);
}

static void test_infra_pointer_assertions(void) {
    int val = 123;
    int* pVal = &val;
    int* pNull = NULL;
    ASSERT_NOT_NULL(pVal);
    ASSERT_NULL(pNull);
    ASSERT_EQ_PTR(pVal, &val);
    ASSERT_NE_PTR(pVal, pNull);
}

static void test_infra_string_assertions(void) {
    const char* s1 = "Windows Computer-Use Agent";
    const char* s2 = "Windows Computer-Use Agent";
    ASSERT_EQ_STR(s1, s2);
    ASSERT_STR_CONTAINS(s1, "Computer-Use");
    ASSERT_STR_CONTAINS(s1, "Agent");
}

static void test_infra_wide_string_assertions(void) {
    const wchar_t* w1 = L"IUIAutomation Pure C";
    const wchar_t* w2 = L"IUIAutomation Pure C";
    ASSERT_EQ_WSTR(w1, w2);
}

static void test_infra_float_assertions(void) {
    double x = 3.14159265;
    double y = 3.14159200;
    ASSERT_DOUBLE_NEAR(x, y, 0.0001);
}

static void test_infra_hresult_assertions(void) {
    HRESULT hr_ok = S_OK;
    HRESULT hr_fail = E_FAIL;
    HRESULT hr_inv = E_INVALIDARG;
    ASSERT_HRESULT_OK(hr_ok);
    ASSERT_HRESULT_FAIL(hr_fail);
    ASSERT_EQ_HRESULT(E_INVALIDARG, hr_inv);
}

static void test_infra_skip_behavior(void) {
    /* Test skip functionality */
    TEST_SKIP("Verifying test skip mechanism");
    /* Code after TEST_SKIP must not execute */
    ASSERT_FAIL("Unreachable code reached after skip");
}

void RegisterInfraSelfTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "TestInfraSelfTest",
        "Self-verification of pure C test infrastructure and assertion macros",
        NULL, NULL
    );
    if (!suite) return;

    test_case_register(suite, "Test_QPC_TimerMonotonic",
                       "Verify QueryPerformanceCounter monotonicity and accuracy",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_qpc_timer_monotonic);

    test_case_register(suite, "Test_IntegerAssertions",
                       "Verify integer and unsigned equality and inequality assertions",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_integer_assertions);

    test_case_register(suite, "Test_PointerAssertions",
                       "Verify pointer nullity and pointer address comparison assertions",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_pointer_assertions);

    test_case_register(suite, "Test_StringAssertions",
                       "Verify ANSI string equality and substring search assertions",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_string_assertions);

    test_case_register(suite, "Test_WideStringAssertions",
                       "Verify Unicode wide string equality assertions",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_wide_string_assertions);

    test_case_register(suite, "Test_FloatNearAssertions",
                       "Verify floating point proximity comparison with epsilon tolerance",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_float_assertions);

    test_case_register(suite, "Test_HResultAssertions",
                       "Verify COM HRESULT success, failure, and equality assertions",
                       15, TIER_1_FEATURE_COVERAGE, test_infra_hresult_assertions);

    test_case_register(suite, "Test_SkipMechanism",
                       "Verify TEST_SKIP aborts test without reporting failure",
                       15, TIER_2_BOUNDARY_CORNER, test_infra_skip_behavior);
}

/* ========================================================================= */
/* CLI Printing & Listing Utilities                                          */
/* ========================================================================= */

static void PrintUsage(const char* exe_name) {
    printf("Usage: %s [options]\n\n", exe_name);
    printf("Options:\n");
    printf("  --all                  Execute all registered test suites (default)\n");
    printf("  --tier <1-5>           Execute tests matching specific tier:\n");
    printf("                           1 = Tier 1: Feature Coverage\n");
    printf("                           2 = Tier 2: Boundary & Corner Cases\n");
    printf("                           3 = Tier 3: Cross-Feature Combinations\n");
    printf("                           4 = Tier 4: Real-World Scenarios\n");
    printf("                           5 = Tier 5: Adversarial Hardening\n");
    printf("  --feature <1-16>       Execute tests matching specific feature number\n");
    printf("  --suite <name>         Execute tests in suites matching name substring\n");
    printf("  --filter <name>        Execute tests matching test name substring\n");
    printf("  --json <path>          Export execution results to structured JSON file\n");
    printf("  --verbose, -v          Display detailed assertion logs\n");
    printf("  --list                 List all registered suites and test cases without running\n");
    printf("  --help, -h             Display this help message\n\n");
    printf("Examples:\n");
    printf("  %s --all\n", exe_name);
    printf("  %s --tier 1\n", exe_name);
    printf("  %s --feature 3\n", exe_name);
    printf("  %s --suite Perception --json results.json\n", exe_name);
}

static void ListTests(void) {
    printf("\n%s================================================================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s   REGISTERED TEST SUITES & TEST CASES%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s================================================================================%s\n\n", COLOR_CYAN, COLOR_RESET);

    int total_cases = 0;
    for (int s = 0; s < g_test_ctx.suite_count; s++) {
        TestSuite* suite = g_test_ctx.suites[s];
        printf("%sSuite [%d]: %-32s (%s)%s\n", COLOR_BOLD, s + 1, suite->name,
               suite->description[0] != '\0' ? suite->description : "", COLOR_RESET);
        for (int t = 0; t < suite->test_count; t++) {
            TestCase* tc = &suite->tests[t];
            printf("  [%s][F%02d] %-44s - %s\n",
                   test_infra_tier_short(tc->tier),
                   tc->feature_id,
                   tc->name,
                   tc->description[0] != '\0' ? tc->description : "");
            total_cases++;
        }
        printf("\n");
    }
    printf("Total Suites: %d | Total Test Cases: %d\n\n", g_test_ctx.suite_count, total_cases);
}

static LRESULT CALLBACK hook_target_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1U, 40U, NULL);
            SetTimer(hwnd, 2U, 15000U, NULL);
            return 0;
        case WM_TIMER:
            if (wParam == 2U) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else if (wParam == 1U) {
                HDC hdc = GetDC(hwnd);
                if (hdc) {
                    static const WCHAR kExtTxt[] = L"CPowerHookCapture_42";
                    static const WCHAR kDtTxt[] = L"CPowerDrawText_99";
                    RECT rc = { 16, 48, 280, 96 };
                    ExtTextOutW(hdc, 16, 16, 0U, NULL, kExtTxt, (UINT)(sizeof(kExtTxt) / sizeof(WCHAR) - 1U), NULL);
                    DrawTextW(hdc, kDtTxt, (int)(sizeof(kDtTxt) / sizeof(WCHAR) - 1U), &rc, DT_LEFT | DT_SINGLELINE);
                    ReleaseDC(hwnd, hdc);
                }
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (hdc) {
                static const WCHAR kExtTxt[] = L"CPowerHookCapture_42";
                ExtTextOutW(hdc, 16, 16, 0U, NULL, kExtTxt, (UINT)(sizeof(kExtTxt) / sizeof(WCHAR) - 1U), NULL);
                EndPaint(hwnd, &ps);
            }
            return 0;
        }
        case WM_CLOSE:
            KillTimer(hwnd, 1U);
            KillTimer(hwnd, 2U);
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

static int run_hook_target_helper_process(const char* eventNameAnsi) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = hook_target_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CPowerHookTargetWinCls";

    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0) {
        return 2;
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"CPowerHookTargetProcess",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        140, 140, 320, 180,
        NULL, NULL, wc.hInstance, NULL
    );
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 3;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    if (eventNameAnsi && eventNameAnsi[0] != '\0') {
        HANDLE hEvt = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventNameAnsi);
        if (hEvt) {
            SetEvent(hEvt);
            CloseHandle(hEvt);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

/* ========================================================================= */
/* Main Entry Point                                                          */
/* ========================================================================= */

int main(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--hook-target-mode") == 0) {
        const char* evtName = (argc >= 3) ? argv[2] : NULL;
        return run_hook_target_helper_process(evtName);
    }

    test_infra_init();

    /* Register test suites */
    RegisterInfraSelfTestSuite();

#if defined(ENABLE_PERCEPTION_TESTS)
    RegisterPerceptionTestSuite();
#endif

#if defined(ENABLE_INPUT_TESTS)
    RegisterInputTestSuite();
#endif

#if defined(ENABLE_IPC_TESTS)
    RegisterIpcTestSuite();
#endif

#if defined(ENABLE_SECURITY_TESTS)
    RegisterSecurityTestSuite();
#endif

#if defined(ENABLE_ARCH_TESTS)
    RegisterArchTestSuite();
#endif

#if defined(ENABLE_E2E_TESTS)
    RegisterE2ETestSuite();
#endif

    TestRunnerConfig config;
    memset(&config, 0, sizeof(config));
    config.color_output = true;

    bool do_list = false;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            /* Default */
        } else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc) {
            config.filter_tier = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--feature") == 0 && i + 1 < argc) {
            config.filter_feature = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            strncpy(config.filter_suite, argv[++i], sizeof(config.filter_suite) - 1);
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            strncpy(config.filter_name, argv[++i], sizeof(config.filter_name) - 1);
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            strncpy(config.json_output_path, argv[++i], sizeof(config.json_output_path) - 1);
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            config.verbose = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            do_list = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (do_list) {
        ListTests();
        return 0;
    }

    return test_infra_run_all(&config);
}
