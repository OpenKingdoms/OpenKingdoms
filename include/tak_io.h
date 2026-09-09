#ifndef TAK_IO_H
#define TAK_IO_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE tak_file_t;
#define TAK_INVALID_FILE INVALID_HANDLE_VALUE
#else
typedef int tak_file_t;
#define TAK_INVALID_FILE (-1)
#endif

/* Open a file for reading. Returns TAK_INVALID_FILE on failure. */
tak_file_t tak_file_open(const char *path);

/* Close a file handle. Safe to call with TAK_INVALID_FILE. */
void tak_file_close(tak_file_t f);

/* Read `count` bytes from file at `offset` without changing the file position.
   Thread-safe: multiple threads can call this concurrently on the same handle.
   Returns number of bytes read, or -1 on error. */
int64_t tak_pread(tak_file_t f, void *buf, size_t count, uint64_t offset);

#endif /* TAK_IO_H */
