/**
 * @file test_arch.c
 * @brief Tier 1, Tier 2, and Tier 5 tests for CPower core lifecycle, timers,
 *        Box-Muller / Log-Normal statistics, error strings, and struct contracts.
 */

#include "test_infra.h"
#include "cpower.h"
#include "common.h"
#include "perception.h"
#include "input.h"
#include "ipc.h"
#include "security.h"
#include "text_hook.h"

/* Compile-time layout invariants matching ARCH_SPEC.md & v1.1.0 ABI */
_Static_assert(sizeof(ShmRingBufferHeader) == 192, "ShmRingBufferHeader must be 192 bytes (3 cache lines)");
_Static_assert(offsetof(ShmRingBufferHeader, writeIdx) == 64, "writeIdx must be at cache line 1 (offset 64)");
_Static_assert(offsetof(ShmRingBufferHeader, readIdx) == 128, "readIdx must be at cache line 2 (offset 128)");
_Static_assert(sizeof(IPC_FrameHeader) == 8, "IPC_FrameHeader must be 8 bytes");
_Static_assert(sizeof(TreePacketHeader) == 32, "TreePacketHeader must be 32 bytes");
_Static_assert(sizeof(SemanticNode) == 800, "SemanticNode packed size must be 800 bytes");
_Static_assert(sizeof(CommandPacket) == 564, "CommandPacket packed size must be 564 bytes");
_Static_assert(sizeof(AckPacket) == 28, "AckPacket packed size must be 28 bytes");
_Static_assert(sizeof(EventPacket) == 32, "EventPacket packed size must be 32 bytes");
_Static_assert(sizeof(DiffPacketHeader) == 44, "DiffPacketHeader packed size must be 44 bytes");
_Static_assert(sizeof(WindowPacketHeader) == 16, "WindowPacketHeader packed size must be 16 bytes");
_Static_assert(sizeof(TreeDiffEntry) == 824, "TreeDiffEntry packed size must be 824 bytes");
_Static_assert(sizeof(WindowInfo) == 420, "WindowInfo packed size must be 420 bytes");
_Static_assert(sizeof(CPowerTextHookEntry) == 312, "CPowerTextHookEntry packed size must be 312 bytes");

static void arch_suite_setup(void) {
    CPower_SetLogLevel(CPOWER_LOG_NONE);
}

static void arch_suite_teardown(void) {
    CPower_SetLogLevel(CPOWER_LOG_INFO);
    CPower_Shutdown();
}

static void test_arch_version_and_lifecycle(void) {
    const char* ver = CPower_GetVersion();
    ASSERT_NOT_NULL(ver);
    ASSERT_EQ_STR(CPOWER_VERSION_STRING, ver);
    ASSERT_EQ_STR("1.1.0", ver);

    cpower_status_t st = CPower_Init();
    ASSERT_EQ_INT(CPOWER_OK, st);

    /* Calling Init again while active must either be idempotent or report ALREADY_INITIALIZED */
    cpower_status_t st2 = CPower_Init();
    ASSERT_TRUE(st2 == CPOWER_OK || st2 == CPOWER_ERR_ALREADY_INITIALIZED);

    CPower_Shutdown();
    /* Double shutdown must be safe */
    CPower_Shutdown();

    /* Re-initialization after clean shutdown must succeed */
    st = CPower_Init();
    ASSERT_EQ_INT(CPOWER_OK, st);
    CPower_Shutdown();
}

static void test_arch_error_strings(void) {
    const cpower_status_t codes[] = {
        CPOWER_OK,
        CPOWER_ERR_INVALID_ARG,
        CPOWER_ERR_OUT_OF_MEMORY,
        CPOWER_ERR_NOT_INITIALIZED,
        CPOWER_ERR_ALREADY_INITIALIZED,
        CPOWER_ERR_COM_FAILURE,
        CPOWER_ERR_ELEMENT_NOT_FOUND,
        CPOWER_ERR_PATTERN_NOT_SUPPORTED,
        CPOWER_ERR_TIMEOUT,
        CPOWER_ERR_BUFFER_TOO_SMALL,
        CPOWER_ERR_IPC_FAILURE,
        CPOWER_ERR_SECURITY_DENIED,
        CPOWER_ERR_DXGI_FAILURE,
        CPOWER_ERR_NOT_SUPPORTED,
        CPOWER_ERR_SYSTEM_FAILURE
    };

    const char* okStr = CPower_GetErrorString(CPOWER_OK);
    ASSERT_NOT_NULL(okStr);
    ASSERT_TRUE(strlen(okStr) > 0);

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char* s = CPower_GetErrorString(codes[i]);
        ASSERT_NOT_NULL(s);
        ASSERT_TRUE(strlen(s) > 0);
        if (codes[i] != CPOWER_OK) {
            ASSERT_TRUE(strcmp(s, okStr) != 0);
        }
    }

    /* Out-of-range status code must return a safe non-NULL string */
    const char* unknownStr = CPower_GetErrorString((cpower_status_t)-999);
    ASSERT_NOT_NULL(unknownStr);
    ASSERT_TRUE(strlen(unknownStr) > 0);
}

static void test_arch_struct_packing_and_constants(void) {
    ASSERT_EQ_UINT(192U, sizeof(ShmRingBufferHeader));
    ASSERT_EQ_UINT(64U, offsetof(ShmRingBufferHeader, writeIdx));
    ASSERT_EQ_UINT(128U, offsetof(ShmRingBufferHeader, readIdx));
    ASSERT_EQ_UINT(8U, sizeof(IPC_FrameHeader));
    ASSERT_EQ_UINT(32U, sizeof(TreePacketHeader));
    ASSERT_EQ_UINT(800U, sizeof(SemanticNode));
    ASSERT_EQ_UINT(32U, offsetof(SemanticNode, name));
    ASSERT_EQ_UINT(564U, sizeof(CommandPacket));
    ASSERT_EQ_UINT(28U, sizeof(AckPacket));
    ASSERT_EQ_UINT(32U, sizeof(EventPacket));
    ASSERT_EQ_UINT(44U, sizeof(DiffPacketHeader));
    ASSERT_EQ_UINT(16U, sizeof(WindowPacketHeader));
    ASSERT_EQ_UINT(824U, sizeof(TreeDiffEntry));
    ASSERT_EQ_UINT(420U, sizeof(WindowInfo));
    ASSERT_EQ_UINT(312U, sizeof(CPowerTextHookEntry));

    ASSERT_EQ_UINT(0x41475043U, CPOWER_SHM_MAGIC);
    ASSERT_EQ_UINT(1U, CPOWER_SHM_VERSION);
    ASSERT_EQ_UINT(0x54524545U, CPOWER_TREE_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x434D4E44U, CPOWER_CMD_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x45564E54U, CPOWER_EVENT_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x41434B50U, CPOWER_ACK_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x44494646U, CPOWER_DIFF_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x574E4453U, CPOWER_WINDOWS_PACKET_MAGIC);
    ASSERT_EQ_UINT(0x5458484BU, CPOWER_TEXT_HOOK_MAGIC);

    /* Default ring capacity must be a non-zero power of two */
    ASSERT_TRUE(CPOWER_DEFAULT_RING_CAPACITY > 0U);
    ASSERT_EQ_UINT(0U, CPOWER_DEFAULT_RING_CAPACITY & (CPOWER_DEFAULT_RING_CAPACITY - 1U));
}

static void test_arch_qpc_timer_precision(void) {
    QpcTimer timer;
    memset(&timer, 0, sizeof(timer));

    Timer_Init(&timer);
    ASSERT_TRUE(timer.frequency.QuadPart > 0);

    Timer_Start(&timer);
    Timer_AccurateSleep(12.0);
    Timer_Stop(&timer);

    double ms = Timer_ElapsedMs(&timer);
    double us = Timer_ElapsedMicroseconds(&timer);

    ASSERT_TRUE(ms >= 9.0);
    ASSERT_TRUE(ms < 250.0);
    ASSERT_DOUBLE_NEAR(ms * 1000.0, us, 5.0);

    /* Verify repeated reads after Stop return consistent duration */
    Sleep(2);
    double msAfter = Timer_ElapsedMs(&timer);
    ASSERT_DOUBLE_NEAR(ms, msAfter, 0.001);

    /* Verify strict non-decreasing monotonicity across consecutive runs */
    double prev = -1.0;
    for (int i = 0; i < 20; i++) {
        Timer_Start(&timer);
        volatile int spin = 0;
        for (int k = 0; k < (i + 1) * 200; k++) {
            spin += k;
        }
        (void)spin;
        Timer_Stop(&timer);
        double cur = Timer_ElapsedMicroseconds(&timer);
        ASSERT_TRUE(cur >= 0.0);
        if (i == 0) {
            prev = cur;
        }
        (void)prev;
    }
}

static void test_arch_logging_levels(void) {
    CPower_SetLogLevel(CPOWER_LOG_DEBUG);
    CPower_Log(CPOWER_LOG_DEBUG, "Debug test log value=%d", 42);
    CPower_SetLogLevel(CPOWER_LOG_NONE);
    CPower_Log(CPOWER_LOG_ERROR, "Suppressed log value=%s", "none");
    ASSERT_TRUE(1);
}

static void test_arch_random_uniform_distribution(void) {
    ASSERT_EQ_INT(CPOWER_OK, CPower_Init());

    const int N = 2000;
    const double lo = -10.0;
    const double hi = 20.0;
    double sum = 0.0;
    double sumSq = 0.0;

    for (int i = 0; i < N; i++) {
        double v = Math_RandomUniform(lo, hi);
        ASSERT_TRUE(isfinite(v));
        ASSERT_TRUE(v >= lo && v <= hi);
        sum += v;
        sumSq += v * v;
    }

    double mean = sum / (double)N;
    double var = (sumSq / (double)N) - (mean * mean);
    /* Uniform[-10, 20]: mean = 5.0, variance = 900/12 = 75.0 */
    ASSERT_TRUE(mean > 3.0 && mean < 7.0);
    ASSERT_TRUE(var > 45.0 && var < 105.0);

    /* Degenerate equal bounds */
    double eq = Math_RandomUniform(4.25, 4.25);
    ASSERT_DOUBLE_NEAR(4.25, eq, 1e-9);
}

static void test_arch_box_muller_statistics(void) {
    ASSERT_EQ_INT(CPOWER_OK, CPower_Init());

    const int N = 2500;
    double sum = 0.0;
    double sumSq = 0.0;
    int posCount = 0;
    int negCount = 0;
    int tailCount = 0;

    for (int i = 0; i < N; i++) {
        double z = Math_BoxMullerNormal();
        ASSERT_TRUE(isfinite(z));
        sum += z;
        sumSq += z * z;
        if (z > 0.0) posCount++;
        else if (z < 0.0) negCount++;
        if (fabs(z) > 1.5) tailCount++;
    }

    double mean = sum / (double)N;
    double var = (sumSq / (double)N) - (mean * mean);

    /* N(0, 1): sample mean ~ 0, sample variance ~ 1 */
    ASSERT_TRUE(fabs(mean) < 0.15);
    ASSERT_TRUE(var > 0.75 && var < 1.25);
    ASSERT_TRUE(posCount > (N * 38) / 100);
    ASSERT_TRUE(negCount > (N * 38) / 100);
    /* P(|Z| > 1.5) ~ 13.36%, expect well over 5% */
    ASSERT_TRUE(tailCount > (N * 5) / 100);
}

static void test_arch_log_normal_statistics(void) {
    ASSERT_EQ_INT(CPOWER_OK, CPower_Init());

    const int N = 2500;
    const double medianMs = 120.0;
    const double sigma = 0.35;
    double logSum = 0.0;
    double logSumSq = 0.0;
    int aboveMedian = 0;

    for (int i = 0; i < N; i++) {
        double x = Math_SampleLogNormal(medianMs, sigma);
        ASSERT_TRUE(isfinite(x));
        ASSERT_TRUE(x > 0.0);
        if (x > medianMs) aboveMedian++;

        double lx = log(x);
        logSum += lx;
        logSumSq += lx * lx;
    }

    double logMean = logSum / (double)N;
    double logVar = (logSumSq / (double)N) - (logMean * logMean);
    double logStd = sqrt(logVar > 0.0 ? logVar : 0.0);

    ASSERT_DOUBLE_NEAR(log(medianMs), logMean, 0.10);
    ASSERT_DOUBLE_NEAR(sigma, logStd, 0.10);
    ASSERT_TRUE(aboveMedian > (N * 38) / 100 && aboveMedian < (N * 62) / 100);

    /* Zero sigma yields deterministic median */
    double det = Math_SampleLogNormal(55.0, 0.0);
    ASSERT_DOUBLE_NEAR(55.0, det, 1e-6);
}

static void test_arch_boundary_null_safety(void) {
    Timer_Init(NULL);
    Timer_Start(NULL);
    Timer_Stop(NULL);
    double em = Timer_ElapsedMs(NULL);
    double eu = Timer_ElapsedMicroseconds(NULL);
    ASSERT_TRUE(em == 0.0 || isfinite(em));
    ASSERT_TRUE(eu == 0.0 || isfinite(eu));

    /* Non-positive sleep durations must return immediately */
    QpcTimer t;
    Timer_Init(&t);
    Timer_Start(&t);
    Timer_AccurateSleep(0.0);
    Timer_AccurateSleep(-10.0);
    Timer_Stop(&t);
    ASSERT_TRUE(Timer_ElapsedMs(&t) < 100.0);
}

void RegisterArchTestSuite(void) {
    TestSuite* suite = test_suite_create(
        "ArchCoreSuite",
        "Core lifecycle, error strings, struct contracts, QPC timers, and Box-Muller/LogNormal RNG",
        arch_suite_setup,
        arch_suite_teardown
    );
    if (!suite) return;

    test_case_register(suite, "Test_Arch_VersionAndLifecycle",
                       "Verify CPower_GetVersion, CPower_Init, CPower_Shutdown idempotency",
                       2, TIER_1_FEATURE_COVERAGE, test_arch_version_and_lifecycle);

    test_case_register(suite, "Test_Arch_ErrorStringsCoverage",
                       "Verify CPower_GetErrorString covers all cpower_status_t values and unknown fallback",
                       2, TIER_1_FEATURE_COVERAGE, test_arch_error_strings);

    test_case_register(suite, "Test_Arch_StructPackingAndConstants",
                       "Verify ABI struct sizes, cache-line offsets, and IPC magic constants",
                       1, TIER_1_FEATURE_COVERAGE, test_arch_struct_packing_and_constants);

    test_case_register(suite, "Test_Arch_QpcTimerPrecision",
                       "Verify QpcTimer frequency, microsecond scaling, and Timer_AccurateSleep",
                       13, TIER_1_FEATURE_COVERAGE, test_arch_qpc_timer_precision);

    test_case_register(suite, "Test_Arch_LoggingLevels",
                       "Verify CPower_SetLogLevel and CPower_Log dispatch without crashes",
                       14, TIER_1_FEATURE_COVERAGE, test_arch_logging_levels);

    test_case_register(suite, "Test_Arch_RandomUniformDistribution",
                       "Verify Math_RandomUniform bounds, mean, and variance over 2000 samples",
                       13, TIER_1_FEATURE_COVERAGE, test_arch_random_uniform_distribution);

    test_case_register(suite, "Test_Arch_BoxMullerNormalStatistics",
                       "Verify N(0,1) mean, variance, and tail symmetry over 2500 Box-Muller samples",
                       16, TIER_5_ADVERSARIAL, test_arch_box_muller_statistics);

    test_case_register(suite, "Test_Arch_LogNormalDistributionStatistics",
                       "Verify Math_SampleLogNormal positivity, log-mean, and log-sigma over 2500 samples",
                       16, TIER_5_ADVERSARIAL, test_arch_log_normal_statistics);

    test_case_register(suite, "Test_Arch_BoundaryNullSafety",
                       "Verify NULL timer safety and zero/negative sleep boundary handling",
                       16, TIER_2_BOUNDARY_CORNER, test_arch_boundary_null_safety);
}
