#ifndef TAK_MAP_SHARE_H
#define TAK_MAP_SHARE_H

#include <stddef.h>
#include <stdint.h>

#include "tak_map_fingerprint.h"

/* A room's map sent from the host to a player who lacks it, protocol 11.
 *
 * What goes is a bundle of the map's own files, the four its fingerprint
 * covers and the blurb beside it, compressed. The player it comes to
 * checks the fingerprint of what came against the room's before anything
 * is written, then keeps it as a map pack of its own in
 * Paths_ReceivedMapDir, which the VFS mounts after the game's own packs.
 *
 * Only a map a player made is sent. One the game, the expansion or
 * Cavedog's map packs ship is known by its fingerprint, or by coming out
 * of one of the game folder's own .hpi archives, and a player without it
 * is told what they need instead. */

/* The blurb is the one part the fingerprint leaves out, so it stays small. */
#define TAK_MAP_SHARE_TXT_MAX  4096u
/* The most a bundle may inflate to. */
#define TAK_MAP_SHARE_RAW_MAX  (16u << 20)

typedef struct TAK_MapShareFiles {
    TAK_MapFiles map;              /* tnt, ota, crt, tdf */
    const void  *txt;
    size_t       txt_size;
    uint8_t     *owned;            /* what the pointers above point into */
} TAK_MapShareFiles;

/* The bundle of files in memory, compressed, in a buffer the caller frees
 * with tak_free. 0 on success, -2 when it is over TAK_NET_MAP_MAX. */
int  TAK_MapShare_Pack(const TAK_MapShareFiles *files, uint8_t **out, uint32_t *out_len);

/* An installed map's bundle, read through the VFS as its fingerprint is.
 * -2 over the size, -3 when what was read is not `want`, -1 otherwise. */
int  TAK_MapShare_PackInstalled(const char *map_key,
                                const uint8_t want[TAK_MAP_FINGERPRINT_BYTES],
                                uint8_t **out, uint32_t *out_len);

/* Read a bundle that came over the wire: inflate it, read its parts and
 * check they are the map `want`. 0 with *out filled, freed with
 * TAK_MapShare_FreeFiles. -1 for a bundle that does not read, -2 for one
 * that is some other map. */
int  TAK_MapShare_Unpack(const uint8_t *wire, uint32_t len,
                         const uint8_t want[TAK_MAP_FINGERPRINT_BYTES],
                         TAK_MapShareFiles *out);
void TAK_MapShare_FreeFiles(TAK_MapShareFiles *f);

/* What a host says of a map it has, a TAK_NetMapShare. Builds the bundle
 * to learn its size, and hands it over in *bundle when the answer is
 * TAK_MAPSHARE_OFFERED and bundle is not NULL. */
int  TAK_MapShare_Status(const char *map_key, const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES],
                         uint8_t **bundle, uint32_t *bundle_len);

/* Which of the original's releases ships the map with this fingerprint:
 * TAK_MAPSHARE_GAME, _EXPANSION or _MAP_PACK, or 0 for none. */
int  TAK_MapShare_Shipped(const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES]);

/* Whether a room's map name can name a file: ASCII letters, digits,
 * spaces and a few marks, no separators, no device names. */
int  TAK_MapShare_NameOk(const char *name);

/* Whether this player holds a map of that name that did not come from
 * another player. A received map is never put over one of theirs. */
int  TAK_MapShare_HasOwn(const char *map_key);

/* Write a checked map as <name>.kmp in dir, Paths_ReceivedMapDir with
 * its separator, over a received one of that name, and mount it now. 0
 * on success. */
int  TAK_MapShare_Keep(const char *dir, const char *map_key, const TAK_MapShareFiles *files);

/* Write files as a map pack, kmap/<name>.<ext> uncompressed, the format
 * the original reads from its Maps folder. 0 on success. */
int  TAK_MapShare_WritePack(const char *path, const char *map_key,
                            const TAK_MapShareFiles *files, uint32_t date);

/* What a player without the room's map is told, given the host's word on
 * it, a TAK_NetMapShare, and the name the room shows. */
void TAK_MapShare_Advice(int share, const char *shown, const char *map_key,
                         char *out, size_t cap);

#endif /* TAK_MAP_SHARE_H */
