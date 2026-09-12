#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <time.h>

static int _tf_pass_count = 0;
static int _tf_fail_count = 0;
static int _tf_skip_count = 0;
static int _tf_total_count = 0;
static int _tf_current_failed = 0;
static int _tf_current_skipped = 0;

/* A skipped case tested nothing, so by default the binary that skipped
 * one does not report success. A suite with a mode where skipping is the
 * right answer, such as the CI run that has no game data to load, says so
 * with TEST_ALLOW_SKIPS. */
static int _tf_skips_allowed = 0;
static const char *_tf_skips_allowed_why = "";

/* The cases that skipped, so the report can name them instead of leaving
 * a reader to hunt back up the log for the word SKIP. */
#define TF_MAX_SKIPPED_NAMES 64
static const char *_tf_skipped_names[TF_MAX_SKIPPED_NAMES];
static int _tf_skipped_named = 0;

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
        _tf_current_skipped = 0; \
        printf("  %-50s ", #name); \
        fflush(stdout); \
        _tf_t0 = _tf_now_ms(); \
        name(); \
        _tf_last_ms = _tf_now_ms() - _tf_t0; \
        if (_tf_current_failed) { \
            printf("    took %8.1f ms\n", _tf_last_ms); \
        } else if (_tf_current_skipped) { \
            if (_tf_skipped_named < TF_MAX_SKIPPED_NAMES) { \
                _tf_skipped_names[_tf_skipped_named++] = #name; \
            } \
            _tf_skip_count++; \
            printf("SKIP %8.1f ms\n", _tf_last_ms); \
        } else { \
            _tf_pass_count++; \
            printf("PASS %8.1f ms\n", _tf_last_ms); \
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

/* A precondition the case could not meet. It is not a pass, because the
 * case tested nothing. It is counted on its own and, unless the suite has
 * declared that skips are expected, it fails the binary. Use it wherever
 * the old code printed SKIP and returned. */
#define SKIP(...) \
    do { \
        SKIP_MARK(__VA_ARGS__); \
        return; \
    } while (0)

/* The same mark, for a helper that hands its result back to the case
 * instead of returning out of it. */
#define SKIP_MARK(...) \
    do { \
        printf("SKIP ("); \
        printf(__VA_ARGS__); \
        printf(") "); \
        _tf_current_skipped = 1; \
    } while (0)

/* Declare that a skip is the right answer for this run, for instance the
 * CI run with no game data. Without it a skip fails the binary. */
#define TEST_ALLOW_SKIPS(why) \
    do { \
        _tf_skips_allowed = 1; \
        _tf_skips_allowed_why = (why); \
    } while (0)

#define RUN(name) _run_##name()

#define TEST_REPORT() \
    do { \
        printf("\n----------------------------------------\n"); \
        printf("Results: %d passed, %d failed, %d skipped, %d total\n", \
               _tf_pass_count, _tf_fail_count, _tf_skip_count, \
               _tf_total_count); \
        if (_tf_skip_count > 0) { \
            int _tf_i; \
            printf("Tested nothing:"); \
            for (_tf_i = 0; _tf_i < _tf_skipped_named; _tf_i++) { \
                printf(" %s", _tf_skipped_names[_tf_i]); \
            } \
            if (_tf_skip_count > _tf_skipped_named) printf(" and more"); \
            printf("\n"); \
            if (_tf_skips_allowed) { \
                printf("Skips are expected in this run: %s\n", \
                       _tf_skips_allowed_why); \
            } else { \
                printf("A skip is not a pass, so this run is not green.\n"); \
            } \
        } \
        printf("----------------------------------------\n"); \
        return (_tf_fail_count > 0 \
                || (_tf_skip_count > 0 && !_tf_skips_allowed)) ? 1 : 0; \
    } while (0)

#define TEST_SUITE(name) \
    printf("\n== %s ==\n", name)

#endif /* TEST_FRAMEWORK_H */
