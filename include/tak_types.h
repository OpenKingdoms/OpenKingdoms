#ifndef TAK_TYPES_H
#define TAK_TYPES_H

/*
 * tak_types.h — Portable type aliases for the TAK reverse-engineering project
 *
 * These are shorthand types that appear throughout the legacy reference.
 * They map directly to standard C types — no platform headers needed.
 *
 * The reference export redefined hundreds of Windows SDK types
 * (HWND, RECT, CRITICAL_SECTION, IMAGE_DOS_HEADER, ...).  Those belong
 * to <windows.h> on Windows and to platform-abstraction headers elsewhere;
 * they are NOT reproduced here.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Fixed-width shorthand (reference naming conventions) ───────────────── */
typedef uint8_t     byte;
typedef uint16_t    word;
typedef uint32_t    dword;

/* ── Convenient C shorthand ──────────────────────────────────────────── */
typedef unsigned char       uchar;
typedef unsigned short      ushort;
typedef unsigned int        uint;
typedef unsigned long       ulong;

/* ── Reference pointer / address types (original binary is 32-bit x86) ── */
typedef uint32_t    pointer32;
typedef pointer32   ImageBaseOffset32;

/* ── Wide-char alias (original engine uses 16-bit wchar) ─────────────  */
typedef uint16_t    wchar16;

#endif /* TAK_TYPES_H */
