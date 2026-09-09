/*
 * io.c — Cross-platform position-independent file I/O
 *
 * Provides tak_pread(): reads at a given offset without modifying
 * the file position, making it safe for concurrent access from
 * multiple threads on the same file handle.
 *
 * Windows:  CreateFileA / ReadFile with OVERLAPPED
 * POSIX:    open / pread
 */

#include "tak_io.h"

#ifdef _WIN32
/* ── Windows ─────────────────────────────────────────────────────── */

tak_file_t tak_file_open(const char *path) {
    if (!path) return TAK_INVALID_FILE;
    return CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
}

void tak_file_close(tak_file_t f) {
    if (f != TAK_INVALID_FILE) CloseHandle(f);
}

int64_t tak_pread(tak_file_t f, void *buf, size_t count, uint64_t offset) {
    if (f == TAK_INVALID_FILE || !buf || count == 0) return -1;

    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    DWORD bytes_read = 0;
    if (!ReadFile(f, buf, (DWORD)count, &bytes_read, &ov)) {
        return -1;
    }
    return (int64_t)bytes_read;
}

#else
/* ── POSIX ───────────────────────────────────────────────────────── */

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

tak_file_t tak_file_open(const char *path) {
    if (!path) return TAK_INVALID_FILE;
    return open(path, O_RDONLY);
}

void tak_file_close(tak_file_t f) {
    if (f != TAK_INVALID_FILE) close(f);
}

int64_t tak_pread(tak_file_t f, void *buf, size_t count, uint64_t offset) {
    if (f == TAK_INVALID_FILE || !buf || count == 0) return -1;

    ssize_t result = pread(f, buf, count, (off_t)offset);
    return (int64_t)result;
}

#endif
