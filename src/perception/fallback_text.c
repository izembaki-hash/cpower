/**
 * @file fallback_text.c
 * @brief Win32 GDI fallback text extraction (WM_GETTEXT, EM_GETLINE, LB_GETTEXT,
 *        CB_GETLBTEXT, cross-process SysListView32) and Chromium accessibility poke.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "perception.h"
#include <commctrl.h>
#include <limits.h>
#include <string.h>

#define CPOWER_MSG_TIMEOUT_MS 500U
#define CPOWER_LV_TEXT_MAX    256
#define CPOWER_LIST_BUF_MIN   4096U

#ifndef IMAGE_FILE_MACHINE_UNKNOWN
#define IMAGE_FILE_MACHINE_UNKNOWN 0x0000U
#endif

#pragma pack(push, 4)
typedef struct CPOWER_LVITEMW32 {
    UINT     mask;
    int      iItem;
    int      iSubItem;
    UINT     state;
    UINT     stateMask;
    uint32_t pszText;
    int      cchTextMax;
    int      iImage;
    int32_t  lParam;
    int      iIndent;
    int      iGroupId;
    UINT     cColumns;
    uint32_t puColumns;
    uint32_t piColFmt;
    int      iGroup;
} CPOWER_LVITEMW32;
#pragma pack(pop)

_Static_assert(sizeof(CPOWER_LVITEMW32) == 60, "CPOWER_LVITEMW32 must match 32-bit LVITEMW layout");

typedef BOOL (WINAPI *PFN_IsWow64Process2)(HANDLE hProcess, USHORT* pProcessMachine, USHORT* pNativeMachine);

static BOOL safe_send_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR* pOutRes) {
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(
        hwnd,
        msg,
        wp,
        lp,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        CPOWER_MSG_TIMEOUT_MS,
        &res
    );
    if (pOutRes) {
        *pOutRes = res;
    }
    return (ok != 0);
}

static void append_wstr(WCHAR* dst, int maxChars, int* pPos, const WCHAR* src, int srcLen) {
    if (!dst || !pPos || !src || srcLen <= 0) {
        return;
    }
    int limit = maxChars - 1;
    for (int i = 0; i < srcLen && *pPos < limit; i++) {
        if (src[i] == L'\0') {
            break;
        }
        dst[(*pPos)++] = src[i];
    }
    dst[*pPos] = L'\0';
}

static void append_wchar(WCHAR* dst, int maxChars, int* pPos, WCHAR ch) {
    if (!dst || !pPos) {
        return;
    }
    if (*pPos < maxChars - 1) {
        dst[(*pPos)++] = ch;
        dst[*pPos] = L'\0';
    }
}

static HRESULT extract_wm_gettext(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    DWORD_PTR lenRes = 0;
    if (!safe_send_msg(hwnd, WM_GETTEXTLENGTH, 0, 0, &lenRes)) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }
    if (lenRes == 0) {
        pBuffer[0] = L'\0';
        return S_FALSE;
    }

    DWORD_PTR copied = 0;
    if (!safe_send_msg(hwnd, WM_GETTEXT, (WPARAM)maxChars, (LPARAM)pBuffer, &copied)) {
        DWORD err = GetLastError();
        pBuffer[0] = L'\0';
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    pBuffer[maxChars - 1] = L'\0';
    return (copied > 0 || pBuffer[0] != L'\0') ? S_OK : S_FALSE;
}

static HRESULT extract_edit_text(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    DWORD_PTR lineCountRes = 0;
    if (safe_send_msg(hwnd, EM_GETLINECOUNT, 0, 0, &lineCountRes) && (LONG_PTR)lineCountRes > 1) {
        int lineCount = (int)(LONG_PTR)lineCountRes;
        int outPos = 0;
        int lineCap = (maxChars > 1024) ? maxChars : 1024;
        if (lineCap > 65535) {
            lineCap = 65535;
        }

        WCHAR stackLine[1024];
        WCHAR* lineBuf = stackLine;
        BOOL heapAlloc = FALSE;
        if (lineCap > 1024) {
            lineBuf = (WCHAR*)malloc((size_t)lineCap * sizeof(WCHAR));
            if (!lineBuf) {
                return E_OUTOFMEMORY;
            }
            heapAlloc = TRUE;
        }

        for (int i = 0; i < lineCount && outPos < maxChars - 1; i++) {
            if (i > 0) {
                append_wchar(pBuffer, maxChars, &outPos, L'\n');
                if (outPos >= maxChars - 1) {
                    break;
                }
            }

            /* Critical Win32 EM_GETLINE requirement: first WORD holds buffer capacity in WCHARs */
            *((WORD*)lineBuf) = (WORD)(lineCap - 1);
            DWORD_PTR charsRead = 0;
            if (safe_send_msg(hwnd, EM_GETLINE, (WPARAM)i, (LPARAM)lineBuf, &charsRead)) {
                int n = (int)charsRead;
                if (n >= lineCap) {
                    n = lineCap - 1;
                }
                if (n > 0) {
                    lineBuf[n] = L'\0';
                    append_wstr(pBuffer, maxChars, &outPos, lineBuf, n);
                }
            }
        }

        if (heapAlloc) {
            free(lineBuf);
        }

        if (outPos > 0) {
            return S_OK;
        }
    }

    return extract_wm_gettext(hwnd, pBuffer, maxChars);
}

static HRESULT ensure_list_text_buf(WCHAR** ppBuf, size_t* pCapChars, LONG_PTR itemLen) {
    if (itemLen < 0) {
        return E_INVALIDARG;
    }
    if ((uint64_t)itemLen > ((uint64_t)SIZE_MAX / sizeof(WCHAR) - 64ULL) / 2ULL) {
        return E_OUTOFMEMORY;
    }

    size_t needCap = ((size_t)itemLen * 2U) + 64U;
    if (needCap < CPOWER_LIST_BUF_MIN) {
        needCap = CPOWER_LIST_BUF_MIN;
    }

    if (*ppBuf != NULL && *pCapChars >= needCap) {
        return S_OK;
    }

    WCHAR* grown = (WCHAR*)realloc(*ppBuf, needCap * sizeof(WCHAR));
    if (!grown) {
        return E_OUTOFMEMORY;
    }
    *ppBuf = grown;
    *pCapChars = needCap;
    return S_OK;
}

static HRESULT read_list_item_text(
    HWND hwnd,
    UINT msgGet,
    LONG_PTR idx,
    LONG_PTR itemLen,
    LONG_PTR errVal,
    WCHAR** ppBuf,
    size_t* pCapChars,
    WCHAR* pOut,
    int maxChars,
    int* pOutPos
) {
    HRESULT hrBuf = ensure_list_text_buf(ppBuf, pCapChars, itemLen);
    if (FAILED(hrBuf)) {
        return hrBuf;
    }

    WCHAR* itemBuf = *ppBuf;
    size_t capChars = *pCapChars;
    ZeroMemory(itemBuf, capChars * sizeof(WCHAR));

    DWORD_PTR res = 0;
    if (safe_send_msg(hwnd, msgGet, (WPARAM)idx, (LPARAM)itemBuf, &res) &&
        (LONG_PTR)res != errVal && (LONG_PTR)res > 0) {
        size_t len = (size_t)res;
        if (len > capChars - 1U) {
            len = capChars - 1U;
        }
        itemBuf[len] = L'\0';
        itemBuf[capChars - 1U] = L'\0';

        int rem = (maxChars - 1) - *pOutPos;
        if (rem > 0) {
            int copyLen = (len > (size_t)rem) ? rem : (int)len;
            append_wstr(pOut, maxChars, pOutPos, itemBuf, copyLen);
        }
    }

    return S_OK;
}

static HRESULT extract_listbox_text(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    DWORD_PTR countRes = 0;
    if (!safe_send_msg(hwnd, LB_GETCOUNT, 0, 0, &countRes)) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    LONG_PTR count = (LONG_PTR)countRes;
    if (count == LB_ERR || count <= 0) {
        pBuffer[0] = L'\0';
        return S_FALSE;
    }

    WCHAR* itemBuf = NULL;
    size_t bufCap = 0U;
    int outPos = 0;
    for (LONG_PTR i = 0; i < count && outPos < maxChars - 1; i++) {
        DWORD_PTR lenRes = 0;
        if (!safe_send_msg(hwnd, LB_GETTEXTLEN, (WPARAM)i, 0, &lenRes)) {
            continue;
        }
        LONG_PTR itemLen = (LONG_PTR)lenRes;
        if (itemLen == LB_ERR || itemLen < 0) {
            continue;
        }

        if (i > 0) {
            append_wchar(pBuffer, maxChars, &outPos, L'\n');
            if (outPos >= maxChars - 1) {
                break;
            }
        }

        if (itemLen == 0) {
            continue;
        }

        HRESULT hrItem = read_list_item_text(
            hwnd, LB_GETTEXT, i, itemLen, LB_ERR, &itemBuf, &bufCap, pBuffer, maxChars, &outPos
        );
        if (FAILED(hrItem)) {
            free(itemBuf);
            return hrItem;
        }
    }

    free(itemBuf);
    return (outPos > 0) ? S_OK : S_FALSE;
}

static HRESULT extract_combobox_text(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    /* Try selected/edit text via WM_GETTEXT first */
    HRESULT hr = extract_wm_gettext(hwnd, pBuffer, maxChars);
    if (hr == S_OK && pBuffer[0] != L'\0') {
        return S_OK;
    }

    WCHAR* itemBuf = NULL;
    size_t bufCap = 0U;

    /* Try current selection via CB_GETCURSEL + CB_GETLBTEXT */
    DWORD_PTR selRes = 0;
    if (safe_send_msg(hwnd, CB_GETCURSEL, 0, 0, &selRes) && (LONG_PTR)selRes != CB_ERR) {
        LONG_PTR sel = (LONG_PTR)selRes;
        DWORD_PTR lenRes = 0;
        if (safe_send_msg(hwnd, CB_GETLBTEXTLEN, (WPARAM)sel, 0, &lenRes) &&
            (LONG_PTR)lenRes != CB_ERR && (LONG_PTR)lenRes > 0) {
            int outPos = 0;
            HRESULT hrItem = read_list_item_text(
                hwnd, CB_GETLBTEXT, sel, (LONG_PTR)lenRes, CB_ERR, &itemBuf, &bufCap, pBuffer, maxChars, &outPos
            );
            if (FAILED(hrItem)) {
                free(itemBuf);
                return hrItem;
            }
            if (outPos > 0) {
                free(itemBuf);
                return S_OK;
            }
        }
    }

    /* Enumerate all ComboBox dropdown items via CB_GETLBTEXT */
    DWORD_PTR countRes = 0;
    if (!safe_send_msg(hwnd, CB_GETCOUNT, 0, 0, &countRes)) {
        free(itemBuf);
        return hr;
    }
    LONG_PTR count = (LONG_PTR)countRes;
    if (count == CB_ERR || count <= 0) {
        free(itemBuf);
        pBuffer[0] = L'\0';
        return S_FALSE;
    }

    int outPos = 0;
    for (LONG_PTR i = 0; i < count && outPos < maxChars - 1; i++) {
        DWORD_PTR lenRes = 0;
        if (!safe_send_msg(hwnd, CB_GETLBTEXTLEN, (WPARAM)i, 0, &lenRes)) {
            continue;
        }
        LONG_PTR itemLen = (LONG_PTR)lenRes;
        if (itemLen == CB_ERR || itemLen < 0) {
            continue;
        }

        if (i > 0) {
            append_wchar(pBuffer, maxChars, &outPos, L'\n');
            if (outPos >= maxChars - 1) {
                break;
            }
        }
        if (itemLen == 0) {
            continue;
        }

        HRESULT hrItem = read_list_item_text(
            hwnd, CB_GETLBTEXT, i, itemLen, CB_ERR, &itemBuf, &bufCap, pBuffer, maxChars, &outPos
        );
        if (FAILED(hrItem)) {
            free(itemBuf);
            return hrItem;
        }
    }

    free(itemBuf);
    return (outPos > 0) ? S_OK : S_FALSE;
}

static BOOL is_target_process_32bit(HANDLE hProc) {
#if defined(_WIN64)
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        FARPROC proc = GetProcAddress(hKernel32, "IsWow64Process2");
        if (proc) {
            PFN_IsWow64Process2 pfnWow2 = NULL;
            memcpy(&pfnWow2, &proc, sizeof(pfnWow2));
            USHORT procMachine = 0;
            USHORT nativeMachine = 0;
            if (pfnWow2(hProc, &procMachine, &nativeMachine)) {
                return (procMachine != IMAGE_FILE_MACHINE_UNKNOWN) ? TRUE : FALSE;
            }
        }
    }

    BOOL isWow64 = FALSE;
    if (IsWow64Process(hProc, &isWow64)) {
        return isWow64 ? TRUE : FALSE;
    }
    return FALSE;
#else
    (void)hProc;
    return TRUE;
#endif
}

static HRESULT extract_listview_text(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    DWORD_PTR countRes = 0;
    if (!safe_send_msg(hwnd, LVM_GETITEMCOUNT, 0, 0, &countRes)) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    int itemCount = (int)(LONG_PTR)countRes;
    if (itemCount <= 0) {
        pBuffer[0] = L'\0';
        return S_FALSE;
    }

    DWORD targetPid = 0;
    GetWindowThreadProcessId(hwnd, &targetPid);
    if (targetPid == 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    int outPos = 0;
    if (targetPid == GetCurrentProcessId()) {
        WCHAR localText[CPOWER_LV_TEXT_MAX];
        for (int i = 0; i < itemCount && outPos < maxChars - 1; i++) {
            if (i > 0) {
                append_wchar(pBuffer, maxChars, &outPos, L'\n');
                if (outPos >= maxChars - 1) {
                    break;
                }
            }
            ZeroMemory(localText, sizeof(localText));
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = localText;
            lvi.cchTextMax = CPOWER_LV_TEXT_MAX;

            DWORD_PTR len = 0;
            if (safe_send_msg(hwnd, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)&lvi, &len) && len > 0) {
                localText[CPOWER_LV_TEXT_MAX - 1] = L'\0';
                append_wstr(pBuffer, maxChars, &outPos, localText, (int)len);
            }
        }
        return (outPos > 0) ? S_OK : S_FALSE;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
        FALSE,
        targetPid
    );
    if (!hProc) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_ACCESSDENIED;
    }

    BOOL targetIs32Bit = is_target_process_32bit(hProc);
    size_t itemStructSize = targetIs32Bit ? sizeof(CPOWER_LVITEMW32) : sizeof(LVITEMW);
    size_t textBytes = (size_t)CPOWER_LV_TEXT_MAX * sizeof(WCHAR);
    size_t remoteAllocSize = itemStructSize + textBytes;
    BYTE* remoteMem = (BYTE*)VirtualAllocEx(
        hProc, NULL, remoteAllocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );
    if (!remoteMem) {
        DWORD err = GetLastError();
        CloseHandle(hProc);
        return err ? HRESULT_FROM_WIN32(err) : E_OUTOFMEMORY;
    }

    WCHAR localText[CPOWER_LV_TEXT_MAX];
    BYTE zeroText[CPOWER_LV_TEXT_MAX * sizeof(WCHAR)];
    ZeroMemory(zeroText, sizeof(zeroText));

    for (int i = 0; i < itemCount && outPos < maxChars - 1; i++) {
        if (i > 0) {
            append_wchar(pBuffer, maxChars, &outPos, L'\n');
            if (outPos >= maxChars - 1) {
                break;
            }
        }

        BYTE* remoteText = remoteMem + itemStructSize;
        if (targetIs32Bit) {
            if ((uintptr_t)remoteText > (uintptr_t)UINT32_MAX) {
                break;
            }
            CPOWER_LVITEMW32 lvi32;
            ZeroMemory(&lvi32, sizeof(lvi32));
            lvi32.mask = LVIF_TEXT;
            lvi32.iItem = i;
            lvi32.iSubItem = 0;
            lvi32.pszText = (uint32_t)(uintptr_t)remoteText;
            lvi32.cchTextMax = CPOWER_LV_TEXT_MAX;

            if (!WriteProcessMemory(hProc, remoteMem, &lvi32, sizeof(lvi32), NULL) ||
                !WriteProcessMemory(hProc, remoteText, zeroText, textBytes, NULL)) {
                break;
            }
        } else {
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = (LPWSTR)remoteText;
            lvi.cchTextMax = CPOWER_LV_TEXT_MAX;

            if (!WriteProcessMemory(hProc, remoteMem, &lvi, sizeof(lvi), NULL) ||
                !WriteProcessMemory(hProc, remoteText, zeroText, textBytes, NULL)) {
                break;
            }
        }

        DWORD_PTR len = 0;
        if (safe_send_msg(hwnd, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)remoteMem, &len)) {
            ZeroMemory(localText, sizeof(localText));
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProc, remoteText, localText, textBytes, &bytesRead)) {
                localText[CPOWER_LV_TEXT_MAX - 1] = L'\0';
                int slen = (int)wcslen(localText);
                if (slen > 0) {
                    append_wstr(pBuffer, maxChars, &outPos, localText, slen);
                }
            }
        }
    }

    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return (outPos > 0) ? S_OK : S_FALSE;
}

HRESULT Perception_FallbackExtractText(HWND hwnd, WCHAR* pBuffer, int maxChars) {
    if (!pBuffer) {
        return E_POINTER;
    }
    if (maxChars <= 0) {
        return E_INVALIDARG;
    }
    pBuffer[0] = L'\0';

    if (!hwnd || !IsWindow(hwnd)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    if (maxChars == 1) {
        return S_OK;
    }

    WCHAR className[128];
    ZeroMemory(className, sizeof(className));
    GetClassNameW(hwnd, className, (int)(sizeof(className) / sizeof(className[0])));

    if (_wcsicmp(className, L"SysListView32") == 0) {
        return extract_listview_text(hwnd, pBuffer, maxChars);
    }
    if (_wcsicmp(className, L"ListBox") == 0) {
        return extract_listbox_text(hwnd, pBuffer, maxChars);
    }
    if (_wcsicmp(className, L"ComboBox") == 0) {
        return extract_combobox_text(hwnd, pBuffer, maxChars);
    }
    if (_wcsicmp(className, L"Edit") == 0 ||
        _wcsnicmp(className, L"RichEdit", 8) == 0 ||
        wcsstr(className, L"RICHEDIT") != NULL) {
        return extract_edit_text(hwnd, pBuffer, maxChars);
    }

    return extract_wm_gettext(hwnd, pBuffer, maxChars);
}

void Perception_PokeChromiumAccessibility(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    DWORD_PTR dwResult = 0;
    LRESULT ok = SendMessageTimeoutW(
        hwnd,
        WM_GETOBJECT,
        0,
        (LPARAM)OBJID_CLIENT,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        200,
        &dwResult
    );

    if (ok != 0) {
        HWND hRender = FindWindowExW(hwnd, NULL, L"Chrome_RenderWidgetHostHWND", NULL);
        if (hRender != NULL && hRender != hwnd && IsWindow(hRender)) {
            DWORD_PTR childRes = 0;
            SendMessageTimeoutW(
                hRender,
                WM_GETOBJECT,
                0,
                (LPARAM)OBJID_CLIENT,
                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                200,
                &childRes
            );
        }
    }
}
