#ifndef TAK_MEMORY_H
#define TAK_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════════
 *  TAK memory allocator
 *
 *  A single cross-platform allocator that wraps libc malloc/free and
 *  prepends a small bookkeeping header to every block. Thread-safe.
 *
 *  Behavior per build config:
 *
 *    Debug  (TAK_DEBUG defined)
 *      - Alloc/free poison fills  (catch uninit reads & use-after-free)
 *      - Magic canary in header   (catch double-free & wild pointer free)
 *      - Live-allocation linked list + leak report at shutdown
 *      - Optional per-alloc debug name via tak_malloc_named()
 *      - Full stats
 *
 *    Release
 *      - Header stripped to the minimum (magic + size)
 *      - No poison, no live-list, no per-alloc name storage
 *      - Full stats still work
 *
 *  Call tak_mem_init() once at startup (idempotent) and tak_mem_shutdown()
 *  at exit so the leak report can fire.
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Core API ──────────────────────────────────────────────────────── */

void *tak_malloc(size_t size);
void  tak_free(void *ptr);
void *tak_realloc(void *ptr, size_t new_size);
void *tak_calloc(size_t count, size_t elem_size);
char *tak_strdup(const char *src);

/* ── Named variants (for debug leak-report attribution) ───────────────
 *
 * In Debug builds the name pointer is stored in the block's header and
 * printed alongside any surviving allocations at tak_mem_shutdown().
 * In Release builds the name is discarded.
 *
 * The name string must outlive the allocation — pass a string literal. */

void *tak_malloc_named(const char *debug_name, size_t size);
void  tak_free_named(void *ptr);
void *tak_realloc_named(void *ptr, const char *debug_name, size_t new_size);

/* ── Init / shutdown ───────────────────────────────────────────────── */

/* Initialize the allocator. Idempotent. Thread-safe. Safe to call from
   any thread; the first allocation also self-initializes as a fallback. */
void tak_mem_init(void);

/* Tear down the allocator. In Debug builds this prints a leak report to
   stderr naming every surviving allocation. Does not free the leaks
   (that would hide them from Valgrind/ASan); just reports them. */
void tak_mem_shutdown(void);

/* ── OOM handler ───────────────────────────────────────────────────── */

typedef void (*tak_oom_handler_fn)(void);
tak_oom_handler_fn tak_set_oom_handler(tak_oom_handler_fn handler);

/* ── Statistics (debug HUD) ────────────────────────────────────────── */

typedef struct TakMemStats {
    unsigned int total_alloc_count; /* lifetime allocations  */
    unsigned int live_alloc_count;  /* currently live        */
    unsigned int peak_alloc_count;  /* high-water mark       */
    unsigned int total_bytes;       /* lifetime requested    */
    unsigned int live_bytes;        /* currently live        */
    unsigned int peak_live_bytes;   /* high-water mark       */
} TakMemStats;

void tak_mem_get_stats(TakMemStats *out);
void tak_mem_reset_stats(void);

#endif /* TAK_MEMORY_H */
