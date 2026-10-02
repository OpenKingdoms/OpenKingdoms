#ifndef TAK_MODSET_H
#define TAK_MODSET_H

#include <stddef.h>
#include <stdint.h>

/* ── Mod sets ────────────────────────────────────────────────────────
 *
 * A mod set is a named list of archives and folders mounted over the
 * game's own data, first to last, with a later one winning (see
 * VFS_SetModArchives). Two kinds are found under a mod root, which is
 * the game folder unless told otherwise:
 *
 *   TAK Enhanced presets, TAKEnhanced/Presets/*.preset.json, each naming
 *   the Mods/*.hpi files it switches on. A preset with its mods switched
 *   off is the game itself and is not listed.
 *
 *   A folder of its own under Mods/. Its archives (.hpi, .ufo) mount in
 *   name order and then the folder itself, read loose, so a file edited
 *   in it wins over everything. An optional mod.tdf in it names it:
 *
 *       [MOD]
 *       {
 *       name=My Mod;
 *       version=1.0;
 *       fingerprint=0123456789abcdef;
 *       }
 *
 * That file is the mod's manifest. The fingerprint is the data
 * fingerprint the mod produces, the content line --data-report prints,
 * so a lobby can tell which installed mod a game needs. A preset takes
 * a manifest beside it, its file name with .mod.tdf for .preset.json,
 * whose fields win over the preset's own.
 *
 * "vanilla" is always there and mounts nothing. The data fingerprint
 * reads through the mount, so a mod set changes it the way it changes
 * the game. */

#define TAK_MODSET_MAX       24
#define TAK_MODSET_PATHS     32
#define TAK_MODSET_PATH_MAX  260

typedef struct TAK_ModSet {
    char id[64];
    char name[96];
    char version[32];
    char kind[8];                 /* "preset" or "folder" */
    int  count;
    char path[TAK_MODSET_PATHS][TAK_MODSET_PATH_MAX];
    int  missing;                 /* preset entries with no file behind them */
    uint64_t fingerprint;         /* the manifest's, 0 when it names none */
} TAK_ModSet;

/* The mod sets under root, vanilla first. Returns how many, up to cap. */
int TAK_ModSet_Scan(const char *root, TAK_ModSet *out, int cap);

/* A TAK Enhanced preset read from its JSON. The paths are mods_dir with
 * each selected file name joined on, whether or not they exist. 0 on
 * success, -1 when it is not a preset or its mods are switched off. */
int TAK_ModSet_ParsePreset(const char *json, size_t len, const char *mods_dir,
                           TAK_ModSet *out);

/* The set with this id, case folded, or NULL. */
const TAK_ModSet *TAK_ModSet_Find(const TAK_ModSet *sets, int n, const char *id);

/* One manifest's text into a set: name=, version= and fingerprint=,
 * each only where the set has none yet. */
void TAK_ModSet_ReadManifest(const char *text, TAK_ModSet *m);

/* What the running game was mounted with, for the menu and the lobby.
 * ActiveName is the name and version together, ActiveModName and
 * ActiveVersion the two apart, as a room advertises them. */
void        TAK_ModSet_SetActive(const TAK_ModSet *set);
const char *TAK_ModSet_ActiveId(void);
const char *TAK_ModSet_ActiveName(void);
const char *TAK_ModSet_ActiveModName(void);
const char *TAK_ModSet_ActiveVersion(void);
/* The active set's manifest fingerprint, 0 when it names none. */
uint64_t    TAK_ModSet_ActiveFingerprint(void);
int         TAK_ModSet_IsVanilla(void);

/* Where Mods/ and the presets are looked for, the game folder unless
 * --mod-root names another. A registry install writes there. */
void        TAK_ModSet_SetRoot(const char *root);
const char *TAK_ModSet_Root(void);

/* The sets the scan found, kept so the lobby can say which one a game
 * needs. The copy holds names and fingerprints, not paths. */
void TAK_ModSet_SetInstalled(const TAK_ModSet *sets, int n);

/* "TAK Enhanced 1.4", or "" for a room that named no mod set. */
void TAK_ModSet_Label(const char *name, const char *version, char *out, size_t cap);

/* What a lobby row says when its data differs from ours: the mod set the
 * room plays, and whether we have it, have another version of it, or
 * have it and need only choose it. A room that named no mod set gets
 * the plain data line. */
void TAK_ModSet_JoinAdvice(const char *room_mod, const char *room_version,
                           uint64_t room_content, char *out, size_t cap);

#endif /* TAK_MODSET_H */
