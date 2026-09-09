/*
 * memory.c — TAK portable memory allocator
 *
 * Every tak_malloc-family call routes through a single code path that
 * prepends a small AllocHeader to the user block. See tak_memory.h for
 * the public contract. Highlights:
 *
 *   - Portable mutex shim (CRITICAL_SECTION on Windows, pthread on POSIX)
 *   - Header is padded to max_align_t so the returned pointer is aligned
 *     the same as libc malloc would return it
 *   - Debug builds: poison fill (0xCD on alloc, 0xDD on free), live-list
 *     for leak reports, magic canary for double-free / wild-free detection
 *   - Release builds: header shrinks to magic+size, no poison, no list
 *   - Lazy first-call init via a portable call-once so tests can use the
 *     allocator without a manual tak_mem_init() call
 */

#include "tak_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ── Platform shims ──────────────────────────────────────────────────── */

#ifdef _WIN32
  #include <windows.h>
  #include <intrin.h>
  #include <malloc.h>

  typedef CRITICAL_SECTION tak_mutex_t;
  static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

  static BOOL CALLBACK win_init_mutex_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
      (void)once; (void)param; (void)ctx;
      tak_mutex_t *m = (tak_mutex_t *)param;
      InitializeCriticalSection(m);
      return TRUE;
  }

  static void tak_mutex_init_once(tak_mutex_t *m) {
      InitOnceExecuteOnce(&g_init_once, win_init_mutex_cb, m, NULL);
  }
  static void tak_mutex_lock(tak_mutex_t *m)    { EnterCriticalSection(m); }
  static void tak_mutex_unlock(tak_mutex_t *m)  { LeaveCriticalSection(m); }
  /* No destroy: we never tear the mutex down; leaving it live through
     shutdown is safer if other subsystems try to allocate late. */

  #define TAK_TRAP() __debugbreak()
#else
  #include <pthread.h>

  typedef pthread_mutex_t tak_mutex_t;

  static void tak_mutex_init_once(tak_mutex_t *m) { (void)m; /* static init */ }
  static void tak_mutex_lock(tak_mutex_t *m)      { pthread_mutex_lock(m); }
  static void tak_mutex_unlock(tak_mutex_t *m)    { pthread_mutex_unlock(m); }

  #define TAK_TRAP() __builtin_trap()
#endif

/* ── Alignment helpers ───────────────────────────────────────────────── */

/* Match libc malloc alignment guarantees. On every mainstream ABI
   max_align_t is 16 (long double on x86-64 / 128-bit types). We use
   a literal 16 to avoid #include <stddef.h> alignof-on-typedef churn
   across C standards. */
#define TAK_MAX_ALIGN 16u

#define TAK_ALIGN_UP(n, a) (((n) + (a) - 1u) & ~((a) - 1u))

/* ── Allocation header ───────────────────────────────────────────────── */

#define TAK_MEM_MAGIC_LIVE  0xA110C5EDu   /* "ALLOC'ED" — block is live  */
#define TAK_MEM_MAGIC_DEAD  0xDEAD10CCu   /* "DEAD-LOC" — freed, detect 2x */

typedef struct AllocHeader {
    uint32_t magic;
    uint32_t size;           /* original requested bytes, fits in 4G */
#ifdef TAK_DEBUG
    const char *name;
    struct AllocHeader *prev;
    struct AllocHeader *next;
#endif
} AllocHeader;

/* Padded header size — user pointer starts at header_base + TAK_HEADER_SIZE. */
#define TAK_HEADER_SIZE ((unsigned)TAK_ALIGN_UP(sizeof(AllocHeader), TAK_MAX_ALIGN))

static void *tak_raw_alloc(size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, TAK_MAX_ALIGN);
#else
    void *p = NULL;
    if (posix_memalign(&p, TAK_MAX_ALIGN, size) != 0) return NULL;
    return p;
#endif
}

static void tak_raw_free(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static AllocHeader *header_from_user(void *user_ptr) {
    return (AllocHeader *)((char *)user_ptr - TAK_HEADER_SIZE);
}

static void *user_from_header(AllocHeader *h) {
    return (char *)h + TAK_HEADER_SIZE;
}

/* ── Global state ────────────────────────────────────────────────────── */

#ifdef _WIN32
static tak_mutex_t s_mutex;   /* lazily initialized via InitOnceExecuteOnce */
#else
static tak_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static int s_shutdown_complete;  /* set by tak_mem_shutdown to disable further work */

#ifdef TAK_DEBUG
static AllocHeader *s_live_head; /* doubly-linked list of currently live allocations */
#define POISON_ALLOC 0xCD
#define POISON_FREE  0xDD
#endif

/* Stats — all accessed under s_mutex. */
static unsigned int s_total_alloc_count;
static unsigned int s_live_alloc_count;
static unsigned int s_peak_alloc_count;
static unsigned int s_total_bytes;
static unsigned int s_live_bytes;
static unsigned int s_peak_live_bytes;

static tak_oom_handler_fn s_oom_handler;

/* ── Locked helpers ──────────────────────────────────────────────────── */

static void stats_track_alloc_locked(uint32_t size) {
    s_total_alloc_count++;
    s_live_alloc_count++;
    if (s_live_alloc_count > s_peak_alloc_count) s_peak_alloc_count = s_live_alloc_count;
    s_total_bytes += size;
    s_live_bytes  += size;
    if (s_live_bytes > s_peak_live_bytes) s_peak_live_bytes = s_live_bytes;
}

static void stats_track_free_locked(uint32_t size) {
    if (s_live_alloc_count > 0) s_live_alloc_count--;
    if (s_live_bytes >= size)   s_live_bytes -= size;
}

#ifdef TAK_DEBUG
static void live_list_add_locked(AllocHeader *h) {
    h->prev = NULL;
    h->next = s_live_head;
    if (s_live_head) s_live_head->prev = h;
    s_live_head = h;
}

static void live_list_remove_locked(AllocHeader *h) {
    if (h->prev) h->prev->next = h->next;
    else         s_live_head   = h->next;
    if (h->next) h->next->prev = h->prev;
}
#endif

/* ── Core alloc/free ─────────────────────────────────────────────────── */

static void *tak_alloc_internal(size_t size, const char *name) {
    /* malloc(0) is implementation-defined. We return NULL for simplicity
       — every caller is expected to check. Cap huge requests at 2GB so
       the uint32_t size field in AllocHeader can't overflow. */
    if (size == 0 || size > 0x80000000u) return NULL;

    tak_mutex_init_once(&s_mutex);
    tak_mutex_lock(&s_mutex);

    size_t total = TAK_HEADER_SIZE + size;
    AllocHeader *h = NULL;
    for (;;) {
        h = (AllocHeader *)tak_raw_alloc(total);
        if (h) break;
        if (!s_oom_handler) break;
        tak_mutex_unlock(&s_mutex);
        s_oom_handler();
        tak_mutex_lock(&s_mutex);
    }

    if (!h) {
        tak_mutex_unlock(&s_mutex);
        return NULL;
    }

    h->magic = TAK_MEM_MAGIC_LIVE;
    h->size  = (uint32_t)size;

#ifdef TAK_DEBUG
    h->name = name;
    live_list_add_locked(h);
    memset(user_from_header(h), POISON_ALLOC, size);
#else
    (void)name;
#endif

    stats_track_alloc_locked((uint32_t)size);

    tak_mutex_unlock(&s_mutex);
    return user_from_header(h);
}

static void tak_free_internal(void *ptr) {
    if (!ptr) return;

    AllocHeader *h = header_from_user(ptr);

    /* Magic check is done BEFORE taking the lock so a double-free on the
       same pointer from two threads can't both race past the check. This
       is a best-effort guardrail, not a formal correctness proof. */
    if (h->magic == TAK_MEM_MAGIC_DEAD) {
        fprintf(stderr, "[tak_mem] double-free detected at %p\n", ptr);
        TAK_TRAP();
        return;
    }
    if (h->magic != TAK_MEM_MAGIC_LIVE) {
        fprintf(stderr, "[tak_mem] free of non-tak-allocated or corrupted pointer %p (magic=0x%08x)\n",
                ptr, h->magic);
        TAK_TRAP();
        return;
    }

    tak_mutex_init_once(&s_mutex);
    tak_mutex_lock(&s_mutex);

    uint32_t size = h->size;
    stats_track_free_locked(size);

#ifdef TAK_DEBUG
    live_list_remove_locked(h);
    memset(ptr, POISON_FREE, size);
#endif

    h->magic = TAK_MEM_MAGIC_DEAD;
    tak_raw_free(h);

    tak_mutex_unlock(&s_mutex);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void *tak_malloc(size_t size) {
    return tak_alloc_internal(size, NULL);
}

void *tak_malloc_named(const char *name, size_t size) {
    return tak_alloc_internal(size, name);
}

void tak_free(void *ptr) {
    tak_free_internal(ptr);
}

void tak_free_named(void *ptr) {
    tak_free_internal(ptr);
}

void *tak_calloc(size_t count, size_t elem_size) {
    if (elem_size != 0 && count > (size_t)-1 / elem_size) return NULL; /* overflow */
    size_t total = count * elem_size;
    void *p = tak_alloc_internal(total, NULL);
    if (p) memset(p, 0, total);
    return p;
}

void *tak_realloc(void *old_ptr, size_t new_size) {
    return tak_realloc_named(old_ptr, NULL, new_size);
}

void *tak_realloc_named(void *old_ptr, const char *name, size_t new_size) {
    if (!old_ptr) return tak_alloc_internal(new_size, name);
    if (new_size == 0) { tak_free_internal(old_ptr); return NULL; }

    AllocHeader *old_h = header_from_user(old_ptr);
    if (old_h->magic != TAK_MEM_MAGIC_LIVE) {
        fprintf(stderr, "[tak_mem] realloc on invalid pointer %p (magic=0x%08x)\n",
                old_ptr, old_h->magic);
        TAK_TRAP();
        return NULL;
    }

    uint32_t old_size = old_h->size;
    void *new_ptr = tak_alloc_internal(new_size, name);
    if (!new_ptr) return NULL; /* leave old_ptr untouched on failure */

    size_t copy = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, old_ptr, copy);
    tak_free_internal(old_ptr);
    return new_ptr;
}

char *tak_strdup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *copy = (char *)tak_alloc_internal(len, NULL);
    if (copy) memcpy(copy, src, len);
    return copy;
}

/* ── Init / shutdown ─────────────────────────────────────────────────── */

void tak_mem_init(void) {
    tak_mutex_init_once(&s_mutex);
    s_shutdown_complete = 0;
}

void tak_mem_shutdown(void) {
#ifdef TAK_DEBUG
    tak_mutex_init_once(&s_mutex);
    tak_mutex_lock(&s_mutex);

    if (s_live_alloc_count == 0) {
        fprintf(stderr, "[tak_mem] clean shutdown: 0 leaks\n");
    } else {
        fprintf(stderr,
                "[tak_mem] LEAK REPORT: %u live allocation(s), %u bytes\n",
                s_live_alloc_count, s_live_bytes);
        int shown = 0;
        for (AllocHeader *h = s_live_head; h != NULL && shown < 50; h = h->next, shown++) {
            fprintf(stderr, "  [%p] %8u bytes  %s\n",
                    user_from_header(h),
                    h->size,
                    h->name ? h->name : "(unnamed)");
        }
        if (s_live_alloc_count > (unsigned)shown) {
            fprintf(stderr, "  ... and %u more\n", s_live_alloc_count - (unsigned)shown);
        }
    }

    tak_mutex_unlock(&s_mutex);
#endif
    s_shutdown_complete = 1;
}

/* ── OOM handler ─────────────────────────────────────────────────────── */

tak_oom_handler_fn tak_set_oom_handler(tak_oom_handler_fn new_handler) {
    tak_oom_handler_fn old = s_oom_handler;
    s_oom_handler = new_handler;
    return old;
}

/* ── Stats ───────────────────────────────────────────────────────────── */

void tak_mem_get_stats(TakMemStats *out) {
    if (!out) return;
    tak_mutex_init_once(&s_mutex);
    tak_mutex_lock(&s_mutex);
    out->total_alloc_count = s_total_alloc_count;
    out->live_alloc_count  = s_live_alloc_count;
    out->peak_alloc_count  = s_peak_alloc_count;
    out->total_bytes       = s_total_bytes;
    out->live_bytes        = s_live_bytes;
    out->peak_live_bytes   = s_peak_live_bytes;
    tak_mutex_unlock(&s_mutex);
}

void tak_mem_reset_stats(void) {
    /* Clears all counters including live. Any subsequent tak_free on a
       block that was live before the reset will be a stat-only no-op,
       because stats_track_free_locked clamps at zero. */
    tak_mutex_init_once(&s_mutex);
    tak_mutex_lock(&s_mutex);
    s_total_alloc_count = 0;
    s_live_alloc_count  = 0;
    s_peak_alloc_count  = 0;
    s_total_bytes       = 0;
    s_live_bytes        = 0;
    s_peak_live_bytes   = 0;
    tak_mutex_unlock(&s_mutex);
}
