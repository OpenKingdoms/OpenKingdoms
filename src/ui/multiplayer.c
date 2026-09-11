/*
 * multiplayer.c -- Multiplayer battle room (GAMESTATE_MULTIPLAYER).
 *
 * battlemenumulti.gui on the SimpleScreen loop, plus what the original
 * does to the dialog when the room opens. Placeholders resolve through
 * the translate tables, the chat line template leaves the dialog, the
 * rows show the host and the empty slots, and MapName names the map.
 */

#include "tak_multiplayer.h"
#include "tak_gameloop.h"
#include "tak_simple_screen.h"
#include "tak_translate.h"
#include "tak_battle_config.h"
#include "tak_font.h"
#include "tak_hpi.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_ui.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* The player table has eight rows 22 px apart, the first row's widgets
 * between y=61 and y=64. The right column shares those rows, so only
 * widgets left of x=400 belong to the table. */
#define MP_ROWS      8
#define MP_ROW_TOP   58
#define MP_ROW_PITCH 22
#define MP_TABLE_X   400

static SimpleScreen   mp;
static TranslateTable mp_tt;
static Font          *mp_font;

static const SimpleScreenClick mp_routes[] = {
    { NULL, 0 }
};

static int mp_row_of(const GUIWidget *w) {
    if (w->rect.x >= MP_TABLE_X || w->rect.y < MP_ROW_TOP) return -1;
    int row = (w->rect.y - MP_ROW_TOP) / MP_ROW_PITCH;
    return row < MP_ROWS ? row : -1;
}

/* Slot 0 is the local host until the room has a session, the rest are
 * free. The original hides every ready box at open (legacy:136181-136196)
 * and then fills each slot. A free slot reads "Empty" (legacy:134822-134826)
 * and hides its side, ping, colour, team and ready box
 * (legacy:136310-136334). The host's row shows its side, its ping as a
 * number and its own ready box (legacy:136313-136318,
 * legacy:136336-136377). */
static void mp_fill_rows(void) {
    static const char *sides[] = { "Aramon", "Taros", "Veruna", "Zhon" };
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    int side = cfg.players[0].side;
    const char *side_name = (side >= 0 && side < 4) ? sides[side] : sides[0];

    for (int i = 0; i < mp.dialog.num_children; i++) {
        const GUIWidget *w = &mp.dialog.children[i];
        int row = mp_row_of(w);
        if (row < 0) continue;
        int host = (row == 0);
        if (tak_stricmp(w->name, "PlayerName") == 0) {
            GUIRuntime_SetWidgetTextAt(mp.rt, i,
                host ? "Player" : Translate_Lookup(&mp_tt, "Empty"));
        } else if (tak_stricmp(w->name, "PlayerSide") == 0) {
            if (host) GUIRuntime_SetWidgetTextAt(mp.rt, i, side_name);
            GUIRuntime_SetWidgetVisibleAt(mp.rt, i, host);
        } else if (tak_stricmp(w->name, "PlayerPing") == 0) {
            if (host) GUIRuntime_SetWidgetTextAt(mp.rt, i, "0");
            GUIRuntime_SetWidgetVisibleAt(mp.rt, i, host);
        } else if (tak_stricmp(w->name, "PlayerReady") == 0 ||
                   tak_stricmp(w->name, "PlayerColor") == 0 ||
                   tak_stricmp(w->name, "PlayerTeam") == 0) {
            GUIRuntime_SetWidgetVisibleAt(mp.rt, i, host);
        }
    }
}

/* Buttons draw their state's text as well as their art
 * (legacy:329670-329705). The runtime draws label text only, so the
 * table's text buttons are drawn here, from the rect origin like a label. */
static void mp_draw_button_text(SimpleScreen *s) {
    SDL_Surface *off = UI_Offscreen();
    if (!mp_font || !off) return;
    for (int i = 0; i < s->dialog.num_children; i++) {
        const GUIWidget *w = &s->dialog.children[i];
        if (w->type != GUI_WT_BUTTON || w->num_frames > 0) continue;
        if (!w->display_text[0] || GUIRuntime_WidgetHiddenAt(s->rt, i)) continue;
        int x = GUI_AlignedTextX(w, mp_font, w->display_text, w->rect.x);
        Font_DrawString(mp_font, off, x, w->rect.y, w->display_text);
    }
}

/* No saved netgame options exist yet, so the room opens on the first map
 * by name, as the skirmish screen does. The map sources are the same
 * ones the chooser reads, map packs included (see tak_maps.h). */
static void mp_pick_first_map(void) {
    TAK_MapEntry *found = NULL;
    int n = 0;
    if (TAK_Maps_Scan(&found, &n) != 0) return;
    if (n > 0) Multiplayer_SelectMap(found[0].key);
    TAK_Maps_Free(found);
}

int Multiplayer_Init(TAK_Platform *platform) {
    memset(&mp, 0, sizeof(mp));
    Translate_Free(&mp_tt);
    if (mp_font) Font_Free(mp_font);
    mp_font = NULL;
    mp.gui_path       = "data/guis/battlemenumulti.gui";
    mp.self_state     = GAMESTATE_MULTIPLAYER;
    mp.default_return = GAMESTATE_MENU;
    mp.click_routes   = mp_routes;
    mp.after_render   = mp_draw_button_text;
    if (SimpleScreen_Init(&mp, platform) != 0) return -1;

    /* Placeholders such as _MPGo_ and _MPUnits_ resolve through the
     * translate tables (legacy:267931). */
    Translate_Load(&mp_tt, "english/translate/maps.tdf");
    Translate_Load(&mp_tt, "english/translate/gui_text.tdf");
    Translate_Dialog(&mp_tt, &mp.dialog);

    /* The chat list keeps ChatTemplate as its line template and the dialog
     * deletes it, children and all (legacy:136856-136864,
     * legacy:311882-311896). */
    GUIRuntime_SetWidgetVisible(mp.rt, "ChatTemplate", 0);
    GUIRuntime_SetWidgetVisible(mp.rt, "ChatPlayerName", 0);
    GUIRuntime_SetWidgetVisible(mp.rt, "Message", 0);

    /* The help strip starts empty (legacy:148800). */
    GUIRuntime_SetWidgetText(mp.rt, "HelpText", "");

    /* The host picks the map, anyone else only views it
     * (legacy:136832-136851). */
    GUIRuntime_SetWidgetVisible(mp.rt, "Map", 1);
    GUIRuntime_SetWidgetVisible(mp.rt, "ViewMap", 0);

    mp_font = Font_Load("data/fonts/b_times new roman (100)", UI_RGBAFormat());
    mp_fill_rows();
    mp_pick_first_map();
    return 0;
}

int Multiplayer_Tick(TAK_Platform *platform, float frame_dt) {
    return SimpleScreen_Tick(&mp, platform, frame_dt);
}

void Multiplayer_Shutdown(void) {
    SimpleScreen_Shutdown(&mp);
    Translate_Free(&mp_tt);
    if (mp_font) Font_Free(mp_font);
    mp_font = NULL;
}

GUIRuntime *Multiplayer_Runtime(void) {
    return mp.initialized ? mp.rt : NULL;
}

/* MapName holds the chosen map's name (legacy:137716-137722). The room
 * has no preview of its own: battlemenumulti.gui authors this label and
 * the two map buttons, and the 128x128 MapView lives in the choosemap and
 * viewmap dialogs those buttons open (legacy:137355-137378). The skirmish
 * screen differs, authoring SelectedMapView beside its map list. */
int Multiplayer_SelectMap(const char *key) {
    if (!mp.initialized || !key || !*key) return -1;
    char path[256];
    if (TAK_Maps_FindFile(key, "ota", path, sizeof(path)) != 0) return -1;
    char shown[128];
    Translate_MapName(&mp_tt, key, shown, sizeof(shown));
    GUIRuntime_SetWidgetText(mp.rt, "MapName", shown);
    return 0;
}
