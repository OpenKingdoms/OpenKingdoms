/*
 * test_hpi_write.c -- an archive written by HPI_WritePack reads back
 * through the engine's own reader, file for file. Data free.
 */
#include "test_framework.h"
#include "tak_hpi.h"
#include "tak_hpi_write.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK "test_hpi_write.kmp"

static uint8_t big[200000];

static int read_back(HPIArchive *a, const char *path, const void *want, uint32_t size) {
    void *data = NULL;
    uint32_t got = 0;
    if (HPI_ReadFile(a, path, &data, &got) != 0) return 0;
    int same = got == size && memcmp(data, want, size) == 0;
    HPI_FreeBuffer(data);
    return same;
}

TEST(a_pack_reads_back_file_for_file) {
    const char *ota = "[GlobalHeader]\n{\nmissionname=Test;\n}\n";
    uint32_t seed = 7;
    for (size_t i = 0; i < sizeof big; i++) {
        seed = seed * 1103515245u + 12345u;
        big[i] = (uint8_t)(i < sizeof big / 2 ? (seed >> 16) : (uint8_t)(i / 97));
    }
    const uint8_t empty = 0;
    HPIPackFile files[] = {
        { "kmap/test map.ota", ota, (uint32_t)strlen(ota) },
        { "kmap/test map.tnt", big, (uint32_t)sizeof big },
        { "kmap/sub/deeper.txt", "deep", 4 },
        { "readme.txt", "hello", 5 },
        { "kmap/empty.dat", &empty, 0 },
    };
    char err[128];
    ASSERT_EQ_INT(0, HPI_WritePack(PACK, files, 5, 1234567890u, err, sizeof err));
    HPIArchive *a = HPI_OpenArchive(PACK);
    ASSERT_NOT_NULL(a);
    ASSERT(HPI_GetVersion(a) == HPI_VERSION_V2);
    ASSERT(read_back(a, "kmap/test map.ota", ota, (uint32_t)strlen(ota)));
    ASSERT(read_back(a, "kmap/test map.tnt", big, (uint32_t)sizeof big));
    ASSERT(read_back(a, "kmap/sub/deeper.txt", "deep", 4));
    ASSERT(read_back(a, "readme.txt", "hello", 5));
    ASSERT(read_back(a, "KMAP/Test Map.OTA", ota, (uint32_t)strlen(ota)));
    void *data = NULL;
    uint32_t size = 0;
    ASSERT_EQ_INT(-1, HPI_ReadFile(a, "kmap/missing.ota", &data, &size));
    HPI_CloseArchive(a);
    remove(PACK);
}

TEST(a_pack_refuses_a_name_twice_and_nothing_at_all) {
    char err[128];
    HPIPackFile twice[] = { { "a/b.txt", "1", 1 }, { "A/B.TXT", "2", 1 } };
    ASSERT_EQ_INT(-1, HPI_WritePack(PACK, twice, 2, 0, err, sizeof err));
    ASSERT(err[0]);
    ASSERT_EQ_INT(-1, HPI_WritePack(PACK, NULL, 0, 0, err, sizeof err));
    remove(PACK);
}

int main(void) {
    TEST_SUITE("hpi_write");
    RUN(a_pack_reads_back_file_for_file);
    RUN(a_pack_refuses_a_name_twice_and_nothing_at_all);
    TEST_REPORT();
}
