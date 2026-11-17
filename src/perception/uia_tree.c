/**
 * @file uia_tree.c
 * @brief Native Pure C COM IUIAutomation MTA tree traversal and serialization.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "perception.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>

#define CPOWER_MAX_TREE_DEPTH        64U
#define CPOWER_MAX_NODES_PER_WINDOW  4096U
#define CPOWER_MAX_CHILDREN_PER_NODE 4096U

#ifndef UIA_E_ELEMENTNOTAVAILABLE
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#endif

/* Internal coordination with dxgi_capture.c */
extern void PerceptionInternal_DxgiSetEnabled(BOOL enabled);
extern void PerceptionInternal_DxgiShutdown(void);

/* Standalone GUID constants (zero dependency on uuid.lib / INITGUID linkage) */
static const GUID CPOWER_CLSID_CUIAutomation =
    { 0xff48dba4U, 0x60efU, 0x4201U, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID CPOWER_IID_IUIAutomation =
    { 0x30cbe57dU, 0xd9d0U, 0x452aU, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
static const GUID CPOWER_IID_IUIAutomationValuePattern =
    { 0xa94cd8b1U, 0x0844U, 0x4cd6U, { 0x9d, 0x2d, 0x64, 0x05, 0x37, 0xab, 0x39, 0xe9 } };

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE value);
#define CPOWER_DPI_CONTEXT_PMV2 ((HANDLE)(LONG_PTR)-4)

static INIT_ONCE                 g_uiaCsOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION          g_uiaCs;
static IUIAutomation*            g_pAutomation = NULL;
static IUIAutomationTreeWalker*  g_pControlWalker = NULL;
static BOOL                      g_bInitialized = FALSE;
static BOOL                      g_bComInitialized = FALSE;
static DWORD                     g_comInitThreadId = 0;
static volatile LONG             g_treeSequenceId = 0;

static PerceptionConfig g_config = {
    CPOWER_MAX_TREE_DEPTH,
    CPOWER_MAX_NODES_PER_WINDOW,
    FALSE,
    TRUE
};

static BOOL CALLBACK init_uia_cs(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    InitializeCriticalSection(&g_uiaCs);
    return TRUE;
}

static void perception_enable_dpi_awareness(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        hUser32 = LoadLibraryW(L"user32.dll");
    }
    if (!hUser32) {
        return;
    }

    FARPROC proc = GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (proc) {
        PFN_SetProcessDpiAwarenessContext pfn = NULL;
        memcpy(&pfn, &proc, sizeof(pfn));
        (void)pfn(CPOWER_DPI_CONTEXT_PMV2);
    }
}

static uint64_t get_utc_filetime_u64(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
}

static void copy_bstr_bounded(BSTR bstr, WCHAR* dst, size_t dstCap) {
    if (!dst || dstCap == 0) {
        return;
    }
    dst[0] = L'\0';
    if (!bstr) {
        return;
    }
    UINT bstrLen = SysStringLen(bstr);
    size_t copyLen = (size_t)bstrLen;
    if (copyLen >= dstCap) {
        copyLen = dstCap - 1;
    }
    if (copyLen > 0) {
        memcpy(dst, bstr, copyLen * sizeof(WCHAR));
    }
    dst[copyLen] = L'\0';
}

HRESULT Perception_Init(void) {
    InitOnceExecuteOnce(&g_uiaCsOnce, init_uia_cs, NULL, NULL);
    EnterCriticalSection(&g_uiaCs);

    if (g_bInitialized) {
        LeaveCriticalSection(&g_uiaCs);
        return S_FALSE;
    }

    perception_enable_dpi_awareness();

    BOOL comOwned = FALSE;
    DWORD comThread = 0;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hrCom == S_OK || hrCom == S_FALSE) {
        comOwned = TRUE;
        comThread = GetCurrentThreadId();
    } else if (hrCom == RPC_E_CHANGED_MODE) {
        comOwned = FALSE;
        comThread = 0;
        CPower_Log(CPOWER_LOG_WARN, "Perception_Init: thread already initialized in STA (RPC_E_CHANGED_MODE)");
    } else {
        LeaveCriticalSection(&g_uiaCs);
        CPower_Log(CPOWER_LOG_ERROR, "Perception_Init: CoInitializeEx failed (0x%08lX)", (unsigned long)hrCom);
        return hrCom;
    }

    IUIAutomation* pAuto = NULL;
    HRESULT hr = CoCreateInstance(
        &CPOWER_CLSID_CUIAutomation,
        NULL,
        CLSCTX_INPROC_SERVER,
        &CPOWER_IID_IUIAutomation,
        (void**)&pAuto
    );
    if (FAILED(hr) || !pAuto) {
        if (comOwned) {
            CoUninitialize();
        }
        LeaveCriticalSection(&g_uiaCs);
        CPower_Log(CPOWER_LOG_ERROR, "Perception_Init: CoCreateInstance(CUIAutomation) failed (0x%08lX)", (unsigned long)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    IUIAutomationTreeWalker* pWalker = NULL;
    hr = IUIAutomation_get_ControlViewWalker(pAuto, &pWalker);
    if (FAILED(hr) || !pWalker) {
        IUIAutomation_Release(pAuto);
        if (comOwned) {
            CoUninitialize();
        }
        LeaveCriticalSection(&g_uiaCs);
        CPower_Log(CPOWER_LOG_ERROR, "Perception_Init: get_ControlViewWalker failed (0x%08lX)", (unsigned long)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    g_pAutomation = pAuto;
    g_pControlWalker = pWalker;
    g_bComInitialized = comOwned;
    g_comInitThreadId = comThread;
    g_bInitialized = TRUE;

    PerceptionInternal_DxgiSetEnabled(g_config.enableDxgiCapture);

    LeaveCriticalSection(&g_uiaCs);
    CPower_Log(CPOWER_LOG_INFO, "Perception engine initialized (UIA ControlViewWalker ready)");
    return S_OK;
}

void Perception_Shutdown(void) {
    Perception_StopEventHook();
    PerceptionInternal_DxgiShutdown();

    InitOnceExecuteOnce(&g_uiaCsOnce, init_uia_cs, NULL, NULL);
    EnterCriticalSection(&g_uiaCs);

    CPOWER_SAFE_RELEASE(g_pControlWalker);
    CPOWER_SAFE_RELEASE(g_pAutomation);

    if (g_bComInitialized && GetCurrentThreadId() == g_comInitThreadId) {
        CoUninitialize();
    }
    g_bComInitialized = FALSE;
    g_comInitThreadId = 0;
    g_bInitialized = FALSE;

    LeaveCriticalSection(&g_uiaCs);
    CPower_Log(CPOWER_LOG_INFO, "Perception engine shut down");
}

HRESULT Perception_Configure(const PerceptionConfig* pConfig) {
    if (!pConfig) {
        return E_POINTER;
    }

    InitOnceExecuteOnce(&g_uiaCsOnce, init_uia_cs, NULL, NULL);
    EnterCriticalSection(&g_uiaCs);

    uint32_t depth = pConfig->maxTreeDepth;
    if (depth == 0) {
        depth = CPOWER_MAX_TREE_DEPTH;
    } else if (depth > CPOWER_MAX_TREE_DEPTH) {
        depth = CPOWER_MAX_TREE_DEPTH;
    }

    uint32_t maxNodes = pConfig->maxNodes;
    if (maxNodes == 0) {
        maxNodes = CPOWER_MAX_NODES_PER_WINDOW;
    } else if (maxNodes > CPOWER_MAX_NODES_PER_WINDOW) {
        maxNodes = CPOWER_MAX_NODES_PER_WINDOW;
    }

    g_config.maxTreeDepth = depth;
    g_config.maxNodes = maxNodes;
    g_config.includeOffscreen = pConfig->includeOffscreen ? TRUE : FALSE;
    g_config.enableDxgiCapture = pConfig->enableDxgiCapture ? TRUE : FALSE;

    PerceptionInternal_DxgiSetEnabled(g_config.enableDxgiCapture);

    LeaveCriticalSection(&g_uiaCs);
    return S_OK;
}

typedef struct TraversalFrame {
    IUIAutomationElement* pElement;
    uint16_t              uiaDepth;
    uint16_t              treeDepth;
    int32_t               parentId;
} TraversalFrame;

typedef struct TraversalStack {
    TraversalFrame* frames;
    size_t          top;
    size_t          capacity;
} TraversalStack;

static BOOL stack_push(
    TraversalStack* s,
    IUIAutomationElement* pElem,
    uint16_t uiaDepth,
    uint16_t treeDepth,
    int32_t parentId
) {
    if (s->top >= s->capacity) {
        size_t newCap = (s->capacity == 0) ? 256 : (s->capacity * 2);
        if (newCap > 32768) {
            return FALSE;
        }
        TraversalFrame* grown = (TraversalFrame*)realloc(s->frames, newCap * sizeof(TraversalFrame));
        if (!grown) {
            return FALSE;
        }
        s->frames = grown;
        s->capacity = newCap;
    }
    s->frames[s->top].pElement = pElem;
    s->frames[s->top].uiaDepth = uiaDepth;
    s->frames[s->top].treeDepth = treeDepth;
    s->frames[s->top].parentId = parentId;
    s->top++;
    return TRUE;
}

static void stack_free_all(TraversalStack* s) {
    if (!s) {
        return;
    }
    for (size_t i = 0; i < s->top; i++) {
        CPOWER_SAFE_RELEASE(s->frames[i].pElement);
    }
    CPOWER_SAFE_FREE(s->frames);
    s->top = 0;
    s->capacity = 0;
}

static HRESULT extract_element_node(IUIAutomationElement* pElem, SemanticNode* pOut) {
    if (!pElem || !pOut) {
        return E_POINTER;
    }

    memset(pOut, 0, sizeof(*pOut));

    CONTROLTYPEID ctId = 0;
    HRESULT hr = IUIAutomationElement_get_CurrentControlType(pElem, &ctId);
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        return hr;
    }
    if (SUCCEEDED(hr)) {
        pOut->controlTypeId = (uint16_t)ctId;
    }

    RECT rc;
    ZeroMemory(&rc, sizeof(rc));
    hr = IUIAutomationElement_get_CurrentBoundingRectangle(pElem, &rc);
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        return hr;
    }
    if (SUCCEEDED(hr)) {
        pOut->bounds = rc;
    }

    BOOL bVal = FALSE;
    uint32_t flags = 0;

    if (SUCCEEDED(IUIAutomationElement_get_CurrentIsEnabled(pElem, &bVal)) && bVal) {
        flags |= CPOWER_NODE_FLAG_ENABLED;
    }
    bVal = FALSE;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentIsOffscreen(pElem, &bVal)) && bVal) {
        flags |= CPOWER_NODE_FLAG_OFFSCREEN;
    }
    bVal = FALSE;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentHasKeyboardFocus(pElem, &bVal)) && bVal) {
        flags |= CPOWER_NODE_FLAG_FOCUSED;
    }
    bVal = FALSE;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentIsKeyboardFocusable(pElem, &bVal)) && bVal) {
        flags |= CPOWER_NODE_FLAG_KEYBOARD_FOCUSABLE;
    }
    bVal = FALSE;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentIsPassword(pElem, &bVal)) && bVal) {
        flags |= CPOWER_NODE_FLAG_PASSWORD;
    }
    pOut->flags = flags;

    BSTR bstrName = NULL;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentName(pElem, &bstrName))) {
        copy_bstr_bounded(bstrName, pOut->name, sizeof(pOut->name) / sizeof(pOut->name[0]));
        CPOWER_SAFE_SYSFREESTRING(bstrName);
    }

    BSTR bstrClass = NULL;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentClassName(pElem, &bstrClass))) {
        copy_bstr_bounded(bstrClass, pOut->className, sizeof(pOut->className) / sizeof(pOut->className[0]));
        CPOWER_SAFE_SYSFREESTRING(bstrClass);
    }

    BSTR bstrAutoId = NULL;
    if (SUCCEEDED(IUIAutomationElement_get_CurrentAutomationId(pElem, &bstrAutoId))) {
        copy_bstr_bounded(bstrAutoId, pOut->automationId, sizeof(pOut->automationId) / sizeof(pOut->automationId[0]));
        CPOWER_SAFE_SYSFREESTRING(bstrAutoId);
    }

    IUIAutomationValuePattern* pValPat = NULL;
    hr = IUIAutomationElement_GetCurrentPatternAs(
        pElem,
        UIA_ValuePatternId,
        &CPOWER_IID_IUIAutomationValuePattern,
        (void**)&pValPat
    );
    if (SUCCEEDED(hr) && pValPat != NULL) {
        BSTR bstrVal = NULL;
        if (SUCCEEDED(IUIAutomationValuePattern_get_CurrentValue(pValPat, &bstrVal))) {
            copy_bstr_bounded(bstrVal, pOut->value, sizeof(pOut->value) / sizeof(pOut->value[0]));
            CPOWER_SAFE_SYSFREESTRING(bstrVal);
        }
        IUIAutomationValuePattern_Release(pValPat);
    }

    return S_OK;
}

static HRESULT traverse_root_to_tree(
    IUIAutomationTreeWalker* pWalker,
    IUIAutomationElement* pRoot,
    HWND rootHwnd,
    const PerceptionConfig* pCfg,
    SemanticTree** ppTreeOut
) {
    if (!pWalker || !pRoot || !pCfg || !ppTreeOut) {
        return E_POINTER;
    }

    SemanticTree* pTree = (SemanticTree*)calloc(1, sizeof(SemanticTree));
    if (!pTree) {
        return E_OUTOFMEMORY;
    }

    uint32_t initialCap = (pCfg->maxNodes < 64U) ? pCfg->maxNodes : 64U;
    if (initialCap == 0U) {
        initialCap = 1U;
    }

    pTree->nodes = (SemanticNode*)calloc(initialCap, sizeof(SemanticNode));
    if (!pTree->nodes) {
        free(pTree);
        return E_OUTOFMEMORY;
    }

    pTree->nodeCount = 0;
    pTree->maxNodes = initialCap;
    pTree->rootHwnd = rootHwnd;
    pTree->timestampUtc = get_utc_filetime_u64();

    TraversalStack stack;
    ZeroMemory(&stack, sizeof(stack));

    IUIAutomationElement_AddRef(pRoot);
    if (!stack_push(&stack, pRoot, 0U, 0U, -1)) {
        IUIAutomationElement_Release(pRoot);
        Perception_FreeSemanticTree(pTree);
        return E_OUTOFMEMORY;
    }

    size_t childBufCap = 64;
    IUIAutomationElement** childBuf = (IUIAutomationElement**)malloc(childBufCap * sizeof(IUIAutomationElement*));
    if (!childBuf) {
        stack_free_all(&stack);
        Perception_FreeSemanticTree(pTree);
        return E_OUTOFMEMORY;
    }

    while (stack.top > 0) {
        TraversalFrame curr = stack.frames[--stack.top];
        IUIAutomationElement* pElem = curr.pElement;

        if (pTree->nodeCount >= pCfg->maxNodes) {
            IUIAutomationElement_Release(pElem);
            break;
        }

        SemanticNode node;
        HRESULT hrExtract = extract_element_node(pElem, &node);
        if (hrExtract == UIA_E_ELEMENTNOTAVAILABLE) {
            IUIAutomationElement_Release(pElem);
            continue;
        }
        if (FAILED(hrExtract)) {
            IUIAutomationElement_Release(pElem);
            continue;
        }

        BOOL isFocused = (node.flags & CPOWER_NODE_FLAG_FOCUSED) != 0;
        BOOL isOffscreen = (node.flags & CPOWER_NODE_FLAG_OFFSCREEN) != 0;
        BOOL isZeroArea = (node.bounds.right <= node.bounds.left) ||
                          (node.bounds.bottom <= node.bounds.top);

        BOOL pruneNode = FALSE;
        if (curr.uiaDepth > 0U && !pCfg->includeOffscreen && !isFocused) {
            if (isOffscreen) {
                /* Prune entire offscreen subtree when includeOffscreen is FALSE */
                IUIAutomationElement_Release(pElem);
                continue;
            }
            if (isZeroArea) {
                pruneNode = TRUE;
            }
        }

        int32_t nextParentId = curr.parentId;
        uint16_t nextTreeDepth = curr.treeDepth;
        if (!pruneNode) {
            if (pTree->nodeCount >= pTree->maxNodes) {
                uint32_t newCap = pTree->maxNodes * 2U;
                if (newCap > pCfg->maxNodes) {
                    newCap = pCfg->maxNodes;
                }
                if (newCap <= pTree->maxNodes) {
                    IUIAutomationElement_Release(pElem);
                    break;
                }
                SemanticNode* grownNodes = (SemanticNode*)realloc(pTree->nodes, (size_t)newCap * sizeof(SemanticNode));
                if (!grownNodes) {
                    IUIAutomationElement_Release(pElem);
                    free(childBuf);
                    stack_free_all(&stack);
                    Perception_FreeSemanticTree(pTree);
                    return E_OUTOFMEMORY;
                }
                pTree->nodes = grownNodes;
                pTree->maxNodes = newCap;
            }

            node.id = pTree->nodeCount;
            node.parentId = curr.parentId;
            node.depth = curr.treeDepth;
            pTree->nodes[pTree->nodeCount++] = node;
            nextParentId = (int32_t)node.id;
            nextTreeDepth = (uint16_t)(curr.treeDepth + 1U);
        }

        if ((uint32_t)(curr.uiaDepth + 1U) < pCfg->maxTreeDepth &&
            pTree->nodeCount < pCfg->maxNodes) {
            size_t childCount = 0;
            IUIAutomationElement* pChild = NULL;
            HRESULT hrChild = IUIAutomationTreeWalker_GetFirstChildElement(pWalker, pElem, &pChild);

            while (SUCCEEDED(hrChild) && pChild != NULL && childCount < CPOWER_MAX_CHILDREN_PER_NODE) {
                if (childCount >= childBufCap) {
                    size_t newChildCap = childBufCap * 2;
                    if (newChildCap > CPOWER_MAX_CHILDREN_PER_NODE) {
                        newChildCap = CPOWER_MAX_CHILDREN_PER_NODE;
                    }
                    IUIAutomationElement** grownChildren = (IUIAutomationElement**)realloc(
                        childBuf, newChildCap * sizeof(IUIAutomationElement*)
                    );
                    if (!grownChildren) {
                        IUIAutomationElement_Release(pChild);
                        break;
                    }
                    childBuf = grownChildren;
                    childBufCap = newChildCap;
                }

                childBuf[childCount++] = pChild;

                IUIAutomationElement* pNext = NULL;
                hrChild = IUIAutomationTreeWalker_GetNextSiblingElement(pWalker, pChild, &pNext);
                pChild = pNext;
            }
            if (pChild != NULL) {
                IUIAutomationElement_Release(pChild);
            }

            uint16_t nextUiaDepth = (uint16_t)(curr.uiaDepth + 1U);
            for (size_t i = childCount; i > 0; i--) {
                IUIAutomationElement* c = childBuf[i - 1];
                if (!stack_push(&stack, c, nextUiaDepth, nextTreeDepth, nextParentId)) {
                    IUIAutomationElement_Release(c);
                }
            }
        }

        IUIAutomationElement_Release(pElem);
    }

    free(childBuf);
    stack_free_all(&stack);
    *ppTreeOut = pTree;
    return S_OK;
}

static HRESULT acquire_uia_refs(
    IUIAutomation** ppAuto,
    IUIAutomationTreeWalker** ppWalker,
    PerceptionConfig* pCfgOut
) {
    InitOnceExecuteOnce(&g_uiaCsOnce, init_uia_cs, NULL, NULL);
    EnterCriticalSection(&g_uiaCs);

    if (!g_bInitialized || !g_pAutomation || !g_pControlWalker) {
        LeaveCriticalSection(&g_uiaCs);
        return CO_E_NOTINITIALIZED;
    }

    *ppAuto = g_pAutomation;
    *ppWalker = g_pControlWalker;
    IUIAutomation_AddRef(*ppAuto);
    IUIAutomationTreeWalker_AddRef(*ppWalker);
    *pCfgOut = g_config;

    LeaveCriticalSection(&g_uiaCs);
    return S_OK;
}

HRESULT Perception_CaptureSemanticTree(HWND targetHwnd, SemanticTree** ppTreeOut) {
    if (!ppTreeOut) {
        return E_POINTER;
    }
    *ppTreeOut = NULL;

    IUIAutomation* pAuto = NULL;
    IUIAutomationTreeWalker* pWalker = NULL;
    PerceptionConfig cfg;
    HRESULT hr = acquire_uia_refs(&pAuto, &pWalker, &cfg);
    if (FAILED(hr)) {
        return hr;
    }

    BOOL comOwned = FALSE;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hrCom == S_OK || hrCom == S_FALSE) {
        comOwned = TRUE;
    }

    HWND resolvedHwnd = targetHwnd;
    if (resolvedHwnd == NULL) {
        resolvedHwnd = GetForegroundWindow();
        if (resolvedHwnd == NULL) {
            resolvedHwnd = GetShellWindow();
        }
        if (resolvedHwnd == NULL) {
            resolvedHwnd = GetDesktopWindow();
        }
    }

    if (resolvedHwnd == NULL || !IsWindow(resolvedHwnd)) {
        IUIAutomationTreeWalker_Release(pWalker);
        IUIAutomation_Release(pAuto);
        if (comOwned) {
            CoUninitialize();
        }
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    IUIAutomationElement* pRoot = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        hr = IUIAutomation_ElementFromHandle(pAuto, (UIA_HWND)resolvedHwnd, &pRoot);
        if (hr != RPC_E_CALL_REJECTED) {
            break;
        }
        Sleep(15);
    }

    if (SUCCEEDED(hr) && pRoot != NULL) {
        hr = traverse_root_to_tree(pWalker, pRoot, resolvedHwnd, &cfg, ppTreeOut);
        IUIAutomationElement_Release(pRoot);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }

    IUIAutomationTreeWalker_Release(pWalker);
    IUIAutomation_Release(pAuto);
    if (comOwned) {
        CoUninitialize();
    }
    return hr;
}

HRESULT Perception_CaptureDesktopTree(SemanticTree** ppTreeOut) {
    if (!ppTreeOut) {
        return E_POINTER;
    }
    *ppTreeOut = NULL;

    IUIAutomation* pAuto = NULL;
    IUIAutomationTreeWalker* pWalker = NULL;
    PerceptionConfig cfg;
    HRESULT hr = acquire_uia_refs(&pAuto, &pWalker, &cfg);
    if (FAILED(hr)) {
        return hr;
    }

    BOOL comOwned = FALSE;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hrCom == S_OK || hrCom == S_FALSE) {
        comOwned = TRUE;
    }

    IUIAutomationElement* pRoot = NULL;
    hr = IUIAutomation_GetRootElement(pAuto, &pRoot);
    if (SUCCEEDED(hr) && pRoot != NULL) {
        hr = traverse_root_to_tree(pWalker, pRoot, GetDesktopWindow(), &cfg, ppTreeOut);
        IUIAutomationElement_Release(pRoot);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }

    IUIAutomationTreeWalker_Release(pWalker);
    IUIAutomation_Release(pAuto);
    if (comOwned) {
        CoUninitialize();
    }
    return hr;
}

void Perception_FreeSemanticTree(SemanticTree* pTree) {
    if (!pTree) {
        return;
    }
    CPOWER_SAFE_FREE(pTree->nodes);
    free(pTree);
}

HRESULT Perception_SerializeTree(const SemanticTree* pTree, void* pBuffer, size_t bufferSize, size_t* pBytesWritten) {
    if (!pTree) {
        return E_POINTER;
    }
    if (!pBuffer && !pBytesWritten) {
        return E_POINTER;
    }
    if (pTree->nodeCount > 0 && !pTree->nodes) {
        return E_INVALIDARG;
    }

    if ((uint64_t)pTree->nodeCount > (0xFFFFFFFFULL - (uint64_t)sizeof(TreePacketHeader)) / (uint64_t)sizeof(SemanticNode)) {
        return E_INVALIDARG;
    }

    size_t nodesBytes = (size_t)pTree->nodeCount * sizeof(SemanticNode);
    size_t totalBytes = sizeof(TreePacketHeader) + nodesBytes;

    if (pBytesWritten) {
        *pBytesWritten = totalBytes;
    }

    if (!pBuffer && bufferSize == 0) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!pBuffer) {
        return E_POINTER;
    }
    if (bufferSize < totalBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    TreePacketHeader hdr;
    ZeroMemory(&hdr, sizeof(hdr));
    hdr.packetMagic = CPOWER_TREE_PACKET_MAGIC;
    hdr.sequenceId = (uint32_t)InterlockedIncrement(&g_treeSequenceId);
    hdr.timestampUs = (pTree->timestampUtc > 0) ? (pTree->timestampUtc / 10ULL) : (get_utc_filetime_u64() / 10ULL);
    hdr.totalPayloadSize = (uint32_t)totalBytes;
    hdr.nodeCount = pTree->nodeCount;
    hdr.rootNodeIndex = 0;
    hdr.stringTableBytes = 0;

    uint8_t* dst = (uint8_t*)pBuffer;
    memcpy(dst, &hdr, sizeof(hdr));
    if (nodesBytes > 0) {
        memcpy(dst + sizeof(hdr), pTree->nodes, nodesBytes);
    }

    return S_OK;
}

/* -------------------------------------------------------------------------
 * Text & JSON Serialization Helpers
 * ------------------------------------------------------------------------- */

typedef struct StrBuf {
    char*  data;
    size_t cap;
    size_t len;
} StrBuf;

static void sb_putc(StrBuf* sb, char c) {
    if (sb->data && sb->len < sb->cap) {
        sb->data[sb->len] = c;
    }
    sb->len++;
}

static void sb_puts(StrBuf* sb, const char* s) {
    if (!s) {
        return;
    }
    while (*s) {
        sb_putc(sb, *s++);
    }
}

static void sb_printf(StrBuf* sb, const char* fmt, ...) {
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n > 0) {
        size_t cnt = (size_t)n;
        if (cnt >= sizeof(tmp)) {
            cnt = sizeof(tmp) - 1;
        }
        for (size_t i = 0; i < cnt; i++) {
            sb_putc(sb, tmp[i]);
        }
    }
}

static size_t bounded_wcslen(const WCHAR* ws, size_t maxWchars) {
    size_t len = 0;
    while (len < maxWchars && ws[len] != L'\0') {
        len++;
    }
    return len;
}

static void sb_append_json_wstr(StrBuf* sb, const WCHAR* ws, size_t maxWchars) {
    sb_putc(sb, '"');
    if (ws && maxWchars > 0) {
        size_t wlen = bounded_wcslen(ws, maxWchars);
        if (wlen > 0) {
            char utf8[640];
            int utf8Bytes = WideCharToMultiByte(
                CP_UTF8, 0, ws, (int)wlen, utf8, (int)sizeof(utf8), NULL, NULL
            );
            if (utf8Bytes > 0) {
                for (int i = 0; i < utf8Bytes; i++) {
                    unsigned char ch = (unsigned char)utf8[i];
                    switch (ch) {
                        case '"':  sb_puts(sb, "\\\""); break;
                        case '\\': sb_puts(sb, "\\\\"); break;
                        case '\b': sb_puts(sb, "\\b");  break;
                        case '\f': sb_puts(sb, "\\f");  break;
                        case '\n': sb_puts(sb, "\\n");  break;
                        case '\r': sb_puts(sb, "\\r");  break;
                        case '\t': sb_puts(sb, "\\t");  break;
                        default:
                            if (ch < 0x20U) {
                                sb_printf(sb, "\\u%04x", (unsigned int)ch);
                            } else {
                                sb_putc(sb, (char)ch);
                            }
                            break;
                    }
                }
            }
        }
    }
    sb_putc(sb, '"');
}

static void sb_append_clean_utf8(StrBuf* sb, const WCHAR* ws, size_t maxWchars) {
    if (!ws || maxWchars == 0) {
        return;
    }
    size_t wlen = bounded_wcslen(ws, maxWchars);
    if (wlen == 0) {
        return;
    }
    char utf8[640];
    int utf8Bytes = WideCharToMultiByte(
        CP_UTF8, 0, ws, (int)wlen, utf8, (int)sizeof(utf8), NULL, NULL
    );
    if (utf8Bytes > 0) {
        for (int i = 0; i < utf8Bytes; i++) {
            unsigned char ch = (unsigned char)utf8[i];
            if (ch == '\r' || ch == '\n' || ch == '\t') {
                sb_putc(sb, ' ');
            } else if (ch == '"') {
                sb_puts(sb, "\\\"");
            } else if (ch >= 0x20U) {
                sb_putc(sb, (char)ch);
            }
        }
    }
}

HRESULT Perception_SerializeTreeJson(const SemanticTree* pTree, char* pBuffer, size_t bufferSize, size_t* pBytesWritten) {
    if (!pTree) {
        return E_POINTER;
    }
    if (!pBuffer && !pBytesWritten) {
        return E_POINTER;
    }
    if (pTree->nodeCount > 0 && !pTree->nodes) {
        return E_INVALIDARG;
    }

    StrBuf sb;
    sb.data = pBuffer;
    sb.cap = bufferSize;
    sb.len = 0;

    sb_printf(&sb,
              "{\"rootHwnd\":\"0x%llx\",\"timestampUtc\":%llu,\"nodeCount\":%u,\"nodes\":[",
              (unsigned long long)(uintptr_t)pTree->rootHwnd,
              (unsigned long long)pTree->timestampUtc,
              (unsigned int)pTree->nodeCount);

    for (uint32_t i = 0; i < pTree->nodeCount; i++) {
        const SemanticNode* n = &pTree->nodes[i];
        if (i > 0) {
            sb_putc(&sb, ',');
        }
        sb_printf(&sb,
                  "{\"id\":%u,\"parentId\":%d,\"depth\":%u,\"controlTypeId\":%u,\"controlType\":\"%s\","
                  "\"bounds\":{\"left\":%ld,\"top\":%ld,\"right\":%ld,\"bottom\":%ld},"
                  "\"enabled\":%s,\"offscreen\":%s,\"focused\":%s,\"focusable\":%s,\"password\":%s,",
                  (unsigned int)n->id,
                  (int)n->parentId,
                  (unsigned int)n->depth,
                  (unsigned int)n->controlTypeId,
                  Perception_ControlTypeIdToString((long)n->controlTypeId),
                  (long)n->bounds.left,
                  (long)n->bounds.top,
                  (long)n->bounds.right,
                  (long)n->bounds.bottom,
                  (n->flags & CPOWER_NODE_FLAG_ENABLED) ? "true" : "false",
                  (n->flags & CPOWER_NODE_FLAG_OFFSCREEN) ? "true" : "false",
                  (n->flags & CPOWER_NODE_FLAG_FOCUSED) ? "true" : "false",
                  (n->flags & CPOWER_NODE_FLAG_KEYBOARD_FOCUSABLE) ? "true" : "false",
                  (n->flags & CPOWER_NODE_FLAG_PASSWORD) ? "true" : "false");

        sb_puts(&sb, "\"name\":");
        sb_append_json_wstr(&sb, n->name, sizeof(n->name) / sizeof(n->name[0]));
        sb_puts(&sb, ",\"className\":");
        sb_append_json_wstr(&sb, n->className, sizeof(n->className) / sizeof(n->className[0]));
        sb_puts(&sb, ",\"automationId\":");
        sb_append_json_wstr(&sb, n->automationId, sizeof(n->automationId) / sizeof(n->automationId[0]));
        sb_puts(&sb, ",\"value\":");
        sb_append_json_wstr(&sb, n->value, sizeof(n->value) / sizeof(n->value[0]));
        sb_putc(&sb, '}');
    }

    sb_puts(&sb, "]}");

    if (!pBuffer && bufferSize > 0U) {
        return E_POINTER;
    }
    if (sb.len == SIZE_MAX) {
        return E_INVALIDARG;
    }

    size_t required = sb.len + 1;
    if (pBytesWritten) {
        *pBytesWritten = required;
    }
    if (!pBuffer || bufferSize < required) {
        if (pBuffer && bufferSize > 0) {
            pBuffer[bufferSize - 1] = '\0';
        }
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    pBuffer[sb.len] = '\0';
    return S_OK;
}

HRESULT Perception_SerializeTreeIndented(const SemanticTree* pTree, char* pBuffer, size_t bufferSize, size_t* pBytesWritten) {
    if (!pTree) {
        return E_POINTER;
    }
    if (!pBuffer && !pBytesWritten) {
        return E_POINTER;
    }
    if (pTree->nodeCount > 0 && !pTree->nodes) {
        return E_INVALIDARG;
    }

    StrBuf sb;
    sb.data = pBuffer;
    sb.cap = bufferSize;
    sb.len = 0;

    for (uint32_t i = 0; i < pTree->nodeCount; i++) {
        const SemanticNode* n = &pTree->nodes[i];
        uint16_t indent = (n->depth <= 63U) ? n->depth : 63U;
        for (uint16_t d = 0; d < indent; d++) {
            sb_puts(&sb, "  ");
        }

        sb_printf(&sb, "[%u] %s",
                  (unsigned int)n->id,
                  Perception_ControlTypeIdToString((long)n->controlTypeId));

        if (n->name[0] != L'\0') {
            sb_puts(&sb, " \"");
            sb_append_clean_utf8(&sb, n->name, sizeof(n->name) / sizeof(n->name[0]));
            sb_putc(&sb, '"');
        }

        sb_printf(&sb, " (%ld,%ld,%ld,%ld)",
                  (long)n->bounds.left,
                  (long)n->bounds.top,
                  (long)n->bounds.right,
                  (long)n->bounds.bottom);

        if (n->automationId[0] != L'\0') {
            sb_puts(&sb, " #");
            sb_append_clean_utf8(&sb, n->automationId, sizeof(n->automationId) / sizeof(n->automationId[0]));
        }

        if (n->value[0] != L'\0') {
            sb_puts(&sb, " val=\"");
            sb_append_clean_utf8(&sb, n->value, sizeof(n->value) / sizeof(n->value[0]));
            sb_putc(&sb, '"');
        }

        if (n->flags & CPOWER_NODE_FLAG_FOCUSED) {
            sb_puts(&sb, " [focused]");
        }
        if (!(n->flags & CPOWER_NODE_FLAG_ENABLED)) {
            sb_puts(&sb, " [disabled]");
        }
        if (n->flags & CPOWER_NODE_FLAG_PASSWORD) {
            sb_puts(&sb, " [password]");
        }

        sb_putc(&sb, '\n');
    }

    if (!pBuffer && bufferSize > 0U) {
        return E_POINTER;
    }
    if (sb.len == SIZE_MAX) {
        return E_INVALIDARG;
    }

    size_t required = sb.len + 1;
    if (pBytesWritten) {
        *pBytesWritten = required;
    }
    if (!pBuffer || bufferSize < required) {
        if (pBuffer && bufferSize > 0) {
            pBuffer[bufferSize - 1] = '\0';
        }
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    pBuffer[sb.len] = '\0';
    return S_OK;
}

const char* Perception_ControlTypeIdToString(long controlTypeId) {
    switch (controlTypeId) {
        case UIA_ButtonControlTypeId:       return "Button";
        case UIA_CalendarControlTypeId:     return "Calendar";
        case UIA_CheckBoxControlTypeId:     return "CheckBox";
        case UIA_ComboBoxControlTypeId:     return "ComboBox";
        case UIA_EditControlTypeId:         return "Edit";
        case UIA_HyperlinkControlTypeId:    return "Hyperlink";
        case UIA_ImageControlTypeId:        return "Image";
        case UIA_ListItemControlTypeId:     return "ListItem";
        case UIA_ListControlTypeId:         return "List";
        case UIA_MenuControlTypeId:         return "Menu";
        case UIA_MenuBarControlTypeId:      return "MenuBar";
        case UIA_MenuItemControlTypeId:     return "MenuItem";
        case UIA_ProgressBarControlTypeId:  return "ProgressBar";
        case UIA_RadioButtonControlTypeId:  return "RadioButton";
        case UIA_ScrollBarControlTypeId:    return "ScrollBar";
        case UIA_SliderControlTypeId:       return "Slider";
        case UIA_SpinnerControlTypeId:      return "Spinner";
        case UIA_StatusBarControlTypeId:    return "StatusBar";
        case UIA_TabControlTypeId:          return "Tab";
        case UIA_TabItemControlTypeId:      return "TabItem";
        case UIA_TextControlTypeId:         return "Text";
        case UIA_ToolBarControlTypeId:      return "ToolBar";
        case UIA_ToolTipControlTypeId:      return "ToolTip";
        case UIA_TreeControlTypeId:         return "Tree";
        case UIA_TreeItemControlTypeId:     return "TreeItem";
        case UIA_CustomControlTypeId:       return "Custom";
        case UIA_GroupControlTypeId:        return "Group";
        case UIA_ThumbControlTypeId:        return "Thumb";
        case UIA_DataGridControlTypeId:     return "DataGrid";
        case UIA_DataItemControlTypeId:     return "DataItem";
        case UIA_DocumentControlTypeId:     return "Document";
        case UIA_SplitButtonControlTypeId:  return "SplitButton";
        case UIA_WindowControlTypeId:       return "Window";
        case UIA_PaneControlTypeId:         return "Pane";
        case UIA_HeaderControlTypeId:       return "Header";
        case UIA_HeaderItemControlTypeId:   return "HeaderItem";
        case UIA_TableControlTypeId:        return "Table";
        case UIA_TitleBarControlTypeId:     return "TitleBar";
        case UIA_SeparatorControlTypeId:    return "Separator";
        case UIA_SemanticZoomControlTypeId: return "SemanticZoom";
        case UIA_AppBarControlTypeId:       return "AppBar";
        default:                            return "Unknown";
    }
}
