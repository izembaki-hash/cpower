/**
 * @file ipc.h
 * @brief Low-Latency IPC Architecture (Lock-Free SPSC Shared Memory & Named Pipes).
 *
 * Implements dual Single-Producer Single-Consumer circular ring buffers with
 * 64-byte cache-line isolation, power-of-two bitmask wrapping, cross-integrity
 * SDDL security descriptors, flat binary semantic tree serialization, and a
 * message-mode Named Pipes fallback.
 */

#ifndef CPOWER_IPC_H
#define CPOWER_IPC_H

#include "common.h"
#include "perception.h"
#include <sddl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Protocol Constants & Magic Numbers
 * ------------------------------------------------------------------------- */
#define CPOWER_SHM_MAGIC             0x41475043U /* "AGPC" */
#define CPOWER_SHM_VERSION           1U
#define CPOWER_TREE_PACKET_MAGIC     0x54524545U /* "TREE" */
#define CPOWER_CMD_PACKET_MAGIC      0x434D4E44U /* "CMND" */
#define CPOWER_EVENT_PACKET_MAGIC    0x45564E54U /* "EVNT" */
#define CPOWER_ACK_PACKET_MAGIC      0x41434B50U /* "ACKP" */
#define CPOWER_DIFF_PACKET_MAGIC     0x44494646U /* "DIFF" */
#define CPOWER_WINDOWS_PACKET_MAGIC  0x574E4453U /* "WNDS" */

#define CPOWER_DEFAULT_SHM_NAME      L"Local\\CPowerAgentIPC"
#define CPOWER_DEFAULT_PIPE_NAME     L"\\\\.\\pipe\\cpower_ipc"
#define CPOWER_DEFAULT_RING_CAPACITY (4U * 1024U * 1024U) /* 4 MB per direction */
/* Baseline SDDL (SYSTEM + Administrators, Medium Mandatory Label).
   Runtime IPC channels use IPC_CreateLeastPrivilegeSA() to include the current
   process token's User SID dynamically without granting Everyone (WD) or Low (LW). */
#define CPOWER_SDDL_CROSS_INTEGRITY  L"D:(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;ME)"

#define CPOWER_EXEC_MODE_MASK        0x00FFU
#define CPOWER_EXEC_FLAG_HAS_NODE_ID 0x0100U
#define CPOWER_EXEC_FLAG_HAS_COORDS  0x0200U

#define CPOWER_ACK_FLAG_NONE        0U
#define CPOWER_ACK_FLAG_HAS_TREE    (1U << 0)
#define CPOWER_ACK_FLAG_HAS_DIFF    (1U << 1)
#define CPOWER_ACK_FLAG_HAS_IMAGE   (1U << 2)
#define CPOWER_ACK_FLAG_HAS_WINDOWS (1U << 3)

/* -------------------------------------------------------------------------
 * Packet & Command Enumerations
 * ------------------------------------------------------------------------- */

typedef enum IPC_PacketType {
    IPC_PKT_UNKNOWN   = 0,
    IPC_PKT_TREE      = 1,
    IPC_PKT_COMMAND   = 2,
    IPC_PKT_EVENT     = 3,
    IPC_PKT_ACK       = 4,
    IPC_PKT_IMAGE     = 5,
    IPC_PKT_TREE_DIFF = 6,
    IPC_PKT_WINDOWS   = 7
} IPC_PacketType;

typedef enum IPC_CommandCode {
    CMD_NOP                  = 0,
    CMD_CLICK                = 1,
    CMD_DOUBLE_CLICK         = 2,
    CMD_RIGHT_CLICK          = 3,
    CMD_MOUSE_DOWN           = 4,
    CMD_MOUSE_UP             = 5,
    CMD_MOUSE_MOVE           = 6,
    CMD_DRAG                 = 7,
    CMD_KEY_DOWN             = 8,
    CMD_KEY_UP               = 9,
    CMD_TYPE_TEXT            = 10,
    CMD_HOTKEY               = 11,
    CMD_SCROLL               = 12,
    CMD_UIA_INVOKE           = 13,
    CMD_UIA_SET_VALUE        = 14,
    CMD_UIA_TOGGLE           = 15,
    CMD_UIA_SELECT           = 16,
    CMD_WIN32_CLICK          = 17,
    CMD_WIN32_SET_TEXT       = 18,
    CMD_CAPTURE_MICRO_REGION = 19,
    CMD_CAPTURE_TREE         = 20,
    CMD_SHUTDOWN             = 21,
    CMD_FOCUS_WINDOW         = 22,
    CMD_SHOW_WINDOW          = 23,
    CMD_DIFF_TREE            = 24,
    CMD_LIST_WINDOWS         = 25,
    CMD_HSCROLL              = 26
} IPC_CommandCode;

typedef enum IPC_ExecutionMode {
    EXEC_MODE_DIRECT_INSTANT  = 0,
    EXEC_MODE_HUMAN_EMULATION = 1
} IPC_ExecutionMode;

/* -------------------------------------------------------------------------
 * Cache-Line Isolated SPSC Ring Buffer Header (192 bytes)
 * ------------------------------------------------------------------------- */
#pragma pack(push, 8)

typedef struct ShmRingBufferHeader {
    /* Cache Line 0: Immutable Configuration (64 bytes) */
    uint32_t magic;       /**< CPOWER_SHM_MAGIC */
    uint32_t version;     /**< CPOWER_SHM_VERSION */
    uint32_t capacity;    /**< Data payload byte size (power of 2) */
    uint32_t mask;        /**< capacity - 1 */
    uint8_t  pad0[48];

    /* Cache Line 1: Producer Write Cursor (64 bytes) */
    volatile uint32_t writeIdx; /**< Monotonic byte offset written */
    uint8_t  pad1[60];

    /* Cache Line 2: Consumer Read Cursor (64 bytes) */
    volatile uint32_t readIdx;  /**< Monotonic byte offset read */
    uint8_t  pad2[60];
} ShmRingBufferHeader;

#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Binary Wire Packet Layouts
 * ------------------------------------------------------------------------- */
#pragma pack(push, 4)

typedef struct IPC_FrameHeader {
    uint32_t payloadSize; /**< Size of payload following this header */
    uint16_t packetType;  /**< IPC_PacketType */
    uint16_t flags;       /**< Reserved */
} IPC_FrameHeader;

typedef struct TreePacketHeader {
    uint32_t packetMagic;      /**< CPOWER_TREE_PACKET_MAGIC ("TREE") */
    uint32_t sequenceId;       /**< Incremental snapshot ID */
    uint64_t timestampUs;      /**< QPC/UTC microsecond timestamp */
    uint32_t totalPayloadSize; /**< Header + nodes + strings */
    uint32_t nodeCount;        /**< Number of SemanticNode or IPC_FlatNode items */
    uint32_t rootNodeIndex;    /**< Root node index (0) */
    uint32_t stringTableBytes; /**< Trailing string blob size */
} TreePacketHeader;

typedef struct CommandPacket {
    uint32_t packetMagic;      /**< CPOWER_CMD_PACKET_MAGIC ("CMND") */
    uint32_t commandId;        /**< Request tracking sequence number */
    uint16_t commandCode;      /**< IPC_CommandCode */
    uint16_t executionMode;    /**< IPC_ExecutionMode */
    int32_t  targetX;          /**< Screen X coordinate */
    int32_t  targetY;          /**< Screen Y coordinate */
    int32_t  param1;           /**< MouseButton / vkCode / scroll delta / endX / showAction */
    int32_t  param2;           /**< Modifier flags / endY / horizontal flag */
    float    speedFactor;      /**< Speed scale (1.0 = normal) */
    float    jitterScale;      /**< Tremor scale (1.0 = normal) */
    uint64_t targetHwnd;       /**< Target HWND */
    uint32_t targetNodeId;     /**< Target UIA semantic node ID */
    uint32_t textPayloadBytes; /**< Byte length of valid UTF-16 textPayload */
    WCHAR    textPayload[256]; /**< Inline wide-char payload */
} CommandPacket;

typedef struct AckPacket {
    uint32_t magic;      /**< CPOWER_ACK_PACKET_MAGIC ("ACKP") */
    uint32_t commandId;  /**< Matching CommandPacket.commandId */
    int32_t  hresult;    /**< Execution HRESULT */
    uint32_t elapsedUs;  /**< Execution duration in microseconds */
    uint32_t nodeCount;  /**< Node/window count produced (if applicable) */
    uint32_t diffCount;  /**< Diff entries produced (if applicable) */
    uint32_t flags;      /**< Bitmask of CPOWER_ACK_FLAG_* */
} AckPacket;

typedef struct EventPacket {
    uint32_t magic;      /**< CPOWER_EVENT_PACKET_MAGIC ("EVNT") */
    uint32_t event;      /**< WinEvent EVENT_* code */
    uint64_t hwnd;       /**< Fixed-width 64-bit window handle */
    int32_t  idObject;   /**< Object identifier */
    int32_t  idChild;    /**< Child identifier */
    uint32_t threadId;   /**< Generating thread ID */
    uint32_t timestamp;  /**< Event timestamp in ms */
} EventPacket;

typedef struct DiffPacketHeader {
    uint32_t packetMagic;      /**< CPOWER_DIFF_PACKET_MAGIC ("DIFF") */
    uint32_t sequenceId;       /**< Incremental diff ID */
    uint64_t prevTimestampUtc; /**< Previous snapshot timestamp */
    uint64_t currTimestampUtc; /**< Current snapshot timestamp */
    uint32_t totalPayloadSize; /**< Header + entryCount * sizeof(TreeDiffEntry) */
    uint32_t entryCount;       /**< Total diff entries */
    uint32_t addedCount;       /**< Added nodes count */
    uint32_t removedCount;     /**< Removed nodes count */
    uint32_t modifiedCount;    /**< Modified nodes count */
} DiffPacketHeader;

typedef struct WindowPacketHeader {
    uint32_t packetMagic;      /**< CPOWER_WINDOWS_PACKET_MAGIC ("WNDS") */
    uint32_t windowCount;      /**< Number of trailing WindowInfo structs */
    uint32_t totalPayloadSize; /**< Header + windowCount * sizeof(WindowInfo) */
    uint32_t reserved;
} WindowPacketHeader;

#pragma pack(pop)

_Static_assert(sizeof(ShmRingBufferHeader) == 192, "ShmRingBufferHeader packed size must be 192 bytes");
_Static_assert(sizeof(IPC_FrameHeader) == 8, "IPC_FrameHeader packed size must be 8 bytes");
_Static_assert(sizeof(TreePacketHeader) == 32, "TreePacketHeader packed size must be 32 bytes");
_Static_assert(sizeof(CommandPacket) == 564, "CommandPacket packed size must be 564 bytes");
_Static_assert(sizeof(AckPacket) == 28, "AckPacket packed size must be 28 bytes");
_Static_assert(sizeof(EventPacket) == 32, "EventPacket packed size must be 32 bytes");
_Static_assert(sizeof(DiffPacketHeader) == 44, "DiffPacketHeader packed size must be 44 bytes");
_Static_assert(sizeof(WindowPacketHeader) == 16, "WindowPacketHeader packed size must be 16 bytes");
_Static_assert(sizeof(TreeDiffEntry) == 824, "TreeDiffEntry packed size must be 824 bytes");

/* -------------------------------------------------------------------------
 * Channel & Pipe Handles
 * ------------------------------------------------------------------------- */

typedef struct ShmChannel {
    HANDLE               hMapFile;
    void*                pBaseView;
    size_t               totalMapSize;
    size_t               ringCapacity;
    ShmRingBufferHeader* pOutRing;    /**< Agent -> Client */
    uint8_t*             pOutData;
    ShmRingBufferHeader* pInRing;     /**< Client -> Agent */
    uint8_t*             pInData;
    HANDLE               hOutEvent;   /**< Signaled when Agent writes to OutRing */
    HANDLE               hInEvent;    /**< Signaled when Client writes to InRing */
    BOOL                 isServer;
    uint32_t             nextSeqId;
    WCHAR                channelName[128];
} ShmChannel;

typedef struct PipeServer {
    HANDLE hPipe;
    WCHAR  pipeName[MAX_PATH];
    BOOL   isServer;
    BOOL   isConnected;
} PipeServer;

/* -------------------------------------------------------------------------
 * Shared Memory IPC Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Constructs a dynamic least-privilege cross-integrity SECURITY_ATTRIBUTES
 *        granting access only to SYSTEM, Administrators, and the current
 *        process token's User SID at Medium integrity (S:(ML;;NW;;;ME)).
 *        Fails closed if token lookup or SD construction fails.
 */
HRESULT IPC_CreateLeastPrivilegeSA(SECURITY_ATTRIBUTES* pSa);

/**
 * @brief Frees security descriptor memory allocated by IPC_CreateLeastPrivilegeSA.
 */
void IPC_FreeSecurityAttributes(SECURITY_ATTRIBUTES* pSa);

/**
 * @brief Creates a dual-ring SPSC shared memory IPC channel as the host server.
 * @param pszName Kernel mapping name (if NULL, uses CPOWER_DEFAULT_SHM_NAME).
 * @param capacity Byte capacity per ring (rounded up to power of 2; 0 uses default).
 * @param ppChannel Output pointer receiving allocated ShmChannel.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT IPC_CreateShmChannel(const WCHAR* pszName, size_t capacity, ShmChannel** ppChannel);

/**
 * @brief Opens an existing shared memory IPC channel as a client.
 */
HRESULT IPC_OpenShmChannel(const WCHAR* pszName, ShmChannel** ppChannel);

/**
 * @brief Closes and frees a ShmChannel instance.
 */
void IPC_CloseShmChannel(ShmChannel* pChannel);

/**
 * @brief Writes a serialized semantic tree buffer into the outbound ring buffer.
 */
HRESULT IPC_WriteTreePacket(ShmChannel* pChannel, const void* pData, size_t size);

/**
 * @brief Reads a semantic tree packet from the outbound ring buffer (client side).
 */
HRESULT IPC_ReadTreePacket(ShmChannel* pChannel, void* pBuffer, size_t bufferSize, size_t* pBytesRead, DWORD timeoutMs);

/**
 * @brief Writes a command packet into the inbound ring buffer (client -> agent).
 */
HRESULT IPC_WriteCommandPacket(ShmChannel* pChannel, const CommandPacket* pCmd);

/**
 * @brief Reads a command packet from the inbound ring buffer (agent side).
 */
HRESULT IPC_ReadCommandPacket(ShmChannel* pChannel, CommandPacket* pCmdOut, DWORD timeoutMs);

/**
 * @brief Low-level ring buffer frame write.
 */
HRESULT IPC_WriteRawFrame(ShmChannel* pChannel, BOOL toClient, uint16_t pktType, const void* pPayload, uint32_t payloadSize);

/**
 * @brief Low-level ring buffer frame read with hybrid spin-wait + event sleep.
 */
HRESULT IPC_ReadRawFrame(ShmChannel* pChannel, BOOL fromClient, uint16_t* pOutType, void* pOutBuffer, uint32_t maxBytes, uint32_t* pOutBytes, DWORD timeoutMs);

/* -------------------------------------------------------------------------
 * Named Pipes Fallback Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Creates a message-mode cross-integrity Named Pipe server.
 */
HRESULT IPC_CreatePipeServer(const WCHAR* pszPipeName, PipeServer** ppServer);

/**
 * @brief Waits for a client connection on the Named Pipe server.
 */
HRESULT IPC_AcceptPipeClient(PipeServer* pServer);

/**
 * @brief Connects to an existing Named Pipe server as a client.
 */
HRESULT IPC_ConnectPipeClient(const WCHAR* pszPipeName, DWORD timeoutMs, PipeServer** ppClient);

/**
 * @brief Disconnects and destroys a PipeServer or PipeClient instance.
 */
void IPC_ClosePipeServer(PipeServer* pServer);

/**
 * @brief Writes a discrete message packet over a Named Pipe.
 */
HRESULT IPC_PipeWrite(PipeServer* pPipe, const void* pData, uint32_t size, uint32_t* pBytesWritten);

/**
 * @brief Reads a discrete message packet from a Named Pipe.
 */
HRESULT IPC_PipeRead(PipeServer* pPipe, void* pBuffer, uint32_t maxBytes, uint32_t* pBytesRead);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_IPC_H */
