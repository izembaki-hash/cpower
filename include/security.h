/**
 * @file security.h
 * @brief Privilege Handling, uiAccess Verification, and UIPI Integrity Checks.
 *
 * Inspects process token privileges (TOKEN_UIACCESS, TokenElevation,
 * TokenIntegrityLevel), Windows registry policy (EnableSecureUIAPaths),
 * and cross-integrity window control feasibility.
 */

#ifndef CPOWER_SECURITY_H
#define CPOWER_SECURITY_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Windows Integrity Levels
 * ------------------------------------------------------------------------- */

typedef enum SecurityIntegrityLevel {
    INTEGRITY_UNKNOWN     = 0,
    INTEGRITY_UNTRUSTED   = 1,
    INTEGRITY_LOW         = 2,
    INTEGRITY_MEDIUM      = 3,
    INTEGRITY_MEDIUM_PLUS = 4,
    INTEGRITY_HIGH        = 5,
    INTEGRITY_SYSTEM      = 6
} SecurityIntegrityLevel;

/* -------------------------------------------------------------------------
 * Security & Environment Diagnostic Report
 * ------------------------------------------------------------------------- */

typedef struct SecurityEnvironmentReport {
    BOOL                   hasUiAccess;
    BOOL                   isElevated;
    SecurityIntegrityLevel integrityLevel;
    DWORD                  integrityRid;
    BOOL                   secureUiaPathsEnabled;
    BOOL                   inSecureDirectory;
    WCHAR                  executablePath[MAX_PATH];
} SecurityEnvironmentReport;

/* -------------------------------------------------------------------------
 * Security & UIPI Inspection Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Checks if the current process token has TOKEN_UIACCESS enabled.
 * @return TRUE if uiAccess=true is active on the process token, FALSE otherwise.
 */
BOOL Security_HasUiAccess(void);

/**
 * @brief Checks if the current process is running with elevated Administrator privileges.
 * @return TRUE if elevated, FALSE otherwise.
 */
BOOL Security_IsElevated(void);

/**
 * @brief Retrieves the Mandatory Integrity Level of the current process token.
 * @param pLevelOut Optional pointer receiving mapped SecurityIntegrityLevel enum.
 * @param pRidOut Optional pointer receiving raw Windows SECURITY_MANDATORY_* RID.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Security_GetIntegrityLevel(SecurityIntegrityLevel* pLevelOut, DWORD* pRidOut);

/**
 * @brief Converts a SecurityIntegrityLevel enum to a readable ASCII string.
 */
const char* Security_IntegrityLevelToString(SecurityIntegrityLevel level);

/**
 * @brief Queries HKLM\...\Policies\System\EnableSecureUIAPaths registry flag.
 * @return TRUE if secure path enforcement is active (1 or missing), FALSE if disabled (0).
 */
BOOL Security_IsSecureUiaPathsEnabled(void);

/**
 * @brief Checks if the current executable resides in a trusted Windows
 *        installation directory (registry ProgramFiles/ProgramFiles(x86)/ProgramW6432
 *        or GetSystemDirectoryW System32).
 */
BOOL Security_IsExecutableInSecurePath(void);

/**
 * @brief Verifies whether the current process holds active UIPI capability
 *        (TokenUIAccess or elevated Administrator privilege).
 * @return S_OK if active UIPI bypass capability is present, S_FALSE otherwise.
 */
HRESULT Security_VerifyEnvironment(void);

/**
 * @brief Populates a comprehensive SecurityEnvironmentReport for the current process.
 */
HRESULT Security_GetEnvironmentReport(SecurityEnvironmentReport* pReportOut);

/**
 * @brief Checks whether the current process can bypass UIPI to control targetHwnd.
 *        Rejects System-integrity windows when the caller is not System.
 * @param targetHwnd Target window handle.
 * @return TRUE if interaction is permitted by integrity / uiAccess rules.
 */
BOOL Security_CanInteractWithWindow(HWND targetHwnd);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_SECURITY_H */
