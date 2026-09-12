#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <time.h>

static int _tf_pass_count = 0;
static int _tf_fail_count = 0;
static int _tf_total_count = 0;
static int _tf_current_failed = 0;

/* Milliseconds off a clock that only counts up, so a case time never
 * comes out negative and never jumps when the wall clock is corrected.
 * On Windows clock() is time since the process started, which is what
 * we want. Elsewhere CLOCK_MONOTONIC is the right one, and where a
 * strict standard mode hides it we fall back to clock(), which is CPU
 * time there: close enough for tests that spend their time computing. */
static double _tf_now_ms(void) {
#if !defined(_WIN32) && defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    }
#endif
    return (double)clock() * (1000.0 / (double)CLOCKS_PER_SEC);
}

/* How long the case that just ran took. A suite can assert on its own
 * timing, and a caller can name the slowest cases. -1 before any run. */
static double _tf_last_ms = -1.0;

#define TEST(name) \
    static void name(void); \
    static void _run_##name(void) { \
        double _tf_t0; \
        _tf_total_count++; \
        _tf_current_failed = 0; \
        printf("  %-50s ", #name); \
        fflush(stdout); \
        _tf_t0 = _tf_now_ms(); \
        name(); \
        _tf_last_ms = _tf_now_ms() - _tf_t0; \
        if (!_tf_current_failed) { \
            _tf_pass_count++; \
            printf("PASS %8.1f ms\n", _tf_last_ms); \
        } else { \
            printf("    took %8.1f ms\n", _tf_last_ms); \
        } \
    } \
    static void name(void)

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_EQ_INT(expected, actual) \
    do { \
        int _e = (expected), _a = (actual); \
        if (_e != _a) { \
            printf("FAIL\n    %s:%d: expected %d, got %d\n", \
                   __FILE__, __LINE__, _e, _a); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_EQ_STR(expected, actual) \
    do { \
        const char *_e = (expected), *_a = (actual); \
        if (_a == NULL) { \
            printf("FAIL\n    %s:%d: expected \"%s\", got NULL\n", \
                   __FILE__, __LINE__, _e); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
        if (strcmp(_e, _a) != 0) { \
            printf("FAIL\n    %s:%d: expected \"%s\", got \"%s\"\n", \
                   __FILE__, __LINE__, _e, _a); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_NULL(expr) \
    do { \
        const void *_v = (expr); \
        if (_v != NULL) { \
            printf("FAIL\n    %s:%d: expected NULL\n", __FILE__, __LINE__); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_NOT_NULL(expr) \
    do { \
        const void *_v = (expr); \
        if (_v == NULL) { \
            printf("FAIL\n    %s:%d: expected non-NULL\n", __FILE__, __LINE__); \
            _tf_fail_count++; \
            _tf_current_failed = 1; \
            return; \
        } \
    } while (0)

#define RUN(name) _run_##name()

#define TEST_REPORT() \
    do { \
        printf("\n----------------------------------------\n"); \
        printf("Results: %d passed, %d failed, %d total\n", \
               _tf_pass_count, _tf_fail_count, _tf_total_count); \
        printf("----------------------------------------\n"); \
        return _tf_fail_count > 0 ? 1 : 0; \
    } while (0)

#define TEST_SUITE(name) \
    printf("\n== %s ==\n", name)

#endif /* TEST_FRAMEWORK_H */
