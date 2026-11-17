/**
 * @file pipe_transport.c
 * @brief Cross-Integrity Message-Mode Named Pipes IPC Transport.
 *
 * Implements Named Pipe server and client routines with
 * PIPE_ACCESS_DUPLEX | FILE_FLAG_WRITE_THROUGH,
 * PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
 * 256 KB kernel buffers, and cross-integrity SDDL security descriptors.
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

#define CPOWER_PIPE_BUFFER_SIZE (256U * 1024U)

HRESULT IPC_CreatePipeServer(const WCHAR* pszPipeName, PipeServer** ppServer) {
    if (!ppServer) {
        return E_INVALIDARG;
    }
    *ppServer = NULL;

    const WCHAR* pipeName = (pszPipeName != NULL) ? pszPipeName : CPOWER_DEFAULT_PIPE_NAME;
    size_t nameLen = wcslen(pipeName);
    if (nameLen == 0U || nameLen >= MAX_PATH) {
        return E_INVALIDARG;
    }

    SECURITY_ATTRIBUTES sa;
    HRESULT hr = IPC_CreateLeastPrivilegeSA(&sa);
    if (FAILED(hr)) {
        return hr;
    }

    HANDLE hPipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        CPOWER_PIPE_BUFFER_SIZE,
        CPOWER_PIPE_BUFFER_SIZE,
        0,
        &sa
    );

    DWORD createErr = GetLastError();
    IPC_FreeSecurityAttributes(&sa);

    if (hPipe == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(createErr != 0 ? createErr : ERROR_ACCESS_DENIED);
    }

    PipeServer* pServer = (PipeServer*)calloc(1, sizeof(PipeServer));
    if (!pServer) {
        CloseHandle(hPipe);
        return E_OUTOFMEMORY;
    }

    pServer->hPipe = hPipe;
    pServer->isServer = TRUE;
    pServer->isConnected = FALSE;
    wmemcpy(pServer->pipeName, pipeName, nameLen);
    pServer->pipeName[nameLen] = L'\0';

    *ppServer = pServer;
    return S_OK;
}

HRESULT IPC_AcceptPipeClient(PipeServer* pServer) {
    if (!pServer || !pServer->isServer ||
        pServer->hPipe == NULL || pServer->hPipe == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }

    if (pServer->isConnected) {
        return S_OK;
    }

    if (ConnectNamedPipe(pServer->hPipe, NULL)) {
        pServer->isConnected = TRUE;
        return S_OK;
    }

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        pServer->isConnected = TRUE;
        return S_OK;
    }

    return HRESULT_FROM_WIN32(err);
}

HRESULT IPC_ConnectPipeClient(const WCHAR* pszPipeName, DWORD timeoutMs, PipeServer** ppClient) {
    if (!ppClient) {
        return E_INVALIDARG;
    }
    *ppClient = NULL;

    const WCHAR* pipeName = (pszPipeName != NULL) ? pszPipeName : CPOWER_DEFAULT_PIPE_NAME;
    size_t nameLen = wcslen(pipeName);
    if (nameLen == 0U || nameLen >= MAX_PATH) {
        return E_INVALIDARG;
    }

    ULONGLONG startMs = GetTickCount64();
    HANDLE hPipe = INVALID_HANDLE_VALUE;

    for (;;) {
        hPipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH,
            NULL
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY) {
            return HRESULT_FROM_WIN32(err);
        }

        DWORD waitMs = NMPWAIT_NOWAIT;
        if (timeoutMs == INFINITE) {
            waitMs = 100U;
        } else {
            ULONGLONG elapsed = GetTickCount64() - startMs;
            if (elapsed >= (ULONGLONG)timeoutMs) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            ULONGLONG remaining = (ULONGLONG)timeoutMs - elapsed;
            waitMs = (remaining > 100ULL) ? 100U : (DWORD)remaining;
            if (waitMs == 0U) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }

        if (!WaitNamedPipeW(pipeName, waitMs)) {
            DWORD waitErr = GetLastError();
            if (waitErr == ERROR_SEM_TIMEOUT || waitErr == ERROR_TIMEOUT) {
                if (timeoutMs != INFINITE && (GetTickCount64() - startMs) >= (ULONGLONG)timeoutMs) {
                    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                continue;
            }
            return HRESULT_FROM_WIN32(waitErr);
        }
    }

    DWORD pipeMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &pipeMode, NULL, NULL)) {
        DWORD err = GetLastError();
        CloseHandle(hPipe);
        return HRESULT_FROM_WIN32(err);
    }

    PipeServer* pClient = (PipeServer*)calloc(1, sizeof(PipeServer));
    if (!pClient) {
        CloseHandle(hPipe);
        return E_OUTOFMEMORY;
    }

    pClient->hPipe = hPipe;
    pClient->isServer = FALSE;
    pClient->isConnected = TRUE;
    wmemcpy(pClient->pipeName, pipeName, nameLen);
    pClient->pipeName[nameLen] = L'\0';

    *ppClient = pClient;
    return S_OK;
}

void IPC_ClosePipeServer(PipeServer* pServer) {
    if (!pServer) {
        return;
    }
    if (pServer->hPipe != NULL && pServer->hPipe != INVALID_HANDLE_VALUE) {
        if (pServer->isServer && pServer->isConnected) {
            FlushFileBuffers(pServer->hPipe);
            DisconnectNamedPipe(pServer->hPipe);
        }
        CloseHandle(pServer->hPipe);
        pServer->hPipe = NULL;
    }
    pServer->isConnected = FALSE;
    free(pServer);
}

HRESULT IPC_PipeWrite(PipeServer* pPipe, const void* pData, uint32_t size, uint32_t* pBytesWritten) {
    if (pBytesWritten) {
        *pBytesWritten = 0U;
    }
    if (!pPipe || pPipe->hPipe == NULL || pPipe->hPipe == INVALID_HANDLE_VALUE ||
        !pData || size == 0U) {
        return E_INVALIDARG;
    }

    DWORD written = 0U;
    if (!WriteFile(pPipe->hPipe, pData, (DWORD)size, &written, NULL)) {
        DWORD err = GetLastError();
        if (pBytesWritten) {
            *pBytesWritten = (uint32_t)written;
        }
        return HRESULT_FROM_WIN32(err);
    }

    if (pBytesWritten) {
        *pBytesWritten = (uint32_t)written;
    }
    if (written != (DWORD)size) {
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    return S_OK;
}

HRESULT IPC_PipeRead(PipeServer* pPipe, void* pBuffer, uint32_t maxBytes, uint32_t* pBytesRead) {
    if (pBytesRead) {
        *pBytesRead = 0U;
    }
    if (!pPipe || pPipe->hPipe == NULL || pPipe->hPipe == INVALID_HANDLE_VALUE ||
        !pBuffer || maxBytes == 0U) {
        return E_INVALIDARG;
    }

    DWORD bytesRead = 0U;
    if (!ReadFile(pPipe->hPipe, pBuffer, (DWORD)maxBytes, &bytesRead, NULL)) {
        DWORD err = GetLastError();
        if (pBytesRead) {
            *pBytesRead = (uint32_t)bytesRead;
        }
        if (err == ERROR_MORE_DATA) {
            /* Drain remaining bytes of the oversized message so the message stream stays aligned */
            uint8_t drainBuf[4096];
            for (;;) {
                DWORD drainRead = 0U;
                if (ReadFile(pPipe->hPipe, drainBuf, (DWORD)sizeof(drainBuf), &drainRead, NULL)) {
                    break;
                }
                if (GetLastError() != ERROR_MORE_DATA) {
                    break;
                }
            }
        }
        return HRESULT_FROM_WIN32(err);
    }

    if (pBytesRead) {
        *pBytesRead = (uint32_t)bytesRead;
    }
    return S_OK;
}
