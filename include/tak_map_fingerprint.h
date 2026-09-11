#ifndef TAK_MAP_FINGERPRINT_H
#define TAK_MAP_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#include "tak_sha256.h"

/* A map's content fingerprint: a SHA-256 over the parts of a map that
 * decide how a battle plays. Two players whose fingerprints match are
 * playing the same map, whether one has it in an archive, the other in a
 * map pack, and a third as loose files.
 *
 * What goes in, in this order:
 *   tnt   the .tnt file, byte for byte
 *   ota   the .ota in canonical form, without missionname and
 *         missiondescription, which are labels and change nothing
 *   crt   the .crt file byte for byte, when the map has one
 *   tdf   the map's .tdf in canonical form, when the map has one
 * The .txt is left out: it is the blurb shown beside the map.
 *
 * Canonical form of a TDF text, which is also what the map editor must
 * produce to keep a fingerprint stable across a save:
 *   - Blank lines and // comment lines are dropped.
 *   - Section and key names are lower cased, values are trimmed of
 *     leading and trailing blanks and otherwise kept as authored.
 *   - Sections keep the order the file gives them, because the engine
 *     spawns units and reads start positions in that order.
 *   - Keys inside a section are sorted by name, with two keys of the
 *     same name keeping their file order, because the parser looks keys
 *     up by name and takes the first.
 *   - Each section is written as "[name]", "{", its keys as
 *     "key=value", then its subsections, then "}", one per line.
 * The hashed stream is "OKMAP1", then for each part a line naming it
 * and its byte count, then the bytes, with "-" for a part the map does
 * not have.
 */

#define TAK_MAP_FINGERPRINT_BYTES TAK_SHA256_BYTES
#define TAK_MAP_FINGERPRINT_HEX   65

typedef struct TAK_MapFiles {
    const void *tnt;  size_t tnt_size;
    const void *ota;  size_t ota_size;
    const void *crt;  size_t crt_size;   /* NULL when the map has none */
    const void *tdf;  size_t tdf_size;   /* NULL when the map has none */
} TAK_MapFiles;

/* Fingerprint of files already in memory. Returns 0 on success, -1 when
 * the .tnt or the .ota is missing. */
int  TAK_MapFingerprint_FromFiles(const TAK_MapFiles *files,
                                  uint8_t out[TAK_MAP_FINGERPRINT_BYTES]);

/* Fingerprint of an installed map, found by the name the chooser uses.
 * Reads through the VFS, so archives, map packs and loose files all
 * work. Returns 0 on success. */
int  TAK_MapFingerprint_FromName(const char *map_key,
                                 uint8_t out[TAK_MAP_FINGERPRINT_BYTES]);

void TAK_MapFingerprint_ToHex(const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES],
                              char out[TAK_MAP_FINGERPRINT_HEX]);

/* The canonical text the fingerprint hashes for a TDF part, for tools
 * that need to show or diff it. Writes at most out_cap bytes and returns
 * the length it would have written, or -1 on bad input. */
int  TAK_MapFingerprint_CanonicalTDF(const void *text, size_t len,
                                     char *out, size_t out_cap);

#endif /* TAK_MAP_FINGERPRINT_H */
