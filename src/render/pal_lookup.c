/*
 * pal_lookup.c -- TAK GAF-to-palette mapping table
 *
 * Reverse-engineered from the TAK Kingdoms game binary. The original engine
 * loads specific palettes for specific GAF files via UnitTypeMap_InsertNode()
 * and Palette_LoadFromPCX(). This table captures those associations.
 *
 * Mapping sources (from legacy SideData_LoadAll and GameArt_LoadAllResources):
 *   gameart.pcx       -- Default for all unit/animation GAFs
 *   fx.pcx            -- Effects: smoke, fire, death, shadows, damage flames
 *   cursors.pcx       -- All cursor sprites
 *   colorlogos.pcx    -- Faction logos, team logos
 *   guipal.pcx        -- GUI elements: buttons, scrollbars, fonts, HUD
 *   modalbuttons.pcx  -- In-game modal dialog buttons
 *   Per-faction        -- ara_textures/aramon_features, tar_textures/taros_features, etc.
 */

#include "tak_palette.h"
#include <string.h>

static int pal_istrcmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int pal_ends_with(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    return pal_istrcmp(haystack + hlen - nlen, needle) == 0;
}

static void pal_basename_no_ext(const char *path, char *out, int out_size) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    int i = 0;
    while (name[i] && name[i] != '.' && i < out_size - 1) {
        out[i] = name[i];
        i++;
    }
    out[i] = '\0';
}

static int pal_prefix3(const char *name, char a, char b, char c) {
    char n0 = (name[0] >= 'A' && name[0] <= 'Z') ? name[0] + 32 : name[0];
    char n1 = (name[1] >= 'A' && name[1] <= 'Z') ? name[1] + 32 : name[1];
    char n2 = (name[2] >= 'A' && name[2] <= 'Z') ? name[2] + 32 : name[2];
    return n0 == a && n1 == b && n2 == c;
}

/* GAF lists for specific palettes */
static const char *fx_gafs[] = {
    "smoke", "radiated", "nobuild", "transportfx", "deathmagic",
    "activity_fires", "minifire", "oldfx", NULL
};

static const char *cursor_gafs[] = { "cursors", "cursors_tex", NULL };

static const char *logo_gafs[] = {
    "colorlogos", "colorlogos2", "teamlogos", "logos", NULL
};

static const char *gui_gafs[] = {
    "commongui", "commongui_french", "commongui_german",
    "gui", "buildbuttons", "scrollbars", "modalbuttons",
    /* In-game HUD action buttons. The legacy engine pulls these from
     * igcommonbuttons.gaf (referenced from araingame.gui) — 29×29
     * GUI icons that share guipal with everything else GUI. The
     * older "actionbuttons.gaf" name is shipped but unused.  */
    "actionbuttons", "igcommonbuttons", "buildpic",
    "byhelpbutton", "byhelpcrusadesbutton",
    "bymessagemarquee", "bynewsconsole",
    "byc_icons", "byc_primary", "byc_secondary", "byc_shadows",
    "byfakemetamapbg", "bywarroombutton", "bynewsninfobutton",
    "bynewsninfocrusadesbutton", "byiconlookup",
    "byiconsgrouped", "byiconslatency",
    "bymetagameforming", "byhouseterror", "byhousehonor",
    "metamap_font", "font48",
    "f2menu", "battlemenu", "battlemenuoptions",
    "multiplayerexitmenu", "singleplayerexitmenu",
    "knighterrant", "strategicpoint", "main_console",
    NULL
};

static int in_list(const char *name, const char **list) {
    for (int i = 0; list[i]; i++)
        if (pal_istrcmp(name, list[i]) == 0) return 1;
    return 0;
}

/*
 * Palette_LookupForGAF -- Determine the best palette PCX for a GAF file.
 *
 * Returns a palette filename (e.g. "guipal.pcx") that should be searched
 * in the palettes directory. Returns NULL if the GAF has a matching .pcx
 * next to it (e.g. singlemachine.gaf -> singlemachine.pcx).
 */
const char *Palette_LookupForGAF(const char *gaf_path) {
    char name[64];
    pal_basename_no_ext(gaf_path, name, sizeof(name));

    /* Self-palette GAFs (matching .pcx next to the GAF) */
    static const char *self_pals[] = {
        "singlemachine", "bodgirl", "multiknight",
        "mainscreen", "mainmenu", "mainmenuconflict",
        "singleplayerwin", "singleplayerlose", "singleplaywin",
        "battlescreens", NULL
    };
    if (in_list(name, self_pals)) return NULL;

    if (in_list(name, fx_gafs)) return "fx.pcx";
    if (in_list(name, cursor_gafs)) return "cursors.pcx";
    if (in_list(name, logo_gafs)) return "colorlogos.pcx";
    if (in_list(name, gui_gafs)) return "guipal.pcx";

    /* Faction prefixes -> features palette (primary), textures (alt) */
    if (pal_prefix3(name, 'a', 'r', 'a')) return "aramon_features.pcx";
    if (pal_prefix3(name, 'a', 'i', 'd')) return "aiden_features.pcx";
    if (pal_prefix3(name, 't', 'a', 'r')) return "taros_features.pcx";
    if (pal_prefix3(name, 'v', 'e', 'r')) return "veruna_features.pcx";
    if (pal_prefix3(name, 'z', 'o', 'n')) return "zhon_features.pcx";
    if (pal_prefix3(name, 'z', 'h', 'o')) return "zhon_features.pcx";
    if (pal_prefix3(name, 'n', 'p', 'c')) return "npc_textures.pcx";
    if (pal_prefix3(name, 'm', 'o', 'n')) return "mon_textures.pcx";

    if (pal_ends_with(name, "bipal")) return "guipal.pcx";
    if (pal_ends_with(name, "shadow") || pal_ends_with(name, "shadows")) return "fx.pcx";
    if (strstr(name, "font") || strstr(name, "Font") ||
        strstr(name, "times") || strstr(name, "Times") ||
        strstr(name, "lombardic") || strstr(name, "Lombardic"))
        return "guipal.pcx";

    return "gameart.pcx";
}

/*
 * Palette_LookupForGAFAlt -- Alternative palette for faction GAFs.
 * Primary returns _features; this returns _textures. NULL if no alt.
 */
const char *Palette_LookupForGAFAlt(const char *gaf_path) {
    char name[64];
    pal_basename_no_ext(gaf_path, name, sizeof(name));

    if (pal_prefix3(name, 'a', 'r', 'a')) return "ara_textures.pcx";
    if (pal_prefix3(name, 'a', 'i', 'd')) return "aid_textures.pcx";
    if (pal_prefix3(name, 't', 'a', 'r')) return "tar_textures.pcx";
    if (pal_prefix3(name, 'v', 'e', 'r')) return "ver_textures.pcx";
    if (pal_prefix3(name, 'z', 'o', 'n')) return "zon_textures.pcx";
    if (pal_prefix3(name, 'z', 'h', 'o')) return "zon_textures.pcx";
    /* Iron Plague's Creon. Its sidedata palette is ara_textures.pal and
     * this file holds the same colours. */
    if (pal_prefix3(name, 'c', 'r', 'e')) return "cre_textures.pcx";
    return NULL;
}
