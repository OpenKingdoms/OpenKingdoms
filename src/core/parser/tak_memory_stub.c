/*
 * tak_memory_stub.c - Minimal memory shim for standalone test builds
 *
 * Maps tak_malloc/tak_free/etc. to stdlib equivalents so that code
 * using the tak_memory.h API can compile and link without pulling in
 * the full Windows-dependent memory.c allocator.
 */

#include <stdlib.h>
#include <string.h>

void *tak_malloc(size_t size)                     { return malloc(size); }
void  tak_free(void *ptr)                         { free(ptr); }
void *tak_realloc(void *ptr, size_t new_size)     { return realloc(ptr, new_size); }
void *tak_calloc(size_t count, size_t elem_size)  { return calloc(count, elem_size); }

char *tak_strdup(const char *str) {
    size_t len;
    char *copy;
    if (!str) return NULL;
    len = strlen(str) + 1;
    copy = (char *)malloc(len);
    if (copy) memcpy(copy, str, len);
    return copy;
}
