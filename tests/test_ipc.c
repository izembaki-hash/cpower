/**
 * @file test_ipc.c
 * @brief Tier 1, Tier 2, and Tier 5 tests for SPSC Shared Memory ring buffers,
 *        Tree/Command packet round-trips, multi-threaded wraparound stress, and Named Pipes.
 */

#include "test_infra.h"
#include "ipc.h"
#include "input.h"
#include <wchar.h>

static ShmChannel* g_shmServer = NULL;
static ShmChannel* g_shmClient = NULL;
static PipeServer* g_pipeServer = NULL;
static PipeServer* g_pipeClient = NULL;
static HANDLE g_workerThreadA = NULL;
static HANDLE g_workerThreadB = NULL;
static LONG g_ipcChannelSeq = 0;

static void make_unique_shm_name(WCHAR* outBuf, size_t maxChars) {
    LONG seq = InterlockedIncrement(&g_ipcChannelSeq);
    swprintf(outBuf, maxChars, L"Local\\CPowerTestShm_%lu_%ld", GetCurrentProcessId(), seq);
}

static void make_unique_pipe_name(WCHAR* outBuf, size_t maxChars) {
    LONG seq = InterlockedIncrement(&g_ipcChannelSeq);
    swprintf(outBuf, maxChars, L"\\\\.\\pipe\\cpower_test_pipe_%lu_%ld", GetCurrentProcessId(), seq);
}

static void ipc_suite_setup(void) {
    g_shmServer = NULL;
    g_shmClient = NULL;
    g_pipeServer = NULL;
    g_pipeClient = NULL;
    g_workerThreadA = NULL;
    g_workerThreadB = NULL;
}

static void ipc_suite_teardown(void) {
    if (g_workerThreadA) {
        WaitForSingleObject(g_workerThreadA, 3000);
        CloseHandle(g_workerThreadA);
        g_workerThreadA = NULL;
    }
    if (g_workerThreadB) {
        WaitForSingleObject(g_workerThreadB, 3000);
        CloseHandle(g_workerThreadB);
        g_workerThreadB = NULL;
    }
    if (g_shmClient) {
        IPC_CloseShmChannel(g_shmClient);
        g_shmClient = NULL;
    }
    if (g_shmServer) {
        IPC_CloseShmChannel(g_shmServer);
        g_shmServer = NULL;
    }
    if (g_pipeClient) {
        IPC_ClosePipeServer(g_pipeClient);
        g_pipeClient = NULL;
    }
    if (g_pipeServer) {
        IPC_ClosePipeServer(g_pipeServer);
        g_pipeServer = NULL;
    }
}

static uint32_t fnv1a_hash(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619U;
    }
    return h;
}

static void test_ipc_shm_create_open_close(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);

    /* Non-power-of-two capacity 10000 should round up to power-of-two >= 10000 */
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 10000U, &g_shmServer));
    ASSERT_NOT_NULL(g_shmServer);
    ASSERT_TRUE(g_shmServer->isServer);
    ASSERT_NOT_NULL(g_shmServer->pOutRing);
    ASSERT_NOT_NULL(g_shmServer->pInRing);
    ASSERT_NOT_NULL(g_shmServer->pOutData);
    ASSERT_NOT_NULL(g_shmServer->pInData);
    ASSERT_TRUE(g_shmServer->ringCapacity >= 10000U);
    ASSERT_EQ_UINT(0U, g_shmServer->ringCapacity & (g_shmServer->ringCapacity - 1U));

    ASSERT_EQ_UINT(CPOWER_SHM_MAGIC, g_shmServer->pOutRing->magic);
    ASSERT_EQ_UINT(CPOWER_SHM_VERSION, g_shmServer->pOutRing->version);
    ASSERT_EQ_UINT((uint32_t)g_shmServer->ringCapacity, g_shmServer->pOutRing->capacity);
    ASSERT_EQ_UINT((uint32_t)g_shmServer->ringCapacity - 1U, g_shmServer->pOutRing->mask);

    ASSERT_EQ_UINT(CPOWER_SHM_MAGIC, g_shmServer->pInRing->magic);
    ASSERT_EQ_UINT((uint32_t)g_shmServer->ringCapacity, g_shmServer->pInRing->capacity);

    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));
    ASSERT_NOT_NULL(g_shmClient);
    ASSERT_FALSE(g_shmClient->isServer);
    ASSERT_EQ_UINT(g_shmServer->ringCapacity, g_shmClient->ringCapacity);

    IPC_CloseShmChannel(g_shmClient);
    g_shmClient = NULL;
    IPC_CloseShmChannel(g_shmServer);
    g_shmServer = NULL;
}

static void test_ipc_shm_tree_packet_roundtrip(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 65536U, &g_shmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));

    enum { NODE_COUNT = 4 };
    const size_t pktSize = sizeof(TreePacketHeader) + NODE_COUNT * sizeof(SemanticNode);
    uint8_t* txBuf = (uint8_t*)calloc(1, pktSize);
    uint8_t* rxBuf = (uint8_t*)calloc(1, pktSize + 256);
    ASSERT_NOT_NULL(txBuf);
    ASSERT_NOT_NULL(rxBuf);

    TreePacketHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.packetMagic = CPOWER_TREE_PACKET_MAGIC;
    hdr.sequenceId = 7;
    hdr.timestampUs = 123456789ULL;
    hdr.totalPayloadSize = (uint32_t)pktSize;
    hdr.nodeCount = NODE_COUNT;
    hdr.rootNodeIndex = 0;
    hdr.stringTableBytes = 0;
    memcpy(txBuf, &hdr, sizeof(hdr));

    for (uint32_t i = 0; i < NODE_COUNT; i++) {
        SemanticNode node;
        memset(&node, 0, sizeof(node));
        node.id = i;
        node.parentId = (i == 0) ? -1 : 0;
        node.depth = (i == 0) ? 0 : 1;
        node.controlTypeId = (i == 0) ? 50032 : 50000;
        node.bounds.left = 10 + (LONG)i * 40;
        node.bounds.top = 20;
        node.bounds.right = 50 + (LONG)i * 40;
        node.bounds.bottom = 60;
        node.flags = CPOWER_NODE_FLAG_ENABLED;
        swprintf(node.name, 128, L"Node_%u", i);
        swprintf(node.className, 64, L"Button");
        memcpy(txBuf + sizeof(TreePacketHeader) + (size_t)i * sizeof(SemanticNode), &node, sizeof(node));
    }

    ASSERT_HRESULT_OK(IPC_WriteTreePacket(g_shmServer, txBuf, pktSize));

    size_t rxBytes = 0;
    ASSERT_HRESULT_OK(IPC_ReadTreePacket(g_shmClient, rxBuf, pktSize + 256, &rxBytes, 1000));
    ASSERT_EQ_UINT(pktSize, rxBytes);
    ASSERT_EQ_INT(0, memcmp(txBuf, rxBuf, pktSize));

    free(txBuf);
    free(rxBuf);
}

static void test_ipc_shm_command_packet_roundtrip(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 65536U, &g_shmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));

    CommandPacket txCmd;
    memset(&txCmd, 0, sizeof(txCmd));
    txCmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    txCmd.commandId = 101;
    txCmd.commandCode = CMD_UIA_SET_VALUE;
    txCmd.executionMode = EXEC_MODE_DIRECT_INSTANT;
    txCmd.targetX = 640;
    txCmd.targetY = 480;
    txCmd.param1 = 1;
    txCmd.param2 = KEY_MOD_CTRL | KEY_MOD_SHIFT;
    txCmd.speedFactor = 0.75f;
    txCmd.jitterScale = 0.25f;
    txCmd.targetHwnd = 0x0000000012345678ULL;
    txCmd.targetNodeId = 12;
    wcscpy(txCmd.textPayload, L"Hello CPower IPC Payload");
    txCmd.textPayloadBytes = (uint32_t)(wcslen(txCmd.textPayload) * sizeof(WCHAR));

    ASSERT_HRESULT_OK(IPC_WriteCommandPacket(g_shmClient, &txCmd));

    CommandPacket rxCmd;
    memset(&rxCmd, 0, sizeof(rxCmd));
    ASSERT_HRESULT_OK(IPC_ReadCommandPacket(g_shmServer, &rxCmd, 1000));

    ASSERT_EQ_UINT(txCmd.packetMagic, rxCmd.packetMagic);
    ASSERT_EQ_UINT(txCmd.commandId, rxCmd.commandId);
    ASSERT_EQ_UINT(txCmd.commandCode, rxCmd.commandCode);
    ASSERT_EQ_UINT(txCmd.executionMode, rxCmd.executionMode);
    ASSERT_EQ_INT(txCmd.targetX, rxCmd.targetX);
    ASSERT_EQ_INT(txCmd.targetY, rxCmd.targetY);
    ASSERT_EQ_INT(txCmd.param1, rxCmd.param1);
    ASSERT_EQ_INT(txCmd.param2, rxCmd.param2);
    ASSERT_DOUBLE_NEAR(txCmd.speedFactor, rxCmd.speedFactor, 1e-5);
    ASSERT_DOUBLE_NEAR(txCmd.jitterScale, rxCmd.jitterScale, 1e-5);
    ASSERT_EQ_UINT(txCmd.targetHwnd, rxCmd.targetHwnd);
    ASSERT_EQ_UINT(txCmd.targetNodeId, rxCmd.targetNodeId);
    ASSERT_EQ_UINT(txCmd.textPayloadBytes, rxCmd.textPayloadBytes);
    ASSERT_EQ_WSTR(txCmd.textPayload, rxCmd.textPayload);
}

/* Tier 5 Stress Test Worker Contexts */
#pragma pack(push, 1)
typedef struct StressFrameHeader {
    uint32_t seq;
    uint32_t dataLen;
    uint32_t hash;
    int64_t  qpcSent;
} StressFrameHeader;
#pragma pack(pop)

typedef struct StressProducerArgs {
    ShmChannel* pServer;
    ShmChannel* pClient;
    int frameCount;
    int cmdCount;
    BOOL ok;
    char errMsg[128];
} StressProducerArgs;

typedef struct StressConsumerArgs {
    ShmChannel* pClient;
    ShmChannel* pServer;
    int frameCount;
    int cmdCount;
    LARGE_INTEGER qpcFreq;
    double maxLatencyUs;
    double sumLatencyUs;
    BOOL ok;
    char errMsg[128];
} StressConsumerArgs;

static DWORD WINAPI stress_out_producer_thread(LPVOID lpParam) {
    StressProducerArgs* args = (StressProducerArgs*)lpParam;
    uint8_t buf[600];
    LARGE_INTEGER now;
    DWORD startTick = GetTickCount();

    for (int i = 0; i < args->frameCount; i++) {
        uint32_t dataLen = 16U + (uint32_t)((i * 73) % 480);
        StressFrameHeader hdr;
        uint8_t* payload = buf + sizeof(StressFrameHeader);
        for (uint32_t k = 0; k < dataLen; k++) {
            payload[k] = (uint8_t)((i * 31U + k * 17U) & 0xFFU);
        }
        hdr.seq = (uint32_t)i;
        hdr.dataLen = dataLen;
        hdr.hash = fnv1a_hash(payload, dataLen);
        QueryPerformanceCounter(&now);
        hdr.qpcSent = now.QuadPart;
        memcpy(buf, &hdr, sizeof(hdr));

        uint32_t totalLen = (uint32_t)sizeof(StressFrameHeader) + dataLen;
        HRESULT hr = E_FAIL;
        for (;;) {
            hr = IPC_WriteRawFrame(args->pServer, TRUE, IPC_PKT_EVENT, buf, totalLen);
            if (SUCCEEDED(hr)) break;
            if ((GetTickCount() - startTick) > 10000U) {
                snprintf(args->errMsg, sizeof(args->errMsg),
                         "Out producer timeout at seq=%d hr=0x%08lX", i, (unsigned long)hr);
                args->ok = FALSE;
                return 1;
            }
            SwitchToThread();
        }
    }

    /* Also verify inbound command stream */
    for (int c = 0; c < args->cmdCount; c++) {
        CommandPacket cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
        cmd.commandId = (uint32_t)c;
        cmd.commandCode = CMD_MOUSE_MOVE;
        cmd.targetX = c * 3;
        cmd.targetY = c * 2;
        HRESULT hr = E_FAIL;
        for (;;) {
            hr = IPC_WriteCommandPacket(args->pClient, &cmd);
            if (SUCCEEDED(hr)) break;
            if ((GetTickCount() - startTick) > 10000U) {
                snprintf(args->errMsg, sizeof(args->errMsg),
                         "Cmd producer timeout at c=%d hr=0x%08lX", c, (unsigned long)hr);
                args->ok = FALSE;
                return 1;
            }
            SwitchToThread();
        }
    }

    args->ok = TRUE;
    return 0;
}

static DWORD WINAPI stress_out_consumer_thread(LPVOID lpParam) {
    StressConsumerArgs* args = (StressConsumerArgs*)lpParam;
    uint8_t buf[600];
    LARGE_INTEGER now;

    for (int i = 0; i < args->frameCount; i++) {
        uint16_t pktType = 0;
        uint32_t rxBytes = 0;
        HRESULT hr = IPC_ReadRawFrame(args->pClient, FALSE, &pktType, buf, sizeof(buf), &rxBytes, 5000);
        if (FAILED(hr)) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Out consumer read failed at seq=%d hr=0x%08lX", i, (unsigned long)hr);
            args->ok = FALSE;
            return 1;
        }
        QueryPerformanceCounter(&now);

        if (pktType != IPC_PKT_EVENT || rxBytes < sizeof(StressFrameHeader)) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Invalid frame header at seq=%d type=%u bytes=%u", i, pktType, rxBytes);
            args->ok = FALSE;
            return 1;
        }
        StressFrameHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.seq != (uint32_t)i || rxBytes != sizeof(StressFrameHeader) + hdr.dataLen) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Sequence/length mismatch expected=%d got=%u", i, hdr.seq);
            args->ok = FALSE;
            return 1;
        }
        const uint8_t* payload = buf + sizeof(StressFrameHeader);
        uint32_t actualHash = fnv1a_hash(payload, hdr.dataLen);
        if (actualHash != hdr.hash) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Checksum mismatch at seq=%d", i);
            args->ok = FALSE;
            return 1;
        }

        if (args->qpcFreq.QuadPart > 0) {
            double us = ((double)(now.QuadPart - hdr.qpcSent) * 1000000.0) /
                        (double)args->qpcFreq.QuadPart;
            if (us > args->maxLatencyUs) args->maxLatencyUs = us;
            args->sumLatencyUs += us;
        }
    }

    for (int c = 0; c < args->cmdCount; c++) {
        CommandPacket rxCmd;
        memset(&rxCmd, 0, sizeof(rxCmd));
        HRESULT hr = IPC_ReadCommandPacket(args->pServer, &rxCmd, 5000);
        if (FAILED(hr)) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Cmd consumer failed at c=%d hr=0x%08lX", c, (unsigned long)hr);
            args->ok = FALSE;
            return 1;
        }
        if (rxCmd.commandId != (uint32_t)c || rxCmd.targetX != c * 3 || rxCmd.targetY != c * 2) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Cmd payload mismatch at c=%d gotId=%u", c, rxCmd.commandId);
            args->ok = FALSE;
            return 1;
        }
    }

    args->ok = TRUE;
    return 0;
}

static void test_ipc_shm_concurrent_spsc_wraparound_stress(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);
    /* 8 KB rings force ~40+ circular wrap-arounds over 1,200 variable frames */
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 8192U, &g_shmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));

    StressProducerArgs prodArgs;
    memset(&prodArgs, 0, sizeof(prodArgs));
    prodArgs.pServer = g_shmServer;
    prodArgs.pClient = g_shmClient;
    prodArgs.frameCount = 1200;
    prodArgs.cmdCount = 400;

    StressConsumerArgs consArgs;
    memset(&consArgs, 0, sizeof(consArgs));
    consArgs.pClient = g_shmClient;
    consArgs.pServer = g_shmServer;
    consArgs.frameCount = 1200;
    consArgs.cmdCount = 400;
    QueryPerformanceFrequency(&consArgs.qpcFreq);

    g_workerThreadA = CreateThread(NULL, 0, stress_out_consumer_thread, &consArgs, 0, NULL);
    g_workerThreadB = CreateThread(NULL, 0, stress_out_producer_thread, &prodArgs, 0, NULL);
    ASSERT_NOT_NULL(g_workerThreadA);
    ASSERT_NOT_NULL(g_workerThreadB);

    HANDLE handles[2] = { g_workerThreadA, g_workerThreadB };
    DWORD waitRes = WaitForMultipleObjects(2, handles, TRUE, 15000);
    ASSERT_EQ_UINT(WAIT_OBJECT_0, waitRes);

    CloseHandle(g_workerThreadA); g_workerThreadA = NULL;
    CloseHandle(g_workerThreadB); g_workerThreadB = NULL;

    if (!prodArgs.ok) ASSERT_FAIL(prodArgs.errMsg);
    if (!consArgs.ok) ASSERT_FAIL(consArgs.errMsg);
    ASSERT_TRUE(prodArgs.ok);
    ASSERT_TRUE(consArgs.ok);

    double avgUs = consArgs.sumLatencyUs / (double)consArgs.frameCount;
    ASSERT_TRUE(avgUs >= 0.0);
    CPower_Log(CPOWER_LOG_INFO, "SPSC 1200-frame avg latency: %.2f us (max %.2f us)",
               avgUs, consArgs.maxLatencyUs);
}

static void test_ipc_shm_boundary_and_timeouts(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 4096U, &g_shmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));

    /* Timeout on empty OutRing */
    uint8_t rxBuf[256];
    size_t rxBytes = 0;
    QpcTimer t;
    Timer_Init(&t);
    Timer_Start(&t);
    HRESULT hrTimeout = IPC_ReadTreePacket(g_shmClient, rxBuf, sizeof(rxBuf), &rxBytes, 15);
    Timer_Stop(&t);
    ASSERT_HRESULT_FAIL(hrTimeout);
    ASSERT_TRUE(Timer_ElapsedMs(&t) < 1000.0);

    /* Timeout on empty InRing */
    CommandPacket rxCmd;
    ASSERT_HRESULT_FAIL(IPC_ReadCommandPacket(g_shmServer, &rxCmd, 15));

    /* Oversized payload > ringCapacity (4096) must fail immediately */
    uint8_t* hugeBuf = (uint8_t*)calloc(1, 5120);
    ASSERT_NOT_NULL(hugeBuf);
    ASSERT_HRESULT_FAIL(IPC_WriteRawFrame(g_shmServer, TRUE, IPC_PKT_TREE, hugeBuf, 5120U));
    free(hugeBuf);

    /* Read buffer too small must fail cleanly */
    uint8_t frameData[200];
    memset(frameData, 0xAB, sizeof(frameData));
    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_shmServer, TRUE, IPC_PKT_EVENT, frameData, sizeof(frameData)));
    uint16_t outType = 0;
    uint32_t outBytes = 0;
    uint8_t smallBuf[16];
    ASSERT_HRESULT_FAIL(IPC_ReadRawFrame(g_shmClient, FALSE, &outType, smallBuf, sizeof(smallBuf), &outBytes, 100));

    /* Duplicate SHM creation (squatting protection) must fail with ERROR_ALREADY_EXISTS */
    ShmChannel* squatted = NULL;
    ASSERT_EQ_INT((int)HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                  (int)IPC_CreateShmChannel(shmName, 4096U, &squatted));
    ASSERT_NULL(squatted);

    /* ABI execution mode constants */
    ASSERT_EQ_UINT(0x00FFU, CPOWER_EXEC_MODE_MASK);
    ASSERT_EQ_UINT(0x0100U, CPOWER_EXEC_FLAG_HAS_NODE_ID);
    ASSERT_EQ_UINT(0x0200U, CPOWER_EXEC_FLAG_HAS_COORDS);

    /* Non-existent SHM name */
    ShmChannel* missing = NULL;
    ASSERT_HRESULT_FAIL(IPC_OpenShmChannel(L"Local\\CPowerNonExistentChannel_99999", &missing));
    ASSERT_NULL(missing);

    /* NULL pointer safety */
    ASSERT_HRESULT_FAIL(IPC_CreateShmChannel(shmName, 4096U, NULL));
    ASSERT_HRESULT_FAIL(IPC_OpenShmChannel(NULL, &missing));
    ASSERT_HRESULT_FAIL(IPC_OpenShmChannel(shmName, NULL));
    ASSERT_HRESULT_FAIL(IPC_WriteTreePacket(NULL, frameData, 16));
    ASSERT_HRESULT_FAIL(IPC_WriteTreePacket(g_shmServer, NULL, 16));
    ASSERT_HRESULT_FAIL(IPC_ReadTreePacket(NULL, rxBuf, sizeof(rxBuf), &rxBytes, 10));
    ASSERT_HRESULT_FAIL(IPC_ReadTreePacket(g_shmClient, NULL, sizeof(rxBuf), &rxBytes, 10));
    ASSERT_HRESULT_FAIL(IPC_WriteCommandPacket(NULL, &rxCmd));
    ASSERT_HRESULT_FAIL(IPC_WriteCommandPacket(g_shmClient, NULL));
    ASSERT_HRESULT_FAIL(IPC_ReadCommandPacket(NULL, &rxCmd, 10));
    ASSERT_HRESULT_FAIL(IPC_ReadCommandPacket(g_shmServer, NULL, 10));
    IPC_CloseShmChannel(NULL);
}

typedef struct PipeServerWorkerArgs {
    PipeServer* pServer;
    CommandPacket expectedCmd;
    uint8_t responsePayload[256];
    uint32_t responseSize;
    BOOL ok;
    char errMsg[128];
} PipeServerWorkerArgs;

static DWORD WINAPI pipe_server_worker_thread(LPVOID lpParam) {
    PipeServerWorkerArgs* args = (PipeServerWorkerArgs*)lpParam;

    HRESULT hr = IPC_AcceptPipeClient(args->pServer);
    if (FAILED(hr)) {
        snprintf(args->errMsg, sizeof(args->errMsg),
                 "IPC_AcceptPipeClient failed hr=0x%08lX", (unsigned long)hr);
        args->ok = FALSE;
        return 1;
    }

    CommandPacket rxCmd;
    uint32_t bytesRead = 0;
    memset(&rxCmd, 0, sizeof(rxCmd));
    hr = IPC_PipeRead(args->pServer, &rxCmd, sizeof(rxCmd), &bytesRead);
    if (FAILED(hr) || bytesRead != sizeof(CommandPacket)) {
        snprintf(args->errMsg, sizeof(args->errMsg),
                 "Server IPC_PipeRead failed hr=0x%08lX bytes=%u", (unsigned long)hr, bytesRead);
        args->ok = FALSE;
        return 1;
    }

    if (memcmp(&rxCmd, &args->expectedCmd, sizeof(CommandPacket)) != 0) {
        snprintf(args->errMsg, sizeof(args->errMsg), "Server received CommandPacket mismatch");
        args->ok = FALSE;
        return 1;
    }

    uint32_t bytesWritten = 0;
    hr = IPC_PipeWrite(args->pServer, args->responsePayload, args->responseSize, &bytesWritten);
    if (FAILED(hr) || bytesWritten != args->responseSize) {
        snprintf(args->errMsg, sizeof(args->errMsg),
                 "Server IPC_PipeWrite failed hr=0x%08lX written=%u", (unsigned long)hr, bytesWritten);
        args->ok = FALSE;
        return 1;
    }

    args->ok = TRUE;
    return 0;
}

static void test_ipc_named_pipe_roundtrip(void) {
    WCHAR pipeName[MAX_PATH];
    make_unique_pipe_name(pipeName, MAX_PATH);

    ASSERT_HRESULT_OK(IPC_CreatePipeServer(pipeName, &g_pipeServer));
    ASSERT_NOT_NULL(g_pipeServer);

    PipeServerWorkerArgs workerArgs;
    memset(&workerArgs, 0, sizeof(workerArgs));
    workerArgs.pServer = g_pipeServer;
    workerArgs.expectedCmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    workerArgs.expectedCmd.commandId = 555;
    workerArgs.expectedCmd.commandCode = CMD_WIN32_SET_TEXT;
    workerArgs.expectedCmd.executionMode = EXEC_MODE_DIRECT_INSTANT;
    wcscpy(workerArgs.expectedCmd.textPayload, L"NamedPipeTest");
    workerArgs.expectedCmd.textPayloadBytes = (uint32_t)(wcslen(workerArgs.expectedCmd.textPayload) * sizeof(WCHAR));

    workerArgs.responseSize = 128U;
    for (uint32_t i = 0; i < workerArgs.responseSize; i++) {
        workerArgs.responsePayload[i] = (uint8_t)(i ^ 0x5A);
    }

    g_workerThreadA = CreateThread(NULL, 0, pipe_server_worker_thread, &workerArgs, 0, NULL);
    ASSERT_NOT_NULL(g_workerThreadA);

    Sleep(15);
    ASSERT_HRESULT_OK(IPC_ConnectPipeClient(pipeName, 3000, &g_pipeClient));
    ASSERT_NOT_NULL(g_pipeClient);

    uint32_t written = 0;
    ASSERT_HRESULT_OK(IPC_PipeWrite(g_pipeClient, &workerArgs.expectedCmd, sizeof(CommandPacket), &written));
    ASSERT_EQ_UINT(sizeof(CommandPacket), written);

    uint8_t rxResp[256];
    uint32_t rxRead = 0;
    memset(rxResp, 0, sizeof(rxResp));
    ASSERT_HRESULT_OK(IPC_PipeRead(g_pipeClient, rxResp, sizeof(rxResp), &rxRead));
    ASSERT_EQ_UINT(workerArgs.responseSize, rxRead);
    ASSERT_EQ_INT(0, memcmp(workerArgs.responsePayload, rxResp, workerArgs.responseSize));

    DWORD waitRes = WaitForSingleObject(g_workerThreadA, 3000);
    ASSERT_EQ_UINT(WAIT_OBJECT_0, waitRes);
    CloseHandle(g_workerThreadA);
    g_workerThreadA = NULL;

    if (!workerArgs.ok) ASSERT_FAIL(workerArgs.errMsg);
    ASSERT_TRUE(workerArgs.ok);
}

static void test_ipc_named_pipe_boundary(void) {
    PipeServer* badClient = NULL;
    ASSERT_HRESULT_FAIL(IPC_ConnectPipeClient(L"\\\\.\\pipe\\cpower_nonexistent_pipe_xyz", 30, &badClient));
    ASSERT_NULL(badClient);

    ASSERT_HRESULT_FAIL(IPC_CreatePipeServer(NULL, NULL));
    ASSERT_HRESULT_FAIL(IPC_AcceptPipeClient(NULL));
    ASSERT_HRESULT_FAIL(IPC_ConnectPipeClient(NULL, 50, &badClient));
    ASSERT_HRESULT_FAIL(IPC_ConnectPipeClient(L"\\\\.\\pipe\\x", 50, NULL));

    uint8_t dummy[16] = {0};
    uint32_t cnt = 0;
    ASSERT_HRESULT_FAIL(IPC_PipeWrite(NULL, dummy, sizeof(dummy), &cnt));
    ASSERT_HRESULT_FAIL(IPC_PipeRead(NULL, dummy, sizeof(dummy), &cnt));
    IPC_ClosePipeServer(NULL);
}

typedef struct V11PipeEchoArgs {
    PipeServer* pServer;
    AckPacket   ackPkt;
    EventPacket evtPkt;
    uint8_t     diffBuf[sizeof(DiffPacketHeader) + 2U * sizeof(TreeDiffEntry)];
    uint8_t     winBuf[sizeof(WindowPacketHeader) + 2U * sizeof(WindowInfo)];
    volatile BOOL ok;
    char        errMsg[128];
} V11PipeEchoArgs;

static DWORD WINAPI v11_pipe_echo_server_thread(LPVOID lpParam) {
    V11PipeEchoArgs* args = (V11PipeEchoArgs*)lpParam;
    args->ok = FALSE;

    HRESULT hr = IPC_AcceptPipeClient(args->pServer);
    if (FAILED(hr)) {
        snprintf(args->errMsg, sizeof(args->errMsg), "Accept failed hr=0x%08lX", (unsigned long)hr);
        return 1;
    }

    const void* pkts[4] = { &args->ackPkt, &args->evtPkt, args->diffBuf, args->winBuf };
    const uint32_t sizes[4] = {
        (uint32_t)sizeof(AckPacket),
        (uint32_t)sizeof(EventPacket),
        (uint32_t)sizeof(args->diffBuf),
        (uint32_t)sizeof(args->winBuf)
    };
    uint8_t rxTemp[4096];

    for (int i = 0; i < 4; i++) {
        uint32_t bytesRead = 0U;
        hr = IPC_PipeRead(args->pServer, rxTemp, (uint32_t)sizeof(rxTemp), &bytesRead);
        if (FAILED(hr) || bytesRead != sizes[i] || memcmp(rxTemp, pkts[i], sizes[i]) != 0) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Server mismatch at idx=%d hr=0x%08lX read=%u exp=%u",
                     i, (unsigned long)hr, bytesRead, sizes[i]);
            return 1;
        }
        uint32_t written = 0U;
        hr = IPC_PipeWrite(args->pServer, rxTemp, bytesRead, &written);
        if (FAILED(hr) || written != bytesRead) {
            snprintf(args->errMsg, sizeof(args->errMsg),
                     "Server write failed at idx=%d hr=0x%08lX", i, (unsigned long)hr);
            return 1;
        }
    }

    args->ok = TRUE;
    return 0;
}

static void test_ipc_v11_packets_shm_and_pipe_roundtrip(void) {
    WCHAR shmName[128];
    make_unique_shm_name(shmName, 128);
    ASSERT_HRESULT_OK(IPC_CreateShmChannel(shmName, 65536U, &g_shmServer));
    ASSERT_HRESULT_OK(IPC_OpenShmChannel(shmName, &g_shmClient));

    /* 1. Build sample AckPacket, EventPacket, DiffPacketHeader+2, WindowPacketHeader+2 */
    AckPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = CPOWER_ACK_PACKET_MAGIC;
    ack.commandId = 901U;
    ack.hresult = (int32_t)S_OK;
    ack.elapsedUs = 345U;
    ack.nodeCount = 12U;
    ack.diffCount = 2U;
    ack.flags = CPOWER_ACK_FLAG_HAS_TREE | CPOWER_ACK_FLAG_HAS_DIFF;

    EventPacket evt;
    memset(&evt, 0, sizeof(evt));
    evt.magic = CPOWER_EVENT_PACKET_MAGIC;
    evt.event = EVENT_OBJECT_NAMECHANGE;
    evt.hwnd = 0x00000000CAFEBABEULL;
    evt.idObject = 0;
    evt.idChild = 1;
    evt.threadId = 4321U;
    evt.timestamp = 987654U;

    uint8_t diffBuf[sizeof(DiffPacketHeader) + 2U * sizeof(TreeDiffEntry)];
    memset(diffBuf, 0, sizeof(diffBuf));
    DiffPacketHeader dHdr;
    memset(&dHdr, 0, sizeof(dHdr));
    dHdr.packetMagic = CPOWER_DIFF_PACKET_MAGIC;
    dHdr.sequenceId = 3U;
    dHdr.prevTimestampUtc = 100000ULL;
    dHdr.currTimestampUtc = 105000ULL;
    dHdr.totalPayloadSize = (uint32_t)sizeof(diffBuf);
    dHdr.entryCount = 2U;
    dHdr.addedCount = 1U;
    dHdr.modifiedCount = 1U;
    memcpy(diffBuf, &dHdr, sizeof(dHdr));

    TreeDiffEntry dEnt[2];
    memset(dEnt, 0, sizeof(dEnt));
    dEnt[0].op = DIFF_OP_ADDED;
    dEnt[0].nodeId = 4U;
    dEnt[0].prevNodeId = 0xFFFFFFFFU;
    dEnt[0].changedMask = 0U;
    dEnt[0].fingerprint = 0x1122334455667788ULL;
    dEnt[0].node.id = 4U;
    wcscpy(dEnt[0].node.name, L"AddedButton");
    dEnt[1].op = DIFF_OP_MODIFIED;
    dEnt[1].nodeId = 2U;
    dEnt[1].prevNodeId = 2U;
    dEnt[1].changedMask = CPOWER_DIFF_CHANGE_VALUE;
    dEnt[1].fingerprint = 0x8877665544332211ULL;
    dEnt[1].node.id = 2U;
    wcscpy(dEnt[1].node.value, L"ChangedText");
    memcpy(diffBuf + sizeof(dHdr), dEnt, sizeof(dEnt));

    uint8_t winBuf[sizeof(WindowPacketHeader) + 2U * sizeof(WindowInfo)];
    memset(winBuf, 0, sizeof(winBuf));
    WindowPacketHeader wHdr;
    memset(&wHdr, 0, sizeof(wHdr));
    wHdr.packetMagic = CPOWER_WINDOWS_PACKET_MAGIC;
    wHdr.windowCount = 2U;
    wHdr.totalPayloadSize = (uint32_t)sizeof(winBuf);
    memcpy(winBuf, &wHdr, sizeof(wHdr));

    WindowInfo wEnt[2];
    memset(wEnt, 0, sizeof(wEnt));
    wEnt[0].hwnd = 0x1010ULL;
    wEnt[0].pid = 1111U;
    wEnt[0].isVisible = 1U;
    wcscpy(wEnt[0].title, L"PrimaryWin");
    wcscpy(wEnt[0].className, L"CPowerWin");
    wEnt[1].hwnd = 0x2020ULL;
    wEnt[1].pid = 2222U;
    wEnt[1].isVisible = 1U;
    wEnt[1].isMinimized = 1U;
    wcscpy(wEnt[1].title, L"SecondaryWin");
    memcpy(winBuf + sizeof(wHdr), wEnt, sizeof(wEnt));

    /* 2. SHM OutRing roundtrip for all four v1.1.0 packet types */
    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_shmServer, TRUE, (uint16_t)IPC_PKT_ACK, &ack, (uint32_t)sizeof(ack)));
    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_shmServer, TRUE, (uint16_t)IPC_PKT_EVENT, &evt, (uint32_t)sizeof(evt)));
    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_shmServer, TRUE, (uint16_t)IPC_PKT_TREE_DIFF, diffBuf, (uint32_t)sizeof(diffBuf)));
    ASSERT_HRESULT_OK(IPC_WriteRawFrame(g_shmServer, TRUE, (uint16_t)IPC_PKT_WINDOWS, winBuf, (uint32_t)sizeof(winBuf)));

    uint8_t rxBuf[4096];
    uint16_t pktType = 0U;
    uint32_t rxBytes = 0U;

    ASSERT_HRESULT_OK(IPC_ReadRawFrame(g_shmClient, FALSE, &pktType, rxBuf, (uint32_t)sizeof(rxBuf), &rxBytes, 1000U));
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_ACK, pktType);
    ASSERT_EQ_UINT((uint32_t)sizeof(AckPacket), rxBytes);
    ASSERT_EQ_INT(0, memcmp(&ack, rxBuf, sizeof(AckPacket)));

    ASSERT_HRESULT_OK(IPC_ReadRawFrame(g_shmClient, FALSE, &pktType, rxBuf, (uint32_t)sizeof(rxBuf), &rxBytes, 1000U));
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_EVENT, pktType);
    ASSERT_EQ_UINT((uint32_t)sizeof(EventPacket), rxBytes);
    ASSERT_EQ_INT(0, memcmp(&evt, rxBuf, sizeof(EventPacket)));

    ASSERT_HRESULT_OK(IPC_ReadRawFrame(g_shmClient, FALSE, &pktType, rxBuf, (uint32_t)sizeof(rxBuf), &rxBytes, 1000U));
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_TREE_DIFF, pktType);
    ASSERT_EQ_UINT((uint32_t)sizeof(diffBuf), rxBytes);
    ASSERT_EQ_INT(0, memcmp(diffBuf, rxBuf, sizeof(diffBuf)));

    ASSERT_HRESULT_OK(IPC_ReadRawFrame(g_shmClient, FALSE, &pktType, rxBuf, (uint32_t)sizeof(rxBuf), &rxBytes, 1000U));
    ASSERT_EQ_UINT((uint16_t)IPC_PKT_WINDOWS, pktType);
    ASSERT_EQ_UINT((uint32_t)sizeof(winBuf), rxBytes);
    ASSERT_EQ_INT(0, memcmp(winBuf, rxBuf, sizeof(winBuf)));

    /* 3. Named Pipe message-mode roundtrip for the same 4 v1.1.0 packets */
    WCHAR pipeName[MAX_PATH];
    make_unique_pipe_name(pipeName, MAX_PATH);
    ASSERT_HRESULT_OK(IPC_CreatePipeServer(pipeName, &g_pipeServer));

    V11PipeEchoArgs pArgs;
    memset(&pArgs, 0, sizeof(pArgs));
    pArgs.pServer = g_pipeServer;
    pArgs.ackPkt = ack;
    pArgs.evtPkt = evt;
    memcpy(pArgs.diffBuf, diffBuf, sizeof(diffBuf));
    memcpy(pArgs.winBuf, winBuf, sizeof(winBuf));

    g_workerThreadA = CreateThread(NULL, 0, v11_pipe_echo_server_thread, &pArgs, 0, NULL);
    ASSERT_NOT_NULL(g_workerThreadA);
    Sleep(15);

    ASSERT_HRESULT_OK(IPC_ConnectPipeClient(pipeName, 3000U, &g_pipeClient));

    const void* txPkts[4] = { &ack, &evt, diffBuf, winBuf };
    const uint32_t txSizes[4] = {
        (uint32_t)sizeof(AckPacket),
        (uint32_t)sizeof(EventPacket),
        (uint32_t)sizeof(diffBuf),
        (uint32_t)sizeof(winBuf)
    };
    for (int i = 0; i < 4; i++) {
        uint32_t wr = 0U, rd = 0U;
        ASSERT_HRESULT_OK(IPC_PipeWrite(g_pipeClient, txPkts[i], txSizes[i], &wr));
        ASSERT_EQ_UINT(txSizes[i], wr);
        memset(rxBuf, 0, txSizes[i]);
        ASSERT_HRESULT_OK(IPC_PipeRead(g_pipeClient, rxBuf, (uint32_t)sizeof(rxBuf), &rd));
        ASSERT_EQ_UINT(txSizes[i], rd);
        ASSERT_EQ_INT(0, memcmp(txPkts[i], rxBuf, txSizes[i]));
    }

    ASSERT_EQ_UINT(WAIT_OBJECT_0, WaitForSingleObject(g_workerThreadA, 3000U));
    CloseHandle(g_workerThreadA);
    g_workerThreadA = NULL;
    if (!pArgs.ok) ASSERT_FAIL(pArgs.errMsg);
    ASSERT_TRUE(pArgs.ok);
}

void RegisterIpcTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "IpcSuite",
        "Lock-free SPSC shared memory ring buffers, Tree/Command packets, wraparound stress, and Named Pipes",
        ipc_suite_setup,
        ipc_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_Ipc_ShmCreateOpenClose",
                       "Verify ShmChannel creation, power-of-two capacity rounding, magic/version, and client attach",
                       9, TIER_1_FEATURE_COVERAGE, test_ipc_shm_create_open_close);

    test_case_register(suite, "Test_Ipc_ShmTreePacketRoundTrip",
                       "Verify TreePacketHeader + SemanticNode serialization round-trip over ShmChannel",
                       9, TIER_1_FEATURE_COVERAGE, test_ipc_shm_tree_packet_roundtrip);

    test_case_register(suite, "Test_Ipc_ShmCommandPacketRoundTrip",
                       "Verify full CommandPacket round-trip from client to server over ShmChannel",
                       9, TIER_1_FEATURE_COVERAGE, test_ipc_shm_command_packet_roundtrip);

    test_case_register(suite, "Test_Ipc_ShmConcurrentSpscWraparoundStress",
                       "Multi-threaded SPSC stress test across 1200 variable frames + 400 commands wrapping 8KB rings",
                       16, TIER_5_ADVERSARIAL, test_ipc_shm_concurrent_spsc_wraparound_stress);

    test_case_register(suite, "Test_Ipc_ShmBoundaryAndTimeouts",
                       "Verify empty-ring timeouts, oversized frame rejection, tiny read buffers, and NULL safety",
                       16, TIER_2_BOUNDARY_CORNER, test_ipc_shm_boundary_and_timeouts);

    test_case_register(suite, "Test_Ipc_NamedPipeRoundTrip",
                       "Verify worker-thread Named Pipe server accept, client connect, and message-mode round-trip",
                       10, TIER_1_FEATURE_COVERAGE, test_ipc_named_pipe_roundtrip);

    test_case_register(suite, "Test_Ipc_NamedPipeBoundary",
                       "Verify Named Pipe connection failure on missing pipe and NULL parameter safety",
                       10, TIER_2_BOUNDARY_CORNER, test_ipc_named_pipe_boundary);

    test_case_register(suite, "Test_Ipc_V11PacketsShmAndPipeRoundTrip",
                       "Verify AckPacket, EventPacket, DiffPacketHeader, and WindowPacketHeader over SHM and Pipe",
                       9, TIER_1_FEATURE_COVERAGE, test_ipc_v11_packets_shm_and_pipe_roundtrip);
}
