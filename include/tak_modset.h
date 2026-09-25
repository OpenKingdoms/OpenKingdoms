#ifndef TAK_MODSET_H
#define TAK_MODSET_H

#include <stddef.h>

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
 *       }
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

/* What the running game was mounted with, for the menu and the lobby. */
void        TAK_ModSet_SetActive(const TAK_ModSet *set);
const char *TAK_ModSet_ActiveId(void);
const char *TAK_ModSet_ActiveName(void);
int         TAK_ModSet_IsVanilla(void);

#endif /* TAK_MODSET_H */
