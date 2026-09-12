/*
 * chat.c -- the in battle chat console.
 *
 * Enter opens the line, Enter sends it, Escape throws it away. What is
 * sent and what arrives lands in a thirty slot ring that draws down the
 * top left corner of the screen. The shape and the numbers are in
 * tak_chat.h.
 *
 * Everything here is local view state. The console holds no simulation
 * state, writes none, and is never hashed.
 */

#include "tak_chat.h"
#include "tak_font.h"
#include "tak_ui.h"
#include "tak_settings.h"
#include <stdio.h>
#include <string.h>

typedef struct ChatEntry {
    char     text[CHAT_TEXT_MAX];
    uint32_t ms;        /* when it was stored, off the frame clock */
    uint8_t  owner;     /* a player slot, or CHAT_OWNER_SYSTEM */
    uint8_t  type;      /* CHAT_TYPE_MINE, _THEIRS or _NOTICE   */
} ChatEntry;

/* The block never paints more rows than this. Twenty messages is the
 * option's ceiling and a wrapped one costs more than a row, so the cap
 * is generous rather than exact. */
#define CHAT_MAX_ROWS  64
#define CHAT_ROW_CHARS 160
/* Enough for one encoded chat frame: a scope, three seats, a turn, a
 * name and the line. */
#define CHAT_FRAME_CAP 512

static struct {
    int        initialized;
    Font      *font;

    ChatEntry  ring[CHAT_RING_SLOTS];
    int        head;        /* next free slot */
    int        tail;        /* oldest stored slot */

    int        open;
    char       line[CHAT_INPUT_MAX + 1];
    int        len;
    int        caret;

    int        local_slot;
    char       local_name[TAK_NET_NAME_MAX];

    Chat_SendFn send;
    void       *send_user;
    uint8_t     last_frame[CHAT_FRAME_CAP];
    size_t      last_frame_len;

    /* The wrapped rows the last row walk produced. */
    char        rows[CHAT_MAX_ROWS][CHAT_ROW_CHARS];
    int         row_count;
} ch;

/* ── Options ──────────────────────────────────────────────────────── */

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int Chat_ChatLevel(void) {
    return Settings_GetInt("ChatLevel", 1) ? 1 : 0;
}

int Chat_TextScrollTime(void) {
    return clamp_int(Settings_GetInt("TextScrollTime", 5), 0, 20);
}

int Chat_MaxTextLines(void) {
    return clamp_int(Settings_GetInt("MaxTextLines", 8), 0, 20);
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

int Chat_Init(void) {
    if (!ch.initialized) {
        memset(&ch, 0, sizeof(ch));
        ch.local_slot = 1;
        snprintf(ch.local_name, sizeof(ch.local_name), "%s", "Player");
        ch.initialized = 1;
    }
    if (!ch.font) {
        /* The in game face. console.gui names the bare "times new
         * roman (100)" for its edit box, which ships no .pcx and so
         * carries no palette to decode a glyph with. The ig_ variant is
         * the same face with one. */
        ch.font = Font_Load("data/fonts/ig_times new roman (100)",
                            UI_RGBAFormat());
    }
    return ch.font ? 0 : -1;
}

void Chat_Shutdown(void) {
    if (ch.font) Font_Free(ch.font);
    memset(&ch, 0, sizeof(ch));
}

void Chat_Reset(void) {
    ch.head = ch.tail = 0;
    ch.open = 0;
    ch.line[0] = '\0';
    ch.len = ch.caret = 0;
    ch.row_count = 0;
    ch.last_frame_len = 0;
    memset(ch.ring, 0, sizeof(ch.ring));
}

void Chat_SetLocalPlayer(int slot, const char *name) {
    ch.local_slot = slot;
    if (name && name[0]) {
        snprintf(ch.local_name, sizeof(ch.local_name), "%s", name);
    }
}

int Chat_LocalSlot(void) { return ch.local_slot; }

void Chat_SetSender(Chat_SendFn fn, void *user) {
    ch.send = fn;
    ch.send_user = user;
}

/* ── The ring ─────────────────────────────────────────────────────── */

static int ring_count(void) {
    return (ch.head - ch.tail + CHAT_RING_SLOTS) % CHAT_RING_SLOTS;
}

int Chat_Count(void) { return ring_count(); }

static ChatEntry *ring_at(int index) {
    if (index < 0 || index >= ring_count()) return NULL;
    return &ch.ring[(ch.tail + index) % CHAT_RING_SLOTS];
}

const char *Chat_EntryText(int index) {
    const ChatEntry *e = ring_at(index);
    return e ? e->text : "";
}

int Chat_EntryType(int index) {
    const ChatEntry *e = ring_at(index);
    return e ? e->type : 0;
}

int Chat_EntryOwner(int index) {
    const ChatEntry *e = ring_at(index);
    return e ? e->owner : 0;
}

int Chat_Push(const char *text, int type, int owner_slot, uint32_t now_ms) {
    if (!text || !text[0]) return 0;
    /* Text Lines at zero stores nothing at all, which is also why it is
     * not the default (legacy:205792). */
    if (Chat_MaxTextLines() <= 0) return 0;

    /* A full ring drops its oldest entry to make room. */
    if (ring_count() >= CHAT_RING_SLOTS - 1) {
        ch.tail = (ch.tail + 1) % CHAT_RING_SLOTS;
    }
    ChatEntry *e = &ch.ring[ch.head];
    memset(e, 0, sizeof(*e));
    snprintf(e->text, sizeof(e->text), "%s", text);
    e->ms    = now_ms;
    e->owner = (uint8_t)owner_slot;
    e->type  = (uint8_t)type;
    ch.head = (ch.head + 1) % CHAT_RING_SLOTS;
    return 1;
}

void Chat_Expire(uint32_t now_ms) {
    if (ring_count() <= 0) return;
    const ChatEntry *oldest = &ch.ring[ch.tail];
    int32_t life_ms = (int32_t)(Chat_TextScrollTime() + 1) * 1000;
    /* A signed difference, so the frame clock rolling over past 2^32
     * does not make every stored message immortal. */
    if ((int32_t)(now_ms - oldest->ms) > life_ms) {
        ch.tail = (ch.tail + 1) % CHAT_RING_SLOTS;
    }
}

/* ── The input line ───────────────────────────────────────────────── */

int Chat_IsOpen(void) { return ch.open; }

static void chat_text_input(int on) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    if (on) SDL_StartTextInput();
    else    SDL_StopTextInput();
}

void Chat_Open(void) {
    ch.line[0] = '\0';
    ch.len = ch.caret = 0;
    ch.open = 1;
    chat_text_input(1);
}

static void chat_close(void) {
    ch.open = 0;
    ch.line[0] = '\0';
    ch.len = ch.caret = 0;
    chat_text_input(0);
}

void Chat_Cancel(void) { chat_close(); }

void Chat_TypeText(const char *text) {
    if (!ch.open || !text) return;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* The font carries glyphs 32 to 126 and draws a plain gap for
         * anything else, so nothing wider gets into the line. */
        if (c < 0x20 || c > 0x7e) continue;
        if (ch.len >= CHAT_INPUT_MAX) break;
        memmove(ch.line + ch.caret + 1, ch.line + ch.caret,
                (size_t)(ch.len - ch.caret) + 1);
        ch.line[ch.caret] = (char)c;
        ch.len++;
        ch.caret++;
    }
    ch.line[ch.len] = '\0';
}

void Chat_Backspace(void) {
    if (!ch.open || ch.caret <= 0) return;
    memmove(ch.line + ch.caret - 1, ch.line + ch.caret,
            (size_t)(ch.len - ch.caret) + 1);
    ch.caret--;
    ch.len--;
}

void Chat_Delete(void) {
    if (!ch.open || ch.caret >= ch.len) return;
    memmove(ch.line + ch.caret, ch.line + ch.caret + 1,
            (size_t)(ch.len - ch.caret));
    ch.len--;
}

void Chat_CaretLeft(void)  { if (ch.open && ch.caret > 0) ch.caret--; }
void Chat_CaretRight(void) { if (ch.open && ch.caret < ch.len) ch.caret++; }
void Chat_CaretHome(void)  { if (ch.open) ch.caret = 0; }
void Chat_CaretEnd(void)   { if (ch.open) ch.caret = ch.len; }

const char *Chat_Line(void) { return ch.line; }
int Chat_Caret(void) { return ch.caret; }

const void *Chat_LastSentFrame(size_t *out_len) {
    if (out_len) *out_len = ch.last_frame_len;
    return ch.last_frame;
}

int Chat_Submit(uint32_t now_ms) {
    if (!ch.open) return 0;

    const char *s = ch.line;
    while (*s == ' ' || *s == '\t') s++;
    /* An empty or whitespace only line just closes. */
    if (!*s) {
        chat_close();
        return 0;
    }

    /* A line is a command only when its first non space character is
     * '+' (legacy:154470). The interpreter behind those also backs the
     * in game key bindings, so it is not a chat feature and it is not
     * in yet. Say so rather than broadcasting the line. */
    if (*s == '+') {
        Chat_Push("Console commands are not in yet.",
                  CHAT_TYPE_NOTICE, CHAT_OWNER_SYSTEM, now_ms);
        chat_close();
        return 0;
    }

    char formatted[CHAT_TEXT_MAX];
    snprintf(formatted, sizeof(formatted), "%s: %s", ch.local_name, s);

    /* One path to the network, the protocol's own chat message. With no
     * session registered the line still shows up locally, which is what
     * the original does in single player: its send loop finds no other
     * active player and nothing leaves the machine. */
    TAK_MsgChat m;
    memset(&m, 0, sizeof(m));
    m.scope     = TAK_CHAT_ALL;
    m.from_seat = (uint8_t)ch.local_slot;
    m.to_seat   = TAK_NET_SEAT_NONE;
    m.turn      = 0;
    snprintf(m.name, sizeof(m.name), "%s", ch.local_name);
    snprintf(m.text, sizeof(m.text), "%s", s);

    ch.last_frame_len = TAK_Msg_ChatEncode(&m, ch.last_frame,
                                           sizeof(ch.last_frame));
    if (ch.send && ch.last_frame_len > 0) {
        ch.send(ch.last_frame, ch.last_frame_len, ch.send_user);
    }

    /* Your own line is stored with type 4 and your own slot. Only a
     * received line is type 8, which is why ChatLevel off hides your own
     * and nobody else's (legacy:206003-206004). */
    Chat_Push(formatted, CHAT_TYPE_MINE, ch.local_slot, now_ms);
    chat_close();
    return 1;
}

void Chat_OnMessage(const TAK_MsgChat *m, uint32_t now_ms) {
    if (!m) return;
    char formatted[CHAT_TEXT_MAX];
    int owner = (m->from_seat == TAK_NET_SEAT_NONE) ? CHAT_OWNER_SYSTEM
                                                    : (int)m->from_seat;
    if (m->name[0]) {
        snprintf(formatted, sizeof(formatted), "%s: %s", m->name, m->text);
    } else {
        snprintf(formatted, sizeof(formatted), "%s", m->text);
    }
    Chat_Push(formatted, CHAT_TYPE_THEIRS, owner, now_ms);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

/* A run of text measured in pixels. Without a font loaded, an estimate
 * off the character count, so the row walk still answers in a run with
 * no game data behind it. */
static int chat_measure(const char *s, int len) {
    char buf[CHAT_ROW_CHARS];
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    if (ch.font) return Font_MeasureString(ch.font, buf);
    return len * 6;
}

/* Break one message into rows no wider than wrap_px, on a space where
 * there is one and mid word where there is not. Rows land in ch.rows
 * from *row onward and *row advances past them. */
static void chat_wrap(const char *text, int wrap_px, int *row) {
    int n = (int)strlen(text);
    int start = 0;
    if (wrap_px < 16) wrap_px = 16;
    while (start < n && *row < CHAT_MAX_ROWS) {
        int fit = 0;              /* characters that fit on this row */
        int last_space = -1;
        while (start + fit < n) {
            int next = fit + 1;
            if (next >= CHAT_ROW_CHARS) break;
            if (chat_measure(text + start, next) > wrap_px) break;
            if (text[start + fit] == ' ') last_space = fit;
            fit = next;
        }
        if (fit == 0) fit = 1;                  /* always make progress */
        int take = fit;
        if (start + fit < n && last_space > 0) take = last_space;
        memcpy(ch.rows[*row], text + start, (size_t)take);
        ch.rows[*row][take] = '\0';
        (*row)++;
        start += take;
        while (start < n && text[start] == ' ') start++;
    }
}

/* An entry the block paints. The original passes one only when
 * ChatLevel is on or the type is 8 (legacy:206003-206004). A local
 * notice is ours, not the original's, and is never hidden by an option
 * about other players' chat. */
static int chat_entry_visible(const ChatEntry *e) {
    if (e->type == CHAT_TYPE_THEIRS) return 1;
    if (e->type == CHAT_TYPE_NOTICE) return 1;
    return Chat_ChatLevel();
}

static void chat_build_rows(int canvas_w) {
    ch.row_count = 0;
    int max = Chat_MaxTextLines();
    if (max <= 0) return;

    /* Walk back from the newest, taking at most MaxTextLines messages
     * that pass the filter, then lay them out oldest first. */
    int picked[20];
    int n_picked = 0;
    int count = ring_count();
    for (int i = count - 1; i >= 0 && n_picked < max; i--) {
        const ChatEntry *e = ring_at(i);
        if (!e || !chat_entry_visible(e)) continue;
        picked[n_picked++] = i;
    }
    int wrap_px = canvas_w - CHAT_LIST_X - CHAT_RIGHT_INSET;
    for (int k = n_picked - 1; k >= 0; k--) {
        const ChatEntry *e = ring_at(picked[k]);
        if (!e) continue;
        chat_wrap(e->text, wrap_px, &ch.row_count);
    }
}

int Chat_VisibleRows(void) {
    chat_build_rows(640);
    return ch.row_count;
}

const char *Chat_VisibleRow(int row) {
    if (row < 0 || row >= ch.row_count) return "";
    return ch.rows[row];
}

void Chat_Draw(SDL_Surface *off) {
    if (!off || !ch.font) return;
    chat_build_rows(off->w);
    for (int r = 0; r < ch.row_count; r++) {
        Font_DrawString(ch.font, off, CHAT_LIST_X,
                        r * CHAT_LINE_PITCH + CHAT_LIST_TOP, ch.rows[r]);
    }
}

void Chat_DrawInput(SDL_Surface *off) {
    if (!off || !ch.open) return;

    /* The console's own layout puts its one widget on the bottom of the
     * screen, whatever the authored y says (legacy:154381-154391). */
    int y = off->h - 8 - CHAT_INPUT_H;
    SDL_Rect panel = { CHAT_INPUT_X, y, CHAT_INPUT_W, CHAT_INPUT_H };
    SDL_FillRect(off, &panel, SDL_MapRGBA(off->format, 0, 0, 0, 200));

    Uint32 edge_rgba = SDL_MapRGBA(off->format, 128, 128, 128, 255);
    SDL_Rect edge = panel;
    edge.h = 1;
    SDL_FillRect(off, &edge, edge_rgba);
    edge.y = panel.y + panel.h - 1;
    SDL_FillRect(off, &edge, edge_rgba);
    edge = panel;
    edge.w = 1;
    SDL_FillRect(off, &edge, edge_rgba);
    edge.x = panel.x + panel.w - 1;
    SDL_FillRect(off, &edge, edge_rgba);

    if (!ch.font) return;
    int text_x = CHAT_INPUT_X + CHAT_INPUT_INSET;
    int line_h = Font_LineHeight(ch.font);
    if (line_h <= 0) line_h = 16;
    int text_y = y + (CHAT_INPUT_H - line_h) / 2;
    if (text_y < y) text_y = y;
    Font_DrawString(ch.font, off, text_x, text_y, ch.line);

    SDL_Rect caret = { text_x + chat_measure(ch.line, ch.caret),
                       text_y + 2, 1, line_h - 2 };
    if (caret.h < 4) caret.h = 4;
    SDL_FillRect(off, &caret, SDL_MapRGBA(off->format, 192, 192, 192, 255));
}
