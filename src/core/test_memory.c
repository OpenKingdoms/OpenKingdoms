/*
 * test_memory.c — Unit tests for the portable tak_memory allocator.
 *
 * Covers:
 *   - Basic alloc / free / calloc / realloc / strdup contracts
 *   - Named variants (Debug leak attribution)
 *   - Stats tracking (live / peak / total)
 *   - OOM handler plumbing
 *   - Poison fills on alloc/free (Debug only)
 *   - Realloc content preservation
 *   - Stress loop
 *
 * Double-free and wild-pointer-free land on TAK_TRAP() which aborts the
 * process. Those are intentional crashes, not something we can assert on
 * from inside a test process.
 */

#include "test_framework.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int oom_handler_call_count = 0;

static void test_oom_handler(void) {
    oom_handler_call_count++;
}

/* ══════════════════════════════════════════════════════════════════════
 *  tak_malloc / tak_free basics
 * ══════════════════════════════════════════════════════════════════════ */

TEST(malloc_returns_non_null) {
    void *p = tak_malloc(64);
    ASSERT_NOT_NULL(p);
    tak_free(p);
}

TEST(malloc_zero_returns_null) {
    /* By policy the allocator rejects zero-size requests so callers
       don't rely on implementation-defined libc behavior. */
    ASSERT_NULL(tak_malloc(0));
}

TEST(malloc_huge_returns_null) {
    /* Anything above 0x80000000 is rejected (header size field is u32). */
    ASSERT_NULL(tak_malloc(0x80000001U));
}

TEST(free_null_is_safe) {
    tak_free(NULL);
}

TEST(malloc_memory_is_writable) {
    char *p = (char *)tak_malloc(128);
    ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 128);
    ASSERT_EQ_INT(0xAB, (unsigned char)p[0]);
    ASSERT_EQ_INT(0xAB, (unsigned char)p[127]);
    tak_free(p);
}

TEST(malloc_returns_aligned_memory) {
    /* User pointer must satisfy max_align_t alignment (16 on x86-64 /
       arm64 / Apple clang). */
    void *p = tak_malloc(1);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(0, (int)((uintptr_t)p & 15u));
    tak_free(p);
}

/* ══════════════════════════════════════════════════════════════════════
 *  tak_calloc
 * ══════════════════════════════════════════════════════════════════════ */

TEST(calloc_returns_zeroed_memory) {
    unsigned char *p = (unsigned char *)tak_calloc(32, 4);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ_INT(0, p[i]);
    }
    tak_free(p);
}

TEST(calloc_overflow_returns_null) {
    /* count * elem_size overflows size_t — must return NULL, not crash. */
    void *p = tak_calloc((size_t)-1 / 2, 4);
    ASSERT_NULL(p);
}

/* ══════════════════════════════════════════════════════════════════════
 *  tak_realloc
 * ══════════════════════════════════════════════════════════════════════ */

TEST(realloc_null_acts_as_malloc) {
    void *p = tak_realloc(NULL, 64);
    ASSERT_NOT_NULL(p);
    tak_free(p);
}

TEST(realloc_preserves_data_on_grow) {
    char *p = (char *)tak_malloc(16);
    ASSERT_NOT_NULL(p);
    memcpy(p, "hello world!\0\0\0", 16);

    char *p2 = (char *)tak_realloc(p, 256);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ_INT(0, memcmp(p2, "hello world!", 13));
    tak_free(p2);
}

TEST(realloc_preserves_data_on_shrink) {
    char *p = (char *)tak_malloc(64);
    ASSERT_NOT_NULL(p);
    memcpy(p, "aramon-veruna-zhon-taros", 24);

    char *p2 = (char *)tak_realloc(p, 24);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ_INT(0, memcmp(p2, "aramon-veruna-zhon-taros", 24));
    tak_free(p2);
}

TEST(realloc_to_zero_frees) {
    void *p = tak_malloc(64);
    ASSERT_NOT_NULL(p);
    void *p2 = tak_realloc(p, 0);
    ASSERT_NULL(p2);
}

/* ══════════════════════════════════════════════════════════════════════
 *  tak_strdup
 * ══════════════════════════════════════════════════════════════════════ */

TEST(strdup_copies_string) {
    char *copy = tak_strdup("Aramon");
    ASSERT_NOT_NULL(copy);
    ASSERT_EQ_STR("Aramon", copy);
    tak_free(copy);
}

TEST(strdup_null_returns_null) {
    ASSERT_NULL(tak_strdup(NULL));
}

TEST(strdup_empty_string) {
    char *copy = tak_strdup("");
    ASSERT_NOT_NULL(copy);
    ASSERT_EQ_STR("", copy);
    tak_free(copy);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Statistics tracking
 * ══════════════════════════════════════════════════════════════════════ */

TEST(stats_track_alloc_and_free) {
    tak_mem_reset_stats();

    void *p1 = tak_malloc(100);
    void *p2 = tak_malloc(200);

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(2, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(2, (int)stats.total_alloc_count);
    ASSERT_EQ_INT(300, (int)stats.live_bytes);
    ASSERT_EQ_INT(300, (int)stats.total_bytes);

    tak_free(p1);
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(1, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(2, (int)stats.total_alloc_count);
    ASSERT_EQ_INT(200, (int)stats.live_bytes);

    tak_free(p2);
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.live_bytes);
}

TEST(stats_peak_tracks_high_water_mark) {
    tak_mem_reset_stats();

    void *p1 = tak_malloc(64);
    void *p2 = tak_malloc(64);
    void *p3 = tak_malloc(64);
    tak_free(p2);
    tak_free(p3);

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(3, (int)stats.peak_alloc_count);
    ASSERT_EQ_INT(1, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(192, (int)stats.peak_live_bytes);

    tak_free(p1);
}

TEST(stats_reset_clears_everything) {
    tak_mem_reset_stats();
    void *p = tak_malloc(64);

    tak_mem_reset_stats();

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(0, (int)stats.total_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.peak_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.total_bytes);
    ASSERT_EQ_INT(0, (int)stats.live_bytes);
    ASSERT_EQ_INT(0, (int)stats.peak_live_bytes);

    /* Free-after-reset must not crash or underflow thanks to the
       zero-clamp in stats_track_free_locked. */
    tak_free(p);
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.live_bytes);
}

/* ══════════════════════════════════════════════════════════════════════
 *  OOM handler
 * ══════════════════════════════════════════════════════════════════════ */

TEST(oom_handler_set_and_restore) {
    tak_oom_handler_fn old = tak_set_oom_handler(test_oom_handler);
    ASSERT_NULL(old);

    tak_oom_handler_fn prev = tak_set_oom_handler(NULL);
    ASSERT(prev == test_oom_handler);
}

/* ══════════════════════════════════════════════════════════════════════
 *  tak_malloc_named / tak_free_named
 * ══════════════════════════════════════════════════════════════════════ */

TEST(malloc_named_works) {
    tak_mem_reset_stats();
    void *p = tak_malloc_named("test.named_alloc", 128);
    ASSERT_NOT_NULL(p);

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(1, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(128, (int)stats.live_bytes);

    tak_free_named(p);
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
}

TEST(realloc_named_preserves_data) {
    char *p = (char *)tak_malloc_named("test.rn_initial", 16);
    ASSERT_NOT_NULL(p);
    memcpy(p, "testing123\0\0\0\0\0", 16);

    char *p2 = (char *)tak_realloc_named(p, "test.rn_grown", 64);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ_INT(0, memcmp(p2, "testing123", 10));
    tak_free(p2);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Poison fills (Debug builds only)
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef TAK_DEBUG
TEST(debug_alloc_is_poisoned_with_0xCD) {
    unsigned char *p = (unsigned char *)tak_malloc(32);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ_INT(0xCD, p[i]);
    }
    tak_free(p);
}
#endif

/* ══════════════════════════════════════════════════════════════════════
 *  Stress
 * ══════════════════════════════════════════════════════════════════════ */

TEST(many_alloc_free_cycles) {
    tak_mem_reset_stats();
    for (int i = 0; i < 1000; i++) {
        void *p = tak_malloc(64);
        ASSERT_NOT_NULL(p);
        tak_free(p);
    }

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(1000, (int)stats.total_alloc_count);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
    ASSERT_EQ_INT(1, (int)stats.peak_alloc_count);
}

TEST(alternating_sizes_and_patterns) {
    tak_mem_reset_stats();
    void *ptrs[32];
    size_t sizes[32];
    for (int i = 0; i < 32; i++) {
        sizes[i] = (size_t)((i + 1) * 17);
        ptrs[i] = tak_malloc(sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (i & 0xff), sizes[i]);
    }
    /* Verify each block still holds its pattern — catches header-overlap bugs. */
    for (int i = 0; i < 32; i++) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            ASSERT_EQ_INT((i & 0xff), p[j]);
        }
    }
    for (int i = 0; i < 32; i++) tak_free(ptrs[i]);

    TakMemStats stats;
    tak_mem_get_stats(&stats);
    ASSERT_EQ_INT(0, (int)stats.live_alloc_count);
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    tak_mem_init();

    TEST_SUITE("tak_malloc / tak_free");
    RUN(malloc_returns_non_null);
    RUN(malloc_zero_returns_null);
    RUN(malloc_huge_returns_null);
    RUN(free_null_is_safe);
    RUN(malloc_memory_is_writable);
    RUN(malloc_returns_aligned_memory);

    TEST_SUITE("tak_calloc");
    RUN(calloc_returns_zeroed_memory);
    RUN(calloc_overflow_returns_null);

    TEST_SUITE("tak_realloc");
    RUN(realloc_null_acts_as_malloc);
    RUN(realloc_preserves_data_on_grow);
    RUN(realloc_preserves_data_on_shrink);
    RUN(realloc_to_zero_frees);

    TEST_SUITE("tak_strdup");
    RUN(strdup_copies_string);
    RUN(strdup_null_returns_null);
    RUN(strdup_empty_string);

    TEST_SUITE("Statistics");
    RUN(stats_track_alloc_and_free);
    RUN(stats_peak_tracks_high_water_mark);
    RUN(stats_reset_clears_everything);

    TEST_SUITE("OOM handler");
    RUN(oom_handler_set_and_restore);

    TEST_SUITE("Named variants");
    RUN(malloc_named_works);
    RUN(realloc_named_preserves_data);

#ifdef TAK_DEBUG
    TEST_SUITE("Debug poison fills");
    RUN(debug_alloc_is_poisoned_with_0xCD);
#endif

    TEST_SUITE("Stress");
    RUN(many_alloc_free_cycles);
    RUN(alternating_sizes_and_patterns);

    /* Don't call tak_mem_shutdown here — in Debug it would print a leak
       report to stderr that makes parsing test output noisier. The
       TEST_REPORT macro returns directly. */
    TEST_REPORT();
}
