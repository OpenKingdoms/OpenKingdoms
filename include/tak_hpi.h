#include "tak_types.h"

// Opaque types
typedef struct HPIArchive HPIArchive;
typedef struct HPIFile HPIFile;

#define HPI_MAGIC       0x49504148  /* "HAPI" */
#define HPI_VERSION_V1  0x00010000  /* Total Annihilation */
#define HPI_VERSION_V2  0x00020000  /* TA:Kingdoms */
#define HPI_SAVEGAME    0x4B4E4142  /* "BANK" -- saved games, skip these */
#define HPI_SQSH_MARKER 0x48535153  /* "SQSH" -- compressed chunk marker */

#pragma pack(push, 1)

typedef struct HPIHeader_V1 {
    uint32_t directory_size;  /* Total size of directory (includes header) */
    uint32_t key;             /* XOR decryption key (0 = unencrypted) */
    uint32_t start;           /* File offset where directory begins (usually 0x14) */
} HPIHeader_V1;

/* At the start offset: a count + pointer to entry list */
/* int32_t entry_count; int32_t entry_list_offset; */

typedef struct HPIEntry_V1 {
    uint32_t name_offset;     /* Offset to null-terminated name (within directory buffer) */
    uint32_t data_offset;     /* If dir: offset to count/pointer pair. If file: offset to HPIFileData_V1 */
    uint8_t  is_directory;    /* 1 = directory, 0 = file */
} HPIEntry_V1;

typedef struct HPIFileData_V1 {
    uint32_t data_offset;     /* Offset to file data in the archive */
    uint32_t file_size;       /* Decompressed file size */
    uint8_t  compression;     /* 0 = none, 1 = LZ77, 2 = zlib */
} HPIFileData_V1;

typedef struct HPIVersion {
    uint32_t marker;      /* Must be HPI_MAGIC */
    uint32_t version;     /* HPI_VERSION_V1, HPI_VERSION_V2, or HPI_SAVEGAME */
} HPIVersion;

typedef struct HPIHeader_V2 {
    uint32_t dir_block;       /* Offset to directory block (may be SQSH-compressed) */
    uint32_t dir_size;        /* Decompressed size of directory block */
    uint32_t name_block;      /* Offset to name block (may be SQSH-compressed) */
    uint32_t name_size;       /* Decompressed size of name block */
    uint32_t data;            /* Start of file data area (usually 0x20) */
    uint32_t last78;          /* Offset to last 78 bytes (copyright info), or 0 */
} HPIHeader_V2;

typedef struct HPIDir_V2 {
    uint32_t name_ptr;        /* Offset into name_block for this directory's name */
    uint32_t first_subdir;    /* Offset into dir_block of first subdirectory entry */
    uint32_t subdir_count;    /* Number of subdirectory entries */
    uint32_t first_file;      /* Offset into dir_block of first file entry */
    uint32_t file_count;      /* Number of file entries */
} HPIDir_V2;

typedef struct HPIFileEntry_V2 {
    uint32_t name_ptr;            /* Offset into name_block */
    uint32_t start;               /* Offset to file data in the archive */
    uint32_t decompressed_size;   /* Final decompressed size */
    uint32_t compressed_size;     /* Total compressed size (0 = uncompressed) */
    uint32_t date;                /* File date as time_t */
    uint32_t checksum;            /* 4 checksums packed into 1 uint32 */
} HPIFileEntry_V2;

typedef struct HPIChunk {
    uint32_t marker;              /* Must be HPI_SQSH_MARKER ("SQSH") */
    uint8_t  version;             /* Always 0x02 */
    uint8_t  compression_method;  /* 1 = LZ77, 2 = zlib */
    uint8_t  encrypted;           /* 1 = chunk data is encrypted */
    uint32_t compressed_size;     /* Size of compressed data following this header */
    uint32_t decompressed_size;   /* Size after decompression */
    uint32_t checksum;            /* Sum of all unsigned bytes in compressed data */
} HPIChunk;

#pragma pack(pop)

typedef struct HPIFileRecord {
    char *path;                   /* Normalized full path (e.g. "units/aramon/acolyte.fbi") */
    uint32_t data_offset;         /* Offset in the archive file to the data/chunks */
    uint32_t decompressed_size;   /* Final file size */
    uint32_t compressed_size;     /* 0 = uncompressed (v2), or compression flag (v1) */
    uint8_t  compression;         /* 0/1/2 for v1; derived from compressed_size for v2 */
} HPIFileRecord;

// ---- Single-archive operations ----

// Open an HPI archive file. Returns NULL on failure.
// Reads and parses the header and full directory tree into memory.
// Handles both v1 and v2 formats automatically.
HPIArchive *HPI_OpenArchive(const char *filepath);

// Close an archive and free all associated memory.
void HPI_CloseArchive(HPIArchive *archive);

// Get the format version of an opened archive (returns HPI_VERSION_V1 or HPI_VERSION_V2).
uint32_t HPI_GetVersion(const HPIArchive *archive);

// Check if a file path exists in this archive.
// Path uses forward slashes, lookup is case-insensitive.
int HPI_FileExists(const HPIArchive *archive, const char *path);

// Read a file from this archive into a newly allocated buffer.
// Caller owns the buffer and must free it with HPI_FreeBuffer().
// On success: returns 0, sets *out_data and *out_size.
// On failure: returns -1.
int HPI_ReadFile(const HPIArchive *archive, const char *path,
                 void **out_data, uint32_t *out_size);

// Free a buffer returned by HPI_ReadFile.
void HPI_FreeBuffer(void *data);

// Get the number of file entries in this archive.
unsigned int HPI_GetEntryCount(const HPIArchive *archive);

// List all files matching a glob pattern within this archive.
// Pattern uses forward slashes, matching is case-insensitive.
// Supports * and ? wildcards.
// Returns 0 on success, fills *out_paths (caller frees each string + the array).
int HPI_ListFiles(const HPIArchive *archive, const char *pattern,
                  char ***out_paths, int *out_count);

// ---- VFS (multi-archive + loose files) ----

// Initialize the VFS. Scans game_dir for *.hpi files, opens them all.
// Archives are loaded in alphabetical order; last-loaded wins on conflicts.
// If loose_dir is non-NULL, loose files are checked as a LAST resort
// (HPI archives take priority, matching original engine behavior).
// Returns 0 on success, -1 on failure.
int VFS_Init(const char *game_dir, const char *loose_dir);

// Shut down the VFS. Closes all archives, frees all memory.
void VFS_Shutdown(void);

// 1 between a successful VFS_Init and VFS_Shutdown, else 0.
int VFS_IsInitialized(void);

// Get the number of loaded archives.
int VFS_GetArchiveCount(void);

// Read a file from the VFS (searches all archives, last-loaded first).
// Path uses forward slashes, lookup is case-insensitive.
// Returns 0 on success, -1 if not found.
int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size);

// Check if a file exists in any loaded archive (or loose dir).
int VFS_FileExists(const char *path);

// List all files matching a glob across all loaded archives + loose dir.
// Deduplicates results (last-loaded archive's version wins).
int VFS_ListFiles(const char *pattern, char ***out_paths, int *out_count);

// Free a buffer returned by VFS_ReadFile.
void VFS_FreeBuffer(void *data);

int hpi_decompress_chunk(
    const uint8_t *chunk_data,
    uint32_t chunk_total_size,
    uint8_t *out_buf,
    uint32_t out_buf_size
);