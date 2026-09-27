#ifndef TAK_HPI_WRITE_H
#define TAK_HPI_WRITE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Writing an archive in the format TA:Kingdoms reads, the one its .hpi
 * and .kmp files are in: the HAPI header of version 2, a directory
 * block and a name block each compressed as one SQSH chunk, and every
 * file as zlib SQSH chunks of at most 64 KiB. A map pack is one of
 * these with the map's files under kmap/.
 */

typedef struct HPIPackFile {
    const char *path;       /* inside the archive, '/' between folders */
    const void *data;
    uint32_t    size;
} HPIPackFile;

/* Write `count` files to `out_path`, stamped with `date` (seconds since
 * 1970, which is how an archive ranks copies of one file). 0 on
 * success, -1 with the reason in err. */
int HPI_WritePack(const char *out_path, const HPIPackFile *files, int count,
                  uint32_t date, char *err, size_t err_cap);

#endif /* TAK_HPI_WRITE_H */
