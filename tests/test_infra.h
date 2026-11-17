/**
 * @file test_infra.h
 * @brief Pure C Zero-Dependency Windows-Native Test Framework
 *
 * Designed for the Windows-Native Computer-Use Agent project.
 * Implements a lightweight, robust, 4-tier E2E and unit testing harness:
 *   - Tier 1: Feature Coverage
 *   - Tier 2: Boundary & Corner Cases
 *   - Tier 3: Cross-Feature Combinations
 *   - Tier 4: Real-World Application Scenarios
 *
 * Features:
 *   - Zero external third-party dependencies (pure C99/C11 + Win32 API).
 *   - Non-fatal test isolation using setjmp/longjmp (failed assertions abort
 *     the current test without crashing the test runner process).
 *   - High-resolution timing via QueryPerformanceCounter (< 1 microsecond).
 *   - Colorized console output (ANSI escape codes on Win10/11 with fallback).
 *   - Rich assertion library (Integers, Pointers, Strings, Wide Strings,
 *     Floating Point approximations, and COM HRESULTs).
 *   - Flexible filtering by Tier, Feature ID, Suite name, and Test name.
 *   - Structured JSON test report generation.
 */

#ifndef CPOWER_TEST_INFRA_H
#define CPOWER_TEST_INFRA_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Test Framework Enums & Constants                                          */
/* ========================================================================= */

#define TEST_INFRA_VERSION "1.0.0"
#define MAX_TESTS_PER_SUITE 256
#define MAX_SUITES 64
#define MAX_NAME_LEN 128
#define MAX_DESC_LEN 256
#define MAX_MSG_LEN 512

typedef enum {
    TIER_ALL = 0,
    TIER_1_FEATURE_COVERAGE = 1,
    TIER_2_BOUNDARY_CORNER  = 2,
    TIER_3_CROSS_FEATURE    = 3,
    TIER_4_REAL_WORLD       = 4,
    TIER_5_ADVERSARIAL      = 5
} TestTier;

typedef enum {
    TEST_STATUS_PENDING = 0,
    TEST_STATUS_PASS,
    TEST_STATUS_FAIL,
    TEST_STATUS_SKIP,
    TEST_STATUS_TIMEOUT
} TestStatus;

/* ========================================================================= */
/* Data Structures                                                           */
/* ========================================================================= */

typedef void (*TestFunction)(void);
typedef void (*FixtureFunction)(void);

typedef struct {
    char name[MAX_NAME_LEN];
    char description[MAX_DESC_LEN];
    int feature_id;                 /* Feature 1 to 16 from PROJECT.md */
    TestTier tier;                  /* Tier 1 to 5 */
    TestFunction fn;                /* Test entry point */
    TestStatus status;              /* PASS, FAIL, SKIP */
    double elapsed_ms;              /* Elapsed execution time in milliseconds */
    int assertion_count;            /* Number of assertions evaluated */
    char failure_msg[MAX_MSG_LEN];  /* Diagnostic error message */
    char failure_file[MAX_NAME_LEN];/* Source file where assertion failed */
    int failure_line;               /* Source line number */
} TestCase;

typedef struct {
    char name[MAX_NAME_LEN];
    char description[MAX_DESC_LEN];
    FixtureFunction setup;          /* Runs before each test case in suite */
    FixtureFunction teardown;       /* Runs after each test case in suite */
    TestCase tests[MAX_TESTS_PER_SUITE];
    int test_count;
    int passed_count;
    int failed_count;
    int skipped_count;
    double total_elapsed_ms;
} TestSuite;

typedef struct {
    int filter_tier;                /* 0 for all, 1-5 for specific tier */
    int filter_feature;             /* 0 for all, 1-16 for specific feature */
    char filter_suite[MAX_NAME_LEN];/* Substring match for suite name */
    char filter_name[MAX_NAME_LEN]; /* Substring match for test name */
    bool verbose;                   /* Print individual assertion successes */
    bool color_output;              /* Enable ANSI color codes */
    char json_output_path[MAX_PATH];/* Optional JSON output path */
} TestRunnerConfig;

typedef struct {
    TestSuite* suites[MAX_SUITES];
    int suite_count;
    TestSuite* current_suite;
    TestCase* current_test;
    jmp_buf jump_env;
    bool in_test;
    LARGE_INTEGER qpc_freq;
    int total_assertions;
    TestRunnerConfig config;
} TestContext;

/* Global Test Context Singleton (defined once in test_runner.c) */
extern TestContext g_test_ctx;

/* ========================================================================= */
/* Console Colors & Output Helpers                                           */
/* ========================================================================= */

#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_GRAY    "\033[90m"

static inline void test_infra_enable_ansi(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
    g_test_ctx.config.color_output = true;
}

static inline const char* test_infra_tier_str(TestTier tier) {
    switch (tier) {
        case TIER_1_FEATURE_COVERAGE: return "Tier 1: Feature Coverage";
        case TIER_2_BOUNDARY_CORNER:  return "Tier 2: Boundary & Corner";
        case TIER_3_CROSS_FEATURE:    return "Tier 3: Cross-Feature";
        case TIER_4_REAL_WORLD:       return "Tier 4: Real-World Scenario";
        case TIER_5_ADVERSARIAL:      return "Tier 5: Adversarial Hardening";
        default:                      return "Tier: General";
    }
}

static inline const char* test_infra_tier_short(TestTier tier) {
    switch (tier) {
        case TIER_1_FEATURE_COVERAGE: return "T1";
        case TIER_2_BOUNDARY_CORNER:  return "T2";
        case TIER_3_CROSS_FEATURE:    return "T3";
        case TIER_4_REAL_WORLD:       return "T4";
        case TIER_5_ADVERSARIAL:      return "T5";
        default:                      return "T?";
    }
}

/* ========================================================================= */
/* High-Precision Timing (QPC)                                               */
/* ========================================================================= */

static inline void test_infra_timer_init(void) {
    QueryPerformanceFrequency(&g_test_ctx.qpc_freq);
}

static inline int64_t test_infra_timer_now(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static inline double test_infra_timer_ms(int64_t start, int64_t end) {
    if (g_test_ctx.qpc_freq.QuadPart == 0) return 0.0;
    return ((double)(end - start) * 1000.0) / (double)g_test_ctx.qpc_freq.QuadPart;
}

/* ========================================================================= */
/* Framework Lifecycle & Suite Registration                                  */
/* ========================================================================= */

static inline void test_infra_init(void) {
    memset(&g_test_ctx, 0, sizeof(TestContext));
    test_infra_enable_ansi();
    test_infra_timer_init();
    g_test_ctx.config.color_output = true;
    g_test_ctx.config.verbose = false;
}

static inline TestSuite* test_suite_create(const char* name, const char* description,
                                           FixtureFunction setup, FixtureFunction teardown) {
    if (g_test_ctx.suite_count >= MAX_SUITES) {
        fprintf(stderr, "Error: Maximum test suites (%d) exceeded\n", MAX_SUITES);
        return NULL;
    }
    TestSuite* suite = (TestSuite*)calloc(1, sizeof(TestSuite));
    if (!suite) return NULL;

    strncpy(suite->name, name, sizeof(suite->name) - 1);
    if (description) {
        strncpy(suite->description, description, sizeof(suite->description) - 1);
    }
    suite->setup = setup;
    suite->teardown = teardown;

    g_test_ctx.suites[g_test_ctx.suite_count++] = suite;
    return suite;
}

static inline void test_case_register(TestSuite* suite, const char* name, const char* description,
                                      int feature_id, TestTier tier, TestFunction fn) {
    if (!suite) return;
    if (suite->test_count >= MAX_TESTS_PER_SUITE) {
        fprintf(stderr, "Error: Maximum tests per suite (%d) exceeded in '%s'\n",
                MAX_TESTS_PER_SUITE, suite->name);
        return;
    }
    TestCase* tc = &suite->tests[suite->test_count++];
    strncpy(tc->name, name, sizeof(tc->name) - 1);
    if (description) {
        strncpy(tc->description, description, sizeof(tc->description) - 1);
    }
    tc->feature_id = feature_id;
    tc->tier = tier;
    tc->fn = fn;
    tc->status = TEST_STATUS_PENDING;
}

/* ========================================================================= */
/* Assertion Engine                                                          */
/* ========================================================================= */

static inline void test_infra_record_failure(const char* file, int line, const char* msg) {
    if (g_test_ctx.current_test) {
        g_test_ctx.current_test->status = TEST_STATUS_FAIL;
        strncpy(g_test_ctx.current_test->failure_file, file, sizeof(g_test_ctx.current_test->failure_file) - 1);
        g_test_ctx.current_test->failure_line = line;
        strncpy(g_test_ctx.current_test->failure_msg, msg, sizeof(g_test_ctx.current_test->failure_msg) - 1);
    }
    if (g_test_ctx.in_test) {
        longjmp(g_test_ctx.jump_env, 1);
    }
}

static inline void test_infra_record_skip(const char* reason) {
    if (g_test_ctx.current_test) {
        g_test_ctx.current_test->status = TEST_STATUS_SKIP;
        strncpy(g_test_ctx.current_test->failure_msg, reason ? reason : "Skipped",
                sizeof(g_test_ctx.current_test->failure_msg) - 1);
    }
    if (g_test_ctx.in_test) {
        longjmp(g_test_ctx.jump_env, 2);
    }
}

static inline void test_infra_assert_pass(void) {
    g_test_ctx.total_assertions++;
    if (g_test_ctx.current_test) {
        g_test_ctx.current_test->assertion_count++;
    }
}

#define ASSERT_TRUE(cond) do { \
    test_infra_assert_pass(); \
    if (!(cond)) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_TRUE(" #cond ") evaluated to false"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    test_infra_assert_pass(); \
    if (cond) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_FALSE(" #cond ") evaluated to true"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, act) do { \
    test_infra_assert_pass(); \
    int64_t _e = (int64_t)(exp); \
    int64_t _a = (int64_t)(act); \
    if (_e != _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_INT(" #act "): expected %lld, got %lld", (long long)_e, (long long)_a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_NE_INT(exp, act) do { \
    test_infra_assert_pass(); \
    int64_t _e = (int64_t)(exp); \
    int64_t _a = (int64_t)(act); \
    if (_e == _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_NE_INT(" #act "): unexpected equal value %lld", (long long)_a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_UINT(exp, act) do { \
    test_infra_assert_pass(); \
    uint64_t _e = (uint64_t)(exp); \
    uint64_t _a = (uint64_t)(act); \
    if (_e != _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_UINT(" #act "): expected %llu, got %llu", (unsigned long long)_e, (unsigned long long)_a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_PTR(exp, act) do { \
    test_infra_assert_pass(); \
    const void* _e = (const void*)(exp); \
    const void* _a = (const void*)(act); \
    if (_e != _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_PTR(" #act "): expected %p, got %p", _e, _a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_NE_PTR(exp, act) do { \
    test_infra_assert_pass(); \
    const void* _e = (const void*)(exp); \
    const void* _a = (const void*)(act); \
    if (_e == _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_NE_PTR(" #act "): unexpected equal pointer %p", _a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_NULL(ptr) do { \
    test_infra_assert_pass(); \
    const void* _p = (const void*)(ptr); \
    if (_p != NULL) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_NULL(" #ptr "): expected NULL, got %p", _p); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
    test_infra_assert_pass(); \
    const void* _p = (const void*)(ptr); \
    if (_p == NULL) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_NOT_NULL(" #ptr "): expected non-NULL, got NULL"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_STR(exp, act) do { \
    test_infra_assert_pass(); \
    const char* _e = (const char*)(exp); \
    const char* _a = (const char*)(act); \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_STR(" #act "): expected \"%s\", got \"%s\"", \
                 _e ? _e : "<NULL>", _a ? _a : "<NULL>"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    test_infra_assert_pass(); \
    const char* _h = (const char*)(haystack); \
    const char* _n = (const char*)(needle); \
    if (_h == NULL || _n == NULL || strstr(_h, _n) == NULL) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_STR_CONTAINS: \"%s\" not found in \"%s\"", \
                 _n ? _n : "<NULL>", _h ? _h : "<NULL>"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_WSTR(exp, act) do { \
    test_infra_assert_pass(); \
    const wchar_t* _e = (const wchar_t*)(exp); \
    const wchar_t* _a = (const wchar_t*)(act); \
    if (_e == NULL || _a == NULL || wcscmp(_e, _a) != 0) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_WSTR(" #act "): wide string mismatch"); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_HRESULT_OK(hr) do { \
    test_infra_assert_pass(); \
    HRESULT _h = (HRESULT)(hr); \
    if (FAILED(_h)) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_HRESULT_OK(" #hr "): failed with hr = 0x%08lX", (unsigned long)_h); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_HRESULT_FAIL(hr) do { \
    test_infra_assert_pass(); \
    HRESULT _h = (HRESULT)(hr); \
    if (SUCCEEDED(_h)) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_HRESULT_FAIL(" #hr "): expected failure, got SUCCEEDED (0x%08lX)", (unsigned long)_h); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_EQ_HRESULT(exp, act) do { \
    test_infra_assert_pass(); \
    HRESULT _e = (HRESULT)(exp); \
    HRESULT _a = (HRESULT)(act); \
    if (_e != _a) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_EQ_HRESULT(" #act "): expected 0x%08lX, got 0x%08lX", \
                 (unsigned long)_e, (unsigned long)_a); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_DOUBLE_NEAR(exp, act, tol) do { \
    test_infra_assert_pass(); \
    double _e = (double)(exp); \
    double _a = (double)(act); \
    double _t = (double)(tol); \
    if (fabs(_e - _a) > _t) { \
        char _msg[MAX_MSG_LEN]; \
        snprintf(_msg, sizeof(_msg), "ASSERT_DOUBLE_NEAR(" #act "): expected %f, got %f (delta %f > tol %f)", \
                 _e, _a, fabs(_e - _a), _t); \
        test_infra_record_failure(__FILE__, __LINE__, _msg); \
    } \
} while (0)

#define ASSERT_FAIL(reason) do { \
    test_infra_assert_pass(); \
    test_infra_record_failure(__FILE__, __LINE__, reason); \
} while (0)

#define TEST_SKIP(reason) do { \
    test_infra_record_skip(reason); \
} while (0)

/* ========================================================================= */
/* Test Runner Execution Engine                                              */
/* ========================================================================= */

static inline bool test_infra_matches_filter(const TestSuite* suite, const TestCase* tc,
                                             const TestRunnerConfig* cfg) {
    if (!cfg) return true;
    if (cfg->filter_tier > 0 && tc->tier != (TestTier)cfg->filter_tier) {
        return false;
    }
    if (cfg->filter_feature > 0 && tc->feature_id != cfg->filter_feature) {
        return false;
    }
    if (cfg->filter_suite[0] != '\0' && strstr(suite->name, cfg->filter_suite) == NULL) {
        return false;
    }
    if (cfg->filter_name[0] != '\0' && strstr(tc->name, cfg->filter_name) == NULL) {
        return false;
    }
    return true;
}

static inline void test_infra_run_test(TestSuite* suite, TestCase* tc) {
    g_test_ctx.current_suite = suite;
    g_test_ctx.current_test = tc;
    tc->assertion_count = 0;
    tc->status = TEST_STATUS_PENDING;

    /* Setup fixture */
    if (suite->setup) {
        suite->setup();
    }

    int64_t start = test_infra_timer_now();
    g_test_ctx.in_test = true;

    int jump_code = setjmp(g_test_ctx.jump_env);
    if (jump_code == 0) {
        /* Execute actual test case */
        tc->fn();
        tc->status = TEST_STATUS_PASS;
    } else if (jump_code == 1) {
        /* Assertion failed */
        tc->status = TEST_STATUS_FAIL;
    } else if (jump_code == 2) {
        /* Test skipped */
        tc->status = TEST_STATUS_SKIP;
    }

    g_test_ctx.in_test = false;
    int64_t end = test_infra_timer_now();
    tc->elapsed_ms = test_infra_timer_ms(start, end);

    /* Teardown fixture */
    if (suite->teardown) {
        suite->teardown();
    }

    if (tc->status == TEST_STATUS_PASS) {
        suite->passed_count++;
        printf("  %s[ PASS ]%s [%s][F%02d] %-48s %s(%6.2f ms)%s\n",
               COLOR_GREEN, COLOR_RESET,
               test_infra_tier_short(tc->tier), tc->feature_id,
               tc->name, COLOR_GRAY, tc->elapsed_ms, COLOR_RESET);
    } else if (tc->status == TEST_STATUS_SKIP) {
        suite->skipped_count++;
        printf("  %s[ SKIP ]%s [%s][F%02d] %-48s %s(Reason: %s)%s\n",
               COLOR_YELLOW, COLOR_RESET,
               test_infra_tier_short(tc->tier), tc->feature_id,
               tc->name, COLOR_GRAY, tc->failure_msg, COLOR_RESET);
    } else {
        suite->failed_count++;
        printf("  %s[ FAIL ]%s [%s][F%02d] %-48s %s(%6.2f ms)%s\n",
               COLOR_RED, COLOR_RESET,
               test_infra_tier_short(tc->tier), tc->feature_id,
               tc->name, COLOR_GRAY, tc->elapsed_ms, COLOR_RESET);
        printf("         %s--> %s:%d%s\n", COLOR_YELLOW, tc->failure_file, tc->failure_line, COLOR_RESET);
        printf("         %s--> %s%s\n", COLOR_RED, tc->failure_msg, COLOR_RESET);
    }
    fflush(stdout);
}

static inline void test_infra_export_json(const char* path, int total_tests, int total_passed,
                                         int total_failed, int total_skipped, double total_time_ms) {
    if (!path || path[0] == '\0') return;
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Warning: Could not open JSON output file '%s'\n", path);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": \"%s\",\n", TEST_INFRA_VERSION);
    fprintf(f, "  \"timestamp\": \"2026-11-12T05:00:00Z\",\n");
    fprintf(f, "  \"summary\": {\n");
    fprintf(f, "    \"total_suites\": %d,\n", g_test_ctx.suite_count);
    fprintf(f, "    \"total_tests\": %d,\n", total_tests);
    fprintf(f, "    \"passed\": %d,\n", total_passed);
    fprintf(f, "    \"failed\": %d,\n", total_failed);
    fprintf(f, "    \"skipped\": %d,\n", total_skipped);
    fprintf(f, "    \"total_time_ms\": %.2f,\n", total_time_ms);
    fprintf(f, "    \"success_rate_pct\": %.1f\n", total_tests > 0 ? (total_passed * 100.0 / total_tests) : 0.0);
    fprintf(f, "  },\n");
    fprintf(f, "  \"suites\": [\n");

    for (int s = 0; s < g_test_ctx.suite_count; s++) {
        TestSuite* suite = g_test_ctx.suites[s];
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", suite->name);
        fprintf(f, "      \"description\": \"%s\",\n", suite->description);
        fprintf(f, "      \"passed\": %d,\n", suite->passed_count);
        fprintf(f, "      \"failed\": %d,\n", suite->failed_count);
        fprintf(f, "      \"skipped\": %d,\n", suite->skipped_count);
        fprintf(f, "      \"elapsed_ms\": %.2f,\n", suite->total_elapsed_ms);
        fprintf(f, "      \"tests\": [\n");

        for (int t = 0; t < suite->test_count; t++) {
            TestCase* tc = &suite->tests[t];
            fprintf(f, "        {\n");
            fprintf(f, "          \"name\": \"%s\",\n", tc->name);
            fprintf(f, "          \"description\": \"%s\",\n", tc->description);
            fprintf(f, "          \"tier\": %d,\n", (int)tc->tier);
            fprintf(f, "          \"tier_name\": \"%s\",\n", test_infra_tier_str(tc->tier));
            fprintf(f, "          \"feature_id\": %d,\n", tc->feature_id);
            fprintf(f, "          \"status\": \"%s\",\n",
                    tc->status == TEST_STATUS_PASS ? "PASS" :
                    (tc->status == TEST_STATUS_FAIL ? "FAIL" :
                     (tc->status == TEST_STATUS_SKIP ? "SKIP" : "PENDING")));
            fprintf(f, "          \"elapsed_ms\": %.2f,\n", tc->elapsed_ms);
            fprintf(f, "          \"assertions\": %d%s\n", tc->assertion_count,
                    tc->status == TEST_STATUS_FAIL ? "," : "");
            if (tc->status == TEST_STATUS_FAIL) {
                fprintf(f, "          \"failure\": {\n");
                fprintf(f, "            \"file\": \"%s\",\n", tc->failure_file);
                fprintf(f, "            \"line\": %d,\n", tc->failure_line);
                fprintf(f, "            \"message\": \"%s\"\n", tc->failure_msg);
                fprintf(f, "          }\n");
            }
            fprintf(f, "        }%s\n", t + 1 < suite->test_count ? "," : "");
        }
        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", s + 1 < g_test_ctx.suite_count ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

static inline int test_infra_run_all(const TestRunnerConfig* cfg) {
    if (cfg) {
        g_test_ctx.config = *cfg;
    }

    printf("\n");
    printf("%s================================================================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s   Windows-Native Pure C Computer-Use Agent - E2E & Unit Test Harness%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s   Zero-Dependency Pure C Framework (Version %s) | 4-Tier Test Architecture%s\n", COLOR_GRAY, TEST_INFRA_VERSION, COLOR_RESET);
    printf("%s================================================================================%s\n\n", COLOR_CYAN, COLOR_RESET);

    if (g_test_ctx.config.filter_tier > 0) {
        printf("%sFiltering by Tier: %s%s\n", COLOR_MAGENTA,
               test_infra_tier_str((TestTier)g_test_ctx.config.filter_tier), COLOR_RESET);
    }
    if (g_test_ctx.config.filter_feature > 0) {
        printf("%sFiltering by Feature: Feature %02d%s\n", COLOR_MAGENTA,
               g_test_ctx.config.filter_feature, COLOR_RESET);
    }
    if (g_test_ctx.config.filter_suite[0] != '\0') {
        printf("%sFiltering by Suite substring: \"%s\"%s\n", COLOR_MAGENTA,
               g_test_ctx.config.filter_suite, COLOR_RESET);
    }
    if (g_test_ctx.config.filter_name[0] != '\0') {
        printf("%sFiltering by Test substring: \"%s\"%s\n", COLOR_MAGENTA,
               g_test_ctx.config.filter_name, COLOR_RESET);
    }

    int grand_total = 0;
    int grand_passed = 0;
    int grand_failed = 0;
    int grand_skipped = 0;
    int64_t total_start = test_infra_timer_now();

    for (int s = 0; s < g_test_ctx.suite_count; s++) {
        TestSuite* suite = g_test_ctx.suites[s];

        /* Count matching tests in this suite */
        int matching_tests = 0;
        for (int t = 0; t < suite->test_count; t++) {
            if (test_infra_matches_filter(suite, &suite->tests[t], &g_test_ctx.config)) {
                matching_tests++;
            }
        }

        if (matching_tests == 0) continue;

        printf("\n%s--------------------------------------------------------------------------------%s\n", COLOR_GRAY, COLOR_RESET);
        printf("%sSuite: %-30s %s(%s)%s\n", COLOR_BOLD, suite->name, COLOR_GRAY,
               suite->description[0] != '\0' ? suite->description : "", COLOR_RESET);
        printf("%s--------------------------------------------------------------------------------%s\n", COLOR_GRAY, COLOR_RESET);
        fflush(stdout);

        int64_t suite_start = test_infra_timer_now();
        suite->passed_count = 0;
        suite->failed_count = 0;
        suite->skipped_count = 0;

        for (int t = 0; t < suite->test_count; t++) {
            TestCase* tc = &suite->tests[t];
            if (test_infra_matches_filter(suite, tc, &g_test_ctx.config)) {
                test_infra_run_test(suite, tc);
                grand_total++;
            }
        }

        int64_t suite_end = test_infra_timer_now();
        suite->total_elapsed_ms = test_infra_timer_ms(suite_start, suite_end);
        grand_passed += suite->passed_count;
        grand_failed += suite->failed_count;
        grand_skipped += suite->skipped_count;

        printf("  %sSuite Summary: %d passed, %d failed, %d skipped in %.2f ms%s\n",
               suite->failed_count > 0 ? COLOR_RED : COLOR_GREEN,
               suite->passed_count, suite->failed_count, suite->skipped_count,
               suite->total_elapsed_ms, COLOR_RESET);
        fflush(stdout);
    }

    int64_t total_end = test_infra_timer_now();
    double grand_time_ms = test_infra_timer_ms(total_start, total_end);

    printf("\n%s================================================================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s   TEST RUNNER SUMMARY%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s================================================================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("  Total Test Cases Executed: %d\n", grand_total);
    printf("  Passed:                    %s%d%s\n", COLOR_GREEN, grand_passed, COLOR_RESET);
    printf("  Failed:                    %s%d%s\n", grand_failed > 0 ? COLOR_RED : COLOR_GREEN, grand_failed, COLOR_RESET);
    printf("  Skipped:                   %s%d%s\n", COLOR_YELLOW, grand_skipped, COLOR_RESET);
    printf("  Total Assertions Checked:  %d\n", g_test_ctx.total_assertions);
    printf("  Total Wall Clock Time:     %.2f ms (%.3f sec)\n", grand_time_ms, grand_time_ms / 1000.0);

    if (grand_total > 0) {
        double pct = (double)grand_passed * 100.0 / (double)grand_total;
        printf("  Pass Rate:                 %s%.1f%%%s\n",
               grand_failed == 0 ? COLOR_GREEN : COLOR_RED, pct, COLOR_RESET);
    }

    if (g_test_ctx.config.json_output_path[0] != '\0') {
        test_infra_export_json(g_test_ctx.config.json_output_path, grand_total,
                               grand_passed, grand_failed, grand_skipped, grand_time_ms);
        printf("  JSON Report Written:       %s\n", g_test_ctx.config.json_output_path);
    }

    if (grand_failed == 0) {
        printf("\n%s[ ALL TESTS PASSED SUCCESSFULLY ]%s\n\n", COLOR_GREEN, COLOR_RESET);
        return 0;
    } else {
        printf("\n%s[ TEST RUNNER FINISHED WITH FAILURES: %d FAILED ]%s\n\n", COLOR_RED, grand_failed, COLOR_RESET);
        return 1;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_TEST_INFRA_H */
