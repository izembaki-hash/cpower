/**
 * @file hook_injector.c
 * @brief Opt-in GDI/User32 Text-Capture Hook Injector, Ring Reader, and Clean Detach.
 *
 * Manages explicit injection of `cpower_hook64.dll` into a specific 64-bit
 * target process PID, invokes `CPowerHook_Install` outside Loader Lock,
 * reads captured text records from the cross-integrity MPSC shared-memory
 * ring buffer, and cleanly unpatches/unloads on detach.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "text_hook.h"
#include "ipc.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

#ifndef IMAGE_FILE_MACHINE_UNKNOWN
#define IMAGE_FILE_MACHINE_UNKNOWN 0
#endif
#ifndef IMAGE_FILE_MACHINE_AMD64
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#endif

static void hook_build_shm_name(DWORD pid, WCHAR* pBuf, size_t bufCap) {
    _snwprintf(pBuf, bufCap, CPOWER_TEXT_HOOK_SHM_FMT, (unsigned long)pid);
    pBuf[bufCap - 1U] = L'\0';
}

static HRESULT hook_resolve_default_dll_path(WCHAR* pOutPath, size_t cap) {
    if (!pOutPath || cap < 32U) {
        return E_INVALIDARG;
    }

    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) {
        lastSlash = wcsrchr(exePath, L'/');
    }
    if (!lastSlash) {
        return E_FAIL;
    }
    *(lastSlash + 1) = L'\0';

    int written = _snwprintf(pOutPath, cap, L"%scpower_hook64.dll", exePath);
    if (written <= 0 || (size_t)written >= cap) {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }
    return S_OK;
}

static BOOL hook_find_remote_module(
    DWORD pid,
    const WCHAR* pszModuleLeafName,
    HMODULE* pOutBase,
    WCHAR* pOutFullPath,
    size_t fullPathCap
) {
    if (!pszModuleLeafName || !pOutBase) {
        return FALSE;
    }
    *pOutBase = NULL;
    if (pOutFullPath && fullPathCap > 0) {
        pOutFullPath[0] = L'\0';
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    BOOL found = FALSE;
    MODULEENTRY32W me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (CompareStringOrdinal(me.szModule, -1, pszModuleLeafName, -1, TRUE) == CSTR_EQUAL) {
                *pOutBase = me.hModule;
                if (pOutFullPath && fullPathCap > 0) {
                    wcsncpy(pOutFullPath, me.szExePath, fullPathCap - 1U);
                    pOutFullPath[fullPathCap - 1U] = L'\0';
                }
                found = TRUE;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);
    return found;
}

HRESULT Hook_IsProcess64Bit(DWORD pid, BOOL* pIs64Out) {
    if (!pIs64Out) {
        return E_POINTER;
    }
    *pIs64Out = FALSE;
    if (pid == 0) {
        return E_INVALIDARG;
    }

    SetLastError(0);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    typedef BOOL (WINAPI *PFN_IsWow64Process2)(HANDLE, USHORT*, USHORT*);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pIsWow2Raw = hKernel32 ? GetProcAddress(hKernel32, "IsWow64Process2") : NULL;
    PFN_IsWow64Process2 pIsWow64Process2 = NULL;
    if (pIsWow2Raw) {
        memcpy(&pIsWow64Process2, &pIsWow2Raw, sizeof(pIsWow64Process2));
    }

    if (pIsWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        BOOL ok2 = pIsWow64Process2(hProc, &processMachine, &nativeMachine);
        DWORD err2 = ok2 ? 0U : GetLastError();
        CloseHandle(hProc);
        if (!ok2) {
            return HRESULT_FROM_WIN32(err2 != 0U ? err2 : ERROR_GEN_FAILURE);
        }
        *pIs64Out = (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
                     nativeMachine == IMAGE_FILE_MACHINE_AMD64) ? TRUE : FALSE;
        return S_OK;
    }

    BOOL isWow64 = FALSE;
    BOOL ok = IsWow64Process(hProc, &isWow64);
    DWORD err = ok ? 0U : GetLastError();
    CloseHandle(hProc);

    if (!ok) {
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_GEN_FAILURE);
    }

    *pIs64Out = isWow64 ? FALSE : TRUE;
    return S_OK;
}

static HRESULT hook_invoke_remote_export(
    HANDLE hProc,
    HMODULE hRemoteMod,
    const WCHAR* pszLocalDllPath,
    const char* pszExportName,
    DWORD timeoutMs,
    DWORD* pExitCodeOut
) {
    if (pExitCodeOut) {
        *pExitCodeOut = 0U;
    }
    HMODULE hLocalRef = LoadLibraryExW(pszLocalDllPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!hLocalRef) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_MOD_NOT_FOUND);
    }

    FARPROC pExpRaw = GetProcAddress(hLocalRef, pszExportName);
    if (!pExpRaw) {
        DWORD err = GetLastError();
        FreeLibrary(hLocalRef);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_PROC_NOT_FOUND);
    }

    uintptr_t expAddr = 0;
    memcpy(&expAddr, &pExpRaw, sizeof(expAddr));
    ptrdiff_t offset = (const uint8_t*)expAddr - (const uint8_t*)hLocalRef;
    FreeLibrary(hLocalRef);

    if (offset <= 0) {
        return E_FAIL;
    }

    uintptr_t remoteExpAddr = (uintptr_t)((uint8_t*)hRemoteMod + offset);
    LPTHREAD_START_ROUTINE pRemoteStart = NULL;
    memcpy(&pRemoteStart, &remoteExpAddr, sizeof(pRemoteStart));

    SetLastError(0);
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, pRemoteStart, NULL, 0, NULL);
    if (!hThread) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_ACCESS_DENIED);
    }

    DWORD waitRes = WaitForSingleObject(hThread, timeoutMs);
    DWORD exitCode = 0U;
    BOOL gotExit = GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    if (waitRes != WAIT_OBJECT_0) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (!gotExit) {
        return E_FAIL;
    }
    if (pExitCodeOut) {
        *pExitCodeOut = exitCode;
    }
    return S_OK;
}

static void hook_remote_freelibrary(HANDLE hProc, HMODULE hRemoteMod, DWORD timeoutMs) {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pFreeLibRaw = hKernel32 ? GetProcAddress(hKernel32, "FreeLibrary") : NULL;
    LPTHREAD_START_ROUTINE pFreeStart = NULL;
    if (pFreeLibRaw) {
        memcpy(&pFreeStart, &pFreeLibRaw, sizeof(pFreeStart));
    }
    if (!pFreeStart) {
        return;
    }
    HANDLE hFreeThread = CreateRemoteThread(hProc, NULL, 0, pFreeStart, hRemoteMod, 0, NULL);
    if (hFreeThread) {
        WaitForSingleObject(hFreeThread, timeoutMs);
        CloseHandle(hFreeThread);
    }
}

HRESULT Hook_InjectProcess(DWORD pid, const WCHAR* pszDllPath) {
    if (pid == 0) {
        return E_INVALIDARG;
    }

    BOOL is64 = FALSE;
    HRESULT hr = Hook_IsProcess64Bit(pid, &is64);
    if (FAILED(hr)) {
        return hr;
    }
    if (!is64) {
        return HRESULT_FROM_WIN32(ERROR_EXE_MACHINE_TYPE_MISMATCH);
    }

    WCHAR rawDllPath[MAX_PATH];
    if (pszDllPath && pszDllPath[0] != L'\0') {
        size_t len = wcslen(pszDllPath);
        if (len >= MAX_PATH) {
            return E_INVALIDARG;
        }
        memcpy(rawDllPath, pszDllPath, (len + 1U) * sizeof(WCHAR));
    } else {
        hr = hook_resolve_default_dll_path(rawDllPath, MAX_PATH);
        if (FAILED(hr)) {
            return hr;
        }
    }

    WCHAR dllPath[MAX_PATH];
    DWORD fullLen = GetFullPathNameW(rawDllPath, MAX_PATH, dllPath, NULL);
    if (fullLen == 0U || fullLen >= MAX_PATH) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_FILENAME_EXCED_RANGE);
    }

    DWORD attrs = GetFileAttributesW(dllPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const WCHAR* dllLeaf = wcsrchr(dllPath, L'\\');
    if (!dllLeaf) {
        dllLeaf = wcsrchr(dllPath, L'/');
    }
    dllLeaf = dllLeaf ? (dllLeaf + 1) : dllPath;

    HMODULE existingMod = NULL;
    if (hook_find_remote_module(pid, dllLeaf, &existingMod, NULL, 0) && existingMod != NULL) {
        return S_FALSE; /* Already injected; avoid incrementing loader refcount */
    }

    /* Pre-create cross-integrity SHM with least-privilege User/System/Admin SA */
    WCHAR shmName[128];
    hook_build_shm_name(pid, shmName, 128);

    SECURITY_ATTRIBUTES sa;
    hr = IPC_CreateLeastPrivilegeSA(&sa);
    if (FAILED(hr)) {
        return hr; /* Fail closed */
    }

    SetLastError(0);
    HANDLE hShm = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &sa,
        PAGE_READWRITE,
        0,
        (DWORD)sizeof(CPowerTextHookShm),
        shmName
    );
    DWORD shmErr = GetLastError();
    IPC_FreeSecurityAttributes(&sa);
    if (!hShm) {
        return HRESULT_FROM_WIN32(shmErr != 0U ? shmErr : ERROR_ACCESS_DENIED);
    }
    if (shmErr == ERROR_ALREADY_EXISTS) {
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    CPowerTextHookShm* pShm = (CPowerTextHookShm*)MapViewOfFile(
        hShm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CPowerTextHookShm));
    if (!pShm) {
        DWORD err = GetLastError();
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_ACCESS_DENIED);
    }

    memset(pShm, 0, sizeof(CPowerTextHookShm));
    pShm->version = CPOWER_TEXT_HOOK_VERSION;
    pShm->targetPid = pid;
    pShm->capacity = CPOWER_TEXT_HOOK_RING_CAP;
    MemoryBarrier();
    pShm->magic = CPOWER_TEXT_HOOK_MAGIC;
    MemoryBarrier();
    InterlockedExchange(&pShm->active, 1);
    UnmapViewOfFile(pShm);

    /* Open target process for remote DLL injection */
    SetLastError(0);
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid
    );
    if (!hProc) {
        DWORD err = GetLastError();
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_ACCESS_DENIED);
    }

    size_t bytes = (wcslen(dllPath) + 1U) * sizeof(WCHAR);
    void* pRemoteBuf = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteBuf) {
        DWORD err = GetLastError();
        CloseHandle(hProc);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_OUTOFMEMORY);
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, pRemoteBuf, dllPath, bytes, &written) || written != bytes) {
        DWORD err = GetLastError();
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_WRITE_FAULT);
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pLoadLibRaw = hKernel32 ? GetProcAddress(hKernel32, "LoadLibraryW") : NULL;
    LPTHREAD_START_ROUTINE pStart = NULL;
    if (pLoadLibRaw) {
        memcpy(&pStart, &pLoadLibRaw, sizeof(pStart));
    }
    if (!pStart) {
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        CloseHandle(hShm);
        return E_FAIL;
    }

    SetLastError(0);
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, pStart, pRemoteBuf, 0, NULL);
    if (!hThread) {
        DWORD err = GetLastError();
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0U ? err : ERROR_ACCESS_DENIED);
    }

    DWORD waitRes = WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);

    if (waitRes != WAIT_OBJECT_0) {
        CloseHandle(hProc);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    /* Verify loaded module via 64-bit toolhelp snapshot instead of 32-bit GetExitCodeThread */
    HMODULE hRemoteMod = NULL;
    if (!hook_find_remote_module(pid, dllLeaf, &hRemoteMod, NULL, 0) || !hRemoteMod) {
        CloseHandle(hProc);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED);
    }

    /* Invoke exported CPowerHook_Install outside Loader Lock */
    DWORD installExit = 0U;
    hr = hook_invoke_remote_export(hProc, hRemoteMod, dllPath, "CPowerHook_Install", 5000, &installExit);
    if (FAILED(hr) || installExit != ERROR_SUCCESS) {
        hook_remote_freelibrary(hProc, hRemoteMod, 3000);
        CloseHandle(hProc);
        CloseHandle(hShm);
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(installExit);
    }

    CloseHandle(hProc);
    CloseHandle(hShm);
    return S_OK;
}

HRESULT Hook_ReadCapturedText(
    DWORD pid,
    CPowerTextHookEntry* pOut,
    uint32_t maxEntries,
    uint32_t* pOutCount,
    uint64_t* pInOutLastSeq
) {
    if (!pOut || !pOutCount) {
        return E_POINTER;
    }
    *pOutCount = 0;
    if (pid == 0 || maxEntries == 0) {
        return E_INVALIDARG;
    }

    WCHAR shmName[128];
    hook_build_shm_name(pid, shmName, 128);

    SetLastError(0);
    HANDLE hShm = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
    if (!hShm) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_FILE_NOT_FOUND);
    }

    const CPowerTextHookShm* pShm = (const CPowerTextHookShm*)MapViewOfFile(
        hShm, FILE_MAP_READ, 0, 0, sizeof(CPowerTextHookShm));
    if (!pShm) {
        DWORD err = GetLastError();
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    if (pShm->magic != CPOWER_TEXT_HOOK_MAGIC ||
        pShm->version != CPOWER_TEXT_HOOK_VERSION ||
        pShm->targetPid != pid ||
        pShm->capacity != CPOWER_TEXT_HOOK_RING_CAP) {
        UnmapViewOfFile(pShm);
        CloseHandle(hShm);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    MemoryBarrier();
    uint64_t writeSeq = (uint64_t)(uint32_t)pShm->writeSeq;
    uint64_t minSeq = (writeSeq > CPOWER_TEXT_HOOK_RING_CAP)
        ? (writeSeq - CPOWER_TEXT_HOOK_RING_CAP + 1U)
        : 1U;
    uint64_t lastSeq = pInOutLastSeq ? *pInOutLastSeq : 0U;
    uint64_t nextSeq = (lastSeq + 1U > minSeq) ? (lastSeq + 1U) : minSeq;

    uint32_t copied = 0;
    uint64_t highestRead = lastSeq;

    for (uint64_t targetSeq = nextSeq; targetSeq <= writeSeq && copied < maxEntries; ++targetSeq) {
        uint32_t idx = (uint32_t)((targetSeq - 1U) % CPOWER_TEXT_HOOK_RING_CAP);
        const CPowerTextHookEntry* pSlot = &pShm->entries[idx];

        uint64_t seq1 = (uint64_t)pSlot->seq;
        MemoryBarrier();
        if (seq1 != targetSeq) {
            uint64_t curWriteSeq = (uint64_t)(uint32_t)pShm->writeSeq;
            if ((curWriteSeq - targetSeq) >= CPOWER_TEXT_HOOK_RING_CAP) {
                highestRead = targetSeq;
                continue;
            }
            break; /* Writer still in-progress: do not advance cursor past targetSeq */
        }

        CPowerTextHookEntry snap;
        memcpy(&snap, (const void*)pSlot, sizeof(snap));
        MemoryBarrier();

        uint64_t seq2 = (uint64_t)pSlot->seq;
        if (seq2 != targetSeq || (uint64_t)snap.seq != targetSeq) {
            uint64_t curWriteSeq = (uint64_t)(uint32_t)pShm->writeSeq;
            if ((curWriteSeq - targetSeq) >= CPOWER_TEXT_HOOK_RING_CAP) {
                highestRead = targetSeq;
                continue;
            }
            break; /* Slot torn or overwritten mid-copy: retry on next poll */
        }

        if (snap.charCount > 127U) {
            snap.charCount = 127U;
        }
        snap.text[snap.charCount] = L'\0';
        snap.text[127] = L'\0';
        pOut[copied++] = snap;
        highestRead = targetSeq;
    }

    if (pInOutLastSeq) {
        *pInOutLastSeq = highestRead;
    }
    *pOutCount = copied;

    UnmapViewOfFile(pShm);
    CloseHandle(hShm);
    return S_OK;
}

HRESULT Hook_DetachProcess(DWORD pid, DWORD timeoutMs) {
    if (pid == 0) {
        return E_INVALIDARG;
    }
    if (timeoutMs == 0) {
        timeoutMs = 3000;
    }

    /* First signal active=0 through SHM if present */
    WCHAR shmName[128];
    hook_build_shm_name(pid, shmName, 128);
    HANDLE hShm = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (hShm) {
        CPowerTextHookShm* pShm = (CPowerTextHookShm*)MapViewOfFile(
            hShm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CPowerTextHookShm));
        if (pShm) {
            InterlockedExchange(&pShm->active, 0);
            UnmapViewOfFile(pShm);
        }
        CloseHandle(hShm);
    }

    HMODULE hRemoteMod = NULL;
    WCHAR remoteDllPath[MAX_PATH];
    if (!hook_find_remote_module(pid, L"cpower_hook64.dll", &hRemoteMod, remoteDllPath, MAX_PATH)) {
        return S_FALSE;
    }

    SetLastError(0);
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid
    );
    if (!hProc) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    /* Call CPowerHook_PrepareDetach remotely before FreeLibrary.
       If PrepareDetach fails or times out waiting for g_inFlight == 0,
       fail closed with ERROR_BUSY and DO NOT call FreeLibrary! */
    DWORD prepExit = ERROR_BUSY;
    HRESULT hrPrep = hook_invoke_remote_export(
        hProc,
        hRemoteMod,
        remoteDllPath,
        "CPowerHook_PrepareDetach",
        timeoutMs,
        &prepExit
    );
    if (FAILED(hrPrep) || prepExit != ERROR_SUCCESS) {
        CloseHandle(hProc);
        if (hrPrep == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || prepExit == ERROR_BUSY) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        return FAILED(hrPrep) ? hrPrep : HRESULT_FROM_WIN32(prepExit);
    }

    /* Unload DLL cleanly via FreeLibrary after quiescent drain + grace period */
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pFreeLibRaw = hKernel32 ? GetProcAddress(hKernel32, "FreeLibrary") : NULL;
    LPTHREAD_START_ROUTINE pFreeStart = NULL;
    if (pFreeLibRaw) {
        memcpy(&pFreeStart, &pFreeLibRaw, sizeof(pFreeStart));
    }
    if (!pFreeStart) {
        CloseHandle(hProc);
        return E_FAIL;
    }

    SetLastError(0);
    HANDLE hFreeThread = CreateRemoteThread(hProc, NULL, 0, pFreeStart, hRemoteMod, 0, NULL);
    if (!hFreeThread) {
        DWORD err = GetLastError();
        CloseHandle(hProc);
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_ACCESS_DENIED);
    }

    DWORD waitRes = WaitForSingleObject(hFreeThread, timeoutMs);
    CloseHandle(hFreeThread);
    CloseHandle(hProc);

    return (waitRes == WAIT_OBJECT_0) ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
