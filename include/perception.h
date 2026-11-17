/**
 * @file perception.h
 * @brief Zero-Screenshot Semantic Perception Engine interface contracts and data types.
 *
 * Implements native COM IUIAutomation tree traversal, real-time WinEvent hooks,
 * fallback GDI/DirectWrite text extraction, and on-demand micro-region DXGI capture.
 */

#ifndef CPOWER_PERCEPTION_H
#define CPOWER_PERCEPTION_H

#include "common.h"
#include <ole2.h>
#include <uiautomationclient.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Semantic Node Bitflags
 * ------------------------------------------------------------------------- */
#define CPOWER_NODE_FLAG_ENABLED            (1U << 0)
#define CPOWER_NODE_FLAG_OFFSCREEN          (1U << 1)
#define CPOWER_NODE_FLAG_FOCUSED            (1U << 2)
#define CPOWER_NODE_FLAG_KEYBOARD_FOCUSABLE (1U << 3)
#define CPOWER_NODE_FLAG_PASSWORD           (1U << 4)

/* -------------------------------------------------------------------------
 * Semantic Tree Data Structures
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

/**
 * @brief Compact, flat semantic node representation.
 */
typedef struct SemanticNode {
    uint32_t id;               /**< Sequential node index [0..nodeCount-1] */
    int32_t  parentId;         /**< Parent node index, or -1 if root */
    uint16_t depth;            /**< Traversal depth hierarchy level [0..63] */
    uint16_t controlTypeId;    /**< Standard UIA_*ControlTypeId */
    RECT     bounds;           /**< Screen coordinate bounding box */
    uint32_t flags;            /**< Combination of CPOWER_NODE_FLAG_* */
    WCHAR    name[128];        /**< Accessible element name */
    WCHAR    className[64];    /**< Win32 window class or framework class */
    WCHAR    automationId[64]; /**< Developer-assigned automation ID */
    WCHAR    value[128];       /**< Value pattern content or text value */
} SemanticNode;

#pragma pack(pop)

/**
 * @brief Complete in-memory semantic tree for an active window or desktop.
 */
typedef struct SemanticTree {
    uint32_t      nodeCount;    /**< Number of active nodes */
    uint32_t      maxNodes;     /**< Total allocated capacity */
    HWND          rootHwnd;     /**< Handle to window tree root */
    uint64_t      timestampUtc; /**< Snapshot timestamp in 100ns units */
    SemanticNode* nodes;        /**< Contiguous array of semantic nodes */
} SemanticTree;

/**
 * @brief Raw image buffer for micro-region VRAM captures.
 */
typedef struct ImageBuffer {
    int      width;    /**< Pixel width */
    int      height;   /**< Pixel height */
    int      stride;   /**< Bytes per row (width * 4) */
    size_t   dataSize; /**< Total byte size of pixel buffer */
    uint8_t* pixels;   /**< BGRA32 pixel array */
} ImageBuffer;

/**
 * @brief Configuration parameters for perception extraction.
 */
typedef struct PerceptionConfig {
    uint32_t maxTreeDepth;      /**< Maximum traversal depth (default: 64) */
    uint32_t maxNodes;          /**< Maximum nodes per tree (default: 4096) */
    BOOL     includeOffscreen;  /**< Whether to retain offscreen nodes */
    BOOL     enableDxgiCapture; /**< Whether DXGI capture is permitted */
} PerceptionConfig;

/**
 * @brief Record representing a captured WinEvent notification.
 */
typedef struct WinEventRecord {
    DWORD event;     /**< EVENT_* code */
    HWND  hwnd;      /**< Window generating the event */
    LONG  idObject;  /**< Object identifier (e.g., OBJID_CLIENT) */
    LONG  idChild;   /**< Child identifier */
    DWORD threadId;  /**< Generating thread ID */
    DWORD timestamp; /**< Event timestamp in ms */
} WinEventRecord;

/**
 * @brief Event callback function pointer for WinEvent notifications.
 */
typedef void (*PerceptionEventCallback)(const WinEventRecord* pEvent, void* pUserData);
typedef PerceptionEventCallback EventCallback;

/* -------------------------------------------------------------------------
 * Perception Engine Public Interface
 * ------------------------------------------------------------------------- */

/**
 * @brief Initializes the perception engine, COM MTA apartment, and internal buffers.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_Init(void);

/**
 * @brief Shuts down the perception engine and releases all allocated COM interfaces.
 */
void Perception_Shutdown(void);

/**
 * @brief Updates perception engine configuration parameters.
 * @param pConfig Pointer to PerceptionConfig.
 * @return S_OK on success.
 */
HRESULT Perception_Configure(const PerceptionConfig* pConfig);

/**
 * @brief Captures the semantic UI tree for a specific target window handle.
 * @param targetHwnd Target window handle (if NULL, captures active foreground window).
 * @param ppTreeOut Pointer to receive newly allocated SemanticTree.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_CaptureSemanticTree(HWND targetHwnd, SemanticTree** ppTreeOut);

/**
 * @brief Captures the semantic UI tree starting from the full desktop root.
 * @param ppTreeOut Pointer to receive newly allocated SemanticTree.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_CaptureDesktopTree(SemanticTree** ppTreeOut);

/**
 * @brief Frees all memory associated with a SemanticTree allocated by perception routines.
 * @param pTree Pointer to SemanticTree to free.
 */
void Perception_FreeSemanticTree(SemanticTree* pTree);

/**
 * @brief Serializes a SemanticTree into a compact binary packet format.
 * @param pTree Source SemanticTree.
 * @param pBuffer Destination memory buffer.
 * @param bufferSize Capacity of destination buffer in bytes.
 * @param pBytesWritten Pointer receiving total bytes written.
 * @return S_OK on success, or HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER).
 */
HRESULT Perception_SerializeTree(const SemanticTree* pTree, void* pBuffer, size_t bufferSize, size_t* pBytesWritten);

/**
 * @brief Serializes a SemanticTree into a standard JSON string.
 */
HRESULT Perception_SerializeTreeJson(const SemanticTree* pTree, char* pBuffer, size_t bufferSize, size_t* pBytesWritten);

/**
 * @brief Serializes a SemanticTree into an indented, token-efficient text format.
 */
HRESULT Perception_SerializeTreeIndented(const SemanticTree* pTree, char* pBuffer, size_t bufferSize, size_t* pBytesWritten);

/**
 * @brief Registers an out-of-context WinEvent hook and starts the event dispatcher thread.
 * @param pfnCallback Callback invoked on each delivered WinEvent.
 * @param pUserData Optional user context passed to callback.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_StartEventHook(EventCallback pfnCallback, void* pUserData);

/**
 * @brief Unregisters the WinEvent hook and terminates the event pump thread.
 */
void Perception_StopEventHook(void);

/**
 * @brief Polls an event from the internal queue (non-blocking).
 * @param pEventOut Pointer to receive WinEventRecord.
 * @return TRUE if an event was popped, FALSE if queue is empty.
 */
BOOL Perception_PollEvent(WinEventRecord* pEventOut);

/**
 * @brief Extracts fallback text from a window using GDI messages (WM_GETTEXT, EM_GETLINE).
 * @param hwnd Target window handle.
 * @param pBuffer Output wide character buffer.
 * @param maxChars Capacity of pBuffer in characters.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_FallbackExtractText(HWND hwnd, WCHAR* pBuffer, int maxChars);

/**
 * @brief Sends a synthetic WM_GETOBJECT message to force Chromium/Electron to activate accessibility.
 * @param hwnd Window handle of Chrome_RenderWidgetHostHWND.
 */
void Perception_PokeChromiumAccessibility(HWND hwnd);

/**
 * @brief Captures a micro-region from VRAM via DXGI Direct3D 11 staging textures.
 * @param left Screen X minimum.
 * @param top Screen Y minimum.
 * @param right Screen X maximum.
 * @param bottom Screen Y maximum.
 * @param pImageOut Pointer to receive ImageBuffer containing BGRA pixels.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Perception_CaptureMicroRegion(int left, int top, int right, int bottom, ImageBuffer* pImageOut);

/**
 * @brief Frees the pixel memory of an ImageBuffer.
 * @param pImage Pointer to ImageBuffer.
 */
void Perception_FreeImageBuffer(ImageBuffer* pImage);

/**
 * @brief Encodes an ImageBuffer into a 32-bit BMP file in memory.
 */
HRESULT Perception_EncodeBmpInMemory(const ImageBuffer* pImage, uint8_t* pOutBmp, size_t maxBytes, size_t* pBytesWritten);

/**
 * @brief Encodes an ImageBuffer into an RFC 1950/1951 PNG file in memory with zero dependencies.
 */
HRESULT Perception_EncodePngInMemory(const ImageBuffer* pImage, uint8_t* pOutPng, size_t maxBytes, size_t* pBytesWritten);

/**
 * @brief Converts a UIA control type ID to a human-readable string.
 * @param controlTypeId UIA_*ControlTypeId.
 * @return Constant string (e.g. "Button", "Edit").
 */
const char* Perception_ControlTypeIdToString(long controlTypeId);

/* -------------------------------------------------------------------------
 * Semantic Node Locator & Delta Tree Diffing (v1.1.0)
 * ------------------------------------------------------------------------- */

#define CPOWER_DIFF_CHANGE_BOUNDS (1U << 0)
#define CPOWER_DIFF_CHANGE_VALUE  (1U << 1)
#define CPOWER_DIFF_CHANGE_FLAGS  (1U << 2)
#define CPOWER_DIFF_CHANGE_NAME   (1U << 3)

typedef struct SemanticQuery {
    WCHAR    nameSubstr[128];   /**< Case-insensitive substring match against node name (empty = ignore) */
    WCHAR    automationId[64];  /**< Exact case-insensitive automationId match (empty = ignore) */
    WCHAR    className[64];     /**< Exact or substring case-insensitive className match (empty = ignore) */
    uint16_t controlTypeId;     /**< Target UIA_*ControlTypeId, or 0 for any */
    BOOL     onlyEnabled;       /**< Require CPOWER_NODE_FLAG_ENABLED */
    BOOL     onlyVisible;       /**< Exclude CPOWER_NODE_FLAG_OFFSCREEN and empty-area rects */
} SemanticQuery;

typedef struct SemanticMatch {
    uint32_t     nodeId;
    POINT        center;
    SemanticNode node;
} SemanticMatch;

typedef enum TreeDiffOp {
    DIFF_OP_ADDED    = 1,
    DIFF_OP_REMOVED  = 2,
    DIFF_OP_MODIFIED = 3
} TreeDiffOp;

#pragma pack(push, 1)

typedef struct TreeDiffEntry {
    uint32_t     op;          /**< TreeDiffOp */
    uint32_t     nodeId;      /**< Curr node index (ADDED/MODIFIED) or prev node index (REMOVED) */
    uint32_t     prevNodeId;  /**< Previous node index for MODIFIED/REMOVED, or UINT32_MAX */
    uint32_t     changedMask; /**< Bitmask of CPOWER_DIFF_CHANGE_* for MODIFIED */
    uint64_t     fingerprint; /**< Structural FNV-1a fingerprint */
    SemanticNode node;        /**< Node payload */
} TreeDiffEntry;

#pragma pack(pop)

typedef struct SemanticTreeDiff {
    HWND           rootHwnd;
    uint64_t       prevTimestampUtc;
    uint64_t       currTimestampUtc;
    uint32_t       entryCount;
    uint32_t       addedCount;
    uint32_t       removedCount;
    uint32_t       modifiedCount;
    TreeDiffEntry* entries;
} SemanticTreeDiff;

/**
 * @brief Searches a captured SemanticTree using case-insensitive criteria.
 */
HRESULT Perception_FindNodes(
    const SemanticTree* pTree,
    const SemanticQuery* pQuery,
    SemanticMatch* pMatches,
    uint32_t maxMatches,
    uint32_t* pMatchCount
);

/**
 * @brief Computes structural delta diff between two SemanticTree snapshots.
 */
HRESULT Perception_DiffTrees(
    const SemanticTree* pPrev,
    const SemanticTree* pCurr,
    SemanticTreeDiff** ppDiffOut
);

/**
 * @brief Frees a SemanticTreeDiff allocated by Perception_DiffTrees.
 */
void Perception_FreeTreeDiff(SemanticTreeDiff* pDiff);

/**
 * @brief Serializes a SemanticTreeDiff into UTF-8 JSON.
 */
HRESULT Perception_SerializeDiffJson(
    const SemanticTreeDiff* pDiff,
    char* pBuffer,
    size_t bufferSize,
    size_t* pBytesWritten
);

/**
 * @brief Serializes a SemanticTreeDiff into a binary DIFF IPC packet.
 */
HRESULT Perception_SerializeDiffBinary(
    const SemanticTreeDiff* pDiff,
    void* pBuffer,
    size_t bufferSize,
    size_t* pBytesWritten
);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_PERCEPTION_H */
