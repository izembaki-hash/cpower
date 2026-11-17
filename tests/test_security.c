/**
 * @file test_security.c
 * @brief Tier 1, Tier 2, and Tier 3 tests for uiAccess privilege verification,
 *        token integrity level inspection, registry policy, and UIPI checks.
 */

#include "test_infra.h"
#include "security.h"
#include <wchar.h>

static HWND g_secTestHwnd = NULL;
static ATOM g_secTestAtom = 0;
static WCHAR g_secClassName[64] = {0};
static HANDLE g_secThread = NULL;
static HANDLE g_secReadyEvent = NULL;

static LRESULT CALLBACK sec_probe_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI sec_probe_thread(LPVOID lpParam) {
    (void)lpParam;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = sec_probe_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_secClassName;
    g_secTestAtom = RegisterClassExW(&wc);
    if (g_secTestAtom != 0) {
        g_secTestHwnd = CreateWindowExW(
            0, g_secClassName, L"CPowerSecProbeWindow",
            WS_OVERLAPPEDWINDOW, 10, 10, 200, 120,
            NULL, NULL, GetModuleHandleW(NULL), NULL
        );
    }
    SetEvent(g_secReadyEvent);
    if (!g_secTestHwnd) {
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static void security_suite_setup(void) {
    g_secTestHwnd = NULL;
    g_secTestAtom = 0;
    g_secThread = NULL;
    g_secReadyEvent = NULL;
}

static void security_suite_teardown(void) {
    if (g_secTestHwnd && IsWindow(g_secTestHwnd)) {
        PostMessageW(g_secTestHwnd, WM_CLOSE, 0, 0);
    }
    if (g_secThread) {
        if (WaitForSingleObject(g_secThread, 3000) == WAIT_TIMEOUT && g_secTestHwnd) {
            PostThreadMessageW(GetThreadId(g_secThread), WM_QUIT, 0, 0);
            WaitForSingleObject(g_secThread, 1000);
        }
        CloseHandle(g_secThread);
        g_secThread = NULL;
    }
    if (g_secReadyEvent) {
        CloseHandle(g_secReadyEvent);
        g_secReadyEvent = NULL;
    }
    g_secTestHwnd = NULL;
    if (g_secTestAtom != 0) {
        UnregisterClassW(g_secClassName, GetModuleHandleW(NULL));
        g_secTestAtom = 0;
    }
}

static BOOL query_expected_token_flags(BOOL* pOutUiAccess, BOOL* pOutElevated, DWORD* pOutRid) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return FALSE;
    }

    if (pOutUiAccess) {
        DWORD uiAccess = 0;
        DWORD retLen = 0;
        if (GetTokenInformation(hToken, TokenUIAccess, &uiAccess, sizeof(uiAccess), &retLen)) {
            *pOutUiAccess = (uiAccess != 0) ? TRUE : FALSE;
        } else {
            *pOutUiAccess = FALSE;
        }
    }

    if (pOutElevated) {
        TOKEN_ELEVATION elev;
        DWORD retLen = 0;
        memset(&elev, 0, sizeof(elev));
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &retLen)) {
            *pOutElevated = (elev.TokenIsElevated != 0) ? TRUE : FALSE;
        } else {
            *pOutElevated = FALSE;
        }
    }

    if (pOutRid) {
        union {
            TOKEN_MANDATORY_LABEL tml;
            uint8_t               raw[256];
        } buf;
        DWORD retLen = 0;
        memset(&buf, 0, sizeof(buf));
        if (GetTokenInformation(hToken, TokenIntegrityLevel, &buf, sizeof(buf), &retLen) &&
            buf.tml.Label.Sid != NULL &&
            IsValidSid(buf.tml.Label.Sid)) {
            PUCHAR pSubCount = GetSidSubAuthorityCount(buf.tml.Label.Sid);
            if (pSubCount != NULL && *pSubCount > 0U) {
                *pOutRid = *GetSidSubAuthority(buf.tml.Label.Sid, (DWORD)(*pSubCount - 1U));
            } else {
                *pOutRid = 0;
            }
        } else {
            *pOutRid = 0;
        }
    }

    CloseHandle(hToken);
    return TRUE;
}

static BOOL query_expected_secure_uia_paths(void) {
    HKEY hKey = NULL;
    LONG rc = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &hKey
    );
    if (rc != ERROR_SUCCESS) {
        return TRUE; /* Windows default when key is absent */
    }
    DWORD val = 1;
    DWORD type = 0;
    DWORD cb = sizeof(val);
    rc = RegQueryValueExW(hKey, L"EnableSecureUIAPaths", NULL, &type, (LPBYTE)&val, &cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return TRUE;
    }
    return (val != 0) ? TRUE : FALSE;
}

static BOOL starts_with_dir(const WCHAR* exePath, const WCHAR* dirPrefix) {
    if (!exePath || !dirPrefix || dirPrefix[0] == L'\0') return FALSE;
    size_t prefixLen = wcslen(dirPrefix);
    while (prefixLen > 0 && dirPrefix[prefixLen - 1] == L'\\') {
        prefixLen--;
    }
    if (prefixLen == 0) return FALSE;
    if (_wcsnicmp(exePath, dirPrefix, prefixLen) != 0) return FALSE;
    return (exePath[prefixLen] == L'\\') ? TRUE : FALSE;
}

static BOOL query_expected_in_secure_dir(const WCHAR* exePath) {
    WCHAR dirBuf[MAX_PATH];
    BOOL inSecure = FALSE;

    HKEY hKey = NULL;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &hKey) == ERROR_SUCCESS) {
        static const WCHAR* const kVals[] = {
            L"ProgramFilesDir",
            L"ProgramFilesDir (x86)",
            L"ProgramW6432Dir"
        };
        for (size_t i = 0; i < sizeof(kVals) / sizeof(kVals[0]); i++) {
            DWORD type = 0;
            DWORD cb = (DWORD)((MAX_PATH - 1) * sizeof(WCHAR));
            if (RegQueryValueExW(hKey, kVals[i], NULL, &type, (LPBYTE)dirBuf, &cb) == ERROR_SUCCESS &&
                type == REG_SZ && cb >= sizeof(WCHAR)) {
                DWORD chars = cb / (DWORD)sizeof(WCHAR);
                if (chars >= MAX_PATH) chars = MAX_PATH - 1;
                dirBuf[chars] = L'\0';
                if (starts_with_dir(exePath, dirBuf)) {
                    inSecure = TRUE;
                    break;
                }
            }
        }
        RegCloseKey(hKey);
    }

    if (inSecure) return TRUE;

    UINT sysLen = GetSystemDirectoryW(dirBuf, MAX_PATH);
    if (sysLen > 0 && sysLen < MAX_PATH && starts_with_dir(exePath, dirBuf)) {
        return TRUE;
    }
    return FALSE;
}

static void test_security_uiaccess_and_elevation(void) {
    BOOL expectedUiAccess = FALSE;
    BOOL expectedElevated = FALSE;
    ASSERT_TRUE(query_expected_token_flags(&expectedUiAccess, &expectedElevated, NULL));

    BOOL actualUiAccess = Security_HasUiAccess();
    BOOL actualElevated = Security_IsElevated();

    ASSERT_TRUE(actualUiAccess == TRUE || actualUiAccess == FALSE);
    ASSERT_TRUE(actualElevated == TRUE || actualElevated == FALSE);
    ASSERT_EQ_INT(expectedUiAccess, actualUiAccess);
    ASSERT_EQ_INT(expectedElevated, actualElevated);
}

static void test_security_integrity_level_and_strings(void) {
    DWORD expectedRid = 0;
    ASSERT_TRUE(query_expected_token_flags(NULL, NULL, &expectedRid));

    SecurityIntegrityLevel level = INTEGRITY_UNKNOWN;
    DWORD rid = 0;
    ASSERT_HRESULT_OK(Security_GetIntegrityLevel(&level, &rid));
    ASSERT_NE_INT(INTEGRITY_UNKNOWN, (int)level);
    ASSERT_TRUE(level >= INTEGRITY_UNTRUSTED && level <= INTEGRITY_SYSTEM);
    ASSERT_EQ_UINT(expectedRid, rid);

    if (rid < SECURITY_MANDATORY_LOW_RID) {
        ASSERT_EQ_INT(INTEGRITY_UNTRUSTED, (int)level);
    } else if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
        ASSERT_EQ_INT(INTEGRITY_LOW, (int)level);
    } else if (rid < SECURITY_MANDATORY_MEDIUM_PLUS_RID) {
        ASSERT_EQ_INT(INTEGRITY_MEDIUM, (int)level);
    } else if (rid < SECURITY_MANDATORY_HIGH_RID) {
        ASSERT_EQ_INT(INTEGRITY_MEDIUM_PLUS, (int)level);
    } else if (rid < SECURITY_MANDATORY_SYSTEM_RID) {
        ASSERT_EQ_INT(INTEGRITY_HIGH, (int)level);
    } else {
        ASSERT_EQ_INT(INTEGRITY_SYSTEM, (int)level);
    }

    const SecurityIntegrityLevel allLevels[] = {
        INTEGRITY_UNKNOWN,
        INTEGRITY_UNTRUSTED,
        INTEGRITY_LOW,
        INTEGRITY_MEDIUM,
        INTEGRITY_MEDIUM_PLUS,
        INTEGRITY_HIGH,
        INTEGRITY_SYSTEM
    };
    for (size_t i = 0; i < sizeof(allLevels) / sizeof(allLevels[0]); i++) {
        const char* s = Security_IntegrityLevelToString(allLevels[i]);
        ASSERT_NOT_NULL(s);
        ASSERT_TRUE(strlen(s) > 0);
        if (allLevels[i] != INTEGRITY_UNKNOWN) {
            ASSERT_TRUE(strcmp(s, Security_IntegrityLevelToString(INTEGRITY_UNKNOWN)) != 0);
        }
    }
}

static void test_security_uia_paths_and_exe_location(void) {
    BOOL expectedSecurePaths = query_expected_secure_uia_paths();
    BOOL actualSecurePaths = Security_IsSecureUiaPathsEnabled();
    ASSERT_EQ_INT(expectedSecurePaths, actualSecurePaths);

    WCHAR exePath[MAX_PATH];
    DWORD pathLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    ASSERT_TRUE(pathLen > 0 && pathLen < MAX_PATH);

    BOOL expectedInSecureDir = query_expected_in_secure_dir(exePath);
    BOOL actualInSecureDir = Security_IsExecutableInSecurePath();
    ASSERT_EQ_INT(expectedInSecureDir, actualInSecureDir);

    /* Negative check: spoofing ProgramFiles env var must NOT fool Security_IsExecutableInSecurePath */
    if (!expectedInSecureDir) {
        WCHAR exeDir[MAX_PATH];
        wcscpy(exeDir, exePath);
        WCHAR* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            WCHAR prevEnv[MAX_PATH];
            DWORD prevLen = GetEnvironmentVariableW(L"ProgramFiles", prevEnv, MAX_PATH);
            SetEnvironmentVariableW(L"ProgramFiles", exeDir);
            ASSERT_FALSE(Security_IsExecutableInSecurePath());
            SetEnvironmentVariableW(L"ProgramFiles", (prevLen > 0 && prevLen < MAX_PATH) ? prevEnv : NULL);
        }
    }
}

static void test_security_environment_report_and_verify(void) {
    SecurityEnvironmentReport report;
    memset(&report, 0, sizeof(report));

    ASSERT_HRESULT_OK(Security_GetEnvironmentReport(&report));
    ASSERT_EQ_INT(Security_HasUiAccess(), report.hasUiAccess);
    ASSERT_EQ_INT(Security_IsElevated(), report.isElevated);
    ASSERT_EQ_INT(Security_IsSecureUiaPathsEnabled(), report.secureUiaPathsEnabled);
    ASSERT_EQ_INT(Security_IsExecutableInSecurePath(), report.inSecureDirectory);

    SecurityIntegrityLevel level = INTEGRITY_UNKNOWN;
    DWORD rid = 0;
    ASSERT_HRESULT_OK(Security_GetIntegrityLevel(&level, &rid));
    ASSERT_EQ_INT((int)level, (int)report.integrityLevel);
    ASSERT_EQ_UINT(rid, report.integrityRid);

    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    ASSERT_TRUE(len > 0 && len < MAX_PATH);
    ASSERT_TRUE(wcslen(report.executablePath) > 0);
    ASSERT_EQ_INT(0, _wcsicmp(exePath, report.executablePath));

    HRESULT hrVerify = Security_VerifyEnvironment();
    if (report.hasUiAccess || report.isElevated) {
        ASSERT_HRESULT_OK(hrVerify);
    } else {
        /* Without uiAccess or Admin elevation, VerifyEnvironment must return S_FALSE
           even if EnableSecureUIAPaths is 0 */
        ASSERT_EQ_INT(S_FALSE, hrVerify);
    }
}

static void test_security_can_interact_with_own_window(void) {
    swprintf(g_secClassName, 64, L"CPowerSecProbe_%lu", GetCurrentProcessId());
    g_secReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT_NOT_NULL(g_secReadyEvent);

    g_secThread = CreateThread(NULL, 0, sec_probe_thread, NULL, 0, NULL);
    ASSERT_NOT_NULL(g_secThread);
    ASSERT_EQ_INT(WAIT_OBJECT_0, (int)WaitForSingleObject(g_secReadyEvent, 5000));
    ASSERT_TRUE(g_secTestAtom != 0);
    ASSERT_NOT_NULL(g_secTestHwnd);
    ASSERT_TRUE(IsWindow(g_secTestHwnd));

    /* Same-process window always has equal integrity level */
    ASSERT_TRUE(Security_CanInteractWithWindow(g_secTestHwnd));
}

static void test_security_boundary_null_and_invalid(void) {
    SecurityIntegrityLevel level = INTEGRITY_UNKNOWN;
    DWORD rid = 0;

    ASSERT_HRESULT_OK(Security_GetIntegrityLevel(NULL, &rid));
    ASSERT_TRUE(rid > 0);

    ASSERT_HRESULT_OK(Security_GetIntegrityLevel(&level, NULL));
    ASSERT_NE_INT(INTEGRITY_UNKNOWN, (int)level);

    ASSERT_HRESULT_OK(Security_GetIntegrityLevel(NULL, NULL));

    ASSERT_HRESULT_FAIL(Security_GetEnvironmentReport(NULL));

    const char* bad1 = Security_IntegrityLevelToString((SecurityIntegrityLevel)999);
    const char* bad2 = Security_IntegrityLevelToString((SecurityIntegrityLevel)-42);
    ASSERT_NOT_NULL(bad1);
    ASSERT_NOT_NULL(bad2);
    ASSERT_TRUE(strlen(bad1) > 0);
    ASSERT_TRUE(strlen(bad2) > 0);

    ASSERT_FALSE(Security_CanInteractWithWindow(NULL));
    ASSERT_FALSE(Security_CanInteractWithWindow((HWND)(uintptr_t)0xDEADBEEF));

    /* Negative check: non-System callers must not interact with System-owned desktop window */
    if (rid < SECURITY_MANDATORY_SYSTEM_RID) {
        HWND hDesktop = GetDesktopWindow();
        if (hDesktop != NULL) {
            ASSERT_FALSE(Security_CanInteractWithWindow(hDesktop));
        }
    }
}

void RegisterSecurityTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "SecuritySuite",
        "Token uiAccess, elevation, Mandatory Integrity Levels, registry policy, and UIPI checks",
        security_suite_setup,
        security_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_Security_UiAccessAndElevation",
                       "Cross-check Security_HasUiAccess and Security_IsElevated against process token",
                       11, TIER_1_FEATURE_COVERAGE, test_security_uiaccess_and_elevation);

    test_case_register(suite, "Test_Security_IntegrityLevelAndStrings",
                       "Verify Security_GetIntegrityLevel RID mapping and IntegrityLevelToString coverage",
                       11, TIER_1_FEATURE_COVERAGE, test_security_integrity_level_and_strings);

    test_case_register(suite, "Test_Security_UiaPathsAndExeLocation",
                       "Verify EnableSecureUIAPaths registry query and secure directory path check",
                       12, TIER_1_FEATURE_COVERAGE, test_security_uia_paths_and_exe_location);

    test_case_register(suite, "Test_Security_EnvironmentReportAndVerify",
                       "Verify Security_GetEnvironmentReport consistency and Security_VerifyEnvironment",
                       11, TIER_1_FEATURE_COVERAGE, test_security_environment_report_and_verify);

    test_case_register(suite, "Test_Security_CanInteractWithOwnWindow",
                       "Verify Security_CanInteractWithWindow succeeds for same-integrity window",
                       11, TIER_3_CROSS_FEATURE, test_security_can_interact_with_own_window);

    test_case_register(suite, "Test_Security_BoundaryNullAndInvalid",
                       "Verify NULL pointers, optional outputs, out-of-range enums, and invalid HWNDs",
                       16, TIER_2_BOUNDARY_CORNER, test_security_boundary_null_and_invalid);
}
