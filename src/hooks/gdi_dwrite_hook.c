/**
 * @file gdi_dwrite_hook.c
 * @brief Opt-in GDI/User32 Text-Capture IAT Hook DLL (cpower_hook64.dll).
 *
 * Explicitly injected only when requested via `cpower hook --pid <PID>` or
 * Hook_InjectProcess(). Scope is strictly GDI/User32 IAT patching: walks loaded
 * module IATs outside Loader Lock (via exported CPowerHook_Install) to intercept
 * ExtTextOutW, TextOutW, DrawTextW, and DrawTextExW from gdi32.dll,
 * gdi32full.dll, user32.dll, and api-ms-win-gdi-* / ext-ms-win-gdi-* API sets.
 * DirectWrite / Direct2D COM interfaces are out of scope.
 *
 * Publishes captured UTF-16 text snippets and coordinates into a lock-free
 * MPSC shared-memory ring buffer (`Local\CPowerTextHook_<pid>`).
 *
 * Designed for strict ISO C11 compilation (`-Wall -Wextra -Werror -pedantic`)
 * with zero non-OS DLL dependencies and deterministic IAT restoration on detach.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "text_hook.h"
#include <tlhelp32.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATCHED_SLOTS 2048U

typedef BOOL (WINAPI *PFN_ExtTextOutW)(
    HDC hdc, int x, int y, UINT options, const RECT* lprect,
    LPCWSTR lpString, UINT c, const INT* lpDx);
typedef BOOL (WINAPI *PFN_TextOutW)(
    HDC hdc, int x, int y, LPCWSTR lpString, int c);
typedef int (WINAPI *PFN_DrawTextW)(
    HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);
typedef int (WINAPI *PFN_DrawTextExW)(
    HDC hdc, LPWSTR lpchText, int cchText, LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp);

typedef struct PatchedIatSlot {
    ULONGLONG* pSlot;
    ULONGLONG  originalFn;
    ULONGLONG  hookFn;
} PatchedIatSlot;

static HMODULE            g_hSelf           = NULL;
static HANDLE             g_hShmMap         = NULL;
static CPowerTextHookShm* g_pShm            = NULL;
static DWORD              g_tlsDepthSlot    = TLS_OUT_OF_INDEXES;
static volatile LONG      g_inFlight        = 0;
static volatile LONG      g_installed       = 0;
static volatile LONG      g_unpatched       = 0;
static CRITICAL_SECTION   g_patchLock;
static BOOL               g_patchLockInit   = FALSE;

static PatchedIatSlot     g_patchedSlots[MAX_PATCHED_SLOTS];
static uint32_t           g_patchedCount    = 0;

static PFN_ExtTextOutW    g_origExtTextOutW = NULL;
static PFN_TextOutW       g_origTextOutW    = NULL;
static PFN_DrawTextW      g_origDrawTextW   = NULL;
static PFN_DrawTextExW    g_origDrawTextExW = NULL;

static size_t hook_strnlen(const char* s, size_t maxLen) {
    size_t n = 0U;
    if (!s) {
        return 0U;
    }
    while (n < maxLen && s[n] != '\0') {
        ++n;
    }
    return n;
}

static BOOL hook_ascii_istarts_with(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return FALSE;
    }
    while (*prefix) {
        unsigned char ca = (unsigned char)*s++;
        unsigned char cb = (unsigned char)*prefix++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) {
            return FALSE;
        }
    }
    return TRUE;
}

static int hook_ascii_icmp(const char* a, const char* b) {
    if (!a || !b) {
        return (a == b) ? 0 : (a ? 1 : -1);
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static ULONGLONG hook_fn_to_u64(const void* pFnBytes, size_t fnSize) {
    ULONGLONG val = 0;
    if (fnSize > sizeof(val)) {
        fnSize = sizeof(val);
    }
    memcpy(&val, pFnBytes, fnSize);
    return val;
}

static void hook_record_text(
    HDC hdc,
    uint32_t apiKind,
    int x,
    int y,
    const RECT* pRect,
    LPCWSTR lpText,
    int charCount
) {
    CPowerTextHookShm* pShm = g_pShm;
    if (!pShm || InterlockedCompareExchange(&pShm->active, 0, 0) == 0) {
        return;
    }
    if (!lpText || charCount == 0) {
        return;
    }

    int len = charCount;
    if (len < 0) {
        /* NUL-terminated */
        size_t maxScan = 127;
        size_t n = 0;
        while (n < maxScan && lpText[n] != L'\0') {
            ++n;
        }
        len = (int)n;
    }
    if (len <= 0) {
        return;
    }
    if (len > 127) {
        len = 127;
    }

    /* Skip pure whitespace */
    BOOL hasVisible = FALSE;
    for (int i = 0; i < len; ++i) {
        WCHAR ch = lpText[i];
        if (ch > L' ' && ch != 0x00A0) {
            hasVisible = TRUE;
            break;
        }
    }
    if (!hasVisible) {
        return;
    }

    /* Transform logical coordinates to window/client coordinates if HDC is valid */
    POINT pt = { x, y };
    RECT rc = { 0, 0, 0, 0 };
    if (pRect) {
        rc = *pRect;
    }
    if (hdc) {
        LPtoDP(hdc, &pt, 1);
        if (pRect) {
            POINT corners[2] = { { pRect->left, pRect->top }, { pRect->right, pRect->bottom } };
            LPtoDP(hdc, corners, 2);
            rc.left = corners[0].x;
            rc.top = corners[0].y;
            rc.right = corners[1].x;
            rc.bottom = corners[1].y;
        }
    }

    LONG seqSigned = InterlockedIncrement(&pShm->writeSeq);
    uint64_t seq = (uint64_t)(uint32_t)seqSigned;
    if (seq == 0U) {
        seqSigned = InterlockedIncrement(&pShm->writeSeq);
        seq = (uint64_t)(uint32_t)seqSigned;
    }
    uint32_t idx = (uint32_t)((seq - 1U) % CPOWER_TEXT_HOOK_RING_CAP);
    CPowerTextHookEntry* pEntry = &pShm->entries[idx];

    /* MPSC seq-lock: publish 0 (in-progress) -> fill payload -> barrier -> commit seq */
    InterlockedExchange64(&pEntry->seq, 0);
    MemoryBarrier();

    pEntry->timestampMs = GetTickCount64();
    pEntry->pid = GetCurrentProcessId();
    pEntry->tid = GetCurrentThreadId();
    pEntry->x = pt.x;
    pEntry->y = pt.y;
    pEntry->rect = rc;
    pEntry->apiKind = apiKind;
    pEntry->charCount = (uint16_t)len;
    pEntry->reserved = 0;

    memcpy(pEntry->text, lpText, (size_t)len * sizeof(WCHAR));
    pEntry->text[len] = L'\0';

    MemoryBarrier();
    InterlockedExchange64(&pEntry->seq, (LONG64)seq);
}

static BOOL hook_enter_depth(void) {
    if (g_tlsDepthSlot == TLS_OUT_OF_INDEXES) {
        return TRUE;
    }
    uintptr_t depth = (uintptr_t)TlsGetValue(g_tlsDepthSlot);
    TlsSetValue(g_tlsDepthSlot, (LPVOID)(depth + 1U));
    return (depth == 0);
}

static void hook_leave_depth(void) {
    if (g_tlsDepthSlot == TLS_OUT_OF_INDEXES) {
        return;
    }
    uintptr_t depth = (uintptr_t)TlsGetValue(g_tlsDepthSlot);
    if (depth > 0) {
        TlsSetValue(g_tlsDepthSlot, (LPVOID)(depth - 1U));
    }
}

/* -------------------------------------------------------------------------
 * Hooked GDI / User32 Text Drawing Wrappers
 * ------------------------------------------------------------------------- */

static BOOL WINAPI Hook_ExtTextOutW(
    HDC hdc, int x, int y, UINT options, const RECT* lprect,
    LPCWSTR lpString, UINT c, const INT* lpDx
) {
    InterlockedIncrement(&g_inFlight);
    BOOL topLevel = hook_enter_depth();
    if (topLevel && (options & ETO_GLYPH_INDEX) == 0U && lpString && c > 0U) {
        hook_record_text(hdc, HOOK_API_EXTTEXTOUTW, x, y, lprect, lpString, (int)c);
    }

    PFN_ExtTextOutW orig = g_origExtTextOutW;
    BOOL ret = orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx) : FALSE;

    hook_leave_depth();
    InterlockedDecrement(&g_inFlight);
    return ret;
}

static BOOL WINAPI Hook_TextOutW(
    HDC hdc, int x, int y, LPCWSTR lpString, int c
) {
    InterlockedIncrement(&g_inFlight);
    BOOL topLevel = hook_enter_depth();
    if (topLevel && lpString && c > 0) {
        hook_record_text(hdc, HOOK_API_TEXTOUTW, x, y, NULL, lpString, c);
    }

    PFN_TextOutW orig = g_origTextOutW;
    BOOL ret = orig ? orig(hdc, x, y, lpString, c) : FALSE;

    hook_leave_depth();
    InterlockedDecrement(&g_inFlight);
    return ret;
}

static int WINAPI Hook_DrawTextW(
    HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format
) {
    InterlockedIncrement(&g_inFlight);
    BOOL topLevel = hook_enter_depth();
    if (topLevel && (format & DT_CALCRECT) == 0U && lpchText && cchText != 0) {
        int x = lprc ? lprc->left : 0;
        int y = lprc ? lprc->top : 0;
        hook_record_text(hdc, HOOK_API_DRAWTEXTW, x, y, lprc, lpchText, cchText);
    }

    PFN_DrawTextW orig = g_origDrawTextW;
    int ret = orig ? orig(hdc, lpchText, cchText, lprc, format) : 0;

    hook_leave_depth();
    InterlockedDecrement(&g_inFlight);
    return ret;
}

static int WINAPI Hook_DrawTextExW(
    HDC hdc, LPWSTR lpchText, int cchText, LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp
) {
    InterlockedIncrement(&g_inFlight);
    BOOL topLevel = hook_enter_depth();
    if (topLevel && (format & DT_CALCRECT) == 0U && lpchText && cchText != 0) {
        int x = lprc ? lprc->left : 0;
        int y = lprc ? lprc->top : 0;
        hook_record_text(hdc, HOOK_API_DRAWTEXTEXW, x, y, lprc, lpchText, cchText);
    }

    PFN_DrawTextExW orig = g_origDrawTextExW;
    int ret = orig ? orig(hdc, lpchText, cchText, lprc, format, lpdtp) : 0;

    hook_leave_depth();
    InterlockedDecrement(&g_inFlight);
    return ret;
}

/* -------------------------------------------------------------------------
 * Safe PE IAT Patching & Restoration
 * ------------------------------------------------------------------------- */

static BOOL hook_patch_iat_slot(ULONGLONG* pSlot, ULONGLONG newFn) {
    if (!pSlot || !newFn) {
        return FALSE;
    }
    if (*pSlot == newFn) {
        return FALSE;
    }
    if (g_patchedCount >= MAX_PATCHED_SLOTS) {
        return FALSE;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(pSlot, sizeof(ULONGLONG), PAGE_READWRITE, &oldProt)) {
        return FALSE;
    }

    ULONGLONG prevFn = *pSlot;
    *pSlot = newFn;
    FlushInstructionCache(GetCurrentProcess(), pSlot, sizeof(ULONGLONG));

    DWORD dummy = 0;
    VirtualProtect(pSlot, sizeof(ULONGLONG), oldProt, &dummy);

    g_patchedSlots[g_patchedCount].pSlot = pSlot;
    g_patchedSlots[g_patchedCount].originalFn = prevFn;
    g_patchedSlots[g_patchedCount].hookFn = newFn;
    g_patchedCount++;
    return TRUE;
}

static void hook_patch_module_iat(HMODULE hMod, DWORD modSize) {
    if (!hMod || hMod == g_hSelf || modSize < sizeof(IMAGE_DOS_HEADER)) {
        return;
    }

    const uint64_t modSz = (uint64_t)modSize;
    const uint8_t* base = (const uint8_t*)hMod;
    const IMAGE_DOS_HEADER* pDos = (const IMAGE_DOS_HEADER*)base;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    if (pDos->e_lfanew <= 0) {
        return;
    }

    const uint64_t ntOff = (uint64_t)(DWORD)pDos->e_lfanew;
    if (ntOff + (uint64_t)offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + sizeof(WORD) > modSz) {
        return;
    }

    const IMAGE_NT_HEADERS64* pNt = (const IMAGE_NT_HEADERS64*)(base + ntOff);
    if (pNt->Signature != IMAGE_NT_SIGNATURE ||
        pNt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }
    if (pNt->FileHeader.SizeOfOptionalHeader <
            offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT + 1])) {
        return;
    }
    if (ntOff + (uint64_t)offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            (uint64_t)pNt->FileHeader.SizeOfOptionalHeader > modSz) {
        return;
    }
    if (pNt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return;
    }

    IMAGE_DATA_DIRECTORY impDir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0U || impDir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return;
    }
    if ((uint64_t)impDir.VirtualAddress >= modSz ||
        (uint64_t)impDir.VirtualAddress + (uint64_t)impDir.Size > modSz) {
        return;
    }

    /* Skip core low-level system DLLs and GDI implementation modules themselves */
    WCHAR modName[MAX_PATH];
    DWORD len = GetModuleFileNameW(hMod, modName, MAX_PATH);
    if (len > 0U && len < MAX_PATH) {
        const WCHAR* leaf = modName;
        for (DWORD i = 0; i < len; ++i) {
            if (modName[i] == L'\\' || modName[i] == L'/') {
                leaf = &modName[i + 1];
            }
        }
        if (CompareStringOrdinal(leaf, -1, L"ntdll.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"kernel32.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"kernelbase.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"gdi32.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"gdi32full.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"user32.dll", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(leaf, -1, L"win32u.dll", -1, TRUE) == CSTR_EQUAL) {
            return;
        }
    }

    PFN_ExtTextOutW pHookExt = Hook_ExtTextOutW;
    PFN_TextOutW    pHookTxt = Hook_TextOutW;
    PFN_DrawTextW   pHookDt  = Hook_DrawTextW;
    PFN_DrawTextExW pHookDte = Hook_DrawTextExW;
    ULONGLONG u64HookExt = hook_fn_to_u64(&pHookExt, sizeof(pHookExt));
    ULONGLONG u64HookTxt = hook_fn_to_u64(&pHookTxt, sizeof(pHookTxt));
    ULONGLONG u64HookDt  = hook_fn_to_u64(&pHookDt, sizeof(pHookDt));
    ULONGLONG u64HookDte = hook_fn_to_u64(&pHookDte, sizeof(pHookDte));

    const IMAGE_IMPORT_DESCRIPTOR* pDesc =
        (const IMAGE_IMPORT_DESCRIPTOR*)(base + impDir.VirtualAddress);

    for (size_t d = 0; ((uint64_t)(d + 1U) * (uint64_t)sizeof(IMAGE_IMPORT_DESCRIPTOR)) <= (uint64_t)impDir.Size; ++d) {
        if (pDesc[d].Name == 0U || pDesc[d].FirstThunk == 0U) {
            break;
        }
        if ((uint64_t)pDesc[d].Name >= modSz || (uint64_t)pDesc[d].FirstThunk >= modSz) {
            continue;
        }

        const char* dllName = (const char*)(base + pDesc[d].Name);
        size_t maxDllLen = (size_t)(modSz - (uint64_t)pDesc[d].Name);
        size_t dllNameLen = hook_strnlen(dllName, maxDllLen);
        if (dllNameLen == 0U || dllNameLen >= maxDllLen) {
            continue;
        }

        BOOL isGdi = (hook_ascii_icmp(dllName, "gdi32.dll") == 0 ||
                      hook_ascii_icmp(dllName, "gdi32full.dll") == 0 ||
                      hook_ascii_istarts_with(dllName, "api-ms-win-gdi-") ||
                      hook_ascii_istarts_with(dllName, "ext-ms-win-gdi-"));
        BOOL isUser32 = (hook_ascii_icmp(dllName, "user32.dll") == 0 ||
                         hook_ascii_istarts_with(dllName, "api-ms-win-ntuser-") ||
                         hook_ascii_istarts_with(dllName, "ext-ms-win-ntuser-"));
        if (!isGdi && !isUser32) {
            continue;
        }

        DWORD origRva = pDesc[d].OriginalFirstThunk ? pDesc[d].OriginalFirstThunk : pDesc[d].FirstThunk;
        if ((uint64_t)origRva >= modSz) {
            continue;
        }
        if ((origRva & 7U) != 0U || (pDesc[d].FirstThunk & 7U) != 0U) {
            continue;
        }

        const IMAGE_THUNK_DATA64* pOrigThunk = (const IMAGE_THUNK_DATA64*)(base + origRva);
        IMAGE_THUNK_DATA64*       pIatThunk  = (IMAGE_THUNK_DATA64*)((uint8_t*)hMod + pDesc[d].FirstThunk);

        for (size_t idx = 0; ; ++idx) {
            uint64_t thunkSpan = (uint64_t)(idx + 1U) * (uint64_t)sizeof(IMAGE_THUNK_DATA64);
            if ((uint64_t)origRva + thunkSpan > modSz ||
                (uint64_t)pDesc[d].FirstThunk + thunkSpan > modSz) {
                break;
            }
            if (pOrigThunk[idx].u1.AddressOfData == 0ULL || pIatThunk[idx].u1.Function == 0ULL) {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL64(pOrigThunk[idx].u1.Ordinal)) {
                continue;
            }
            ULONGLONG nameRva = pOrigThunk[idx].u1.AddressOfData;
            if ((uint64_t)nameRva + (uint64_t)offsetof(IMAGE_IMPORT_BY_NAME, Name) + 2ULL > modSz) {
                continue;
            }
            const IMAGE_IMPORT_BY_NAME* pByName = (const IMAGE_IMPORT_BY_NAME*)(base + nameRva);
            const char* fnName = pByName->Name;
            size_t maxFnLen = (size_t)(modSz - ((uint64_t)nameRva + (uint64_t)offsetof(IMAGE_IMPORT_BY_NAME, Name)));
            size_t fnLen = hook_strnlen(fnName, maxFnLen);
            if (fnLen == 0U || fnLen >= maxFnLen) {
                continue;
            }

            if (isGdi && strcmp(fnName, "ExtTextOutW") == 0) {
                if (!g_origExtTextOutW) {
                    ULONGLONG origVal = pIatThunk[idx].u1.Function;
                    memcpy(&g_origExtTextOutW, &origVal, sizeof(g_origExtTextOutW));
                }
                hook_patch_iat_slot(&pIatThunk[idx].u1.Function, u64HookExt);
            } else if (isGdi && strcmp(fnName, "TextOutW") == 0) {
                if (!g_origTextOutW) {
                    ULONGLONG origVal = pIatThunk[idx].u1.Function;
                    memcpy(&g_origTextOutW, &origVal, sizeof(g_origTextOutW));
                }
                hook_patch_iat_slot(&pIatThunk[idx].u1.Function, u64HookTxt);
            } else if (isUser32 && strcmp(fnName, "DrawTextW") == 0) {
                if (!g_origDrawTextW) {
                    ULONGLONG origVal = pIatThunk[idx].u1.Function;
                    memcpy(&g_origDrawTextW, &origVal, sizeof(g_origDrawTextW));
                }
                hook_patch_iat_slot(&pIatThunk[idx].u1.Function, u64HookDt);
            } else if (isUser32 && strcmp(fnName, "DrawTextExW") == 0) {
                if (!g_origDrawTextExW) {
                    ULONGLONG origVal = pIatThunk[idx].u1.Function;
                    memcpy(&g_origDrawTextExW, &origVal, sizeof(g_origDrawTextExW));
                }
                hook_patch_iat_slot(&pIatThunk[idx].u1.Function, u64HookDte);
            }
        }
    }
}

static void hook_unpatch_all_slots(void) {
    if (g_patchLockInit) {
        EnterCriticalSection(&g_patchLock);
    }

    if (InterlockedExchange(&g_unpatched, 1) == 0) {
        for (uint32_t i = g_patchedCount; i > 0; --i) {
            PatchedIatSlot* pRec = &g_patchedSlots[i - 1U];
            if (pRec->pSlot && pRec->originalFn) {
                HMODULE hModCheck = NULL;
                if (GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)pRec->pSlot,
                        &hModCheck) && hModCheck != NULL) {
                    if (*pRec->pSlot == pRec->hookFn) {
                        DWORD oldProt = 0;
                        if (VirtualProtect(pRec->pSlot, sizeof(ULONGLONG), PAGE_READWRITE, &oldProt)) {
                            if (*pRec->pSlot == pRec->hookFn) {
                                *pRec->pSlot = pRec->originalFn;
                                FlushInstructionCache(GetCurrentProcess(), pRec->pSlot, sizeof(ULONGLONG));
                            }
                            DWORD dummy = 0;
                            VirtualProtect(pRec->pSlot, sizeof(ULONGLONG), oldProt, &dummy);
                        }
                    }
                }
            }
        }
        g_patchedCount = 0;
    }

    if (g_patchLockInit) {
        LeaveCriticalSection(&g_patchLock);
    }
}

static void hook_resolve_original_apis(void) {
    HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll");
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");

    if (hGdi32) {
        FARPROC pExt = GetProcAddress(hGdi32, "ExtTextOutW");
        FARPROC pTxt = GetProcAddress(hGdi32, "TextOutW");
        if (pExt) memcpy(&g_origExtTextOutW, &pExt, sizeof(g_origExtTextOutW));
        if (pTxt) memcpy(&g_origTextOutW, &pTxt, sizeof(g_origTextOutW));
    }
    if (hUser32) {
        FARPROC pDt  = GetProcAddress(hUser32, "DrawTextW");
        FARPROC pDte = GetProcAddress(hUser32, "DrawTextExW");
        if (pDt)  memcpy(&g_origDrawTextW, &pDt, sizeof(g_origDrawTextW));
        if (pDte) memcpy(&g_origDrawTextExW, &pDte, sizeof(g_origDrawTextExW));
    }
}

static BOOL hook_create_least_privilege_sa(SECURITY_ATTRIBUTES* pSa) {
    if (!pSa) {
        return FALSE;
    }
    memset(pSa, 0, sizeof(*pSa));
    pSa->nLength = sizeof(SECURITY_ATTRIBUTES);
    pSa->bInheritHandle = FALSE;

    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return FALSE;
    }

    DWORD needed = 0U;
    if (!GetTokenInformation(hToken, TokenUser, NULL, 0U, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(hToken);
        return FALSE;
    }
    if (needed < sizeof(TOKEN_USER)) {
        CloseHandle(hToken);
        return FALSE;
    }

    union {
        TOKEN_USER user;
        uint8_t    raw[256];
    } stackBuf;
    TOKEN_USER* pTokenUser = (needed <= sizeof(stackBuf))
        ? &stackBuf.user
        : (TOKEN_USER*)HeapAlloc(GetProcessHeap(), 0, needed);
    if (!pTokenUser) {
        CloseHandle(hToken);
        return FALSE;
    }

    BOOL ok = FALSE;
    LPWSTR pszUserSid = NULL;
    if (GetTokenInformation(hToken, TokenUser, pTokenUser, needed, &needed) &&
        pTokenUser->User.Sid && IsValidSid(pTokenUser->User.Sid) &&
        ConvertSidToStringSidW(pTokenUser->User.Sid, &pszUserSid) && pszUserSid) {
        WCHAR sddl[512];
        int written = swprintf(
            sddl,
            512,
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%ls)S:(ML;;NW;;;ME)",
            pszUserSid
        );
        if (written > 0 && written < 512 &&
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &pSa->lpSecurityDescriptor, NULL)) {
            ok = TRUE;
        }
    }

    if (pszUserSid) {
        LocalFree(pszUserSid);
    }
    if (pTokenUser != &stackBuf.user) {
        HeapFree(GetProcessHeap(), 0, pTokenUser);
    }
    CloseHandle(hToken);
    return ok;
}

static BOOL hook_init_shm(DWORD pid) {
    WCHAR shmName[128];
    swprintf(shmName, 128, CPOWER_TEXT_HOOK_SHM_FMT, (unsigned long)pid);
    shmName[127] = L'\0';

    g_hShmMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (!g_hShmMap) {
        SECURITY_ATTRIBUTES sa;
        if (!hook_create_least_privilege_sa(&sa)) {
            return FALSE; /* Fail closed: never create unprotected SHM */
        }
        g_hShmMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            (DWORD)sizeof(CPowerTextHookShm),
            shmName
        );
        LocalFree(sa.lpSecurityDescriptor);
    }
    if (!g_hShmMap) {
        return FALSE;
    }

    g_pShm = (CPowerTextHookShm*)MapViewOfFile(
        g_hShmMap,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(CPowerTextHookShm)
    );
    if (!g_pShm) {
        CloseHandle(g_hShmMap);
        g_hShmMap = NULL;
        return FALSE;
    }

    if (g_pShm->magic != CPOWER_TEXT_HOOK_MAGIC || g_pShm->targetPid != pid) {
        memset(g_pShm, 0, sizeof(CPowerTextHookShm));
        g_pShm->version = CPOWER_TEXT_HOOK_VERSION;
        g_pShm->targetPid = pid;
        g_pShm->capacity = CPOWER_TEXT_HOOK_RING_CAP;
        MemoryBarrier();
        g_pShm->magic = CPOWER_TEXT_HOOK_MAGIC;
        MemoryBarrier();
    }
    InterlockedExchange(&g_pShm->active, 1);
    return TRUE;
}

static void hook_install_all_iats(void) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap == INVALID_HANDLE_VALUE) {
        return;
    }

    EnterCriticalSection(&g_patchLock);

    MODULEENTRY32W me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32FirstW(hSnap, &me)) {
        do {
            hook_patch_module_iat(me.hModule, me.modBaseSize);
        } while (Module32NextW(hSnap, &me));
    }

    if (g_pShm) {
        g_pShm->patchCount = g_patchedCount;
    }

    LeaveCriticalSection(&g_patchLock);
    CloseHandle(hSnap);
}

__declspec(dllexport) DWORD WINAPI CPowerHook_Install(LPVOID lpParam) {
    (void)lpParam;
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        return ERROR_SUCCESS;
    }
    if (!g_patchLockInit) {
        InterlockedExchange(&g_installed, 0);
        return ERROR_NOT_READY;
    }

    InterlockedExchange(&g_unpatched, 0);
    hook_resolve_original_apis();
    if (!hook_init_shm(GetCurrentProcessId())) {
        InterlockedExchange(&g_installed, 0);
        return ERROR_OPEN_FAILED;
    }
    hook_install_all_iats();
    return ERROR_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CPowerHook_PrepareDetach(LPVOID lpParam) {
    (void)lpParam;
    if (g_pShm) {
        InterlockedExchange(&g_pShm->active, 0);
    }
    hook_unpatch_all_slots();

    /* Wait for all in-flight hook wrapper calls to drain outside Loader Lock */
    DWORD waitedMs = 0U;
    const DWORD maxWaitMs = 2000U;
    while (InterlockedCompareExchange(&g_inFlight, 0, 0) > 0) {
        if (waitedMs >= maxWaitMs) {
            return ERROR_BUSY;
        }
        Sleep(5);
        waitedMs += 5U;
    }

    /* Post-quiescence grace period outside Loader Lock before FreeLibrary */
    Sleep(50);

    if (InterlockedCompareExchange(&g_inFlight, 0, 0) > 0) {
        return ERROR_BUSY;
    }

    InterlockedExchange(&g_installed, 0);
    return ERROR_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID lpReserved) {
    if (ulReason == DLL_PROCESS_ATTACH) {
        /* Keep DllMain minimal: NO CreateToolhelp32Snapshot or heavy work under Loader Lock.
           IAT installation is deferred to exported CPowerHook_Install(). */
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_patchLock);
        g_patchLockInit = TRUE;
        g_tlsDepthSlot = TlsAlloc();
    } else if (ulReason == DLL_PROCESS_DETACH) {
        if (lpReserved != NULL) {
            /* Process termination: exit immediately without unpatching, Sleep,
               CriticalSection operations, or freeing locks. */
            return TRUE;
        }
        if (g_pShm) {
            InterlockedExchange(&g_pShm->active, 0);
        }
        hook_unpatch_all_slots();

        if (g_pShm) {
            UnmapViewOfFile(g_pShm);
            g_pShm = NULL;
        }
        if (g_hShmMap) {
            CloseHandle(g_hShmMap);
            g_hShmMap = NULL;
        }
        if (g_tlsDepthSlot != TLS_OUT_OF_INDEXES) {
            TlsFree(g_tlsDepthSlot);
            g_tlsDepthSlot = TLS_OUT_OF_INDEXES;
        }
        if (g_patchLockInit) {
            DeleteCriticalSection(&g_patchLock);
            g_patchLockInit = FALSE;
        }
    }
    return TRUE;
}
