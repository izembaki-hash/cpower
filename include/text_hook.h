/**
 * @file text_hook.h
 * @brief Opt-in GDI/User32 Text-Capture IAT Hook shared memory ring & injector API.
 *
 * Strictly disabled by default; activated only when explicitly invoked via
 * `cpower hook --pid <PID>` or Hook_InjectProcess().
 *
 * Scope: Captures GDI and User32 text drawing APIs (ExtTextOutW, TextOutW,
 * DrawTextW, DrawTextExW) via PE Import Address Table (IAT) patching.
 * DirectWrite / Direct2D interfaces are outside the IAT hook scope.
 */

#ifndef CPOWER_TEXT_HOOK_H
#define CPOWER_TEXT_HOOK_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPOWER_TEXT_HOOK_MAGIC     0x5458484BU /* "TXHK" */
#define CPOWER_TEXT_HOOK_VERSION   1U
#define CPOWER_TEXT_HOOK_RING_CAP  256U
#define CPOWER_TEXT_HOOK_SHM_FMT   L"Local\\CPowerTextHook_%lu"
#define CPOWER_TEXT_HOOK_STOP_FMT  L"Local\\CPowerTextHookStop_%lu"

typedef enum CPowerTextHookApiKind {
    HOOK_API_UNKNOWN     = 0,
    HOOK_API_EXTTEXTOUTW = 1,
    HOOK_API_TEXTOUTW    = 2,
    HOOK_API_DRAWTEXTW   = 3,
    HOOK_API_DRAWTEXTEXW = 4
} CPowerTextHookApiKind;

#pragma pack(push, 4)

typedef struct CPowerTextHookEntry {
    volatile LONG64 seq;
    uint64_t        timestampMs;
    uint32_t pid;
    uint32_t tid;
    int32_t  x;
    int32_t  y;
    RECT     rect;
    uint32_t apiKind;
    uint16_t charCount;
    uint16_t reserved;
    WCHAR    text[128];
} CPowerTextHookEntry;

typedef struct CPowerTextHookShm {
    uint32_t            magic;
    uint32_t            version;
    uint32_t            targetPid;
    uint32_t            capacity;
    volatile LONG       writeSeq;
    volatile LONG       active;
    uint32_t            patchCount;
    uint32_t            reserved;
    CPowerTextHookEntry entries[CPOWER_TEXT_HOOK_RING_CAP];
} CPowerTextHookShm;

#pragma pack(pop)

_Static_assert(sizeof(CPowerTextHookEntry) == 312, "CPowerTextHookEntry packed size must be 312 bytes");
_Static_assert(offsetof(CPowerTextHookEntry, seq) == 0, "CPowerTextHookEntry.seq must be at offset 0");

/**
 * @brief Checks if the target process is 64-bit (native x64, non-WOW64).
 */
HRESULT Hook_IsProcess64Bit(DWORD pid, BOOL* pIs64Out);

/**
 * @brief Injects cpower_hook64.dll into an explicit target 64-bit process PID.
 * @param pid Target process ID.
 * @param pszDllPath Optional path to cpower_hook64.dll (NULL resolves alongside current exe).
 */
HRESULT Hook_InjectProcess(DWORD pid, const WCHAR* pszDllPath);

/**
 * @brief Reads captured text entries from Local\CPowerTextHook_<pid> shared memory ring.
 * @param pid Target process ID.
 * @param pOut Destination array of entries.
 * @param maxEntries Maximum entries to copy.
 * @param pOutCount Number of entries copied.
 * @param pInOutLastSeq Optional cursor tracking highest sequence number consumed so far.
 */
HRESULT Hook_ReadCapturedText(
    DWORD pid,
    CPowerTextHookEntry* pOut,
    uint32_t maxEntries,
    uint32_t* pOutCount,
    uint64_t* pInOutLastSeq
);

/**
 * @brief Cleanly disables IAT hooks and unloads cpower_hook64.dll from target process.
 */
HRESULT Hook_DetachProcess(DWORD pid, DWORD timeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_TEXT_HOOK_H */
