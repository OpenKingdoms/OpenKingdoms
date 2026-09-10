#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>

static int _tf_pass_count = 0;
static int _tf_fail_count = 0;
static int _tf_total_count = 0;
static int _tf_current_failed = 0;

#define TEST(name) \
    static void name(void); \
    static void _run_##name(void) { \
        _tf_total_count++; \
        _tf_current_failed = 0; \
        printf("  %-50s ", #name); \
        fflush(stdout); \
        name(); \
        if (!_tf_current_failed) { \
            _tf_pass_count++; \
            printf("PASS\n"); \
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
