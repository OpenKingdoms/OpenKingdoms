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
#include "tak_map_fingerprint.h"
#include "tak_net_session.h"
#include "tak_net_room.h"
#include "tak_blit.h"
#include "tak_gaf.h"
#include "tak_hpi.h"
#include "tak_palette.h"
#include "tak_tdf.h"
#include "tak_tnt.h"
#include "tak_settings.h"
#include "tak_tdf.h"
#include "tak_unit.h"
#include "tak_world.h"
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
static TAK_Platform  *mp_platform;
static TranslateTable mp_tt;
static Font          *mp_font;
/* The host's side and its Allow Creon choice. Creon needs both that
 * choice and the expansion (legacy:134048-134052). */
static int            mp_host_side = 0;
static int            mp_allow_creon = 0;

static const SimpleScreenClick mp_routes[] = {
    { NULL, 0 }
};

/* The room's chat, as the original keeps it: the last lines, newest at
 * the bottom, and one line being typed under them (legacy:136856). */
#define MP_CHAT_LINES 32
static struct {
    char lines[MP_CHAT_LINES][TAK_NET_NAME_MAX + TAK_NET_CHAT_MAX + 4];
    int  count, head;
    char typing[TAK_NET_CHAT_MAX];
} mp_chat;

/* The rule checkboxes, each to the bit it carries on the wire. The
 * names are the widget names battlemenumulti.gui authors. */
static const struct { const char *widget; uint32_t bit; } mp_rules[] = {
    { "LineOfSight",    TAK_ROOMOPT_LINE_OF_SIGHT },
    { "Mapping",        TAK_ROOMOPT_MAP_REVEALED },
    { "MonarchDeath",   TAK_ROOMOPT_MONARCH_EXPEND },
    { "StartLocations", TAK_ROOMOPT_RANDOM_STARTS },
    { "CheatCodes",     TAK_ROOMOPT_POWER_CODES },
    { "SlowGame",       TAK_ROOMOPT_SLOW_GAME },
    { "Crusades",       TAK_ROOMOPT_CRUSADES_BALANCE },
};
#define MP_RULES ((int)(sizeof mp_rules / sizeof mp_rules[0]))

static const TAK_MsgRoomState *mp_room_state(void);
static int mp_row_of(const GUIWidget *w);

/* The side's team logo sheet, one frame per colour, decoded once.
 * Loaded the way the skirmish screen loads it. */
static struct {
    GAFFile  *gaf;
    uint32_t  rgba[256];
    uint32_t *frame[TAK_SIDES_MAX][TAK_PLAYER_COLOR_COUNT];
    int       w[TAK_SIDES_MAX][TAK_PLAYER_COLOR_COUNT];
    int       h[TAK_SIDES_MAX][TAK_PLAYER_COLOR_COUNT];
    int       loaded;
} mp_logo;

static void mp_art_path(const char *stem, const char *ext, char *out, size_t cap) {
    snprintf(out, cap, "anims/%s.%s", stem, ext);
    if (VFS_FileExists(out) != 0) snprintf(out, cap, "data/anims/%s.%s", stem, ext);
}

static void mp_load_logos(void) {
    if (mp_logo.loaded) return;
    mp_logo.loaded = 1;
    int count = Sides_Count();
    if (count > TAK_SIDES_MAX) count = TAK_SIDES_MAX;
    char stem[64] = "colorlogos2";
    for (int s = 0; s < count; s++) {
        const TakSideInfo *si = Sides_Get(s);
        if (si && si->logogaf[0]) snprintf(stem, sizeof stem, "%s", si->logogaf);
    }
    char gaf_path[128], pcx_path[128];
    mp_art_path(stem, "gaf", gaf_path, sizeof gaf_path);
    mp_art_path(stem, "pcx", pcx_path, sizeof pcx_path);
    if (UI_LoadGAFWithPalette(gaf_path, pcx_path, &mp_logo.gaf, mp_logo.rgba) != 0) {
        mp_logo.gaf = NULL;
        return;
    }
    for (int s = 0; s < count; s++) {
        const TakSideInfo *si = Sides_Get(s);
        if (!si || !si->commander[0]) continue;
        int off = si->logoart[0] ? GAF_FindSequence(mp_logo.gaf, si->logoart) : -1;
        if (off < 0 && s < (int)mp_logo.gaf->num_entries &&
            12u + (uint32_t)(s + 1) * 4u <= mp_logo.gaf->data_size) {
            off = (int)*(uint32_t *)(mp_logo.gaf->data + 12 + s * 4);
        }
        if (off < 0 || (uint32_t)off + sizeof(EntryHeader) > mp_logo.gaf->data_size) continue;
        EntryHeader *eh = (EntryHeader *)(mp_logo.gaf->data + off);
        int nframes = (int)eh->num_frames;
        /* The twelve frame sheets lead with two greyed states, so the
         * colour index starts at frame 2 there (legacy:136390). */
        int base = (nframes >= TAK_PLAYER_COLOR_COUNT + 2) ? 2 : 0;
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
            if (base + c >= nframes) break;
            mp_logo.frame[s][c] = UI_DecodeFrame(mp_logo.gaf, off, base + c, mp_logo.rgba,
                                                 &mp_logo.w[s][c], &mp_logo.h[s][c]);
        }
    }
}

static void mp_free_logos(void) {
    for (int s = 0; s < TAK_SIDES_MAX; s++)
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
            tak_free(mp_logo.frame[s][c]);
            mp_logo.frame[s][c] = NULL;
        }
    if (mp_logo.gaf) GAF_Close(mp_logo.gaf);
    memset(&mp_logo, 0, sizeof mp_logo);
}

static void mp_draw_badge(SDL_Surface *off, int cx, int cy, int colour, int side) {
    if (colour < 0) colour = 0;
    colour %= TAK_PLAYER_COLOR_COUNT;
    if (side < 0 || side >= TAK_SIDES_MAX) side = 0;
    uint32_t *px = mp_logo.frame[side][colour];
    int w = mp_logo.w[side][colour], h = mp_logo.h[side][colour];
    if (!px || w <= 0 || h <= 0) {
        const TakPlayerColor *pc = BattleConfig_PlayerColor(colour);
        SDL_Rect r = { cx - 6, cy - 6, 12, 12 };
        SDL_FillRect(off, &r, SDL_MapRGBA(off->format, pc->r, pc->g, pc->b, 255));
        return;
    }
    Blit_RGBA(off, cx - w / 2, cy - h / 2, px, w, h);
}

/* Over every taken row: the badge in the colour cell and the team in
 * the team cell, both from the server's snapshot. */
static void mp_draw_cells(void) {
    SDL_Surface *off = UI_Offscreen();
    const TAK_MsgRoomState *rs = mp_room_state();
    if (!off || !rs) return;
    mp_load_logos();
    for (int i = 0; i < mp.dialog.num_children; i++) {
        const GUIWidget *w = &mp.dialog.children[i];
        int row = mp_row_of(w);
        if (row < 0) continue;
        const TAK_NetSlot *slot = &rs->slot[row];
        if (slot->kind == 0) continue;
        if (tak_stricmp(w->name, "PlayerColor") == 0) {
            mp_draw_badge(off, w->rect.x + w->rect.w / 2, w->rect.y + w->rect.h / 2,
                          slot->colour, slot->side);
        } else if (tak_stricmp(w->name, "PlayerTeam") == 0 && slot->team && mp_font) {
            char team[8];
            snprintf(team, sizeof team, "%u", (unsigned)slot->team);
            Font_DrawString(mp_font, off, w->rect.x + 4, w->rect.y + 2, team);
        }
    }
}

static int mp_is_host(void) {
    TAK_NetClient *c = NetSession_Client();
    return c && c->room.room_id != 0 && c->room.host_client_id == c->session_id;
}

/* One line on the help strip, which is where the original puts what a
 * player needs to read. A refusal from the server lands here too, so a
 * Play that the server would not allow says why rather than nothing. */
static void mp_say(const char *text) {
    if (mp.rt) GUIRuntime_SetWidgetText(mp.rt, "HelpText", text ? text : "");
}

/* An edit the host makes to the room's own fields. Anyone else's press
 * on them is ignored, which is the original's rule (legacy:136832). */
static void mp_edit_room(uint8_t field, uint32_t value) {
    TAK_NetClient *c = NetSession_Client();
    if (!c || !mp_is_host()) return;
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = field;
    e.seat = TAK_NET_SEAT_NONE;
    e.value = value;
    (void)TAK_NetClient_EditRoom(c, &e);
}

static void mp_chat_push(const char *who, const char *text) {
    char *slot = mp_chat.lines[(mp_chat.head + mp_chat.count) % MP_CHAT_LINES];
    if (who && who[0]) snprintf(slot, sizeof mp_chat.lines[0], "%s: %s", who, text);
    else               snprintf(slot, sizeof mp_chat.lines[0], "%s", text);
    if (mp_chat.count < MP_CHAT_LINES) mp_chat.count++;
    else mp_chat.head = (mp_chat.head + 1) % MP_CHAT_LINES;
}

const char *Multiplayer_ChatTyping(void)  { return mp_chat.typing; }
int         Multiplayer_ChatLineCount(void) { return mp_chat.count; }
const char *Multiplayer_ChatLine(int index) {
    if (index < 0 || index >= mp_chat.count) return "";
    return mp_chat.lines[(mp_chat.head + index) % MP_CHAT_LINES];
}
static void mp_chat_send(void);
void Multiplayer_ChatSend(void) { mp_chat_send(); }

static void mp_chat_send(void) {
    TAK_NetClient *c = NetSession_Client();
    if (!mp_chat.typing[0]) return;
    if (c && c->room.room_id != 0) {
        TAK_MsgChat m;
        memset(&m, 0, sizeof m);
        m.scope = TAK_CHAT_ROOM;
        m.from_seat = c->seat;
        m.to_seat = TAK_NET_SEAT_NONE;
        snprintf(m.text, sizeof m.text, "%s", mp_chat.typing);
        (void)TAK_NetClient_Chat(c, &m);
    }
    mp_chat.typing[0] = '\0';
}

static int mp_row_of(const GUIWidget *w);

/* Whether this install has the room's map, by fingerprint rather than
 * by name, because two installs can hold different maps under one
 * name. The host cannot start until every human has said yes, and
 * saying nothing reads as no, so this is sent whenever the room's map
 * changes and not only on the way in.
 *
 * A map we do not have is reported too. The server greys the row and
 * says why, which is the whole point of carrying the reason. */
static void mp_report_have_map(void) {
    static uint8_t sent_for[TAK_NET_FINGERPRINT_BYTES];
    static int     sent_any;
    TAK_NetClient *c = NetSession_Client();
    if (!c || c->seat == TAK_NET_SEAT_NONE) return;
    if (!c->room.map_name[0]) return;
    if (sent_any &&
        memcmp(sent_for, c->room.map_fingerprint, sizeof sent_for) == 0) {
        return;
    }
    memcpy(sent_for, c->room.map_fingerprint, sizeof sent_for);
    sent_any = 1;

    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = TAK_EDIT_HAVE_MAP;
    /* Our own fingerprint for that map name. The server compares it
     * with the host's and decides. If we cannot compute one we send
     * zeroes, which cannot match, which is the honest answer. */
    (void)TAK_MapFingerprint_FromName(c->room.map_name, e.fingerprint);
    (void)TAK_NetClient_EditRoom(c, &e);
}

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

/* The host's edit to another seat. */
static void mp_edit_slot(uint8_t field, int seat) {
    TAK_NetClient *c = NetSession_Client();
    if (!c || !mp_is_host()) return;
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = field;
    e.seat = (uint8_t)seat;
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
        /* The room's own fields. The host changes them and everyone
         * else sees the result arrive in the next snapshot. */
        for (int i = 0; i < MP_RULES; i++) {
            if (tak_stricmp(name, mp_rules[i].widget) != 0) continue;
            if (!mp_is_host()) { mp_say("Only the host changes the rules."); return 1; }
            mp_edit_room(TAK_EDIT_OPTIONS, c->room.options ^ mp_rules[i].bit);
            return 1;
        }
        if (tak_stricmp(name, "MaxUnits") == 0 ||
            tak_stricmp(name, "incbutton") == 0 ||
            tak_stricmp(name, "decbutton") == 0) {
            const GUIWidget *uw = GUIRuntime_WidgetAt(mp.rt, widget_index);
            /* The chat list has nubs of the same name lower down. */
            if (!uw || uw->rect.y > 240) return 0;
            if (!mp_is_host()) { mp_say("Only the host sets the unit limit."); return 1; }
            int cap = (int)c->room.unit_cap;
            int mx = -1, my = -1;
            (void)TAK_Platform_MouseThisFrame(mp_platform, &mx, &my);
            /* The nubs sit on the bar and the bar is what the runtime
             * reports, so a press is checked against the nubs first. */
            int on_inc = 0, on_dec = 0;
            for (int i = 0; i < mp.dialog.num_children; i++) {
                const GUIWidget *nw = &mp.dialog.children[i];
                if (nw->rect.y > 240) continue;
                SDL_Point pt = { mx, my };
                if (!SDL_PointInRect(&pt, &nw->rect)) continue;
                if (tak_stricmp(nw->name, "incbutton") == 0) on_inc = 1;
                if (tak_stricmp(nw->name, "decbutton") == 0) on_dec = 1;
            }
            if (on_inc || tak_stricmp(name, "incbutton") == 0) cap += TAK_UNITS_PER_SIDE_STEP;
            else if (on_dec || tak_stricmp(name, "decbutton") == 0) cap -= TAK_UNITS_PER_SIDE_STEP;
            else {
                /* A press on the bar sets the value where it landed. */
                if (mx >= 0 && uw->rect.w > 0) {
                    int rel = mx - uw->rect.x;
                    if (rel < 0) rel = 0;
                    if (rel > uw->rect.w) rel = uw->rect.w;
                    cap = TAK_UNITS_PER_SIDE_MIN +
                          (int)((int64_t)rel * (TAK_UNITS_PER_SIDE_MAX - TAK_UNITS_PER_SIDE_MIN) / uw->rect.w);
                    cap = (cap / TAK_UNITS_PER_SIDE_STEP) * TAK_UNITS_PER_SIDE_STEP;
                }
            }
            if (cap < TAK_UNITS_PER_SIDE_MIN) cap = TAK_UNITS_PER_SIDE_MIN;
            if (cap > TAK_UNITS_PER_SIDE_MAX) cap = TAK_UNITS_PER_SIDE_MAX;
            mp_edit_room(TAK_EDIT_UNIT_CAP, (uint32_t)cap);
            return 1;
        }
        if (tak_stricmp(name, "Map") == 0 || tak_stricmp(name, "ViewMap") == 0) {
            Multiplayer_OpenMapChooser(mp_is_host());
            return 1;
        }
        const GUIWidget *w = GUIRuntime_WidgetAt(mp.rt, widget_index);
        int row = w ? mp_row_of(w) : -1;
        /* Another seat's name cycles it: open, computer, closed, open,
         * and a human out of it (legacy:136190-136218). */
        if (row >= 0 && (uint8_t)row != c->seat &&
            tak_stricmp(name, "PlayerName") == 0) {
            if (!mp_is_host()) { mp_say("Only the host changes a slot."); return 1; }
            switch (c->room.slot[row].kind) {
                case TAK_NSLOT_EMPTY:    mp_edit_slot(TAK_EDIT_ADD_COMPUTER, row); break;
                case TAK_NSLOT_COMPUTER: mp_edit_slot(TAK_EDIT_REMOVE_COMPUTER, row);
                                         mp_edit_slot(TAK_EDIT_BLOCK_SLOT, row); break;
                case TAK_NSLOT_BLOCKED:  mp_edit_slot(TAK_EDIT_UNBLOCK_SLOT, row); break;
                case TAK_NSLOT_HUMAN:    mp_edit_slot(TAK_EDIT_KICK, row); break;
                default: break;
            }
            return 1;
        }
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
    int taken = slot->kind == TAK_NSLOT_HUMAN || slot->kind == TAK_NSLOT_COMPUTER;
    if (tak_stricmp(w->name, "PlayerName") == 0) {
        GUIRuntime_SetWidgetTextAt(mp.rt, widget,
            taken && slot->name[0] ? slot->name
            : slot->kind == TAK_NSLOT_BLOCKED ? Translate_Lookup(&mp_tt, "Closed")
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
    } else if (tak_stricmp(w->name, "PlayerReady") == 0) {
        /* 3 is the empty box and 4 the ticked one on the five frame
         * sheet (legacy:139330). */
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget, taken);
        GUIRuntime_SetFrameOverrideAt(mp.rt, widget, slot->ready ? 4 : 3);
    } else if (tak_stricmp(w->name, "PlayerColor") == 0) {
        /* The twelve frame sheet leads with two greyed states, so the
         * colour index starts at frame 2 (legacy:136390). */
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget, taken);
    } else if (tak_stricmp(w->name, "PlayerTeam") == 0) {
        char team[8] = "";
        if (taken && slot->team) snprintf(team, sizeof team, "%u", (unsigned)slot->team);
        GUIRuntime_SetWidgetTextAt(mp.rt, widget, team);
        GUIRuntime_SetWidgetVisibleAt(mp.rt, widget, taken);
    }
}

/* The room's own fields, from the snapshot: the rule boxes, the unit
 * limit and the map name. Drawn for everyone, changed by the host. */
static void mp_fill_room_fields(void) {
    const TAK_MsgRoomState *rs = mp_room_state();
    if (!rs || !mp.rt) return;
    for (int i = 0; i < MP_RULES; i++) {
        GUIRuntime_SetFrameOverride(mp.rt, mp_rules[i].widget,
                                    (rs->options & mp_rules[i].bit) ? 4 : 3);
    }
    char units[16];
    snprintf(units, sizeof units, "%u", (unsigned)rs->unit_cap);
    GUIRuntime_SetWidgetText(mp.rt, "NumberOfUnits", units);
    if (rs->map_name[0]) {
        char shown[128];
        Translate_MapName(&mp_tt, rs->map_name, shown, sizeof shown);
        GUIRuntime_SetWidgetText(mp.rt, "MapName", shown);
    }
    /* The host picks the map, anyone else only views it
     * (legacy:136832-136851). */
    int host = mp_is_host();
    GUIRuntime_SetWidgetVisible(mp.rt, "Map", host);
    GUIRuntime_SetWidgetVisible(mp.rt, "ViewMap", !host);
}

static void mp_fill_rows(void) {
    mp_fill_room_fields();
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
static void mp_draw_chat(SimpleScreen *s);
static void mp_draw_button_text_only(SimpleScreen *s);
static void mp_draw_button_text(SimpleScreen *s) {
    mp_draw_button_text_only(s);
    mp_draw_cells();
    mp_draw_chat(s);
}
static void mp_draw_button_text_only(SimpleScreen *s) {
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
    memset(&mp_chat, 0, sizeof mp_chat);
    SDL_StartTextInput();
    return 0;
}

/* The map's own faction, which picks the minimap palette. It is read
 * out of the map rather than off the wire: every seat had to prove it
 * holds this exact map before the host could start, so every seat
 * reads the same answer. */
static void mp_map_kingdom(const char *map, char *out, size_t cap) {
    out[0] = '\0';
    char path[512];
    TAK_Maps_FindFile(map, "ota", path, sizeof path);
    TDFFile *tdf = TDF_Open(path);
    if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "GlobalHeader") == 0) {
        const char *kingdom = TDF_ReadString(tdf, "kingdom", "");
        size_t k = 0;
        for (; kingdom && kingdom[k] && k + 1 < cap; k++) {
            char ch = kingdom[k];
            out[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
        }
        out[k] = '\0';
    }
    if (tdf) TDF_Close(tdf);
}

/* The battle the server just described, in the terms a world is built
 * from. Every value here comes off the wire. One a machine picked for
 * itself is a desync waiting for the first tick. */
static void mp_config_from_start(const TAK_MsgStartGame *sg,
                                 BattleConfig *cfg) {
    BattleConfig_SetDefaults(cfg);
    snprintf(cfg->map_name, sizeof cfg->map_name, "%s", sg->map_name);
    cfg->seed = sg->seed;
    cfg->units_per_side = sg->unit_cap ? (int)sg->unit_cap
                                       : TAK_UNITS_PER_SIDE_DEFAULT;
    cfg->line_of_sight          = (sg->options & TAK_ROOMOPT_LINE_OF_SIGHT) != 0;
    cfg->map_revealed           = (sg->options & TAK_ROOMOPT_MAP_REVEALED) != 0;
    cfg->monarch_expendable     = (sg->options & TAK_ROOMOPT_MONARCH_EXPEND) != 0;
    cfg->random_start_locations = (sg->options & TAK_ROOMOPT_RANDOM_STARTS) != 0;
    cfg->power_codes            = (sg->options & TAK_ROOMOPT_POWER_CODES) != 0;
    cfg->slow_game              = (sg->options & TAK_ROOMOPT_SLOW_GAME) != 0;
    cfg->crusades_balance       = (sg->options & TAK_ROOMOPT_CRUSADES_BALANCE) != 0;

    for (int i = 0; i < TAK_MAX_PLAYERS && i < TAK_NET_SEATS; i++) {
        PlayerSlot *p = &cfg->players[i];
        memset(p, 0, sizeof *p);
        switch (sg->slot[i].kind) {
        case TAK_NSLOT_HUMAN:    p->kind = TAK_SLOT_HUMAN;  break;
        case TAK_NSLOT_COMPUTER: p->kind = TAK_SLOT_AI;     break;
        default:                 p->kind = TAK_SLOT_CLOSED; break;
        }
        p->side  = sg->slot[i].side;
        p->team  = sg->slot[i].team;
        p->color = sg->slot[i].colour;
        snprintf(p->name, sizeof p->name, "%s", sg->slot[i].name);
    }
}

/* Build it. Returns 0, or -1 when this install cannot, which is worth
 * a message and a way back rather than a screen that never finishes. */
static int mp_begin_match_world(TAK_Platform *platform,
                                const TAK_MsgStartGame *sg) {
    if (!sg->map_name[0]) return -1;
    BattleConfig cfg;
    mp_config_from_start(sg, &cfg);
    char kingdom[32];
    mp_map_kingdom(sg->map_name, kingdom, sizeof kingdom);
    if (World_BeginLoad(platform, &cfg, sg->map_name, kingdom) != 0) return -1;
    /* Seats count from zero on the wire and players from one in the
     * simulation. Without this every client plays the first seat: same
     * world, same fog, same sidebar, and one army nobody is driving. */
    Units_SetLocalPlayer((int)sg->your_seat + 1);
    return 0;
}

/* What the session did since the last frame. The room is a view of the
 * server's snapshot, so everything here either refills the rows or
 * moves the screen on. */
static int mp_take_events(TAK_Platform *platform) {
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
            mp_report_have_map();
            break;
        case TAK_NC_EV_REFUSED:
            /* The server said no to something this screen asked. Play
             * with a seat not ready, a map someone lacks, everyone on
             * one team: each has words, and the strip is where they go.
             * Silence here is what made Go look broken. */
            mp_say(c->reject.text[0] ? c->reject.text : "The server said no.");
            break;
        case TAK_NC_EV_CHAT:
            mp_chat_push(c->chat.name, c->chat.text);
            break;
        case TAK_NC_EV_START_GAME:
            /* Every client builds the same world from the same seed,
             * so the loading screen takes it from here. Building it is
             * this screen's job: the loading screen loads a world, it
             * does not make one, and handing it none is a bar stuck at
             * ten per cent with nothing said. */
            if (mp_begin_match_world(platform, &c->start) == 0) {
                next = GAMESTATE_GAME_LOADING;
            } else {
                fprintf(stderr,
                        "Multiplayer: no world could be built for %s\n",
                        c->start.map_name);
                (void)TAK_NetClient_LeaveRoom(c);
                next = GAMESTATE_SELECT_GAME;
            }
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

/* The chat line. The original has no separate box: what is typed
 * appears under the last line and Enter sends it (legacy:136856). Text
 * input is on for the whole of the room so a player can just type. */
static void mp_chat_keys(TAK_Platform *platform) {
    static int prev_enter, prev_back;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int focus = platform && platform->has_focus;
    int enter = focus && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);
    int back  = focus && keys[SDL_SCANCODE_BACKSPACE];
    if ((back && !prev_back) || (focus && platform->pressed_backspace)) {
        size_t n = strlen(mp_chat.typing);
        if (n) mp_chat.typing[n - 1] = '\0';
    }
    if (platform && platform->text_in_len > 0) {
        for (int i = 0; i < platform->text_in_len; i++) {
            size_t n = strlen(mp_chat.typing);
            if (n + 1 >= sizeof mp_chat.typing) break;
            mp_chat.typing[n] = platform->text_in[i];
            mp_chat.typing[n + 1] = '\0';
        }
    }
    if ((enter && !prev_enter) || (focus && platform->pressed_enter)) mp_chat_send();
    prev_enter = enter;
    prev_back = back;
}

static void mp_draw_chat(SimpleScreen *s) {
    (void)s;
    SDL_Surface *off = UI_Offscreen();
    const GUIWidget *box = GUIDialog_FindByName(&mp.dialog, "UserChat");
    if (!off || !box || !mp_font) return;
    int lh = Font_LineHeight(mp_font);
    if (lh <= 0) lh = 12;
    SDL_Rect r = box->rect;
    /* The line being typed sits at the bottom, the history above it. */
    int y = r.y + r.h - lh - 2;
    char typed[TAK_NET_CHAT_MAX + 2];
    snprintf(typed, sizeof typed, "%s_", mp_chat.typing);
    Font_DrawString(mp_font, off, r.x + 4, y, typed);
    y -= lh;
    for (int i = mp_chat.count - 1; i >= 0 && y >= r.y; i--) {
        const char *line = mp_chat.lines[(mp_chat.head + i) % MP_CHAT_LINES];
        Font_DrawString(mp_font, off, r.x + 4, y, line);
        y -= lh;
    }
}

int Multiplayer_Tick(TAK_Platform *platform, float frame_dt) {
    mp_platform = platform;
    NetSession_Tick(SDL_GetTicks64());
    int next = mp_take_events(platform);
    if (next != GAMESTATE_MULTIPLAYER) return next;
    if (Multiplayer_MapChooserOpen()) {
        return Multiplayer_MapChooserTick(platform, frame_dt);
    }
    mp_chat_keys(platform);
    return SimpleScreen_Tick(&mp, platform, frame_dt);
}

void Multiplayer_Shutdown(void) {
    SDL_StopTextInput();
    Multiplayer_CloseMapChooser();
    mp_free_logos();
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


/* ── The map chooser ──────────────────────────────────────────────────
 *
 * The host presses Map and gets choosemap.gui: a list of every map the
 * install offers, a preview, the map's own description, OK and Cancel.
 * Anyone else presses View Map and gets viewmap.gui, which is the same
 * panel with no list and no Cancel (legacy:136832-136851). OK on the
 * host's sends the server the map's name and fingerprint in one edit,
 * so the room's map is what this install has under that name and not
 * a name two installs might disagree about.
 *
 * It owns its own dialog and runtime, the way the save browser does,
 * and takes the whole frame while it is up. */

typedef struct MpMapRow { char key[96]; char display[128]; } MpMapRow;

static struct {
    int         open, host;
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    MpMapRow   *rows;
    int         count, selected, scroll;
    int         idx_list;
    /* The list's own bar: the widgets choosemap.gui authors beside the
     * list, told from the description's bar by where they sit. */
    int         idx_track, idx_thumb, idx_inc, idx_dec;
    int         dragging, grab_dy;
    int         prev_mouse;
    /* The selected map's picture and words. */
    TNTFile     tnt;
    int         have_tnt;
    char        desc[512];
} mc;

static int mc_row_cmp(const void *a, const void *b) {
    return tak_stricmp(((const MpMapRow *)a)->display, ((const MpMapRow *)b)->display);
}

static int mc_row_height(void) {
    const GUIWidget *t = GUIDialog_FindByName(&mc.dialog, "MapNameEntryTemplate");
    return (t && t->rect.h > 0) ? t->rect.h : 19;
}

static SDL_Rect mc_list_rect(void) {
    SDL_Rect r = { 0, 0, 0, 0 };
    if (mc.idx_list >= 0) r = mc.dialog.children[mc.idx_list].rect;
    if (mc.idx_track >= 0) {
        const GUIWidget *t = &mc.dialog.children[mc.idx_track];
        if (t->rect.x > r.x) r.w = t->rect.x - r.x;
    }
    return r;
}

/* Where a widget's art lands, since the bar's art sits inside its
 * nubs by a hotspot and the .gui rect alone is off. */
static void mc_draw_rect(int idx, SDL_Rect *out) {
    out->x = out->y = 0;
    out->w = out->h = 1;
    if (idx < 0 || idx >= mc.dialog.num_children) return;
    if (!mc.rt || GUIRuntime_WidgetDrawRect(mc.rt, idx, out) != 0)
        *out = mc.dialog.children[idx].rect;
}

static int mc_rows_visible(void) {
    SDL_Rect r = mc_list_rect();
    int h = mc_row_height();
    return (h > 0 && r.h > 0) ? r.h / h : 1;
}

static void mc_clamp_scroll(void) {
    int m = mc.count - mc_rows_visible();
    if (m < 0) m = 0;
    if (mc.scroll > m) mc.scroll = m;
    if (mc.scroll < 0) mc.scroll = 0;
}

/* Picture and words for the selected map. The picture is the map's
 * own minimap through the map's own palette, cropped to its content
 * the way the skirmish preview crops it (see battle_setup.c). */
static void mc_load_selected(void) {
    if (mc.have_tnt) { TNT_Close(&mc.tnt); mc.have_tnt = 0; }
    mc.desc[0] = '\0';
    if (mc.selected < 0 || mc.selected >= mc.count) return;
    const char *key = mc.rows[mc.selected].key;

    char kingdom[32];
    mp_map_kingdom(key, kingdom, sizeof kingdom);
    uint32_t rgba[256];
    int have_pal = 0;
    if (kingdom[0]) {
        char pcx[128];
        snprintf(pcx, sizeof pcx, "data/palettes/%s.pcx", kingdom);
        Palette pal;
        if (Palette_LoadPCX(&pal, pcx) == 0) {
            Palette_BuildRGBATable(&pal, UI_RGBAFormat(), rgba, 0);
            have_pal = 1;
        }
    }
    char path[512];
    if (have_pal && TAK_Maps_FindFile(key, "tnt", path, sizeof path) == 0 &&
        TNT_Load(&mc.tnt, path, rgba) == 0) {
        mc.have_tnt = 1;
    }

    if (TAK_Maps_FindFile(key, "ota", path, sizeof path) == 0) {
        TDFFile *tdf = TDF_Open(path);
        if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "GlobalHeader") == 0) {
            const char *raw = TDF_ReadString(tdf, "missiondescription", "");
            snprintf(mc.desc, sizeof mc.desc, "%s", Translate_Lookup(&mp_tt, raw ? raw : ""));
        }
        if (tdf) TDF_Close(tdf);
    }
    if (mc.rt) {
        GUIRuntime_SetWidgetText(mc.rt, "HelpText", "");
    }
}

static int mc_max_scroll(void) {
    int m = mc.count - mc_rows_visible();
    return m > 0 ? m : 0;
}

/* The thumb sits on the track where the list is, unless a drag has it. */
static void mc_sync_thumb(void) {
    if (mc.idx_thumb < 0 || mc.idx_track < 0 || mc.dragging) return;
    SDL_Rect track, thumb;
    mc_draw_rect(mc.idx_track, &track);
    mc_draw_rect(mc.idx_thumb, &thumb);
    int travel = track.h - thumb.h;
    if (travel < 0) travel = 0;
    int m = mc_max_scroll();
    GUIWidget *w = &mc.dialog.children[mc.idx_thumb];
    int rel = m > 0 ? (travel * mc.scroll) / m : 0;
    /* Rect, not draw position: the renderer re-applies the hotspot. */
    w->rect.y = track.y + rel + (w->rect.y - thumb.y);
}

/* The pointer over the chooser: the thumb drags, a press on the track
 * above or below it pages, and a press in the list picks a row. */
static void mc_pointer(int mx, int my, int down) {
    SDL_Point pt = { mx, my };
    if (mc.idx_thumb >= 0 && mc.idx_track >= 0) {
        SDL_Rect thumb;
        mc_draw_rect(mc.idx_thumb, &thumb);
        if (down && !mc.prev_mouse && !mc.dragging && SDL_PointInRect(&pt, &thumb)) {
            mc.dragging = 1;
            mc.grab_dy = my - thumb.y;
        }
        if (!down) mc.dragging = 0;
        if (mc.dragging) {
            SDL_Rect track;
            mc_draw_rect(mc.idx_track, &track);
            int th = thumb.h > 0 ? thumb.h : 1;
            int travel = track.h - th;
            int m = mc_max_scroll();
            if (travel > 0 && m > 0) {
                int rel = my - mc.grab_dy - track.y;
                if (rel < 0) rel = 0;
                if (rel > travel) rel = travel;
                mc.scroll = (rel * m + travel / 2) / travel;
                mc_clamp_scroll();
                GUIWidget *w = &mc.dialog.children[mc.idx_thumb];
                w->rect.y = track.y + rel + (w->rect.y - thumb.y);
            }
        }
    }
    if (down && !mc.prev_mouse && !mc.dragging && mx >= 0) {
        SDL_Rect lr = mc_list_rect();
        SDL_Rect track = { 0, 0, 0, 0 }, thumb = { 0, 0, 0, 0 };
        if (mc.idx_track >= 0) mc_draw_rect(mc.idx_track, &track);
        if (mc.idx_thumb >= 0) mc_draw_rect(mc.idx_thumb, &thumb);
        if (mc.idx_list >= 0 && SDL_PointInRect(&pt, &lr)) {
            int row = mc.scroll + (my - lr.y) / mc_row_height();
            if (row >= 0 && row < mc.count) Multiplayer_MapChooserSelect(row);
        } else if (mc.idx_track >= 0 && SDL_PointInRect(&pt, &track)) {
            int vis = mc_rows_visible();
            if (my < thumb.y) mc.scroll -= vis;
            else if (my >= thumb.y + thumb.h) mc.scroll += vis;
            mc_clamp_scroll();
        }
    }
    mc.prev_mouse = down;
}

void Multiplayer_MapChooserPointer(int x, int y, int down) {
    if (!mc.open) return;
    mc_pointer(x, y, down);
    mc_sync_thumb();
}

void Multiplayer_MapChooserSelect(int row) {
    if (row < 0 || row >= mc.count) return;
    mc.selected = row;
    if (row < mc.scroll) mc.scroll = row;
    if (row >= mc.scroll + mc_rows_visible()) mc.scroll = row - mc_rows_visible() + 1;
    mc_clamp_scroll();
    mc_sync_thumb();
    mc_load_selected();
}

int         Multiplayer_MapChooserOpen(void)      { return mc.open; }
int         Multiplayer_MapChooserScroll(void)    { return mc.scroll; }
int         Multiplayer_MapChooserRowsVisible(void) { return mc_rows_visible(); }
int Multiplayer_MapChooserThumbRect(SDL_Rect *out) {
    if (!mc.open || mc.idx_thumb < 0 || !out) return 0;
    mc_draw_rect(mc.idx_thumb, out);
    return 1;
}
int Multiplayer_MapChooserTrackRect(SDL_Rect *out) {
    if (!mc.open || mc.idx_track < 0 || !out) return 0;
    mc_draw_rect(mc.idx_track, out);
    return 1;
}
int Multiplayer_MapChooserWidgetHidden(const char *name) {
    return (mc.open && mc.rt) ? GUIRuntime_WidgetHidden(mc.rt, name) : 0;
}
int         Multiplayer_MapChooserRowCount(void)  { return mc.count; }
const char *Multiplayer_MapChooserRowKey(int row) {
    return (row >= 0 && row < mc.count) ? mc.rows[row].key : NULL;
}

void Multiplayer_CloseMapChooser(void) {
    if (mc.have_tnt) TNT_Close(&mc.tnt);
    if (mc.rt) GUIRuntime_Destroy(mc.rt);
    if (mc.has_dialog) GUIDialog_Free(&mc.dialog);
    tak_free(mc.rows);
    memset(&mc, 0, sizeof mc);
    mc.selected = -1;
    mc.idx_list = -1;
}

void Multiplayer_OpenMapChooser(int as_host) {
    Multiplayer_CloseMapChooser();
    const char *file = as_host ? "data/guis/choosemap.gui" : "data/guis/viewmap.gui";
    if (GUIDialog_Load(&mc.dialog, file) != 0) {
        fprintf(stderr, "Multiplayer: failed to load %s\n", file);
        return;
    }
    mc.has_dialog = 1;
    mc.rt = GUIRuntime_Create(&mc.dialog);
    if (!mc.rt) { GUIDialog_Free(&mc.dialog); mc.has_dialog = 0; return; }
    Translate_Dialog(&mp_tt, &mc.dialog);
    mc.host = as_host;
    mc.selected = -1;
    mc.idx_list = -1;
    mc.idx_track = mc.idx_thumb = mc.idx_inc = mc.idx_dec = -1;
    mc.dragging = 0;
    mc.prev_mouse = 0;
    for (int i = 0; i < mc.dialog.num_children; i++) {
        if (tak_stricmp(mc.dialog.children[i].name, "MapList") == 0) mc.idx_list = i;
    }
    if (mc.idx_list >= 0) {
        SDL_Rect lr = mc.dialog.children[mc.idx_list].rect;
        for (int i = 0; i < mc.dialog.num_children; i++) {
            const GUIWidget *w = &mc.dialog.children[i];
            int beside = w->rect.x >= lr.x + lr.w - 40 && w->rect.x < lr.x + lr.w + 40 &&
                         w->rect.y >= lr.y - 4 && w->rect.y < lr.y + lr.h + 4;
            if (!beside) continue;
            if (tak_stricmp(w->name, "slider") == 0)    mc.idx_track = i;
            if (tak_stricmp(w->name, "sbutton") == 0)   mc.idx_thumb = i;
            if (tak_stricmp(w->name, "incbutton") == 0) mc.idx_inc = i;
            if (tak_stricmp(w->name, "decbutton") == 0) mc.idx_dec = i;
        }
    }
    /* The row template and its label are art the list draws over. */
    GUIRuntime_SetWidgetVisible(mc.rt, "MapNameEntryTemplate", 0);
    GUIRuntime_SetWidgetVisible(mc.rt, "MapName", 0);
    GUIRuntime_SetWidgetVisible(mc.rt, "LineTemplate", 0);
    GUIRuntime_SetWidgetVisible(mc.rt, "TextLine", 0);

    TAK_MapEntry *found = NULL;
    int n = 0;
    if (TAK_Maps_Scan(&found, &n) == 0 && n > 0) {
        mc.rows = (MpMapRow *)tak_malloc(sizeof(MpMapRow) * (size_t)n);
        if (mc.rows) {
            for (int i = 0; i < n; i++) {
                snprintf(mc.rows[i].key, sizeof mc.rows[i].key, "%s", found[i].key);
                Translate_MapName(&mp_tt, found[i].key, mc.rows[i].display,
                                  sizeof mc.rows[i].display);
            }
            mc.count = n;
            qsort(mc.rows, (size_t)n, sizeof(MpMapRow), mc_row_cmp);
        }
    }
    TAK_Maps_Free(found);

    /* Open on the room's current map, which is what a viewer came to
     * see and what the host is most likely changing from. */
    const TAK_MsgRoomState *rs = mp_room_state();
    int start = 0;
    if (rs && rs->map_name[0]) {
        for (int i = 0; i < mc.count; i++) {
            if (tak_stricmp(mc.rows[i].key, rs->map_name) == 0) { start = i; break; }
        }
    }
    mc.open = 1;
    if (mc.count > 0) Multiplayer_MapChooserSelect(start);
}

/* OK on the host's chooser is the whole point: the map and the
 * fingerprint this install computed for it, in one edit. Cancel, and
 * OK on a viewer's, only close. */
void Multiplayer_MapChooserPress(const char *name) {
    if (!mc.open || !name) return;
    if (tak_stricmp(name, "OK") == 0) {
        if (mc.host && mc.selected >= 0 && mc.selected < mc.count) {
            TAK_NetClient *c = NetSession_Client();
            if (c) {
                TAK_MsgRoomEdit e;
                memset(&e, 0, sizeof e);
                e.field = TAK_EDIT_MAP;
                e.seat = TAK_NET_SEAT_NONE;
                snprintf(e.text, sizeof e.text, "%s", mc.rows[mc.selected].key);
                (void)TAK_MapFingerprint_FromName(mc.rows[mc.selected].key, e.fingerprint);
                (void)TAK_NetClient_EditRoom(c, &e);
            }
        }
        Multiplayer_CloseMapChooser();
    } else if (tak_stricmp(name, "Cancel") == 0) {
        Multiplayer_CloseMapChooser();
    }
}

static void mc_draw_rows(SDL_Surface *off) {
    if (mc.idx_list < 0 || !mp_font) return;
    SDL_Rect lr = mc_list_rect();
    int h = mc_row_height();
    int vis = mc_rows_visible();
    for (int i = 0; i < vis; i++) {
        int row = mc.scroll + i;
        if (row >= mc.count) break;
        SDL_Rect r = { lr.x, lr.y + i * h, lr.w, h };
        if (row == mc.selected) {
            SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 90, 70, 40, 255));
        }
        Font_DrawString(mp_font, off, r.x + 10, r.y + 2, mc.rows[row].display);
    }
}

static void mc_draw_preview(SDL_Surface *off) {
    const GUIWidget *v = GUIDialog_FindByName(&mc.dialog, "MapView");
    if (!v) return;
    SDL_Rect p = v->rect;
    SDL_FillRect(off, &p, SDL_MapRGBA(off->format, 0, 0, 0, 255));
    if (!mc.have_tnt || !mc.tnt.minimap_rgba) return;
    int mw = mc.tnt.minimap_w, mh = mc.tnt.minimap_h;
    int map_w = mc.tnt.width_tiles  > 0 ? mc.tnt.width_tiles  : mw;
    int map_h = mc.tnt.height_tiles > 0 ? mc.tnt.height_tiles : mh;
    int cw, ch;
    if (map_w >= map_h) { cw = mw; ch = (mh * map_h + map_w / 2) / map_w; }
    else                { cw = (mw * map_w + map_h / 2) / map_h; ch = mh; }
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if (cw > mw) cw = mw; if (ch > mh) ch = mh;
    int fw, fh;
    if (cw * p.h >= ch * p.w) { fw = p.w; fh = (ch * p.w + cw / 2) / cw; }
    else                      { fh = p.h; fw = (cw * p.h + ch / 2) / ch; }
    if (fw < 1) fw = 1; if (fh < 1) fh = 1;
    int ox = (p.w - fw) / 2, oy = (p.h - fh) / 2;
    for (int y = 0; y < fh; y++) {
        int sy = y * ch / fh;
        for (int x = 0; x < fw; x++) {
            int sx = x * cw / fw;
            SDL_Rect px = { p.x + ox + x, p.y + oy + y, 1, 1 };
            SDL_FillRect(off, &px, mc.tnt.minimap_rgba[sy * mw + sx]);
        }
    }
}

/* The description, wrapped to the panel by words. */
static void mc_draw_desc(SDL_Surface *off) {
    const GUIWidget *box = GUIDialog_FindByName(&mc.dialog, "MapInfo");
    if (!box || !mp_font || !mc.desc[0]) return;
    int lh = Font_LineHeight(mp_font);
    if (lh <= 0) lh = 12;
    /* About six characters per pixel column of text is a fair width
     * for this font. Words that do not fit go to the next line. */
    int max_chars = box->rect.w / 6;
    if (max_chars < 16) max_chars = 16;
    int y = box->rect.y + 2;
    const char *p = mc.desc;
    char line[160];
    while (*p && y + lh <= box->rect.y + box->rect.h) {
        int n = 0;
        int last_space = -1;
        while (p[n] && n < max_chars && n < (int)sizeof line - 1) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        memcpy(line, p, (size_t)n);
        line[n] = '\0';
        Font_DrawString(mp_font, off, box->rect.x + 8, y, line);
        y += lh;
        p += n;
        while (*p == ' ') p++;
    }
}

int Multiplayer_MapChooserTick(TAK_Platform *platform, float dt) {
    (void)dt;
    if (!mc.open || !mc.rt) return GAMESTATE_MULTIPLAYER;

    int mx = -1, my = -1, mouse_down = 0;
    if (platform && platform->has_focus) mouse_down = TAK_Platform_MouseThisFrame(platform, &mx, &my);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (platform && platform->has_focus &&
        (keys[SDL_SCANCODE_ESCAPE] || platform->pressed_escape)) {
        Multiplayer_CloseMapChooser();
        return GAMESTATE_MULTIPLAYER;
    }

    mc_pointer(mx, my, mouse_down);

    /* The wheel over the list or its bar scrolls it. */
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
        SDL_Rect lr = mc_list_rect(), track = { 0, 0, 0, 0 };
        if (mc.idx_track >= 0) mc_draw_rect(mc.idx_track, &track);
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &lr) || SDL_PointInRect(&pt, &track)) {
            mc.scroll -= ev.wheel.y;
            mc_clamp_scroll();
        }
    }

    char clicked[64];
    int  clicked_idx = -1;
    clicked[0] = '\0';
    (void)GUIRuntime_UpdateEx(mc.rt, mx, my, mouse_down, clicked, sizeof clicked,
                              &clicked_idx);
    if (clicked[0] && !mc.dragging) {
        if (clicked_idx == mc.idx_inc)        { mc.scroll--; mc_clamp_scroll(); }
        else if (clicked_idx == mc.idx_dec)   { mc.scroll++; mc_clamp_scroll(); }
        else if (clicked_idx == mc.idx_thumb) { /* the drag has it */ }
        else Multiplayer_MapChooserPress(clicked);
        if (!mc.open) return GAMESTATE_MULTIPLAYER;
    }
    mc_sync_thumb();

    /* The room stays under it, as the original draws the chooser over
     * the battle menu. */
    GUIRuntime_Render(mp.rt);
    GUIRuntime_Render(mc.rt);
    SDL_Surface *off = UI_Offscreen();
    if (off) {
        mc_draw_rows(off);
        mc_draw_preview(off);
        mc_draw_desc(off);
    }
    UI_Present(platform);
    return GAMESTATE_MULTIPLAYER;
}
