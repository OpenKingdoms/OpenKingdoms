/*
 * game_info.c -- the F1 menu's Game Information dialog.
 *
 * See tak_game_info.h. The tab panels are the original's own files,
 * authored at the top left of the screen and parented into the main
 * dialog's placement guide at run time (legacy:155037-155039), so
 * each is loaded and shifted to sit there. A tab's rows are drawn by
 * hand in the row template's font, the listbox being a list of group
 * boxes the original builds one line at a time.
 */

#include "tak_game_info.h"
#include "tak_briefing.h"
#include "tak_font.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_translate.h"
#include "tak_ui.h"
#include "tak_util.h"
#include "tak_world.h"

#include <stdio.h>
#include <string.h>

#define GI_MAIN      "data/guis/gameinformation.gui"
#define GI_BRIEFING  "data/guis/gameinfobriefing.gui"
#define GI_SETTINGS  "data/guis/gamesettings.gui"
#define GI_ROWS      64
#define GI_ROW_CAP   96
#define GI_ROW_H     20

typedef struct GiPanel {
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    SDL_Rect    list;        /* where the rows go */
    int         text_dx;     /* the row's text, off the list's left */
    int         value_dx;    /* the settings row's value column */
} GiPanel;

static struct {
    int         open;
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    GiPanel     panel;
    char        tab[16];
    Font       *font;
    TranslateTable tt;
    int         tt_loaded;
    int         prev_mouse;
    int         prev_esc;

    /* The tab's rows: the briefing a line each, the settings an
     * attribute and its value. */
    char        rows[GI_ROWS][GI_ROW_CAP];
    char        values[GI_ROWS][GI_ROW_CAP];
    int         row_count;
    int         scroll;
    int         visible;

    /* What the rows are built from. */
    char        mission_text[2048];
    int         is_campaign;
    BattleConfig cfg;
    char        map_name[96];
} gi;

static const char *tr(const char *key) {
    if (!gi.tt_loaded) {
        Translate_Load(&gi.tt, "english/translate/gui_text.tdf");
        gi.tt_loaded = 1;
    }
    return Translate_Lookup(&gi.tt, key);
}

static void panel_close(GiPanel *p) {
    if (p->rt) GUIRuntime_Destroy(p->rt);
    if (p->has_dialog) GUIDialog_Free(&p->dialog);
    memset(p, 0, sizeof(*p));
}

/* Load a tab panel and move it into the placement guide. */
static int panel_open(GiPanel *p, const char *path, const char *list_name) {
    panel_close(p);
    if (GUIDialog_Load(&p->dialog, path) != 0) return -1;
    p->has_dialog = 1;
    const GUIWidget *guide = GUIDialog_FindByName(&gi.dialog, "PanelPlacementGuide");
    int dx = guide ? guide->rect.x - p->dialog.root.rect.x : 0;
    int dy = guide ? guide->rect.y - p->dialog.root.rect.y : 0;
    p->dialog.root.rect.x += dx;
    p->dialog.root.rect.y += dy;
    for (int i = 0; i < p->dialog.num_children; i++) {
        p->dialog.children[i].rect.x += dx;
        p->dialog.children[i].rect.y += dy;
    }
    p->rt = GUIRuntime_Create(&p->dialog);
    if (!p->rt) { panel_close(p); return -1; }
    const GUIWidget *list = GUIDialog_FindByName(&p->dialog, list_name);
    p->list = list ? list->rect : p->dialog.root.rect;
    /* The templates are drawn by hand, not by the runtime. */
    for (int i = 0; i < p->dialog.num_children; i++) {
        const char *n = p->dialog.children[i].name;
        if (tak_stricmp(n, "BriefingLineTemplate") == 0 || tak_stricmp(n, "BriefingText") == 0 ||
            tak_stricmp(n, "SettingTemplate") == 0 || tak_stricmp(n, "Setting") == 0 ||
            tak_stricmp(n, "Attribute") == 0) {
            GUIRuntime_SetWidgetVisibleAt(p->rt, i, 0);
        }
    }
    const GUIWidget *text = GUIDialog_FindByName(&p->dialog, "BriefingText");
    if (!text) text = GUIDialog_FindByName(&p->dialog, "Attribute");
    p->text_dx = text ? text->rect.x - p->list.x : 6;
    const GUIWidget *value = GUIDialog_FindByName(&p->dialog, "Setting");
    p->value_dx = value ? value->rect.x - p->list.x : p->list.w / 2;
    gi.visible = p->list.h / GI_ROW_H;
    if (gi.visible < 1) gi.visible = 1;
    return 0;
}

static void add_row(const char *text, const char *value) {
    if (gi.row_count >= GI_ROWS) return;
    snprintf(gi.rows[gi.row_count], GI_ROW_CAP, "%s", text ? text : "");
    snprintf(gi.values[gi.row_count], GI_ROW_CAP, "%s", value ? value : "");
    gi.row_count++;
}

/* One paragraph, broken at spaces to the row's width. */
static void add_wrapped(const char *para, int width) {
    char cur[GI_ROW_CAP];
    size_t n = 0;
    cur[0] = 0;
    const char *p = para;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char word[GI_ROW_CAP];
        size_t wl = 0;
        while (*p && *p != ' ' && *p != '\t' && wl + 1 < sizeof(word)) word[wl++] = *p++;
        word[wl] = 0;
        if (!wl) break;
        char trial[GI_ROW_CAP * 2];
        snprintf(trial, sizeof(trial), "%s%s%s", cur, n ? " " : "", word);
        int fits = strlen(trial) < GI_ROW_CAP &&
                   (!gi.font || Font_MeasureString(gi.font, trial) <= width);
        if (n && !fits) {
            add_row(cur, "");
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", trial);
        }
        n = strlen(cur);
    }
    if (n) add_row(cur, "");
}

static void build_briefing_rows(void) {
    gi.row_count = 0;
    gi.scroll = 0;
    int width = gi.panel.list.w - gi.panel.text_dx - 30;
    if (!gi.mission_text[0]) {
        /* legacy:155053, the line a battle with no briefing shows. */
        add_wrapped(tr("WARNING: This mission does not have a briefing."), width);
        return;
    }
    char copy[sizeof(gi.mission_text)];
    snprintf(copy, sizeof(copy), "%s", gi.mission_text);
    for (char *para = copy; para && *para; ) {
        char *end = strpbrk(para, "\r\n");
        if (end) *end++ = 0;
        if (*para) add_wrapped(para, width);
        para = end;
    }
}

static void build_settings_rows(void) {
    gi.row_count = 0;
    gi.scroll = 0;
    char num[16];
    /* The rows and their order (legacy:155173-155230). */
    add_row(tr("Line of Sight:"), tr(gi.cfg.line_of_sight ? "On" : "Off"));
    add_row(tr("Map Revealed:"), tr(gi.cfg.map_revealed ? "Yes" : "No"));
    if (!gi.is_campaign) {
        add_row(tr("Monarch Expendable:"), tr(gi.cfg.monarch_expendable ? "Yes" : "No"));
    }
    add_row(tr("Start Locations:"), tr(gi.cfg.random_start_locations ? "Random" : "Fixed"));
    add_row(tr("Map:"), gi.map_name);
    snprintf(num, sizeof(num), "%d", gi.cfg.units_per_side);
    add_row(tr("Max Units:"), num);
}

static int show_tab(const char *tab) {
    int briefing = tak_stricmp(tab, "Briefing") == 0;
    if (panel_open(&gi.panel, briefing ? GI_BRIEFING : GI_SETTINGS,
                   briefing ? "Briefing" : "SettingsListbox") != 0) return -1;
    snprintf(gi.tab, sizeof(gi.tab), "%s", briefing ? "Briefing" : "GameSettings");
    if (briefing) build_briefing_rows(); else build_settings_rows();
    /* The lit tab is the one on show (legacy:154903-154927). */
    GUIRuntime_SetFrameOverride(gi.rt, "Briefing", briefing ? 1 : 0);
    GUIRuntime_SetFrameOverride(gi.rt, "GameSettings", briefing ? 0 : 1);
    return 0;
}

int GameInfo_Open(const struct GameWorld *world) {
    GameInfo_Close();
    if (GUIDialog_Load(&gi.dialog, GI_MAIN) != 0) return -1;
    gi.has_dialog = 1;
    gi.rt = GUIRuntime_Create(&gi.dialog);
    if (!gi.rt) { GameInfo_Close(); return -1; }
    GUIRuntime_SetWidgetText(gi.rt, "HelpText", "");
    gi.font = Font_Load("data/fonts/b_times new roman (100)", UI_RGBAFormat());
    if (world) {
        gi.cfg = world->cfg;
        snprintf(gi.map_name, sizeof(gi.map_name), "%s", world->map_name);
        gi.is_campaign = world->mission.path[0] != 0;
        if (gi.is_campaign) {
            (void)Briefing_LoadText(world->map_name, gi.mission_text, sizeof(gi.mission_text));
        }
    }
    if (show_tab(gi.is_campaign ? "Briefing" : "GameSettings") != 0) {
        GameInfo_Close();
        return -1;
    }
    gi.open = 1;
    gi.prev_mouse = 1;
    gi.prev_esc = 1;
    return 0;
}

void GameInfo_Close(void) {
    panel_close(&gi.panel);
    if (gi.rt) GUIRuntime_Destroy(gi.rt);
    if (gi.has_dialog) GUIDialog_Free(&gi.dialog);
    if (gi.font) Font_Free(gi.font);
    if (gi.tt_loaded) Translate_Free(&gi.tt);
    memset(&gi, 0, sizeof(gi));
}

int GameInfo_IsOpen(void) { return gi.open; }
const char *GameInfo_Tab(void) { return gi.open ? gi.tab : ""; }
int GameInfo_RowCount(void) { return gi.row_count; }
int GameInfo_Scroll(void) { return gi.scroll; }

const char *GameInfo_Row(int i) {
    static char line[GI_ROW_CAP * 2];
    if (i < 0 || i >= gi.row_count) return "";
    if (gi.values[i][0]) snprintf(line, sizeof(line), "%s %s", gi.rows[i], gi.values[i]);
    else snprintf(line, sizeof(line), "%s", gi.rows[i]);
    return line;
}

static void scroll_by(int d) {
    int most = gi.row_count - gi.visible;
    if (most < 0) most = 0;
    gi.scroll += d;
    if (gi.scroll > most) gi.scroll = most;
    if (gi.scroll < 0) gi.scroll = 0;
}

/* 1 when the press closed the dialog. */
int GameInfo_Press(const char *name) {
    if (!gi.open || !name || !name[0]) return 0;
    if (tak_stricmp(name, "Ok") == 0) { GameInfo_Close(); return 1; }
    if (tak_stricmp(name, "Briefing") == 0 || tak_stricmp(name, "GameSettings") == 0) {
        if (tak_stricmp(name, gi.tab) != 0) (void)show_tab(name);
        return 0;
    }
    if (tak_stricmp(name, "incbutton") == 0) scroll_by(-1);
    if (tak_stricmp(name, "decbutton") == 0) scroll_by(1);
    return 0;
}

static void draw_rows(void) {
    SDL_Surface *off = UI_Offscreen();
    if (!off || !gi.font) return;
    const SDL_Rect *l = &gi.panel.list;
    for (int i = 0; i < gi.visible; i++) {
        int row = gi.scroll + i;
        if (row >= gi.row_count) break;
        int y = l->y + i * GI_ROW_H + 2;
        Font_DrawString(gi.font, off, l->x + gi.panel.text_dx, y, gi.rows[row]);
        if (gi.values[row][0]) {
            Font_DrawString(gi.font, off, l->x + gi.panel.value_dx, y, gi.values[row]);
        }
    }
}

int GameInfo_Tick(TAK_Platform *platform) {
    if (!gi.open) return 0;
    int focus = platform && platform->has_focus;
    int mx = -1, my = -1, mouse_down = 0;
    if (focus) mouse_down = TAK_Platform_MouseThisFrame(platform, &mx, &my);
    int esc = platform && platform->pressed_escape;

    char clicked[64];
    clicked[0] = 0;
    int got = GUIRuntime_Update(gi.rt, mx, my, mouse_down, clicked, sizeof(clicked));
    if (!got && gi.panel.rt) {
        got = GUIRuntime_Update(gi.panel.rt, mx, my, mouse_down, clicked, sizeof(clicked));
    }
    GUIRuntime_Render(gi.rt);
    if (gi.panel.rt) GUIRuntime_Render(gi.panel.rt);
    draw_rows();
    /* The help strip follows the pointer, as the menu's does. */
    const GUIWidget *hw = GUIRuntime_HoveredWidget(gi.rt);
    GUIRuntime_SetWidgetText(gi.rt, "HelpText", hw && hw->tooltip[0] ? hw->tooltip : "");

    gi.prev_mouse = mouse_down;
    if (esc) { GameInfo_Close(); return 1; }
    if (got && clicked[0]) return GameInfo_Press(clicked);
    return 0;
}
