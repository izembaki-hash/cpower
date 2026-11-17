/**
 * @file privilege.c
 * @brief Privilege Inspection, uiAccess Verification, and UIPI Boundary Checks.
 *
 * Inspects process access tokens (TokenUIAccess, TokenElevation,
 * TokenIntegrityLevel), Windows policy registry flags (EnableSecureUIAPaths),
 * secure installation paths, and cross-integrity window interaction checks.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "security.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SECURITY_MANDATORY_UNTRUSTED_RID
#define SECURITY_MANDATORY_UNTRUSTED_RID   (0x00000000L)
#endif
#ifndef SECURITY_MANDATORY_LOW_RID
#define SECURITY_MANDATORY_LOW_RID         (0x00001000L)
#endif
#ifndef SECURITY_MANDATORY_MEDIUM_RID
#define SECURITY_MANDATORY_MEDIUM_RID      (0x00002000L)
#endif
#ifndef SECURITY_MANDATORY_MEDIUM_PLUS_RID
#define SECURITY_MANDATORY_MEDIUM_PLUS_RID (SECURITY_MANDATORY_MEDIUM_RID + 0x100)
#endif
#ifndef SECURITY_MANDATORY_HIGH_RID
#define SECURITY_MANDATORY_HIGH_RID        (0x00003000L)
#endif
#ifndef SECURITY_MANDATORY_SYSTEM_RID
#define SECURITY_MANDATORY_SYSTEM_RID      (0x00004000L)
#endif

static SecurityIntegrityLevel sec_map_rid_to_level(DWORD rid) {
    if (rid < (DWORD)SECURITY_MANDATORY_LOW_RID) {
        return INTEGRITY_UNTRUSTED;
    }
    if (rid < (DWORD)SECURITY_MANDATORY_MEDIUM_RID) {
        return INTEGRITY_LOW;
    }
    if (rid < (DWORD)SECURITY_MANDATORY_MEDIUM_PLUS_RID) {
        return INTEGRITY_MEDIUM;
    }
    if (rid < (DWORD)SECURITY_MANDATORY_HIGH_RID) {
        return INTEGRITY_MEDIUM_PLUS;
    }
    if (rid < (DWORD)SECURITY_MANDATORY_SYSTEM_RID) {
        return INTEGRITY_HIGH;
    }
    return INTEGRITY_SYSTEM;
}

static HRESULT sec_get_token_integrity_rid(HANDLE hToken, SecurityIntegrityLevel* pLevelOut, DWORD* pRidOut) {
    if (!hToken) {
        return E_INVALIDARG;
    }

    DWORD needed = 0U;
    if (!GetTokenInformation(hToken, TokenIntegrityLevel, NULL, 0U, &needed)) {
        DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
            return HRESULT_FROM_WIN32(err);
        }
    }
    if (needed == 0U) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    uint8_t stackBuf[256];
    TOKEN_MANDATORY_LABEL* pLabel = (needed <= sizeof(stackBuf))
        ? (TOKEN_MANDATORY_LABEL*)stackBuf
        : (TOKEN_MANDATORY_LABEL*)malloc(needed);

    if (!pLabel) {
        return E_OUTOFMEMORY;
    }

    HRESULT hr = S_OK;
    if (!GetTokenInformation(hToken, TokenIntegrityLevel, pLabel, needed, &needed)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else if (!pLabel->Label.Sid || !IsValidSid(pLabel->Label.Sid)) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_SID);
    } else {
        PUCHAR pSubCount = GetSidSubAuthorityCount(pLabel->Label.Sid);
        if (!pSubCount || *pSubCount == 0U) {
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_SID);
        } else {
            DWORD rid = *GetSidSubAuthority(pLabel->Label.Sid, (DWORD)(*pSubCount - 1U));
            if (pRidOut) {
                *pRidOut = rid;
            }
            if (pLevelOut) {
                *pLevelOut = sec_map_rid_to_level(rid);
            }
        }
    }

    if ((uint8_t*)pLabel != stackBuf) {
        free(pLabel);
    }
    return hr;
}

static BOOL sec_path_starts_with_dir(const WCHAR* exePath, const WCHAR* dirPrefix) {
    if (!exePath || !dirPrefix || dirPrefix[0] == L'\0') {
        return FALSE;
    }
    size_t dirLen = wcslen(dirPrefix);
    while (dirLen > 0U && dirPrefix[dirLen - 1U] == L'\\') {
        dirLen--;
    }
    if (dirLen == 0U) {
        return FALSE;
    }
    if (_wcsnicmp(exePath, dirPrefix, dirLen) != 0) {
        return FALSE;
    }
    return (exePath[dirLen] == L'\\') ? TRUE : FALSE;
}

BOOL Security_HasUiAccess(void) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return FALSE;
    }

    DWORD uiAccess = 0U;
    DWORD returned = 0U;
    BOOL ok = GetTokenInformation(hToken, TokenUIAccess, &uiAccess, sizeof(uiAccess), &returned);
    CloseHandle(hToken);

    return (ok && uiAccess != 0U) ? TRUE : FALSE;
}

BOOL Security_IsElevated(void) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return FALSE;
    }

    TOKEN_ELEVATION elevation;
    memset(&elevation, 0, sizeof(elevation));
    DWORD returned = 0U;
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(hToken);

    return (ok && elevation.TokenIsElevated != 0U) ? TRUE : FALSE;
}

HRESULT Security_GetIntegrityLevel(SecurityIntegrityLevel* pLevelOut, DWORD* pRidOut) {
    if (pLevelOut) {
        *pLevelOut = INTEGRITY_UNKNOWN;
    }
    if (pRidOut) {
        *pRidOut = 0U;
    }

    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT hr = sec_get_token_integrity_rid(hToken, pLevelOut, pRidOut);
    CloseHandle(hToken);
    return hr;
}

const char* Security_IntegrityLevelToString(SecurityIntegrityLevel level) {
    switch (level) {
        case INTEGRITY_UNTRUSTED:   return "Untrusted";
        case INTEGRITY_LOW:         return "Low";
        case INTEGRITY_MEDIUM:      return "Medium";
        case INTEGRITY_MEDIUM_PLUS: return "Medium Plus";
        case INTEGRITY_HIGH:        return "High";
        case INTEGRITY_SYSTEM:      return "System";
        case INTEGRITY_UNKNOWN:
        default:                    return "Unknown";
    }
}

BOOL Security_IsSecureUiaPathsEnabled(void) {
    HKEY hKey = NULL;
    LONG rc = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &hKey
    );
    if (rc != ERROR_SUCCESS) {
        return TRUE;
    }

    DWORD val = 1U;
    DWORD type = 0U;
    DWORD size = sizeof(val);
    rc = RegQueryValueExW(hKey, L"EnableSecureUIAPaths", NULL, &type, (LPBYTE)&val, &size);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return TRUE;
    }
    return (val != 0U) ? TRUE : FALSE;
}

static BOOL sec_query_hklm_dir(HKEY hKey, const WCHAR* valueName, WCHAR* outBuf, DWORD outChars) {
    if (!hKey || !valueName || !outBuf || outChars == 0U) {
        return FALSE;
    }
    DWORD type = 0U;
    DWORD cb = (outChars - 1U) * (DWORD)sizeof(WCHAR);
    LONG rc = RegQueryValueExW(hKey, valueName, NULL, &type, (LPBYTE)outBuf, &cb);
    if (rc != ERROR_SUCCESS || type != REG_SZ || cb < sizeof(WCHAR)) {
        return FALSE;
    }
    DWORD chars = cb / (DWORD)sizeof(WCHAR);
    if (chars >= outChars) {
        chars = outChars - 1U;
    }
    outBuf[chars] = L'\0';
    return (outBuf[0] != L'\0') ? TRUE : FALSE;
}

BOOL Security_IsExecutableInSecurePath(void) {
    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0U || len >= MAX_PATH) {
        return FALSE;
    }
    exePath[len] = L'\0';

    WCHAR dirBuf[MAX_PATH];
    BOOL matched = FALSE;

    /* Query authoritative HKLM installation directories (unspoofable by env vars) */
    HKEY hKey = NULL;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &hKey) == ERROR_SUCCESS) {
        static const WCHAR* const kRegValues[] = {
            L"ProgramFilesDir",
            L"ProgramFilesDir (x86)",
            L"ProgramW6432Dir"
        };
        for (size_t i = 0; i < sizeof(kRegValues) / sizeof(kRegValues[0]); ++i) {
            if (sec_query_hklm_dir(hKey, kRegValues[i], dirBuf, MAX_PATH) &&
                sec_path_starts_with_dir(exePath, dirBuf)) {
                matched = TRUE;
                break;
            }
        }
        RegCloseKey(hKey);
    }

    if (matched) {
        return TRUE;
    }

    UINT sysLen = GetSystemDirectoryW(dirBuf, MAX_PATH);
    if (sysLen > 0U && sysLen < MAX_PATH && sec_path_starts_with_dir(exePath, dirBuf)) {
        return TRUE;
    }

    return FALSE;
}

HRESULT Security_GetEnvironmentReport(SecurityEnvironmentReport* pReportOut) {
    if (!pReportOut) {
        return E_INVALIDARG;
    }
    memset(pReportOut, 0, sizeof(*pReportOut));

    DWORD pathLen = GetModuleFileNameW(NULL, pReportOut->executablePath, MAX_PATH);
    if (pathLen == 0U) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (pathLen >= MAX_PATH) {
        pReportOut->executablePath[MAX_PATH - 1U] = L'\0';
    }

    pReportOut->hasUiAccess = Security_HasUiAccess();
    pReportOut->isElevated = Security_IsElevated();
    pReportOut->secureUiaPathsEnabled = Security_IsSecureUiaPathsEnabled();
    pReportOut->inSecureDirectory = Security_IsExecutableInSecurePath();

    HRESULT hr = Security_GetIntegrityLevel(&pReportOut->integrityLevel, &pReportOut->integrityRid);
    if (FAILED(hr)) {
        return hr;
    }
    return S_OK;
}

static BOOL sec_has_active_uipi_capability(const SecurityEnvironmentReport* pReport) {
    if (!pReport) {
        return FALSE;
    }
    /* Runtime S_OK requires actual token UIPI bypass (uiAccess) or Admin elevation.
       Secure path / EnableSecureUIAPaths only govern launch eligibility, not runtime capability. */
    return (pReport->hasUiAccess || pReport->isElevated) ? TRUE : FALSE;
}

HRESULT Security_VerifyEnvironment(void) {
    SecurityEnvironmentReport report;
    HRESULT hr = Security_GetEnvironmentReport(&report);
    if (FAILED(hr)) {
        return hr;
    }

    if (sec_has_active_uipi_capability(&report)) {
        return S_OK;
    }
    return S_FALSE;
}

BOOL Security_CanInteractWithWindow(HWND targetHwnd) {
    if (targetHwnd == NULL || !IsWindow(targetHwnd)) {
        return FALSE;
    }

    DWORD targetPid = 0U;
    GetWindowThreadProcessId(targetHwnd, &targetPid);
    if (targetPid == 0U) {
        return FALSE;
    }
    if (targetPid == GetCurrentProcessId()) {
        return TRUE;
    }

    DWORD myRid = 0U;
    if (FAILED(Security_GetIntegrityLevel(NULL, &myRid))) {
        return FALSE;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (hProc == NULL) {
        return FALSE;
    }

    HANDLE hTargetToken = NULL;
    if (!OpenProcessToken(hProc, TOKEN_QUERY, &hTargetToken)) {
        CloseHandle(hProc);
        return FALSE;
    }

    DWORD targetRid = 0U;
    HRESULT hr = sec_get_token_integrity_rid(hTargetToken, NULL, &targetRid);
    CloseHandle(hTargetToken);
    CloseHandle(hProc);

    if (FAILED(hr)) {
        return FALSE;
    }

    /* Even uiAccess / elevated Admin callers cannot cross into System integrity */
    if (targetRid >= (DWORD)SECURITY_MANDATORY_SYSTEM_RID &&
        myRid < (DWORD)SECURITY_MANDATORY_SYSTEM_RID) {
        return FALSE;
    }

    if (Security_HasUiAccess()) {
        return TRUE;
    }

    return (myRid >= targetRid) ? TRUE : FALSE;
}
