/**
 * @file shm_transport.c
 * @brief Lock-Free SPSC Dual Circular Ring Buffer Shared Memory IPC Transport.
 *
 * Implements dual Single-Producer Single-Consumer circular ring buffers
 * (OutRing: Agent -> Client, InRing: Client -> Agent) inside a single
 * CreateFileMappingW section with cross-integrity SDDL security descriptors,
 * 64-byte cache-line isolation, power-of-two bitmask wrapping, MemoryBarrier()
 * release/acquire ordering, and hybrid CPU pause + WaitForSingleObject waits.
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

#include "ipc.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CPOWER_MAX_CHANNEL_NAME_CHARS 120U
#define CPOWER_MAX_RING_CAPACITY      (512U * 1024U * 1024U)
#define CPOWER_SPIN_ITERATIONS        100

static void shm_cpu_pause(void) {
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_ia32_pause();
#else
    YieldProcessor();
#endif
}

static size_t shm_align64(size_t value) {
    return (value + 63U) & ~(size_t)63U;
}

static HRESULT shm_round_up_pow2(size_t requested, uint32_t* pCapacityOut) {
    if (requested == 0U) {
        *pCapacityOut = CPOWER_DEFAULT_RING_CAPACITY;
        return S_OK;
    }
    if (requested > (size_t)CPOWER_MAX_RING_CAPACITY) {
        return E_INVALIDARG;
    }

    uint32_t v = (uint32_t)requested;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;

    if (v == 0U) {
        return E_INVALIDARG;
    }

    *pCapacityOut = v;
    return S_OK;
}

static BOOL shm_build_event_name(const WCHAR* baseName, const WCHAR* suffix, WCHAR* outBuf, size_t outChars) {
    if (!baseName || !suffix || !outBuf || outChars == 0U) {
        return FALSE;
    }
    size_t baseLen = wcslen(baseName);
    size_t sufLen = wcslen(suffix);
    if (baseLen + sufLen + 1U > outChars) {
        return FALSE;
    }
    wmemcpy(outBuf, baseName, baseLen);
    wmemcpy(outBuf + baseLen, suffix, sufLen);
    outBuf[baseLen + sufLen] = L'\0';
    return TRUE;
}

HRESULT IPC_CreateLeastPrivilegeSA(SECURITY_ATTRIBUTES* pSa) {
    if (!pSa) {
        return E_INVALIDARG;
    }
    memset(pSa, 0, sizeof(*pSa));
    pSa->nLength = sizeof(SECURITY_ATTRIBUTES);
    pSa->bInheritHandle = FALSE;

    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    DWORD needed = 0U;
    if (!GetTokenInformation(hToken, TokenUser, NULL, 0U, &needed)) {
        DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
            CloseHandle(hToken);
            return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
        }
    }
    if (needed < sizeof(TOKEN_USER)) {
        CloseHandle(hToken);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    union {
        TOKEN_USER user;
        uint8_t    raw[256];
    } stackBuf;
    TOKEN_USER* pTokenUser = (needed <= sizeof(stackBuf))
        ? &stackBuf.user
        : (TOKEN_USER*)malloc(needed);
    if (!pTokenUser) {
        CloseHandle(hToken);
        return E_OUTOFMEMORY;
    }

    HRESULT hr = S_OK;
    LPWSTR pszUserSid = NULL;
    if (!GetTokenInformation(hToken, TokenUser, pTokenUser, needed, &needed)) {
        DWORD err = GetLastError();
        hr = HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    } else if (!pTokenUser->User.Sid || !IsValidSid(pTokenUser->User.Sid)) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_SID);
    } else if (!ConvertSidToStringSidW(pTokenUser->User.Sid, &pszUserSid) || !pszUserSid) {
        DWORD err = GetLastError();
        hr = HRESULT_FROM_WIN32(err != 0 ? err : ERROR_INVALID_SID);
    } else {
        WCHAR sddl[512];
        int written = swprintf(
            sddl,
            512,
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%ls)S:(ML;;NW;;;ME)",
            pszUserSid
        );
        if (written <= 0 || written >= 512) {
            hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        } else if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl,
                SDDL_REVISION_1,
                &pSa->lpSecurityDescriptor,
                NULL)) {
            DWORD err = GetLastError();
            pSa->lpSecurityDescriptor = NULL;
            hr = HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
        }
    }

    if (pszUserSid) {
        LocalFree(pszUserSid);
    }
    if (pTokenUser != &stackBuf.user) {
        free(pTokenUser);
    }
    CloseHandle(hToken);
    return hr;
}

void IPC_FreeSecurityAttributes(SECURITY_ATTRIBUTES* pSa) {
    if (pSa && pSa->lpSecurityDescriptor) {
        LocalFree(pSa->lpSecurityDescriptor);
        pSa->lpSecurityDescriptor = NULL;
    }
}

static void shm_ring_write_bytes(uint8_t* pData, uint32_t capacity, uint32_t mask,
                                 uint32_t startIdx, const void* pSrc, uint32_t len) {
    if (len == 0U) {
        return;
    }
    uint32_t offset = startIdx & mask;
    uint32_t firstChunk = capacity - offset;
    if (firstChunk >= len) {
        memcpy(pData + offset, pSrc, len);
    } else {
        memcpy(pData + offset, pSrc, firstChunk);
        memcpy(pData, (const uint8_t*)pSrc + firstChunk, len - firstChunk);
    }
}

static void shm_ring_read_bytes(const uint8_t* pData, uint32_t capacity, uint32_t mask,
                                uint32_t startIdx, void* pDst, uint32_t len) {
    if (len == 0U) {
        return;
    }
    uint32_t offset = startIdx & mask;
    uint32_t firstChunk = capacity - offset;
    if (firstChunk >= len) {
        memcpy(pDst, pData + offset, len);
    } else {
        memcpy(pDst, pData + offset, firstChunk);
        memcpy((uint8_t*)pDst + firstChunk, pData, len - firstChunk);
    }
}

static void shm_init_ring_header(ShmRingBufferHeader* pRing, uint32_t capacity) {
    memset(pRing, 0, sizeof(*pRing));
    pRing->version = CPOWER_SHM_VERSION;
    pRing->capacity = capacity;
    pRing->mask = capacity - 1U;
    pRing->writeIdx = 0U;
    pRing->readIdx = 0U;
    MemoryBarrier();
    pRing->magic = CPOWER_SHM_MAGIC;
    MemoryBarrier();
}

static BOOL shm_validate_ring_header(const ShmRingBufferHeader* pRing, uint32_t expectedCap) {
    if (!pRing) {
        return FALSE;
    }
    if (pRing->magic != CPOWER_SHM_MAGIC) {
        return FALSE;
    }
    MemoryBarrier();
    if (pRing->version != CPOWER_SHM_VERSION) {
        return FALSE;
    }
    uint32_t cap = pRing->capacity;
    if (cap == 0U || (cap & (cap - 1U)) != 0U) {
        return FALSE;
    }
    if (pRing->mask != cap - 1U) {
        return FALSE;
    }
    if (expectedCap != 0U && cap != expectedCap) {
        return FALSE;
    }
    return TRUE;
}

HRESULT IPC_CreateShmChannel(const WCHAR* pszName, size_t capacity, ShmChannel** ppChannel) {
    if (!ppChannel || !pszName) {
        return E_INVALIDARG;
    }
    *ppChannel = NULL;

    const WCHAR* channelName = pszName;
    size_t nameLen = wcslen(channelName);
    if (nameLen == 0U || nameLen >= CPOWER_MAX_CHANNEL_NAME_CHARS) {
        return E_INVALIDARG;
    }

    uint32_t ringCap = 0U;
    HRESULT hr = shm_round_up_pow2(capacity, &ringCap);
    if (FAILED(hr)) {
        return hr;
    }

    size_t inRingOffset = shm_align64(sizeof(ShmRingBufferHeader) + (size_t)ringCap);
    size_t totalMapSize = inRingOffset + sizeof(ShmRingBufferHeader) + (size_t)ringCap;

    WCHAR outEvtName[256];
    WCHAR inEvtName[256];
    if (!shm_build_event_name(channelName, L"_OutEvt", outEvtName, 256U) ||
        !shm_build_event_name(channelName, L"_InEvt", inEvtName, 256U)) {
        return E_INVALIDARG;
    }

    SECURITY_ATTRIBUTES sa;
    hr = IPC_CreateLeastPrivilegeSA(&sa);
    if (FAILED(hr)) {
        return hr;
    }

    /* Create sync events BEFORE CreateFileMappingW and reject squatted objects */
    SetLastError(0);
    HANDLE hOutEvt = CreateEventW(&sa, FALSE, FALSE, outEvtName);
    DWORD outEvtErr = GetLastError();
    if (hOutEvt == NULL) {
        IPC_FreeSecurityAttributes(&sa);
        return HRESULT_FROM_WIN32(outEvtErr != 0 ? outEvtErr : ERROR_ACCESS_DENIED);
    }
    if (outEvtErr == ERROR_ALREADY_EXISTS) {
        CloseHandle(hOutEvt);
        IPC_FreeSecurityAttributes(&sa);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    SetLastError(0);
    HANDLE hInEvt = CreateEventW(&sa, FALSE, FALSE, inEvtName);
    DWORD inEvtErr = GetLastError();
    if (hInEvt == NULL) {
        CloseHandle(hOutEvt);
        IPC_FreeSecurityAttributes(&sa);
        return HRESULT_FROM_WIN32(inEvtErr != 0 ? inEvtErr : ERROR_ACCESS_DENIED);
    }
    if (inEvtErr == ERROR_ALREADY_EXISTS) {
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        IPC_FreeSecurityAttributes(&sa);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    DWORD sizeHigh = (DWORD)((uint64_t)totalMapSize >> 32);
    DWORD sizeLow = (DWORD)((uint64_t)totalMapSize & 0xFFFFFFFFULL);

    SetLastError(0);
    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, sizeHigh, sizeLow, channelName);
    DWORD mapErr = GetLastError();
    IPC_FreeSecurityAttributes(&sa);
    if (hMap == NULL) {
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        return HRESULT_FROM_WIN32(mapErr != 0 ? mapErr : ERROR_ACCESS_DENIED);
    }
    if (mapErr == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMap);
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    void* pView = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, totalMapSize);
    if (pView == NULL) {
        DWORD err = GetLastError();
        CloseHandle(hMap);
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    ShmChannel* pCh = (ShmChannel*)calloc(1, sizeof(ShmChannel));
    if (!pCh) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        return E_OUTOFMEMORY;
    }

    uint8_t* pBytes = (uint8_t*)pView;
    ShmRingBufferHeader* pOutRing = (ShmRingBufferHeader*)pBytes;
    uint8_t* pOutData = pBytes + sizeof(ShmRingBufferHeader);
    ShmRingBufferHeader* pInRing = (ShmRingBufferHeader*)(pBytes + inRingOffset);
    uint8_t* pInData = pBytes + inRingOffset + sizeof(ShmRingBufferHeader);

    shm_init_ring_header(pOutRing, ringCap);
    shm_init_ring_header(pInRing, ringCap);

    pCh->hMapFile = hMap;
    pCh->pBaseView = pView;
    pCh->totalMapSize = totalMapSize;
    pCh->ringCapacity = (size_t)ringCap;
    pCh->pOutRing = pOutRing;
    pCh->pOutData = pOutData;
    pCh->pInRing = pInRing;
    pCh->pInData = pInData;
    pCh->hOutEvent = hOutEvt;
    pCh->hInEvent = hInEvt;
    pCh->isServer = TRUE;
    pCh->nextSeqId = 1U;
    wmemcpy(pCh->channelName, channelName, nameLen);
    pCh->channelName[nameLen] = L'\0';

    *ppChannel = pCh;
    return S_OK;
}

HRESULT IPC_OpenShmChannel(const WCHAR* pszName, ShmChannel** ppChannel) {
    if (!ppChannel || !pszName) {
        return E_INVALIDARG;
    }
    *ppChannel = NULL;

    const WCHAR* channelName = pszName;
    size_t nameLen = wcslen(channelName);
    if (nameLen == 0U || nameLen >= CPOWER_MAX_CHANNEL_NAME_CHARS) {
        return E_INVALIDARG;
    }

    WCHAR outEvtName[256];
    WCHAR inEvtName[256];
    if (!shm_build_event_name(channelName, L"_OutEvt", outEvtName, 256U) ||
        !shm_build_event_name(channelName, L"_InEvt", inEvtName, 256U)) {
        return E_INVALIDARG;
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, channelName);
    if (hMap == NULL) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    void* pView = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (pView == NULL) {
        DWORD err = GetLastError();
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(err);
    }

    MEMORY_BASIC_INFORMATION mbi;
    memset(&mbi, 0, sizeof(mbi));
    if (VirtualQuery(pView, &mbi, sizeof(mbi)) == 0 ||
        mbi.RegionSize < sizeof(ShmRingBufferHeader)) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    MemoryBarrier();
    uint8_t* pBytes = (uint8_t*)pView;
    ShmRingBufferHeader* pOutRing = (ShmRingBufferHeader*)pBytes;
    if (!shm_validate_ring_header(pOutRing, 0U)) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    uint32_t ringCap = pOutRing->capacity;
    if (ringCap > CPOWER_MAX_RING_CAPACITY) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    size_t inRingOffset = shm_align64(sizeof(ShmRingBufferHeader) + (size_t)ringCap);
    size_t totalMapSize = inRingOffset + sizeof(ShmRingBufferHeader) + (size_t)ringCap;
    if (mbi.RegionSize < totalMapSize) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    ShmRingBufferHeader* pInRing = (ShmRingBufferHeader*)(pBytes + inRingOffset);
    if (!shm_validate_ring_header(pInRing, ringCap)) {
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    HANDLE hOutEvt = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, outEvtName);
    if (hOutEvt == NULL) {
        DWORD err = GetLastError();
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(err);
    }

    HANDLE hInEvt = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, inEvtName);
    if (hInEvt == NULL) {
        DWORD err = GetLastError();
        CloseHandle(hOutEvt);
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return HRESULT_FROM_WIN32(err);
    }

    ShmChannel* pCh = (ShmChannel*)calloc(1, sizeof(ShmChannel));
    if (!pCh) {
        CloseHandle(hInEvt);
        CloseHandle(hOutEvt);
        UnmapViewOfFile(pView);
        CloseHandle(hMap);
        return E_OUTOFMEMORY;
    }

    pCh->hMapFile = hMap;
    pCh->pBaseView = pView;
    pCh->totalMapSize = totalMapSize;
    pCh->ringCapacity = (size_t)ringCap;
    pCh->pOutRing = pOutRing;
    pCh->pOutData = pBytes + sizeof(ShmRingBufferHeader);
    pCh->pInRing = pInRing;
    pCh->pInData = pBytes + inRingOffset + sizeof(ShmRingBufferHeader);
    pCh->hOutEvent = hOutEvt;
    pCh->hInEvent = hInEvt;
    pCh->isServer = FALSE;
    pCh->nextSeqId = 1U;
    wmemcpy(pCh->channelName, channelName, nameLen);
    pCh->channelName[nameLen] = L'\0';

    *ppChannel = pCh;
    return S_OK;
}

void IPC_CloseShmChannel(ShmChannel* pChannel) {
    if (!pChannel) {
        return;
    }
    CPOWER_SAFE_CLOSE_HANDLE(pChannel->hOutEvent);
    CPOWER_SAFE_CLOSE_HANDLE(pChannel->hInEvent);
    if (pChannel->pBaseView != NULL) {
        UnmapViewOfFile(pChannel->pBaseView);
        pChannel->pBaseView = NULL;
    }
    CPOWER_SAFE_CLOSE_HANDLE(pChannel->hMapFile);
    pChannel->pOutRing = NULL;
    pChannel->pOutData = NULL;
    pChannel->pInRing = NULL;
    pChannel->pInData = NULL;
    free(pChannel);
}

HRESULT IPC_WriteRawFrame(ShmChannel* pChannel, BOOL toClient, uint16_t pktType, const void* pPayload, uint32_t payloadSize) {
    if (!pChannel) {
        return E_INVALIDARG;
    }
    if (payloadSize > 0U && pPayload == NULL) {
        return E_INVALIDARG;
    }

    ShmRingBufferHeader* pRing = toClient ? pChannel->pOutRing : pChannel->pInRing;
    uint8_t* pData = toClient ? pChannel->pOutData : pChannel->pInData;
    HANDLE hEvt = toClient ? pChannel->hOutEvent : pChannel->hInEvent;

    if (!pRing || !pData) {
        return E_INVALIDARG;
    }

    uint32_t cap = pRing->capacity;
    uint32_t mask = pRing->mask;
    if (cap == 0U || (cap & (cap - 1U)) != 0U || mask != cap - 1U) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    if ((size_t)payloadSize > (size_t)cap ||
        (size_t)sizeof(IPC_FrameHeader) + (size_t)payloadSize > (size_t)cap) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    uint32_t wIdx = pRing->writeIdx;
    uint32_t rIdx = pRing->readIdx;
    MemoryBarrier();

    uint32_t used = wIdx - rIdx;
    if (used > cap) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    uint32_t totalBytes = (uint32_t)sizeof(IPC_FrameHeader) + payloadSize;
    uint32_t freeBytes = cap - used;
    if (freeBytes < totalBytes) {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }

    IPC_FrameHeader hdr;
    hdr.payloadSize = payloadSize;
    hdr.packetType = pktType;
    hdr.flags = 0U;

    shm_ring_write_bytes(pData, cap, mask, wIdx, &hdr, (uint32_t)sizeof(hdr));
    if (payloadSize > 0U) {
        shm_ring_write_bytes(pData, cap, mask, wIdx + (uint32_t)sizeof(hdr), pPayload, payloadSize);
    }

    MemoryBarrier();
    pRing->writeIdx = wIdx + totalBytes;
    MemoryBarrier();

    if (hEvt != NULL) {
        SetEvent(hEvt);
    }
    return S_OK;
}

static HRESULT shm_try_read_frame(ShmRingBufferHeader* pRing, const uint8_t* pData,
                                  uint16_t* pOutType, void* pOutBuffer, uint32_t maxBytes,
                                  uint32_t* pOutBytes, BOOL* pGotData) {
    *pGotData = FALSE;

    uint32_t cap = pRing->capacity;
    uint32_t mask = pRing->mask;
    uint32_t wIdx = pRing->writeIdx;
    uint32_t rIdx = pRing->readIdx;
    MemoryBarrier();

    uint32_t avail = wIdx - rIdx;
    if (avail > cap) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (avail < (uint32_t)sizeof(IPC_FrameHeader)) {
        return S_OK;
    }

    *pGotData = TRUE;

    IPC_FrameHeader hdr;
    shm_ring_read_bytes(pData, cap, mask, rIdx, &hdr, (uint32_t)sizeof(hdr));

    if ((size_t)hdr.payloadSize > (size_t)cap - sizeof(IPC_FrameHeader) ||
        avail < (uint32_t)sizeof(IPC_FrameHeader) + hdr.payloadSize) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    if (pOutType) {
        *pOutType = hdr.packetType;
    }
    if (pOutBytes) {
        *pOutBytes = hdr.payloadSize;
    }

    if (hdr.payloadSize > maxBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    if (hdr.payloadSize > 0U) {
        shm_ring_read_bytes(pData, cap, mask, rIdx + (uint32_t)sizeof(hdr), pOutBuffer, hdr.payloadSize);
    }

    MemoryBarrier();
    pRing->readIdx = rIdx + (uint32_t)sizeof(hdr) + hdr.payloadSize;
    return S_OK;
}

HRESULT IPC_ReadRawFrame(ShmChannel* pChannel, BOOL fromClient, uint16_t* pOutType,
                         void* pOutBuffer, uint32_t maxBytes, uint32_t* pOutBytes, DWORD timeoutMs) {
    if (!pChannel || !pOutBuffer) {
        return E_INVALIDARG;
    }

    ShmRingBufferHeader* pRing = fromClient ? pChannel->pInRing : pChannel->pOutRing;
    const uint8_t* pData = fromClient ? pChannel->pInData : pChannel->pOutData;
    HANDLE hEvt = fromClient ? pChannel->hInEvent : pChannel->hOutEvent;

    if (!pRing || !pData) {
        return E_INVALIDARG;
    }
    if (pRing->capacity == 0U || (pRing->capacity & (pRing->capacity - 1U)) != 0U ||
        pRing->mask != pRing->capacity - 1U) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    if (pOutType) {
        *pOutType = IPC_PKT_UNKNOWN;
    }
    if (pOutBytes) {
        *pOutBytes = 0U;
    }

    ULONGLONG startMs = GetTickCount64();

    for (;;) {
        for (int spin = 0; spin < CPOWER_SPIN_ITERATIONS; ++spin) {
            BOOL gotData = FALSE;
            HRESULT hr = shm_try_read_frame(pRing, pData, pOutType, pOutBuffer, maxBytes, pOutBytes, &gotData);
            if (gotData) {
                return hr;
            }
            if (FAILED(hr)) {
                return hr;
            }
            if (timeoutMs == 0U) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            shm_cpu_pause();
        }

        DWORD waitSlice = 20U;
        if (timeoutMs != INFINITE) {
            ULONGLONG elapsed = GetTickCount64() - startMs;
            if (elapsed >= (ULONGLONG)timeoutMs) {
                BOOL gotData = FALSE;
                HRESULT hr = shm_try_read_frame(pRing, pData, pOutType, pOutBuffer, maxBytes, pOutBytes, &gotData);
                if (gotData || FAILED(hr)) {
                    return hr;
                }
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            ULONGLONG remaining = (ULONGLONG)timeoutMs - elapsed;
            if (remaining < (ULONGLONG)waitSlice) {
                waitSlice = (DWORD)remaining;
            }
        }

        if (hEvt != NULL) {
            WaitForSingleObject(hEvt, waitSlice);
        } else {
            Sleep(waitSlice > 0U ? 1U : 0U);
        }
    }
}

HRESULT IPC_WriteTreePacket(ShmChannel* pChannel, const void* pData, size_t size) {
    if (!pChannel || !pData || size == 0U) {
        return E_INVALIDARG;
    }
    if (size > (size_t)UINT32_MAX) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    return IPC_WriteRawFrame(pChannel, TRUE, (uint16_t)IPC_PKT_TREE, pData, (uint32_t)size);
}

HRESULT IPC_ReadTreePacket(ShmChannel* pChannel, void* pBuffer, size_t bufferSize, size_t* pBytesRead, DWORD timeoutMs) {
    if (!pChannel || !pBuffer || bufferSize == 0U) {
        return E_INVALIDARG;
    }

    uint32_t maxBytes = (bufferSize > (size_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)bufferSize;
    uint16_t pktType = IPC_PKT_UNKNOWN;
    uint32_t bytesRead = 0U;

    HRESULT hr = IPC_ReadRawFrame(pChannel, FALSE, &pktType, pBuffer, maxBytes, &bytesRead, timeoutMs);
    if (pBytesRead) {
        *pBytesRead = (size_t)bytesRead;
    }
    if (FAILED(hr)) {
        return hr;
    }
    if (pktType != (uint16_t)IPC_PKT_TREE) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    return S_OK;
}

HRESULT IPC_WriteCommandPacket(ShmChannel* pChannel, const CommandPacket* pCmd) {
    if (!pChannel || !pCmd) {
        return E_INVALIDARG;
    }
    if (pCmd->packetMagic != 0U && pCmd->packetMagic != CPOWER_CMD_PACKET_MAGIC) {
        return E_INVALIDARG;
    }
    if (pCmd->textPayloadBytes > (uint32_t)sizeof(pCmd->textPayload)) {
        return E_INVALIDARG;
    }

    CommandPacket localCmd = *pCmd;
    if (localCmd.packetMagic == 0U) {
        localCmd.packetMagic = CPOWER_CMD_PACKET_MAGIC;
    }

    return IPC_WriteRawFrame(pChannel, FALSE, (uint16_t)IPC_PKT_COMMAND, &localCmd, (uint32_t)sizeof(CommandPacket));
}

HRESULT IPC_ReadCommandPacket(ShmChannel* pChannel, CommandPacket* pCmdOut, DWORD timeoutMs) {
    if (!pChannel || !pCmdOut) {
        return E_INVALIDARG;
    }

    uint16_t pktType = IPC_PKT_UNKNOWN;
    uint32_t bytesRead = 0U;

    HRESULT hr = IPC_ReadRawFrame(pChannel, TRUE, &pktType, pCmdOut, (uint32_t)sizeof(CommandPacket), &bytesRead, timeoutMs);
    if (FAILED(hr)) {
        return hr;
    }
    if (pktType != (uint16_t)IPC_PKT_COMMAND ||
        bytesRead != (uint32_t)sizeof(CommandPacket) ||
        pCmdOut->packetMagic != CPOWER_CMD_PACKET_MAGIC) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    return S_OK;
}
