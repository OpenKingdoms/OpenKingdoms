/**
 * HPI v2 format - HPI is a proprietary archive format created by, I believe, Cavedog Entertainment
 * Let's implement an API for opening/saving/and doing all of the fun, HPI things!
 */

 #include "tak_hpi.h"
 #include "tak_io.h"
 #include "tak_util.h"
 #include "tak_memory.h"
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <ctype.h>

typedef struct HPIArchive {
    tak_file_t handle;
    HPIVersion version;
    HPIFileRecord *records;
    unsigned int record_count;
    char *decrypt_key; // Only for V1
} HPIArchive;

typedef struct HPIFile HPIFile;

/* qsort/bsearch comparator */
int compare_record(const void *a, const void *b) {
    const HPIFileRecord *ra = (const HPIFileRecord *)a;
    const HPIFileRecord *rb = (const HPIFileRecord *)b;
    return tak_stricmp(ra->path, rb->path);
}

// ── Two-pass directory walk ─────────────────────────────────────────────────
// Pass 1: count total files in the directory tree (no allocation).
// Pass 2: fill the pre-allocated records array.

static size_t count_v2_files(const uint8_t *dir_block, size_t dir_offset) {
    HPIDir_V2 dir;
    memcpy(&dir, dir_block + dir_offset, sizeof(HPIDir_V2));

    size_t count = dir.file_count;
    for (uint32_t i = 0; i < dir.subdir_count; i++) {
        count += count_v2_files(dir_block, dir.first_subdir + i * sizeof(HPIDir_V2));
    }
    return count;
}

static void fill_v2_records(
    HPIFileRecord *records,
    size_t *write_index,
    const uint8_t *dir_block,
    const uint8_t *name_block,
    size_t dir_offset,
    const char *parent_path
) {
    HPIDir_V2 dir;
    memcpy(&dir, dir_block + dir_offset, sizeof(HPIDir_V2));

    // Process files in this directory
    for (uint32_t i = 0; i < dir.file_count; i++) {
        HPIFileEntry_V2 entry;
        memcpy(&entry, dir_block + dir.first_file + i * sizeof(HPIFileEntry_V2),
               sizeof(HPIFileEntry_V2));

        const char *filename = (const char *)(name_block + entry.name_ptr);
        char path_buf[512];

        if (parent_path[0] == '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s", filename);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", parent_path, filename);
        }

        HPIFileRecord *rec = &records[*write_index];
        rec->path = normalize_path(path_buf);
        rec->data_offset = entry.start;
        rec->decompressed_size = entry.decompressed_size;
        rec->compressed_size = entry.compressed_size;
        rec->date = entry.date;
        rec->compression = entry.compressed_size > 0 ? 1 : 0;
        (*write_index)++;
    }

    // Recurse into subdirectories
    for (uint32_t i = 0; i < dir.subdir_count; i++) {
        size_t sub_offset = dir.first_subdir + i * sizeof(HPIDir_V2);
        HPIDir_V2 subdir;
        memcpy(&subdir, dir_block + sub_offset, sizeof(HPIDir_V2));

        const char *dirname = (const char *)(name_block + subdir.name_ptr);
        char path_buf[512];

        if (parent_path[0] == '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s", dirname);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", parent_path, dirname);
        }

        fill_v2_records(records, write_index, dir_block, name_block, sub_offset, path_buf);
    }
}

// ── Block reading helper ────────────────────────────────────────────────────
// Reads a v2 block (dir or name) from the archive. If the block is
// SQSH-compressed, decompresses it. Returns a malloc'd buffer the caller
// must free, or NULL on failure. Sets *out_size to the usable buffer size.

static uint8_t *read_v2_block(tak_file_t handle, uint32_t block_offset,
                              uint32_t on_disk_size, uint32_t *out_size) {
    HPIChunk chunk;
    tak_pread(handle, &chunk, sizeof(HPIChunk), block_offset);

    if (chunk.marker == HPI_SQSH_MARKER) {
        uint8_t *raw = (uint8_t *)tak_malloc_named("hpi.raw_chunk", on_disk_size);
        uint8_t *out = (uint8_t *)tak_malloc_named("hpi.v2_block", chunk.decompressed_size);
        if (!raw || !out) { tak_free(raw); tak_free(out); return NULL; }

        tak_pread(handle, raw, on_disk_size, block_offset);
        int result = hpi_decompress_chunk(raw, on_disk_size,
                                          out, chunk.decompressed_size);
        tak_free(raw);
        if (result < 0) { tak_free(out); return NULL; }

        *out_size = chunk.decompressed_size;
        return out;
    } else {
        uint8_t *out = (uint8_t *)tak_malloc_named("hpi.v2_block", on_disk_size);
        if (!out) return NULL;

        tak_pread(handle, out, on_disk_size, block_offset);
        *out_size = on_disk_size;
        return out;
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

HPIArchive *HPI_OpenArchive(const char *filepath) {
    tak_file_t handle = tak_file_open(filepath);
    if (handle == TAK_INVALID_FILE) return NULL;

    HPIArchive *out = (HPIArchive *)tak_calloc(1, sizeof(HPIArchive));
    if (!out) { tak_file_close(handle); return NULL; }

    HPIVersion version;
    tak_pread(handle, &version, sizeof(HPIVersion), 0);

    out->version = version;
    out->handle = handle;

    if (version.version == HPI_VERSION_V2) {
        HPIHeader_V2 header;
        tak_pread(handle, &header, sizeof(HPIHeader_V2), sizeof(HPIVersion));

        // Read and decompress the directory and name blocks
        uint32_t dir_size = 0, name_size = 0;
        uint8_t *dir_block = read_v2_block(handle, header.dir_block,
                                           header.dir_size, &dir_size);
        uint8_t *name_block = read_v2_block(handle, header.name_block,
                                            header.name_size, &name_size);
        if (!dir_block || !name_block) {
            tak_free(dir_block);
            tak_free(name_block);
            return out;
        }

        // Pass 1: count files
        size_t total_files = count_v2_files(dir_block, 0);

        out->records = (HPIFileRecord *)tak_calloc(total_files, sizeof(HPIFileRecord));
        if (!out->records) {
            tak_free(dir_block);
            tak_free(name_block);
            return out;
        }

        // Pass 2: fill records
        size_t write_idx = 0;
        fill_v2_records(out->records, &write_idx, dir_block, name_block, 0, "");
        out->record_count = (unsigned int)write_idx;

        // Sort for bsearch lookups
        qsort(out->records, out->record_count, sizeof(HPIFileRecord), compare_record);

        tak_free(dir_block);
        tak_free(name_block);
    } else if (version.version == HPI_VERSION_V1) {
        // V1 support not yet implemented
    }

    return out;
}

void HPI_CloseArchive(HPIArchive *archive) {
    if (!archive) return;

    if (archive->records) {
        for (unsigned int i = 0; i < archive->record_count; i++) {
            tak_free(archive->records[i].path);
        }
        tak_free(archive->records);
    }

    tak_free(archive->decrypt_key);

    if (archive->handle != TAK_INVALID_FILE) {
        tak_file_close(archive->handle);
    }

    tak_free(archive);
}

uint32_t HPI_GetVersion(const HPIArchive *archive) {
    return archive ? archive->version.version : (uint32_t)-1;
}

int HPI_FileExists(const HPIArchive *archive, const char *path) {
    if (!archive || !path) return 0;

    char *norm = normalize_path(path);
    if (!norm) return 0;

    HPIFileRecord key;
    key.path = norm;
    HPIFileRecord *found = bsearch(&key, archive->records, archive->record_count,
                                   sizeof(HPIFileRecord), compare_record);
    tak_free(norm);
    return found ? 1 : 0;
}

int HPI_FindFile(const HPIArchive *archive, const char *path,
                 uint32_t *out_date) {
    if (out_date) *out_date = 0;
    if (!archive || !path) return 0;

    char *norm = normalize_path(path);
    if (!norm) return 0;

    HPIFileRecord key;
    key.path = norm;
    HPIFileRecord *found = bsearch(&key, archive->records, archive->record_count,
                                   sizeof(HPIFileRecord), compare_record);
    tak_free(norm);
    if (!found) return 0;
    if (out_date) *out_date = found->date;
    return 1;
}

unsigned int HPI_GetEntryCount(const HPIArchive *archive) {
    return archive ? archive->record_count : 0;
}

int HPI_ListFiles(const HPIArchive *archive, const char *pattern,
                  char ***out_paths, int *out_count) {
    if (!archive || !pattern || !out_paths || !out_count) return -1;

    char *norm_pattern = normalize_path(pattern);
    if (!norm_pattern) return -1;

    /* Guard against zero-size allocation when the archive is empty. */
    if (archive->record_count == 0) {
        tak_free(norm_pattern);
        *out_paths = NULL;
        *out_count = 0;
        return 0;
    }

    char **matches = (char **)tak_malloc_named("hpi.list_matches",
                                               sizeof(char *) * archive->record_count);
    if (!matches) { tak_free(norm_pattern); return -1; }

    int count = 0;
    for (unsigned int i = 0; i < archive->record_count; i++) {
        if (glob_path_match(norm_pattern, archive->records[i].path)) {
            matches[count] = tak_strdup(archive->records[i].path);
            if (!matches[count]) {
                for (int j = 0; j < count; j++) tak_free(matches[j]);
                tak_free(matches);
                tak_free(norm_pattern);
                *out_paths = NULL;
                *out_count = 0;
                return -1;
            }
            count++;
        }
    }

    tak_free(norm_pattern);

    if (count == 0) {
        tak_free(matches);
        *out_paths = NULL;
        *out_count = 0;
        return 0;
    }

    char **shrunk = (char **)tak_realloc(matches, count * sizeof(char *));
    *out_paths = shrunk ? shrunk : matches;  /* realloc failure is non-fatal */
    *out_count = count;
    return 0;
}

int HPI_ReadFile(const HPIArchive *archive, const char *path,
                 void **out_data, uint32_t *out_size) {
    if (!archive || !path || !out_data || !out_size) return -1;

    char *norm = normalize_path(path);
    if (!norm) return -1;

    HPIFileRecord key;
    key.path = norm;
    HPIFileRecord *rec = bsearch(&key, archive->records, archive->record_count,
                                 sizeof(HPIFileRecord), compare_record);
    tak_free(norm);
    if (!rec) return -1;

    const uint32_t file_size = rec->decompressed_size;
    const uint32_t offset = rec->data_offset;

    if (rec->compressed_size == 0) {
        // Uncompressed: read directly
        uint8_t *buf = (uint8_t *)tak_malloc_named("hpi.file_data", file_size);
        if (!buf) return -1;
        tak_pread(archive->handle, buf, file_size, offset);
        *out_data = buf;
        *out_size = file_size;
    } else {
        // Compressed: read SQSH chunks until we have all decompressed bytes
        uint8_t *buf = (uint8_t *)tak_malloc_named("hpi.file_data", file_size);
        if (!buf) return -1;

        uint32_t bytes_written = 0;
        size_t read_offset = 0;

        while (bytes_written < file_size) {
            HPIChunk chunk_hdr;
            tak_pread(archive->handle, &chunk_hdr, sizeof(HPIChunk),
                      offset + read_offset);

            uint32_t chunk_total = sizeof(HPIChunk) + chunk_hdr.compressed_size;
            uint8_t *chunk_buf = (uint8_t *)tak_malloc_named("hpi.raw_chunk", chunk_total);
            if (!chunk_buf) { tak_free(buf); return -1; }

            tak_pread(archive->handle, chunk_buf, chunk_total,
                      offset + read_offset);

            int result = hpi_decompress_chunk(chunk_buf, chunk_total,
                                              buf + bytes_written,
                                              chunk_hdr.decompressed_size);
            tak_free(chunk_buf);
            if (result < 0) { tak_free(buf); return -1; }

            bytes_written += chunk_hdr.decompressed_size;
            read_offset += chunk_total;
        }

        *out_data = buf;
        *out_size = file_size;
    }

    return 0;
}

void HPI_FreeBuffer(void *data) {
    tak_free(data);
}
