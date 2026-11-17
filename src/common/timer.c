/**
 * @file timer.c
 * @brief High-resolution QPC timers, precision waitable-timer sleep, and thread-safe RNG.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "common.h"
#include <math.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002UL
#endif

#define CPOWER_TWO_PI 6.283185307179586476925286766559

typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(
    LPSECURITY_ATTRIBUTES lpTimerAttributes,
    LPCWSTR lpTimerName,
    DWORD dwFlags,
    DWORD dwDesiredAccess
);

static INIT_ONCE                 g_timerApiOnce = INIT_ONCE_STATIC_INIT;
static PFN_CreateWaitableTimerExW g_pfnCreateWaitableTimerExW = NULL;

static INIT_ONCE                 g_rngSeedOnce = INIT_ONCE_STATIC_INIT;
static volatile LONG64           g_rngState = 0;

static BOOL CALLBACK init_waitable_timer_api(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        FARPROC proc = GetProcAddress(hKernel32, "CreateWaitableTimerExW");
        if (proc) {
            memcpy(&g_pfnCreateWaitableTimerExW, &proc, sizeof(g_pfnCreateWaitableTimerExW));
        }
    }
    return TRUE;
}

static uint64_t splitmix64_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static BOOL CALLBACK init_rng_seed(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ft64 = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;

    uint64_t seed = (uint64_t)qpc.QuadPart
                  ^ ft64
                  ^ ((uint64_t)GetCurrentProcessId() << 32)
                  ^ (uint64_t)GetCurrentThreadId()
                  ^ (uint64_t)GetTickCount64()
                  ^ (uint64_t)(uintptr_t)&qpc;

    uint64_t mixed = splitmix64_mix(seed);
    if (mixed == 0) {
        mixed = 0x9e3779b97f4a7c15ULL;
    }
    InterlockedExchange64(&g_rngState, (LONG64)mixed);
    return TRUE;
}

static uint64_t rng_next_u64(void) {
    InitOnceExecuteOnce(&g_rngSeedOnce, init_rng_seed, NULL, NULL);

    uint64_t tidMix = ((uint64_t)GetCurrentThreadId()) * 0x9e3779b97f4a7c15ULL;
    for (;;) {
        LONG64 prevSigned = InterlockedCompareExchange64(&g_rngState, 0, 0);
        uint64_t prev = (uint64_t)prevSigned;
        uint64_t next = prev + 0x9e3779b97f4a7c15ULL;
        if (InterlockedCompareExchange64(&g_rngState, (LONG64)next, prevSigned) == prevSigned) {
            return splitmix64_mix(next ^ tidMix);
        }
    }
}

void Timer_Init(QpcTimer* timer) {
    if (!timer) {
        return;
    }
    QueryPerformanceFrequency(&timer->frequency);
    timer->start.QuadPart = 0;
    timer->stop.QuadPart = 0;
}

void Timer_Start(QpcTimer* timer) {
    if (!timer) {
        return;
    }
    if (timer->frequency.QuadPart <= 0) {
        QueryPerformanceFrequency(&timer->frequency);
    }
    QueryPerformanceCounter(&timer->start);
    timer->stop.QuadPart = 0;
}

void Timer_Stop(QpcTimer* timer) {
    if (!timer) {
        return;
    }
    QueryPerformanceCounter(&timer->stop);
}

double Timer_ElapsedMs(const QpcTimer* timer) {
    if (!timer || timer->frequency.QuadPart <= 0 || timer->start.QuadPart == 0) {
        return 0.0;
    }

    LONGLONG endTick = timer->stop.QuadPart;
    if (endTick == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        endTick = now.QuadPart;
    }

    LONGLONG delta = endTick - timer->start.QuadPart;
    if (delta <= 0) {
        return 0.0;
    }
    return ((double)delta * 1000.0) / (double)timer->frequency.QuadPart;
}

double Timer_ElapsedMicroseconds(const QpcTimer* timer) {
    if (!timer || timer->frequency.QuadPart <= 0 || timer->start.QuadPart == 0) {
        return 0.0;
    }

    LONGLONG endTick = timer->stop.QuadPart;
    if (endTick == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        endTick = now.QuadPart;
    }

    LONGLONG delta = endTick - timer->start.QuadPart;
    if (delta <= 0) {
        return 0.0;
    }
    return ((double)delta * 1000000.0) / (double)timer->frequency.QuadPart;
}

void Timer_AccurateSleep(double ms) {
    if (!(ms > 0.0)) {
        return;
    }

    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    if (freq.QuadPart <= 0) {
        DWORD wholeMs = (DWORD)ms;
        Sleep(wholeMs > 0 ? wholeMs : 1);
        return;
    }

    double targetDeltaTicks = (ms * (double)freq.QuadPart) / 1000.0;
    LONGLONG targetTick = start.QuadPart + (LONGLONG)targetDeltaTicks;
    if (targetTick <= start.QuadPart) {
        targetTick = start.QuadPart + 1;
    }

    InitOnceExecuteOnce(&g_timerApiOnce, init_waitable_timer_api, NULL, NULL);

    if (ms > 1.5 && g_pfnCreateWaitableTimerExW != NULL) {
        HANDLE hTimer = g_pfnCreateWaitableTimerExW(
            NULL,
            NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );
        if (hTimer != NULL) {
            double waitMs = ms - 0.8;
            if (waitMs > 0.5) {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -(LONGLONG)(waitMs * 10000.0);
                if (dueTime.QuadPart < 0 &&
                    SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(hTimer, INFINITE);
                }
            }
            CloseHandle(hTimer);
        } else if (ms > 18.0) {
            /* Fallback for pre-Win10 1803 where CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is unsupported */
            Sleep((DWORD)(ms - 16.0));
        }
    } else if (ms > 18.0) {
        Sleep((DWORD)(ms - 16.0));
    }

    /* High-precision QPC spin tail */
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= targetTick) {
            break;
        }
        double remMs = ((double)(targetTick - now.QuadPart) * 1000.0) / (double)freq.QuadPart;
        if (remMs > 1.5) {
            Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

double Math_RandomUniform(double minVal, double maxVal) {
    if (minVal > maxVal) {
        double tmp = minVal;
        minVal = maxVal;
        maxVal = tmp;
    }
    if (minVal == maxVal) {
        return minVal;
    }

    /* 53-bit uniform in [0.0, 1.0] */
    uint64_t r = rng_next_u64() >> 11;
    double u = (double)r / 9007199254740991.0; /* (2^53 - 1) */
    return minVal + u * (maxVal - minVal);
}

double Math_BoxMullerNormal(void) {
    /* Strictly positive u1 in (0, 1) prevents log(0) */
    double u1 = ((double)(rng_next_u64() >> 11) + 0.5) / 9007199254740992.0;
    double u2 = ((double)(rng_next_u64() >> 11)) / 9007199254740992.0;

    return sqrt(-2.0 * log(u1)) * cos(CPOWER_TWO_PI * u2);
}

double Math_SampleLogNormal(double medianMs, double sigma) {
    if (medianMs <= 0.0) {
        return 0.0;
    }
    if (sigma <= 0.0) {
        return medianMs;
    }

    double z = Math_BoxMullerNormal();
    return medianMs * exp(sigma * z);
}
