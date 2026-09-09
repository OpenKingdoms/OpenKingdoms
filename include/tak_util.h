#ifndef TAK_UTIL_H
#define TAK_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int glob_path_match(const char *pattern, const char *str);

size_t trimwhitespace(char *out, size_t len, const char *str);

/* Command-line parsing (implemented in util.c) */
char *find_cmdline_arg(const char *flag_to_find);

uint8_t check_cmdline_flag_pair(
    const char *unused_name,
    uint32_t    unused_ver,
    uint8_t     default_value,
    const char *enable_flag_1,
    const char *disable_flag_1,
    const char *enable_flag_2,
    const char *disable_flag_2
);

uint32_t read_cmdline_int_value(
    const char *unused_name,
    uint32_t    unused_fallback,
    uint32_t    default_value,
    const char *flag_to_search
);

/* Normalize a path: forward slashes, lowercase. Allocates via tak_strdup;
   caller must release with tak_free. Returns NULL on failure or NULL input.
   Defined in src/core/util.c. */
char *normalize_path(const char *path);

/* Case-insensitive string compare, ASCII-only and locale-independent.
 * Portable replacement for MSVC stricmp / POSIX strcasecmp.
 * Returns <0, 0, >0 like strcmp. */
static inline int tak_stricmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        unsigned char la = (ca >= 'A' && ca <= 'Z') ? (unsigned char)(ca + 32) : ca;
        unsigned char lb = (cb >= 'A' && cb <= 'Z') ? (unsigned char)(cb + 32) : cb;
        if (la != lb) return (int)la - (int)lb;
        if (ca == 0) return 0;
    }
}

/* Bounded case-insensitive string compare, ASCII-only and locale-independent.
 * Portable replacement for MSVC strnicmp / POSIX strncasecmp. */
static inline int tak_strnicmp(const char *a, const char *b, size_t n) {
    while (n--) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        unsigned char la = (ca >= 'A' && ca <= 'Z') ? (unsigned char)(ca + 32) : ca;
        unsigned char lb = (cb >= 'A' && cb <= 'Z') ? (unsigned char)(cb + 32) : cb;
        if (la != lb) return (int)la - (int)lb;
        if (ca == 0) return 0;
    }
    return 0;
}

#endif
