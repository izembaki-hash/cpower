/**
 * @file logger.c
 * @brief Thread-safe structured diagnostic logging to stderr.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "common.h"
#include <stdio.h>
#include <stdarg.h>

static INIT_ONCE        g_logCsOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_logCs;
static volatile LONG    g_logMinLevel = (LONG)CPOWER_LOG_INFO;

static BOOL CALLBACK init_logger_cs(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    InitializeCriticalSection(&g_logCs);
    return TRUE;
}

static const char* log_level_label(CPowerLogLevel level) {
    switch (level) {
        case CPOWER_LOG_DEBUG: return "DEBUG";
        case CPOWER_LOG_INFO:  return "INFO";
        case CPOWER_LOG_WARN:  return "WARN";
        case CPOWER_LOG_ERROR: return "ERROR";
        default:               return "LOG";
    }
}

void CPower_SetLogLevel(CPowerLogLevel level) {
    if (level < CPOWER_LOG_DEBUG) {
        level = CPOWER_LOG_DEBUG;
    } else if (level > CPOWER_LOG_NONE) {
        level = CPOWER_LOG_NONE;
    }
    InterlockedExchange(&g_logMinLevel, (LONG)level);
}

void CPower_Log(CPowerLogLevel level, const char* fmt, ...) {
    if (!fmt || level >= CPOWER_LOG_NONE) {
        return;
    }

    LONG minLevel = InterlockedCompareExchange(&g_logMinLevel, 0, 0);
    if ((LONG)level < minLevel) {
        return;
    }

    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    if (written < 0) {
        msgBuf[0] = '\0';
    } else {
        msgBuf[sizeof(msgBuf) - 1] = '\0';
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();
    const char* tag = log_level_label(level);

    InitOnceExecuteOnce(&g_logCsOnce, init_logger_cs, NULL, NULL);
    EnterCriticalSection(&g_logCs);
    fprintf(
        stderr,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%-5s] [tid:%05lu] %s\n",
        (unsigned)st.wYear,
        (unsigned)st.wMonth,
        (unsigned)st.wDay,
        (unsigned)st.wHour,
        (unsigned)st.wMinute,
        (unsigned)st.wSecond,
        (unsigned)st.wMilliseconds,
        tag,
        (unsigned long)tid,
        msgBuf
    );
    fflush(stderr);
    LeaveCriticalSection(&g_logCs);
}
