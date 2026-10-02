#ifndef TAK_MOD_REGISTRY_H
#define TAK_MOD_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

/* ── The mod registry ───────────────────────────────────────────────
 *
 * web/mods/registry.json, served with the site, lists the mods a player
 * can install in one click (docs/MODDING.md, "The mod registry"):
 *
 *   { "registry": 1, "mods": [ { "id": "tak-enhanced", ... }, ... ] }
 *
 * Each entry names the mod, its version, author and home page. A one
 * click entry adds the download: its https url, its size in bytes, the
 * sha256 of those bytes, the mod set id the install produces and the
 * data fingerprint that mod set gives, which is what a room carries. An
 * entry whose download a script cannot reach, behind a forum login say,
 * has "manual" instead: what to do by hand.
 *
 * The download is a zip. Only what sits under Mods/ and
 * TAKEnhanced/Presets/ in it is installed, into the mod root, so a mod
 * that ships its own Kingdoms.exe or Keys.tdf leaves the player's alone.
 *
 * The page reads the same file in web/mods.js and validates it by the
 * same rules. Keep the two in step. */

#define TAK_MODREG_MAX         32
/* The largest download listed or streamed, so the relay stays small. */
#define TAK_MODREG_SIZE_MAX    (64u << 20)

typedef struct TAK_ModEntry {
    char     id[48];
    char     name[32];          /* TAK_NET_MOD_NAME_MAX, as a room carries it */
    char     version[16];       /* TAK_NET_MOD_VERSION_MAX */
    char     author[64];
    char     page[256];
    char     modset[64];        /* the mod set id the install produces */
    char     url[512];
    uint64_t size;
    uint8_t  sha256[32];
    uint64_t fingerprint;
    char     manual[512];       /* set instead of url for a by-hand install */
} TAK_ModEntry;

typedef struct TAK_ModRegistry {
    int          count;
    TAK_ModEntry mod[TAK_MODREG_MAX];
    int          errors;        /* entries left out as invalid */
    char         first_error[160];
} TAK_ModRegistry;

/* Reads a registry. Valid entries are kept and invalid ones counted and
 * left out, the first reason in first_error. Returns how many were
 * kept, or -1 when the text is not a registry at all. */
int TAK_ModRegistry_Parse(const char *json, size_t len, TAK_ModRegistry *out);

/* The entry with this id, or NULL. */
const TAK_ModEntry *TAK_ModRegistry_Find(const TAK_ModRegistry *r, const char *id);

/* The entry a room's mod set comes from: the one whose fingerprint is
 * the room's, else one with the room's name and version. NULL when the
 * registry has none. */
const TAK_ModEntry *TAK_ModRegistry_ForRoom(const TAK_ModRegistry *r,
                                            const char *mod, const char *version,
                                            uint64_t content);

int TAK_ModEntry_OneClick(const TAK_ModEntry *e);

/* Whether downloaded bytes are the entry's, and the entry the room's.
 * 0 when they are, else -1 with the reason a player reads. A room
 * content of 0 skips the room. */
int TAK_ModEntry_Check(const TAK_ModEntry *e, const void *bytes, size_t len,
                       uint64_t room_content, char *why, size_t cap);

#endif /* TAK_MOD_REGISTRY_H */
