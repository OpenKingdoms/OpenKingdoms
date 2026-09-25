/*
 * select_game.c -- Select Game (selectgame.gui).
 *
 * The screen between the main menu and a battle room. It shows what
 * rooms a server is offering, and lets a player join one or host their
 * own, which is what the original's screen did.
 *
 * Two ways in, as the original had. It offered Boneyards, a lobby
 * service, and an address to type. Boneyards is gone, so that button
 * connects to where this build came from instead: in a browser that is
 * the page's own origin, which is both the closest thing to what the
 * player clicked and the reason no address is written down anywhere in
 * this repository. Recorded as a deviation.
 *
 * It reads the room list off the session and never touches a socket.
 * That is what lets the whole screen be tested with no server, by
 * feeding the session client the messages a server would have sent.
 */

#include "tak_data_fingerprint.h"
#include "tak_select_game.h"

#include "tak_font.h"
#include "tak_gameloop.h"
#include "tak_battle_config.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_maps.h"
#include "tak_map_fingerprint.h"
#include "tak_net_session.h"
#include "tak_settings.h"
#include "tak_ui.h"
#include "tak_util.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define SG_ADDRESS_MAX 96
#define SG_STATUS_MAX  128
/* TAK_NET_NAME_MAX is what the wire carries, and the box holds no more
 * than the wire will take. */
#define SG_NAME_MAX    TAK_NET_NAME_MAX
/* The key the name is kept under, so it survives a restart. */
#define SG_NAME_KEY    "PlayerName" 
#define SG_ADDRESS_KEY "ServerAddress"

static struct {
    int         open;
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    Font       *font_row;
    Font       *font_help;

    int idx_list, idx_track, idx_thumb, idx_inc, idx_dec;

    int scroll;
    int selected;

    char address[SG_ADDRESS_MAX];
    char status[SG_STATUS_MAX];
    char name[SG_NAME_MAX];
    /* Which box has the caret. A screen with two text boxes needs to
     * know, and the original's Enter key means different things in
     * each: in the address box it connects, in the name box it does
     * not. */
    int  typing_address;
    int  typing_name;
    /* A name that changed and has not been written out yet. */
    int  name_dirty;
    /* The address being tried is the one the page came from, which
     * nobody chose and nobody can be expected to fix. A failure there
     * needs a different sentence from one on an address a player
     * typed. */
    int  tried_default;

    /* Asked for a room list and waiting, so the screen can say so
     * rather than showing an empty list that looks like no games. */
    int  listing;

    int  prev_mouse, prev_enter, prev_esc, prev_back;
    int  dragging, grab_dy;
    int  next_state;
} sg;

/* Defined below, beside the rest of what a press does. Declared here
 * because the test seam sits above it. */
static void sg_press(const char *name);
static void connect_to(const char *address);
static void fill_info(void);

static TAK_NetClient *client(void) { return NetSession_Client(); }

/* Never empty. A player who has typed nothing plays as "Player",
 * which is what the room shows and what the server is told. */
const char *SelectGame_PlayerName(void) {
    return sg.name[0] ? sg.name : "Player";
}

/* Kept with the rest of the player's settings, so a name typed once is
 * the name next time. */
static void load_name(void) {
    const char *saved = Settings_GetStr(SG_NAME_KEY, "");
    snprintf(sg.name, sizeof sg.name, "%s", saved ? saved : "");
}

/* The same for the server, so it is typed once. A browser fills this
 * from the page it came from and never reaches here. */
static void load_address(void) {
    const char *saved = Settings_GetStr(SG_ADDRESS_KEY, "");
    snprintf(sg.address, sizeof sg.address, "%s", saved ? saved : "");
}

static void save_address(void) {
    if (strcmp(Settings_GetStr(SG_ADDRESS_KEY, ""), sg.address) == 0) return;
    Settings_SetStr(SG_ADDRESS_KEY, sg.address);
    (void)Settings_Save();
}

/* Which box has the caret, and whether SDL should be sending typed
 * characters at all.
 *
 * SDL delivers SDL_TEXTINPUT only between SDL_StartTextInput and
 * SDL_StopTextInput, and the platform stops it at start up. Nothing on
 * this screen ever started it, so both boxes looked like boxes and
 * neither took a key. On a phone or a tablet this is also what raises
 * the on screen keyboard. */
static void set_caret(int on_address, int on_name) {
    sg.typing_address = on_address;
    sg.typing_name = on_name;
    if (on_address || on_name) SDL_StartTextInput();
    else                       SDL_StopTextInput();
}

/* The server was told a name when this screen connected, and that was
 * before the player had any chance to type one. So a name that changes
 * has to reach the server again.
 *
 * In a room that is one edit to our own row. In the lobby the name
 * travels in the greeting and nowhere else, so the greeting has to
 * happen again, which costs nothing there because nothing is in
 * progress and the room list comes straight back. */
static void push_name(void) {
    TAK_NetClient *c = client();
    if (!c) return;
    if (c->seat != TAK_NET_SEAT_NONE) {
        TAK_MsgRoomEdit e;
        memset(&e, 0, sizeof e);
        e.field = TAK_EDIT_NAME;
        snprintf(e.text, sizeof e.text, "%s", SelectGame_PlayerName());
        (void)TAK_NetClient_EditRoom(c, &e);
        return;
    }
    if (NetSession_State() == NET_SESSION_READY && sg.address[0]) {
        connect_to(sg.address);
    }
}

static void save_name(void) {
    if (!sg.name_dirty) return;
    sg.name_dirty = 0;
    Settings_SetStr(SG_NAME_KEY, sg.name);
    (void)Settings_Save();
    push_name();
}

static int room_count(void) {
    TAK_NetClient *c = client();
    return c ? (int)c->rooms.count : 0;
}

static void set_status(const char *text) {
    if (!text) { sg.status[0] = '\0'; return; }
    size_t n = strlen(text);
    if (n >= sizeof sg.status) n = sizeof sg.status - 1;
    memcpy(sg.status, text, n);
    sg.status[n] = '\0';
}

static void cache_indices(void) {
    sg.idx_list = sg.idx_track = sg.idx_thumb = sg.idx_inc = sg.idx_dec = -1;
    for (int i = 0; i < sg.dialog.num_children; i++) {
        const GUIWidget *w = &sg.dialog.children[i];
        if (tak_stricmp(w->name, "GameList")  == 0) sg.idx_list  = i;
        if (tak_stricmp(w->name, "slider")    == 0) sg.idx_track = i;
        if (tak_stricmp(w->name, "sbutton")   == 0) sg.idx_thumb = i;
        if (tak_stricmp(w->name, "incbutton") == 0) sg.idx_inc   = i;
        if (tak_stricmp(w->name, "decbutton") == 0) sg.idx_dec   = i;
    }
}

/* A row is one GameInfoTemplate tall, the way the original clones that
 * template once per game it lists. */
static int row_height(void) {
    const GUIWidget *t = GUIDialog_FindByName(&sg.dialog, "GameInfoTemplate");
    return (t && t->rect.h > 0) ? t->rect.h : 16;
}

static SDL_Rect list_rect(void) {
    SDL_Rect r = { 40, 120, 380, 200 };
    if (sg.idx_list >= 0) r = sg.dialog.children[sg.idx_list].rect;
    if (sg.idx_track >= 0) {
        const GUIWidget *t = &sg.dialog.children[sg.idx_track];
        if (t->rect.x > r.x) r.w = t->rect.x - r.x;
    }
    return r;
}

static int rows_visible(void) {
    int n = list_rect().h / row_height();
    return n > 0 ? n : 1;
}

static int max_scroll(void) {
    int m = room_count() - rows_visible();
    return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
    int m = max_scroll();
    if (sg.scroll > m) sg.scroll = m;
    if (sg.scroll < 0) sg.scroll = 0;
}

static void widget_draw_rect(int index, SDL_Rect *out) {
    out->x = out->y = 0;
    out->w = out->h = 1;
    if (index < 0 || index >= sg.dialog.num_children) return;
    if (GUIRuntime_WidgetDrawRect(sg.rt, index, out) != 0)
        *out = sg.dialog.children[index].rect;
}

/* ── what the screen asks the session for ──────────────────────────── */

static void ask_for_rooms(void) {
    TAK_NetClient *c = client();
    if (!c) return;
    if (TAK_NetClient_ListRooms(c) == 0) {
        sg.listing = 1;
        set_status("Looking for games.");
    }
}

/* A game to join as soon as the server answers, from a join link. It
 * stays until the room opens, so a name typed after a refusal for want
 * of one still gets there. */
static char g_join_code[TAK_NET_CODE_MAX];

void SelectGame_SetJoinCode(const char *code) {
    size_t j = 0;
    for (const char *p = code ? code : ""; *p && j + 1 < sizeof g_join_code; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) g_join_code[j++] = c;
    }
    g_join_code[j] = '\0';
}

const char *SelectGame_JoinCode(void) { return g_join_code; }

static void join_by_code(void) {
    TAK_NetClient *c = client();
    if (!c || !g_join_code[0]) return;
    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof jr);
    snprintf(jr.code, sizeof jr.code, "%s", g_join_code);
    if (TAK_NetClient_JoinRoom(c, &jr) != 0) {
        set_status("Could not ask to join that game.");
        return;
    }
    char line[64];
    snprintf(line, sizeof line, "Joining game %s.", g_join_code);
    set_status(line);
}

/* What a refusal says, and for a data mismatch which of the five groups
 * differs, so the player knows whether it is a mod, a unit file or a
 * script (docs/MULTIPLAYER.md). */
static void say_refusal(const TAK_MsgReject *r) {
    if (r->reason == TAK_REJECT_DATA_MISMATCH && r->detail < TAK_DATA_GROUP_COUNT) {
        char line[160];
        snprintf(line, sizeof line,
                 "Your game data does not match that game: the %s differ. "
                 "One of you has a mod or another release.",
                 TAK_DataFingerprint_GroupName(r->detail));
        set_status(line);
        return;
    }
    set_status(r->text[0] ? r->text : "The server said no.");
}

static void connect_to(const char *address) {
    if (!address || !address[0]) {
        set_status("Type the address of a server to join.");
        return;
    }
    if (NetSession_Connect(address, SelectGame_PlayerName()) != 0) {
        set_status(NetSession_Why());
        return;
    }
    set_status("Connecting.");
}

static void join_selected(void) {
    TAK_NetClient *c = client();
    if (!c || sg.selected < 0 || sg.selected >= room_count()) {
        set_status("Choose a game first.");
        return;
    }
    const TAK_RoomSummary *r = &c->rooms.room[sg.selected];
    if (r->compat != 0) {
        /* The original dropped a game it could not join off the list
         * without a word, which left players wondering why a friend's
         * game was invisible. The row stays and says why. */
        set_status(r->compat == TAK_REJECT_DATA_MISMATCH
                   ? "That game uses different game data, a mod or another release."
                   : "That game is not one this build can join.");
        return;
    }
    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof jr);
    jr.room_id = r->room_id;
    if (TAK_NetClient_JoinRoom(c, &jr) != 0) {
        set_status("Could not ask to join that game.");
        return;
    }
    set_status("Joining.");
}

/* The first map the chooser offers, with the fingerprint that says
 * which map it actually is. A room with no map cannot be started, and
 * two installs can hold different maps under one name, so the name
 * alone would not do. The host may change it in the room. */
static int first_map(char *name, size_t name_cap,
                     uint8_t fp[TAK_NET_FINGERPRINT_BYTES]) {
    TAK_MapEntry *found = NULL;
    int n = 0;
    if (TAK_Maps_Scan(&found, &n) != 0 || n <= 0) {
        TAK_Maps_Free(found);
        return -1;
    }
    int rc = -1;
    for (int i = 0; i < n && rc != 0; i++) {
        if (TAK_MapFingerprint_FromName(found[i].key, fp) != 0) continue;
        size_t len = strlen(found[i].key);
        if (len >= name_cap) len = name_cap - 1;
        memcpy(name, found[i].key, len);
        name[len] = '\0';
        rc = 0;
    }
    TAK_Maps_Free(found);
    return rc;
}

static void host_game(void) {
    TAK_NetClient *c = client();
    if (!c) {
        set_status("Connect to a server first.");
        return;
    }
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    snprintf(cr.name, sizeof cr.name, "%s's game", SelectGame_PlayerName());
    cr.flags = TAK_ROOMF_LISTED | TAK_ROOMF_ALLOW_WATCHING;
    cr.max_players = TAK_NET_SEATS;
    /* The rules this build plays a skirmish under. Sending nothing
     * reads as every rule off, which is not a default anybody chose:
     * line of sight alone is a different battle. */
    BattleConfig defaults;
    BattleConfig_SetDefaults(&defaults);
    cr.unit_cap = (uint16_t)defaults.units_per_side;
    cr.options =
        (defaults.line_of_sight          ? TAK_ROOMOPT_LINE_OF_SIGHT    : 0u) |
        (defaults.map_revealed           ? TAK_ROOMOPT_MAP_REVEALED     : 0u) |
        (defaults.monarch_expendable     ? TAK_ROOMOPT_MONARCH_EXPEND   : 0u) |
        (defaults.random_start_locations ? TAK_ROOMOPT_RANDOM_STARTS    : 0u) |
        (defaults.power_codes            ? TAK_ROOMOPT_POWER_CODES      : 0u) |
        (defaults.slow_game              ? TAK_ROOMOPT_SLOW_GAME        : 0u) |
        (defaults.crusades_balance       ? TAK_ROOMOPT_CRUSADES_BALANCE : 0u);
    if (first_map(cr.map_name, sizeof cr.map_name, cr.map_fingerprint) != 0) {
        /* Without a map the server would take the room and then refuse
         * every attempt to start it, which is a worse answer than this. */
        set_status("No maps installed to host a game on.");
        return;
    }
    if (TAK_NetClient_CreateRoom(c, &cr) != 0) {
        set_status("Could not ask for a game.");
        return;
    }
    set_status("Making a game.");
}

/* ── drawing ───────────────────────────────────────────────────────── */

static void draw_rows(void) {
    SDL_Surface *off = UI_Offscreen();
    TAK_NetClient *c = client();
    if (!off || !sg.font_row) return;
    SDL_Rect r = list_rect();
    int rh = row_height();
    SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    if (!c) return;

    int rows = rows_visible();
    for (int i = 0; i < rows; i++) {
        int idx = sg.scroll + i;
        if (idx < 0 || idx >= room_count()) break;
        const TAK_RoomSummary *s = &c->rooms.room[idx];
        if (idx == sg.selected) {
            SDL_Rect sel = { r.x, r.y + i * rh, r.w, rh };
            SDL_FillRect(off, &sel, SDL_MapRGBA(off->format, 60, 50, 35, 255));
        }
        char line[128];
        /* A room this build cannot join is listed and marked, not
         * hidden, which is the one thing the original got wrong here.
         * The game under Game Name and its host under Host, the two
         * columns GameInfoTemplate authors. */
        snprintf(line, sizeof line, "%s%s",
                 s->compat ? "- " : "  ",
                 s->name[0] ? s->name : "(no name)");
        Font_DrawString(sg.font_row, off, r.x + 4, r.y + i * rh + 2, line);
        Font_DrawString(sg.font_row, off, r.x + 158, r.y + i * rh + 2,
                        s->host_name[0] ? s->host_name : "");
    }
}

/* The name box, with a caret while it has one. The .gui authors the
 * label above it and the frame around it, and this is the text. */
static void draw_name(void) {
    SDL_Surface *off = UI_Offscreen();
    const GUIWidget *w = GUIDialog_FindByName(&sg.dialog, "Name");
    if (!off || !w || !sg.font_row) return;
    SDL_Rect r = w->rect;
    SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    char shown[SG_NAME_MAX + 2];
    snprintf(shown, sizeof shown, "%s%s", SelectGame_PlayerName(),
             sg.typing_name ? "_" : "");
    Font_DrawString(sg.font_row, off, r.x + 4, r.y + 3, shown);
}

static void draw_address(void) {
    SDL_Surface *off = UI_Offscreen();
    const GUIWidget *w = GUIDialog_FindByName(&sg.dialog, "EnterTCPIPAddress");
    if (!off || !w || !sg.font_row) return;
    SDL_Rect r = w->rect;
    SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    char shown[SG_ADDRESS_MAX + 2];
    snprintf(shown, sizeof shown, "%s%s", sg.address,
             sg.typing_address ? "_" : "");
    Font_DrawString(sg.font_row, off, r.x + 4, r.y + 3, shown);
}

static void draw_status(void) {
    if (!sg.font_help || !sg.status[0]) return;
    SDL_Surface *off = UI_Offscreen();
    const GUIWidget *help = GUIDialog_FindByName(&sg.dialog, "HelpText");
    if (!off || !help) return;
    int tw = Font_MeasureString(sg.font_help, sg.status);
    SDL_Rect r = help->rect;
    Font_DrawString(sg.font_help, off, r.x + (r.w - tw) / 2, r.y + 2,
                    sg.status);
}

/* ── the screen ────────────────────────────────────────────────────── */

int SelectGame_Init(TAK_Platform *platform) {
    memset(&sg, 0, sizeof sg);
    sg.selected = -1;
    sg.next_state = GAMESTATE_SELECT_GAME;

    (void)platform;
    if (GUIDialog_Load(&sg.dialog, "data/guis/selectgame.gui") != 0) {
        return -1;
    }
    sg.has_dialog = 1;
    sg.rt = GUIRuntime_Create(&sg.dialog);
    if (!sg.rt) { GUIDialog_Free(&sg.dialog); sg.has_dialog = 0; return -1; }
    cache_indices();

    sg.font_row  = Font_Load("data/fonts/b_times new roman (100)",
                             UI_RGBAFormat());
    sg.font_help = Font_Load("data/fonts/b_times new roman (100b)",
                             UI_RGBAFormat());

    load_name();
    set_caret(0, 0);
    fill_info();

    NetSession_DefaultAddress(sg.address, sizeof sg.address);
    if (!sg.address[0]) load_address();
    if (sg.address[0]) {
        /* A browser came from somewhere, so there is an address to try
         * and the player never has to know its name. Whether anything
         * answers there is another matter, which is what
         * sg.tried_default is for. */
        sg.tried_default = 1;
        connect_to(sg.address);
    } else {
        set_status("Type the address of a server to join.");
    }
    sg.open = 1;
    return 0;
}

void SelectGame_Shutdown(void) {
    /* A name typed and not confirmed with Enter is still the name the
     * player meant, so leaving the screen writes it out. */
    save_name();
    save_address();
    SDL_StopTextInput();
    if (sg.rt) GUIRuntime_Destroy(sg.rt);
    if (sg.has_dialog) GUIDialog_Free(&sg.dialog);
    if (sg.font_row) Font_Free(sg.font_row);
    if (sg.font_help && sg.font_help != sg.font_row) Font_Free(sg.font_help);
    memset(&sg, 0, sizeof sg);
}

int SelectGame_RowCount(void) { return room_count(); }
int SelectGame_Selected(void) { return sg.selected; }

/* The Game Information panel: the chosen game's name, host, map,
 * rules, players and state in the labels selectgame.gui authors.
 * Nothing chosen, nothing said. */
static void set_label(const char *name, const char *text) {
    if (sg.rt && GUIDialog_FindByName(&sg.dialog, name))
        GUIRuntime_SetWidgetText(sg.rt, name, text);
}

static const char *yes_no(int on) { return on ? "Yes" : "No"; }

static const char *status_word(uint8_t status) {
    switch (status) {
        case TAK_ROOM_OPEN:        return "Open";
        case TAK_ROOM_LOCKED:      return "Locked";
        case TAK_ROOM_LOADING:     return "Loading";
        case TAK_ROOM_IN_PROGRESS: return "Playing";
        case TAK_ROOM_ENDED:       return "Ended";
        default:                   return "";
    }
}

static void fill_info(void) {
    TAK_NetClient *c = client();
    const TAK_RoomSummary *r = (c && sg.selected >= 0 && sg.selected < room_count())
                             ? &c->rooms.room[sg.selected] : NULL;
    char players[32];
    players[0] = '\0';
    if (r) snprintf(players, sizeof players, "%u/%u",
                    (unsigned)r->players, (unsigned)r->max_players);
    set_label("Game",           r ? r->name : "");
    set_label("Host",           r ? r->host_name : "");
    set_label("MapName",        r ? r->map_name : "");
    set_label("MonarchDeath",   r ? ((r->options & TAK_ROOMOPT_MONARCH_EXPEND)
                                     ? "Expendable" : "Fatal") : "");
    set_label("LOS",            r ? yes_no(r->options & TAK_ROOMOPT_LINE_OF_SIGHT) : "");
    set_label("Mapping",        r ? yes_no(r->options & TAK_ROOMOPT_MAP_REVEALED) : "");
    set_label("NumPlayers",     players);
    set_label("GameStatus",     r ? status_word(r->status) : "");
    set_label("ScriptedStatus", r ? "No" : "");
    set_label("Creon",          r ? yes_no(r->flags & TAK_ROOMF_IRON_PLAGUE) : "");
}

int SelectGame_Scroll(void) { return sg.scroll; }
int SelectGame_RowsVisible(void) { return rows_visible(); }
void SelectGame_Press(const char *name) { sg_press(name); }

void SelectGame_SelectRow(int row) {
    sg.selected = (row >= 0 && row < room_count()) ? row : -1;
    fill_info();
}

int SelectGame_LabelText(const char *name, char *out, size_t cap) {
    if (!sg.rt || !name || !out || !cap) return 0;
    const GUIWidget *w = GUIRuntime_WidgetByName(sg.rt, name);
    if (!w) return 0;
    snprintf(out, cap, "%s", w->display_text);
    return 1;
}

const char *SelectGame_RowName(int index) {
    TAK_NetClient *c = client();
    if (!c || index < 0 || index >= room_count()) return NULL;
    return c->rooms.room[index].name;
}

const char *SelectGame_Status(void) { return sg.status; }

const char *SelectGame_Address(void) { return sg.address; }

void SelectGame_HandleClick(const char *name) {
    if (!sg.open) return;
    sg_press(name);
}

/* Everything the session did since the last frame, turned into the one
 * line the screen shows and the state it moves to. */
static void take_events(void) {
    TAK_NetClient *c = client();
    if (!c) return;
    TAK_NetClientEvent e;
    while (TAK_NetClient_PollEvent(c, &e)) {
        switch (e.kind) {
        case TAK_NC_EV_WELCOMED:
            sg.tried_default = 0;
            set_status("Connected.");
            if (g_join_code[0]) join_by_code();
            else ask_for_rooms();
            break;
        case TAK_NC_EV_ROOM_LIST:
            sg.listing = 0;
            if (room_count() == 0) {
                set_status("No games yet. Host one.");
            } else {
                set_status("");
                if (sg.selected >= room_count()) sg.selected = -1;
            }
            clamp_scroll();
            fill_info();
            break;
        case TAK_NC_EV_ROOM_STATE:
            /* In a room, so the battle room takes over from here. */
            g_join_code[0] = '\0';
            sg.next_state = GAMESTATE_MULTIPLAYER;
            break;
        case TAK_NC_EV_REFUSED:
            say_refusal(&c->reject);
            /* A link to a game that is gone or cannot be joined leaves
             * the player on the list of the games there are. Wanting a
             * name keeps the code for when one is typed. */
            if (g_join_code[0] && c->reject.reason != TAK_REJECT_NAME_REQUIRED &&
                c->state == TAK_NC_LOBBY) {
                g_join_code[0] = '\0';
                ask_for_rooms();
            }
            break;
        case TAK_NC_EV_GONE:
            set_status("The connection closed.");
            break;
        default:
            break;
        }
    }
}

/* What a press on a named button does. One place, because a test
 * presses buttons through SelectGame_HandleClick and a player presses
 * them through the runtime, and the two had already drifted: the seam
 * knew three of these and the screen knew seven, so a test could not
 * see the name box at all. */
static void sg_press(const char *name) {
    if (!name || !name[0]) return;
    if (tak_stricmp(name, "incbutton") == 0) {
        sg.scroll--;
        clamp_scroll();
    } else if (tak_stricmp(name, "decbutton") == 0) {
        sg.scroll++;
        clamp_scroll();
    } else if (tak_stricmp(name, "MainMenu") == 0) {
        set_caret(0, 0);
        NetSession_Disconnect();
        sg.next_state = GAMESTATE_MENU;
    } else if (tak_stricmp(name, "Update") == 0) {
        ask_for_rooms();
    } else if (tak_stricmp(name, "Join") == 0) {
        join_selected();
    } else if (tak_stricmp(name, "HostGame") == 0) {
        host_game();
    } else if (tak_stricmp(name, "Boneyards") == 0) {
        /* The service this button named is gone. It connects to where
         * this build came from instead, which in a browser is the
         * page's own origin. */
        char def[SG_ADDRESS_MAX];
        NetSession_DefaultAddress(def, sizeof def);
        if (def[0]) {
            memcpy(sg.address, def, sizeof def);
            sg.tried_default = 1;
            connect_to(sg.address);
        } else {
            set_status("This build has no server of its own. "
                       "Type an address.");
        }
    } else if (tak_stricmp(name, "EnterTCPIPAddress") == 0) {
        set_caret(1, 0);
        set_status("Type an address, then press Enter.");
    } else if (tak_stricmp(name, "Name") == 0) {
        set_caret(0, 1);
        set_status("Type the name other players will see.");
    }
}

int SelectGame_Tick(TAK_Platform *platform, float dt) {
    (void)dt;
    if (!sg.open || !sg.rt) return GAMESTATE_MENU;

    NetSession_Tick(SDL_GetTicks64());
    if (NetSession_State() == NET_SESSION_FAILED) {
        if (sg.tried_default) {
            /* The page's own origin answered nothing. That is not a
             * thing the player did, and "the connection closed" tells
             * them nothing they can act on, so this says what is
             * true and what is left to try. */
            sg.tried_default = 0;
            set_status("No game server is running at this address. "
                       "Type the address of one to join it.");
        } else {
            const char *why = NetSession_Why();
            if (why && why[0]) set_status(why);
        }
    }
    take_events();
    if (sg.next_state != GAMESTATE_SELECT_GAME) {
        int next = sg.next_state;
        sg.next_state = GAMESTATE_SELECT_GAME;
        return next;
    }

    int focus = platform && platform->has_focus;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int enter = focus && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);
    int esc   = focus && keys[SDL_SCANCODE_ESCAPE];
    int back  = focus && keys[SDL_SCANCODE_BACKSPACE];
    int enter_edge = (enter && !sg.prev_enter) || (focus && platform->pressed_enter);
    int esc_edge   = (esc && !sg.prev_esc)     || (focus && platform->pressed_escape);
    int back_edge  = (back && !sg.prev_back)   || (focus && platform->pressed_backspace);
    sg.prev_enter = enter;
    sg.prev_esc = esc;
    sg.prev_back = back;

    int mx = -1, my = -1, mouse_down = 0;
    if (focus) mouse_down = TAK_Platform_MouseThisFrame(platform, &mx, &my);

    /* The screen's own keys, which the .gui names: Enter joins and
     * Escape goes back to the main menu. */
    if (esc_edge) {
        set_caret(0, 0);
        NetSession_Disconnect();
        return GAMESTATE_MENU;
    }

    if (sg.typing_address || sg.typing_name) {
        char  *box = sg.typing_address ? sg.address : sg.name;
        size_t cap = sg.typing_address ? sizeof sg.address : sizeof sg.name;
        if (back_edge) {
            size_t n = strlen(box);
            if (n) {
                box[n - 1] = '\0';
                if (box == sg.name) sg.name_dirty = 1;
            }
        }
        if (platform && platform->text_in_len > 0) {
            for (int i = 0; i < platform->text_in_len; i++) {
                size_t n = strlen(box);
                if (n + 1 >= cap) break;
                box[n] = platform->text_in[i];
                box[n + 1] = '\0';
            }
            if (box == sg.name) sg.name_dirty = 1;
        }
        if (enter_edge) {
            /* Enter in the address box connects, which is the original's
             * accelerator for this screen. In the name box it only puts
             * the caret down: a name is not a thing to act on. */
            if (sg.typing_address) {
                set_caret(0, 0);
                sg.tried_default = 0;
                connect_to(sg.address);
            } else {
                set_caret(0, 0);
                save_name();
            }
        }
    } else if (enter_edge) {
        join_selected();
    }

    /* Thumb drag, the same way the save browser does it: the thumb
     * travels only inside the bar art. */
    if (sg.idx_thumb >= 0 && sg.idx_track >= 0) {
        SDL_Point pt = { mx, my };
        SDL_Rect thumb_draw;
        widget_draw_rect(sg.idx_thumb, &thumb_draw);
        if (mouse_down && !sg.dragging && SDL_PointInRect(&pt, &thumb_draw)) {
            sg.dragging = 1;
            sg.grab_dy = my - thumb_draw.y;
        }
        if (!mouse_down) sg.dragging = 0;
        if (sg.dragging) {
            SDL_Rect track;
            widget_draw_rect(sg.idx_track, &track);
            int th = thumb_draw.h > 0 ? thumb_draw.h : 1;
            int travel = track.h - th;
            int m = max_scroll();
            if (travel > 0 && m > 0) {
                int rel = my - sg.grab_dy - track.y;
                if (rel < 0) rel = 0;
                if (rel > travel) rel = travel;
                sg.scroll = (rel * m + travel / 2) / travel;
                clamp_scroll();
                GUIWidget *tw = &sg.dialog.children[sg.idx_thumb];
                tw->rect.y = track.y + rel + (tw->rect.y - thumb_draw.y);
            }
        }
    }

    char clicked[64];
    clicked[0] = '\0';
    int clicked_idx = GUIRuntime_Update(sg.rt, mx, my, mouse_down,
                                        clicked, sizeof clicked);
    (void)clicked_idx;

    /* A click in the list picks a row, and a second click on the same
     * row joins it, which is how the original's lists behave. */
    if (mouse_down && !sg.prev_mouse && mx >= 0) {
        SDL_Point pt = { mx, my };
        SDL_Rect lr = list_rect();
        if (SDL_PointInRect(&pt, &lr)) {
            int row = sg.scroll + (my - lr.y) / row_height();
            if (row >= 0 && row < room_count()) {
                if (row == sg.selected) join_selected();
                sg.selected = row;
                fill_info();
            }
        }
        /* The name box is an edit field and not a button, so the
         * runtime never reports a press on it and the press has to be
         * found here, the way a row in the list is. Clicking it did
         * nothing at all until this. */
        const GUIWidget *nw = GUIDialog_FindByName(&sg.dialog, "Name");
        if (nw && SDL_PointInRect(&pt, &nw->rect)) sg_press("Name");
    }

    if (clicked[0]) sg_press(clicked);
    sg.prev_mouse = mouse_down;
    if (sg.next_state != GAMESTATE_SELECT_GAME) {
        int next = sg.next_state;
        sg.next_state = GAMESTATE_SELECT_GAME;
        return next;
    }

    /* The thumb sits on the track where the list is. */
    if (sg.idx_thumb >= 0 && sg.idx_track >= 0 && !sg.dragging) {
        SDL_Rect track, thumb;
        widget_draw_rect(sg.idx_track, &track);
        widget_draw_rect(sg.idx_thumb, &thumb);
        int travel = track.h - thumb.h;
        if (travel < 0) travel = 0;
        int m = max_scroll();
        GUIWidget *tw = &sg.dialog.children[sg.idx_thumb];
        int rel = m > 0 ? (travel * sg.scroll) / m : 0;
        tw->rect.y = track.y + rel + (tw->rect.y - thumb.y);
    }
    GUIRuntime_Render(sg.rt);
    draw_rows();
    draw_name();
    draw_address();
    draw_status();
    /* Drawing is not showing. Every other screen presents its own
     * frame, and without this one the canvas is filled and never
     * reaches the window, which is a black screen that no test reading
     * the canvas can see. A browser found it. */
    UI_Present(platform);
    return GAMESTATE_SELECT_GAME;
}
