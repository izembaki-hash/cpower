/**
 * @file main.c
 * @brief Standalone CLI & IPC Daemon Entry Point for cpower v1.1.0.
 *
 * Subcommands:
 *   tree    [--hwnd <hex|dec>] [--desktop] [--format indent|json|binary]
 *   find    [--hwnd <hex|dec>] [--desktop] [--name STR] [--id STR] [--class STR]
 *           [--type TYPE] [--enabled] [--visible] [--json]
 *   diff    [--hwnd <hex|dec>] [--desktop] [--delay <ms>] [--format json|text]
 *   windows [--json]
 *   focus   --hwnd <hex|dec> [--action focus|restore|minimize|maximize|close]
 *   events  [--duration <ms>]
 *   action  --cmd <click|double_click|right_click|move|drag|scroll|hscroll|
 *                  type|key|invoke|set_value|toggle|select|direct_click|direct_text>
 *           [--x N] [--y N] [--to-x N] [--to-y N] [--delta N] [--node ID]
 *           [--hwnd H] [--text STR] [--vk N] [--mods ctrl+shift]
 *           [--mode direct|human] [--speed F] [--verify]
 *   capture --left L --top T --right R --bottom B --out <path.bmp|path.png>
 *   hook    --pid <PID> [--inject] [--read] [--detach] [--duration <ms>] [--dll <path>]
 *   daemon  [--transport shm|pipe] [--name <name>] [--duration <ms>] [--stream-events]
 *   verify
 *   version
 *   --help
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

#include "cpower.h"
#include "common.h"
#include "perception.h"
#include "input.h"
#include "ipc.h"
#include "security.h"
#include "text_hook.h"

#ifndef CPOWER_EXEC_MODE_MASK
#define CPOWER_EXEC_MODE_MASK        0x00FFU
#endif
#ifndef CPOWER_EXEC_FLAG_HAS_NODE_ID
#define CPOWER_EXEC_FLAG_HAS_NODE_ID 0x0100U
#endif
#ifndef CPOWER_EXEC_FLAG_HAS_COORDS
#define CPOWER_EXEC_FLAG_HAS_COORDS  0x0200U
#endif

#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <ole2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomationclient.h>
#include <wchar.h>

/* -------------------------------------------------------------------------
 * Explicit UIA GUIDs (zero uuid.lib dependency)
 * ------------------------------------------------------------------------- */
static const GUID CPOWER_CLI_CLSID_CUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e }
};

static const GUID CPOWER_CLI_IID_IUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee }
};

/* -------------------------------------------------------------------------
 * Dynamic Win32 API Helpers (ISO C11 strict, no -Wcast-function-type)
 * ------------------------------------------------------------------------- */
#define CPOWER_DPI_AWARENESS_CONTEXT_PER_MONITOR_V2 ((HANDLE)(intptr_t)-4)

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE value);
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef BOOL (WINAPI *PFN_CancelSynchronousIo)(HANDLE hThread);

static void cli_enable_per_monitor_v2_dpi(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        hUser32 = LoadLibraryW(L"user32.dll");
    }
    if (!hUser32) {
        return;
    }

    FARPROC fpCtx = GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (fpCtx != NULL) {
        PFN_SetProcessDpiAwarenessContext pfnSetCtx = NULL;
        memcpy(&pfnSetCtx, &fpCtx, sizeof(pfnSetCtx));
        if (pfnSetCtx(CPOWER_DPI_AWARENESS_CONTEXT_PER_MONITOR_V2)) {
            return;
        }
    }

    FARPROC fpAware = GetProcAddress(hUser32, "SetProcessDPIAware");
    if (fpAware != NULL) {
        PFN_SetProcessDPIAware pfnSetAware = NULL;
        memcpy(&pfnSetAware, &fpAware, sizeof(pfnSetAware));
        (void)pfnSetAware();
    }
}

static UINT cli_get_system_dpi(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32 != NULL) {
        FARPROC fpDpi = GetProcAddress(hUser32, "GetDpiForSystem");
        if (fpDpi != NULL) {
            PFN_GetDpiForSystem pfnGetDpi = NULL;
            memcpy(&pfnGetDpi, &fpDpi, sizeof(pfnGetDpi));
            return pfnGetDpi();
        }
    }
    HDC hdc = GetDC(NULL);
    UINT dpi = 96U;
    if (hdc != NULL) {
        dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }
    return dpi;
}

static void cli_cancel_sync_io(HANDLE hThread) {
    if (!hThread) {
        return;
    }
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        return;
    }
    FARPROC fpCancel = GetProcAddress(hKernel32, "CancelSynchronousIo");
    if (fpCancel != NULL) {
        PFN_CancelSynchronousIo pfnCancel = NULL;
        memcpy(&pfnCancel, &fpCancel, sizeof(pfnCancel));
        (void)pfnCancel(hThread);
    }
}

/* -------------------------------------------------------------------------
 * Console Ctrl+C / Graceful Shutdown Handling
 * ------------------------------------------------------------------------- */
static volatile LONG g_stopRequested = 0;

static BOOL WINAPI cli_console_ctrl_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    }
    return FALSE;
}

/* -------------------------------------------------------------------------
 * Parsing & UTF-8 Formatting Helpers
 * ------------------------------------------------------------------------- */
static void cli_wide_to_utf8(const WCHAR* pszIn, char* pOut, size_t outCap) {
    if (!pOut || outCap == 0U) {
        return;
    }
    pOut[0] = '\0';
    if (!pszIn || pszIn[0] == L'\0') {
        return;
    }
    int written = WideCharToMultiByte(CP_UTF8, 0, pszIn, -1, pOut, (int)outCap, NULL, NULL);
    if (written <= 0) {
        pOut[outCap - 1U] = '\0';
    }
}

static void cli_fprint_json_escaped(FILE* fp, const char* pszUtf8) {
    fputc('"', fp);
    if (pszUtf8) {
        for (const unsigned char* p = (const unsigned char*)pszUtf8; *p != '\0'; ++p) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                fputc('\\', fp);
                fputc((int)c, fp);
            } else if (c == '\b') {
                fputs("\\b", fp);
            } else if (c == '\f') {
                fputs("\\f", fp);
            } else if (c == '\n') {
                fputs("\\n", fp);
            } else if (c == '\r') {
                fputs("\\r", fp);
            } else if (c == '\t') {
                fputs("\\t", fp);
            } else if (c < 0x20U) {
                fprintf(fp, "\\u%04x", (unsigned)c);
            } else {
                fputc((int)c, fp);
            }
        }
    }
    fputc('"', fp);
}

static void cli_fprint_json_wstr(FILE* fp, const WCHAR* pszIn) {
    char utf8[512];
    cli_wide_to_utf8(pszIn, utf8, sizeof(utf8));
    cli_fprint_json_escaped(fp, utf8);
}

static BOOL cli_parse_hwnd(const WCHAR* pszStr, HWND* pOutHwnd) {
    if (!pszStr || pszStr[0] == L'\0' || !pOutHwnd) {
        return FALSE;
    }
    WCHAR* endPtr = NULL;
    unsigned long long val = wcstoull(pszStr, &endPtr, 0);
    if (endPtr == pszStr || *endPtr != L'\0') {
        return FALSE;
    }
    *pOutHwnd = (HWND)(uintptr_t)val;
    return TRUE;
}

static BOOL cli_parse_int(const WCHAR* pszStr, int* pOutVal) {
    if (!pszStr || pszStr[0] == L'\0' || !pOutVal) {
        return FALSE;
    }
    WCHAR* endPtr = NULL;
    long val = wcstol(pszStr, &endPtr, 0);
    if (endPtr == pszStr || *endPtr != L'\0') {
        return FALSE;
    }
    *pOutVal = (int)val;
    return TRUE;
}

static BOOL cli_parse_dword(const WCHAR* pszStr, DWORD* pOutVal) {
    if (!pszStr || pszStr[0] == L'\0' || !pOutVal) {
        return FALSE;
    }
    if (pszStr[0] == L'-') {
        return FALSE;
    }
    WCHAR* endPtr = NULL;
    unsigned long val = wcstoul(pszStr, &endPtr, 0);
    if (endPtr == pszStr || *endPtr != L'\0') {
        return FALSE;
    }
    *pOutVal = (DWORD)val;
    return TRUE;
}

static BOOL cli_parse_double(const WCHAR* pszStr, double* pOutVal) {
    if (!pszStr || pszStr[0] == L'\0' || !pOutVal) {
        return FALSE;
    }
    WCHAR* endPtr = NULL;
    double val = wcstod(pszStr, &endPtr);
    if (endPtr == pszStr || *endPtr != L'\0') {
        return FALSE;
    }
    *pOutVal = val;
    return TRUE;
}

static uint16_t cli_parse_control_type(const WCHAR* pszType) {
    if (!pszType || pszType[0] == L'\0') {
        return 0U;
    }
    DWORD numVal = 0U;
    if (cli_parse_dword(pszType, &numVal) && numVal <= 0xFFFFU) {
        return (uint16_t)numVal;
    }
    if (_wcsicmp(pszType, L"button") == 0)       return UIA_ButtonControlTypeId;
    if (_wcsicmp(pszType, L"calendar") == 0)     return UIA_CalendarControlTypeId;
    if (_wcsicmp(pszType, L"checkbox") == 0)     return UIA_CheckBoxControlTypeId;
    if (_wcsicmp(pszType, L"combobox") == 0)     return UIA_ComboBoxControlTypeId;
    if (_wcsicmp(pszType, L"edit") == 0)         return UIA_EditControlTypeId;
    if (_wcsicmp(pszType, L"hyperlink") == 0)    return UIA_HyperlinkControlTypeId;
    if (_wcsicmp(pszType, L"image") == 0)        return UIA_ImageControlTypeId;
    if (_wcsicmp(pszType, L"listitem") == 0)     return UIA_ListItemControlTypeId;
    if (_wcsicmp(pszType, L"list") == 0)         return UIA_ListControlTypeId;
    if (_wcsicmp(pszType, L"menu") == 0)         return UIA_MenuControlTypeId;
    if (_wcsicmp(pszType, L"menubar") == 0)      return UIA_MenuBarControlTypeId;
    if (_wcsicmp(pszType, L"menuitem") == 0)     return UIA_MenuItemControlTypeId;
    if (_wcsicmp(pszType, L"progressbar") == 0)  return UIA_ProgressBarControlTypeId;
    if (_wcsicmp(pszType, L"radiobutton") == 0)  return UIA_RadioButtonControlTypeId;
    if (_wcsicmp(pszType, L"scrollbar") == 0)    return UIA_ScrollBarControlTypeId;
    if (_wcsicmp(pszType, L"slider") == 0)       return UIA_SliderControlTypeId;
    if (_wcsicmp(pszType, L"spinner") == 0)      return UIA_SpinnerControlTypeId;
    if (_wcsicmp(pszType, L"statusbar") == 0)    return UIA_StatusBarControlTypeId;
    if (_wcsicmp(pszType, L"tab") == 0)          return UIA_TabControlTypeId;
    if (_wcsicmp(pszType, L"tabitem") == 0)      return UIA_TabItemControlTypeId;
    if (_wcsicmp(pszType, L"text") == 0)         return UIA_TextControlTypeId;
    if (_wcsicmp(pszType, L"toolbar") == 0)      return UIA_ToolBarControlTypeId;
    if (_wcsicmp(pszType, L"tooltip") == 0)      return UIA_ToolTipControlTypeId;
    if (_wcsicmp(pszType, L"tree") == 0)         return UIA_TreeControlTypeId;
    if (_wcsicmp(pszType, L"treeitem") == 0)     return UIA_TreeItemControlTypeId;
    if (_wcsicmp(pszType, L"custom") == 0)       return UIA_CustomControlTypeId;
    if (_wcsicmp(pszType, L"group") == 0)        return UIA_GroupControlTypeId;
    if (_wcsicmp(pszType, L"thumb") == 0)        return UIA_ThumbControlTypeId;
    if (_wcsicmp(pszType, L"datagrid") == 0)     return UIA_DataGridControlTypeId;
    if (_wcsicmp(pszType, L"dataitem") == 0)     return UIA_DataItemControlTypeId;
    if (_wcsicmp(pszType, L"document") == 0)     return UIA_DocumentControlTypeId;
    if (_wcsicmp(pszType, L"splitbutton") == 0)  return UIA_SplitButtonControlTypeId;
    if (_wcsicmp(pszType, L"window") == 0)       return UIA_WindowControlTypeId;
    if (_wcsicmp(pszType, L"pane") == 0)         return UIA_PaneControlTypeId;
    if (_wcsicmp(pszType, L"header") == 0)       return UIA_HeaderControlTypeId;
    if (_wcsicmp(pszType, L"headeritem") == 0)   return UIA_HeaderItemControlTypeId;
    if (_wcsicmp(pszType, L"table") == 0)        return UIA_TableControlTypeId;
    if (_wcsicmp(pszType, L"titlebar") == 0)     return UIA_TitleBarControlTypeId;
    if (_wcsicmp(pszType, L"separator") == 0)    return UIA_SeparatorControlTypeId;
    return 0U;
}

static uint32_t cli_parse_modifiers(const WCHAR* pszMods) {
    if (!pszMods) {
        return KEY_MOD_NONE;
    }
    uint32_t mods = KEY_MOD_NONE;
    WCHAR copy[128];
    wcsncpy(copy, pszMods, 127);
    copy[127] = L'\0';

    WCHAR* ctx = NULL;
    WCHAR* tok = wcstok(copy, L"+,| ", &ctx);
    while (tok != NULL) {
        if (_wcsicmp(tok, L"ctrl") == 0 || _wcsicmp(tok, L"control") == 0) {
            mods |= KEY_MOD_CTRL;
        } else if (_wcsicmp(tok, L"shift") == 0) {
            mods |= KEY_MOD_SHIFT;
        } else if (_wcsicmp(tok, L"alt") == 0) {
            mods |= KEY_MOD_ALT;
        } else if (_wcsicmp(tok, L"win") == 0 || _wcsicmp(tok, L"super") == 0) {
            mods |= KEY_MOD_WIN;
        }
        tok = wcstok(NULL, L"+,| ", &ctx);
    }
    return mods;
}

static void cli_print_usage(void) {
    printf("cpower v%s - Pure C Windows-Native Computer-Use Agent\n\n", CPOWER_VERSION_STRING);
    printf("Usage: cpower <subcommand> [options]\n\n");
    printf("Subcommands:\n");
    printf("  tree     [--hwnd <hex|dec>] [--desktop] [--format indent|json|binary]\n");
    printf("           Extract and serialize semantic UI tree (default: foreground, indent)\n\n");
    printf("  find     [--hwnd <hex|dec>] [--desktop] [--name STR] [--id STR] [--class STR]\n");
    printf("           [--type TYPE] [--enabled] [--visible] [--json]\n");
    printf("           Locate matching UI elements by semantic attributes\n\n");
    printf("  diff     [--hwnd <hex|dec>] [--desktop] [--delay <ms>] [--format json|text]\n");
    printf("           Capture baseline -> wait -> capture second snapshot -> emit delta diff\n\n");
    printf("  windows  [--json]\n");
    printf("           Enumerate visible top-level windows\n\n");
    printf("  focus    --hwnd <hex|dec> [--action focus|restore|minimize|maximize|close]\n");
    printf("           Focus or control top-level window state\n\n");
    printf("  events   [--duration <ms>]\n");
    printf("           Stream real-time WinEvent notifications (default: 5000 ms)\n\n");
    printf("  action   --cmd <click|double_click|right_click|move|drag|scroll|hscroll|\n");
    printf("                  type|key|invoke|set_value|toggle|select|direct_click|direct_text>\n");
    printf("           [--x N] [--y N] [--to-x N] [--to-y N] [--delta N] [--node ID]\n");
    printf("           [--hwnd H] [--text STR] [--vk N] [--mods ctrl+shift]\n");
    printf("           [--mode direct|human] [--speed F] [--verify]\n\n");
    printf("  capture  --left L --top T --right R --bottom B --out <path.bmp|path.png>\n");
    printf("           Micro-region DXGI VRAM capture (multi-monitor stitched) as BMP or PNG\n\n");
    printf("  hook     --pid <PID> [--inject] [--read] [--detach] [--duration <ms>] [--dll <path>]\n");
    printf("           Opt-in GDI text-capture IAT hook control for a specific 64-bit process\n\n");
    printf("  daemon   [--transport shm|pipe] [--name <name>] [--duration <ms>] [--stream-events]\n");
    printf("           Listen for CommandPackets and stream back AckPackets, trees, diffs, windows, images\n\n");
    printf("  verify   Run comprehensive OS, COM/UIA, Control, Security/UIPI, and IPC diagnostics\n");
    printf("  version  Display cpower engine version\n");
    printf("  --help   Display this help message\n");
}

/* -------------------------------------------------------------------------
 * Shared Pure C COM UIA Element Resolver (by SemanticNode or Point/HWND)
 * ------------------------------------------------------------------------- */
typedef struct CliUiaDfsFrame {
    IUIAutomationElement* pElem;
    int                   depth;
} CliUiaDfsFrame;

static BOOL cli_uia_str_matches(const WCHAR* pszLive, const WCHAR* pszNodeField, size_t maxFieldChars) {
    if (!pszLive || !pszNodeField || pszNodeField[0] == L'\0') {
        return FALSE;
    }
    size_t nodeLen = wcslen(pszNodeField);
    if (nodeLen == maxFieldChars) {
        return (wcsncmp(pszLive, pszNodeField, maxFieldChars) == 0) ? TRUE : FALSE;
    }
    return (wcscmp(pszLive, pszNodeField) == 0) ? TRUE : FALSE;
}

static IUIAutomationElement* cli_find_uia_element_for_node(
    IUIAutomation* pAuto,
    IUIAutomationTreeWalker* pWalker,
    HWND rootHwnd,
    const SemanticNode* pTargetNode
) {
    if (!pAuto || !pWalker || !pTargetNode) {
        return NULL;
    }

    IUIAutomationElement* pRoot = NULL;
    HRESULT hr = rootHwnd
        ? IUIAutomation_ElementFromHandle(pAuto, (UIA_HWND)rootHwnd, &pRoot)
        : IUIAutomation_GetRootElement(pAuto, &pRoot);
    if (FAILED(hr) || !pRoot) {
        return NULL;
    }

    CliUiaDfsFrame stack[128];
    int top = 0;
    stack[top].pElem = pRoot;
    stack[top].depth = 0;
    top++;

    IUIAutomationElement* pBest = NULL;
    int bestScore = -1;
    int visited = 0;

    while (top > 0 && visited < 4096) {
        top--;
        IUIAutomationElement* pCurr = stack[top].pElem;
        int depth = stack[top].depth;
        visited++;

        CONTROLTYPEID ctype = 0;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentControlType(pCurr, &ctype)) &&
            (uint16_t)ctype == pTargetNode->controlTypeId) {
            int score = 1;
            BOOL autoMatch = FALSE;
            BOOL nameMatch = FALSE;

            if (pTargetNode->automationId[0] != L'\0') {
                BSTR bAuto = NULL;
                if (SUCCEEDED(IUIAutomationElement_get_CurrentAutomationId(pCurr, &bAuto)) && bAuto) {
                    if (cli_uia_str_matches(bAuto, pTargetNode->automationId, 63U)) {
                        score += 4;
                        autoMatch = TRUE;
                    }
                    SysFreeString(bAuto);
                }
            }
            if (pTargetNode->name[0] != L'\0') {
                BSTR bName = NULL;
                if (SUCCEEDED(IUIAutomationElement_get_CurrentName(pCurr, &bName)) && bName) {
                    if (cli_uia_str_matches(bName, pTargetNode->name, 127U)) {
                        score += 3;
                        nameMatch = TRUE;
                    }
                    SysFreeString(bName);
                }
            }
            RECT rc = { 0, 0, 0, 0 };
            if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(pCurr, &rc))) {
                int dl = abs((int)rc.left - (int)pTargetNode->bounds.left);
                int dt = abs((int)rc.top - (int)pTargetNode->bounds.top);
                int dr = abs((int)rc.right - (int)pTargetNode->bounds.right);
                int db = abs((int)rc.bottom - (int)pTargetNode->bounds.bottom);
                if (dl <= 4 && dt <= 4 && dr <= 4 && db <= 4) {
                    score += 3;
                }
            }

            BOOL acceptable = autoMatch || nameMatch ||
                (pTargetNode->name[0] == L'\0' && pTargetNode->automationId[0] == L'\0' && score >= 4);

            if (acceptable && score > bestScore) {
                CPOWER_SAFE_RELEASE(pBest);
                pBest = pCurr;
                IUIAutomationElement_AddRef(pBest);
                bestScore = score;
                if (bestScore >= 10 || (autoMatch && nameMatch && score >= 8)) {
                    IUIAutomationElement_Release(pCurr);
                    break;
                }
            }
        }

        if (depth < 32) {
            IUIAutomationElement* children[64];
            int childCount = 0;
            IUIAutomationElement* pChild = NULL;
            if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(pWalker, pCurr, &pChild)) && pChild) {
                while (pChild && childCount < 64) {
                    children[childCount++] = pChild;
                    IUIAutomationElement* pNext = NULL;
                    if (FAILED(IUIAutomationTreeWalker_GetNextSiblingElement(pWalker, pChild, &pNext))) {
                        pNext = NULL;
                    }
                    pChild = pNext;
                }
                if (pChild) {
                    IUIAutomationElement_Release(pChild);
                }
                for (int i = childCount - 1; i >= 0; --i) {
                    if (top < 128) {
                        stack[top].pElem = children[i];
                        stack[top].depth = depth + 1;
                        top++;
                    } else {
                        IUIAutomationElement_Release(children[i]);
                    }
                }
            }
        }

        IUIAutomationElement_Release(pCurr);
    }

    while (top > 0) {
        top--;
        if (stack[top].pElem) {
            IUIAutomationElement_Release(stack[top].pElem);
        }
    }

    return pBest;
}

/* -------------------------------------------------------------------------
 * Subcommand: tree
 * ------------------------------------------------------------------------- */
typedef enum TreeOutputFormat {
    TREE_FMT_INDENT = 0,
    TREE_FMT_JSON   = 1,
    TREE_FMT_BINARY = 2
} TreeOutputFormat;

static int cmd_tree(int argc, WCHAR* argv[]) {
    HWND targetHwnd = NULL;
    BOOL captureDesktop = FALSE;
    TreeOutputFormat fmt = TREE_FMT_INDENT;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            if (!cli_parse_hwnd(argv[++i], &targetHwnd)) {
                fprintf(stderr, "Invalid --hwnd value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--desktop") == 0) {
            captureDesktop = TRUE;
        } else if (wcscmp(argv[i], L"--format") == 0 && i + 1 < argc) {
            const WCHAR* f = argv[++i];
            if (_wcsicmp(f, L"indent") == 0 || _wcsicmp(f, L"compact") == 0) {
                fmt = TREE_FMT_INDENT;
            } else if (_wcsicmp(f, L"json") == 0) {
                fmt = TREE_FMT_JSON;
            } else if (_wcsicmp(f, L"binary") == 0) {
                fmt = TREE_FMT_BINARY;
            } else {
                fprintf(stderr, "Unknown --format value. Expected indent, json, or binary.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'tree': %ls\n", argv[i]);
            return 1;
        }
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    HRESULT hr = Perception_Init();
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_Init failed: 0x%08lX\n", (unsigned long)hr);
        CPower_Shutdown();
        return 1;
    }

    SemanticTree* pTree = NULL;
    hr = captureDesktop
        ? Perception_CaptureDesktopTree(&pTree)
        : Perception_CaptureSemanticTree(targetHwnd, &pTree);

    if (FAILED(hr) || !pTree) {
        fprintf(stderr, "Semantic tree capture failed: 0x%08lX\n", (unsigned long)hr);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    size_t bufCap = 512U * 1024U;
    if (fmt == TREE_FMT_BINARY) {
        size_t minNeeded = sizeof(TreePacketHeader) + (size_t)pTree->nodeCount * sizeof(SemanticNode) + 4096U;
        if (minNeeded > bufCap) {
            bufCap = minNeeded;
        }
    }

    uint8_t* pOut = NULL;
    size_t bytesWritten = 0U;

    while (bufCap <= 32U * 1024U * 1024U) {
        uint8_t* pNew = (uint8_t*)realloc(pOut, bufCap);
        if (!pNew) {
            hr = E_OUTOFMEMORY;
            break;
        }
        pOut = pNew;
        bytesWritten = 0U;

        if (fmt == TREE_FMT_BINARY) {
            hr = Perception_SerializeTree(pTree, pOut, bufCap, &bytesWritten);
        } else if (fmt == TREE_FMT_JSON) {
            hr = Perception_SerializeTreeJson(pTree, (char*)pOut, bufCap, &bytesWritten);
        } else {
            hr = Perception_SerializeTreeIndented(pTree, (char*)pOut, bufCap, &bytesWritten);
        }

        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            break;
        }
        bufCap = (bytesWritten > bufCap) ? bytesWritten : (bufCap * 2U);
    }

    int exitCode = 0;
    if (SUCCEEDED(hr) && pOut != NULL) {
        if (fmt == TREE_FMT_BINARY) {
            (void)_setmode(_fileno(stdout), _O_BINARY);
            if (bytesWritten > 0U) {
                fwrite(pOut, 1U, bytesWritten, stdout);
            }
        } else if (bytesWritten > 0U) {
            size_t printLen = bytesWritten;
            if (((char*)pOut)[printLen - 1U] == '\0') {
                printLen--;
            }
            if (printLen > 0U) {
                fwrite(pOut, 1U, printLen, stdout);
                if (((char*)pOut)[printLen - 1U] != '\n') {
                    fputc('\n', stdout);
                }
            }
        }
        fflush(stdout);
    } else {
        fprintf(stderr, "Tree serialization failed: 0x%08lX\n", (unsigned long)hr);
        exitCode = 1;
    }

    free(pOut);
    Perception_FreeSemanticTree(pTree);
    Perception_Shutdown();
    CPower_Shutdown();
    return exitCode;
}

/* -------------------------------------------------------------------------
 * Subcommand: find
 * ------------------------------------------------------------------------- */
static int cmd_find(int argc, WCHAR* argv[]) {
    HWND targetHwnd = NULL;
    BOOL captureDesktop = FALSE;
    BOOL jsonOut = FALSE;
    SemanticQuery query;
    memset(&query, 0, sizeof(query));

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            if (!cli_parse_hwnd(argv[++i], &targetHwnd)) {
                fprintf(stderr, "Invalid --hwnd value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--desktop") == 0) {
            captureDesktop = TRUE;
        } else if (wcscmp(argv[i], L"--name") == 0 && i + 1 < argc) {
            wcsncpy(query.nameSubstr, argv[++i], 127);
            query.nameSubstr[127] = L'\0';
        } else if (wcscmp(argv[i], L"--id") == 0 && i + 1 < argc) {
            wcsncpy(query.automationId, argv[++i], 63);
            query.automationId[63] = L'\0';
        } else if (wcscmp(argv[i], L"--class") == 0 && i + 1 < argc) {
            wcsncpy(query.className, argv[++i], 63);
            query.className[63] = L'\0';
        } else if (wcscmp(argv[i], L"--type") == 0 && i + 1 < argc) {
            const WCHAR* t = argv[++i];
            query.controlTypeId = cli_parse_control_type(t);
            if (query.controlTypeId == 0U) {
                fprintf(stderr, "Unknown --type '%ls'.\n", t);
                return 1;
            }
        } else if (wcscmp(argv[i], L"--enabled") == 0) {
            query.onlyEnabled = TRUE;
        } else if (wcscmp(argv[i], L"--visible") == 0) {
            query.onlyVisible = TRUE;
        } else if (wcscmp(argv[i], L"--json") == 0) {
            jsonOut = TRUE;
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'find': %ls\n", argv[i]);
            return 1;
        }
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    HRESULT hr = Perception_Init();
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_Init failed: 0x%08lX\n", (unsigned long)hr);
        CPower_Shutdown();
        return 1;
    }

    SemanticTree* pTree = NULL;
    hr = captureDesktop
        ? Perception_CaptureDesktopTree(&pTree)
        : Perception_CaptureSemanticTree(targetHwnd, &pTree);
    if (FAILED(hr) || !pTree) {
        fprintf(stderr, "Semantic tree capture failed: 0x%08lX\n", (unsigned long)hr);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    uint32_t findCap = (pTree->nodeCount > 0U) ? pTree->nodeCount : 1U;
    SemanticMatch* matches = (SemanticMatch*)calloc(findCap, sizeof(SemanticMatch));
    if (!matches) {
        Perception_FreeSemanticTree(pTree);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    uint32_t matchCount = 0U;
    hr = Perception_FindNodes(pTree, &query, matches, findCap, &matchCount);
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_FindNodes failed: 0x%08lX\n", (unsigned long)hr);
        free(matches);
        Perception_FreeSemanticTree(pTree);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    uint32_t emitCount = (matchCount > findCap) ? findCap : matchCount;
    if (jsonOut) {
        printf("{\"rootHwnd\":\"0x%llX\",\"totalMatches\":%u,\"returnedCount\":%u,\"matches\":[",
               (unsigned long long)(uintptr_t)pTree->rootHwnd,
               (unsigned)matchCount,
               (unsigned)emitCount);
        for (uint32_t i = 0; i < emitCount; ++i) {
            const SemanticMatch* m = &matches[i];
            if (i > 0U) {
                putchar(',');
            }
            printf("{\"nodeId\":%u,\"type\":\"%s\",\"controlTypeId\":%u,\"center\":[%ld,%ld],"
                   "\"bounds\":[%ld,%ld,%ld,%ld],\"flags\":%u,\"name\":",
                   (unsigned)m->nodeId,
                   Perception_ControlTypeIdToString(m->node.controlTypeId),
                   (unsigned)m->node.controlTypeId,
                   (long)m->center.x, (long)m->center.y,
                   (long)m->node.bounds.left, (long)m->node.bounds.top,
                   (long)m->node.bounds.right, (long)m->node.bounds.bottom,
                   (unsigned)m->node.flags);
            cli_fprint_json_wstr(stdout, m->node.name);
            fputs(",\"automationId\":", stdout);
            cli_fprint_json_wstr(stdout, m->node.automationId);
            fputs(",\"className\":", stdout);
            cli_fprint_json_wstr(stdout, m->node.className);
            fputs(",\"value\":", stdout);
            cli_fprint_json_wstr(stdout, m->node.value);
            putchar('}');
        }
        printf("]}\n");
    } else {
        printf("[find] root=0x%p matches=%u\n", (void*)pTree->rootHwnd, (unsigned)matchCount);
        for (uint32_t i = 0; i < emitCount; ++i) {
            const SemanticMatch* m = &matches[i];
            char nameUtf8[256];
            char idUtf8[128];
            char clsUtf8[128];
            cli_wide_to_utf8(m->node.name, nameUtf8, sizeof(nameUtf8));
            cli_wide_to_utf8(m->node.automationId, idUtf8, sizeof(idUtf8));
            cli_wide_to_utf8(m->node.className, clsUtf8, sizeof(clsUtf8));
            printf("  [%u] %s name=\"%s\" id=\"%s\" class=\"%s\" center=(%ld,%ld) bounds=[%ld,%ld,%ld,%ld]\n",
                   (unsigned)m->nodeId,
                   Perception_ControlTypeIdToString(m->node.controlTypeId),
                   nameUtf8, idUtf8, clsUtf8,
                   (long)m->center.x, (long)m->center.y,
                   (long)m->node.bounds.left, (long)m->node.bounds.top,
                   (long)m->node.bounds.right, (long)m->node.bounds.bottom);
        }
    }
    fflush(stdout);

    free(matches);
    Perception_FreeSemanticTree(pTree);
    Perception_Shutdown();
    CPower_Shutdown();
    return 0;
}

/* -------------------------------------------------------------------------
 * Subcommand: diff
 * ------------------------------------------------------------------------- */
static int cmd_diff(int argc, WCHAR* argv[]) {
    HWND targetHwnd = NULL;
    BOOL captureDesktop = FALSE;
    DWORD delayMs = 500U;
    BOOL jsonOut = TRUE;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            if (!cli_parse_hwnd(argv[++i], &targetHwnd)) {
                fprintf(stderr, "Invalid --hwnd value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--desktop") == 0) {
            captureDesktop = TRUE;
        } else if (wcscmp(argv[i], L"--delay") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &delayMs)) {
                fprintf(stderr, "Invalid --delay value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--format") == 0 && i + 1 < argc) {
            const WCHAR* f = argv[++i];
            if (_wcsicmp(f, L"json") == 0) {
                jsonOut = TRUE;
            } else if (_wcsicmp(f, L"text") == 0) {
                jsonOut = FALSE;
            } else {
                fprintf(stderr, "Invalid --format value. Expected json or text.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'diff': %ls\n", argv[i]);
            return 1;
        }
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    if (FAILED(Perception_Init())) {
        fprintf(stderr, "Perception_Init failed.\n");
        CPower_Shutdown();
        return 1;
    }

    SemanticTree* pPrev = NULL;
    SemanticTree* pCurr = NULL;
    SemanticTreeDiff* pDiff = NULL;

    HRESULT hr = captureDesktop
        ? Perception_CaptureDesktopTree(&pPrev)
        : Perception_CaptureSemanticTree(targetHwnd, &pPrev);
    if (FAILED(hr) || !pPrev) {
        fprintf(stderr, "Initial semantic tree capture failed: 0x%08lX\n", (unsigned long)hr);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    if (delayMs > 0U) {
        Sleep(delayMs);
    }

    HWND secondHwnd = targetHwnd ? targetHwnd : pPrev->rootHwnd;
    hr = captureDesktop
        ? Perception_CaptureDesktopTree(&pCurr)
        : Perception_CaptureSemanticTree(secondHwnd, &pCurr);
    if (FAILED(hr) || !pCurr) {
        fprintf(stderr, "Second semantic tree capture failed: 0x%08lX\n", (unsigned long)hr);
        Perception_FreeSemanticTree(pPrev);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    hr = Perception_DiffTrees(pPrev, pCurr, &pDiff);
    if (FAILED(hr) || !pDiff) {
        fprintf(stderr, "Perception_DiffTrees failed: 0x%08lX\n", (unsigned long)hr);
        Perception_FreeSemanticTree(pCurr);
        Perception_FreeSemanticTree(pPrev);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    int exitCode = 0;
    if (jsonOut) {
        size_t cap = 65536U + (size_t)pDiff->entryCount * 1024U;
        char* pJson = (char*)malloc(cap);
        size_t written = 0U;
        if (!pJson) {
            exitCode = 1;
        } else {
            hr = Perception_SerializeDiffJson(pDiff, pJson, cap, &written);
            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && written > cap) {
                char* pBigger = (char*)realloc(pJson, written + 64U);
                if (pBigger) {
                    pJson = pBigger;
                    cap = written + 64U;
                    hr = Perception_SerializeDiffJson(pDiff, pJson, cap, &written);
                }
            }
            if (SUCCEEDED(hr) && written > 0U) {
                size_t len = (pJson[written - 1U] == '\0') ? (written - 1U) : written;
                fwrite(pJson, 1U, len, stdout);
                fputc('\n', stdout);
            } else {
                fprintf(stderr, "Perception_SerializeDiffJson failed: 0x%08lX\n", (unsigned long)hr);
                exitCode = 1;
            }
            free(pJson);
        }
    } else {
        printf("[diff] root=0x%p entries=%u (added=%u, removed=%u, modified=%u)\n",
               (void*)pDiff->rootHwnd,
               (unsigned)pDiff->entryCount,
               (unsigned)pDiff->addedCount,
               (unsigned)pDiff->removedCount,
               (unsigned)pDiff->modifiedCount);
        for (uint32_t i = 0; i < pDiff->entryCount; ++i) {
            const TreeDiffEntry* e = &pDiff->entries[i];
            const char* opStr = (e->op == DIFF_OP_ADDED) ? "ADDED" :
                                ((e->op == DIFF_OP_REMOVED) ? "REMOVED" : "MODIFIED");
            char nameUtf8[256];
            char valUtf8[256];
            cli_wide_to_utf8(e->node.name, nameUtf8, sizeof(nameUtf8));
            cli_wide_to_utf8(e->node.value, valUtf8, sizeof(valUtf8));
            printf("  %s [%u] %s name=\"%s\" val=\"%s\" mask=0x%X\n",
                   opStr,
                   (unsigned)e->nodeId,
                   Perception_ControlTypeIdToString(e->node.controlTypeId),
                   nameUtf8, valUtf8, (unsigned)e->changedMask);
        }
    }
    fflush(stdout);

    Perception_FreeTreeDiff(pDiff);
    Perception_FreeSemanticTree(pCurr);
    Perception_FreeSemanticTree(pPrev);
    Perception_Shutdown();
    CPower_Shutdown();
    return exitCode;
}

/* -------------------------------------------------------------------------
 * Subcommand: windows
 * ------------------------------------------------------------------------- */
static int cmd_windows(int argc, WCHAR* argv[]) {
    BOOL jsonOut = FALSE;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--json") == 0) {
            jsonOut = TRUE;
        } else {
            fprintf(stderr, "Unknown option in 'windows': %ls\n", argv[i]);
            return 1;
        }
    }

    uint32_t winCap = 256U;
    WindowInfo* wins = (WindowInfo*)calloc(winCap, sizeof(WindowInfo));
    if (!wins) {
        return 1;
    }

    uint32_t count = 0U;
    HRESULT hr = Control_EnumWindows(wins, winCap, &count);
    if (SUCCEEDED(hr) && count > winCap) {
        uint32_t biggerCap = count + 32U;
        WindowInfo* pBigger = (WindowInfo*)realloc(wins, (size_t)biggerCap * sizeof(WindowInfo));
        if (pBigger) {
            wins = pBigger;
            winCap = biggerCap;
            memset(wins, 0, (size_t)winCap * sizeof(WindowInfo));
            hr = Control_EnumWindows(wins, winCap, &count);
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "Control_EnumWindows failed: 0x%08lX\n", (unsigned long)hr);
        free(wins);
        return 1;
    }

    uint32_t emit = (count > winCap) ? winCap : count;
    if (jsonOut) {
        printf("{\"count\":%u,\"totalWindows\":%u,\"returnedCount\":%u,\"windows\":[",
               (unsigned)count, (unsigned)count, (unsigned)emit);
        for (uint32_t i = 0; i < emit; ++i) {
            const WindowInfo* w = &wins[i];
            if (i > 0U) {
                putchar(',');
            }
            printf("{\"hwnd\":\"0x%llX\",\"hwndInt\":%llu,\"pid\":%u,\"visible\":%s,\"minimized\":%s,"
                   "\"rect\":[%ld,%ld,%ld,%ld],\"title\":",
                   (unsigned long long)w->hwnd,
                   (unsigned long long)w->hwnd,
                   (unsigned)w->pid,
                   w->isVisible ? "true" : "false",
                   w->isMinimized ? "true" : "false",
                   (long)w->rect.left, (long)w->rect.top, (long)w->rect.right, (long)w->rect.bottom);
            cli_fprint_json_wstr(stdout, w->title);
            fputs(",\"className\":", stdout);
            cli_fprint_json_wstr(stdout, w->className);
            putchar('}');
        }
        printf("]}\n");
    } else {
        printf("[windows] Visible top-level windows: %u\n", (unsigned)count);
        for (uint32_t i = 0; i < emit; ++i) {
            const WindowInfo* w = &wins[i];
            char titleUtf8[256];
            char clsUtf8[128];
            cli_wide_to_utf8(w->title, titleUtf8, sizeof(titleUtf8));
            cli_wide_to_utf8(w->className, clsUtf8, sizeof(clsUtf8));
            printf("  hwnd=0x%llX pid=%u min=%u [%ld,%ld,%ld,%ld] class=\"%s\" title=\"%s\"\n",
                   (unsigned long long)w->hwnd,
                   (unsigned)w->pid,
                   (unsigned)w->isMinimized,
                   (long)w->rect.left, (long)w->rect.top, (long)w->rect.right, (long)w->rect.bottom,
                   clsUtf8, titleUtf8);
        }
    }
    fflush(stdout);

    free(wins);
    return 0;
}

/* -------------------------------------------------------------------------
 * Subcommand: focus
 * ------------------------------------------------------------------------- */
static int cmd_focus(int argc, WCHAR* argv[]) {
    HWND targetHwnd = NULL;
    const WCHAR* pszAction = L"focus";

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            if (!cli_parse_hwnd(argv[++i], &targetHwnd)) {
                fprintf(stderr, "Invalid --hwnd value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--action") == 0 && i + 1 < argc) {
            pszAction = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'focus': %ls\n", argv[i]);
            return 1;
        }
    }

    if (!targetHwnd) {
        fprintf(stderr, "focus requires --hwnd <hex|dec>.\n");
        return 1;
    }

    HRESULT hr = E_INVALIDARG;
    if (_wcsicmp(pszAction, L"focus") == 0) {
        hr = Control_FocusWindow(targetHwnd);
    } else if (_wcsicmp(pszAction, L"restore") == 0) {
        hr = Control_ShowWindow(targetHwnd, WND_RESTORE);
    } else if (_wcsicmp(pszAction, L"minimize") == 0) {
        hr = Control_ShowWindow(targetHwnd, WND_MINIMIZE);
    } else if (_wcsicmp(pszAction, L"maximize") == 0) {
        hr = Control_ShowWindow(targetHwnd, WND_MAXIMIZE);
    } else if (_wcsicmp(pszAction, L"close") == 0) {
        hr = Control_ShowWindow(targetHwnd, WND_CLOSE);
    } else {
        fprintf(stderr, "Unknown --action '%ls'. Expected focus, restore, minimize, maximize, or close.\n", pszAction);
        return 1;
    }

    if (SUCCEEDED(hr)) {
        printf("[focus] hwnd=0x%p action=%ls status=OK (0x%08lX)\n",
               (void*)targetHwnd, pszAction, (unsigned long)hr);
        return 0;
    }
    fprintf(stderr, "[focus] hwnd=0x%p action=%ls status=FAILED (0x%08lX)\n",
            (void*)targetHwnd, pszAction, (unsigned long)hr);
    return 1;
}

/* -------------------------------------------------------------------------
 * Subcommand: events
 * ------------------------------------------------------------------------- */
static CRITICAL_SECTION g_eventPrintCs;
static volatile LONG    g_eventCount = 0;

static const char* cli_winevent_name(DWORD evt) {
    switch (evt) {
        case EVENT_SYSTEM_FOREGROUND:       return "FOREGROUND";
        case EVENT_SYSTEM_MENUSTART:        return "MENUSTART";
        case EVENT_SYSTEM_MENUEND:          return "MENUEND";
        case EVENT_SYSTEM_MENUPOPUPSTART:   return "MENUPOPUPSTART";
        case EVENT_SYSTEM_MENUPOPUPEND:     return "MENUPOPUPEND";
        case EVENT_SYSTEM_MOVESIZESTART:    return "MOVESIZESTART";
        case EVENT_SYSTEM_MOVESIZEEND:      return "MOVESIZEEND";
        case EVENT_SYSTEM_MINIMIZESTART:    return "MINIMIZESTART";
        case EVENT_SYSTEM_MINIMIZEEND:      return "MINIMIZEEND";
        case EVENT_OBJECT_CREATE:           return "OBJECT_CREATE";
        case EVENT_OBJECT_DESTROY:          return "OBJECT_DESTROY";
        case EVENT_OBJECT_SHOW:             return "OBJECT_SHOW";
        case EVENT_OBJECT_HIDE:             return "OBJECT_HIDE";
        case EVENT_OBJECT_FOCUS:            return "OBJECT_FOCUS";
        case EVENT_OBJECT_SELECTION:        return "OBJECT_SELECTION";
        case EVENT_OBJECT_STATECHANGE:      return "OBJECT_STATECHANGE";
        case EVENT_OBJECT_LOCATIONCHANGE:   return "OBJECT_LOCATIONCHANGE";
        case EVENT_OBJECT_NAMECHANGE:       return "OBJECT_NAMECHANGE";
        case EVENT_OBJECT_VALUECHANGE:      return "OBJECT_VALUECHANGE";
        default:                            return "EVENT";
    }
}

static void cli_event_callback(const WinEventRecord* pEvent, void* pUserData) {
    (void)pUserData;
    if (!pEvent) {
        return;
    }
    InterlockedIncrement(&g_eventCount);
    EnterCriticalSection(&g_eventPrintCs);
    printf("[EVENT] code=0x%04lX (%s) hwnd=0x%p obj=%ld child=%ld tid=%lu ts=%lu\n",
           (unsigned long)pEvent->event,
           cli_winevent_name(pEvent->event),
           (void*)pEvent->hwnd,
           (long)pEvent->idObject,
           (long)pEvent->idChild,
           (unsigned long)pEvent->threadId,
           (unsigned long)pEvent->timestamp);
    fflush(stdout);
    LeaveCriticalSection(&g_eventPrintCs);
}

static int cmd_events(int argc, WCHAR* argv[]) {
    DWORD durationMs = 5000U;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--duration") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &durationMs)) {
                fprintf(stderr, "Invalid --duration value.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'events': %ls\n", argv[i]);
            return 1;
        }
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    HRESULT hr = Perception_Init();
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_Init failed: 0x%08lX\n", (unsigned long)hr);
        CPower_Shutdown();
        return 1;
    }

    InitializeCriticalSection(&g_eventPrintCs);
    InterlockedExchange(&g_eventCount, 0);
    InterlockedExchange(&g_stopRequested, 0);
    SetConsoleCtrlHandler(cli_console_ctrl_handler, TRUE);

    hr = Perception_StartEventHook(cli_event_callback, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_StartEventHook failed: 0x%08lX\n", (unsigned long)hr);
        SetConsoleCtrlHandler(cli_console_ctrl_handler, FALSE);
        DeleteCriticalSection(&g_eventPrintCs);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    printf("[events] Listening for WinEvents (%lu ms)...\n", (unsigned long)durationMs);
    fflush(stdout);

    ULONGLONG startMs = GetTickCount64();
    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
        ULONGLONG elapsed = GetTickCount64() - startMs;
        if (elapsed >= (ULONGLONG)durationMs) {
            break;
        }
        ULONGLONG rem = (ULONGLONG)durationMs - elapsed;
        Sleep(rem > 50ULL ? 50U : (DWORD)rem);
    }

    Perception_StopEventHook();
    SetConsoleCtrlHandler(cli_console_ctrl_handler, FALSE);

    LONG captured = InterlockedCompareExchange(&g_eventCount, 0, 0);
    printf("[events] Stopped. Total events received: %ld\n", (long)captured);

    DeleteCriticalSection(&g_eventPrintCs);
    Perception_Shutdown();
    CPower_Shutdown();
    return 0;
}

/* -------------------------------------------------------------------------
 * Subcommand: action
 * ------------------------------------------------------------------------- */
typedef struct CliActionCtx {
    const WCHAR* pszCmd;
    int          x;
    int          y;
    int          toX;
    int          toY;
    int          delta;
    BOOL         hasX;
    BOOL         hasY;
    BOOL         hasTo;
    BOOL         hasNode;
    uint32_t     nodeId;
    HWND         hwnd;
    const WCHAR* pszText;
    DWORD        vk;
    uint32_t     mods;
    ControlMode  mode;
    double       speed;
    SemanticTree* pTree;
} CliActionCtx;

static HRESULT cli_execute_single_action(CliActionCtx* pCtx) {
    if (!pCtx || !pCtx->pszCmd) {
        return E_INVALIDARG;
    }

    int effX = pCtx->x;
    int effY = pCtx->y;
    BOOL haveEffXY = pCtx->hasX || pCtx->hasY;

    if (!haveEffXY && pCtx->hasNode && pCtx->pTree && pCtx->nodeId < pCtx->pTree->nodeCount) {
        const RECT* r = &pCtx->pTree->nodes[pCtx->nodeId].bounds;
        effX = (r->left + r->right) / 2;
        effY = (r->top + r->bottom) / 2;
        haveEffXY = TRUE;
    }

    const WCHAR* cmd = pCtx->pszCmd;
    if (_wcsicmp(cmd, L"click") == 0) {
        if (pCtx->mode == CONTROL_MODE_DIRECT_INSTANT) {
            if (!pCtx->hwnd) {
                fprintf(stderr, "Direct click requires --hwnd.\n");
                return E_INVALIDARG;
            }
            if (pCtx->hasNode && !pCtx->hasX && !pCtx->hasY && haveEffXY) {
                POINT pt = { effX, effY };
                ScreenToClient(pCtx->hwnd, &pt);
                return Control_DirectClickHwnd(pCtx->hwnd, pt.x, pt.y);
            }
            if (pCtx->hasX || pCtx->hasY) {
                return Control_DirectClickHwnd(pCtx->hwnd, pCtx->x, pCtx->y);
            }
            return Control_DirectClickButtonHwnd(pCtx->hwnd);
        }
        if (!haveEffXY) {
            fprintf(stderr, "Command 'click' in human mode requires --x/--y or --node.\n");
            return E_INVALIDARG;
        }
        return Control_EmulateMouseClick(effX, effY, MOUSE_BTN_LEFT);
    }
    if (_wcsicmp(cmd, L"double_click") == 0) {
        if (!haveEffXY) {
            fprintf(stderr, "Command 'double_click' requires --x/--y or --node.\n");
            return E_INVALIDARG;
        }
        return Control_EmulateMouseDoubleClick(effX, effY, MOUSE_BTN_LEFT);
    }
    if (_wcsicmp(cmd, L"right_click") == 0) {
        if (!haveEffXY) {
            fprintf(stderr, "Command 'right_click' requires --x/--y or --node.\n");
            return E_INVALIDARG;
        }
        return Control_EmulateMouseClick(effX, effY, MOUSE_BTN_RIGHT);
    }
    if (_wcsicmp(cmd, L"move") == 0) {
        if (!haveEffXY) {
            fprintf(stderr, "Command 'move' requires --x/--y or --node.\n");
            return E_INVALIDARG;
        }
        return Control_EmulateMouseMove(effX, effY, pCtx->speed);
    }
    if (_wcsicmp(cmd, L"drag") == 0) {
        if (!haveEffXY || !pCtx->hasTo) {
            fprintf(stderr, "Command 'drag' requires --x/--y (or --node) and --to-x/--to-y.\n");
            return E_INVALIDARG;
        }
        return Control_EmulateMouseDrag(effX, effY, pCtx->toX, pCtx->toY, pCtx->speed);
    }
    if (_wcsicmp(cmd, L"scroll") == 0 || _wcsicmp(cmd, L"hscroll") == 0) {
        BOOL horiz = (_wcsicmp(cmd, L"hscroll") == 0);
        int sx = haveEffXY ? effX : INT_MIN;
        int sy = haveEffXY ? effY : INT_MIN;
        return Control_EmulateMouseScroll(sx, sy, pCtx->delta, horiz);
    }
    if (_wcsicmp(cmd, L"type") == 0) {
        if (!pCtx->pszText) {
            fprintf(stderr, "Command 'type' requires --text.\n");
            return E_INVALIDARG;
        }
        if (pCtx->mode == CONTROL_MODE_DIRECT_INSTANT) {
            if (!pCtx->hwnd) {
                fprintf(stderr, "Direct text requires --hwnd.\n");
                return E_INVALIDARG;
            }
            return Control_DirectSetTextHwnd(pCtx->hwnd, pCtx->pszText);
        }
        return Control_EmulateTypeText(pCtx->pszText, pCtx->speed);
    }
    if (_wcsicmp(cmd, L"key") == 0) {
        if (pCtx->vk == 0U || pCtx->vk > 0xFFFFU) {
            fprintf(stderr, "Command 'key' requires valid --vk (1..65535).\n");
            return E_INVALIDARG;
        }
        return Control_EmulateSendKey((WORD)pCtx->vk, pCtx->mods);
    }
    if (_wcsicmp(cmd, L"direct_click") == 0) {
        if (!pCtx->hwnd) {
            fprintf(stderr, "Command 'direct_click' requires --hwnd.\n");
            return E_INVALIDARG;
        }
        if (pCtx->hasX || pCtx->hasY) {
            return Control_DirectClickHwnd(pCtx->hwnd, pCtx->x, pCtx->y);
        }
        return Control_DirectClickButtonHwnd(pCtx->hwnd);
    }
    if (_wcsicmp(cmd, L"direct_text") == 0) {
        if (!pCtx->hwnd || !pCtx->pszText) {
            fprintf(stderr, "Command 'direct_text' requires --hwnd and --text.\n");
            return E_INVALIDARG;
        }
        return Control_DirectSetTextHwnd(pCtx->hwnd, pCtx->pszText);
    }
    if (_wcsicmp(cmd, L"invoke") == 0 ||
        _wcsicmp(cmd, L"set_value") == 0 ||
        _wcsicmp(cmd, L"toggle") == 0 ||
        _wcsicmp(cmd, L"select") == 0) {
        if (_wcsicmp(cmd, L"set_value") == 0 && !pCtx->pszText) {
            fprintf(stderr, "Command 'set_value' requires --text.\n");
            return E_INVALIDARG;
        }
        IUIAutomation* pAuto = NULL;
        IUIAutomationTreeWalker* pWalker = NULL;
        HRESULT hr = CoCreateInstance(
            &CPOWER_CLI_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
            &CPOWER_CLI_IID_IUIAutomation, (void**)&pAuto);
        if (FAILED(hr) || !pAuto) {
            return FAILED(hr) ? hr : E_FAIL;
        }
        (void)IUIAutomation_get_ControlViewWalker(pAuto, &pWalker);

        IUIAutomationElement* pElem = NULL;
        if (pCtx->hasNode && pCtx->pTree && pCtx->nodeId < pCtx->pTree->nodeCount && pWalker) {
            HWND root = pCtx->hwnd ? pCtx->hwnd : pCtx->pTree->rootHwnd;
            pElem = cli_find_uia_element_for_node(pAuto, pWalker, root, &pCtx->pTree->nodes[pCtx->nodeId]);
        }
        if (!pElem) {
            if (haveEffXY) {
                POINT pt = { effX, effY };
                hr = IUIAutomation_ElementFromPoint(pAuto, pt, &pElem);
            } else if (pCtx->hwnd) {
                hr = IUIAutomation_ElementFromHandle(pAuto, (UIA_HWND)pCtx->hwnd, &pElem);
            } else {
                hr = E_INVALIDARG;
            }
        } else {
            hr = S_OK;
        }

        if (SUCCEEDED(hr) && pElem) {
            if (_wcsicmp(cmd, L"invoke") == 0) {
                hr = Control_DirectInvoke(pElem);
            } else if (_wcsicmp(cmd, L"set_value") == 0) {
                hr = Control_DirectSetValue(pElem, pCtx->pszText);
            } else if (_wcsicmp(cmd, L"toggle") == 0) {
                enum ToggleState st = ToggleState_Off;
                hr = Control_DirectToggle(pElem, &st);
            } else {
                hr = Control_DirectSelect(pElem, FALSE);
            }
        }
        CPOWER_SAFE_RELEASE(pElem);
        CPOWER_SAFE_RELEASE(pWalker);
        CPOWER_SAFE_RELEASE(pAuto);
        return hr;
    }

    fprintf(stderr, "Unknown --cmd '%ls'.\n", cmd);
    return E_INVALIDARG;
}

static HRESULT cli_action_verify_callback(void* pUserData) {
    return cli_execute_single_action((CliActionCtx*)pUserData);
}

static int cmd_action(int argc, WCHAR* argv[]) {
    CliActionCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.delta = -120;
    ctx.mode = CONTROL_MODE_HUMAN_EMULATION;
    ctx.speed = 1.0;

    BOOL hasMode = FALSE;
    BOOL hasSpeed = FALSE;
    BOOL doVerify = FALSE;
    BOOL hasToX = FALSE;
    BOOL hasToY = FALSE;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--cmd") == 0 && i + 1 < argc) {
            ctx.pszCmd = argv[++i];
        } else if (wcscmp(argv[i], L"--x") == 0 && i + 1 < argc) {
            if (!cli_parse_int(argv[++i], &ctx.x)) { fprintf(stderr, "Invalid --x integer.\n"); return 1; }
            ctx.hasX = TRUE;
        } else if (wcscmp(argv[i], L"--y") == 0 && i + 1 < argc) {
            if (!cli_parse_int(argv[++i], &ctx.y)) { fprintf(stderr, "Invalid --y integer.\n"); return 1; }
            ctx.hasY = TRUE;
        } else if (wcscmp(argv[i], L"--to-x") == 0 && i + 1 < argc) {
            if (!cli_parse_int(argv[++i], &ctx.toX)) { fprintf(stderr, "Invalid --to-x integer.\n"); return 1; }
            hasToX = TRUE;
        } else if (wcscmp(argv[i], L"--to-y") == 0 && i + 1 < argc) {
            if (!cli_parse_int(argv[++i], &ctx.toY)) { fprintf(stderr, "Invalid --to-y integer.\n"); return 1; }
            hasToY = TRUE;
        } else if (wcscmp(argv[i], L"--delta") == 0 && i + 1 < argc) {
            if (!cli_parse_int(argv[++i], &ctx.delta)) { fprintf(stderr, "Invalid --delta integer.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--node") == 0 && i + 1 < argc) {
            DWORD nId = 0U;
            if (!cli_parse_dword(argv[++i], &nId)) { fprintf(stderr, "Invalid --node ID.\n"); return 1; }
            ctx.nodeId = (uint32_t)nId;
            ctx.hasNode = TRUE;
        } else if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            if (!cli_parse_hwnd(argv[++i], &ctx.hwnd)) { fprintf(stderr, "Invalid --hwnd value.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--text") == 0 && i + 1 < argc) {
            ctx.pszText = argv[++i];
        } else if (wcscmp(argv[i], L"--vk") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &ctx.vk)) { fprintf(stderr, "Invalid --vk value.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--mods") == 0 && i + 1 < argc) {
            ctx.mods = cli_parse_modifiers(argv[++i]);
        } else if (wcscmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
            const WCHAR* m = argv[++i];
            hasMode = TRUE;
            if (_wcsicmp(m, L"direct") == 0) {
                ctx.mode = CONTROL_MODE_DIRECT_INSTANT;
            } else if (_wcsicmp(m, L"human") == 0 || _wcsicmp(m, L"emulated") == 0) {
                ctx.mode = CONTROL_MODE_HUMAN_EMULATION;
            } else {
                fprintf(stderr, "Invalid --mode value. Expected direct or human.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--speed") == 0 && i + 1 < argc) {
            if (!cli_parse_double(argv[++i], &ctx.speed) || ctx.speed <= 0.0) {
                fprintf(stderr, "Invalid --speed multiplier (must be > 0).\n");
                return 1;
            }
            hasSpeed = TRUE;
        } else if (wcscmp(argv[i], L"--verify") == 0) {
            doVerify = TRUE;
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'action': %ls\n", argv[i]);
            return 1;
        }
    }

    ctx.hasTo = (hasToX && hasToY);

    if (!ctx.pszCmd) {
        fprintf(stderr, "Missing required --cmd argument.\n");
        return 1;
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }

    BOOL needPerception = (ctx.hasNode || doVerify ||
        _wcsicmp(ctx.pszCmd, L"invoke") == 0 ||
        _wcsicmp(ctx.pszCmd, L"set_value") == 0 ||
        _wcsicmp(ctx.pszCmd, L"toggle") == 0 ||
        _wcsicmp(ctx.pszCmd, L"select") == 0);

    if (needPerception && FAILED(Perception_Init())) {
        fprintf(stderr, "Perception_Init failed.\n");
        CPower_Shutdown();
        return 1;
    }
    HRESULT hr = Control_Init();
    if (FAILED(hr)) {
        fprintf(stderr, "Control_Init failed: 0x%08lX\n", (unsigned long)hr);
        if (needPerception) Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    if (hasSpeed || hasMode) {
        ControlConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.defaultMode = ctx.mode;
        cfg.trajectory.speedScale = hasSpeed ? ctx.speed : 1.0;
        cfg.trajectory.tremorScale = 1.0;
        cfg.trajectory.curvatureScale = 1.0;
        cfg.trajectory.ouTheta = 15.0;
        cfg.trajectory.ouSigma = 2.5;
        cfg.trajectory.minSteps = 10;
        cfg.sendMessageTimeoutMs = 1000U;
        cfg.typingWpm = 65.0;
        (void)Control_Configure(&cfg);
    }

    if (ctx.hasNode) {
        hr = Perception_CaptureSemanticTree(ctx.hwnd, &ctx.pTree);
        if (FAILED(hr) || !ctx.pTree || ctx.nodeId >= ctx.pTree->nodeCount) {
            fprintf(stderr, "Failed to resolve --node %u (tree nodes=%u).\n",
                    (unsigned)ctx.nodeId,
                    ctx.pTree ? (unsigned)ctx.pTree->nodeCount : 0U);
            if (ctx.pTree) Perception_FreeSemanticTree(ctx.pTree);
            Control_Shutdown();
            if (needPerception) Perception_Shutdown();
            CPower_Shutdown();
            return 1;
        }
    }

    if (doVerify) {
        HWND verifyHwnd = ctx.hwnd ? ctx.hwnd : (ctx.pTree ? ctx.pTree->rootHwnd : NULL);
        SemanticTreeDiff* pDiff = NULL;
        hr = Control_ExecuteAndVerify(verifyHwnd, cli_action_verify_callback, &ctx, 1500U, &pDiff);
        if (SUCCEEDED(hr)) {
            printf("[action] cmd=%ls status=%s (0x%08lX) diffEntries=%u (added=%u, removed=%u, modified=%u)\n",
                   ctx.pszCmd,
                   (hr == S_OK) ? "VERIFIED_CHANGE" : "NO_UI_DIFF",
                   (unsigned long)hr,
                   pDiff ? (unsigned)pDiff->entryCount : 0U,
                   pDiff ? (unsigned)pDiff->addedCount : 0U,
                   pDiff ? (unsigned)pDiff->removedCount : 0U,
                   pDiff ? (unsigned)pDiff->modifiedCount : 0U);
        } else {
            fprintf(stderr, "[action] cmd=%ls status=FAILED (0x%08lX)\n", ctx.pszCmd, (unsigned long)hr);
        }
        if (pDiff) {
            Perception_FreeTreeDiff(pDiff);
        }
    } else {
        hr = cli_execute_single_action(&ctx);
        if (SUCCEEDED(hr)) {
            printf("[action] cmd=%ls status=OK (0x%08lX)\n", ctx.pszCmd, (unsigned long)hr);
        } else {
            fprintf(stderr, "[action] cmd=%ls status=FAILED (0x%08lX)\n", ctx.pszCmd, (unsigned long)hr);
        }
    }

    if (ctx.pTree) {
        Perception_FreeSemanticTree(ctx.pTree);
    }
    Control_Shutdown();
    if (needPerception) {
        Perception_Shutdown();
    }
    CPower_Shutdown();
    return SUCCEEDED(hr) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Subcommand: capture
 * ------------------------------------------------------------------------- */
static BOOL cli_ends_with_png(const WCHAR* path) {
    if (!path) {
        return FALSE;
    }
    size_t len = wcslen(path);
    if (len < 4U) {
        return FALSE;
    }
    return (_wcsicmp(path + len - 4U, L".png") == 0) ? TRUE : FALSE;
}

static int cmd_capture(int argc, WCHAR* argv[]) {
    int left = 0, top = 0, right = 0, bottom = 0;
    BOOL hasL = FALSE, hasT = FALSE, hasR = FALSE, hasB = FALSE;
    const WCHAR* pszOut = NULL;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--left") == 0 && i + 1 < argc) {
            hasL = cli_parse_int(argv[++i], &left);
            if (!hasL) { fprintf(stderr, "Invalid --left.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--top") == 0 && i + 1 < argc) {
            hasT = cli_parse_int(argv[++i], &top);
            if (!hasT) { fprintf(stderr, "Invalid --top.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--right") == 0 && i + 1 < argc) {
            hasR = cli_parse_int(argv[++i], &right);
            if (!hasR) { fprintf(stderr, "Invalid --right.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--bottom") == 0 && i + 1 < argc) {
            hasB = cli_parse_int(argv[++i], &bottom);
            if (!hasB) { fprintf(stderr, "Invalid --bottom.\n"); return 1; }
        } else if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) {
            pszOut = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'capture': %ls\n", argv[i]);
            return 1;
        }
    }

    if (!hasL || !hasT || !hasR || !hasB || !pszOut || right <= left || bottom <= top) {
        fprintf(stderr, "capture requires --left L --top T --right R --bottom B (R>L, B>T) and --out <path>.\n");
        return 1;
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    HRESULT hr = Perception_Init();
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_Init failed: 0x%08lX\n", (unsigned long)hr);
        CPower_Shutdown();
        return 1;
    }

    ImageBuffer img;
    memset(&img, 0, sizeof(img));
    hr = Perception_CaptureMicroRegion(left, top, right, bottom, &img);
    if (FAILED(hr)) {
        fprintf(stderr, "Perception_CaptureMicroRegion failed: 0x%08lX\n", (unsigned long)hr);
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    BOOL asPng = cli_ends_with_png(pszOut);
    size_t outCap = img.dataSize + (size_t)img.height * 16U + 4096U;
    uint8_t* pEncoded = NULL;
    size_t encodedBytes = 0U;

    for (int attempt = 0; attempt < 4; ++attempt) {
        uint8_t* pNew = (uint8_t*)realloc(pEncoded, outCap);
        if (!pNew) {
            hr = E_OUTOFMEMORY;
            break;
        }
        pEncoded = pNew;
        encodedBytes = 0U;
        hr = asPng
            ? Perception_EncodePngInMemory(&img, pEncoded, outCap, &encodedBytes)
            : Perception_EncodeBmpInMemory(&img, pEncoded, outCap, &encodedBytes);
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            break;
        }
        outCap *= 2U;
    }

    int exitCode = 0;
    if (SUCCEEDED(hr) && pEncoded != NULL && encodedBytes > 0U) {
        HANDLE hFile = CreateFileW(pszOut, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Failed to create output file '%ls' (err=%lu).\n",
                    pszOut, (unsigned long)GetLastError());
            exitCode = 1;
        } else {
            DWORD written = 0U;
            if (!WriteFile(hFile, pEncoded, (DWORD)encodedBytes, &written, NULL) ||
                written != (DWORD)encodedBytes) {
                fprintf(stderr, "Failed to write output file '%ls'.\n", pszOut);
                exitCode = 1;
            } else {
                printf("[capture] Saved %s (%dx%d, %zu bytes) -> %ls\n",
                       asPng ? "PNG" : "BMP",
                       img.width, img.height, encodedBytes, pszOut);
            }
            CloseHandle(hFile);
        }
    } else {
        fprintf(stderr, "Image encoding failed: 0x%08lX\n", (unsigned long)hr);
        exitCode = 1;
    }

    free(pEncoded);
    Perception_FreeImageBuffer(&img);
    Perception_Shutdown();
    CPower_Shutdown();
    return exitCode;
}

/* -------------------------------------------------------------------------
 * Subcommand: hook (Opt-in GDI Text Capture IAT Hook)
 * ------------------------------------------------------------------------- */
static const char* cli_hook_api_name(uint32_t apiKind) {
    switch (apiKind) {
        case HOOK_API_EXTTEXTOUTW: return "ExtTextOutW";
        case HOOK_API_TEXTOUTW:    return "TextOutW";
        case HOOK_API_DRAWTEXTW:   return "DrawTextW";
        case HOOK_API_DRAWTEXTEXW: return "DrawTextExW";
        default:                   return "Unknown";
    }
}

static int cmd_hook(int argc, WCHAR* argv[]) {
    DWORD pid = 0U;
    BOOL doInject = FALSE;
    BOOL doRead = FALSE;
    BOOL doDetach = FALSE;
    DWORD durationMs = 0U;
    const WCHAR* pszDll = NULL;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &pid) || pid == 0U) {
                fprintf(stderr, "Invalid --pid value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--inject") == 0) {
            doInject = TRUE;
        } else if (wcscmp(argv[i], L"--read") == 0) {
            doRead = TRUE;
        } else if (wcscmp(argv[i], L"--detach") == 0) {
            doDetach = TRUE;
        } else if (wcscmp(argv[i], L"--duration") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &durationMs)) {
                fprintf(stderr, "Invalid --duration value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            pszDll = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'hook': %ls\n", argv[i]);
            return 1;
        }
    }

    if (pid == 0U || (!doInject && !doRead && !doDetach)) {
        fprintf(stderr, "hook requires --pid <PID> and at least one of --inject, --read, --detach.\n");
        return 1;
    }

    if (doInject) {
        HRESULT hr = Hook_InjectProcess(pid, pszDll);
        if (FAILED(hr)) {
            fprintf(stderr, "[hook] Hook_InjectProcess(%lu) failed: 0x%08lX\n",
                    (unsigned long)pid, (unsigned long)hr);
            return 1;
        }
        printf("[hook] Injected cpower_hook64.dll into PID %lu\n", (unsigned long)pid);
        fflush(stdout);
    }

    if (doRead) {
        CPowerTextHookEntry entries[CPOWER_TEXT_HOOK_RING_CAP];
        uint64_t lastSeq = 0U;
        ULONGLONG startMs = GetTickCount64();

        do {
            uint32_t count = 0U;
            HRESULT hr = Hook_ReadCapturedText(pid, entries, CPOWER_TEXT_HOOK_RING_CAP, &count, &lastSeq);
            if (FAILED(hr)) {
                fprintf(stderr, "[hook] Hook_ReadCapturedText(%lu) failed: 0x%08lX\n",
                        (unsigned long)pid, (unsigned long)hr);
                if (doDetach) {
                    (void)Hook_DetachProcess(pid, 3000U);
                }
                return 1;
            }
            for (uint32_t i = 0; i < count; ++i) {
                char textUtf8[512];
                cli_wide_to_utf8(entries[i].text, textUtf8, sizeof(textUtf8));
                printf("[HOOK seq=%llu api=%s tid=%u pos=(%d,%d) rect=[%ld,%ld,%ld,%ld]] \"%s\"\n",
                       (unsigned long long)entries[i].seq,
                       cli_hook_api_name(entries[i].apiKind),
                       (unsigned)entries[i].tid,
                       (int)entries[i].x, (int)entries[i].y,
                       (long)entries[i].rect.left, (long)entries[i].rect.top,
                       (long)entries[i].rect.right, (long)entries[i].rect.bottom,
                       textUtf8);
            }
            fflush(stdout);
            if (durationMs == 0U || (GetTickCount64() - startMs) >= (ULONGLONG)durationMs) {
                break;
            }
            Sleep(50U);
        } while (TRUE);
    }

    if (doDetach) {
        HRESULT hr = Hook_DetachProcess(pid, 3000U);
        if (FAILED(hr)) {
            fprintf(stderr, "[hook] Hook_DetachProcess(%lu) failed: 0x%08lX\n",
                    (unsigned long)pid, (unsigned long)hr);
            return 1;
        }
        printf("[hook] Detached from PID %lu (status=0x%08lX)\n",
               (unsigned long)pid, (unsigned long)hr);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Subcommand: daemon (v1.1.0 Node-ID Resolution, ACK + Payload, Event Stream)
 * ------------------------------------------------------------------------- */
#define CPOWER_MAX_PIPE_PAYLOAD_BYTES (16U * 1024U * 1024U)

typedef struct DaemonWatchdogArgs {
    HANDLE        hTargetThread;
    DWORD         durationMs;
    volatile LONG targetDone;
} DaemonWatchdogArgs;

typedef struct DaemonState {
    IUIAutomation*           pAuto;
    IUIAutomationTreeWalker* pWalker;
    SemanticTree*            pLastTree;
    HWND                     lastTreeHwnd;
    BOOL                     lastTreeDesktop;
    BOOL                     streamEvents;
    uint32_t                 diffSeq;
} DaemonState;

static DWORD WINAPI daemon_watchdog_thread(LPVOID lpParam) {
    DaemonWatchdogArgs* pArgs = (DaemonWatchdogArgs*)lpParam;
    if (!pArgs) {
        return 0;
    }
    ULONGLONG startMs = GetTickCount64();
    while (InterlockedCompareExchange(&pArgs->targetDone, 0, 0) == 0) {
        if (InterlockedCompareExchange(&g_stopRequested, 0, 0) != 0) {
            cli_cancel_sync_io(pArgs->hTargetThread);
        } else if (pArgs->durationMs > 0U) {
            ULONGLONG elapsed = GetTickCount64() - startMs;
            if (elapsed >= (ULONGLONG)pArgs->durationMs) {
                InterlockedExchange(&g_stopRequested, 1);
                cli_cancel_sync_io(pArgs->hTargetThread);
            }
        }
        Sleep(25U);
    }
    return 0;
}

static HRESULT daemon_send_mouse_btn_at(int x, int y, DWORD flags) {
    LONG normX = 0;
    LONG normY = 0;
    Control_ScreenToVirtualDesktop(x, y, &normX, &normY);

    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = normX;
    inp.mi.dy = normY;
    inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flags;
    if (SendInput(1, &inp, sizeof(INPUT)) != 1) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

static HRESULT daemon_send_vk_state(WORD vk, BOOL keyUp) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    inp.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0U;
    if (SendInput(1, &inp, sizeof(INPUT)) != 1) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

static BOOL daemon_resolve_node_center(
    const DaemonState* pState,
    const CommandPacket* pCmd,
    BOOL hasNodeId,
    int* pOutX,
    int* pOutY
) {
    if (!pState || !pCmd || !pOutX || !pOutY || !hasNodeId) {
        return FALSE;
    }
    if (pState->pLastTree && pCmd->targetNodeId < pState->pLastTree->nodeCount) {
        const RECT* r = &pState->pLastTree->nodes[pCmd->targetNodeId].bounds;
        *pOutX = (r->left + r->right) / 2;
        *pOutY = (r->top + r->bottom) / 2;
        return TRUE;
    }
    return FALSE;
}

static HRESULT daemon_resolve_uia_element(DaemonState* pState, const CommandPacket* pCmd, IUIAutomationElement** ppOut) {
    if (!pState || !pState->pAuto || !pCmd || !ppOut) {
        return E_INVALIDARG;
    }
    *ppOut = NULL;

    BOOL hasNodeId = ((pCmd->executionMode & CPOWER_EXEC_FLAG_HAS_NODE_ID) != 0U) || (pCmd->targetNodeId > 0U);
    BOOL hasCoords = ((pCmd->executionMode & CPOWER_EXEC_FLAG_HAS_COORDS) != 0U) || (pCmd->targetX != 0 || pCmd->targetY != 0);

    if (pState->pLastTree && hasNodeId && pCmd->targetNodeId < pState->pLastTree->nodeCount) {
        const SemanticNode* pNode = &pState->pLastTree->nodes[pCmd->targetNodeId];
        HWND rootHwnd = pCmd->targetHwnd ? (HWND)(uintptr_t)pCmd->targetHwnd : pState->pLastTree->rootHwnd;
        if (pState->pWalker) {
            IUIAutomationElement* pFound = cli_find_uia_element_for_node(
                pState->pAuto, pState->pWalker, rootHwnd, pNode);
            if (pFound) {
                *ppOut = pFound;
                return S_OK;
            }
        }
        /* Geometric center fallback from last captured node bounds (including (0, 0)) */
        POINT ptNode;
        ptNode.x = (pNode->bounds.left + pNode->bounds.right) / 2;
        ptNode.y = (pNode->bounds.top + pNode->bounds.bottom) / 2;
        if (SUCCEEDED(IUIAutomation_ElementFromPoint(pState->pAuto, ptNode, ppOut)) && *ppOut) {
            return S_OK;
        }
    }

    if (hasCoords) {
        POINT pt = { pCmd->targetX, pCmd->targetY };
        return IUIAutomation_ElementFromPoint(pState->pAuto, pt, ppOut);
    }
    if (pCmd->targetHwnd != 0ULL) {
        return IUIAutomation_ElementFromHandle(pState->pAuto, (UIA_HWND)(uintptr_t)pCmd->targetHwnd, ppOut);
    }
    return E_INVALIDARG;
}

static HRESULT daemon_dispatch_command(const CommandPacket* pCmd, DaemonState* pState) {
    if (!pCmd || !pState) {
        return E_INVALIDARG;
    }

    WCHAR textBuf[256];
    memset(textBuf, 0, sizeof(textBuf));
    size_t maxCopyBytes = pCmd->textPayloadBytes;
    if (maxCopyBytes > sizeof(textBuf) - sizeof(WCHAR)) {
        maxCopyBytes = sizeof(textBuf) - sizeof(WCHAR);
    }
    if (maxCopyBytes > 0U) {
        memcpy(textBuf, pCmd->textPayload, maxCopyBytes);
    }
    textBuf[255] = L'\0';

    uint16_t execMode = (uint16_t)(pCmd->executionMode & CPOWER_EXEC_MODE_MASK);
    BOOL hasNodeId = ((pCmd->executionMode & CPOWER_EXEC_FLAG_HAS_NODE_ID) != 0U) || (pCmd->targetNodeId > 0U);
    BOOL hasCoords = ((pCmd->executionMode & CPOWER_EXEC_FLAG_HAS_COORDS) != 0U) || (pCmd->targetX != 0 || pCmd->targetY != 0);

    double speed = (pCmd->speedFactor > 0.0f) ? (double)pCmd->speedFactor : 1.0;
    HWND targetHwnd = (HWND)(uintptr_t)pCmd->targetHwnd;
    MouseButton btn = (pCmd->param1 >= 0 && pCmd->param1 <= 2) ? (MouseButton)pCmd->param1 : MOUSE_BTN_LEFT;

    int effX = pCmd->targetX;
    int effY = pCmd->targetY;
    BOOL resolvedFromNode = FALSE;
    if (!hasCoords) {
        resolvedFromNode = daemon_resolve_node_center(pState, pCmd, hasNodeId, &effX, &effY);
    }

    switch ((IPC_CommandCode)pCmd->commandCode) {
        case CMD_NOP:
            return S_OK;

        case CMD_CLICK:
            if (execMode == EXEC_MODE_DIRECT_INSTANT && targetHwnd != NULL) {
                if (resolvedFromNode) {
                    POINT pt = { effX, effY };
                    ScreenToClient(targetHwnd, &pt);
                    return Control_DirectClickHwnd(targetHwnd, pt.x, pt.y);
                }
                if (hasCoords) {
                    return Control_DirectClickHwnd(targetHwnd, pCmd->targetX, pCmd->targetY);
                }
                return Control_DirectClickButtonHwnd(targetHwnd);
            }
            return Control_EmulateMouseClick(effX, effY, btn);

        case CMD_DOUBLE_CLICK:
            return Control_EmulateMouseDoubleClick(effX, effY, btn);

        case CMD_RIGHT_CLICK:
            return Control_EmulateMouseClick(effX, effY, MOUSE_BTN_RIGHT);

        case CMD_MOUSE_DOWN: {
            DWORD flag = (btn == MOUSE_BTN_RIGHT) ? MOUSEEVENTF_RIGHTDOWN :
                         ((btn == MOUSE_BTN_MIDDLE) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN);
            return daemon_send_mouse_btn_at(effX, effY, flag);
        }

        case CMD_MOUSE_UP: {
            DWORD flag = (btn == MOUSE_BTN_RIGHT) ? MOUSEEVENTF_RIGHTUP :
                         ((btn == MOUSE_BTN_MIDDLE) ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP);
            return daemon_send_mouse_btn_at(effX, effY, flag);
        }

        case CMD_MOUSE_MOVE:
            return Control_EmulateMouseMove(effX, effY, speed);

        case CMD_DRAG:
            return Control_EmulateMouseDrag(effX, effY, pCmd->param1, pCmd->param2, speed);

        case CMD_KEY_DOWN:
            return daemon_send_vk_state((WORD)pCmd->param1, FALSE);

        case CMD_KEY_UP:
            return daemon_send_vk_state((WORD)pCmd->param1, TRUE);

        case CMD_TYPE_TEXT:
            if (execMode == EXEC_MODE_DIRECT_INSTANT && targetHwnd != NULL) {
                return Control_DirectSetTextHwnd(targetHwnd, textBuf);
            }
            return Control_EmulateTypeText(textBuf, speed);

        case CMD_HOTKEY:
            return Control_EmulateSendKey((WORD)pCmd->param1, (uint32_t)pCmd->param2);

        case CMD_SCROLL:
        case CMD_HSCROLL: {
            BOOL horiz = ((IPC_CommandCode)pCmd->commandCode == CMD_HSCROLL) || (pCmd->param2 != 0);
            BOOL skipMove = (pCmd->targetX == INT_MIN || pCmd->targetY == INT_MIN) || (!hasCoords && !resolvedFromNode);
            int sx = skipMove ? INT_MIN : effX;
            int sy = skipMove ? INT_MIN : effY;
            return Control_EmulateMouseScroll(sx, sy, pCmd->param1, horiz);
        }

        case CMD_UIA_INVOKE: {
            IUIAutomationElement* pElem = NULL;
            HRESULT hr = daemon_resolve_uia_element(pState, pCmd, &pElem);
            if (SUCCEEDED(hr) && pElem) {
                hr = Control_DirectInvoke(pElem);
                CPOWER_SAFE_RELEASE(pElem);
            }
            return hr;
        }

        case CMD_UIA_SET_VALUE: {
            IUIAutomationElement* pElem = NULL;
            HRESULT hr = daemon_resolve_uia_element(pState, pCmd, &pElem);
            if (SUCCEEDED(hr) && pElem) {
                hr = Control_DirectSetValue(pElem, textBuf);
                CPOWER_SAFE_RELEASE(pElem);
            }
            return hr;
        }

        case CMD_UIA_TOGGLE: {
            IUIAutomationElement* pElem = NULL;
            HRESULT hr = daemon_resolve_uia_element(pState, pCmd, &pElem);
            if (SUCCEEDED(hr) && pElem) {
                enum ToggleState state = ToggleState_Off;
                hr = Control_DirectToggle(pElem, &state);
                CPOWER_SAFE_RELEASE(pElem);
            }
            return hr;
        }

        case CMD_UIA_SELECT: {
            IUIAutomationElement* pElem = NULL;
            HRESULT hr = daemon_resolve_uia_element(pState, pCmd, &pElem);
            if (SUCCEEDED(hr) && pElem) {
                hr = Control_DirectSelect(pElem, pCmd->param1 != 0);
                CPOWER_SAFE_RELEASE(pElem);
            }
            return hr;
        }

        case CMD_WIN32_CLICK:
            if (hasCoords) {
                return Control_DirectClickHwnd(targetHwnd, pCmd->targetX, pCmd->targetY);
            }
            if (resolvedFromNode && targetHwnd != NULL) {
                POINT pt = { effX, effY };
                ScreenToClient(targetHwnd, &pt);
                return Control_DirectClickHwnd(targetHwnd, pt.x, pt.y);
            }
            return Control_DirectClickButtonHwnd(targetHwnd);

        case CMD_WIN32_SET_TEXT:
            return Control_DirectSetTextHwnd(targetHwnd, textBuf);

        case CMD_FOCUS_WINDOW:
            return Control_FocusWindow(targetHwnd);

        case CMD_SHOW_WINDOW:
            return Control_ShowWindow(targetHwnd, (WindowShowAction)pCmd->param1);

        case CMD_CAPTURE_MICRO_REGION:
        case CMD_CAPTURE_TREE:
        case CMD_DIFF_TREE:
        case CMD_LIST_WINDOWS:
        case CMD_SHUTDOWN:
            return S_OK;

        default:
            return E_NOTIMPL;
    }
}

static HRESULT daemon_capture_and_serialize_tree(
    DaemonState* pState,
    HWND targetHwnd,
    BOOL desktopMode,
    uint8_t** ppBuf,
    size_t* pBufCap,
    size_t* pBytesOut,
    uint32_t* pNodeCountOut
) {
    *pBytesOut = 0U;
    if (pNodeCountOut) {
        *pNodeCountOut = 0U;
    }

    SemanticTree* pTree = NULL;
    HRESULT hr = desktopMode
        ? Perception_CaptureDesktopTree(&pTree)
        : Perception_CaptureSemanticTree(targetHwnd, &pTree);
    if (FAILED(hr) || !pTree) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    size_t minNeeded = sizeof(TreePacketHeader) + (size_t)pTree->nodeCount * sizeof(SemanticNode) + 4096U;
    if (*pBufCap < minNeeded) {
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, minNeeded);
        if (!pNew) {
            Perception_FreeSemanticTree(pTree);
            return E_OUTOFMEMORY;
        }
        *ppBuf = pNew;
        *pBufCap = minNeeded;
    }

    while (*pBufCap <= 32U * 1024U * 1024U) {
        size_t written = 0U;
        hr = Perception_SerializeTree(pTree, *ppBuf, *pBufCap, &written);
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            *pBytesOut = written;
            break;
        }
        size_t nextCap = (*pBufCap) * 2U;
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, nextCap);
        if (!pNew) {
            hr = E_OUTOFMEMORY;
            break;
        }
        *ppBuf = pNew;
        *pBufCap = nextCap;
    }

    if (SUCCEEDED(hr)) {
        if (pNodeCountOut) {
            *pNodeCountOut = pTree->nodeCount;
        }
        if (pState->pLastTree) {
            Perception_FreeSemanticTree(pState->pLastTree);
        }
        pState->pLastTree = pTree;
        pState->lastTreeHwnd = targetHwnd;
        pState->lastTreeDesktop = desktopMode;
    } else {
        Perception_FreeSemanticTree(pTree);
    }
    return hr;
}

static HRESULT daemon_build_diff_packet(
    DaemonState* pState,
    HWND targetHwnd,
    BOOL desktopMode,
    uint8_t** ppBuf,
    size_t* pBufCap,
    size_t* pBytesOut,
    uint32_t* pNodeCountOut,
    uint32_t* pDiffCountOut
) {
    *pBytesOut = 0U;
    if (pNodeCountOut) *pNodeCountOut = 0U;
    if (pDiffCountOut) *pDiffCountOut = 0U;

    SemanticTree* pCurr = NULL;
    HRESULT hr = desktopMode
        ? Perception_CaptureDesktopTree(&pCurr)
        : Perception_CaptureSemanticTree(targetHwnd, &pCurr);
    if (FAILED(hr) || !pCurr) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    const SemanticTree* pPrev = NULL;
    if (pState->pLastTree &&
        pState->lastTreeDesktop == desktopMode &&
        (desktopMode || pState->lastTreeHwnd == targetHwnd || pState->pLastTree->rootHwnd == pCurr->rootHwnd)) {
        pPrev = pState->pLastTree;
    }

    SemanticTreeDiff* pDiff = NULL;
    hr = Perception_DiffTrees(pPrev, pCurr, &pDiff);
    if (FAILED(hr) || !pDiff) {
        Perception_FreeSemanticTree(pCurr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    size_t minNeeded = sizeof(DiffPacketHeader) + (size_t)pDiff->entryCount * sizeof(TreeDiffEntry) + 1024U;
    if (*pBufCap < minNeeded) {
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, minNeeded);
        if (!pNew) {
            Perception_FreeTreeDiff(pDiff);
            Perception_FreeSemanticTree(pCurr);
            return E_OUTOFMEMORY;
        }
        *ppBuf = pNew;
        *pBufCap = minNeeded;
    }

    size_t written = 0U;
    hr = Perception_SerializeDiffBinary(pDiff, *ppBuf, *pBufCap, &written);
    if (SUCCEEDED(hr) && written >= sizeof(DiffPacketHeader)) {
        DiffPacketHeader* pHdr = (DiffPacketHeader*)(*ppBuf);
        pHdr->sequenceId = ++pState->diffSeq;
        *pBytesOut = written;
        if (pNodeCountOut) *pNodeCountOut = pCurr->nodeCount;
        if (pDiffCountOut) *pDiffCountOut = pDiff->entryCount;

        if (pState->pLastTree) {
            Perception_FreeSemanticTree(pState->pLastTree);
        }
        pState->pLastTree = pCurr;
        pState->lastTreeHwnd = targetHwnd;
        pState->lastTreeDesktop = desktopMode;
    } else {
        Perception_FreeSemanticTree(pCurr);
    }

    Perception_FreeTreeDiff(pDiff);
    return hr;
}

static HRESULT daemon_build_windows_packet(
    uint8_t** ppBuf,
    size_t* pBufCap,
    size_t* pBytesOut,
    uint32_t* pWinCountOut
) {
    *pBytesOut = 0U;
    if (pWinCountOut) *pWinCountOut = 0U;

    uint32_t winCap = 256U;
    WindowInfo* wins = (WindowInfo*)calloc(winCap, sizeof(WindowInfo));
    if (!wins) {
        return E_OUTOFMEMORY;
    }

    uint32_t total = 0U;
    HRESULT hr = Control_EnumWindows(wins, winCap, &total);
    if (SUCCEEDED(hr) && total > winCap) {
        uint32_t biggerCap = total + 32U;
        WindowInfo* pBigger = (WindowInfo*)realloc(wins, (size_t)biggerCap * sizeof(WindowInfo));
        if (pBigger) {
            wins = pBigger;
            winCap = biggerCap;
            memset(wins, 0, (size_t)winCap * sizeof(WindowInfo));
            hr = Control_EnumWindows(wins, winCap, &total);
        }
    }
    if (FAILED(hr)) {
        free(wins);
        return hr;
    }

    uint32_t count = (total > winCap) ? winCap : total;
    size_t needed = sizeof(WindowPacketHeader) + (size_t)count * sizeof(WindowInfo);
    if (*pBufCap < needed) {
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, needed);
        if (!pNew) {
            free(wins);
            return E_OUTOFMEMORY;
        }
        *ppBuf = pNew;
        *pBufCap = needed;
    }

    WindowPacketHeader hdr;
    hdr.packetMagic = CPOWER_WINDOWS_PACKET_MAGIC;
    hdr.windowCount = count;
    hdr.totalPayloadSize = (uint32_t)needed;
    hdr.reserved = 0U;
    memcpy(*ppBuf, &hdr, sizeof(hdr));
    if (count > 0U) {
        memcpy(*ppBuf + sizeof(hdr), wins, (size_t)count * sizeof(WindowInfo));
    }

    free(wins);
    *pBytesOut = needed;
    if (pWinCountOut) *pWinCountOut = count;
    return S_OK;
}

static HRESULT daemon_build_image_packet(
    const CommandPacket* pCmd,
    uint8_t** ppBuf,
    size_t* pBufCap,
    size_t* pBytesOut
) {
    *pBytesOut = 0U;
    ImageBuffer img;
    memset(&img, 0, sizeof(img));
    HRESULT hr = Perception_CaptureMicroRegion(pCmd->targetX, pCmd->targetY, pCmd->param1, pCmd->param2, &img);
    if (FAILED(hr)) {
        return hr;
    }

    BOOL wantBmp = (_wcsicmp(pCmd->textPayload, L"bmp") == 0);
    size_t needed = img.dataSize + (size_t)img.height * 16U + 4096U;
    if (*pBufCap < needed) {
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, needed);
        if (!pNew) {
            Perception_FreeImageBuffer(&img);
            return E_OUTOFMEMORY;
        }
        *ppBuf = pNew;
        *pBufCap = needed;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        size_t written = 0U;
        hr = wantBmp
            ? Perception_EncodeBmpInMemory(&img, *ppBuf, *pBufCap, &written)
            : Perception_EncodePngInMemory(&img, *ppBuf, *pBufCap, &written);
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            if (SUCCEEDED(hr)) {
                *pBytesOut = written;
            }
            break;
        }
        size_t nextCap = (*pBufCap) * 2U;
        uint8_t* pNew = (uint8_t*)realloc(*ppBuf, nextCap);
        if (!pNew) {
            hr = E_OUTOFMEMORY;
            break;
        }
        *ppBuf = pNew;
        *pBufCap = nextCap;
    }

    Perception_FreeImageBuffer(&img);
    return hr;
}

static HRESULT daemon_pipe_write_framed_small(
    PipeServer* pServer,
    uint16_t pktType,
    const void* pPayload,
    uint32_t payloadSize
) {
    if (payloadSize > 64U) {
        return E_INVALIDARG;
    }
    uint8_t frame[sizeof(IPC_FrameHeader) + 64U];
    IPC_FrameHeader hdr;
    hdr.payloadSize = payloadSize;
    hdr.packetType = pktType;
    hdr.flags = 0U;
    memcpy(frame, &hdr, sizeof(hdr));
    if (payloadSize > 0U && pPayload) {
        memcpy(frame + sizeof(hdr), pPayload, payloadSize);
    }
    uint32_t total = (uint32_t)sizeof(hdr) + payloadSize;
    uint32_t written = 0U;
    HRESULT hr = IPC_PipeWrite(pServer, frame, total, &written);
    if (FAILED(hr)) {
        return hr;
    }
    return (written == total) ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
}

static void daemon_flush_events_shm(ShmChannel* pChannel) {
    if (!pChannel || !pChannel->pOutRing) {
        return;
    }
    uint32_t cap = pChannel->pOutRing->capacity;
    uint32_t frameBytes = (uint32_t)(sizeof(IPC_FrameHeader) + sizeof(EventPacket));
    uint32_t maxEventUsed = cap / 2U;
    int budget = 16;
    while (budget-- > 0) {
        uint32_t wIdx = pChannel->pOutRing->writeIdx;
        uint32_t rIdx = pChannel->pOutRing->readIdx;
        MemoryBarrier();
        uint32_t used = wIdx - rIdx;
        if (used > cap || used + frameBytes > maxEventUsed) {
            break;
        }
        WinEventRecord ev;
        if (!Perception_PollEvent(&ev)) {
            break;
        }
        EventPacket pkt;
        pkt.magic = CPOWER_EVENT_PACKET_MAGIC;
        pkt.event = (uint32_t)ev.event;
        pkt.hwnd = (uint64_t)(uintptr_t)ev.hwnd;
        pkt.idObject = (int32_t)ev.idObject;
        pkt.idChild = (int32_t)ev.idChild;
        pkt.threadId = (uint32_t)ev.threadId;
        pkt.timestamp = (uint32_t)ev.timestamp;
        if (FAILED(IPC_WriteRawFrame(pChannel, TRUE, (uint16_t)IPC_PKT_EVENT, &pkt, (uint32_t)sizeof(pkt)))) {
            break;
        }
    }
}

static HRESULT daemon_flush_events_pipe(PipeServer* pServer) {
    WinEventRecord ev;
    int budget = 16;
    while (budget-- > 0 && Perception_PollEvent(&ev)) {
        EventPacket pkt;
        pkt.magic = CPOWER_EVENT_PACKET_MAGIC;
        pkt.event = (uint32_t)ev.event;
        pkt.hwnd = (uint64_t)(uintptr_t)ev.hwnd;
        pkt.idObject = (int32_t)ev.idObject;
        pkt.idChild = (int32_t)ev.idChild;
        pkt.threadId = (uint32_t)ev.threadId;
        pkt.timestamp = (uint32_t)ev.timestamp;
        HRESULT hr = daemon_pipe_write_framed_small(pServer, (uint16_t)IPC_PKT_EVENT, &pkt, (uint32_t)sizeof(pkt));
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

static void daemon_process_command_common(
    const CommandPacket* pCmd,
    DaemonState* pState,
    uint8_t** ppPayloadBuf,
    size_t* pPayloadCap,
    size_t maxPayloadBytes,
    AckPacket* pOutAck,
    uint16_t* pOutPktType,
    size_t* pOutPayloadBytes
) {
    QpcTimer timer;
    Timer_Init(&timer);
    Timer_Start(&timer);

    memset(pOutAck, 0, sizeof(*pOutAck));
    pOutAck->magic = CPOWER_ACK_PACKET_MAGIC;
    pOutAck->commandId = pCmd->commandId;
    *pOutPktType = (uint16_t)IPC_PKT_UNKNOWN;
    *pOutPayloadBytes = 0U;

    HWND targetHwnd = (HWND)(uintptr_t)pCmd->targetHwnd;
    BOOL desktopMode = (pCmd->param1 == 1);

    HRESULT hr = S_OK;
    if (pCmd->commandCode == CMD_SHUTDOWN || pCmd->commandCode == CMD_NOP) {
        hr = S_OK;
    } else if (pCmd->commandCode == CMD_CAPTURE_MICRO_REGION) {
        hr = daemon_build_image_packet(pCmd, ppPayloadBuf, pPayloadCap, pOutPayloadBytes);
        if (SUCCEEDED(hr) && *pOutPayloadBytes > 0U && *pOutPayloadBytes <= (size_t)UINT32_MAX) {
            pOutAck->flags |= CPOWER_ACK_FLAG_HAS_IMAGE;
            *pOutPktType = (uint16_t)IPC_PKT_IMAGE;
        }
    } else if (pCmd->commandCode == CMD_LIST_WINDOWS) {
        hr = daemon_build_windows_packet(ppPayloadBuf, pPayloadCap, pOutPayloadBytes, &pOutAck->nodeCount);
        if (SUCCEEDED(hr) && *pOutPayloadBytes > 0U && *pOutPayloadBytes <= (size_t)UINT32_MAX) {
            pOutAck->flags |= CPOWER_ACK_FLAG_HAS_WINDOWS;
            *pOutPktType = (uint16_t)IPC_PKT_WINDOWS;
        }
    } else if (pCmd->commandCode == CMD_DIFF_TREE) {
        hr = daemon_build_diff_packet(
            pState, targetHwnd, desktopMode,
            ppPayloadBuf, pPayloadCap, pOutPayloadBytes,
            &pOutAck->nodeCount, &pOutAck->diffCount);
        if (SUCCEEDED(hr) && *pOutPayloadBytes > 0U && *pOutPayloadBytes <= (size_t)UINT32_MAX) {
            pOutAck->flags |= CPOWER_ACK_FLAG_HAS_DIFF;
            *pOutPktType = (uint16_t)IPC_PKT_TREE_DIFF;
        }
    } else if (pCmd->commandCode == CMD_CAPTURE_TREE) {
        hr = daemon_capture_and_serialize_tree(
            pState, targetHwnd, desktopMode,
            ppPayloadBuf, pPayloadCap, pOutPayloadBytes, &pOutAck->nodeCount);
        if (SUCCEEDED(hr) && *pOutPayloadBytes > 0U && *pOutPayloadBytes <= (size_t)UINT32_MAX) {
            pOutAck->flags |= CPOWER_ACK_FLAG_HAS_TREE;
            *pOutPktType = (uint16_t)IPC_PKT_TREE;
        }
    } else {
        hr = daemon_dispatch_command(pCmd, pState);
        if (FAILED(hr)) {
            CPower_Log(CPOWER_LOG_WARN, "Command %u dispatch failed: 0x%08lX",
                       (unsigned)pCmd->commandCode, (unsigned long)hr);
        } else {
            BOOL postDesktop = (pState->pLastTree != NULL) ? pState->lastTreeDesktop : FALSE;
            size_t treeBytes = 0U;
            uint32_t nCount = 0U;
            if (SUCCEEDED(daemon_capture_and_serialize_tree(
                    pState, targetHwnd, postDesktop,
                    ppPayloadBuf, pPayloadCap, &treeBytes, &nCount)) &&
                treeBytes > 0U && treeBytes <= (size_t)UINT32_MAX) {
                pOutAck->nodeCount = nCount;
                pOutAck->flags |= CPOWER_ACK_FLAG_HAS_TREE;
                *pOutPktType = (uint16_t)IPC_PKT_TREE;
                *pOutPayloadBytes = treeBytes;
            }
        }
    }

    if (*pOutPayloadBytes > maxPayloadBytes ||
        (*pOutPayloadBytes > 0U && *pOutPktType == IPC_PKT_UNKNOWN)) {
        pOutAck->flags = CPOWER_ACK_FLAG_NONE;
        pOutAck->nodeCount = 0U;
        pOutAck->diffCount = 0U;
        *pOutPktType = (uint16_t)IPC_PKT_UNKNOWN;
        *pOutPayloadBytes = 0U;
        if (SUCCEEDED(hr)) {
            hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
    }

    Timer_Stop(&timer);
    pOutAck->hresult = (int32_t)hr;
    pOutAck->elapsedUs = (uint32_t)(Timer_ElapsedMs(&timer) * 1000.0);
}

static int cmd_daemon(int argc, WCHAR* argv[]) {
    BOOL usePipe = FALSE;
    const WCHAR* pszName = NULL;
    DWORD durationMs = 0U;
    BOOL streamEvents = FALSE;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--transport") == 0 && i + 1 < argc) {
            const WCHAR* t = argv[++i];
            if (_wcsicmp(t, L"shm") == 0) {
                usePipe = FALSE;
            } else if (_wcsicmp(t, L"pipe") == 0) {
                usePipe = TRUE;
            } else {
                fprintf(stderr, "Invalid --transport value. Expected shm or pipe.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--name") == 0 && i + 1 < argc) {
            pszName = argv[++i];
        } else if (wcscmp(argv[i], L"--duration") == 0 && i + 1 < argc) {
            if (!cli_parse_dword(argv[++i], &durationMs)) {
                fprintf(stderr, "Invalid --duration value.\n");
                return 1;
            }
        } else if (wcscmp(argv[i], L"--stream-events") == 0) {
            streamEvents = TRUE;
        } else {
            fprintf(stderr, "Unknown or incomplete option in 'daemon': %ls\n", argv[i]);
            return 1;
        }
    }

    if (CPower_Init() != CPOWER_OK) {
        fprintf(stderr, "Failed to initialize cpower core.\n");
        return 1;
    }
    if (FAILED(Perception_Init()) || FAILED(Control_Init())) {
        fprintf(stderr, "Failed to initialize Perception/Control subsystems.\n");
        Perception_Shutdown();
        Control_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    DaemonState state;
    memset(&state, 0, sizeof(state));
    state.streamEvents = streamEvents;
    if (SUCCEEDED(CoCreateInstance(
            &CPOWER_CLI_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
            &CPOWER_CLI_IID_IUIAutomation, (void**)&state.pAuto)) && state.pAuto) {
        (void)IUIAutomation_get_ControlViewWalker(state.pAuto, &state.pWalker);
    }

    size_t payloadCap = 2U * 1024U * 1024U;
    uint8_t* pPayloadBuf = (uint8_t*)malloc(payloadCap);
    if (!pPayloadBuf) {
        CPOWER_SAFE_RELEASE(state.pWalker);
        CPOWER_SAFE_RELEASE(state.pAuto);
        Control_Shutdown();
        Perception_Shutdown();
        CPower_Shutdown();
        return 1;
    }

    if (streamEvents) {
        if (FAILED(Perception_StartEventHook(NULL, NULL))) {
            CPower_Log(CPOWER_LOG_WARN, "Perception_StartEventHook failed; event streaming disabled.");
            state.streamEvents = FALSE;
        }
    }

    InterlockedExchange(&g_stopRequested, 0);
    SetConsoleCtrlHandler(cli_console_ctrl_handler, TRUE);

    int exitCode = 0;
    ULONGLONG startMs = GetTickCount64();

    if (!usePipe) {
        const WCHAR* shmName = pszName ? pszName : CPOWER_DEFAULT_SHM_NAME;
        ShmChannel* pChannel = NULL;
        HRESULT hr = IPC_CreateShmChannel(shmName, 0U, &pChannel);
        if (FAILED(hr) || !pChannel) {
            fprintf(stderr, "IPC_CreateShmChannel('%ls') failed: 0x%08lX\n", shmName, (unsigned long)hr);
            exitCode = 1;
        } else {
            printf("[daemon] SHM transport listening on '%ls'...\n", shmName);
            fflush(stdout);

            while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
                if (durationMs > 0U && (GetTickCount64() - startMs) >= (ULONGLONG)durationMs) {
                    break;
                }

                if (state.streamEvents) {
                    daemon_flush_events_shm(pChannel);
                }

                CommandPacket cmd;
                memset(&cmd, 0, sizeof(cmd));
                hr = IPC_ReadCommandPacket(pChannel, &cmd, state.streamEvents ? 15U : 50U);
                if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
                    continue;
                }
                if (FAILED(hr)) {
                    CPower_Log(CPOWER_LOG_WARN, "SHM command read error: 0x%08lX", (unsigned long)hr);
                    continue;
                }

                AckPacket ack;
                uint16_t pktType = 0U;
                size_t payloadBytes = 0U;
                size_t maxShmPayload = 0U;
                if (pChannel->ringCapacity > (2U * sizeof(IPC_FrameHeader) + sizeof(AckPacket))) {
                    maxShmPayload = pChannel->ringCapacity - (2U * sizeof(IPC_FrameHeader) + sizeof(AckPacket));
                }
                daemon_process_command_common(
                    &cmd, &state, &pPayloadBuf, &payloadCap,
                    maxShmPayload, &ack, &pktType, &payloadBytes);

                BOOL hasPayload = (payloadBytes > 0U && pktType != IPC_PKT_UNKNOWN && ack.flags != CPOWER_ACK_FLAG_NONE);
                uint32_t ackFrameBytes = (uint32_t)(sizeof(IPC_FrameHeader) + sizeof(AckPacket));
                uint32_t payloadFrameBytes = hasPayload ? (uint32_t)(sizeof(IPC_FrameHeader) + payloadBytes) : 0U;
                uint32_t totalFrameBytes = ackFrameBytes + payloadFrameBytes;

                if (pChannel->pOutRing != NULL) {
                    uint32_t cap = pChannel->pOutRing->capacity;
                    ULONGLONG waitStart = GetTickCount64();
                    for (;;) {
                        uint32_t wIdx = pChannel->pOutRing->writeIdx;
                        uint32_t rIdx = pChannel->pOutRing->readIdx;
                        MemoryBarrier();
                        uint32_t used = wIdx - rIdx;
                        if (used <= cap && (cap - used) >= totalFrameBytes) {
                            break;
                        }
                        if (InterlockedCompareExchange(&g_stopRequested, 0, 0) != 0 ||
                            (GetTickCount64() - waitStart) >= 1000ULL) {
                            if (hasPayload) {
                                hasPayload = FALSE;
                                ack.flags = CPOWER_ACK_FLAG_NONE;
                                ack.nodeCount = 0U;
                                ack.diffCount = 0U;
                                ack.hresult = (int32_t)HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                                payloadBytes = 0U;
                                pktType = (uint16_t)IPC_PKT_UNKNOWN;
                            }
                            break;
                        }
                        Sleep(1U);
                    }
                }

                HRESULT ackHr = IPC_WriteRawFrame(pChannel, TRUE, (uint16_t)IPC_PKT_ACK, &ack, (uint32_t)sizeof(ack));
                if (FAILED(ackHr)) {
                    CPower_Log(CPOWER_LOG_WARN, "SHM AckPacket write failed: 0x%08lX", (unsigned long)ackHr);
                } else if (hasPayload) {
                    HRESULT wHr = IPC_WriteRawFrame(pChannel, TRUE, pktType, pPayloadBuf, (uint32_t)payloadBytes);
                    if (FAILED(wHr)) {
                        CPower_Log(CPOWER_LOG_WARN, "SHM payload write failed (type=%u): 0x%08lX",
                                   (unsigned)pktType, (unsigned long)wHr);
                    }
                }

                if (cmd.commandCode == CMD_SHUTDOWN) {
                    printf("[daemon] Received CMD_SHUTDOWN.\n");
                    break;
                }
            }

            IPC_CloseShmChannel(pChannel);
        }
    } else {
        const WCHAR* pipeName = pszName ? pszName : CPOWER_DEFAULT_PIPE_NAME;
        PipeServer* pServer = NULL;
        HRESULT hr = IPC_CreatePipeServer(pipeName, &pServer);
        if (FAILED(hr) || !pServer) {
            fprintf(stderr, "IPC_CreatePipeServer('%ls') failed: 0x%08lX\n", pipeName, (unsigned long)hr);
            exitCode = 1;
        } else {
            printf("[daemon] Named Pipe transport listening on '%ls'...\n", pipeName);
            fflush(stdout);

            HANDLE hMainThread = NULL;
            if (!DuplicateHandle(
                    GetCurrentProcess(),
                    GetCurrentThread(),
                    GetCurrentProcess(),
                    &hMainThread,
                    THREAD_TERMINATE | SYNCHRONIZE | THREAD_SET_CONTEXT,
                    FALSE,
                    0U)) {
                CPower_Log(CPOWER_LOG_WARN, "DuplicateHandle failed: err=%lu", (unsigned long)GetLastError());
                hMainThread = NULL;
            }

            DaemonWatchdogArgs wdArgs;
            wdArgs.hTargetThread = hMainThread;
            wdArgs.durationMs = durationMs;
            wdArgs.targetDone = 0;
            HANDLE hWatchdog = CreateThread(NULL, 0U, daemon_watchdog_thread, &wdArgs, 0U, NULL);

            uint8_t* pPipeFrameBuf = NULL;
            size_t pipeFrameCap = 0U;

            if (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
                SUCCEEDED(IPC_AcceptPipeClient(pServer))) {
                while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
                    if (durationMs > 0U && (GetTickCount64() - startMs) >= (ULONGLONG)durationMs) {
                        break;
                    }

                    if (state.streamEvents) {
                        DWORD avail = 0U;
                        if (!PeekNamedPipe(pServer->hPipe, NULL, 0U, NULL, &avail, NULL)) {
                            break;
                        }
                        if (avail == 0U) {
                            if (FAILED(daemon_flush_events_pipe(pServer))) {
                                break;
                            }
                            Sleep(10U);
                            continue;
                        }
                    }

                    CommandPacket cmd;
                    memset(&cmd, 0, sizeof(cmd));
                    uint32_t bytesRead = 0U;
                    hr = IPC_PipeRead(pServer, &cmd, (uint32_t)sizeof(cmd), &bytesRead);
                    if (FAILED(hr) || bytesRead < sizeof(CommandPacket)) {
                        break;
                    }

                    AckPacket ack;
                    uint16_t pktType = 0U;
                    size_t payloadBytes = 0U;
                    daemon_process_command_common(
                        &cmd, &state, &pPayloadBuf, &payloadCap,
                        CPOWER_MAX_PIPE_PAYLOAD_BYTES, &ack, &pktType, &payloadBytes);

                    BOOL hasPayload = (payloadBytes > 0U && pktType != IPC_PKT_UNKNOWN && ack.flags != CPOWER_ACK_FLAG_NONE);
                    uint32_t totalFrame = 0U;
                    if (hasPayload) {
                        size_t neededFrame = sizeof(IPC_FrameHeader) + payloadBytes;
                        if (pipeFrameCap < neededFrame) {
                            uint8_t* pNew = (uint8_t*)realloc(pPipeFrameBuf, neededFrame);
                            if (!pNew) {
                                hasPayload = FALSE;
                                ack.flags = CPOWER_ACK_FLAG_NONE;
                                ack.nodeCount = 0U;
                                ack.diffCount = 0U;
                                ack.hresult = (int32_t)E_OUTOFMEMORY;
                                payloadBytes = 0U;
                                pktType = (uint16_t)IPC_PKT_UNKNOWN;
                            } else {
                                pPipeFrameBuf = pNew;
                                pipeFrameCap = neededFrame;
                            }
                        }
                        if (hasPayload) {
                            IPC_FrameHeader hdr;
                            hdr.payloadSize = (uint32_t)payloadBytes;
                            hdr.packetType = pktType;
                            hdr.flags = 0U;
                            memcpy(pPipeFrameBuf, &hdr, sizeof(hdr));
                            memcpy(pPipeFrameBuf + sizeof(hdr), pPayloadBuf, payloadBytes);
                            totalFrame = (uint32_t)neededFrame;
                        }
                    }

                    HRESULT wHr = daemon_pipe_write_framed_small(
                        pServer, (uint16_t)IPC_PKT_ACK, &ack, (uint32_t)sizeof(ack));
                    if (FAILED(wHr)) {
                        CPower_Log(CPOWER_LOG_WARN, "Pipe AckPacket write failed: 0x%08lX", (unsigned long)wHr);
                        break;
                    }
                    if (hasPayload && totalFrame > 0U) {
                        uint32_t written = 0U;
                        wHr = IPC_PipeWrite(pServer, pPipeFrameBuf, totalFrame, &written);
                        if (FAILED(wHr) || written != totalFrame) {
                            CPower_Log(CPOWER_LOG_WARN, "Pipe payload write failed (type=%u): 0x%08lX",
                                       (unsigned)pktType, (unsigned long)wHr);
                            break;
                        }
                    }

                    if (cmd.commandCode == CMD_SHUTDOWN) {
                        printf("[daemon] Received CMD_SHUTDOWN.\n");
                        break;
                    }
                }
            }

            free(pPipeFrameBuf);
            InterlockedExchange(&wdArgs.targetDone, 1);
            InterlockedExchange(&g_stopRequested, 1);
            if (hWatchdog) {
                WaitForSingleObject(hWatchdog, 2000U);
                CloseHandle(hWatchdog);
            }
            if (hMainThread) {
                CloseHandle(hMainThread);
            }
            IPC_ClosePipeServer(pServer);
        }
    }

    SetConsoleCtrlHandler(cli_console_ctrl_handler, FALSE);
    if (state.streamEvents) {
        Perception_StopEventHook();
    }
    if (state.pLastTree) {
        Perception_FreeSemanticTree(state.pLastTree);
    }
    free(pPayloadBuf);
    CPOWER_SAFE_RELEASE(state.pWalker);
    CPOWER_SAFE_RELEASE(state.pAuto);
    Control_Shutdown();
    Perception_Shutdown();
    CPower_Shutdown();
    return exitCode;
}

/* -------------------------------------------------------------------------
 * Subcommand: verify
 * ------------------------------------------------------------------------- */
static int cmd_verify(void) {
    printf("========================================================================\n");
    printf("  cpower v%s - System, COM/UIA, Security/UIPI & IPC Diagnostics\n", CPOWER_VERSION_STRING);
    printf("========================================================================\n\n");

    int failures = 0;

    /* 1. Core + Timer + DPI */
    cpower_status_t st = CPower_Init();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    UINT dpi = cli_get_system_dpi();
    int vLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    printf("[1. Core & Hardware Environment]\n");
    printf("  CPower_Init         : %s (%s)\n", (st == CPOWER_OK) ? "PASS" : "FAIL", CPower_GetErrorString(st));
    printf("  QPC Frequency       : %lld Hz\n", (long long)freq.QuadPart);
    printf("  System DPI          : %u (Scale: %u%%)\n", dpi, (dpi * 100U) / 96U);
    printf("  Virtual Desktop     : (%d, %d) %dx%d\n\n", vLeft, vTop, vWidth, vHeight);
    if (st != CPOWER_OK) {
        failures++;
    }

    /* 2. Perception / COM / UIA */
    printf("[2. Perception Engine (COM MTA & IUIAutomation)]\n");
    HRESULT hrPerc = Perception_Init();
    printf("  Perception_Init     : %s (hr=0x%08lX)\n", SUCCEEDED(hrPerc) ? "PASS" : "FAIL", (unsigned long)hrPerc);
    if (SUCCEEDED(hrPerc)) {
        HWND fg = GetForegroundWindow();
        QpcTimer t;
        Timer_Init(&t);
        Timer_Start(&t);
        SemanticTree* pTree = NULL;
        HRESULT hrTree = Perception_CaptureSemanticTree(fg, &pTree);
        Timer_Stop(&t);
        if (SUCCEEDED(hrTree) && pTree) {
            printf("  Foreground HWND     : 0x%p\n", (void*)pTree->rootHwnd);
            printf("  UIA Tree Extraction : PASS (%u nodes in %.2f ms)\n",
                   (unsigned)pTree->nodeCount, Timer_ElapsedMs(&t));
            Perception_FreeSemanticTree(pTree);
        } else {
            printf("  UIA Tree Extraction : WARN (hr=0x%08lX, fg=0x%p)\n",
                   (unsigned long)hrTree, (void*)fg);
        }
        Perception_Shutdown();
    } else {
        failures++;
    }
    printf("\n");

    /* 3. Control Engine */
    printf("[3. Dual-Mode Control Engine]\n");
    HRESULT hrCtrl = Control_Init();
    printf("  Control_Init        : %s (hr=0x%08lX)\n", SUCCEEDED(hrCtrl) ? "PASS" : "FAIL", (unsigned long)hrCtrl);
    if (SUCCEEDED(hrCtrl)) {
        double jerkMid = Control_MinimumJerk(0.5);
        uint32_t winCount = 0U;
        WindowInfo wins[8];
        HRESULT hrWin = Control_EnumWindows(wins, 8U, &winCount);
        printf("  Minimum-Jerk(0.5)   : %.4f\n", jerkMid);
        printf("  Window Enumeration  : %s (%u visible top-level windows)\n",
               SUCCEEDED(hrWin) ? "PASS" : "FAIL", (unsigned)winCount);
        if (FAILED(hrWin)) {
            failures++;
        }
        Control_Shutdown();
    } else {
        failures++;
    }
    printf("\n");

    /* 4. Security & UIPI */
    printf("[4. Security, Integrity & UIPI Bypass]\n");
    SecurityEnvironmentReport sec;
    HRESULT hrSec = Security_GetEnvironmentReport(&sec);
    HRESULT hrVer = Security_VerifyEnvironment();
    if (SUCCEEDED(hrSec)) {
        HWND fg = GetForegroundWindow();
        BOOL canFg = fg ? Security_CanInteractWithWindow(fg) : TRUE;
        printf("  Executable Path     : %ls\n", sec.executablePath);
        printf("  TokenUIAccess       : %s\n", sec.hasUiAccess ? "TRUE (UIPI Bypass Active)" : "FALSE");
        printf("  IsElevated (Admin)  : %s\n", sec.isElevated ? "TRUE" : "FALSE");
        printf("  Integrity Level     : %s (RID=0x%04lX)\n",
               Security_IntegrityLevelToString(sec.integrityLevel),
               (unsigned long)sec.integrityRid);
        printf("  EnableSecureUIAPaths: %s\n", sec.secureUiaPathsEnabled ? "1 (Strict Path Check)" : "0 (Dev Bypass Active)");
        printf("  In Secure Directory : %s\n", sec.inSecureDirectory ? "TRUE" : "FALSE");
        printf("  Can Control FG Wnd  : %s\n", canFg ? "TRUE" : "FALSE");
        printf("  Verify Environment  : %s (hr=0x%08lX)\n",
               (hrVer == S_OK) ? "READY (UIPI Capable)" : "STANDARD (No UIPI Bypass)",
               (unsigned long)hrVer);
    } else {
        printf("  Security Report     : FAIL (hr=0x%08lX)\n", (unsigned long)hrSec);
        failures++;
    }
    printf("\n");

    /* 5. IPC Subsystem Self-Check */
    printf("[5. IPC Transport Self-Check (SHM + Named Pipe)]\n");
    WCHAR testShmName[128];
    _snwprintf(testShmName, 128, L"Local\\CPowerVerifyShm_%lu", (unsigned long)GetCurrentProcessId());
    testShmName[127] = L'\0';

    ShmChannel* pSrv = NULL;
    ShmChannel* pCli = NULL;
    HRESULT hrShm = IPC_CreateShmChannel(testShmName, 65536U, &pSrv);
    if (SUCCEEDED(hrShm) && SUCCEEDED(IPC_OpenShmChannel(testShmName, &pCli))) {
        CommandPacket outCmd;
        CommandPacket inCmd;
        memset(&outCmd, 0, sizeof(outCmd));
        memset(&inCmd, 0, sizeof(inCmd));
        outCmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
        outCmd.commandId = 42U;
        outCmd.commandCode = CMD_NOP;
        HRESULT hrW = IPC_WriteCommandPacket(pCli, &outCmd);
        HRESULT hrR = IPC_ReadCommandPacket(pSrv, &inCmd, 200U);
        if (SUCCEEDED(hrW) && SUCCEEDED(hrR) && inCmd.commandId == 42U) {
            printf("  SHM SPSC Round-Trip : PASS (64 KB ring, SDDL cross-integrity)\n");
        } else {
            printf("  SHM SPSC Round-Trip : FAIL (w=0x%08lX r=0x%08lX)\n",
                   (unsigned long)hrW, (unsigned long)hrR);
            failures++;
        }
    } else {
        printf("  SHM Channel Init    : FAIL (hr=0x%08lX)\n", (unsigned long)hrShm);
        failures++;
    }
    IPC_CloseShmChannel(pCli);
    IPC_CloseShmChannel(pSrv);

    WCHAR testPipeName[128];
    _snwprintf(testPipeName, 128, L"\\\\.\\pipe\\cpower_verify_%lu", (unsigned long)GetCurrentProcessId());
    testPipeName[127] = L'\0';
    PipeServer* pPipe = NULL;
    HRESULT hrPipe = IPC_CreatePipeServer(testPipeName, &pPipe);
    if (SUCCEEDED(hrPipe) && pPipe) {
        printf("  Named Pipe Server   : PASS (Message-Mode, 256 KB, SDDL cross-integrity)\n");
        IPC_ClosePipeServer(pPipe);
    } else {
        printf("  Named Pipe Server   : FAIL (hr=0x%08lX)\n", (unsigned long)hrPipe);
        failures++;
    }

    CPower_Shutdown();
    printf("\n========================================================================\n");
    printf("  Verification Summary: %s (%d failures)\n",
           (failures == 0) ? "ALL CHECKS PASSED" : "FAILED", failures);
    printf("========================================================================\n");
    return (failures == 0) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Entry Point: wmain
 * ------------------------------------------------------------------------- */
int wmain(int argc, WCHAR* argv[]) {
    cli_enable_per_monitor_v2_dpi();
    (void)SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        cli_print_usage();
        return 0;
    }

    const WCHAR* sub = argv[1];

    if (_wcsicmp(sub, L"--help") == 0 || _wcsicmp(sub, L"-h") == 0 || _wcsicmp(sub, L"help") == 0) {
        cli_print_usage();
        return 0;
    }
    if (_wcsicmp(sub, L"version") == 0 || _wcsicmp(sub, L"--version") == 0 || _wcsicmp(sub, L"-v") == 0) {
        printf("cpower version %s\n", CPower_GetVersion());
        return 0;
    }
    if (_wcsicmp(sub, L"tree") == 0) {
        return cmd_tree(argc, argv);
    }
    if (_wcsicmp(sub, L"find") == 0) {
        return cmd_find(argc, argv);
    }
    if (_wcsicmp(sub, L"diff") == 0) {
        return cmd_diff(argc, argv);
    }
    if (_wcsicmp(sub, L"windows") == 0) {
        return cmd_windows(argc, argv);
    }
    if (_wcsicmp(sub, L"focus") == 0) {
        return cmd_focus(argc, argv);
    }
    if (_wcsicmp(sub, L"events") == 0) {
        return cmd_events(argc, argv);
    }
    if (_wcsicmp(sub, L"action") == 0) {
        return cmd_action(argc, argv);
    }
    if (_wcsicmp(sub, L"capture") == 0) {
        return cmd_capture(argc, argv);
    }
    if (_wcsicmp(sub, L"hook") == 0) {
        return cmd_hook(argc, argv);
    }
    if (_wcsicmp(sub, L"daemon") == 0) {
        return cmd_daemon(argc, argv);
    }
    if (_wcsicmp(sub, L"verify") == 0) {
        return cmd_verify();
    }

    fprintf(stderr, "Unknown subcommand: %ls\n\n", sub);
    cli_print_usage();
    return 1;
}
