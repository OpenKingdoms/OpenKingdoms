/*
 * Quick smoke test: open a real HPI archive, print what we find.
 * Re-declares HPIArchive internals for direct inspection (test-only hack).
 */
#include "tak_hpi.h"
#include "tak_io.h"
#include <stdio.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

/* Mirror the internal struct so we can inspect it from the test.
   This must match the definition in hpi.c exactly. */
typedef struct HPIArchiveInternal {
    tak_file_t handle;
    HPIVersion version;
    HPIFileRecord *records;
    unsigned int record_count;
    char *decrypt_key;
} HPIArchiveInternal;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : TAK_GAME_DIR "/data.hpi";
    fprintf(stderr, "Opening: %s\n", path);

    HPIArchive *archive = HPI_OpenArchive(path);
    if (!archive) {
        printf("FAIL: HPI_OpenArchive returned NULL\n");
        return 1;
    }

    /* Cast to our mirror struct to inspect internals */
    HPIArchiveInternal *a = (HPIArchiveInternal *)archive;

    printf("Version:  0x%08X\n", a->version.version);
    printf("Records:  %u\n", a->record_count);

    /* Print first 20 records */
    unsigned int limit = a->record_count < 20 ? a->record_count : 20;
    for (unsigned int i = 0; i < limit; i++) {
        HPIFileRecord *r = &a->records[i];
        printf("  [%4u] %-50s  offset=0x%08X  decomp=%u  comp=%u\n",
               i, r->path ? r->path : "(null)",
               r->data_offset, r->decompressed_size, r->compressed_size);
    }

    if (a->record_count > 20) {
        printf("  ... and %u more records\n", a->record_count - 20);
    }

    fprintf(stderr, "Closing archive...\n");
    HPI_CloseArchive(archive);
    fprintf(stderr, "Done.\n");
    return 0;
}
