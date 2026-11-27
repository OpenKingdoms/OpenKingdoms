/*
 * keybinds_store.c -- the key table's place in the player's folder.
 *
 * The scheme in use and whether the player's own keys are on are two
 * lines of the options. The keys themselves are a file beside them,
 * "keys.cfg", which a player can edit by hand as the original's
 * Keys.TDF could be.
 */

#include "tak_keybinds.h"
#include "tak_paths.h"
#include "tak_settings.h"

#include <stdio.h>

static char s_path[1100];

const char *Keybinds_FilePath(void) {
    snprintf(s_path, sizeof(s_path), "%skeys.cfg", Paths_PrefDir());
    return s_path;
}

void Keybinds_Init(void) {
    Keybinds_UseScheme(Keybinds_SchemeFromName(
        Settings_GetStr(TAK_SETTING_KEY_SCHEME, Keybinds_SchemeName(KEY_SCHEME_DEFAULT))));
    /* After the scheme, which fills a slot the file leaves out. */
    (void)Keybinds_LoadFile(Keybinds_FilePath());
    Keybinds_SetCustomOn(Settings_GetInt(TAK_SETTING_KEYS_CUSTOM, 1));
}

int Keybinds_Store(void) {
    Settings_SetStr(TAK_SETTING_KEY_SCHEME, Keybinds_SchemeName(Keybinds_Scheme()));
    Settings_SetInt(TAK_SETTING_KEYS_CUSTOM, Keybinds_CustomOn());
    int rc = 0;
    /* No keys of the player's own leaves no file to read back. */
    if (Keybinds_HasCustom()) rc = Keybinds_SaveFile(Keybinds_FilePath());
    else (void)remove(Keybinds_FilePath());
    Paths_NotifyPrefWritten();
    return rc;
}
