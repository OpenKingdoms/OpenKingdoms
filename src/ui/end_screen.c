/*
 * end_screen.c -- The victory and defeat statistics screen.
 *
 * Three seconds after the banner the original stops the simulation and
 * opens Defeat.gui, or Victory<side>.gui for the local player's side
 * (legacy:244079-244086, legacy:153765-153776). Each of the eight
 * authored rows shows a player's badge, name, units built, kills,
 * losses, time and score straight from the player record
 * (legacy:153968-154004). Main Menu and Proceed carry their own click
 * sounds in the dialog, Proceed's help text is the mode's destination
 * (legacy:154023-154060), and Enter and Escape press them as the root
 * widget's accelerator string says.
 */

#include "tak_end_screen.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"
#include "tak_blit.h"
#include "tak_ui.h"
#include "tak_gaf.h"
#include "tak_tdf.h"
#include "tak_hpi.h"
#include "tak_unit.h"
#include "tak_game_sound.h"
#include "tak_gameloop.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ES_ROWS TAK_MAX_PLAYERS

typedef struct {
    int panel;                 /* child index of the row's group box  */
    int logo, name, units, kills, losses, time, score;
    int shown;
    char name_text[64];
    char units_text[16], kills_text[16], losses_text[16];
    char time_text[16], score_text[16];
    uint32_t *logo_px;
    int logo_w, logo_h;
} EndRow;

static struct {
    int         open;
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    Font       *font_body;     /* times new roman (100): the numbers   */
    Font       *font_bold;     /* times new roman (100b): help strip   */
    Font       *font_header;   /* lombardic: the column headers        */
    Font       *font_title;    /* decorativesm: Victory / Defeat       */
    EndRow      rows[ES_ROWS];
    char      (*authored)[128];  /* each label's authored text */
    char        path[128];
    char        enter_widget[32];
    char        esc_widget[32];
    char        last_sound[64];
    int         pending_state;
    int         prev_enter;
    int         prev_esc;
} es;

static void set_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

/* sidedata.tdf names each side's prefix (ARA, TAR, ...). The victory
 * dialog and the badge sequence both hang off it (legacy:153773,
 * legacy:153975). */
static void side_prefix(int side, char *out, size_t cap) {
    static const char *const fallback[] = { "ARA", "TAR", "VER", "ZON" };
    set_text(out, cap, (side >= 0 && side < 4) ? fallback[side] : "ARA");
    TDFFile *tdf = TDF_Open("data/gamedata/sidedata.tdf");
    if (!tdf) return;
    if (TDF_Load(tdf) == 0) {
        char section[16];
        snprintf(section, sizeof(section), "SIDE%d", side);
        if (TDF_PushSection(tdf, section) == 0) {
            const char *p = TDF_ReadString(tdf, "nameprefix", "");
            if (p && *p) set_text(out, cap, p);
            TDF_PopSection(tdf);
        }
    }
    TDF_Close(tdf);
}

/* The translate table entry the original puts under Proceed for a
 * skirmish (legacy:154043). The key itself is the fallback
 * (legacy:267931). messages.tdf carries text with stray separators
 * that the TDF validator counts, so the table is scanned as text:
 * the [KEY] line, then the English value inside its braces. */
static int ci_prefix(const char *p, const char *word) {
    for (; *word; p++, word++) {
        char a = *p, b = *word;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void translate_lookup(const char *key, char *out, size_t cap) {
    set_text(out, cap, key);
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile("english/translate/messages.tdf", &data, &size) != 0 || !data) return;
    char *text = (char *)tak_malloc((size_t)size + 1);
    if (!text) { VFS_FreeBuffer(data); return; }
    memcpy(text, data, size);
    text[size] = '\0';
    VFS_FreeBuffer(data);

    size_t klen = strlen(key);
    char *p = text;
    while ((p = strchr(p, '[')) != NULL) {
        p++;
        if (!ci_prefix(p, key) || p[klen] != ']') continue;
        char *brace = strchr(p, '{');
        char *close = brace ? strchr(brace, '}') : NULL;
        if (!brace || !close) break;
        for (char *q = brace; q < close; q++) {
            if (!(q == brace + 1 || q[-1] == '\n' || q[-1] == '\t' || q[-1] == ' ')) continue;
            if (!ci_prefix(q, "English")) continue;
            char *eq = strchr(q, '=');
            char *semi = eq ? strchr(eq, ';') : NULL;
            if (!eq || !semi || semi > close) break;
            eq++;
            while (*eq == ' ' || *eq == '\t') eq++;
            size_t n = (size_t)(semi - eq);
            while (n > 0 && (eq[n - 1] == ' ' || eq[n - 1] == '\t' || eq[n - 1] == '\r')) n--;
            if (n > 0) {
                if (n > cap - 1) n = cap - 1;
                memcpy(out, eq, n);
                out[n] = '\0';
            }
            break;
        }
        break;
    }
    tak_free(text);
}

/* Root tooltip "#Enter#Proceed#Esc#MainMenu": key, widget pairs. */
static void parse_accelerators(const char *spec) {
    es.enter_widget[0] = es.esc_widget[0] = '\0';
    if (!spec) return;
    char buf[128];
    set_text(buf, sizeof(buf), spec);
    char *p = buf;
    while (*p == '#') p++;
    while (*p) {
        char *key = p;
        char *sep = strchr(key, '#');
        if (!sep) break;
        *sep = '\0';
        char *widget = sep + 1;
        char *next = strchr(widget, '#');
        if (next) *next = '\0';
        if (tak_stricmp(key, "Enter") == 0) set_text(es.enter_widget, sizeof(es.enter_widget), widget);
        if (tak_stricmp(key, "Esc") == 0)   set_text(es.esc_widget, sizeof(es.esc_widget), widget);
        if (!next) break;
        p = next + 1;
    }
}

/* Badge from teamlogos.gaf "<prefix>team", frame colour + 2: the sheet
 * leads with two greyed states (legacy:153976, legacy:136390). */
static void load_badge(GAFFile *gaf, const uint32_t *table,
                       const char *prefix, int color, EndRow *row) {
    char seq[64];
    snprintf(seq, sizeof(seq), "%steam", prefix);
    int entry_off = GAF_FindSequence(gaf, seq);
    if (entry_off < 0) return;
    if ((uint32_t)entry_off + sizeof(EntryHeader) > gaf->data_size) return;
    const EntryHeader *eh = (const EntryHeader *)(gaf->data + entry_off);
    int nframes = (int)eh->num_frames;
    int frame = (nframes >= TAK_PLAYER_COLOR_COUNT + 2) ? color + 2 : color;
    if (frame < 0 || frame >= nframes) return;
    row->logo_px = UI_DecodeFrame(gaf, entry_off, frame, table,
                                  &row->logo_w, &row->logo_h);
}

/* Map the flat widget list onto rows: a group box named "0".."7"
 * opens a row and the labels after it belong to that row, the way the
 * original looks each up as a child of the box (legacy:153962). */
static void map_rows(void) {
    for (int r = 0; r < ES_ROWS; r++) {
        EndRow *row = &es.rows[r];
        row->panel = row->logo = row->name = row->units = -1;
        row->kills = row->losses = row->time = row->score = -1;
    }
    int cur = -1;
    for (int i = 0; i < es.dialog.num_children; i++) {
        const GUIWidget *w = &es.dialog.children[i];
        if (w->type == GUI_WT_PANEL && w->name[0] >= '0' && w->name[0] <= '9' &&
            w->name[1] == '\0') {
            cur = w->name[0] - '0';
            if (cur >= 0 && cur < ES_ROWS) es.rows[cur].panel = i;
            continue;
        }
        if (cur < 0 || cur >= ES_ROWS || w->type != GUI_WT_LABEL) continue;
        EndRow *row = &es.rows[cur];
        if      (tak_stricmp(w->name, "Logo") == 0)       row->logo = i;
        else if (tak_stricmp(w->name, "PlayerName") == 0) row->name = i;
        else if (tak_stricmp(w->name, "UnitsBuilt") == 0) row->units = i;
        else if (tak_stricmp(w->name, "Kills") == 0)      row->kills = i;
        else if (tak_stricmp(w->name, "Losses") == 0)     row->losses = i;
        else if (tak_stricmp(w->name, "Time") == 0)       row->time = i;
        else if (tak_stricmp(w->name, "Score") == 0)      row->score = i;
        else cur = -1;   /* the headers and title end the row block */
    }
}

static void set_row_visible(EndRow *row, int visible) {
    int idx[8] = { row->panel, row->logo, row->name, row->units,
                   row->kills, row->losses, row->time, row->score };
    for (int k = 0; k < 8; k++) {
        if (idx[k] >= 0) GUIRuntime_SetWidgetVisibleAt(es.rt, idx[k], visible);
    }
}

static void fill_rows(const GameWorld *world) {
    GAFFile *gaf = NULL;
    uint32_t table[256];
    int have_gaf = UI_LoadGAFWithPalette("data/anims/teamlogos.gaf",
                                         "data/anims/teamlogos.pcx",
                                         &gaf, table) == 0;
    for (int r = 0; r < ES_ROWS; r++) {
        EndRow *row = &es.rows[r];
        const PlayerSlot *slot = &world->cfg.players[r];
        const PlayerBattleStats *st = &world->stats[r + 1];
        /* A row shows for every slot that built something
         * (legacy:153968). */
        row->shown = slot->kind != TAK_SLOT_CLOSED && st->units_built > 0;
        if (!row->shown) {
            set_row_visible(row, 0);
            continue;
        }
        set_text(row->name_text, sizeof(row->name_text),
                 slot->name[0] ? slot->name : "Player");
        snprintf(row->units_text, sizeof(row->units_text), "%d", st->units_built);
        snprintf(row->kills_text, sizeof(row->kills_text), "%d", st->kills);
        snprintf(row->losses_text, sizeof(row->losses_text), "%d", st->losses);
        snprintf(row->score_text, sizeof(row->score_text), "%d", st->score);
        /* The player's last tick alive as hh:mm:ss (legacy:153993-153997
         * at 30 Hz, 60 here). */
        int secs = st->last_alive_tick / 60;
        snprintf(row->time_text, sizeof(row->time_text), "%02d:%02d:%02d",
                 secs / 3600, (secs % 3600) / 60, secs % 60);
        if (have_gaf) {
            char prefix[16];
            side_prefix(slot->side, prefix, sizeof(prefix));
            load_badge(gaf, table, prefix, slot->color, row);
        }
        /* The runtime would print the authored placeholders top-left;
         * the rows are drawn here, in their columns. */
        int idx[7] = { row->logo, row->name, row->units, row->kills,
                       row->losses, row->time, row->score };
        for (int k = 0; k < 7; k++) {
            if (idx[k] >= 0) GUIRuntime_SetWidgetTextAt(es.rt, idx[k], "");
        }
    }
    if (gaf) GAF_Close(gaf);
}

int EndScreen_Open(TAK_Platform *platform, const GameWorld *world) {
    (void)platform;
    if (!world) return -1;
    EndScreen_Close();
    memset(&es, 0, sizeof(es));
    es.pending_state = GAMESTATE_IN_GAME;

    if (world->skirmish_local_result > 0) {
        char prefix[16];
        side_prefix(world->cfg.players[0].side, prefix, sizeof(prefix));
        for (char *p = prefix; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }
        snprintf(es.path, sizeof(es.path), "data/guis/victory%s.gui", prefix);
    } else {
        set_text(es.path, sizeof(es.path), "data/guis/defeat.gui");
    }
    if (GUIDialog_Load(&es.dialog, es.path) != 0) {
        fprintf(stderr, "EndScreen: failed to load %s\n", es.path);
        es.path[0] = '\0';
        return -1;
    }
    es.has_dialog = 1;
    es.rt = GUIRuntime_Create(&es.dialog);
    if (!es.rt) { EndScreen_Close(); return -1; }

    es.font_body   = Font_Load("data/fonts/b_times new roman (100)",  UI_RGBAFormat());
    es.font_bold   = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    es.font_header = Font_Load("data/fonts/lombardic (cd)",           UI_RGBAFormat());
    es.font_title  = Font_Load("data/fonts/decorativesm",             UI_RGBAFormat());

    parse_accelerators(es.dialog.root.tooltip);
    map_rows();
    fill_rows(world);

    /* Proceed leads back to the skirmish battle room; the original
     * writes that as its help text (legacy:154043-154060). */
    GUIWidget *proceed = GUIDialog_FindByName(&es.dialog, "Proceed");
    if (proceed) {
        translate_lookup("SKIRMISH_BATTLE_ROOM", proceed->tooltip,
                         sizeof(proceed->tooltip));
    }
    /* Headers, title and help strip are drawn here too, so their
     * placement follows the authored alignment. Keep the authored
     * text; the runtime's copy is blanked so it draws none itself. */
    es.authored = (char (*)[128])tak_malloc(
        (size_t)(es.dialog.num_children > 0 ? es.dialog.num_children : 1)
        * sizeof(*es.authored));
    if (!es.authored) { EndScreen_Close(); return -1; }
    for (int i = 0; i < es.dialog.num_children; i++) {
        set_text(es.authored[i], sizeof(es.authored[i]),
                 es.dialog.children[i].display_text);
        if (es.dialog.children[i].type == GUI_WT_LABEL)
            GUIRuntime_SetWidgetTextAt(es.rt, i, "");
    }
    es.open = 1;
    return 0;
}

void EndScreen_Close(void) {
    for (int r = 0; r < ES_ROWS; r++) {
        if (es.rows[r].logo_px) tak_free(es.rows[r].logo_px);
        es.rows[r].logo_px = NULL;
    }
    if (es.authored)    tak_free(es.authored);
    if (es.rt)          GUIRuntime_Destroy(es.rt);
    if (es.has_dialog)  GUIDialog_Free(&es.dialog);
    if (es.font_body)   Font_Free(es.font_body);
    if (es.font_bold)   Font_Free(es.font_bold);
    if (es.font_header) Font_Free(es.font_header);
    if (es.font_title)  Font_Free(es.font_title);
    memset(&es, 0, sizeof(es));
}

int EndScreen_IsOpen(void) { return es.open; }

/* Draw text in a widget's rect: centred unless the widget authored the
 * left-aligned form (text_align 1, the "Player" header). */
static void draw_in_rect(Font *f, SDL_Rect r, int align, const char *text) {
    if (!f || !text || !text[0]) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    int tw = Font_MeasureString(f, text);
    int top = 0, bottom = 0;
    if (Font_InkExtent(f, text, &top, &bottom) != 0) return;
    int x = (align == 1) ? r.x + 2 : r.x + (r.w - tw) / 2;
    int y = r.y + (r.h - (bottom - top)) / 2 - top;
    Font_DrawString(f, off, x, y, text);
}

static void draw_label(int index, Font *f, const char *text) {
    if (index < 0 || index >= es.dialog.num_children) return;
    const GUIWidget *w = &es.dialog.children[index];
    draw_in_rect(f, w->rect, w->text_align, text);
}

static void draw_rows(void) {
    SDL_Surface *off = UI_Offscreen();
    for (int r = 0; r < ES_ROWS; r++) {
        const EndRow *row = &es.rows[r];
        if (!row->shown) continue;
        if (row->logo >= 0 && row->logo_px && off) {
            Blit_RGBA_Scaled(off, es.dialog.children[row->logo].rect,
                             row->logo_px, row->logo_w, row->logo_h);
        }
        draw_label(row->name,   es.font_body, row->name_text);
        draw_label(row->units,  es.font_body, row->units_text);
        draw_label(row->kills,  es.font_body, row->kills_text);
        draw_label(row->losses, es.font_body, row->losses_text);
        draw_label(row->time,   es.font_body, row->time_text);
        draw_label(row->score,  es.font_body, row->score_text);
    }
    /* Authored statics: the column headers in lombardic and the big
     * Victory or Defeat in the decorative face. */
    for (int i = 0; es.authored && i < es.dialog.num_children; i++) {
        const GUIWidget *w = &es.dialog.children[i];
        if (w->type != GUI_WT_LABEL || !es.authored[i][0]) continue;
        if (GUIRuntime_WidgetHiddenAt(es.rt, i)) continue;
        int in_row = 0;
        for (int r = 0; r < ES_ROWS && !in_row; r++) {
            const EndRow *row = &es.rows[r];
            in_row = (i == row->logo || i == row->name || i == row->units ||
                      i == row->kills || i == row->losses || i == row->time ||
                      i == row->score);
        }
        if (in_row) continue;
        Font *f = es.font_header;
        if (strstr(w->font, "decorativesm") || strstr(w->font, "Decorativesm"))
            f = es.font_title;
        else if (strstr(w->font, "100b"))
            f = es.font_bold;
        draw_in_rect(f, w->rect, w->text_align, es.authored[i]);
    }
}

static int press_named(const char *name) {
    const GUIWidget *w = name ? GUIDialog_FindByName(&es.dialog, name) : NULL;
    if (!w) return GAMESTATE_IN_GAME;
    if (w->sound[0]) {
        set_text(es.last_sound, sizeof(es.last_sound), w->sound);
        GameSound_PlayUI(w->sound);
    }
    if (tak_stricmp(name, "MainMenu") == 0) return GAMESTATE_MENU;
    if (tak_stricmp(name, "Proceed") == 0)  return GAMESTATE_BATTLE_SETUP;
    return GAMESTATE_IN_GAME;
}

int EndScreen_Tick(TAK_Platform *platform, const GameWorld *world) {
    (void)world;
    if (!es.open || !es.rt) return GAMESTATE_MENU;
    if (es.pending_state != GAMESTATE_IN_GAME) return es.pending_state;

    int wx = 0, wy = 0, mx = -1, my = -1;
    int mouse_down = 0;
    if (platform && platform->has_focus) {
        uint32_t buttons = SDL_GetMouseState(&wx, &wy);
        mouse_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
            mx = -1; my = -1;
        }
    }
    char clicked[64];
    int got = GUIRuntime_Update(es.rt, mx, my, mouse_down, clicked, sizeof(clicked));

    /* Keyboard: the root's "#Enter#Proceed#Esc#MainMenu". */
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
    int esc   = keys[SDL_SCANCODE_ESCAPE];
    const char *key_widget = NULL;
    if (enter && !es.prev_enter && es.enter_widget[0]) key_widget = es.enter_widget;
    if (esc && !es.prev_esc && es.esc_widget[0])       key_widget = es.esc_widget;
    es.prev_enter = enter;
    es.prev_esc = esc;

    GUIRuntime_Render(es.rt);
    draw_rows();

    /* Help strip: the hovered button's tooltip (legacy:46918). */
    const GUIWidget *hover = GUIRuntime_HoveredWidget(es.rt);
    const GUIWidget *help = GUIDialog_FindByName(&es.dialog, "HelpText");
    if (hover && help && hover->tooltip[0]) {
        draw_in_rect(es.font_bold, help->rect, 0, hover->tooltip);
    }

    if (got) es.pending_state = press_named(clicked);
    else if (key_widget) es.pending_state = press_named(key_widget);
    return es.pending_state;
}

/* ── Introspection ─────────────────────────────────────────────────── */

const char *EndScreen_DialogPath(void) { return es.open ? es.path : ""; }

int EndScreen_RowShown(int slot) {
    if (!es.open || slot < 0 || slot >= ES_ROWS) return 0;
    return es.rows[slot].shown;
}

int EndScreen_RowText(int slot, const char *column, char *out, size_t cap) {
    if (!es.open || slot < 0 || slot >= ES_ROWS || !column || !out || !cap) return -1;
    const EndRow *row = &es.rows[slot];
    const char *src = NULL;
    if      (tak_stricmp(column, "PlayerName") == 0) src = row->name_text;
    else if (tak_stricmp(column, "UnitsBuilt") == 0) src = row->units_text;
    else if (tak_stricmp(column, "Kills") == 0)      src = row->kills_text;
    else if (tak_stricmp(column, "Losses") == 0)     src = row->losses_text;
    else if (tak_stricmp(column, "Time") == 0)       src = row->time_text;
    else if (tak_stricmp(column, "Score") == 0)      src = row->score_text;
    if (!src) return -1;
    set_text(out, cap, src);
    return 0;
}

const char *EndScreen_ButtonHelp(const char *button) {
    if (!es.open || !button) return "";
    const GUIWidget *w = GUIDialog_FindByName(&es.dialog, button);
    return w ? w->tooltip : "";
}

int EndScreen_Press(const char *button) {
    if (!es.open) return GAMESTATE_MENU;
    es.pending_state = press_named(button);
    return es.pending_state;
}

const char *EndScreen_LastSound(void) { return es.last_sound; }
