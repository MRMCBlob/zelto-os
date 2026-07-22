// ztest.h — the tiny C assertion library for Zelto's test/ harness.
//
// A test is a plain C `main()` that calls ASSERT_*/EXPECT_* macros and ends with
// `return zt_result();`. The macros speak ONE machine-parseable line per failure
// that test/run-tests.sh greps out and reproduces in the report:
//
//     ZT_FAIL|<file>|<line>|<message>|<expected>|<actual>
//
// and the process exit code is the source of truth: 0 = pass, non-zero = fail.
// (The runner distinguishes a BUILD-FAIL — gcc could not compile the test — from a
// RUN-FAIL — the test ran and an assertion tripped — by whether compilation
// succeeded, so a red test always says *why* it is red.)
//
// ASSERT_* aborts the test on the first failure (the rest of the run would be
// meaningless); EXPECT_* records the failure and keeps going, so one run can
// surface several independent problems. Both emit the same line format, so the
// runner does not care which you used.
//
// The format is deliberately shared with test/framework/ztest.sh: a shell test
// emits byte-identical ZT_FAIL lines, so the runner parses C and shell failures
// through the same path.
#ifndef ZTEST_H
#define ZTEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

// Running failure count for EXPECT_*; a test returns zt_result() at the end.
static int zt_failures_ = 0;

// The one output primitive. `msg` describes the check, `exp`/`act` are already
// stringified. Pipe-delimited so the runner can split it; fields never contain a
// literal newline (they are single tokens the caller controls).
static inline void zt_fail_(const char *file, int line, const char *msg,
                            const char *exp, const char *act) {
    printf("ZT_FAIL|%s|%d|%s|%s|%s\n", file, line, msg, exp, act);
    fflush(stdout);
    zt_failures_++;
}

// Final result: 0 when nothing failed, 1 otherwise. `return zt_result();`.
static inline int zt_result(void) { return zt_failures_ ? 1 : 0; }

// --- boolean --------------------------------------------------------------

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            zt_fail_(__FILE__, __LINE__, "EXPECT_TRUE(" #cond ")", "true",     \
                     "false");                                                 \
        }                                                                      \
    } while (0)

#define ASSERT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            zt_fail_(__FILE__, __LINE__, "ASSERT_TRUE(" #cond ")", "true",     \
                     "false");                                                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define EXPECT_FALSE(cond)                                                     \
    do {                                                                       \
        if ((cond)) {                                                          \
            zt_fail_(__FILE__, __LINE__, "EXPECT_FALSE(" #cond ")", "false",   \
                     "true");                                                  \
        }                                                                      \
    } while (0)

#define ASSERT_FALSE(cond)                                                     \
    do {                                                                       \
        if ((cond)) {                                                          \
            zt_fail_(__FILE__, __LINE__, "ASSERT_FALSE(" #cond ")", "false",   \
                     "true");                                                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// --- integers -------------------------------------------------------------

#define EXPECT_EQ_INT(expected, actual)                                        \
    do {                                                                       \
        long long ze_ = (long long)(expected), za_ = (long long)(actual);      \
        if (ze_ != za_) {                                                      \
            char eb_[32], ab_[32];                                             \
            snprintf(eb_, sizeof(eb_), "%lld", ze_);                           \
            snprintf(ab_, sizeof(ab_), "%lld", za_);                           \
            zt_fail_(__FILE__, __LINE__, "EXPECT_EQ_INT(" #expected ", " #actual \
                     ")", eb_, ab_);                                           \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_INT(expected, actual)                                        \
    do {                                                                       \
        long long ze_ = (long long)(expected), za_ = (long long)(actual);      \
        if (ze_ != za_) {                                                      \
            char eb_[32], ab_[32];                                             \
            snprintf(eb_, sizeof(eb_), "%lld", ze_);                           \
            snprintf(ab_, sizeof(ab_), "%lld", za_);                           \
            zt_fail_(__FILE__, __LINE__, "ASSERT_EQ_INT(" #expected ", " #actual \
                     ")", eb_, ab_);                                           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// --- floats (absolute tolerance) ------------------------------------------

#define EXPECT_NEAR(expected, actual, eps)                                     \
    do {                                                                       \
        double ze_ = (double)(expected), za_ = (double)(actual);              \
        if (!(fabs(ze_ - za_) <= (eps))) {                                     \
            char eb_[48], ab_[48];                                             \
            snprintf(eb_, sizeof(eb_), "%g (+-%g)", ze_, (double)(eps));       \
            snprintf(ab_, sizeof(ab_), "%g", za_);                             \
            zt_fail_(__FILE__, __LINE__, "EXPECT_NEAR(" #expected ", " #actual \
                     ")", eb_, ab_);                                           \
        }                                                                      \
    } while (0)

#define ASSERT_NEAR(expected, actual, eps)                                     \
    do {                                                                       \
        double ze_ = (double)(expected), za_ = (double)(actual);              \
        if (!(fabs(ze_ - za_) <= (eps))) {                                     \
            char eb_[48], ab_[48];                                             \
            snprintf(eb_, sizeof(eb_), "%g (+-%g)", ze_, (double)(eps));       \
            snprintf(ab_, sizeof(ab_), "%g", za_);                             \
            zt_fail_(__FILE__, __LINE__, "ASSERT_NEAR(" #expected ", " #actual \
                     ")", eb_, ab_);                                           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// --- strings --------------------------------------------------------------

#define EXPECT_STR_EQ(expected, actual)                                        \
    do {                                                                       \
        const char *ze_ = (expected), *za_ = (actual);                        \
        if (strcmp(ze_, za_) != 0) {                                           \
            zt_fail_(__FILE__, __LINE__, "EXPECT_STR_EQ(" #expected ", " #actual \
                     ")", ze_, za_);                                           \
        }                                                                      \
    } while (0)

#define ASSERT_STR_EQ(expected, actual)                                        \
    do {                                                                       \
        const char *ze_ = (expected), *za_ = (actual);                        \
        if (strcmp(ze_, za_) != 0) {                                           \
            zt_fail_(__FILE__, __LINE__, "ASSERT_STR_EQ(" #expected ", " #actual \
                     ")", ze_, za_);                                           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#endif  // ZTEST_H
