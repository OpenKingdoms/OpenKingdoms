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
#include "tak_sides.h"
#include "tak_dataset.h"
#include "tak_net_session.h"
#include "tak_net_room.h"
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
/* The host's side and its Allow Creon choice. Creon needs both that
 * choice and the expansion (legacy:134048-134052). */
static int            mp_host_side = 0;
static int            mp_allow_creon = 0;

static const SimpleScreenClick mp_routes[] = {
    { NULL, 0 }
};

static int mp_row_of(const GUIWidget *w);

/* An edit to our own row. The client stamps the seat, so a screen only
 * has to say which field moved. */
static void mp_edit_own_row(uint8_t field, uint32_t value) {
    TAK_NetClient *c = NetSession_Client();
    if (!c || c->seat == TAK_NET_SEAT_NONE) return;
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof(e));
    e.field = field;
    e.value = value;
    (void)TAK_NetClient_EditRoom(c, &e);
}

/* A press on a row, or on one of the room's own buttons. With a
 * session the screen asks the server and waits to be told: the server
 * owns the room and a screen that changed itself first would show a
 * state nobody else has (legacy:136219-136222). */
static int mp_on_click(SimpleScreen *s, const char *name, int widget_index) {
    (void)s;
    TAK_NetClient *c = NetSession_Client();

    if (c && c->room.room_id != 0) {
        if (tak_stricmp(name, "Play") == 0) {
            (void)TAK_NetClient_Start(c);
            return 1;
        }
        if (tak_stricmp(name, "Previous") == 0) {
            (void)TAK_NetClient_LeaveRoom(c);
            return 1;
        }
        const GUIWidget *w = GUIRuntime_WidgetAt(mp.rt, widget_index);
        int row = w ? mp_row_of(w) : -1;
        /* Only your own row is yours to change, which is the server's
         * rule as well as the screen's. */
        if (row < 0 || (uint8_t)row != c->seat) return 0;
        if (tak_stricmp(name, "PlayerSide") == 0) {
            int next = Sides_Cycle(c->room.slot[row].side,
                                   TAK_SIDES_MULTIPLAYER,
                                   Multiplayer_CreonAllowed());
            mp_edit_own_row(TAK_EDIT_SIDE, (uint32_t)next);
            return 1;
        }
        if (tak_stricmp(name, "PlayerTeam") == 0) {
            uint32_t team = c->room.slot[row].team;
            mp_edit_own_row(TAK_EDIT_TEAM, (team + 1) % (TAK_ROOM_TEAMS + 1));
            return 1;
        }
        if (tak_stricmp(name, "PlayerColor") == 0) {
            uint32_t col = c->room.slot[row].colour;
            mp_edit_own_row(TAK_EDIT_COLOUR, (col + 1) % TAK_ROOM_COLOURS);
            return 1;
        }
        if (tak_stricmp(name, "PlayerReady") == 0) {
            /* Go toggles and does not latch, so the value is ignored
             * and the press is the whole message. */
            mp_edit_own_row(TAK_EDIT_READY, 1);
            return 1;
        }
        return 0;
    }

    if (tak_stricmp(name, "PlayerSide") != 0) return 0;
    const GUIWidget *w = GUIRuntime_WidgetAt(mp.rt, widget_index);
    if (w && mp_row_of(w) == 0) Multiplayer_CycleHostSide();
    return 1;
}

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
/* The room a session is sitting in, or NULL when there is none. The
 * screen still opens without one, which is what the local case below
 * is for and what lets it be looked at with no server. */
static const TAK_MsgRoomState *mp_room_state(void) {
    TAK_NetClient *c = NetSession_Client();
    if (!c || c->room.room_id == 0) return NULL;
    return &c->room;
}

/* One row, filled from the seat the server says is there. A seat
 * nobody holds reads "Empty" and hides its side, ping, colour, team
 * and ready box, the way the original fills a free slot
 * (legacy:134822-134826, legacy:136310-136334). */
static void mp_fill_row_from_slot(int widget, const GUIWidget *w,
                                  const TAK_NetSlot *slot) {
    int taken = slot->kind != 0;
    if (tak_stricmp(w->name, "PlayerName") == 0) {
        GUIRuntime_SetWidgetTextAt(mp.rt, widget,
            taken && slot->name[0] ? slot->name
                                   : Translate_Lookup(&mp_tt, "Empty"));
    } else if (tak_stricmp(w->name, "PlayerSide") == 0) {
        if (taken) {
            char side_name[32];
            Sides_DisplayName(slot->side, side_name, sizeof(side_name));
            GUIRuntime_SetWidgetTextAt(mp.rt, widget, side_name);
        }
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget, taken);
    } else if (tak_stricmp(w->name, "PlayerPing") == 0) {
        if (taken) {
            char ping[16];
            snprintf(ping, sizeof(ping), "%u", (unsigned)slot->ping_ms);
            GUIRuntime_SetWidgetTextAt(mp.rt, widget, ping);
        }
        /* A seat whose player has dropped keeps its row and loses its
         * ping, which is how the original showed one waiting. */
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget,
                                      taken && slot->connected);
    } else if (tak_stricmp(w->name, "PlayerReady") == 0 ||
               tak_stricmp(w->name, "PlayerColor") == 0 ||
               tak_stricmp(w->name, "PlayerTeam") == 0) {
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget, taken);
    }
}

static void mp_fill_rows(void) {
    const TAK_MsgRoomState *rs = mp_room_state();
    char side_name[32];
    Sides_DisplayName(mp_host_side, side_name, sizeof(side_name));

    for (int i = 0; i < mp.dialog.num_children; i++) {
        const GUIWidget *w = &mp.dialog.children[i];
        int row = mp_row_of(w);
        if (row < 0) continue;
        if (rs) {
            mp_fill_row_from_slot(i, w, &rs->slot[row]);
            continue;
        }
        /* No session, so the screen shows itself: one local row and
         * seven free ones. This is what the room looked like before it
         * had a server behind it, and it is what a test that only
         * wants the art still gets. */
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
    mp.on_click       = mp_on_click;
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
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    mp_host_side = Sides_Set(cfg.players[0].side, TAK_SIDES_MULTIPLAYER,
                             Multiplayer_CreonAllowed());
    mp_fill_rows();
    mp_pick_first_map();
    return 0;
}

/* What the session did since the last frame. The room is a view of the
 * server's snapshot, so everything here either refills the rows or
 * moves the screen on. */
static int mp_take_events(void) {
    TAK_NetClient *c = NetSession_Client();
    if (!c) return GAMESTATE_MULTIPLAYER;
    int next = GAMESTATE_MULTIPLAYER;
    TAK_NetClientEvent e;
    while (TAK_NetClient_PollEvent(c, &e)) {
        switch (e.kind) {
        case TAK_NC_EV_ROOM_STATE:
            mp_fill_rows();
            if (c->room.map_name[0]) {
                GUIRuntime_SetWidgetText(mp.rt, "MapName", c->room.map_name);
            }
            break;
        case TAK_NC_EV_START_GAME:
            /* Every client builds the same world from the same seed,
             * so the loading screen takes it from here. */
            next = GAMESTATE_GAME_LOADING;
            break;
        case TAK_NC_EV_LEFT_ROOM:
            next = GAMESTATE_SELECT_GAME;
            break;
        case TAK_NC_EV_GONE:
            next = GAMESTATE_SELECT_GAME;
            break;
        default:
            break;
        }
    }
    return next;
}

int Multiplayer_Tick(TAK_Platform *platform, float frame_dt) {
    NetSession_Tick(SDL_GetTicks64());
    int next = mp_take_events();
    if (next != GAMESTATE_MULTIPLAYER) return next;
    return SimpleScreen_Tick(&mp, platform, frame_dt);
}

void Multiplayer_Shutdown(void) {
    SimpleScreen_Shutdown(&mp);
    Translate_Free(&mp_tt);
    if (mp_font) Font_Free(mp_font);
    mp_font = NULL;
}

int Multiplayer_CreonAllowed(void) {
    return mp_allow_creon && TAK_DataSet_HasIronPlague();
}

void Multiplayer_SetAllowCreon(int allow) {
    mp_allow_creon = allow ? 1 : 0;
    mp_host_side = Sides_Set(mp_host_side, TAK_SIDES_MULTIPLAYER,
                             Multiplayer_CreonAllowed());
    if (mp.initialized) mp_fill_rows();
}

/* The side button under the multiplayer setter: past Zhon it reaches
 * Creon only when the room allows it (legacy:134910-134923). */
void Multiplayer_CycleHostSide(void) {
    mp_host_side = Sides_Cycle(mp_host_side, TAK_SIDES_MULTIPLAYER,
                               Multiplayer_CreonAllowed());
    if (mp.initialized) mp_fill_rows();
}

int Multiplayer_HostSide(void) {
    return mp_host_side;
}

int Multiplayer_HandleClick(const char *name, int widget_index) {
    if (!mp.initialized || !name) return 0;
    return mp_on_click(&mp, name, widget_index);
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
