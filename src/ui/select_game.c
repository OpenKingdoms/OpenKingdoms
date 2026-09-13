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

#include "tak_select_game.h"

#include "tak_font.h"
#include "tak_gameloop.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_net_session.h"
#include "tak_ui.h"
#include "tak_util.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define SG_ADDRESS_MAX 96
#define SG_STATUS_MAX  128

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
    int  typing_address;

    /* Asked for a room list and waiting, so the screen can say so
     * rather than showing an empty list that looks like no games. */
    int  listing;

    int  prev_mouse, prev_enter, prev_esc, prev_back;
    int  dragging, grab_dy;
    int  next_state;
} sg;

static TAK_NetClient *client(void) { return NetSession_Client(); }

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

static void connect_to(const char *address) {
    if (!address || !address[0]) {
        set_status("Type the address of a server to join.");
        return;
    }
    if (NetSession_Connect(address, "Player") != 0) {
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
        set_status("That game is not one this build can join.");
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

static void host_game(void) {
    TAK_NetClient *c = client();
    if (!c) {
        set_status("Connect to a server first.");
        return;
    }
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    snprintf(cr.name, sizeof cr.name, "%s's game", "Player");
    cr.flags = TAK_ROOMF_LISTED | TAK_ROOMF_ALLOW_WATCHING;
    cr.max_players = TAK_NET_SEATS;
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
         * hidden, which is the one thing the original got wrong here. */
        snprintf(line, sizeof line, "%s%-24s %s  %u/%u",
                 s->compat ? "- " : "  ",
                 s->name[0] ? s->name : "(no name)",
                 s->map_name[0] ? s->map_name : "(no map)",
                 (unsigned)s->players, (unsigned)s->max_players);
        Font_DrawString(sg.font_row, off, r.x + 4, r.y + i * rh + 2, line);
    }
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

    NetSession_DefaultAddress(sg.address, sizeof sg.address);
    if (sg.address[0]) {
        /* A browser came from somewhere, so there is a server to try
         * and the player never has to know its name. */
        connect_to(sg.address);
    } else {
        set_status("Type the address of a server to join.");
    }
    sg.open = 1;
    return 0;
}

void SelectGame_Shutdown(void) {
    if (sg.rt) GUIRuntime_Destroy(sg.rt);
    if (sg.has_dialog) GUIDialog_Free(&sg.dialog);
    if (sg.font_row) Font_Free(sg.font_row);
    if (sg.font_help && sg.font_help != sg.font_row) Font_Free(sg.font_help);
    memset(&sg, 0, sizeof sg);
}

int SelectGame_RowCount(void) { return room_count(); }
int SelectGame_Selected(void) { return sg.selected; }

const char *SelectGame_RowName(int index) {
    TAK_NetClient *c = client();
    if (!c || index < 0 || index >= room_count()) return NULL;
    return c->rooms.room[index].name;
}

const char *SelectGame_Status(void) { return sg.status; }

/* Everything the session did since the last frame, turned into the one
 * line the screen shows and the state it moves to. */
static void take_events(void) {
    TAK_NetClient *c = client();
    if (!c) return;
    TAK_NetClientEvent e;
    while (TAK_NetClient_PollEvent(c, &e)) {
        switch (e.kind) {
        case TAK_NC_EV_WELCOMED:
            set_status("Connected.");
            ask_for_rooms();
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
            break;
        case TAK_NC_EV_ROOM_STATE:
            /* In a room, so the battle room takes over from here. */
            sg.next_state = GAMESTATE_MULTIPLAYER;
            break;
        case TAK_NC_EV_REFUSED:
            set_status(c->reject.text[0] ? c->reject.text
                                         : "The server said no.");
            break;
        case TAK_NC_EV_GONE:
            set_status("The connection closed.");
            break;
        default:
            break;
        }
    }
}

int SelectGame_Tick(TAK_Platform *platform, float dt) {
    (void)dt;
    if (!sg.open || !sg.rt) return GAMESTATE_MENU;

    NetSession_Tick(SDL_GetTicks64());
    if (NetSession_State() == NET_SESSION_FAILED) {
        const char *why = NetSession_Why();
        if (why && why[0]) set_status(why);
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
    int enter_edge = enter && !sg.prev_enter;
    int esc_edge   = esc && !sg.prev_esc;
    int back_edge  = back && !sg.prev_back;
    sg.prev_enter = enter;
    sg.prev_esc = esc;
    sg.prev_back = back;

    int mx = -1, my = -1, mouse_down = 0;
    if (focus) {
        int wx = 0, wy = 0;
        uint32_t buttons = SDL_GetMouseState(&wx, &wy);
        mouse_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
            mx = -1; my = -1;
        }
    }

    /* The screen's own keys, which the .gui names: Enter joins and
     * Escape goes back to the main menu. */
    if (esc_edge) {
        NetSession_Disconnect();
        return GAMESTATE_MENU;
    }

    if (sg.typing_address) {
        if (back_edge) {
            size_t n = strlen(sg.address);
            if (n) sg.address[n - 1] = '\0';
        }
        if (platform && platform->text_in_len > 0) {
            for (int i = 0; i < platform->text_in_len; i++) {
                size_t n = strlen(sg.address);
                if (n + 1 >= sizeof sg.address) break;
                sg.address[n] = platform->text_in[i];
                sg.address[n + 1] = '\0';
            }
        }
        if (enter_edge) {
            sg.typing_address = 0;
            connect_to(sg.address);
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
            }
        }
    }

    if (clicked[0]) {
        if (tak_stricmp(clicked, "MainMenu") == 0) {
            NetSession_Disconnect();
            sg.prev_mouse = mouse_down;
            return GAMESTATE_MENU;
        } else if (tak_stricmp(clicked, "Update") == 0) {
            ask_for_rooms();
        } else if (tak_stricmp(clicked, "Join") == 0) {
            join_selected();
        } else if (tak_stricmp(clicked, "HostGame") == 0) {
            host_game();
        } else if (tak_stricmp(clicked, "Boneyards") == 0) {
            /* The service this button named is gone. It connects to
             * where this build came from instead, which in a browser is
             * the page's own origin. */
            char def[SG_ADDRESS_MAX];
            NetSession_DefaultAddress(def, sizeof def);
            if (def[0]) {
                memcpy(sg.address, def, sizeof def);
                connect_to(sg.address);
            } else {
                set_status("This build has no server of its own. "
                           "Type an address.");
            }
        } else if (tak_stricmp(clicked, "EnterTCPIPAddress") == 0) {
            sg.typing_address = 1;
            set_status("Type an address, then press Enter.");
        }
    }
    sg.prev_mouse = mouse_down;

    GUIRuntime_Render(sg.rt);
    draw_rows();
    draw_address();
    draw_status();
    return GAMESTATE_SELECT_GAME;
}
