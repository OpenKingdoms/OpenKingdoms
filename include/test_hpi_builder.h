#ifndef TEST_HPI_BUILDER_H
#define TEST_HPI_BUILDER_H

/* Writes a small uncompressed HPI v2 archive so tests can exercise the
 * archive and VFS code without shipping game content. One directory
 * level is enough for every fixture here. */

#include "tak_hpi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct TestHPIEntry {
    const char *path;   /* "name.ext" or "dir/name.ext" */
    const char *data;   /* file contents, null terminated */
    uint32_t    date;   /* entry date, the field the VFS ranks copies by */
} TestHPIEntry;

#define TEST_HPI_MAX_FILES 64
#define TEST_HPI_MAX_DIRS  8

/* Returns 0 on success. The caller removes the file when done. */
static int test_write_hpi(const char *out_path,
                          const TestHPIEntry *entries, int count) {
    if (!out_path || !entries || count <= 0 || count > TEST_HPI_MAX_FILES)
        return -1;

    char dir_names[TEST_HPI_MAX_DIRS][64];
    int  dir_count = 0;
    int  entry_dir[TEST_HPI_MAX_FILES];       /* -1 = root */
    const char *entry_name[TEST_HPI_MAX_FILES];

    for (int i = 0; i < count; i++) {
        const char *slash = strrchr(entries[i].path, '/');
        if (!slash) { entry_dir[i] = -1; entry_name[i] = entries[i].path; continue; }
        size_t dlen = (size_t)(slash - entries[i].path);
        if (dlen >= sizeof(dir_names[0])) return -1;
        int found = -1;
        for (int d = 0; d < dir_count; d++) {
            if (strlen(dir_names[d]) == dlen &&
                memcmp(dir_names[d], entries[i].path, dlen) == 0) { found = d; break; }
        }
        if (found < 0) {
            if (dir_count == TEST_HPI_MAX_DIRS) return -1;
            found = dir_count++;
            memcpy(dir_names[found], entries[i].path, dlen);
            dir_names[found][dlen] = '\0';
        }
        entry_dir[i] = found;
        entry_name[i] = slash + 1;
    }

    /* Name block: the root name, then directory names, then file names. */
    char name_block[8192];
    uint32_t name_size = 0;
    uint32_t root_name_ptr = name_size;
    name_block[name_size++] = '\0';
    uint32_t dir_name_ptr[TEST_HPI_MAX_DIRS];
    for (int d = 0; d < dir_count; d++) {
        dir_name_ptr[d] = name_size;
        size_t n = strlen(dir_names[d]) + 1;
        if (name_size + n > sizeof(name_block)) return -1;
        memcpy(name_block + name_size, dir_names[d], n);
        name_size += (uint32_t)n;
    }
    uint32_t file_name_ptr[TEST_HPI_MAX_FILES];
    for (int i = 0; i < count; i++) {
        file_name_ptr[i] = name_size;
        size_t n = strlen(entry_name[i]) + 1;
        if (name_size + n > sizeof(name_block)) return -1;
        memcpy(name_block + name_size, entry_name[i], n);
        name_size += (uint32_t)n;
    }

    /* Data area starts right after the two headers. */
    uint32_t data_start = (uint32_t)(sizeof(HPIVersion) + sizeof(HPIHeader_V2));
    uint32_t data_offset[TEST_HPI_MAX_FILES];
    uint32_t data_size = 0;
    for (int i = 0; i < count; i++) {
        data_offset[i] = data_start + data_size;
        data_size += (uint32_t)strlen(entries[i].data);
    }

    /* Directory block: the root record, one record per directory, then
     * the file entry arrays in the same order. */
    uint8_t dir_block[8192];
    uint32_t dir_size = 0;
    uint32_t dir_record_off = 0;
    dir_size += (uint32_t)sizeof(HPIDir_V2) * (uint32_t)(1 + dir_count);

    uint32_t file_array_off[TEST_HPI_MAX_DIRS + 1];
    uint32_t file_array_len[TEST_HPI_MAX_DIRS + 1];
    for (int d = -1; d < dir_count; d++) {
        int slot = d + 1;
        file_array_off[slot] = dir_size;
        file_array_len[slot] = 0;
        for (int i = 0; i < count; i++) {
            if (entry_dir[i] != d) continue;
            HPIFileEntry_V2 fe;
            memset(&fe, 0, sizeof(fe));
            fe.name_ptr = file_name_ptr[i];
            fe.start = data_offset[i];
            fe.decompressed_size = (uint32_t)strlen(entries[i].data);
            fe.compressed_size = 0;
            fe.date = entries[i].date;
            fe.checksum = 0;
            if (dir_size + sizeof(fe) > sizeof(dir_block)) return -1;
            memcpy(dir_block + dir_size, &fe, sizeof(fe));
            dir_size += (uint32_t)sizeof(fe);
            file_array_len[slot]++;
        }
    }

    HPIDir_V2 root;
    memset(&root, 0, sizeof(root));
    root.name_ptr = root_name_ptr;
    root.first_subdir = (uint32_t)sizeof(HPIDir_V2);
    root.subdir_count = (uint32_t)dir_count;
    root.first_file = file_array_off[0];
    root.file_count = file_array_len[0];
    memcpy(dir_block + dir_record_off, &root, sizeof(root));

    for (int d = 0; d < dir_count; d++) {
        HPIDir_V2 sub;
        memset(&sub, 0, sizeof(sub));
        sub.name_ptr = dir_name_ptr[d];
        sub.first_subdir = 0;
        sub.subdir_count = 0;
        sub.first_file = file_array_off[d + 1];
        sub.file_count = file_array_len[d + 1];
        memcpy(dir_block + sizeof(HPIDir_V2) * (size_t)(d + 1), &sub, sizeof(sub));
    }

    /* The reader peeks 19 bytes at a block to look for a compression
     * marker, so keep both blocks at least that long. */
    while (dir_size < 32) dir_block[dir_size++] = 0;
    while (name_size < 32) name_block[name_size++] = '\0';

    HPIVersion version;
    version.marker = HPI_MAGIC;
    version.version = HPI_VERSION_V2;

    HPIHeader_V2 header;
    memset(&header, 0, sizeof(header));
    header.name_block = data_start + data_size;
    header.name_size = name_size;
    header.dir_block = header.name_block + name_size;
    header.dir_size = dir_size;
    header.data = data_start;
    header.last78 = 0;

    FILE *fp = fopen(out_path, "wb");
    if (!fp) return -1;
    fwrite(&version, 1, sizeof(version), fp);
    fwrite(&header, 1, sizeof(header), fp);
    for (int i = 0; i < count; i++)
        fwrite(entries[i].data, 1, strlen(entries[i].data), fp);
    fwrite(name_block, 1, name_size, fp);
    fwrite(dir_block, 1, dir_size, fp);
    fclose(fp);
    return 0;
}

#endif /* TEST_HPI_BUILDER_H */
