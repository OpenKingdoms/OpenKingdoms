#ifndef TAK_DEBUG_H
#define TAK_DEBUG_H

#include <stddef.h>

#ifdef TAK_DEBUG
    void debug_track_alloc(const char *name, void *ptr, size_t size);
    void debug_track_free(void *ptr);
    void debug_dump_leaks(void);
#else
    #define debug_track_alloc(name, ptr, size) ((void)0)
    #define debug_track_free(ptr) ((void)0)
    #define debug_dump_leaks() ((void)0)
#endif

#endif
