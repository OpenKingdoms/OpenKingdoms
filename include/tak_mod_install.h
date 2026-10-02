#ifndef TAK_MOD_INSTALL_H
#define TAK_MOD_INSTALL_H

#include <stddef.h>

#include "tak_mod_registry.h"

/* ── Installing a mod from the registry ─────────────────────────────
 *
 * A registry download is a zip laid out like the game folder. What it
 * has under Mods/ and TAKEnhanced/Presets/ is written into the mod
 * root, which is the game folder unless --mod-root names another, and
 * nothing else is. A manifest from the registry entry goes beside the
 * mod set it installs, mod.tdf in a folder mod or <preset>.mod.tdf
 * beside a preset, so the lobby has its name, version and fingerprint.
 *
 * What was written is listed in a receipt, Mods/<id>.registry.txt, and
 * Remove deletes exactly that, keeping any file another receipt lists.
 * web/mods.js does the same in the browser's storage, receipt and all.
 *
 * Check the bytes with TAK_ModEntry_Check before installing them. */

/* The receipt's name under Mods/ for a registry id. */
#define TAK_MODINSTALL_RECEIPT ".registry.txt"

/* 0 on success, else -1 with the reason a player reads, and nothing
 * left behind. */
int TAK_ModInstall_Zip(const char *root, const TAK_ModEntry *e,
                       const void *zip, size_t len, char *why, size_t cap);

/* 0 on success, -1 with why when the id has no receipt or a file would
 * not go. */
int TAK_ModInstall_Remove(const char *root, const char *id, char *why, size_t cap);

/* A zip entry's name as a path under the mod root, with Mods/ or
 * TAKEnhanced/Presets/ spelled as the engine looks for them. 1 to keep,
 * 0 to leave out, -1 for a name that would leave its folder. */
int TAK_ModInstall_Rel(const char *name, char *out, size_t cap);

/* 1 when the id has a receipt under root, its version in version. */
int TAK_ModInstall_Installed(const char *root, const char *id, char *version, size_t cap);

#endif /* TAK_MOD_INSTALL_H */
