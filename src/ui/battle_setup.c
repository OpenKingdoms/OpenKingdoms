/*
 * battle_setup.c -- Skirmish lobby screen (GAMESTATE_BATTLE_SETUP).
 *
 * Data-driven through battlemenusingle.gui + the generic GUI runtime.
 * This file layers the screen-specific game behavior on top:
 *
 *   - Bottom buttons (Previous / Load / Options / Play)
 *   - Right-panel checkboxes (Line of Sight, Map Revealed, etc.)
 *     flip their backing BattleConfig field on click.
 *   - Per-row player slots cycle through Open → Player → AI → Computer
 *     (click "PlayerName"); side cycles through Aramon/Taros/Veruna/Zhon
 *     (click "PlayerSide"); team cycles 1..4 (click "PlayerTeam").
 *   - "Units / side" slider increments on click / decrements on right-click
 *     between TAK_UNITS_PER_SIDE_MIN and ..._MAX in ..._STEP increments.
 *   - Map list scans maps/Maps/*.ota and lets the user pick one. The row
 *     text is the map's authored name, the .ota description goes under
 *     "Map Description" (legacy:167638, legacy:136102).
 *
 * A future .gui-parser extension could drive the slot rects from the
 * dialog directly; for now the per-row rendering uses widget lookup
 * by name + the row's row_y to pick overlays.
 */

#include "tak_battle_setup.h"
#include "tak_gameloop.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_blit.h"
#include "tak_ui.h"
#include "tak_battle_config.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_hpi.h"
#include "tak_tnt.h"
#include "tak_tdf.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_world.h"
#include <SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define BS_MAX_MAPS        256
/* MapNameEntryTemplate in battlemenusingle.gui is 22 px tall, so the
 * 112 px list shows five rows. */
#define BS_MAP_ROW_HEIGHT  22

/* Legacy default when an .ota has no missiondescription (legacy:168923). */
#define BS_NO_DESCRIPTION  "No description available"

/* AI player names. Start with a personal roster (family first), then
 * the legacy pool from data/gamedata/ainames.tdf so modders can see the
 * original flavor when cycling slots. */
static const char *bs_ai_names[] = {
    "Zach", "Cory", "Eric",
    /* Names below come from ainames.tdf in the original. Included as a
     * tribute / fallback pool — Al Shane / Kirenna / etc. are monarchs
     * and intentionally not used as AI identities. */
    "Clayton", "Brian",  "Kevin",  "Paul",    "Steve",  "Matt",
    "Frank",   "Corey",  "Brad",   "Ingrid",  "Gronk",  "John",
    "Ron",     "Joe",    "Garrett","Dan",     "Rebbeca","Ruffus",
    "Bryan",   "Tim",    "Pat",    "Bruce",   "Rick",   "Jay",
    "Mark",    "Dane",   "Stephen","TJ",      "Andrew", "Ludwin",
    "Culin",   "Phil",
};
#define BS_NUM_AI_NAMES (int)(sizeof(bs_ai_names) / sizeof(bs_ai_names[0]))

typedef struct {
    int          initialized;
    GUIDialog    dialog;
    GUIRuntime  *rt;
    Font        *font_small;
    Font        *font_header;
    BattleConfig cfg;
    int          pending_nextstate;

    /* Team-logo badges. sidedata.tdf names the GAF and the per-side entry
     * (logogaf / logoart, legacy:164796). Each entry ships one frame per
     * player colour and the badge is that frame, untinted. */
    GAFFile     *teamlogo_gaf;
    uint32_t     teamlogo_rgba[256];
    uint32_t    *teamlogo_frames[TAK_SIDE_COUNT][TAK_PLAYER_COLOR_COUNT];
    int          teamlogo_w[TAK_SIDE_COUNT][TAK_PLAYER_COLOR_COUNT];
    int          teamlogo_h[TAK_SIDE_COUNT][TAK_PLAYER_COLOR_COUNT];

    /* Map list state. `maps` holds the .ota base name (the load key),
     * `map_display` the name the row shows. */
    char  maps[BS_MAX_MAPS][80];
    char  map_display[BS_MAX_MAPS][80];
    int   num_maps;
    int   selected_map;     /* index into maps[], or -1 */
    int   map_scroll;       /* top visible row */

    /* Per-selected-map metadata parsed from its .ota. */
    int   map_size_x;       /* map width in grid squares */
    int   map_size_y;
    int   map_max_players;
    char  map_kingdom[32];  /* lowercased faction name from OTA kingdom= */
    char  map_description[128];

    /* Terrain palette LUT, built once at init from data/palettes/palette.pal.
    * Used by TNT_Load to decode the minimap's 8-bit indices into RGBA. */
    uint32_t terrain_rgba[256];

    // Loaded TNT file for the currently selectred map. Minimap pixels live
    // inside tnt.minimap_rgba once it has been populated
    TNTFile tnt;

    /* Slider drag state. When mouse is pressed on the MaxUnits track or
     * thumb, we start dragging and update the value every frame until
     * mouseup — not just on click. */
    int   dragging_slider;

    /* Cached widget indices so the per-frame "move the right sbutton"
     * logic can't misidentify them after its own moves have shifted
     * their rects out of the identification bands. Indices into
     * bs.dialog.children. -1 = not found. */
    int   idx_units_thumb;         /* MaxUnits (UnitBattleThumb) */
    int   idx_units_inc, idx_units_dec;
    int   idx_units_track;         /* MaxUnits bar (UnitBattleBar art) */
    int   idx_maplist_thumb;       /* map list ScrollThumb */
    int   idx_maplist_inc, idx_maplist_dec;
    int   idx_maplist_track;       /* map list slider (BattleBar art) */

    /* Map-list thumb drag: 1 while held, plus the grab offset inside it. */
    int   dragging_maplist;
    int   maplist_grab_dy;
} BSState;

static BSState bs;

/* ── Option → BattleConfig field bindings ────────────────────────────── */

typedef struct {
    const char *widget_name;
    size_t      cfg_offset;
} OptionBinding;
static const OptionBinding bs_option_bindings[] = {
    { "LineOfSight",    offsetof(BattleConfig, line_of_sight) },
    { "Mapping",        offsetof(BattleConfig, map_revealed) },
    { "MonarchDeath",   offsetof(BattleConfig, monarch_expendable) },
    { "StartLocations", offsetof(BattleConfig, random_start_locations) },
    { "CheatCodes",     offsetof(BattleConfig, power_codes) },
    { "SlowGame",       offsetof(BattleConfig, slow_game) },
    /* Iron Plague replaced the hidden Slow Game row with this one
     * (legacy:139236 binds both on the skirmish screen). */
    { "CrusadesBalance", offsetof(BattleConfig, crusades_balance) },
};
#define BS_NUM_OPTIONS (int)(sizeof(bs_option_bindings) / sizeof(bs_option_bindings[0]))

static int *bs_cfg_field_by_widget(const char *name) {
    for (int i = 0; i < BS_NUM_OPTIONS; i++) {
        if (tak_stricmp(bs_option_bindings[i].widget_name, name) == 0)
            return (int *)((char *)&bs.cfg + bs_option_bindings[i].cfg_offset);
    }
    return NULL;
}

/* ── Player slot plumbing ────────────────────────────────────────────── */

/* Row Y positions from battlemenusingle.gui (one per player slot). */
static const int bs_slot_row_y[TAK_MAX_PLAYERS] = {
    61, 83, 105, 127, 149, 171, 193, 215
};

/* Given a clicked widget's rect, figure out which slot row it's in. */
static int bs_slot_index_from_rect(const SDL_Rect *r) {
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        int dy = r->y - bs_slot_row_y[i];
        if (dy >= -2 && dy <= 4) return i;     /* tolerant match */
    }
    return -1;
}

/* Cycle: Open → Player(human) → AI → Closed → Open. Slot 0 stays human
 * (it's always the local player). When toggling to AI, assign the next
 * canonical name so the lobby looks populated instead of showing eight
 * "Computer"s. */
static void cycle_slot_kind(PlayerSlot *ps, int slot_idx) {
    if (slot_idx == 0) return;
    switch (ps->kind) {
    case TAK_SLOT_CLOSED:
        ps->kind = TAK_SLOT_AI;
        strncpy(ps->name, bs_ai_names[slot_idx % BS_NUM_AI_NAMES],
                sizeof(ps->name) - 1);
        ps->name[sizeof(ps->name) - 1] = '\0';
        break;
    case TAK_SLOT_AI:
        ps->kind = TAK_SLOT_CLOSED;
        ps->name[0] = '\0';
        break;
    case TAK_SLOT_HUMAN:
        ps->kind = TAK_SLOT_CLOSED;
        ps->name[0] = '\0';
        break;
    }
}

static void cycle_side(PlayerSlot *ps) {
    ps->side = (ps->side + 1) % (int)TAK_SIDE_COUNT;
}

static void cycle_team(PlayerSlot *ps) {
    if (ps->team < 1) ps->team = 1;
    else if (ps->team >= 4) ps->team = 0;     /* 0 = FFA */
    else ps->team++;
}

static const char *slot_name_string(const PlayerSlot *ps, int idx) {
    if (ps->kind == TAK_SLOT_CLOSED) return "Empty";
    if (ps->name[0]) return ps->name;
    if (ps->kind == TAK_SLOT_HUMAN) return idx == 0 ? "Player" : "Human";
    return "Computer";
}

/* Draws the per-slot player emblem: the side's team-logo frame for this
 * colour, blitted as authored with no tint and no blend (legacy:136383).
 * Falls back to the shared table's swatch when the art is missing. */
static void draw_color_badge(SDL_Surface *off, int cx, int cy,
                             int color_idx, int side) {
    if (color_idx < 0) color_idx = 0;
    color_idx %= TAK_PLAYER_COLOR_COUNT;
    if (side < 0 || side >= (int)TAK_SIDE_COUNT) side = 0;
    uint32_t *pixels = bs.teamlogo_frames[side][color_idx];
    int w = bs.teamlogo_w[side][color_idx];
    int h = bs.teamlogo_h[side][color_idx];
    if (!pixels || w <= 0 || h <= 0) {
        const TakPlayerColor *pc = BattleConfig_PlayerColor(color_idx);
        uint32_t col = SDL_MapRGBA(off->format, pc->r, pc->g, pc->b, 255);
        SDL_Rect r = { cx - 6, cy - 6, 12, 12 };
        SDL_FillRect(off, &r, col);
        return;
    }
    Blit_RGBA(off, cx - w / 2, cy - h / 2, pixels, w, h);
}

static const char *side_name_string(const PlayerSlot *ps) {
    if (ps->kind == TAK_SLOT_CLOSED) return "";
    static const char *names[] = { "Aramon", "Taros", "Veruna", "Zhon" };
    if (ps->side < 0 || ps->side >= 4) return "";
    return names[ps->side];
}

/* ── Map list scanning ───────────────────────────────────────────────── */

static int ends_with_ota(const char *name) {
    size_t len = strlen(name);
    return len > 4 && tak_stricmp(name + len - 4, ".ota") == 0;
}

static void strip_ota_ext(char *s) {
    size_t len = strlen(s);
    if (len > 4 && tak_stricmp(s + len - 4, ".ota") == 0) s[len - 4] = '\0';
}

/* Upper-case the first letter of every space-separated word, leaving the
 * rest of each word alone. Legacy applies exactly this to a map's file
 * name when the translate table has no entry for it (legacy:167726). */
static void title_case_words(char *s) {
    for (char *p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
}

/* ── Translate table ─────────────────────────────────────────────────
 *
 * Legacy pipes every displayed string through a sorted key -> text table
 * built from english/translate/*.tdf, and a lookup that misses returns
 * the key unchanged (legacy:267931). Map rows, the map description and
 * the .gui's own label placeholders all go through it. */

typedef struct {
    char key[96];
    char text[128];
} BSTranslateEntry;

static BSTranslateEntry *bs_translate;
static int               bs_num_translate;
static int               bs_cap_translate;

static void bs_translate_add(const char *key, const char *text) {
    if (!key || !*key || !text || !*text) return;
    if (bs_num_translate >= bs_cap_translate) {
        int cap = bs_cap_translate ? bs_cap_translate * 2 : 128;
        BSTranslateEntry *grown = (BSTranslateEntry *)tak_realloc(
            bs_translate, (size_t)cap * sizeof(BSTranslateEntry));
        if (!grown) return;
        bs_translate = grown;
        bs_cap_translate = cap;
    }
    BSTranslateEntry *e = &bs_translate[bs_num_translate++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = '\0';
    strncpy(e->text, text, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
}

static void bs_translate_load(const char *path) {
    TDFFile *tdf = TDF_Open(path);
    if (!tdf) return;
    if (TDF_Load(tdf) != 0) { TDF_Close(tdf); return; }
    for (const char *name = TDF_GetFirstSection(tdf); name;
         name = TDF_GetNextSection(tdf)) {
        char section[96];
        strncpy(section, name, sizeof(section) - 1);
        section[sizeof(section) - 1] = '\0';
        if (TDF_PushSection(tdf, section) != 0) continue;
        const char *text = TDF_ReadString(tdf, "English", "");
        if (text && *text) bs_translate_add(section, text);
        TDF_PopSection(tdf);
    }
    TDF_Close(tdf);
}

/* NULL when the table has no entry. Legacy lower-cases the key first,
 * and our compare is case-insensitive, which is the same thing. */
static const char *bs_translate_find(const char *key) {
    if (!key || !*key) return NULL;
    for (int i = 0; i < bs_num_translate; i++) {
        if (tak_stricmp(bs_translate[i].key, key) == 0) return bs_translate[i].text;
    }
    return NULL;
}

/* Legacy's lookup hands back the key itself on a miss (legacy:267931). */
static const char *bs_translate_lookup(const char *key) {
    const char *hit = bs_translate_find(key);
    return hit ? hit : (key ? key : "");
}

/* Naively scrape a few interesting fields from an OTA file. Proper
 * parsing goes through TDF_Load once the parser handles its quirks;
 * for now we do a case-insensitive substring search for "Size=" and
 * "numPlayers=" which is enough to populate the footer line. */
static int ci_substr_find(const char *hay, size_t hay_len,
                          const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > hay_len) return -1;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nlen) return (int)i;
    }
    return -1;
}

static int ci_strtol_at(const char *buf, size_t len, int pos, int *out) {
    /* Skip past any separator chars. */
    while (pos < (int)len && (buf[pos] == ' ' || buf[pos] == '\t' ||
                               buf[pos] == '=' || buf[pos] == ':')) pos++;
    if (pos >= (int)len) return 0;
    char tmp[16];
    int n = 0;
    while (pos < (int)len && n < (int)sizeof(tmp) - 1 &&
           ((buf[pos] >= '0' && buf[pos] <= '9') || buf[pos] == '-')) {
        tmp[n++] = buf[pos++];
    }
    if (!n) return 0;
    tmp[n] = '\0';
    *out = atoi(tmp);
    return 1;
}

/* Copy an OTA `key=value;` payload starting just past the key. Stops at
 * the terminating ';' or end of line and trims trailing blanks. */
static int ci_string_at(const char *buf, size_t len, int pos,
                        char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    while (pos < (int)len && (buf[pos] == ' ' || buf[pos] == '\t' ||
                              buf[pos] == '=')) pos++;
    size_t n = 0;
    while (pos < (int)len && n + 1 < out_cap &&
           buf[pos] != ';' && buf[pos] != '\r' && buf[pos] != '\n' &&
           buf[pos] != '\0') {
        out[n++] = buf[pos++];
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    out[n] = '\0';
    return n > 0;
}

static void load_selected_map_metadata(void) {
    bs.map_size_x = bs.map_size_y = bs.map_max_players = 0;
    /* The placeholder holds until a parsed description replaces it, so
     * an unreadable file still shows what the original shows. */
    strncpy(bs.map_description, bs_translate_lookup(BS_NO_DESCRIPTION),
            sizeof(bs.map_description) - 1);
    bs.map_description[sizeof(bs.map_description) - 1] = '\0';
    if (bs.selected_map < 0 || bs.selected_map >= bs.num_maps) return;

    /* Archives and the loose tree both keep maps/Maps/<name>.ota. */
    char path[512];
    void *data = NULL;
    uint32_t size = 0;
    snprintf(path, sizeof(path), "maps/Maps/%s.ota", bs.maps[bs.selected_map]);
    if (VFS_ReadFile(path, &data, &size) != 0) {
        snprintf(path, sizeof(path), "maps/maps/%s.ota", bs.maps[bs.selected_map]);
        if (VFS_ReadFile(path, &data, &size) != 0) return;
    }
    if (size == 0 || size > 64 * 1024) { VFS_FreeBuffer(data); return; }
    char *buf = (char *)tak_malloc((size_t)size + 1);
    if (!buf) { VFS_FreeBuffer(data); return; }
    memcpy(buf, data, size);
    buf[size] = '\0';
    VFS_FreeBuffer(data);
    size_t got = size;

    int p = ci_substr_find(buf, got, "SizeX");
    if (p >= 0) ci_strtol_at(buf, got, p + 5, &bs.map_size_x);
    p = ci_substr_find(buf, got, "SizeY");
    if (p >= 0) ci_strtol_at(buf, got, p + 5, &bs.map_size_y);
    p = ci_substr_find(buf, got, "numPlayers");
    if (p >= 0) ci_strtol_at(buf, got, p + 10, &bs.map_max_players);

    /* "Map Description" shows the GlobalHeader's missiondescription run
     * through the translate table, defaulting to the legacy placeholder
     * when the key is absent (legacy:168923). */
    char raw[128];
    raw[0] = '\0';
    p = ci_substr_find(buf, got, "missiondescription");
    if (p < 0 || !ci_string_at(buf, got, p + 18, raw, sizeof(raw))) {
        strncpy(raw, BS_NO_DESCRIPTION, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    }
    strncpy(bs.map_description, bs_translate_lookup(raw),
            sizeof(bs.map_description) - 1);
    bs.map_description[sizeof(bs.map_description) - 1] = '\0';

    /* Parse kingdom= to pick the right per-faction palette for the minimap.
     * Per the legacy reference (legacy:86568), the original game
     * calls Palette_LoadFromBMP(kingdom) from the minimap paint handler,
     * which resolves to data/palettes/{kingdom}.pcx. */
    bs.map_kingdom[0] = '\0';
    p = ci_substr_find(buf, got, "kingdom");
    if (p >= 0) {
        int i = p + 7;
        while (i < (int)got && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '=')) i++;
        int j = 0;
        while (i < (int)got && j < (int)sizeof(bs.map_kingdom) - 1 &&
               buf[i] != ';' && buf[i] != '\r' && buf[i] != '\n' &&
               buf[i] != '\0' && buf[i] != ' ' && buf[i] != '\t') {
            char c = buf[i++];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            bs.map_kingdom[j++] = c;
        }
        bs.map_kingdom[j] = '\0';
    }

    tak_free(buf);

    /* Rebuild the terrain RGBA table from the map's faction palette.
     * Fall back to the init-time gameart palette if the kingdom lookup fails. */
    if (bs.map_kingdom[0]) {
        char pcx_path[128];
        snprintf(pcx_path, sizeof(pcx_path),
                 "data/palettes/%s.pcx", bs.map_kingdom);
        Palette faction_pal;
        if (Palette_LoadPCX(&faction_pal, pcx_path) == 0) {
            Palette_BuildRGBATable(&faction_pal, UI_RGBAFormat(),
                                    bs.terrain_rgba, 0);
        }
    }

    // Close the old tnt if one exists
    TNT_Close(&bs.tnt);

    char tnt_path[512];
    snprintf(tnt_path, sizeof(tnt_path), "maps/Maps/%s.tnt", bs.maps[bs.selected_map]);
    if (TNT_Load(&bs.tnt, tnt_path, bs.terrain_rgba) != 0) {
      /* Try lowercase sibling. VFS lookup is case-insensitive per tak_hpi.h
       * but the .tnt/.TNT extension variation is real in shipped maps. */
      snprintf(tnt_path, sizeof(tnt_path), "maps/Maps/%s.TNT", bs.maps[bs.selected_map]);
      TNT_Load(&bs.tnt, tnt_path, bs.terrain_rgba);
    }
}

void BattleSetup_SelectMap(int index) {
    if (index < 0 || index >= bs.num_maps) return;
    bs.selected_map = index;
    strncpy(bs.cfg.map_name, bs.maps[index], sizeof(bs.cfg.map_name) - 1);
    bs.cfg.map_name[sizeof(bs.cfg.map_name) - 1] = '\0';
    load_selected_map_metadata();
}

int BattleSetup_MapCount(void) { return bs.num_maps; }

const char *BattleSetup_MapDisplayName(int index) {
    if (index < 0 || index >= bs.num_maps) return "";
    return bs.map_display[index];
}

const char *BattleSetup_MapKey(int index) {
    if (index < 0 || index >= bs.num_maps) return "";
    return bs.maps[index];
}

const char *BattleSetup_MapDescription(void) { return bs.map_description; }

const BattleConfig *BattleSetup_Config(void) { return &bs.cfg; }

void BattleSetup_CyclePlayerColor(int slot) {
    if (slot < 0 || slot >= TAK_MAX_PLAYERS) return;
    PlayerSlot *ps = &bs.cfg.players[slot];
    if (ps->kind == TAK_SLOT_CLOSED) return;
    ps->color = BattleConfig_NextFreeColor(
        &bs.cfg, slot, (ps->color + 1) % TAK_PLAYER_COLOR_COUNT);
}

static int map_name_cmp(const void *a, const void *b) {
    return tak_stricmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_maps(void) {
    bs.num_maps = 0;
    bs.selected_map = -1;

    /* Archive and loose trees both keep maps/Maps/*.ota (case varies). */
    static const char *const patterns[] = { "maps/Maps/*.ota", "maps/maps/*.ota", "maps/*.ota" };
    char **paths = NULL;
    int n = 0;
    for (size_t i = 0; i < 3; i++) {
        if (VFS_ListFiles(patterns[i], &paths, &n) == 0 && n > 0) {
            fprintf(stderr, "BattleSetup: %d .ota files match %s\n", n, patterns[i]);
            break;
        }
        if (paths) { for (int k = 0; k < n; k++) tak_free(paths[k]); tak_free(paths); }
        paths = NULL;
        n = 0;
    }
    if (n > 1) qsort(paths, n, sizeof(char *), map_name_cmp);
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        const char *bsl = strrchr(base, '\\');
        if (bsl) base = bsl + 1;
        if (bs.num_maps < BS_MAX_MAPS && ends_with_ota(base)) {
            char *key = bs.maps[bs.num_maps];
            char *shown = bs.map_display[bs.num_maps];
            strncpy(key, base, sizeof(bs.maps[0]) - 1);
            key[sizeof(bs.maps[0]) - 1] = '\0';
            strip_ota_ext(key);
            /* Row text is the translate-table entry for the file name, or
             * the file name with each word capitalised (legacy:167724). */
            const char *label = bs_translate_find(key);
            strncpy(shown, label ? label : key, sizeof(bs.map_display[0]) - 1);
            shown[sizeof(bs.map_display[0]) - 1] = '\0';
            if (!label) title_case_words(shown);
            bs.num_maps++;
        }
        tak_free(paths[i]);
    }
    tak_free(paths);

    fprintf(stderr, "BattleSetup: found %d maps\n", bs.num_maps);
    if (bs.num_maps > 0) BattleSetup_SelectMap(0);
}

/* ── Init / Shutdown ─────────────────────────────────────────────────── */

static void localize_labels(void);   /* defined below */

/* Walk the dialog children once and cache which child index is the
 * MaxUnits slider thumb / inc / dec vs the map list's. Multiple widgets
 * share the name "sbutton"/"incbutton"/"decbutton"/"slider", so we
 * discriminate by rect.y band (done at init, before any of our code has
 * moved them). */
static void cache_scroll_indices(void) {
    bs.idx_units_thumb = bs.idx_units_inc = bs.idx_units_dec = -1;
    bs.idx_maplist_thumb = bs.idx_maplist_inc = bs.idx_maplist_dec = -1;
    bs.idx_maplist_track = bs.idx_units_track = -1;
    for (int i = 0; i < bs.dialog.num_children; i++) {
        const GUIWidget *w = &bs.dialog.children[i];
        if (tak_stricmp(w->name, "MaxUnits") == 0) { bs.idx_units_track = i; continue; }
        int is_thumb = tak_stricmp(w->name, "sbutton") == 0;
        int is_inc   = tak_stricmp(w->name, "incbutton") == 0;
        int is_dec   = tak_stricmp(w->name, "decbutton") == 0;
        int is_track = tak_stricmp(w->name, "slider") == 0;
        if (!(is_thumb || is_inc || is_dec || is_track)) continue;

        /* MaxUnits row y ≈ 218; map list row y ≈ 275-387. */
        int is_units = (w->rect.y >= 200 && w->rect.y <= 240);
        if (is_thumb && is_units) bs.idx_units_thumb = i;
        if (is_inc   && is_units) bs.idx_units_inc   = i;
        if (is_dec   && is_units) bs.idx_units_dec   = i;
        if (is_thumb && !is_units) bs.idx_maplist_thumb = i;
        if (is_inc   && !is_units) bs.idx_maplist_inc   = i;
        if (is_dec   && !is_units) bs.idx_maplist_dec   = i;
        if (is_track && !is_units) bs.idx_maplist_track = i;
    }
}

/* sidedata.tdf names the badge art per side: logogaf is the GAF stem and
 * logoart the entry inside it (legacy:164796). Decode one frame per
 * player colour for each side. */
static void load_team_logos(void) {
    static const char *const side_keys[TAK_SIDE_COUNT] = {
        "SIDE0", "SIDE1", "SIDE2", "SIDE3"
    };
    char art[TAK_SIDE_COUNT][64];
    char gaf_stem[64] = "colorlogos2";
    for (int s = 0; s < (int)TAK_SIDE_COUNT; s++) art[s][0] = '\0';

    TDFFile *tdf = TDF_Open("data/gamedata/sidedata.tdf");
    if (tdf && TDF_Load(tdf) == 0) {
        for (int s = 0; s < (int)TAK_SIDE_COUNT; s++) {
            if (TDF_PushSection(tdf, side_keys[s]) != 0) continue;
            const char *g = TDF_ReadString(tdf, "logogaf", "");
            const char *a = TDF_ReadString(tdf, "logoart", "");
            if (g && *g) { strncpy(gaf_stem, g, sizeof(gaf_stem) - 1);
                           gaf_stem[sizeof(gaf_stem) - 1] = '\0'; }
            if (a && *a) { strncpy(art[s], a, sizeof(art[0]) - 1);
                           art[s][sizeof(art[0]) - 1] = '\0'; }
            TDF_PopSection(tdf);
        }
    }
    if (tdf) TDF_Close(tdf);

    char gaf_path[128], pcx_path[128];
    snprintf(gaf_path, sizeof(gaf_path), "data/anims/%s.gaf", gaf_stem);
    snprintf(pcx_path, sizeof(pcx_path), "data/anims/%s.pcx", gaf_stem);
    if (UI_LoadGAFWithPalette(gaf_path, pcx_path, &bs.teamlogo_gaf,
                              bs.teamlogo_rgba) != 0) {
        fprintf(stderr, "BattleSetup: no team logo art (%s)\n", gaf_path);
        return;
    }

    for (int s = 0; s < (int)TAK_SIDE_COUNT; s++) {
        /* The entry order is not the side order once Iron Plague adds
         * Creon, so resolve by the name sidedata gave us. */
        int entry_off = art[s][0]
                        ? GAF_FindSequence(bs.teamlogo_gaf, art[s]) : -1;
        if (entry_off < 0 && s < (int)bs.teamlogo_gaf->num_entries &&
            12u + (uint32_t)(s + 1) * 4u <= bs.teamlogo_gaf->data_size) {
            entry_off = (int)*(uint32_t *)(bs.teamlogo_gaf->data + 12 + s * 4);
        }
        /* A truncated sheet must not walk us off the buffer. */
        if (entry_off < 0 ||
            (uint32_t)entry_off + sizeof(EntryHeader) > bs.teamlogo_gaf->data_size)
            continue;
        EntryHeader *eh = (EntryHeader *)(bs.teamlogo_gaf->data + entry_off);
        int nframes = (int)eh->num_frames;
        /* The 12-frame sheets lead with two greyed states, so the colour
         * index starts at frame 2 there (legacy:136390). */
        int base = (nframes >= TAK_PLAYER_COLOR_COUNT + 2) ? 2 : 0;
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
            if (base + c >= nframes) break;
            bs.teamlogo_frames[s][c] = UI_DecodeFrame(
                bs.teamlogo_gaf, entry_off, base + c, bs.teamlogo_rgba,
                &bs.teamlogo_w[s][c], &bs.teamlogo_h[s][c]);
        }
    }
}

int BattleSetup_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&bs, 0, sizeof(bs));
    bs.pending_nextstate = -1;
    BattleConfig_SetDefaults(&bs.cfg);

    Palette terrain_palette;
    if (Palette_Load(&terrain_palette, "data/palettes/gameart.pal") == 0) {
        Palette_BuildRGBATable(&terrain_palette, UI_RGBAFormat(), bs.terrain_rgba, 0);
    } else {
        fprintf(stderr, "BattleSetup: failed to load terrain palette\n");
    }

    if (GUIDialog_Load(&bs.dialog, "data/guis/battlemenusingle.gui") != 0) {
        fprintf(stderr, "BattleSetup: failed to parse battlemenusingle.gui\n");
        return -1;
    }
    bs.rt = GUIRuntime_Create(&bs.dialog);
    if (!bs.rt) { GUIDialog_Free(&bs.dialog); return -1; }

    bs.font_small  = Font_Load("data/fonts/b_times new roman (100)",
                                UI_RGBAFormat());
    bs.font_header = Font_Load("data/fonts/b_times new roman (100b)",
                                UI_RGBAFormat());

    load_team_logos();

    /* Displayed strings come from the translate tables, same as legacy
     * (legacy:267931). maps.tdf carries the map names and descriptions,
     * gui_text.tdf the dialog's own label placeholders. */
    bs_translate_load("english/translate/maps.tdf");
    bs_translate_load("english/translate/gui_text.tdf");

    cache_scroll_indices();
    localize_labels();
    scan_maps();

    bs.initialized = 1;
    return 0;
}

void BattleSetup_Shutdown(void) {
    if (!bs.initialized) return;
    for (int s = 0; s < (int)TAK_SIDE_COUNT; s++) {
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
            if (bs.teamlogo_frames[s][c]) tak_free(bs.teamlogo_frames[s][c]);
        }
    }
    if (bs.teamlogo_gaf) GAF_Close(bs.teamlogo_gaf);
    if (bs_translate) {
        tak_free(bs_translate);
        bs_translate = NULL;
        bs_num_translate = bs_cap_translate = 0;
    }
    if (bs.rt)          GUIRuntime_Destroy(bs.rt);
    if (bs.font_small)  Font_Free(bs.font_small);
    if (bs.font_header) Font_Free(bs.font_header);
    TNT_Close(&bs.tnt);
    GUIDialog_Free(&bs.dialog);
    memset(&bs, 0, sizeof(bs));
}

/* ── Tick ────────────────────────────────────────────────────────────── */

static void sync_checkbox_visuals(void) {
    for (int i = 0; i < BS_NUM_OPTIONS; i++) {
        int *v = bs_cfg_field_by_widget(bs_option_bindings[i].widget_name);
        if (!v) continue;
        /* 3 = unchecked, 4 = checked on the 5-frame sheet
         * (legacy:139330). */
        GUIRuntime_SetFrameOverride(bs.rt,
                                     bs_option_bindings[i].widget_name,
                                     (*v) ? 4 : 3);
    }
}

/* Click dispatch for a widget inside the player-slot table. Returns 1 if
 * handled so the generic path skips it. */
static int handle_slot_click(const char *name, int widget_index) {
    if (widget_index < 0 || widget_index >= bs.dialog.num_children) return 0;
    const GUIWidget *w = &bs.dialog.children[widget_index];
    int row = bs_slot_index_from_rect(&w->rect);
    if (row < 0) return 0;
    PlayerSlot *ps = &bs.cfg.players[row];

    if (tak_stricmp(name, "PlayerName") == 0) {
        cycle_slot_kind(ps, row);
        return 1;
    }
    if (tak_stricmp(name, "PlayerSide") == 0) {
        if (ps->kind != TAK_SLOT_CLOSED) cycle_side(ps);
        return 1;
    }
    if (tak_stricmp(name, "PlayerTeam") == 0) {
        if (ps->kind != TAK_SLOT_CLOSED) cycle_team(ps);
        return 1;
    }
    if (tak_stricmp(name, "PlayerColor") == 0) {
        BattleSetup_CyclePlayerColor(row);
        return 1;
    }
    return 0;
}

static int clamp_units(int v) {
    if (v < TAK_UNITS_PER_SIDE_MIN) return TAK_UNITS_PER_SIDE_MIN;
    if (v > TAK_UNITS_PER_SIDE_MAX) return TAK_UNITS_PER_SIDE_MAX;
    return v;
}

/* Where a widget's art actually lands. Both scroll bars are authored with
 * a hotspot that shifts them inside their nubs (legacy:45228), so the
 * .gui rect alone puts the thumb in the wrong place. */
static void bs_draw_rect(int widget_index, SDL_Rect *out) {
    out->x = out->y = 0;
    out->w = out->h = 1;
    if (widget_index < 0 || widget_index >= bs.dialog.num_children) return;
    if (GUIRuntime_WidgetDrawRect(bs.rt, widget_index, out) != 0) {
        *out = bs.dialog.children[widget_index].rect;
    }
}

/* Mouse X along the bar to a units value snapped to STEP. Defined below
 * next to the map list's equivalent. */
static int units_from_mouse_x(int mx);

/* Click anywhere on the MaxUnits bar sets the value there. */
static int handle_slider_click(const char *name, int click_x) {
    if (tak_stricmp(name, "MaxUnits") == 0) {
        bs.cfg.units_per_side = units_from_mouse_x(click_x);
        return 1;
    }
    return 0;
}

/* Move the `sbutton` widget (UnitBattleThumb sprite) to the X position
 * corresponding to the current units_per_side. */
static void sync_slider_thumb_position(void) {
    if (bs.idx_units_thumb < 0 || bs.idx_units_track < 0) return;
    SDL_Rect bar, thumb;
    bs_draw_rect(bs.idx_units_track, &bar);
    bs_draw_rect(bs.idx_units_thumb, &thumb);
    int travel = bar.w - thumb.w;
    if (travel < 1) travel = 1;
    int range = TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN;
    if (range < 1) range = 1;
    int rel = travel * (bs.cfg.units_per_side - TAK_UNITS_PER_SIDE_MIN) / range;
    GUIWidget *w = &bs.dialog.children[bs.idx_units_thumb];
    /* Rect, not draw position: the renderer re-applies the hotspot. */
    w->rect.x = bar.x + rel + (w->rect.x - thumb.x);
}

/* The "Map Description" Static holds the selected .ota's description,
 * not a heading (legacy:136122). */
static void sync_map_description(void) {
    GUIWidget *w = GUIDialog_FindByName(&bs.dialog, "MapDescription");
    if (!w) return;
    strncpy(w->display_text, bs.map_description, sizeof(w->display_text) - 1);
    w->display_text[sizeof(w->display_text) - 1] = '\0';
}

/* Refresh the NumberOfUnits label's display_text so the renderer draws
 * the live value instead of the .gui template ("500"). */
static void sync_units_label_text(void) {
    GUIWidget *w = GUIDialog_FindByName(&bs.dialog, "NumberOfUnits");
    if (!w) return;
    snprintf(w->display_text, sizeof(w->display_text), "%d",
             bs.cfg.units_per_side);
}

/* Every string the dialog displays goes through the translate table, so
 * the column-header placeholders (_SPName_, _SPSide_, _SPColor_,
 * _SPTeam_) resolve to their titles and everything else keeps the text
 * the .gui already carries (legacy:267931). */
static void localize_labels(void) {
    for (int ch = 0; ch < bs.dialog.num_children; ch++) {
        GUIWidget *w = &bs.dialog.children[ch];
        const char *hit = bs_translate_find(w->display_text);
        if (hit) {
            strncpy(w->display_text, hit, sizeof(w->display_text) - 1);
            w->display_text[sizeof(w->display_text) - 1] = '\0';
        }
        hit = bs_translate_find(w->tooltip);
        if (hit) {
            strncpy(w->tooltip, hit, sizeof(w->tooltip) - 1);
            w->tooltip[sizeof(w->tooltip) - 1] = '\0';
        }
    }
}

/* The map list itself: the ListBox rect trimmed at the scrollbar so the
 * BattleBar art stays visible next to the rows. */
static SDL_Rect maplist_rect(void) {
    SDL_Rect r = { 210, 275, 348, 112 };
    const GUIWidget *lb = GUIDialog_FindByName(&bs.dialog, "MapList");
    const GUIWidget *sl = (bs.idx_maplist_track >= 0)
                          ? &bs.dialog.children[bs.idx_maplist_track] : NULL;
    if (lb) r = lb->rect;
    if (sl && sl->rect.x > r.x) r.w = sl->rect.x - r.x;
    return r;
}

static int maplist_rows_visible(void) {
    int rows = maplist_rect().h / BS_MAP_ROW_HEIGHT;
    return rows > 0 ? rows : 1;
}

static int maplist_max_scroll(void) {
    int max = bs.num_maps - maplist_rows_visible();
    return max > 0 ? max : 0;
}

/* Travel band for the ScrollThumb: the BattleBar art, which the hotspot
 * places between the two nubs. */
static void maplist_thumb_travel(SDL_Rect *track, int *thumb_h) {
    SDL_Rect thumb;
    bs_draw_rect(bs.idx_maplist_track, track);
    bs_draw_rect(bs.idx_maplist_thumb, &thumb);
    *thumb_h = thumb.h > 0 ? thumb.h : 1;
}

/* Move the map list's scrollbar thumb (ScrollThumb sprite) to reflect
 * the current map_scroll position. */
static void sync_maplist_thumb(void) {
    if (bs.idx_maplist_thumb < 0 || bs.idx_maplist_track < 0) return;
    if (bs.dragging_maplist) return;      /* the drag owns the position */
    SDL_Rect track, thumb;
    int thumb_h = 1;
    maplist_thumb_travel(&track, &thumb_h);
    bs_draw_rect(bs.idx_maplist_thumb, &thumb);
    int travel = track.h - thumb_h;
    if (travel < 0) travel = 0;
    int max_scroll = maplist_max_scroll();
    GUIWidget *w = &bs.dialog.children[bs.idx_maplist_thumb];
    int rel = max_scroll > 0 ? (travel * bs.map_scroll) / max_scroll : 0;
    /* Rect, not draw position: the renderer re-applies the hotspot. */
    w->rect.y = track.y + rel + (w->rect.y - thumb.y);
}

/* Convert mouse X to a units value, snapped to step. */
static int units_from_mouse_x(int mx) {
    SDL_Rect bar, thumb;
    bs_draw_rect(bs.idx_units_track, &bar);
    bs_draw_rect(bs.idx_units_thumb, &thumb);
    int travel = bar.w - thumb.w;
    if (travel < 1) travel = 1;
    int rel = mx - (bar.x + thumb.w / 2);
    if (rel < 0) rel = 0;
    if (rel > travel) rel = travel;
    int range = TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN;
    int raw   = TAK_UNITS_PER_SIDE_MIN + (range * rel) / travel;
    int step  = TAK_UNITS_PER_SIDE_STEP;
    int snap  = ((raw + step / 2) / step) * step;
    return clamp_units(snap);
}

static void clamp_map_scroll(void) {
    int max = maplist_max_scroll();
    if (bs.map_scroll > max) bs.map_scroll = max;
    if (bs.map_scroll < 0)   bs.map_scroll = 0;
}

/* --skirmish: press Play on the first tick with the default lineup. Kept
 * outside bs so Init's reset can't clear it. */
static int s_autostart = 0;

void BattleSetup_RequestAutoStart(void) { s_autostart = 1; }

int BattleSetup_Tick(TAK_Platform *platform, float frame_dt) {
    (void)frame_dt;
    if (!bs.initialized) return GAMESTATE_BATTLE_SETUP;

    if (s_autostart) {
        s_autostart = 0;
        if (bs.selected_map >= 0 && bs.num_maps > 0 &&
            World_BeginLoad(platform, &bs.cfg, bs.maps[bs.selected_map], bs.map_kingdom) == 0) {
            bs.pending_nextstate = GAMESTATE_GAME_LOADING;
        } else {
            fprintf(stderr, "BattleSetup: autostart could not begin a skirmish (%d maps)\n", bs.num_maps);
        }
    }

    int wx, wy, mx, my;
    SDL_GetMouseState(&wx, &wy);
    if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
        mx = -1; my = -1;
    }
    int mouse_state = SDL_GetMouseState(NULL, NULL);
    int mouse_left = mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT);

    /* ── Slider drag handling ─────────────────────────────────────
     * Start dragging when the mouse is pressed over the slider track
     * or thumb; keep updating the value while held; stop on release. */
    {
        SDL_Point pt = { mx, my };
        SDL_Rect bar;
        bs_draw_rect(bs.idx_units_track, &bar);
        /* Expand the hit zone vertically a few pixels so grabbing the
         * thumb is forgiving. */
        SDL_Rect drag_zone = { bar.x - 4, bar.y - 4, bar.w + 8, bar.h + 8 };
        if (mouse_left && !bs.dragging_slider && SDL_PointInRect(&pt, &drag_zone)) {
            bs.dragging_slider = 1;
        }
        if (!mouse_left) bs.dragging_slider = 0;
        if (bs.dragging_slider) {
            bs.cfg.units_per_side = units_from_mouse_x(mx);
        }
    }

    /* ── Map list thumb drag ──────────────────────────────────────
     * Grab the ScrollThumb and the list follows the pointer. The thumb
     * travels only inside the BattleBar art, between the two nubs. */
    if (bs.idx_maplist_thumb >= 0 && bs.idx_maplist_track >= 0) {
        SDL_Point pt = { mx, my };
        SDL_Rect thumb_draw;
        bs_draw_rect(bs.idx_maplist_thumb, &thumb_draw);
        if (mouse_left && !bs.dragging_maplist &&
            SDL_PointInRect(&pt, &thumb_draw)) {
            bs.dragging_maplist = 1;
            bs.maplist_grab_dy  = my - thumb_draw.y;
        }
        if (!mouse_left) bs.dragging_maplist = 0;
        if (bs.dragging_maplist) {
            SDL_Rect track;
            int thumb_h = 1;
            maplist_thumb_travel(&track, &thumb_h);
            int travel = track.h - thumb_h;
            int max_scroll = maplist_max_scroll();
            if (travel > 0 && max_scroll > 0) {
                int rel = my - bs.maplist_grab_dy - track.y;
                if (rel < 0) rel = 0;
                if (rel > travel) rel = travel;
                bs.map_scroll = (rel * max_scroll + travel / 2) / travel;
                clamp_map_scroll();
                GUIWidget *tw = &bs.dialog.children[bs.idx_maplist_thumb];
                tw->rect.y = track.y + rel + (tw->rect.y - thumb_draw.y);
            }
        }
    }

    char clicked[64];
    int  clicked_idx = -1;
    /* Suppress click routing while dragging so the slider doesn't trigger
     * widget actions. */
    int  suppress_click = bs.dragging_slider || bs.dragging_maplist;
    int  got_click = suppress_click
                     ? (GUIRuntime_UpdateEx(bs.rt, mx, my, mouse_left,
                                             clicked, sizeof(clicked),
                                             &clicked_idx), 0)
                     : GUIRuntime_UpdateEx(bs.rt, mx, my, mouse_left,
                                             clicked, sizeof(clicked),
                                             &clicked_idx);
    if (got_click) {
        if      (tak_stricmp(clicked, "Previous") == 0) bs.pending_nextstate = GAMESTATE_MENU;
        else if (tak_stricmp(clicked, "Play")     == 0) {
            if (bs.selected_map < 0 || bs.num_maps == 0) {

            } else {
                if (World_BeginLoad(platform, &bs.cfg, bs.maps[bs.selected_map], bs.map_kingdom) == 0) {
                    bs.pending_nextstate = GAMESTATE_GAME_LOADING;
                } else {
                    fprintf(stderr, "BattleSetup: Unable to begin world loading\n");
                }
            }
        } else if (tak_stricmp(clicked, "Options")  == 0) bs.pending_nextstate = GAMESTATE_OPTIONS;
        else if (tak_stricmp(clicked, "LoadSkirmish") == 0) {
            fprintf(stderr, "BattleSetup: Load Game (TODO)\n");
        }
        else if (handle_slot_click(clicked, clicked_idx)) { /* done */ }
        else if (handle_slider_click(clicked, mx)) { /* done */ }
        else if (clicked_idx == bs.idx_units_inc) {
            bs.cfg.units_per_side = clamp_units(
                bs.cfg.units_per_side + TAK_UNITS_PER_SIDE_STEP);
        }
        else if (clicked_idx == bs.idx_units_dec) {
            bs.cfg.units_per_side = clamp_units(
                bs.cfg.units_per_side - TAK_UNITS_PER_SIDE_STEP);
        }
        else if (clicked_idx == bs.idx_maplist_inc) {
            /* Top nub scrolls the list up one row. */
            bs.map_scroll--;
            clamp_map_scroll();
        }
        else if (clicked_idx == bs.idx_maplist_dec) {
            bs.map_scroll++;
            clamp_map_scroll();
        }
        else {
            /* Checkbox option */
            int *v = bs_cfg_field_by_widget(clicked);
            if (v) *v = !*v;
        }
    }

    /* Map list clicks — detect by rect lookup since the list isn't a
     * simple button widget. */
    static int prev_mouse_left = 0;
    if (!mouse_left && prev_mouse_left && !suppress_click) {
        SDL_Rect maplist = maplist_rect();
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &maplist)) {
            int row = (my - maplist.y) / BS_MAP_ROW_HEIGHT;
            int idx = bs.map_scroll + row;
            if (idx >= 0 && idx < bs.num_maps) BattleSetup_SelectMap(idx);
        }

        /* Track above/below the thumb pages the list, the way a scrollbar
         * gutter does. */
        SDL_Rect track, thumb_draw;
        int thumb_h = 1;
        maplist_thumb_travel(&track, &thumb_h);
        bs_draw_rect(bs.idx_maplist_thumb, &thumb_draw);
        if (bs.idx_maplist_track >= 0 && SDL_PointInRect(&pt, &track)) {
            int rows_visible = maplist_rows_visible();
            if (my < thumb_draw.y)                bs.map_scroll -= rows_visible;
            else if (my >= thumb_draw.y + thumb_h) bs.map_scroll += rows_visible;
            clamp_map_scroll();
        }
    }
    prev_mouse_left = mouse_left;

    /* Also accept mouse-wheel over the map list. */
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
        SDL_Rect maplist = maplist_rect();
        SDL_Rect track;
        int thumb_h = 1;
        maplist_thumb_travel(&track, &thumb_h);
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &maplist) || SDL_PointInRect(&pt, &track)) {
            bs.map_scroll -= e.wheel.y;
            clamp_map_scroll();
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) bs.pending_nextstate = GAMESTATE_MENU;

    sync_checkbox_visuals();
    sync_slider_thumb_position();
    sync_units_label_text();
    sync_map_description();
    sync_maplist_thumb();

    /* ── Render ──────────────────────────────────────────────────── */
    GUIRuntime_Render(bs.rt);
    SDL_Surface *off = UI_Offscreen();

    /* Slow Game shares row y=193 with Units and the .gui authors it
     * invisible, so the loader's visibility flag is what keeps the two
     * labels from overprinting (legacy:312621). Nothing to do here. */

    /* Overlay per-slot text on top of the rendered widgets. The .gui
     * gave each row widget a template string ("Player", "Side", …); we
     * override with live BattleConfig values. */
    if (bs.font_small) {
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            int y = bs_slot_row_y[i];
            PlayerSlot *ps = &bs.cfg.players[i];

            /* Opaque strip to cover the template text. */
            SDL_Rect clear = { 66, y, 310, 20 };
            SDL_FillRect(off, &clear, SDL_MapRGBA(off->format, 28, 20, 12, 255));

            Font_DrawString(bs.font_small, off, 70, y, slot_name_string(ps, i));
            Font_DrawString(bs.font_small, off, 196, y, side_name_string(ps));
            if (ps->kind != TAK_SLOT_CLOSED && ps->team > 0) {
                char team[8]; snprintf(team, sizeof(team), "Team %d", ps->team);
                Font_DrawString(bs.font_small, off, 317, y, team);
            }
            /* Color badge at the PlayerColor column (.gui rect centered
             * at x≈294, one per row). Drawn only for non-empty slots. */
            if (ps->kind != TAK_SLOT_CLOSED) {
                /* Badge is drawn at the real PlayerColor cell so the
                 * thing you click is the thing that changes; the old
                 * hardcoded x could sit outside the clickable rect. */
                int bx = 294, by = y + 8;
                for (int wi = 0; wi < bs.dialog.num_children; wi++) {
                    const GUIWidget *cw = &bs.dialog.children[wi];
                    if (tak_stricmp(cw->name, "PlayerColor") != 0) continue;
                    if (bs_slot_index_from_rect(&cw->rect) != i) continue;
                    bx = cw->rect.x + cw->rect.w / 2;
                    by = cw->rect.y + cw->rect.h / 2;
                    break;
                }
                draw_color_badge(off, bx, by, ps->color, ps->side);
            }
        }
    }

    /* The NumberOfUnits label and sbutton sprite are now drawn by the
     * generic renderer using their live-updated display_text and rect. */

     {
        SDL_Rect preview = { 67, 276, 110, 110 };
        /* Background is black — the minimap's unused portion letterboxes
         * against it. If the minimap fills the preview, this is unseen. */
        uint32_t bg = SDL_MapRGBA(off->format, 0, 0, 0, 255);
        SDL_FillRect(off, &preview, bg);
        SDL_Rect top = preview;   top.h = 1;
        SDL_Rect bot = preview;   bot.y += preview.h - 1; bot.h = 1;
        SDL_Rect lf  = preview;   lf.w = 1;
        SDL_Rect rt2 = preview;   rt2.x += preview.w - 1; rt2.w = 1;
        uint32_t border = SDL_MapRGBA(off->format, 90, 70, 40, 255);
        SDL_FillRect(off, &top, border);
        SDL_FillRect(off, &bot, border);
        SDL_FillRect(off, &lf,  border);
        SDL_FillRect(off, &rt2, border);

        // Minimap
        if (bs.tnt.minimap_rgba) {
            /* The minimap buffer is always 126x126, but the actual map
             * content within it is sized to the map's aspect ratio and
             * top-left anchored; unused rows/cols are palette-index 0
             * (padding). Crop to just the content rect, then fit it into
             * the preview preserving aspect, letterboxed with black. */
            int mw = bs.tnt.minimap_w, mh = bs.tnt.minimap_h;
            int map_w = bs.tnt.width_tiles  > 0 ? bs.tnt.width_tiles  : mw;
            int map_h = bs.tnt.height_tiles > 0 ? bs.tnt.height_tiles : mh;
            int content_w, content_h;
            if (map_w >= map_h) {
                content_w = mw;
                content_h = (mh * map_h + map_w / 2) / map_w;
            } else {
                content_w = (mw * map_w + map_h / 2) / map_h;
                content_h = mh;
            }
            if (content_w < 1) content_w = 1;
            if (content_h < 1) content_h = 1;
            if (content_w > mw) content_w = mw;
            if (content_h > mh) content_h = mh;

            int inner_w = preview.w - 2, inner_h = preview.h - 2;
            /* Fit content into inner preview preserving aspect. Use
             * floating-point only to avoid awkward rounding games. */
            int fit_w, fit_h;
            if (content_w * inner_h >= content_h * inner_w) {
                fit_w = inner_w;
                fit_h = (content_h * inner_w + content_w / 2) / content_w;
            } else {
                fit_h = inner_h;
                fit_w = (content_w * inner_h + content_h / 2) / content_h;
            }
            if (fit_w < 1) fit_w = 1;
            if (fit_h < 1) fit_h = 1;
            int off_x = (inner_w - fit_w) / 2;
            int off_y = (inner_h - fit_h) / 2;

            for (int y = 0; y < fit_h; y++) {
                int sy = y * content_h / fit_h;
                for (int x = 0; x < fit_w; x++) {
                    int sx = x * content_w / fit_w;
                    uint32_t px = bs.tnt.minimap_rgba[sy * mw + sx];
                    SDL_Rect p = { preview.x + 1 + off_x + x,
                                   preview.y + 1 + off_y + y, 1, 1 };
                    SDL_FillRect(off, &p, px);
                }
            }
        } else { // Fallback

            /* Simple grid placeholder; cell count from OTA SizeX/SizeY. */
            int gx = bs.map_size_x > 0 ? bs.map_size_x : 6;
            int gy = bs.map_size_y > 0 ? bs.map_size_y : 6;
            if (gx > 32) gx = 32;
            if (gy > 32) gy = 32;
            int cell_w = (preview.w - 2) / gx;
            int cell_h = (preview.h - 2) / gy;
            if (cell_w < 1) cell_w = 1;
            if (cell_h < 1) cell_h = 1;
            uint32_t cell_color = SDL_MapRGBA(off->format, 60, 50, 35, 255);
            uint32_t spacer     = SDL_MapRGBA(off->format, 40, 32, 22, 255);
            for (int y = 0; y < gy; y++) {
                for (int x = 0; x < gx; x++) {
                    SDL_Rect c = {
                        preview.x + 1 + x * cell_w,
                        preview.y + 1 + y * cell_h,
                        cell_w - 1, cell_h - 1
                    };
                    SDL_FillRect(off, &c,
                        ((x + y) & 1) ? cell_color : spacer);
                }
            }
        }
    }

    /* Map list overlay. Rows carry the authored map name and the row
     * pitch is MapNameEntryTemplate's height (legacy:136017 clones that
     * template once per map). The overlay stops short of the scrollbar
     * so the BattleBar art stays visible. */
    if (bs.font_small) {
        SDL_Rect listrect = maplist_rect();
        SDL_FillRect(off, &listrect, SDL_MapRGBA(off->format, 16, 12, 8, 255));
        int rows = maplist_rows_visible();
        for (int r = 0; r < rows; r++) {
            int idx = bs.map_scroll + r;
            if (idx < 0 || idx >= bs.num_maps) break;
            int y = listrect.y + r * BS_MAP_ROW_HEIGHT + 3;
            if (idx == bs.selected_map) {
                SDL_Rect sel = { listrect.x, listrect.y + r * BS_MAP_ROW_HEIGHT,
                                 listrect.w, BS_MAP_ROW_HEIGHT };
                SDL_FillRect(off, &sel, SDL_MapRGBA(off->format, 60, 50, 35, 255));
            }
            Font_DrawString(bs.font_small, off,
                             listrect.x + 6, y, bs.map_display[idx]);
        }
    }


    /* Hovered-widget tooltip in the HelpText strip. */
    if (bs.font_header) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(bs.rt);
        if (hw && hw->tooltip[0]) {
            const GUIWidget *help = GUIDialog_FindByName(&bs.dialog, "HelpText");
            SDL_Rect r = help ? help->rect : (SDL_Rect){ 208, 452, 224, 30 };
            SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 20, 14, 8, 255));
            int tw = Font_MeasureString(bs.font_header, hw->tooltip);
            Font_DrawString(bs.font_header, off,
                             r.x + (r.w - tw) / 2, r.y + 4, hw->tooltip);
        }
    }

    UI_Present(platform);

    int next = (bs.pending_nextstate >= 0) ? bs.pending_nextstate
                                           : GAMESTATE_BATTLE_SETUP;
    bs.pending_nextstate = -1;
    return next;
}
