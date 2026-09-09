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
 *   - Map list scans data/extracted/maps/Maps/*.ota and lets the user
 *     pick one; the selected name shows above the list.
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
#define BS_MAP_ROW_HEIGHT  20

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

    /* colorlogos.gaf frames. Entries 0..3 are AraTeam/TarTeam/VerTeam/
     * ZonTeam; each entry has 12 frames (one per player color). So the
     * badge for a slot = entry[side].frame[color]. */
    GAFFile     *colorlogos_gaf;
    uint32_t     colorlogos_rgba[256];
    uint32_t    *colorlogo_frames[4][12];   /* [side][color]           */
    int          colorlogo_w[4][12];
    int          colorlogo_h[4][12];

    /* Map list state. */
    char  maps[BS_MAX_MAPS][80];
    int   num_maps;
    int   selected_map;     /* index into maps[], or -1 */
    int   map_scroll;       /* top visible row */

    /* Per-selected-map metadata parsed from its .ota. */
    int   map_size_x;       /* map width in grid squares */
    int   map_size_y;
    int   map_max_players;
    char  map_kingdom[32];  /* lowercased faction name from OTA kingdom= */

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
    int   idx_maplist_thumb;       /* map list ScrollThumb */
    int   idx_maplist_inc, idx_maplist_dec;
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

/* 12-entry player color palette, matching the legacy assignments
 * (Blue / Red / Green / Yellow / Cyan / Magenta / Orange / White /
 * Dark Blue / Dark Red / Dark Green / Grey). Indexed by PlayerSlot.color. */
static const uint8_t bs_player_colors[12][3] = {
    {  60, 100, 220 },  /* Aramon default — blue   */
    { 210,  50,  50 },  /* Taros default  — red    */
    {  60, 180,  80 },  /* Veruna default — green  */
    { 220, 200,  80 },  /* Zhon default   — yellow */
    {  60, 200, 220 },  /* cyan    */
    { 210, 100, 200 },  /* magenta */
    { 230, 140,  60 },  /* orange  */
    { 230, 230, 230 },  /* white   */
    {  40,  60, 140 },  /* dark blue */
    { 140,  30,  30 },  /* dark red  */
    {  40, 110,  50 },  /* dark green*/
    { 140, 140, 140 },  /* grey      */
};

/* Draws the per-slot player emblem using the colorlogos.gaf sprite
 * matching (side, color). Falls back to a plain colored square if the
 * GAF frame isn't available. */
static void draw_color_badge(SDL_Surface *off, int cx, int cy,
                             int color_idx, int side) {
    if (color_idx < 0 || color_idx >= 12) color_idx = 0;
    if (side      < 0 || side      >= 4)  side      = 0;
    uint32_t *pixels = bs.colorlogo_frames[side][color_idx];
    int w = bs.colorlogo_w[side][color_idx];
    int h = bs.colorlogo_h[side][color_idx];
    if (!pixels || w <= 0 || h <= 0) {
        uint32_t col = SDL_MapRGBA(off->format,
                                    bs_player_colors[color_idx][0],
                                    bs_player_colors[color_idx][1],
                                    bs_player_colors[color_idx][2],
                                    255);
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

/* Title-case a name if every alphabetic character is uppercase. "CASTLE"
 * -> "Castle"; "Angvir's Maze" is left alone. Operates in place. */
static void title_case_if_all_upper(char *s) {
    int has_lower = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') { has_lower = 1; break; }
    }
    if (has_lower) return;

    int at_word_start = 1;
    for (char *p = s; *p; p++) {
        char c = *p;
        int is_alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!is_alpha) {
            at_word_start = 1;
            continue;
        }
        if (at_word_start) {
            if (c >= 'a' && c <= 'z') *p = (char)(c - 'a' + 'A');
        } else {
            if (c >= 'A' && c <= 'Z') *p = (char)(c - 'A' + 'a');
        }
        at_word_start = 0;
    }
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

static void load_selected_map_metadata(void) {
    bs.map_size_x = bs.map_size_y = bs.map_max_players = 0;
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
            strncpy(bs.maps[bs.num_maps], base, sizeof(bs.maps[bs.num_maps]) - 1);
            bs.maps[bs.num_maps][sizeof(bs.maps[bs.num_maps]) - 1] = '\0';
            strip_ota_ext(bs.maps[bs.num_maps]);
            title_case_if_all_upper(bs.maps[bs.num_maps]);
            bs.num_maps++;
        }
        tak_free(paths[i]);
    }
    tak_free(paths);

    if (bs.num_maps > 0) bs.selected_map = 0;
    fprintf(stderr, "BattleSetup: found %d maps\n", bs.num_maps);
    if (bs.selected_map >= 0) {
        strncpy(bs.cfg.map_name, bs.maps[bs.selected_map],
                sizeof(bs.cfg.map_name) - 1);
        load_selected_map_metadata();
    }
}

/* ── Init / Shutdown ─────────────────────────────────────────────────── */

static void localize_column_headers(void);   /* defined below */

/* Walk the dialog children once and cache which child index is the
 * MaxUnits slider thumb / inc / dec vs the map list's. Multiple widgets
 * share the name "sbutton"/"incbutton"/"decbutton", so we discriminate
 * by rect.y band (done at init, before any of our code has moved them). */
static void cache_scroll_indices(void) {
    bs.idx_units_thumb = bs.idx_units_inc = bs.idx_units_dec = -1;
    bs.idx_maplist_thumb = bs.idx_maplist_inc = bs.idx_maplist_dec = -1;
    for (int i = 0; i < bs.dialog.num_children; i++) {
        const GUIWidget *w = &bs.dialog.children[i];
        int is_thumb = tak_stricmp(w->name, "sbutton") == 0;
        int is_inc   = tak_stricmp(w->name, "incbutton") == 0;
        int is_dec   = tak_stricmp(w->name, "decbutton") == 0;
        if (!(is_thumb || is_inc || is_dec)) continue;

        /* MaxUnits row y ≈ 218; map list row y ≈ 275-387. */
        int is_units = (w->rect.y >= 200 && w->rect.y <= 240);
        if (is_thumb && is_units) bs.idx_units_thumb = i;
        if (is_inc   && is_units) bs.idx_units_inc   = i;
        if (is_dec   && is_units) bs.idx_units_dec   = i;
        if (is_thumb && !is_units) bs.idx_maplist_thumb = i;
        if (is_inc   && !is_units) bs.idx_maplist_inc   = i;
        if (is_dec   && !is_units) bs.idx_maplist_dec   = i;
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
        /* Fall back to colorlogos_rgba... TODO: let's have a good fallback here*/
        memcpy(bs.terrain_rgba, bs.colorlogos_rgba, sizeof(bs.terrain_rgba));
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

    /* colorlogos.gaf — 4 entries (AraTeam, TarTeam, VerTeam, ZonTeam),
     * each with 12 frames keyed by player color. Decode the full 4x12
     * matrix of 17x17 sprites up-front so per-slot rendering is a
     * single blit per row. */
    if (UI_LoadGAFWithPalette("data/anims/colorlogos.gaf",
                              "data/anims/colorlogos.pcx",
                              &bs.colorlogos_gaf,
                              bs.colorlogos_rgba) == 0) {
        int entries = (int)bs.colorlogos_gaf->num_entries;
        if (entries > 4) entries = 4;
        for (int s = 0; s < entries; s++) {
            uint32_t entry_off = *(uint32_t *)(bs.colorlogos_gaf->data + 12 + s * 4);
            for (int c = 0; c < 12; c++) {
                bs.colorlogo_frames[s][c] = UI_DecodeFrame(
                    bs.colorlogos_gaf, entry_off, c,
                    bs.colorlogos_rgba,
                    &bs.colorlogo_w[s][c], &bs.colorlogo_h[s][c]);
            }
        }
    }

    cache_scroll_indices();
    localize_column_headers();
    scan_maps();

    bs.initialized = 1;
    return 0;
}

void BattleSetup_Shutdown(void) {
    if (!bs.initialized) return;
    for (int s = 0; s < 4; s++) {
        for (int c = 0; c < 12; c++) {
            if (bs.colorlogo_frames[s][c]) tak_free(bs.colorlogo_frames[s][c]);
        }
    }
    if (bs.colorlogos_gaf) GAF_Close(bs.colorlogos_gaf);
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
        GUIRuntime_SetFrameOverride(bs.rt,
                                     bs_option_bindings[i].widget_name,
                                     (*v) ? 2 : 0);
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
        /* Legacy authors 10 player colours (blue, red, white, green,
             * navy, maroon, gold, black, orange, brown). */
            if (ps->kind != TAK_SLOT_CLOSED) ps->color = (ps->color + 1) % 10;
        return 1;
    }
    return 0;
}

static int clamp_units(int v) {
    if (v < TAK_UNITS_PER_SIDE_MIN) return TAK_UNITS_PER_SIDE_MIN;
    if (v > TAK_UNITS_PER_SIDE_MAX) return TAK_UNITS_PER_SIDE_MAX;
    return v;
}

/* Handle clicks on the MaxUnits slider. Click position along the bar
 * maps to a value in [TAK_UNITS_PER_SIDE_MIN, TAK_UNITS_PER_SIDE_MAX]
 * rounded to the nearest STEP. Returns 1 if handled. */
static int handle_slider_click(const char *name, int click_x) {
    if (tak_stricmp(name, "MaxUnits") == 0) {
        /* MaxUnits slider rect from .gui line 1375: (415, 218, 157, 13). */
        SDL_Rect bar = { 415, 218, 157, 13 };
        int rel = click_x - bar.x;
        if (rel < 0) rel = 0;
        if (rel > bar.w) rel = bar.w;
        int range = TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN;
        int raw = TAK_UNITS_PER_SIDE_MIN + (range * rel) / bar.w;
        int step = TAK_UNITS_PER_SIDE_STEP;
        /* snap to nearest step */
        int snapped = ((raw + step / 2) / step) * step;
        bs.cfg.units_per_side = clamp_units(snapped);
        return 1;
    }
    return 0;
}

/* MaxUnits slider track rect (from battlemenusingle.gui line 1375).
 * Kept at file scope so the sync helpers below can read it. */
static const SDL_Rect BS_SLIDER_RECT = { 415, 218, 157, 13 };

/* Move the `sbutton` widget (UnitBattleThumb sprite) to the X position
 * corresponding to the current units_per_side. */
static void sync_slider_thumb_position(void) {
    if (bs.idx_units_thumb < 0) return;
    const SDL_Rect bar = BS_SLIDER_RECT;
    int range = TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN;
    int rel   = bar.w * (bs.cfg.units_per_side - TAK_UNITS_PER_SIDE_MIN) / range;
    GUIWidget *w = &bs.dialog.children[bs.idx_units_thumb];
    w->rect.x = bar.x + rel - (w->rect.w / 2);
}

/* Refresh the NumberOfUnits label's display_text so the renderer draws
 * the live value instead of the .gui template ("500"). */
static void sync_units_label_text(void) {
    GUIWidget *w = GUIDialog_FindByName(&bs.dialog, "NumberOfUnits");
    if (!w) return;
    snprintf(w->display_text, sizeof(w->display_text), "%d",
             bs.cfg.units_per_side);
}

/* Replace the template placeholders on the column header labels
 * (_SPName_, _SPSide_, _SPColor_, _SPTeam_) with real column titles.
 * Done once at init — the .gui author used placeholders because the
 * original game substituted them at runtime from a locale table. */
static void localize_column_headers(void) {
    struct { const char *name; const char *text; } pairs[] = {
        { "Name",  "Name"  },
        { "Side",  "Side"  },
        { "Color", "Color" },
        { "Team",  "Team"  },
    };
    /* "Name", "Side", etc. are also widget names shared with in-row
     * widgets (PlayerName etc. use different names). The column-header
     * labels at the top of the player table have widget names that
     * exactly match the column title and sit at y≈30. Match on both. */
    for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++) {
        for (int ch = 0; ch < bs.dialog.num_children; ch++) {
            GUIWidget *w = &bs.dialog.children[ch];
            if (w->type != GUI_WT_LABEL) continue;
            if (w->rect.y < 25 || w->rect.y > 40) continue;
            if (tak_stricmp(w->name, pairs[i].name) != 0) continue;
            strncpy(w->display_text, pairs[i].text,
                    sizeof(w->display_text) - 1);
            w->display_text[sizeof(w->display_text) - 1] = '\0';
            break;
        }
    }

    /* "Game Information" header (widget name is "Team" too, but at a
     * different rect). Find the wider one on the right side. */
    for (int ch = 0; ch < bs.dialog.num_children; ch++) {
        GUIWidget *w = &bs.dialog.children[ch];
        if (w->type != GUI_WT_LABEL) continue;
        if (tak_stricmp(w->name, "Team") == 0 && w->rect.w > 150) {
            strncpy(w->display_text, "Game Information",
                    sizeof(w->display_text) - 1);
            w->display_text[sizeof(w->display_text) - 1] = '\0';
        }
    }

    /* "Map Name" header (widget "Name" with different rect on the
     * map area). */
    for (int ch = 0; ch < bs.dialog.num_children; ch++) {
        GUIWidget *w = &bs.dialog.children[ch];
        if (w->type != GUI_WT_LABEL) continue;
        if (tak_stricmp(w->name, "Name") == 0 && w->rect.y > 200) {
            strncpy(w->display_text, "Map Name",
                    sizeof(w->display_text) - 1);
            w->display_text[sizeof(w->display_text) - 1] = '\0';
        }
    }
}

/* Move the map list's scrollbar thumb (ScrollThumb sprite) to reflect
 * the current map_scroll position. */
static void sync_maplist_thumb(void) {
    if (bs.idx_maplist_thumb < 0) return;
    const SDL_Rect track = { 558, 275, 25, 112 };
    int rows_visible = 112 / BS_MAP_ROW_HEIGHT;
    int max_scroll = bs.num_maps - rows_visible;
    if (max_scroll < 1) return;
    GUIWidget *w = &bs.dialog.children[bs.idx_maplist_thumb];
    int thumb_range = track.h - w->rect.h;
    if (thumb_range < 1) thumb_range = 1;
    w->rect.y = track.y + (thumb_range * bs.map_scroll) / max_scroll;
}

/* Convert mouse X to a units value, snapped to step. */
static int units_from_mouse_x(int mx) {
    int rel = mx - BS_SLIDER_RECT.x;
    if (rel < 0) rel = 0;
    if (rel > BS_SLIDER_RECT.w) rel = BS_SLIDER_RECT.w;
    int range = TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN;
    int raw   = TAK_UNITS_PER_SIDE_MIN + (range * rel) / BS_SLIDER_RECT.w;
    int step  = TAK_UNITS_PER_SIDE_STEP;
    int snap  = ((raw + step / 2) / step) * step;
    return clamp_units(snap);
}

int BattleSetup_Tick(TAK_Platform *platform, float frame_dt) {
    (void)frame_dt;
    if (!bs.initialized) return GAMESTATE_BATTLE_SETUP;

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
        /* Expand the hit zone vertically a few pixels so grabbing the
         * thumb is forgiving. */
        SDL_Rect drag_zone = {
            BS_SLIDER_RECT.x - 4,
            BS_SLIDER_RECT.y - 4,
            BS_SLIDER_RECT.w + 8,
            BS_SLIDER_RECT.h + 8
        };
        if (mouse_left && !bs.dragging_slider && SDL_PointInRect(&pt, &drag_zone)) {
            bs.dragging_slider = 1;
        }
        if (!mouse_left) bs.dragging_slider = 0;
        if (bs.dragging_slider) {
            bs.cfg.units_per_side = units_from_mouse_x(mx);
        }
    }

    char clicked[64];
    int  clicked_idx = -1;
    /* Suppress click routing while dragging so the slider doesn't trigger
     * widget actions. */
    int  got_click = bs.dragging_slider
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
            if (bs.map_scroll > 0) bs.map_scroll--;
        }
        else if (clicked_idx == bs.idx_maplist_dec) {
            int rows = 112 / BS_MAP_ROW_HEIGHT;
            int max = bs.num_maps - rows;
            if (max < 0) max = 0;
            if (bs.map_scroll < max) bs.map_scroll++;
        }
        else {
            /* Checkbox option */
            int *v = bs_cfg_field_by_widget(clicked);
            if (v) *v = !*v;
        }
    }

    /* Map list clicks — detect by rect lookup since the list isn't a
     * simple button widget. Rect from .gui: (210, 275, 373, 112). */
    if (!mouse_left && bs.num_maps > 0) {
        /* nothing — we detect clicks on mouseup below */
    }
    static int prev_mouse_left = 0;
    if (!mouse_left && prev_mouse_left) {
        SDL_Rect maplist = { 210, 275, 340, 112 };
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &maplist)) {
            int row = (my - maplist.y) / BS_MAP_ROW_HEIGHT;
            int idx = bs.map_scroll + row;
            if (idx >= 0 && idx < bs.num_maps) {
                bs.selected_map = idx;
                strncpy(bs.cfg.map_name, bs.maps[idx],
                         sizeof(bs.cfg.map_name) - 1);
                load_selected_map_metadata();
            }
        }

        /* Map list scrollbar track (slider widget at x≈558). Click on
         * the track above the thumb scrolls up; click below scrolls down. */
        SDL_Rect track = { 558, 275, 25, 112 };
        if (SDL_PointInRect(&pt, &track)) {
            int rows_visible = 112 / BS_MAP_ROW_HEIGHT;
            int max_scroll = bs.num_maps - rows_visible;
            if (max_scroll < 0) max_scroll = 0;
            int thumb_y = (max_scroll > 0)
                          ? track.y + (track.h * bs.map_scroll) / (max_scroll + rows_visible)
                          : track.y;
            if (my < thumb_y) {
                bs.map_scroll -= rows_visible;
                if (bs.map_scroll < 0) bs.map_scroll = 0;
            } else {
                bs.map_scroll += rows_visible;
                if (bs.map_scroll > max_scroll) bs.map_scroll = max_scroll;
            }
        }
    }
    prev_mouse_left = mouse_left;

    /* Scrollbar nubs on the map list (incbutton/decbutton declared in
     * the .gui — clicking scrolls the list one row). Handled here
     * because GUIRuntime routes them through the generic click path. */
    if (got_click) {
        if (tak_stricmp(clicked, "incbutton") == 0) {
            /* Try to distinguish MaxUnits slider's incbutton vs map list's.
             * The .gui has two "incbutton" widgets; use rect.y to tell them
             * apart — map list's incbutton is at y=275. */
            const GUIWidget *w = &bs.dialog.children[clicked_idx];
            if (w->rect.y > 200 && w->rect.y < 300) {
                if (bs.map_scroll > 0) bs.map_scroll--;
            }
        } else if (tak_stricmp(clicked, "decbutton") == 0) {
            const GUIWidget *w = &bs.dialog.children[clicked_idx];
            int rows_visible = 112 / BS_MAP_ROW_HEIGHT;
            if (w->rect.y > 300 && w->rect.y < 400) {
                if (bs.map_scroll + rows_visible < bs.num_maps) bs.map_scroll++;
            }
        }
    }

    /* Also accept mouse-wheel over the map list. */
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
        SDL_Rect maplist = { 210, 275, 373, 112 };
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &maplist)) {
            bs.map_scroll -= e.wheel.y;
            int rows = 112 / BS_MAP_ROW_HEIGHT;
            int max = bs.num_maps - rows;
            if (max < 0) max = 0;
            if (bs.map_scroll < 0) bs.map_scroll = 0;
            if (bs.map_scroll > max) bs.map_scroll = max;
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) bs.pending_nextstate = GAMESTATE_MENU;

    sync_checkbox_visuals();
    sync_slider_thumb_position();
    sync_units_label_text();
    sync_maplist_thumb();

    /* ── Render ──────────────────────────────────────────────────── */
    GUIRuntime_Render(bs.rt);
    SDL_Surface *off = UI_Offscreen();

    /* SlowGame: the .gui marks both its label and its checkbox as
     * visible=0 (legacy hid the toggle). The generic renderer draws
     * them anyway because my parser ignores the visibility flag, and
     * sync_checkbox_visuals() already sets the correct frame override
     * via the bs_option_bindings table. Nothing to do here. */

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

    /* Map list overlay. Narrower than the full .gui rect so the
     * scrollbar widgets at x=558 (slider / inc / dec / thumb) stay
     * visible — the list fills only the text column. */
    if (bs.font_small) {
        SDL_Rect listrect = { 210, 275, 340, 112 };
        SDL_FillRect(off, &listrect, SDL_MapRGBA(off->format, 16, 12, 8, 255));
        int rows = listrect.h / BS_MAP_ROW_HEIGHT;
        for (int r = 0; r < rows; r++) {
            int idx = bs.map_scroll + r;
            if (idx < 0 || idx >= bs.num_maps) break;
            int y = listrect.y + r * BS_MAP_ROW_HEIGHT + 2;
            if (idx == bs.selected_map) {
                SDL_Rect sel = { listrect.x, y - 2, listrect.w, BS_MAP_ROW_HEIGHT };
                SDL_FillRect(off, &sel, SDL_MapRGBA(off->format, 60, 50, 35, 255));
            }
            Font_DrawString(bs.font_small, off,
                             listrect.x + 6, y, bs.maps[idx]);
        }
        /* Selected map name above the list. */
        if (bs.selected_map >= 0 && bs.selected_map < bs.num_maps) {
            SDL_Rect clear = { 210, 244, 167, 22 };
            SDL_FillRect(off, &clear, SDL_MapRGBA(off->format, 28, 20, 12, 255));
            Font *f = bs.font_header ? bs.font_header : bs.font_small;
            int tw = Font_MeasureString(f, bs.maps[bs.selected_map]);
            Font_DrawString(f, off, 210 + (167 - tw) / 2, 244,
                             bs.maps[bs.selected_map]);
        }

        /* Map footer line beneath the preview (matches legacy
         * "6 x 6   4 Player   16MB" styling). */
        if (bs.map_size_x > 0 && bs.map_size_y > 0) {
            char footer[64];
            if (bs.map_max_players > 0) {
                snprintf(footer, sizeof(footer), "%d x %d  %d Player",
                         bs.map_size_x, bs.map_size_y, bs.map_max_players);
            } else {
                snprintf(footer, sizeof(footer), "%d x %d",
                         bs.map_size_x, bs.map_size_y);
            }
            int tw = Font_MeasureString(bs.font_small, footer);
            SDL_Rect clear = { 180, 388, 280, 16 };
            SDL_FillRect(off, &clear, SDL_MapRGBA(off->format, 28, 20, 12, 255));
            Font_DrawString(bs.font_small, off,
                             180 + (280 - tw) / 2, 390, footer);
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
