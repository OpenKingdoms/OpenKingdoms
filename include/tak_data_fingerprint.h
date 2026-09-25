#ifndef TAK_DATA_FINGERPRINT_H
#define TAK_DATA_FINGERPRINT_H

#include <stdint.h>
#include <stdio.h>

/* ── The data fingerprint ───────────────────────────────────────────
 *
 * What the simulation reads from a player's game files, hashed so two
 * players can tell before a match whether they would play the same
 * game (docs/MULTIPLAYER.md, "The data fingerprint"). Five groups, the
 * five the protocol carries: units (unit files, build lists, side data
 * and movement classes), weapons (the explosion and weapon tables),
 * features, scripts and the AI's profiles.
 *
 * Files are taken through the VFS, so a loose file that overrides an
 * archive is the one hashed. Paths are case folded, a leading "data/"
 * is dropped so the loose and archive layouts agree, and the files go
 * in sorted by path, so how an archive is packed changes nothing. Line
 * ends are dropped from text files, which the parsers ignore anyway.
 * Art, sound, music and maps are not in it: the map has a fingerprint
 * of its own, and the rest is each player's own business. */

enum {
    TAK_DATA_GROUP_UNITS = 0,
    TAK_DATA_GROUP_WEAPONS,
    TAK_DATA_GROUP_FEATURES,
    TAK_DATA_GROUP_SCRIPTS,
    TAK_DATA_GROUP_AI,
    TAK_DATA_GROUP_COUNT
};

/* Bumped when how the engine reads any of these files changes, so two
 * builds that read the same bytes differently do not match. */
#define TAK_DATA_SCHEMA_VERSION 1

typedef struct TAK_DataFingerprint {
    uint64_t schema;
    uint64_t content;
    uint64_t group[TAK_DATA_GROUP_COUNT];
    int      files[TAK_DATA_GROUP_COUNT];
} TAK_DataFingerprint;

/* Hash what is mounted now. 0 on success, -1 with no VFS. */
int TAK_DataFingerprint_Compute(TAK_DataFingerprint *out);

/* The fingerprint of what is mounted, computed once per mount. NULL
 * with no VFS. */
const TAK_DataFingerprint *TAK_DataFingerprint_Get(void);

/* "units", "weapons", "features", "scripts" or "ai". */
const char *TAK_DataFingerprint_GroupName(int group);

/* One line per file, its group, path and hash, then the group totals,
 * so two players can find the one file that differs. */
int TAK_DataFingerprint_Report(FILE *out);

#endif /* TAK_DATA_FINGERPRINT_H */
