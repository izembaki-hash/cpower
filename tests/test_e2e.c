/**
 * @file test_e2e.c
 * @brief Tier 3 (Cross-Feature Combinations) and Tier 4 (Real-World Scenarios)
 *        End-to-End Integration Tests for the Pure C Windows-Native Agent.
 */

#include "test_infra.h"
#include "cpower.h"
#include "perception.h"
#include "input.h"
#include "ipc.h"
#include "security.h"
#include "text_hook.h"
#include <wchar.h>

#ifndef CPOWER_EXEC_MODE_MASK
#define CPOWER_EXEC_MODE_MASK        0x00FFU
#endif
#ifndef CPOWER_EXEC_FLAG_HAS_NODE_ID
#define CPOWER_EXEC_FLAG_HAS_NODE_ID 0x0100U
#endif
#ifndef CPOWER_EXEC_FLAG_HAS_COORDS
#define CPOWER_EXEC_FLAG_HAS_COORDS  0x0200U
#endif

/* Local static UIA GUIDs to avoid linking conflicts with uuid.lib or INITGUID */
static const GUID kE2EClsidCUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID kE2EIidIUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

/* ------------------------------------------------------------------------- */
/* Tracked Suite Resources for Guaranteed Leak-Free Teardown on longjmp      */
/* ------------------------------------------------------------------------- */

typedef struct E2EWin32Window {
    HANDLE hThread;
    HANDLE hReadyEvent;
    HWND   hwnd;
    HWND   hBtn;
    HWND   hEdit;
    HWND   hCheck;
    HWND   hStatic;
    ATOM   classAtom;
    WCHAR  className[64];
    volatile LONG btnClicks;
    volatile LONG clientClicks;
    volatile LONG lastClientX;
    volatile LONG lastClientY;
} E2EWin32Window;

static E2EWin32Window     g_e2eWin;
static PROCESS_INFORMATION g_e2eProc;
static PROCESS_INFORMATION g_e2eDaemonProc;
static DWORD              g_e2eHookPid     = 0;
static BOOL               g_e2eHookActive  = FALSE;
static SemanticTree*      g_e2eTreeA       = NULL;
static SemanticTree*      g_e2eTreeB       = NULL;
static ImageBuffer        g_e2eImg;
static ShmChannel*        g_e2eShmServer   = NULL;
static ShmChannel*        g_e2eShmClient   = NULL;
static PipeServer*        g_e2ePipeServer  = NULL;
static PipeServer*        g_e2ePipeClient  = NULL;
static HANDLE             g_e2eWorker      = NULL;
static void*              g_e2eBufA        = NULL;
static void*              g_e2eBufB        = NULL;
static void*              g_e2eBufC        = NULL;
static HWND               g_e2eExtHwnd     = NULL;
static IUIAutomation*           g_e2eAuto    = NULL;
static IUIAutomationTreeWalker* g_e2eWalker  = NULL;
static BOOL               g_e2eComInit     = FALSE;
static LONG               g_e2eSeq         = 0;

/* ------------------------------------------------------------------------- */
/* Win32 Dedicated UI-Thread Helper Window                                   */
/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK e2e_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    E2EWin32Window* pWin = (E2EWin32Window*)(uintptr_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* cs = (const CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_COMMAND:
            if (pWin && LOWORD(wParam) == 4001 && HIWORD(wParam) == BN_CLICKED) {
                InterlockedIncrement(&pWin->btnClicks);
                if (pWin->hStatic && IsWindow(pWin->hStatic)) {
                    WCHAR status[64];
                    LONG c = InterlockedCompareExchange(&pWin->btnClicks, 0, 0);
                    swprintf(status, 64, L"Clicks:%ld", c);
                    SetWindowTextW(pWin->hStatic, status);
                }
            }
            break;
        case WM_LBUTTONUP:
            if (pWin) {
                InterlockedExchange(&pWin->lastClientX, (LONG)(int16_t)LOWORD(lParam));
                InterlockedExchange(&pWin->lastClientY, (LONG)(int16_t)HIWORD(lParam));
                InterlockedIncrement(&pWin->clientClicks);
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

static DWORD WINAPI e2e_ui_thread(LPVOID lpParam) {
    E2EWin32Window* w = (E2EWin32Window*)lpParam;
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = e2e_wndproc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = w->className;
    w->classAtom = RegisterClassExW(&wc);
    if (w->classAtom == 0) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hwnd = CreateWindowExW(
        0, w->className, L"CPowerE2EWin32Host",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 420, 300,
        NULL, NULL, hInst, w
    );
    if (!w->hwnd) {
        SetEvent(w->hReadyEvent);
        return 1;
    }

    w->hBtn = CreateWindowExW(
        0, L"BUTTON", L"ExecuteE2E",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        24, 24, 150, 34,
        w->hwnd, (HMENU)(uintptr_t)4001, hInst, NULL
    );
    w->hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"InitialHostText",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        24, 72, 260, 28,
        w->hwnd, (HMENU)(uintptr_t)4002, hInst, NULL
    );
    w->hCheck = CreateWindowExW(
        0, L"BUTTON", L"E2EOption",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        24, 114, 160, 28,
        w->hwnd, (HMENU)(uintptr_t)4003, hInst, NULL
    );
    w->hStatic = CreateWindowExW(
        0, L"STATIC", L"Clicks:0",
        WS_CHILD | WS_VISIBLE,
        24, 154, 180, 24,
        w->hwnd, (HMENU)(uintptr_t)4004, hInst, NULL
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

static BOOL start_e2e_win32_window(void) {
    memset(&g_e2eWin, 0, sizeof(g_e2eWin));
    LONG seq = InterlockedIncrement(&g_e2eSeq);
    swprintf(g_e2eWin.className, 64, L"CPowerE2EWin_%lu_%ld", GetCurrentProcessId(), seq);
    g_e2eWin.hReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_e2eWin.hReadyEvent) return FALSE;

    g_e2eWin.hThread = CreateThread(NULL, 0, e2e_ui_thread, &g_e2eWin, 0, NULL);
    if (!g_e2eWin.hThread) {
        CloseHandle(g_e2eWin.hReadyEvent);
        g_e2eWin.hReadyEvent = NULL;
        return FALSE;
    }

    if (WaitForSingleObject(g_e2eWin.hReadyEvent, 5000) != WAIT_OBJECT_0 || !g_e2eWin.hwnd) {
        return FALSE;
    }
    Sleep(50);
    return TRUE;
}

static void stop_e2e_win32_window(void) {
    if (g_e2eWin.hwnd && IsWindow(g_e2eWin.hwnd)) {
        PostMessageW(g_e2eWin.hwnd, WM_CLOSE, 0, 0);
    }
    if (g_e2eWin.hThread) {
        if (WaitForSingleObject(g_e2eWin.hThread, 4000) == WAIT_TIMEOUT && g_e2eWin.hwnd) {
            PostThreadMessageW(GetThreadId(g_e2eWin.hThread), WM_QUIT, 0, 0);
            WaitForSingleObject(g_e2eWin.hThread, 2000);
        }
        CloseHandle(g_e2eWin.hThread);
        g_e2eWin.hThread = NULL;
    }
    if (g_e2eWin.hReadyEvent) {
        CloseHandle(g_e2eWin.hReadyEvent);
        g_e2eWin.hReadyEvent = NULL;
    }
    if (g_e2eWin.classAtom != 0) {
        UnregisterClassW(g_e2eWin.className, GetModuleHandleW(NULL));
        g_e2eWin.classAtom = 0;
    }
    g_e2eWin.hwnd = NULL;
}

static void stop_e2e_ext_process(HWND targetHwnd) {
    if (targetHwnd && IsWindow(targetHwnd)) {
        PostMessageW(targetHwnd, WM_CLOSE, 0, 0);
    }
    if (g_e2eProc.hProcess) {
        if (WaitForSingleObject(g_e2eProc.hProcess, 3000) == WAIT_TIMEOUT) {
            TerminateProcess(g_e2eProc.hProcess, 0);
            WaitForSingleObject(g_e2eProc.hProcess, 2000);
        }
        CloseHandle(g_e2eProc.hProcess);
        g_e2eProc.hProcess = NULL;
    }
    if (g_e2eProc.hThread) {
        CloseHandle(g_e2eProc.hThread);
        g_e2eProc.hThread = NULL;
    }
}

static void e2e_suite_setup(void) {
    memset(&g_e2eWin, 0, sizeof(g_e2eWin));
    memset(&g_e2eProc, 0, sizeof(g_e2eProc));
    memset(&g_e2eDaemonProc, 0, sizeof(g_e2eDaemonProc));
    memset(&g_e2eImg, 0, sizeof(g_e2eImg));
    g_e2eHookPid = 0;
    g_e2eHookActive = FALSE;
    g_e2eTreeA = NULL;
    g_e2eTreeB = NULL;
    g_e2eShmServer = NULL;
    g_e2eShmClient = NULL;
    g_e2ePipeServer = NULL;
    g_e2ePipeClient = NULL;
    g_e2eWorker = NULL;
    g_e2eBufA = NULL;
    g_e2eBufB = NULL;
    g_e2eBufC = NULL;
    g_e2eExtHwnd = NULL;
    g_e2eAuto = NULL;
    g_e2eWalker = NULL;
    g_e2eComInit = FALSE;
}

static void e2e_suite_teardown(void) {
    if (g_e2eHookPid != 0) {
        (void)Hook_DetachProcess(g_e2eHookPid, 1000U);
        g_e2eHookPid = 0;
    }
    if (g_e2eWorker) {
        WaitForSingleObject(g_e2eWorker, 3000);
        CloseHandle(g_e2eWorker);
        g_e2eWorker = NULL;
    }
    if (g_e2eHookActive) {
        Perception_StopEventHook();
        g_e2eHookActive = FALSE;
    }
    stop_e2e_ext_process(g_e2eExtHwnd);
    g_e2eExtHwnd = NULL;
    if (g_e2eDaemonProc.hProcess) {
        if (WaitForSingleObject(g_e2eDaemonProc.hProcess, 1500) == WAIT_TIMEOUT) {
            TerminateProcess(g_e2eDaemonProc.hProcess, 0);
            WaitForSingleObject(g_e2eDaemonProc.hProcess, 1500);
        }
        CloseHandle(g_e2eDaemonProc.hProcess);
        g_e2eDaemonProc.hProcess = NULL;
    }
    if (g_e2eDaemonProc.hThread) {
        CloseHandle(g_e2eDaemonProc.hThread);
        g_e2eDaemonProc.hThread = NULL;
    }
    stop_e2e_win32_window();

    CPOWER_SAFE_RELEASE(g_e2eWalker);
    CPOWER_SAFE_RELEASE(g_e2eAuto);
    if (g_e2eComInit) {
        CoUninitialize();
        g_e2eComInit = FALSE;
    }

    if (g_e2eTreeA) {
        Perception_FreeSemanticTree(g_e2eTreeA);
        g_e2eTreeA = NULL;
    }
    if (g_e2eTreeB) {
        Perception_FreeSemanticTree(g_e2eTreeB);
        g_e2eTreeB = NULL;
    }
    if (g_e2eImg.pixels) {
        Perception_FreeImageBuffer(&g_e2eImg);
        memset(&g_e2eImg, 0, sizeof(g_e2eImg));
    }
    if (g_e2eShmClient) {
        IPC_CloseShmChannel(g_e2eShmClient);
        g_e2eShmClient = NULL;
    }
    if (g_e2eShmServer) {
        IPC_CloseShmChannel(g_e2eShmServer);
        g_e2eShmServer = NULL;
    }
    if (g_e2ePipeClient) {
        IPC_ClosePipeServer(g_e2ePipeClient);
        g_e2ePipeClient = NULL;
    }
    if (g_e2ePipeServer) {
        IPC_ClosePipeServer(g_e2ePipeServer);
        g_e2ePipeServer = NULL;
    }

    CPOWER_SAFE_FREE(g_e2eBufA);
    CPOWER_SAFE_FREE(g_e2eBufB);
    CPOWER_SAFE_FREE(g_e2eBufC);

    Control_Shutdown();
    Perception_Shutdown();
}

/* ------------------------------------------------------------------------- */
/* Pure C COM UIA Element Resolver Helper (Server-Side Target Lookup)        */
/* ------------------------------------------------------------------------- */

typedef struct UiaDfsFrame {
    IUIAutomationElement* pElem;
    int                   depth;
} UiaDfsFrame;

static IUIAutomationElement* find_uia_element_for_node(
    IUIAutomation* pAuto,
    IUIAutomationTreeWalker* pWalker,
    HWND rootHwnd,
    const SemanticNode* pTargetNode
) {
    if (!pAuto || !pWalker || !rootHwnd || !pTargetNode) return NULL;

    IUIAutomationElement* pRoot = NULL;
    if (FAILED(IUIAutomation_ElementFromHandle(pAuto, (UIA_HWND)rootHwnd, &pRoot)) || !pRoot) {
        return NULL;
    }

    UiaDfsFrame stack[128];
    int top = 0;
    stack[top].pElem = pRoot;
    stack[top].depth = 0;
    top++;

    IUIAutomationElement* pMatch = NULL;

    while (top > 0) {
        top--;
        IUIAutomationElement* pCurr = stack[top].pElem;
        int depth = stack[top].depth;

        if (!pMatch) {
            CONTROLTYPEID ctype = 0;
            if (SUCCEEDED(IUIAutomationElement_get_CurrentControlType(pCurr, &ctype)) &&
                (uint16_t)ctype == pTargetNode->controlTypeId) {
                BOOL matched = FALSE;
                if (pTargetNode->automationId[0] != L'\0') {
                    BSTR bAuto = NULL;
                    if (SUCCEEDED(IUIAutomationElement_get_CurrentAutomationId(pCurr, &bAuto)) && bAuto) {
                        if (wcscmp(bAuto, pTargetNode->automationId) == 0) {
                            matched = TRUE;
                        }
                        SysFreeString(bAuto);
                    }
                }
                if (!matched && pTargetNode->name[0] != L'\0') {
                    BSTR bName = NULL;
                    if (SUCCEEDED(IUIAutomationElement_get_CurrentName(pCurr, &bName)) && bName) {
                        if (wcscmp(bName, pTargetNode->name) == 0) {
                            matched = TRUE;
                        }
                        SysFreeString(bName);
                    }
                }
                if (!matched && pTargetNode->name[0] == L'\0' && pTargetNode->automationId[0] == L'\0') {
                    matched = TRUE;
                }

                if (matched) {
                    pMatch = pCurr;
                    IUIAutomationElement_AddRef(pMatch);
                }
            }
        }

        if (!pMatch && depth < 16) {
            IUIAutomationElement* children[32];
            int childCount = 0;
            IUIAutomationElement* pChild = NULL;
            if (SUCCEEDED(IUIAutomationTreeWalker_GetFirstChildElement(pWalker, pCurr, &pChild)) && pChild) {
                while (pChild && childCount < 32) {
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
                for (int i = childCount - 1; i >= 0; i--) {
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
        if (pMatch) break;
    }

    while (top > 0) {
        top--;
        if (stack[top].pElem) {
            IUIAutomationElement_Release(stack[top].pElem);
        }
    }

    return pMatch;
}

/* ------------------------------------------------------------------------- */
/* Test 1 (Tier 4): Full Closed-Loop E2E (External GUI + Hook + SHM + UIA)   */
/* ------------------------------------------------------------------------- */

typedef struct E2EHookState {
    volatile LONG eventCount;
    volatile LONG hasValidEvent;
    WinEventRecord lastRecord;
} E2EHookState;

static void e2e_hook_callback(const WinEventRecord* pEvent, void* pUserData) {
    E2EHookState* st = (E2EHookState*)pUserData;
    if (!st || !pEvent) return;
    if (pEvent->hwnd != NULL && pEvent->event != 0U) {
        st->lastRecord = *pEvent;
        InterlockedExchange(&st->hasValidEvent, 1);
    }
    InterlockedIncrement(&st->eventCount);
}

typedef struct E2EShmClientArgs {
    ShmChannel* pClient;
    HWND        targetHwnd;
    volatile BOOL succeeded;
    char        failReason[256];
    uint32_t    parsedNodeCount;
    int32_t     editNodeId;
    int32_t     checkNodeId;
    int32_t     btnNodeId;
    int32_t     labelNodeId;
} E2EShmClientArgs;

static DWORD WINAPI e2e_shm_client_thread(LPVOID lpParam) {
    E2EShmClientArgs* args = (E2EShmClientArgs*)lpParam;
    args->succeeded = FALSE;
    args->editNodeId = -1;
    args->checkNodeId = -1;
    args->btnNodeId = -1;
    args->labelNodeId = -1;

    enum { RX_CAP = 128 * 1024 };
    uint8_t* rxBuf = (uint8_t*)calloc(1, RX_CAP);
    if (!rxBuf) {
        snprintf(args->failReason, sizeof(args->failReason), "Out of memory allocating rxBuf");
        return 1;
    }

    size_t rxBytes = 0;
    HRESULT hr = IPC_ReadTreePacket(args->pClient, rxBuf, RX_CAP, &rxBytes, 5000);
    if (FAILED(hr)) {
        snprintf(args->failReason, sizeof(args->failReason), "IPC_ReadTreePacket failed hr=0x%08lX", (unsigned long)hr);
        free(rxBuf);
        return 1;
    }
    if (rxBytes < sizeof(TreePacketHeader)) {
        snprintf(args->failReason, sizeof(args->failReason), "rxBytes too small (%zu)", rxBytes);
        free(rxBuf);
        return 1;
    }

    const TreePacketHeader* hdr = (const TreePacketHeader*)rxBuf;
    if (hdr->packetMagic != CPOWER_TREE_PACKET_MAGIC ||
        hdr->totalPayloadSize != (uint32_t)rxBytes ||
        hdr->rootNodeIndex != 0U ||
        hdr->nodeCount < 4U) {
        snprintf(args->failReason, sizeof(args->failReason),
                 "Invalid TreePacketHeader magic=0x%08lX size=%u nodeCount=%u",
                 (unsigned long)hdr->packetMagic, hdr->totalPayloadSize, hdr->nodeCount);
        free(rxBuf);
        return 1;
    }

    size_t needed = sizeof(TreePacketHeader) + (size_t)hdr->nodeCount * sizeof(SemanticNode);
    if (rxBytes < needed) {
        snprintf(args->failReason, sizeof(args->failReason),
                 "Truncated node payload: rxBytes=%zu needed=%zu", rxBytes, needed);
        free(rxBuf);
        return 1;
    }

    args->parsedNodeCount = hdr->nodeCount;
    const SemanticNode* nodes = (const SemanticNode*)(rxBuf + sizeof(TreePacketHeader));

    for (uint32_t i = 0; i < hdr->nodeCount; i++) {
        const SemanticNode* n = &nodes[i];
        if (n->controlTypeId == UIA_EditControlTypeId &&
            (wcscmp(n->value, L"BeforeE2E") == 0 ||
             wcscmp(n->name, L"E2EInputBox") == 0 ||
             wcscmp(n->automationId, L"E2EInputBox") == 0)) {
            args->editNodeId = (int32_t)n->id;
        } else if (n->controlTypeId == UIA_CheckBoxControlTypeId &&
                   (wcscmp(n->name, L"EnableFeatureE2E") == 0 ||
                    wcscmp(n->automationId, L"E2ECheckBox") == 0)) {
            args->checkNodeId = (int32_t)n->id;
        } else if (n->controlTypeId == UIA_ButtonControlTypeId &&
                   (wcscmp(n->name, L"ApplyActionBtn") == 0 ||
                    wcscmp(n->automationId, L"E2EApplyBtn") == 0)) {
            args->btnNodeId = (int32_t)n->id;
        } else if (n->controlTypeId == UIA_TextControlTypeId &&
                   (wcscmp(n->name, L"Status:0") == 0 ||
                    wcscmp(n->automationId, L"E2EStatusLbl") == 0)) {
            args->labelNodeId = (int32_t)n->id;
        }
    }

    if (args->editNodeId < 0 || args->checkNodeId < 0 ||
        args->btnNodeId < 0 || args->labelNodeId < 0) {
        snprintf(args->failReason, sizeof(args->failReason),
                 "Missing target nodes edit=%d chk=%d btn=%d lbl=%d",
                 args->editNodeId, args->checkNodeId, args->btnNodeId, args->labelNodeId);
        free(rxBuf);
        return 1;
    }

    /* Dispatch 3 UIA action commands + CMD_SHUTDOWN back to agent */
    CommandPacket cmds[4];
    memset(cmds, 0, sizeof(cmds));

    /* 1. Set Edit value */
    const SemanticNode* nEdit = &nodes[args->editNodeId];
    cmds[0].packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmds[0].commandId = 1;
    cmds[0].commandCode = CMD_UIA_SET_VALUE;
    cmds[0].executionMode = EXEC_MODE_DIRECT_INSTANT;
    cmds[0].targetX = (nEdit->bounds.left + nEdit->bounds.right) / 2;
    cmds[0].targetY = (nEdit->bounds.top + nEdit->bounds.bottom) / 2;
    cmds[0].targetHwnd = (uint64_t)(uintptr_t)args->targetHwnd;
    cmds[0].targetNodeId = nEdit->id;
    wcscpy(cmds[0].textPayload, L"AfterE2E_Verified");
    cmds[0].textPayloadBytes = (uint32_t)(wcslen(cmds[0].textPayload) * sizeof(WCHAR));

    /* 2. Toggle CheckBox */
    const SemanticNode* nChk = &nodes[args->checkNodeId];
    cmds[1].packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmds[1].commandId = 2;
    cmds[1].commandCode = CMD_UIA_TOGGLE;
    cmds[1].executionMode = EXEC_MODE_DIRECT_INSTANT;
    cmds[1].targetX = (nChk->bounds.left + nChk->bounds.right) / 2;
    cmds[1].targetY = (nChk->bounds.top + nChk->bounds.bottom) / 2;
    cmds[1].targetHwnd = (uint64_t)(uintptr_t)args->targetHwnd;
    cmds[1].targetNodeId = nChk->id;

    /* 3. Invoke Button */
    const SemanticNode* nBtn = &nodes[args->btnNodeId];
    cmds[2].packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmds[2].commandId = 3;
    cmds[2].commandCode = CMD_UIA_INVOKE;
    cmds[2].executionMode = EXEC_MODE_DIRECT_INSTANT;
    cmds[2].targetX = (nBtn->bounds.left + nBtn->bounds.right) / 2;
    cmds[2].targetY = (nBtn->bounds.top + nBtn->bounds.bottom) / 2;
    cmds[2].targetHwnd = (uint64_t)(uintptr_t)args->targetHwnd;
    cmds[2].targetNodeId = nBtn->id;

    /* 4. End of command stream */
    cmds[3].packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmds[3].commandId = 4;
    cmds[3].commandCode = CMD_SHUTDOWN;
    cmds[3].executionMode = EXEC_MODE_DIRECT_INSTANT;

    for (int i = 0; i < 4; i++) {
        hr = IPC_WriteCommandPacket(args->pClient, &cmds[i]);
        if (FAILED(hr)) {
            snprintf(args->failReason, sizeof(args->failReason),
                     "IPC_WriteCommandPacket[%d] failed hr=0x%08lX", i, (unsigned long)hr);
            free(rxBuf);
            return 1;
        }
    }

    free(rxBuf);
    args->succeeded = TRUE;
    return 0;
}

static void test_e2e_full_closed_loop_gui_winevent_shm_control(void) {
    ASSERT_HRESULT_OK(Perception_Init());
    ASSERT_HRESULT_OK(Control_Init());

    PerceptionConfig cfg = { 64, 1024, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&cfg));

    E2EHookState hookState;
    memset(&hookState, 0, sizeof(hookState));
    ASSERT_HRESULT_OK(Perception_StartEventHook(e2e_hook_callback, &hookState));
    g_e2eHookActive = TRUE;

    LONG seq = InterlockedIncrement(&g_e2eSeq);
    WCHAR winTitle[64];
    swprintf(winTitle, 64, L"CPowerE2ETarget_%lu_%ld", GetCurrentProcessId(), seq);

    WCHAR cmdLine[2048];
    swprintf(
        cmdLine, 2048,
        L"powershell.exe -NoProfile -NonInteractive -Command \""
        L"Add-Type -AssemblyName System.Windows.Forms; "
        L"$script:cnt=0; "
        L"$f=New-Object Windows.Forms.Form; $f.Text='%ls'; $f.Width=360; $f.Height=260; "
        L"$tb=New-Object Windows.Forms.TextBox; $tb.Text='BeforeE2E'; $tb.Name='E2EInputBox'; "
        L"$tb.AccessibleName='E2EInputBox'; $tb.SetBounds(20,20,260,24); $f.Controls.Add($tb); "
        L"$chk=New-Object Windows.Forms.CheckBox; $chk.Text='EnableFeatureE2E'; $chk.Name='E2ECheckBox'; "
        L"$chk.SetBounds(20,56,220,24); $f.Controls.Add($chk); "
        L"$lbl=New-Object Windows.Forms.Label; $lbl.Text='Status:0'; $lbl.Name='E2EStatusLbl'; "
        L"$lbl.SetBounds(20,90,180,24); $f.Controls.Add($lbl); "
        L"$btn=New-Object Windows.Forms.Button; $btn.Text='ApplyActionBtn'; $btn.Name='E2EApplyBtn'; "
        L"$btn.SetBounds(20,124,180,32); "
        L"$btn.Add_Click({ $script:cnt++; $lbl.Text='Status:'+$script:cnt }); $f.Controls.Add($btn); "
        L"$guard=New-Object Windows.Forms.Timer; $guard.Interval=25000; $guard.Add_Tick({$guard.Stop();$f.Close()}); "
        L"$f.Add_Shown({ $f.Activate(); $tb.Focus(); $guard.Start() }); "
        L"[Windows.Forms.Application]::Run($f)\"",
        winTitle
    );

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL spawned = CreateProcessW(
        NULL, cmdLine, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &g_e2eProc
    );
    ASSERT_TRUE(spawned);

    /* Wait up to 8000ms for target Form HWND to be created and visible */
    HWND targetHwnd = NULL;
    DWORD t0 = GetTickCount();
    while ((GetTickCount() - t0) < 8000U) {
        targetHwnd = FindWindowW(NULL, winTitle);
        if (targetHwnd && IsWindowVisible(targetHwnd)) {
            break;
        }
        Sleep(40);
    }
    ASSERT_NOT_NULL(targetHwnd);
    g_e2eExtHwnd = targetHwnd;

    /* Wait briefly for all child controls to appear in UIA tree */
    DWORD tTree = GetTickCount();
    BOOL readyTree = FALSE;
    while ((GetTickCount() - tTree) < 4000U) {
        if (g_e2eTreeA) {
            Perception_FreeSemanticTree(g_e2eTreeA);
            g_e2eTreeA = NULL;
        }
        if (SUCCEEDED(Perception_CaptureSemanticTree(targetHwnd, &g_e2eTreeA)) &&
            g_e2eTreeA && g_e2eTreeA->nodeCount >= 5U) {
            BOOL hasEdit = FALSE, hasBtn = FALSE, hasChk = FALSE, hasLbl = FALSE;
            for (uint32_t i = 0; i < g_e2eTreeA->nodeCount; i++) {
                const SemanticNode* n = &g_e2eTreeA->nodes[i];
                if (n->controlTypeId == UIA_EditControlTypeId) hasEdit = TRUE;
                else if (n->controlTypeId == UIA_ButtonControlTypeId &&
                         (wcscmp(n->name, L"ApplyActionBtn") == 0 || wcscmp(n->automationId, L"E2EApplyBtn") == 0)) hasBtn = TRUE;
                else if (n->controlTypeId == UIA_CheckBoxControlTypeId) hasChk = TRUE;
                else if (n->controlTypeId == UIA_TextControlTypeId &&
                         (wcscmp(n->name, L"Status:0") == 0 || wcscmp(n->automationId, L"E2EStatusLbl") == 0)) hasLbl = TRUE;
            }
            if (hasEdit && hasBtn && hasChk && hasLbl) {
                readyTree = TRUE;
                break;
            }
        }
        Sleep(50);
    }
    ASSERT_TRUE(readyTree);
    ASSERT_NOT_NULL(g_e2eTreeA);

    /* Serialize captured tree into binary TREE packet */
    enum { SER_CAP = 128 * 1024 };
    g_e2eBufA = calloc(1, SER_CAP);
    ASSERT_NOT_NULL(g_e2eBufA);
    size_t serBytes = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTree(g_e2eTreeA, g_e2eBufA, SER_CAP, &serBytes));
    ASSERT_TRUE(serBytes >= sizeof(TreePacketHeader));

    /* Create bidirectional SPSC ShmChannel and attach client */
    WCHAR shmName[128];
    swprintf(shmName, 128, L"Local\\CPowerE2E_Loop_%lu_%ld", GetCurrentProcessId(), seq);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 256U * 1024U, &g_e2eShmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_e2eShmClient));

    /* Launch client worker thread */
    E2EShmClientArgs clientArgs;
    memset(&clientArgs, 0, sizeof(clientArgs));
    clientArgs.pClient = g_e2eShmClient;
    clientArgs.targetHwnd = targetHwnd;
    g_e2eWorker = CreateThread(NULL, 0, e2e_shm_client_thread, &clientArgs, 0, NULL);
    ASSERT_NOT_NULL(g_e2eWorker);

    /* Server writes captured SemanticTree packet -> ShmChannel */
    ASSERT_HRESULT_OK(IPC_WriteTreePacket(g_e2eShmServer, g_e2eBufA, serBytes));

    /* Wait for client thread to inspect tree and push all CommandPackets */
    ASSERT_EQ_INT(WAIT_OBJECT_0, (int)WaitForSingleObject(g_e2eWorker, 10000));
    CloseHandle(g_e2eWorker);
    g_e2eWorker = NULL;
    if (!clientArgs.succeeded) {
        ASSERT_FAIL(clientArgs.failReason);
    }
    ASSERT_EQ_UINT(g_e2eTreeA->nodeCount, clientArgs.parsedNodeCount);

    /* Initialize pure C COM UIA resolver for server-side action execution */
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hrCom == S_OK || hrCom == S_FALSE) {
        g_e2eComInit = TRUE;
    }

    HRESULT hrResolver = CoCreateInstance(&kE2EClsidCUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                          &kE2EIidIUIAutomation, (void**)&g_e2eAuto);
    if (SUCCEEDED(hrResolver) && g_e2eAuto) {
        hrResolver = IUIAutomation_get_ControlViewWalker(g_e2eAuto, &g_e2eWalker);
    }
    ASSERT_HRESULT_OK(hrResolver);
    ASSERT_NOT_NULL(g_e2eAuto);
    ASSERT_NOT_NULL(g_e2eWalker);

    LONG eventsBeforeExec = InterlockedCompareExchange(&hookState.eventCount, 0, 0);

    /* Server reads commands from ShmChannel and drives target controls via Control Engine */
    int executedCount = 0;
    enum ToggleState toggledNewState = ToggleState_Off;
    for (;;) {
        CommandPacket cmd;
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_HRESULT_OK(IPC_ReadCommandPacket(g_e2eShmServer, &cmd, 3000));
        ASSERT_EQ_UINT(CPOWER_CMD_PACKET_MAGIC, cmd.packetMagic);

        if (cmd.commandCode == CMD_SHUTDOWN) {
            break;
        }

        ASSERT_TRUE(cmd.targetNodeId < g_e2eTreeA->nodeCount);
        const SemanticNode* nodeInfo = &g_e2eTreeA->nodes[cmd.targetNodeId];
        IUIAutomationElement* pElem = find_uia_element_for_node(g_e2eAuto, g_e2eWalker, targetHwnd, nodeInfo);
        ASSERT_NOT_NULL(pElem);

        HRESULT hrExec = E_FAIL;
        if (cmd.commandCode == CMD_UIA_SET_VALUE) {
            hrExec = Control_DirectSetValue(pElem, cmd.textPayload);
        } else if (cmd.commandCode == CMD_UIA_TOGGLE) {
            hrExec = Control_DirectToggle(pElem, &toggledNewState);
        } else if (cmd.commandCode == CMD_UIA_INVOKE) {
            hrExec = Control_DirectInvoke(pElem);
        }
        IUIAutomationElement_Release(pElem);
        ASSERT_HRESULT_OK(hrExec);
        executedCount++;
    }

    CPOWER_SAFE_RELEASE(g_e2eWalker);
    CPOWER_SAFE_RELEASE(g_e2eAuto);
    if (g_e2eComInit) {
        CoUninitialize();
        g_e2eComInit = FALSE;
    }

    ASSERT_EQ_INT(3, executedCount);
    ASSERT_EQ_INT((int)ToggleState_On, (int)toggledNewState);

    /* Re-capture SemanticTree and verify mutated state in external process UI */
    BOOL verifiedAfter = FALSE;
    DWORD tVerify = GetTickCount();
    while ((GetTickCount() - tVerify) < 3000U) {
        if (g_e2eTreeB) {
            Perception_FreeSemanticTree(g_e2eTreeB);
            g_e2eTreeB = NULL;
        }
        if (SUCCEEDED(Perception_CaptureSemanticTree(targetHwnd, &g_e2eTreeB)) && g_e2eTreeB) {
            BOOL sawUpdatedEdit = FALSE;
            BOOL sawUpdatedStatus = FALSE;
            for (uint32_t i = 0; i < g_e2eTreeB->nodeCount; i++) {
                const SemanticNode* n = &g_e2eTreeB->nodes[i];
                if (n->controlTypeId == UIA_EditControlTypeId &&
                    wcscmp(n->value, L"AfterE2E_Verified") == 0) {
                    sawUpdatedEdit = TRUE;
                } else if (n->controlTypeId == UIA_TextControlTypeId &&
                           wcscmp(n->name, L"Status:1") == 0) {
                    sawUpdatedStatus = TRUE;
                }
            }
            if (sawUpdatedEdit && sawUpdatedStatus) {
                verifiedAfter = TRUE;
                break;
            }
        }
        Sleep(40);
    }
    ASSERT_TRUE(verifiedAfter);

    /* Confirm real-time WinEvent hook observed cross-process events (incl. during actions) */
    WinEventRecord drained;
    while (Perception_PollEvent(&drained)) {
        /* Drain optional queued records */
    }
    LONG eventsAfterExec = InterlockedCompareExchange(&hookState.eventCount, 0, 0);
    ASSERT_TRUE(eventsAfterExec > 0);
    ASSERT_TRUE(eventsAfterExec >= eventsBeforeExec);
    ASSERT_EQ_INT(1, InterlockedCompareExchange(&hookState.hasValidEvent, 0, 0));

    /* Cleanly close external GUI process */
    stop_e2e_ext_process(targetHwnd);
    g_e2eExtHwnd = NULL;

    Perception_StopEventHook();
    g_e2eHookActive = FALSE;
}

/* ------------------------------------------------------------------------- */
/* Test 2 (Tier 4): Desktop / Shell Tree Extraction & Named Pipe Transport   */
/* ------------------------------------------------------------------------- */

typedef struct E2EPipeClientArgs {
    WCHAR    pipeName[MAX_PATH];
    uint32_t expectedNodeCount;
    size_t   expectedBytes;
    volatile BOOL succeeded;
    char     failReason[256];
} E2EPipeClientArgs;

static DWORD WINAPI e2e_pipe_client_thread(LPVOID lpParam) {
    E2EPipeClientArgs* args = (E2EPipeClientArgs*)lpParam;
    args->succeeded = FALSE;

    PipeServer* pClient = NULL;
    HRESULT hr = IPC_ConnectPipeClient(args->pipeName, 5000, &pClient);
    if (FAILED(hr) || !pClient) {
        snprintf(args->failReason, sizeof(args->failReason), "IPC_ConnectPipeClient failed hr=0x%08lX", (unsigned long)hr);
        return 1;
    }

    enum { PIPE_RX_CAP = 256 * 1024 };
    uint8_t* rxBuf = (uint8_t*)calloc(1, PIPE_RX_CAP);
    if (!rxBuf) {
        IPC_ClosePipeServer(pClient);
        snprintf(args->failReason, sizeof(args->failReason), "OOM");
        return 1;
    }

    uint32_t bytesRead = 0;
    hr = IPC_PipeRead(pClient, rxBuf, PIPE_RX_CAP, &bytesRead);
    if (FAILED(hr) || bytesRead != (uint32_t)args->expectedBytes || bytesRead < sizeof(TreePacketHeader)) {
        snprintf(args->failReason, sizeof(args->failReason),
                 "IPC_PipeRead mismatch hr=0x%08lX read=%u expected=%zu",
                 (unsigned long)hr, bytesRead, args->expectedBytes);
        free(rxBuf);
        IPC_ClosePipeServer(pClient);
        return 1;
    }

    TreePacketHeader hdr;
    memcpy(&hdr, rxBuf, sizeof(hdr));
    if (hdr.packetMagic != CPOWER_TREE_PACKET_MAGIC ||
        hdr.nodeCount != args->expectedNodeCount ||
        hdr.totalPayloadSize != bytesRead) {
        snprintf(args->failReason, sizeof(args->failReason),
                 "Invalid pipe TreePacketHeader magic=0x%08lX nodes=%u",
                 (unsigned long)hdr.packetMagic, hdr.nodeCount);
        free(rxBuf);
        IPC_ClosePipeServer(pClient);
        return 1;
    }

    static const char kAck[] = "TREE_ACK_OK";
    uint32_t ackWritten = 0;
    hr = IPC_PipeWrite(pClient, kAck, (uint32_t)sizeof(kAck), &ackWritten);
    if (FAILED(hr) || ackWritten != (uint32_t)sizeof(kAck)) {
        snprintf(args->failReason, sizeof(args->failReason), "IPC_PipeWrite ACK failed hr=0x%08lX", (unsigned long)hr);
        free(rxBuf);
        IPC_ClosePipeServer(pClient);
        return 1;
    }

    free(rxBuf);
    IPC_ClosePipeServer(pClient);
    args->succeeded = TRUE;
    return 0;
}

static void test_e2e_desktop_shell_tree_and_pipe_transport(void) {
    ASSERT_HRESULT_OK(Perception_Init());

    /* Bound depth & node count for predictable sub-second desktop/shell capture */
    PerceptionConfig bounded = { 3, 64, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&bounded));

    HWND shellHwnd = GetShellWindow();
    if (shellHwnd && IsWindow(shellHwnd)) {
        ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(shellHwnd, &g_e2eTreeA));
    } else {
        ASSERT_HRESULT_OK(Perception_CaptureDesktopTree(&g_e2eTreeA));
    }
    ASSERT_NOT_NULL(g_e2eTreeA);
    ASSERT_TRUE(g_e2eTreeA->nodeCount >= 1U && g_e2eTreeA->nodeCount <= 64U);
    ASSERT_EQ_INT(-1, g_e2eTreeA->nodes[0].parentId);

    enum { PKT_CAP = 256 * 1024, JSON_CAP = 256 * 1024 };
    g_e2eBufA = calloc(1, PKT_CAP);
    g_e2eBufB = calloc(1, JSON_CAP);
    ASSERT_NOT_NULL(g_e2eBufA);
    ASSERT_NOT_NULL(g_e2eBufB);

    size_t binBytes = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTree(g_e2eTreeA, g_e2eBufA, PKT_CAP, &binBytes));
    ASSERT_TRUE(binBytes >= sizeof(TreePacketHeader));

    /* Verify JSON & Indented formats also succeed on real desktop/shell tree */
    size_t txtBytes = 0;
    ASSERT_HRESULT_OK(Perception_SerializeTreeJson(g_e2eTreeA, (char*)g_e2eBufB, JSON_CAP, &txtBytes));
    ASSERT_TRUE(txtBytes > 10U);
    ASSERT_EQ_INT('{', ((const char*)g_e2eBufB)[0]);
    ASSERT_HRESULT_OK(Perception_SerializeTreeIndented(g_e2eTreeA, (char*)g_e2eBufB, JSON_CAP, &txtBytes));
    ASSERT_NOT_NULL(strstr((const char*)g_e2eBufB, "[0]"));

    /* Transmit binary TREE packet over cross-integrity Named Pipe */
    LONG seq = InterlockedIncrement(&g_e2eSeq);
    E2EPipeClientArgs pArgs;
    memset(&pArgs, 0, sizeof(pArgs));
    swprintf(pArgs.pipeName, MAX_PATH, L"\\\\.\\pipe\\cpower_e2e_%lu_%ld", GetCurrentProcessId(), seq);
    pArgs.expectedNodeCount = g_e2eTreeA->nodeCount;
    pArgs.expectedBytes = binBytes;

    ASSERT_HRESULT_OK(IPC_CreatePipeServer(pArgs.pipeName, &g_e2ePipeServer));
    ASSERT_NOT_NULL(g_e2ePipeServer);

    g_e2eWorker = CreateThread(NULL, 0, e2e_pipe_client_thread, &pArgs, 0, NULL);
    ASSERT_NOT_NULL(g_e2eWorker);

    ASSERT_HRESULT_OK(IPC_AcceptPipeClient(g_e2ePipeServer));

    uint32_t pipeWritten = 0;
    ASSERT_HRESULT_OK(IPC_PipeWrite(g_e2ePipeServer, g_e2eBufA, (uint32_t)binBytes, &pipeWritten));
    ASSERT_EQ_UINT((uint32_t)binBytes, pipeWritten);

    char ackBuf[32];
    memset(ackBuf, 0, sizeof(ackBuf));
    uint32_t ackRead = 0;
    ASSERT_HRESULT_OK(IPC_PipeRead(g_e2ePipeServer, ackBuf, sizeof(ackBuf), &ackRead));
    ASSERT_EQ_STR("TREE_ACK_OK", ackBuf);

    ASSERT_EQ_INT(WAIT_OBJECT_0, (int)WaitForSingleObject(g_e2eWorker, 5000));
    CloseHandle(g_e2eWorker);
    g_e2eWorker = NULL;
    if (!pArgs.succeeded) {
        ASSERT_FAIL(pArgs.failReason);
    }

    PerceptionConfig restore = { 64, 4096, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&restore));
}

/* ------------------------------------------------------------------------- */
/* Test 3 (Tier 3): Security-Gated Win32 Control & Dual Readback (GDI + UIA) */
/* ------------------------------------------------------------------------- */

static void test_e2e_security_gated_win32_control_and_fallback(void) {
    ASSERT_HRESULT_OK(Perception_Init());
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_e2e_win32_window());

    /* 1. Inspect Security & UIPI gate before driving target window */
    SecurityEnvironmentReport secRpt;
    memset(&secRpt, 0, sizeof(secRpt));
    ASSERT_HRESULT_OK(Security_GetEnvironmentReport(&secRpt));
    ASSERT_NE_INT(INTEGRITY_UNKNOWN, (int)secRpt.integrityLevel);
    ASSERT_TRUE(Security_CanInteractWithWindow(g_e2eWin.hwnd));

    /* 2. Drive Edit text and Button click via Direct Win32 Control */
    ASSERT_HRESULT_OK(Control_DirectSetTextHwnd(g_e2eWin.hEdit, L"DualVerified_E2E_77"));
    ASSERT_HRESULT_OK(Control_DirectClickButtonHwnd(g_e2eWin.hBtn));
    ASSERT_HRESULT_OK(Control_DirectClickButtonHwnd(g_e2eWin.hBtn));
    ASSERT_EQ_INT(2, InterlockedCompareExchange(&g_e2eWin.btnClicks, 0, 0));

    /* 3. Read back via BOTH GDI fallback AND COM UIA SemanticTree */
    WCHAR gdiEdit[128];
    memset(gdiEdit, 0, sizeof(gdiEdit));
    ASSERT_HRESULT_OK(Perception_FallbackExtractText(g_e2eWin.hEdit, gdiEdit, 128));
    ASSERT_EQ_WSTR(L"DualVerified_E2E_77", gdiEdit);

    WCHAR gdiLabel[128];
    memset(gdiLabel, 0, sizeof(gdiLabel));
    ASSERT_HRESULT_OK(Perception_FallbackExtractText(g_e2eWin.hStatic, gdiLabel, 128));
    ASSERT_EQ_WSTR(L"Clicks:2", gdiLabel);

    ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(g_e2eWin.hwnd, &g_e2eTreeA));
    ASSERT_NOT_NULL(g_e2eTreeA);

    BOOL uiaSawEdit = FALSE;
    BOOL uiaSawLabel = FALSE;
    for (uint32_t i = 0; i < g_e2eTreeA->nodeCount; i++) {
        const SemanticNode* n = &g_e2eTreeA->nodes[i];
        if (n->controlTypeId == UIA_EditControlTypeId &&
            (wcscmp(n->value, L"DualVerified_E2E_77") == 0 || wcscmp(n->name, L"DualVerified_E2E_77") == 0)) {
            uiaSawEdit = TRUE;
        } else if (n->controlTypeId == UIA_TextControlTypeId &&
                   wcscmp(n->name, L"Clicks:2") == 0) {
            uiaSawLabel = TRUE;
        }
    }
    ASSERT_TRUE(uiaSawEdit);
    ASSERT_TRUE(uiaSawLabel);
}

/* ------------------------------------------------------------------------- */
/* Test 4 (Tier 3): Micro-Region Capture / PNG Encode / SHM Image Transfer   */
/* ------------------------------------------------------------------------- */

static uint32_t e2e_read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t e2e_png_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static void test_e2e_microregion_encode_and_shm_image_transfer(void) {
    ASSERT_HRESULT_OK(Perception_Init());

    PerceptionConfig cfg = { 64, 4096, TRUE, TRUE };
    ASSERT_HRESULT_OK(Perception_Configure(&cfg));

    /* Attempt live DXGI VRAM micro-region capture first */
    HRESULT hrDxgi = Perception_CaptureMicroRegion(0, 0, 32, 32, &g_e2eImg);
    if (FAILED(hrDxgi)) {
        /*
         * Non-default/headless desktops (e.g. WinSta0\exebox-*) deny DuplicateOutput
         * with E_ACCESSDENIED (0x80070005) or DXGI_ERROR_ACCESS_LOST. Fall back to
         * a deterministic BGRA32 image buffer so the full encode + SHM IPC pipeline runs.
         */
        enum { DW = 32, DH = 32 };
        g_e2eImg.width = DW;
        g_e2eImg.height = DH;
        g_e2eImg.stride = DW * 4;
        g_e2eImg.dataSize = (size_t)(DW * DH * 4);
        g_e2eImg.pixels = (uint8_t*)calloc(1, g_e2eImg.dataSize);
        ASSERT_NOT_NULL(g_e2eImg.pixels);
        for (int y = 0; y < DH; y++) {
            for (int x = 0; x < DW; x++) {
                size_t idx = (size_t)(y * DW + x) * 4U;
                g_e2eImg.pixels[idx + 0] = (uint8_t)((x * 7) & 0xFF);
                g_e2eImg.pixels[idx + 1] = (uint8_t)((y * 7) & 0xFF);
                g_e2eImg.pixels[idx + 2] = (uint8_t)(((x + y) * 3) & 0xFF);
                g_e2eImg.pixels[idx + 3] = 255U;
            }
        }
    }

    ASSERT_TRUE(g_e2eImg.width > 0 && g_e2eImg.height > 0);
    ASSERT_NOT_NULL(g_e2eImg.pixels);

    /* Encode both BMP and lossless PNG in memory */
    enum { IMG_ENC_CAP = 32768 };
    g_e2eBufA = calloc(1, IMG_ENC_CAP);
    g_e2eBufB = calloc(1, IMG_ENC_CAP);
    g_e2eBufC = calloc(1, IMG_ENC_CAP);
    ASSERT_NOT_NULL(g_e2eBufA);
    ASSERT_NOT_NULL(g_e2eBufB);
    ASSERT_NOT_NULL(g_e2eBufC);

    size_t bmpBytes = 0;
    ASSERT_HRESULT_OK(Perception_EncodeBmpInMemory(&g_e2eImg, (uint8_t*)g_e2eBufA, IMG_ENC_CAP, &bmpBytes));
    ASSERT_EQ_INT('B', ((const uint8_t*)g_e2eBufA)[0]);
    ASSERT_EQ_INT('M', ((const uint8_t*)g_e2eBufA)[1]);

    size_t pngBytes = 0;
    ASSERT_HRESULT_OK(Perception_EncodePngInMemory(&g_e2eImg, (uint8_t*)g_e2eBufB, IMG_ENC_CAP, &pngBytes));
    ASSERT_TRUE(pngBytes > 45U && pngBytes <= IMG_ENC_CAP);

    /* Transfer PNG payload over ShmChannel as IPC_PKT_IMAGE */
    LONG seq = InterlockedIncrement(&g_e2eSeq);
    WCHAR shmName[128];
    swprintf(shmName, 128, L"Local\\CPowerE2E_Img_%lu_%ld", GetCurrentProcessId(), seq);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 64U * 1024U, &g_e2eShmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_e2eShmClient));

    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_e2eShmServer, TRUE, IPC_PKT_IMAGE,
                                        g_e2eBufB, (uint32_t)pngBytes));

    uint16_t rxType = 0;
    uint32_t rxBytes = 0;
    ASSERT_HRESULT_OK(IPC_ReadRawFrame(g_e2eShmClient, FALSE, &rxType,
                                       g_e2eBufC, IMG_ENC_CAP, &rxBytes, 1000));
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_IMAGE, rxType);
    ASSERT_EQ_UINT((uint32_t)pngBytes, rxBytes);
    ASSERT_EQ_INT(0, memcmp(g_e2eBufB, g_e2eBufC, pngBytes));

    /* Validate PNG signature + chunk CRC-32 checksums on received buffer */
    static const uint8_t kPngSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const uint8_t* png = (const uint8_t*)g_e2eBufC;
    ASSERT_EQ_INT(0, memcmp(png, kPngSig, 8));

    size_t off = 8;
    BOOL sawIhdr = FALSE, sawIdat = FALSE, sawIend = FALSE;
    while (off + 12 <= (size_t)rxBytes) {
        uint32_t clen = e2e_read_be32(png + off);
        ASSERT_TRUE(off + 12ULL + (uint64_t)clen <= (uint64_t)rxBytes);
        uint32_t crcActual = e2e_png_crc32(png + off + 4, (size_t)clen + 4U);
        uint32_t crcFile = e2e_read_be32(png + off + 8 + clen);
        ASSERT_EQ_UINT(crcFile, crcActual);
        if (memcmp(png + off + 4, "IHDR", 4) == 0) sawIhdr = TRUE;
        if (memcmp(png + off + 4, "IDAT", 4) == 0) sawIdat = TRUE;
        if (memcmp(png + off + 4, "IEND", 4) == 0) sawIend = TRUE;
        off += 12U + (size_t)clen;
    }
    ASSERT_TRUE(sawIhdr && sawIdat && sawIend);
    ASSERT_EQ_UINT((size_t)rxBytes, off);
}

/* ------------------------------------------------------------------------- */
/* Test 5 (Tier 3): SemanticNode Bounds -> Bezier Trajectory -> Win32 Click  */
/* ------------------------------------------------------------------------- */

static void test_e2e_trajectory_target_hit_win32_and_verify(void) {
    ASSERT_HRESULT_OK(Perception_Init());
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_e2e_win32_window());

    ASSERT_HRESULT_OK(Perception_CaptureSemanticTree(g_e2eWin.hwnd, &g_e2eTreeA));
    ASSERT_NOT_NULL(g_e2eTreeA);

    RECT btnRect = { 0, 0, 0, 0 };
    BOOL foundBtn = FALSE;
    for (uint32_t i = 0; i < g_e2eTreeA->nodeCount; i++) {
        const SemanticNode* n = &g_e2eTreeA->nodes[i];
        if (n->controlTypeId == UIA_ButtonControlTypeId &&
            wcscmp(n->name, L"ExecuteE2E") == 0) {
            btnRect = n->bounds;
            foundBtn = TRUE;
            break;
        }
    }
    ASSERT_TRUE(foundBtn);
    ASSERT_TRUE(btnRect.right > btnRect.left);
    ASSERT_TRUE(btnRect.bottom > btnRect.top);

    int targetX = (btnRect.left + btnRect.right) / 2;
    int targetY = (btnRect.top + btnRect.bottom) / 2;
    int startX  = targetX - 180;
    int startY  = targetY - 120;

    TrajectoryConfig tCfg = { 1.0, 1.0, 1.0, 15.0, 2.5, 32 };
    BezierPoint pts[512];
    int ptCount = 0;
    ASSERT_HRESULT_OK(Control_GenerateTrajectoryPoints(
        startX, startY, targetX, targetY, &tCfg, pts, 512, &ptCount
    ));
    ASSERT_TRUE(ptCount >= 32 && ptCount <= 512);

    /* Confirm landing point falls strictly inside perceived button bounding rect */
    POINT landPt;
    landPt.x = (LONG)llround(pts[ptCount - 1].x);
    landPt.y = (LONG)llround(pts[ptCount - 1].y);
    ASSERT_TRUE(PtInRect(&btnRect, landPt));

    /* Confirm normalized SendInput virtual desktop mapping stays within [0, 65535] */
    LONG vNormX = -1, vNormY = -1;
    Control_ScreenToVirtualDesktop(landPt.x, landPt.y, &vNormX, &vNormY);
    ASSERT_TRUE(vNormX >= 0 && vNormX <= 65535);
    ASSERT_TRUE(vNormY >= 0 && vNormY <= 65535);

    /* Dispatch direct client click at coordinates below child controls (window body) */
    RECT clientRect;
    ASSERT_TRUE(GetClientRect(g_e2eWin.hwnd, &clientRect));
    int clickCX = (clientRect.right > 300) ? 280 : (clientRect.right / 2);
    int clickCY = (clientRect.bottom > 220) ? 200 : (clientRect.bottom - 20);
    ASSERT_HRESULT_OK(Control_DirectClickHwnd(g_e2eWin.hwnd, clickCX, clickCY));

    DWORD t0 = GetTickCount();
    while (InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) == 0 &&
           (GetTickCount() - t0) < 1000U) {
        Sleep(10);
    }
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) >= 1);
    ASSERT_EQ_INT(clickCX, InterlockedCompareExchange(&g_e2eWin.lastClientX, 0, 0));
    ASSERT_EQ_INT(clickCY, InterlockedCompareExchange(&g_e2eWin.lastClientY, 0, 0));
}

/* ------------------------------------------------------------------------- */
/* Test 6 (Tier 4): Daemon SHM Node-ID Resolution, AckPacket, Diff, Windows  */
/* ------------------------------------------------------------------------- */

static BOOL resolve_repo_bin_file(const WCHAR* leafName, WCHAR* outPath, size_t cap) {
    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        WCHAR* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            WCHAR* parentSlash = wcsrchr(exePath, L'\\');
            if (parentSlash && _wcsicmp(parentSlash + 1, L"tests") == 0) {
                *parentSlash = L'\0';
                swprintf(outPath, cap, L"%ls\\bin\\%ls", exePath, leafName);
            } else {
                swprintf(outPath, cap, L"%ls\\%ls", exePath, leafName);
            }
            if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
                return TRUE;
            }
        }
    }
    swprintf(outPath, cap, L"bin\\%ls", leafName);
    return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
}

static void e2e_send_daemon_cmd_expect_ack(
    ShmChannel* pCli,
    const CommandPacket* pCmd,
    AckPacket* pAckOut,
    uint16_t* pPayloadTypeOut,
    uint8_t* pPayloadBuf,
    uint32_t payloadCap,
    uint32_t* pPayloadBytesOut,
    LONG* pEventCount
) {
    if (pPayloadTypeOut) *pPayloadTypeOut = (uint16_t)IPC_PKT_UNKNOWN;
    if (pPayloadBytesOut) *pPayloadBytesOut = 0U;

    ASSERT_HRESULT_OK(IPC_WriteCommandPacket(pCli, pCmd));

    BOOL gotAck = FALSE;
    DWORD tStart = GetTickCount();
    while ((GetTickCount() - tStart) < 8000U) {
        uint16_t pktType = 0U;
        uint32_t rxBytes = 0U;
        ASSERT_HRESULT_OK(IPC_ReadRawFrame(pCli, FALSE, &pktType, pPayloadBuf, payloadCap, &rxBytes, 5000U));
        if (pktType == IPC_PKT_EVENT) {
            if (rxBytes >= sizeof(EventPacket)) {
                EventPacket ev;
                memcpy(&ev, pPayloadBuf, sizeof(ev));
                ASSERT_EQ_UINT(CPOWER_EVENT_PACKET_MAGIC, ev.magic);
                if (pEventCount) (*pEventCount)++;
            }
            continue;
        }
        if (pktType == IPC_PKT_ACK) {
            ASSERT_TRUE(rxBytes >= sizeof(AckPacket));
            memcpy(pAckOut, pPayloadBuf, sizeof(AckPacket));
            ASSERT_EQ_UINT(CPOWER_ACK_PACKET_MAGIC, pAckOut->magic);
            if (pAckOut->commandId == pCmd->commandId) {
                gotAck = TRUE;
                break;
            }
        }
    }
    ASSERT_TRUE(gotAck);

    if (pAckOut->flags != CPOWER_ACK_FLAG_NONE) {
        BOOL gotPayload = FALSE;
        DWORD tPay = GetTickCount();
        while ((GetTickCount() - tPay) < 8000U) {
            uint16_t pktType = 0U;
            uint32_t rxBytes = 0U;
            ASSERT_HRESULT_OK(IPC_ReadRawFrame(pCli, FALSE, &pktType, pPayloadBuf, payloadCap, &rxBytes, 5000U));
            if (pktType == IPC_PKT_EVENT) {
                if (pEventCount) (*pEventCount)++;
                continue;
            }
            if (pPayloadTypeOut) *pPayloadTypeOut = pktType;
            if (pPayloadBytesOut) *pPayloadBytesOut = rxBytes;
            gotPayload = TRUE;
            break;
        }
        ASSERT_TRUE(gotPayload);
    }
}

static uint32_t e2e_find_node_id_in_tree_pkt(const uint8_t* pktBuf, uint32_t pktBytes, uint16_t controlTypeId) {
    if (!pktBuf || pktBytes < sizeof(TreePacketHeader)) return 0U;
    TreePacketHeader hdr;
    memcpy(&hdr, pktBuf, sizeof(hdr));
    if (hdr.packetMagic != CPOWER_TREE_PACKET_MAGIC) return 0U;
    for (uint32_t i = 0; i < hdr.nodeCount; i++) {
        size_t off = sizeof(TreePacketHeader) + (size_t)i * sizeof(SemanticNode);
        if (off + sizeof(SemanticNode) > (size_t)pktBytes) break;
        SemanticNode n;
        memcpy(&n, pktBuf + off, sizeof(n));
        if (n.controlTypeId == controlTypeId) {
            return n.id;
        }
    }
    return 0U;
}

static void test_e2e_daemon_shm_node_id_ack_and_diff(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_e2e_win32_window());

    WCHAR cpowerExe[MAX_PATH];
    ASSERT_TRUE(resolve_repo_bin_file(L"cpower.exe", cpowerExe, MAX_PATH));

    LONG seq = InterlockedIncrement(&g_e2eSeq);
    WCHAR shmName[128];
    swprintf(shmName, 128, L"Local\\CPowerE2E_Daemon_%lu_%ld", GetCurrentProcessId(), seq);

    WCHAR daemonCmd[768];
    swprintf(daemonCmd, 768, L"\"%ls\" daemon --transport shm --name %ls --stream-events --duration 20000",
             cpowerExe, shmName);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL okSpawn = CreateProcessW(
        NULL, daemonCmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &g_e2eDaemonProc
    );
    ASSERT_TRUE(okSpawn);

    DWORD tConn = GetTickCount();
    while ((GetTickCount() - tConn) < 5000U) {
        if (SUCCEEDED(IPC_OpenShmChannel(shmName, &g_e2eShmClient)) && g_e2eShmClient) {
            break;
        }
        Sleep(20);
    }
    ASSERT_NOT_NULL(g_e2eShmClient);

    enum { RX_CAP = 512 * 1024 };
    g_e2eBufA = calloc(1, RX_CAP);
    ASSERT_NOT_NULL(g_e2eBufA);

    LONG eventCount = 0;
    CommandPacket cmd;
    AckPacket ack;
    uint16_t pktType = 0U;
    uint32_t rxBytes = 0U;

    /* 1. CMD_LIST_WINDOWS */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 1U;
    cmd.commandCode = CMD_LIST_WINDOWS;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_WINDOWS, pktType);
    ASSERT_TRUE(rxBytes >= sizeof(WindowPacketHeader));
    WindowPacketHeader wHdr;
    memcpy(&wHdr, g_e2eBufA, sizeof(wHdr));
    ASSERT_EQ_UINT(CPOWER_WINDOWS_PACKET_MAGIC, wHdr.packetMagic);
    ASSERT_TRUE(wHdr.windowCount >= 1U);

    /* 2. CMD_CAPTURE_TREE on g_e2eWin.hwnd */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 2U;
    cmd.commandCode = CMD_CAPTURE_TREE;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE, pktType);
    ASSERT_TRUE((ack.flags & CPOWER_ACK_FLAG_HAS_TREE) != 0U);

    uint32_t editNodeId = e2e_find_node_id_in_tree_pkt((const uint8_t*)g_e2eBufA, rxBytes, (uint16_t)UIA_EditControlTypeId);
    ASSERT_TRUE(editNodeId > 0U);

    /* 3. CMD_UIA_SET_VALUE by targetNodeId with targetX=0, targetY=0 */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 3U;
    cmd.commandCode = CMD_UIA_SET_VALUE;
    cmd.executionMode = EXEC_MODE_DIRECT_INSTANT;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    cmd.targetNodeId = editNodeId;
    wcscpy(cmd.textPayload, L"DaemonNodeId_110");
    cmd.textPayloadBytes = (uint32_t)(wcslen(cmd.textPayload) * sizeof(WCHAR));
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE, pktType);

    WCHAR editBuf[128];
    memset(editBuf, 0, sizeof(editBuf));
    GetWindowTextW(g_e2eWin.hEdit, editBuf, 128);
    ASSERT_EQ_WSTR(L"DaemonNodeId_110", editBuf);

    /* 4. CMD_UIA_INVOKE by targetNodeId resolved from daemon's updated tree */
    uint32_t btnNodeId = e2e_find_node_id_in_tree_pkt((const uint8_t*)g_e2eBufA, rxBytes, (uint16_t)UIA_ButtonControlTypeId);
    ASSERT_TRUE(btnNodeId > 0U);

    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 4U;
    cmd.commandCode = CMD_UIA_INVOKE;
    cmd.executionMode = EXEC_MODE_DIRECT_INSTANT;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    cmd.targetNodeId = btnNodeId;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.btnClicks, 0, 0) >= 1);

    /* 5. Mutate target directly and verify CMD_DIFF_TREE produces a delta packet */
    ASSERT_HRESULT_OK(Control_DirectSetTextHwnd(g_e2eWin.hEdit, L"DaemonDiffDelta_220"));
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 5U;
    cmd.commandCode = CMD_DIFF_TREE;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE_DIFF, pktType);
    ASSERT_TRUE((ack.flags & CPOWER_ACK_FLAG_HAS_DIFF) != 0U);
    ASSERT_TRUE(ack.diffCount >= 1U);

    DiffPacketHeader dHdr;
    memcpy(&dHdr, g_e2eBufA, sizeof(dHdr));
    ASSERT_EQ_UINT(CPOWER_DIFF_PACKET_MAGIC, dHdr.packetMagic);
    ASSERT_TRUE(dHdr.entryCount >= 1U);

    /* 6. Unknown command must return E_NOTIMPL with NO tree payload attached */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 6U;
    cmd.commandCode = 9999U;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_EQ_HRESULT(E_NOTIMPL, (HRESULT)ack.hresult);
    ASSERT_EQ_UINT(CPOWER_ACK_FLAG_NONE, ack.flags);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_UNKNOWN, pktType);
    ASSERT_EQ_UINT(0U, rxBytes);

    /* 7. Clean CMD_SHUTDOWN */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 7U;
    cmd.commandCode = CMD_SHUTDOWN;
    e2e_send_daemon_cmd_expect_ack(g_e2eShmClient, &cmd, &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);

    ASSERT_EQ_UINT(WAIT_OBJECT_0, WaitForSingleObject(g_e2eDaemonProc.hProcess, 4000U));
    DWORD daemonExit = 1U;
    ASSERT_TRUE(GetExitCodeProcess(g_e2eDaemonProc.hProcess, &daemonExit));
    ASSERT_EQ_UINT(0U, daemonExit);
    CloseHandle(g_e2eDaemonProc.hProcess);
    g_e2eDaemonProc.hProcess = NULL;
    CloseHandle(g_e2eDaemonProc.hThread);
    g_e2eDaemonProc.hThread = NULL;
}

/* ------------------------------------------------------------------------- */
/* Test 6B (Tier 4): Daemon Named Pipe Framing, Watchdog & Edge Flags        */
/* ------------------------------------------------------------------------- */

static void e2e_pipe_read_framed(
    PipeServer* pClient,
    uint8_t* pFrameScratch,
    uint32_t scratchCap,
    uint16_t* pOutType,
    uint8_t* pOutPayload,
    uint32_t payloadCap,
    uint32_t* pOutPayloadBytes
) {
    uint32_t bytesRead = 0U;
    ASSERT_HRESULT_OK(IPC_PipeRead(pClient, pFrameScratch, scratchCap, &bytesRead));
    ASSERT_TRUE(bytesRead >= sizeof(IPC_FrameHeader));

    IPC_FrameHeader hdr;
    memcpy(&hdr, pFrameScratch, sizeof(hdr));
    ASSERT_EQ_UINT(bytesRead - (uint32_t)sizeof(IPC_FrameHeader), hdr.payloadSize);
    ASSERT_TRUE(hdr.payloadSize <= payloadCap);

    *pOutType = hdr.packetType;
    *pOutPayloadBytes = hdr.payloadSize;
    if (hdr.payloadSize > 0U && pOutPayload) {
        memcpy(pOutPayload, pFrameScratch + sizeof(IPC_FrameHeader), hdr.payloadSize);
    }
}

static void e2e_send_pipe_cmd_expect_ack(
    PipeServer* pClient,
    const CommandPacket* pCmd,
    uint8_t* pFrameScratch,
    uint32_t scratchCap,
    AckPacket* pAckOut,
    uint16_t* pPayloadTypeOut,
    uint8_t* pPayloadBuf,
    uint32_t payloadCap,
    uint32_t* pPayloadBytesOut,
    LONG* pEventCount
) {
    if (pPayloadTypeOut) *pPayloadTypeOut = (uint16_t)IPC_PKT_UNKNOWN;
    if (pPayloadBytesOut) *pPayloadBytesOut = 0U;

    uint32_t written = 0U;
    ASSERT_HRESULT_OK(IPC_PipeWrite(pClient, pCmd, (uint32_t)sizeof(CommandPacket), &written));
    ASSERT_EQ_UINT((uint32_t)sizeof(CommandPacket), written);

    BOOL gotAck = FALSE;
    DWORD tStart = GetTickCount();
    while ((GetTickCount() - tStart) < 8000U) {
        uint16_t pktType = 0U;
        uint32_t rxBytes = 0U;
        e2e_pipe_read_framed(pClient, pFrameScratch, scratchCap, &pktType, pPayloadBuf, payloadCap, &rxBytes);
        if (pktType == IPC_PKT_EVENT) {
            if (rxBytes >= sizeof(EventPacket)) {
                EventPacket ev;
                memcpy(&ev, pPayloadBuf, sizeof(ev));
                ASSERT_EQ_UINT(CPOWER_EVENT_PACKET_MAGIC, ev.magic);
                if (pEventCount) (*pEventCount)++;
            }
            continue;
        }
        if (pktType == IPC_PKT_ACK) {
            ASSERT_TRUE(rxBytes >= sizeof(AckPacket));
            memcpy(pAckOut, pPayloadBuf, sizeof(AckPacket));
            ASSERT_EQ_UINT(CPOWER_ACK_PACKET_MAGIC, pAckOut->magic);
            if (pAckOut->commandId == pCmd->commandId) {
                gotAck = TRUE;
                break;
            }
        }
    }
    ASSERT_TRUE(gotAck);

    if (pAckOut->flags != CPOWER_ACK_FLAG_NONE) {
        BOOL gotPayload = FALSE;
        DWORD tPay = GetTickCount();
        while ((GetTickCount() - tPay) < 8000U) {
            uint16_t pktType = 0U;
            uint32_t rxBytes = 0U;
            e2e_pipe_read_framed(pClient, pFrameScratch, scratchCap, &pktType, pPayloadBuf, payloadCap, &rxBytes);
            if (pktType == IPC_PKT_EVENT) {
                if (pEventCount) (*pEventCount)++;
                continue;
            }
            if (pPayloadTypeOut) *pPayloadTypeOut = pktType;
            if (pPayloadBytesOut) *pPayloadBytesOut = rxBytes;
            gotPayload = TRUE;
            break;
        }
        ASSERT_TRUE(gotPayload);
    }
}

static void test_e2e_daemon_pipe_framed_and_edge_flags(void) {
    ASSERT_HRESULT_OK(Control_Init());
    ASSERT_TRUE(start_e2e_win32_window());

    WCHAR cpowerExe[MAX_PATH];
    ASSERT_TRUE(resolve_repo_bin_file(L"cpower.exe", cpowerExe, MAX_PATH));

    /* Part A: Short --duration Named Pipe daemon without a client must exit cleanly via watchdog */
    LONG seqWd = InterlockedIncrement(&g_e2eSeq);
    WCHAR wdPipeName[128];
    swprintf(wdPipeName, 128, L"\\\\.\\pipe\\cpower_e2e_wd_%lu_%ld", GetCurrentProcessId(), seqWd);

    WCHAR wdCmd[768];
    swprintf(wdCmd, 768, L"\"%ls\" daemon --transport pipe --name %ls --duration 80", cpowerExe, wdPipeName);

    STARTUPINFOW siWd;
    memset(&siWd, 0, sizeof(siWd));
    siWd.cb = sizeof(siWd);
    ASSERT_TRUE(CreateProcessW(NULL, wdCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &siWd, &g_e2eDaemonProc));
    ASSERT_EQ_UINT(WAIT_OBJECT_0, WaitForSingleObject(g_e2eDaemonProc.hProcess, 3500U));
    DWORD wdExit = 1U;
    ASSERT_TRUE(GetExitCodeProcess(g_e2eDaemonProc.hProcess, &wdExit));
    ASSERT_EQ_UINT(0U, wdExit);
    CloseHandle(g_e2eDaemonProc.hProcess);
    g_e2eDaemonProc.hProcess = NULL;
    CloseHandle(g_e2eDaemonProc.hThread);
    g_e2eDaemonProc.hThread = NULL;

    /* Part B: Framed Named Pipe session with --stream-events, E_NOTIMPL, nodeId==0, and (0,0) coords */
    LONG seq = InterlockedIncrement(&g_e2eSeq);
    WCHAR pipeName[128];
    swprintf(pipeName, 128, L"\\\\.\\pipe\\cpower_e2e_daemon_%lu_%ld", GetCurrentProcessId(), seq);

    WCHAR daemonCmd[768];
    swprintf(daemonCmd, 768, L"\"%ls\" daemon --transport pipe --name %ls --stream-events --duration 20000",
             cpowerExe, pipeName);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ASSERT_TRUE(CreateProcessW(NULL, daemonCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_e2eDaemonProc));

    DWORD tConn = GetTickCount();
    while ((GetTickCount() - tConn) < 5000U) {
        if (SUCCEEDED(IPC_ConnectPipeClient(pipeName, 500U, &g_e2ePipeClient)) && g_e2ePipeClient) {
            break;
        }
        Sleep(20);
    }
    ASSERT_NOT_NULL(g_e2ePipeClient);

    enum { RX_CAP = 512 * 1024, SCRATCH_CAP = 512 * 1024 + 64 };
    g_e2eBufA = calloc(1, RX_CAP);
    g_e2eBufB = calloc(1, SCRATCH_CAP);
    ASSERT_NOT_NULL(g_e2eBufA);
    ASSERT_NOT_NULL(g_e2eBufB);

    LONG eventCount = 0;
    CommandPacket cmd;
    AckPacket ack;
    uint16_t pktType = 0U;
    uint32_t rxBytes = 0U;

    /* 1. Framed CMD_LIST_WINDOWS */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 101U;
    cmd.commandCode = CMD_LIST_WINDOWS;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_TRUE((ack.flags & CPOWER_ACK_FLAG_HAS_WINDOWS) != 0U);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_WINDOWS, pktType);
    ASSERT_TRUE(rxBytes >= sizeof(WindowPacketHeader));
    WindowPacketHeader wHdr;
    memcpy(&wHdr, g_e2eBufA, sizeof(wHdr));
    ASSERT_EQ_UINT(CPOWER_WINDOWS_PACKET_MAGIC, wHdr.packetMagic);
    ASSERT_TRUE(wHdr.windowCount >= 1U);

    /* 2. Framed CMD_CAPTURE_TREE on g_e2eWin.hwnd */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 102U;
    cmd.commandCode = CMD_CAPTURE_TREE;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_TRUE((ack.flags & CPOWER_ACK_FLAG_HAS_TREE) != 0U);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE, pktType);
    uint32_t btnNodeId = e2e_find_node_id_in_tree_pkt((const uint8_t*)g_e2eBufA, rxBytes, (uint16_t)UIA_ButtonControlTypeId);
    ASSERT_TRUE(btnNodeId > 0U);

    /* 3. Unknown command returns E_NOTIMPL without attaching a spurious SemanticTree */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 103U;
    cmd.commandCode = 9999U;
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_EQ_HRESULT(E_NOTIMPL, (HRESULT)ack.hresult);
    ASSERT_EQ_UINT(CPOWER_ACK_FLAG_NONE, ack.flags);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_UNKNOWN, pktType);
    ASSERT_EQ_UINT(0U, rxBytes);

    /* 4. targetNodeId == 0 with CPOWER_EXEC_FLAG_HAS_NODE_ID resolves node 0 center (NOT (0,0)) */
    LONG beforeNode0Clicks = InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0);
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 104U;
    cmd.commandCode = CMD_CLICK;
    cmd.executionMode = (uint16_t)(EXEC_MODE_DIRECT_INSTANT | CPOWER_EXEC_FLAG_HAS_NODE_ID);
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    cmd.targetNodeId = 0U;
    cmd.targetX = 0;
    cmd.targetY = 0;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE, pktType);

    DWORD tNode0 = GetTickCount();
    while (InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) == beforeNode0Clicks &&
           (GetTickCount() - tNode0) < 1000U) {
        Sleep(10);
    }
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) > beforeNode0Clicks);
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.lastClientX, 0, 0) > 50);
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.lastClientY, 0, 0) > 50);

    /* 5. Explicit (0, 0) coords with CPOWER_EXEC_FLAG_HAS_COORDS overrides targetNodeId */
    LONG beforeZeroClicks = InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0);
    LONG beforeBtnClicks = InterlockedCompareExchange(&g_e2eWin.btnClicks, 0, 0);
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 105U;
    cmd.commandCode = CMD_CLICK;
    cmd.executionMode = (uint16_t)(EXEC_MODE_DIRECT_INSTANT | CPOWER_EXEC_FLAG_HAS_COORDS);
    cmd.targetHwnd = (uint64_t)(uintptr_t)g_e2eWin.hwnd;
    cmd.targetNodeId = btnNodeId;
    cmd.targetX = 0;
    cmd.targetY = 0;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);

    DWORD tZero = GetTickCount();
    while (InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) == beforeZeroClicks &&
           (GetTickCount() - tZero) < 1000U) {
        Sleep(10);
    }
    ASSERT_TRUE(InterlockedCompareExchange(&g_e2eWin.clientClicks, 0, 0) > beforeZeroClicks);
    ASSERT_EQ_INT(0, InterlockedCompareExchange(&g_e2eWin.lastClientX, 0, 0));
    ASSERT_EQ_INT(0, InterlockedCompareExchange(&g_e2eWin.lastClientY, 0, 0));
    ASSERT_EQ_INT(beforeBtnClicks, InterlockedCompareExchange(&g_e2eWin.btnClicks, 0, 0));

    /* 6. Clean CMD_SHUTDOWN */
    memset(&cmd, 0, sizeof(cmd));
    cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    cmd.commandId = 106U;
    cmd.commandCode = CMD_SHUTDOWN;
    e2e_send_pipe_cmd_expect_ack(g_e2ePipeClient, &cmd, (uint8_t*)g_e2eBufB, SCRATCH_CAP,
                                 &ack, &pktType, (uint8_t*)g_e2eBufA, RX_CAP, &rxBytes, &eventCount);
    ASSERT_HRESULT_OK((HRESULT)ack.hresult);

    ASSERT_EQ_UINT(WAIT_OBJECT_0, WaitForSingleObject(g_e2eDaemonProc.hProcess, 4000U));
    DWORD daemonExit = 1U;
    ASSERT_TRUE(GetExitCodeProcess(g_e2eDaemonProc.hProcess, &daemonExit));
    ASSERT_EQ_UINT(0U, daemonExit);
    CloseHandle(g_e2eDaemonProc.hProcess);
    g_e2eDaemonProc.hProcess = NULL;
    CloseHandle(g_e2eDaemonProc.hThread);
    g_e2eDaemonProc.hThread = NULL;
}

/* ------------------------------------------------------------------------- */
/* Test 7 (Tier 3/4): Isolated-Process Opt-In GDI Text IAT Hook + Detach     */
/* ------------------------------------------------------------------------- */

static void test_e2e_opt_in_text_hook_isolated_process(void) {
    /* Boundary validation first */
    BOOL dummy64 = FALSE;
    ASSERT_HRESULT_FAIL(Hook_IsProcess64Bit(0U, &dummy64));
    ASSERT_HRESULT_FAIL(Hook_IsProcess64Bit(GetCurrentProcessId(), NULL));
    ASSERT_HRESULT_FAIL(Hook_InjectProcess(0U, NULL));

    WCHAR selfExe[MAX_PATH];
    DWORD selfLen = GetModuleFileNameW(NULL, selfExe, MAX_PATH);
    ASSERT_TRUE(selfLen > 0 && selfLen < MAX_PATH);

    WCHAR dllPath[MAX_PATH];
    ASSERT_TRUE(resolve_repo_bin_file(L"cpower_hook64.dll", dllPath, MAX_PATH));

    LONG seq = InterlockedIncrement(&g_e2eSeq);
    WCHAR readyEvtName[96];
    swprintf(readyEvtName, 96, L"Local\\CPowerHookReady_%lu_%ld", GetCurrentProcessId(), seq);
    HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, readyEvtName);
    ASSERT_NOT_NULL(hReady);

    WCHAR cmdLine[768];
    swprintf(cmdLine, 768, L"\"%ls\" --hook-target-mode %ls", selfExe, readyEvtName);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL spawned = CreateProcessW(
        NULL, cmdLine, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &g_e2eProc
    );
    ASSERT_TRUE(spawned);

    DWORD waitReady = WaitForSingleObject(hReady, 5000U);
    CloseHandle(hReady);
    ASSERT_EQ_UINT(WAIT_OBJECT_0, waitReady);

    BOOL is64 = FALSE;
    ASSERT_HRESULT_OK(Hook_IsProcess64Bit(g_e2eProc.dwProcessId, &is64));
    ASSERT_TRUE(is64);

    /* Explicit opt-in IAT injection into isolated child PID */
    ASSERT_HRESULT_OK(Hook_InjectProcess(g_e2eProc.dwProcessId, dllPath));
    g_e2eHookPid = g_e2eProc.dwProcessId;

    /* Poll ring buffer until child window GDI text calls appear */
    BOOL sawCapturedText = FALSE;
    uint64_t lastSeq = 0ULL;
    DWORD tPoll = GetTickCount();
    while ((GetTickCount() - tPoll) < 3000U) {
        CPowerTextHookEntry entries[64];
        uint32_t outCount = 0U;
        if (SUCCEEDED(Hook_ReadCapturedText(g_e2eHookPid, entries, 64U, &outCount, &lastSeq))) {
            for (uint32_t i = 0; i < outCount; i++) {
                if (wcsstr(entries[i].text, L"CPowerHookCapture_42") != NULL ||
                    wcsstr(entries[i].text, L"CPowerDrawText_99") != NULL) {
                    ASSERT_EQ_UINT((uint32_t)g_e2eHookPid, entries[i].pid);
                    ASSERT_TRUE(entries[i].charCount > 0U);
                    sawCapturedText = TRUE;
                    break;
                }
            }
            if (sawCapturedText) {
                break;
            }
        }
        Sleep(30);
    }
    ASSERT_TRUE(sawCapturedText);

    /* Clean IAT unpatch + FreeLibrary detach */
    ASSERT_HRESULT_OK(Hook_DetachProcess(g_e2eHookPid, 3000U));
    DWORD detachedPid = g_e2eHookPid;
    g_e2eHookPid = 0;

    /* Target process remains alive and responsive after DLL unload */
    ASSERT_EQ_UINT(WAIT_TIMEOUT, WaitForSingleObject(g_e2eProc.hProcess, 150U));
    ASSERT_EQ_HRESULT(S_FALSE, Hook_DetachProcess(detachedPid, 500U));

    HWND targetHwnd = FindWindowW(L"CPowerHookTargetWinCls", L"CPowerHookTargetProcess");
    if (targetHwnd && IsWindow(targetHwnd)) {
        PostMessageW(targetHwnd, WM_CLOSE, 0, 0);
    }
    ASSERT_EQ_UINT(WAIT_OBJECT_0, WaitForSingleObject(g_e2eProc.hProcess, 3000U));
    DWORD childExit = 1U;
    ASSERT_TRUE(GetExitCodeProcess(g_e2eProc.hProcess, &childExit));
    ASSERT_EQ_UINT(0U, childExit);
    CloseHandle(g_e2eProc.hProcess);
    g_e2eProc.hProcess = NULL;
    CloseHandle(g_e2eProc.hThread);
    g_e2eProc.hThread = NULL;
}

void RegisterE2ETestSuite(void) {
    TestSuite* suite = test_suite_create(
        "E2ESuite",
        "Cross-subsystem Tier 3 pipelines and Tier 4 closed-loop real-world desktop scenarios",
        e2e_suite_setup,
        e2e_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_E2E_FullClosedLoop_GuiWinEventShmControl",
                       "External WinForms GUI -> WinEventHook -> UIA Tree -> SHM -> Commands -> Control Verify",
                       15, TIER_4_REAL_WORLD, test_e2e_full_closed_loop_gui_winevent_shm_control);

    test_case_register(suite, "Test_E2E_DesktopShellTreeAndPipeTransport",
                       "Real Shell/Desktop UIA tree extraction serialized over message-mode Named Pipe",
                       15, TIER_4_REAL_WORLD, test_e2e_desktop_shell_tree_and_pipe_transport);

    test_case_register(suite, "Test_E2E_SecurityGatedWin32ControlAndFallback",
                       "Security UIPI gate + Win32 direct control + dual GDI fallback & UIA verification",
                       15, TIER_3_CROSS_FEATURE, test_e2e_security_gated_win32_control_and_fallback);

    test_case_register(suite, "Test_E2E_MicroRegionEncodeAndShmImageTransfer",
                       "Micro-region capture/image + BMP/PNG encode + SHM RawFrame transfer & CRC check",
                       15, TIER_3_CROSS_FEATURE, test_e2e_microregion_encode_and_shm_image_transfer);

    test_case_register(suite, "Test_E2E_TrajectoryTargetHitWin32AndVerify",
                       "SemanticNode bounds -> Bezier/O-U landing -> VirtualDesktop map -> client click",
                       15, TIER_3_CROSS_FEATURE, test_e2e_trajectory_target_hit_win32_and_verify);

    test_case_register(suite, "Test_E2E_DaemonShmNodeIdAckAndDiff",
                       "Spawned cpower.exe daemon SHM session: windows, tree, node_id actions, AckPacket, diff",
                       15, TIER_4_REAL_WORLD, test_e2e_daemon_shm_node_id_ack_and_diff);

    test_case_register(suite, "Test_E2E_DaemonPipeFramedAndEdgeFlags",
                       "Spawned cpower.exe daemon Pipe session: watchdog duration, framed Ack/Payload/Event, nodeId=0 & (0,0) flags, E_NOTIMPL",
                       15, TIER_4_REAL_WORLD, test_e2e_daemon_pipe_framed_and_edge_flags);

    test_case_register(suite, "Test_E2E_OptInTextHookIsolatedProcess",
                       "Opt-in cpower_hook64.dll IAT text capture on isolated 64-bit Win32 process and clean detach",
                       15, TIER_4_REAL_WORLD, test_e2e_opt_in_text_hook_isolated_process);
}
