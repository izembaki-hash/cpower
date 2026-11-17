/**
 * @file common.h
 * @brief Common definitions, memory management macros, high-resolution QPC timers,
 *        and mathematical helper routines for cpower.
 */

#ifndef CPOWER_COMMON_H
#define CPOWER_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "cpower.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Memory and Resource Safety Macros
 * ------------------------------------------------------------------------- */

#define CPOWER_SAFE_RELEASE(p) \
    do { \
        if ((p) != NULL) { \
            (p)->lpVtbl->Release((p)); \
            (p) = NULL; \
        } \
    } while (0)

#define CPOWER_SAFE_FREE(p) \
    do { \
        if ((p) != NULL) { \
            free(p); \
            (p) = NULL; \
        } \
    } while (0)

#define CPOWER_SAFE_CLOSE_HANDLE(h) \
    do { \
        if ((h) != NULL && (h) != INVALID_HANDLE_VALUE) { \
            CloseHandle(h); \
            (h) = NULL; \
        } \
    } while (0)

#define CPOWER_SAFE_SYSFREESTRING(bstr) \
    do { \
        if ((bstr) != NULL) { \
            SysFreeString(bstr); \
            (bstr) = NULL; \
        } \
    } while (0)

/* -------------------------------------------------------------------------
 * Memory Alignment and Hardware Cache Line Directives
 * ------------------------------------------------------------------------- */
#define CPOWER_CACHE_LINE 64

#if defined(_MSC_VER)
#define CPOWER_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
#define CPOWER_ALIGN(n) __attribute__((aligned(n)))
#else
#define CPOWER_ALIGN(n)
#endif

/* -------------------------------------------------------------------------
 * High-Resolution QPC Timer
 * ------------------------------------------------------------------------- */
typedef struct QpcTimer {
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER stop;
} QpcTimer;

/**
 * @brief Initializes a QpcTimer instance with the system performance frequency.
 * @param timer Pointer to QpcTimer.
 */
void Timer_Init(QpcTimer* timer);

/**
 * @brief Records the starting tick count for a QpcTimer.
 * @param timer Pointer to QpcTimer.
 */
void Timer_Start(QpcTimer* timer);

/**
 * @brief Records the stopping tick count for a QpcTimer.
 * @param timer Pointer to QpcTimer.
 */
void Timer_Stop(QpcTimer* timer);

/**
 * @brief Computes elapsed time in milliseconds between start and stop/current.
 * @param timer Pointer to QpcTimer.
 * @return Elapsed time in milliseconds.
 */
double Timer_ElapsedMs(const QpcTimer* timer);

/**
 * @brief Computes elapsed time in microseconds between start and stop/current.
 * @param timer Pointer to QpcTimer.
 * @return Elapsed time in microseconds.
 */
double Timer_ElapsedMicroseconds(const QpcTimer* timer);

/**
 * @brief High-precision sleep combining timeBeginPeriod, OS yield, and QPC busy-wait.
 * @param ms Desired sleep duration in milliseconds.
 */
void Timer_AccurateSleep(double ms);

/* -------------------------------------------------------------------------
 * Mathematical & Stochastic Generation Routines
 * ------------------------------------------------------------------------- */

/**
 * @brief Generates a pseudo-random floating point number in [minVal, maxVal].
 */
double Math_RandomUniform(double minVal, double maxVal);

/**
 * @brief Generates a standard normal random variate N(0, 1) using Box-Muller transform.
 */
double Math_BoxMullerNormal(void);

/**
 * @brief Samples a log-normal distribution with given median and shape parameter sigma.
 * @param medianMs Median value in milliseconds.
 * @param sigma Shape parameter (dispersion).
 * @return Sampled duration in milliseconds.
 */
double Math_SampleLogNormal(double medianMs, double sigma);

/* -------------------------------------------------------------------------
 * Diagnostic Structured Logging
 * ------------------------------------------------------------------------- */
typedef enum CPowerLogLevel {
    CPOWER_LOG_DEBUG = 0,
    CPOWER_LOG_INFO  = 1,
    CPOWER_LOG_WARN  = 2,
    CPOWER_LOG_ERROR = 3,
    CPOWER_LOG_NONE  = 4
} CPowerLogLevel;

/**
 * @brief Logs a formatted message to standard error if level meets the threshold.
 * @param level Severity level.
 * @param fmt Printf-style format string.
 */
void CPower_Log(CPowerLogLevel level, const char* fmt, ...);

/**
 * @brief Configures global logging verbosity.
 * @param level Minimum severity level to emit.
 */
void CPower_SetLogLevel(CPowerLogLevel level);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_COMMON_H */
