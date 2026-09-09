/*
 * probe_cob.c — Phase D M1 recon. Dumps the structure of a single
 * .cob file so we can pin down the header layout empirically before
 * committing to a parser.
 *
 * The TAK COB header has a known shape inherited from TA COB v6, but
 * source documentation conflicts on field order. We probe instead.
 *
 * Usage: probe_cob [vfs_path]   (default: scripts/araking.cob)
 *
 * Output is annotated and verbose; pipe to a file for analysis.
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static int looks_like_string(const uint8_t *buf, uint32_t off, uint32_t buf_size) {
    if (off >= buf_size) return 0;
    int len = 0;
    for (uint32_t i = off; i < buf_size && i < off + 64; i++) {
        uint8_t c = buf[i];
        if (c == 0) return len > 0 && len < 64;
        if (c < 0x20 || c > 0x7e) return 0;
        len++;
    }
    return 0;
}

static const char *str_at(const uint8_t *buf, uint32_t off, uint32_t buf_size) {
    if (looks_like_string(buf, off, buf_size)) {
        return (const char *)(buf + off);
    }
    return NULL;
}

static void dump_offset_table(const char *label, const uint8_t *buf,
                               uint32_t buf_size, uint32_t table_off,
                               uint32_t count, int as_strings) {
    printf("--- %s @ 0x%08x (%u entries)\n", label, table_off, count);
    if (table_off + count * 4 > buf_size) {
        printf("    (out of bounds)\n");
        return;
    }
    for (uint32_t i = 0; i < count && i < 50; i++) {
        uint32_t v = *(const uint32_t *)(buf + table_off + i * 4);
        if (as_strings) {
            const char *s = str_at(buf, v, buf_size);
            if (s) {
                printf("    [%2u] 0x%08x -> \"%s\"\n", i, v, s);
            } else {
                printf("    [%2u] 0x%08x -> (not a string at this offset)\n", i, v);
            }
        } else {
            printf("    [%2u] 0x%08x\n", i, v);
        }
    }
    if (count > 50) printf("    ... %u more\n", count - 50);
}

int main(int argc, char **argv) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    const char *path = (argc > 1) ? argv[1] : "scripts/araking.cob";

    void *raw = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &raw, &size) != 0 || !raw) {
        fprintf(stderr, "Failed to read %s\n", path);
        return 1;
    }

    const uint8_t *buf = (const uint8_t *)raw;
    printf("=== %s (%u bytes) ===\n\n", path, size);

    /* Dump first 64 bytes raw, annotated as uint32s. */
    printf("--- Header (first 13 uint32s)\n");
    for (uint32_t i = 0; i < 13 && i * 4 < size; i++) {
        uint32_t v = *(const uint32_t *)(buf + i * 4);
        printf("    +0x%02x: 0x%08x  (%u)\n", i * 4, v, v);
    }
    printf("\n");

    /* Hypothesis: TAK COB v6 header layout
     *   0x00  version
     *   0x04  ?  (one of: num_scripts, num_pieces)
     *   0x08  ?  (the other)
     *   0x0c  code_length_in_words
     *   0x10  num_static_vars
     *   0x14  reserved
     *   0x18  offset of script-offsets array
     *   0x1c  offset of script-name pointers array
     *   0x20  offset of piece-name pointers array
     *   0x24  offset of code (typically 52)
     *   0x28  ? (file length / sound name pointers)
     *   0x2c  ? (sound name count?)
     *
     * Probe both interpretations to see which produces sensible names.
     */
    uint32_t v04 = *(const uint32_t *)(buf + 0x04);
    uint32_t v08 = *(const uint32_t *)(buf + 0x08);
    uint32_t off_script_offsets = *(const uint32_t *)(buf + 0x18);
    uint32_t off_script_names   = *(const uint32_t *)(buf + 0x1c);
    uint32_t off_piece_names    = *(const uint32_t *)(buf + 0x20);
    uint32_t off_code           = *(const uint32_t *)(buf + 0x24);

    printf("Probing as if v04=num_scripts (%u), v08=num_pieces (%u):\n\n", v04, v08);

    dump_offset_table("script entry-point byte offsets",
                       buf, size, off_script_offsets, v04, 0);
    dump_offset_table("script name pointers (target = string?)",
                       buf, size, off_script_names, v04, 1);
    dump_offset_table("piece name pointers (target = string?)",
                       buf, size, off_piece_names, v08, 1);

    /* If we got "Create" / "Killed" out of script names, hypothesis is right. */
    printf("\nProbing the SWAPPED interpretation (v04=num_pieces, v08=num_scripts):\n\n");
    dump_offset_table("if v04=pieces: piece-name pointers (target = string?)",
                       buf, size, off_piece_names, v04, 1);
    dump_offset_table("if v08=scripts: script-name pointers (target = string?)",
                       buf, size, off_script_names, v08, 1);

    printf("\ncode region: starts @ 0x%08x, length %u words = %u bytes (ends @ 0x%08x)\n",
           off_code,
           *(const uint32_t *)(buf + 0x0c),
           *(const uint32_t *)(buf + 0x0c) * 4,
           off_code + *(const uint32_t *)(buf + 0x0c) * 4);

    tak_free(raw);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
