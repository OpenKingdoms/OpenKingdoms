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

#include "tak_battle_config.h"
#include "tak_console_cmd.h"
#include "tak_font.h"
#include "tak_gaf.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_sides.h"
#include "tak_ui.h"
#include "tak_settings.h"
#include "tak_world.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct ChatEntry {
    char     text[CHAT_TEXT_MAX];
    uint32_t ms;        /* when it was stored, off the frame clock */
    uint8_t  owner;     /* a player slot, or CHAT_OWNER_SYSTEM */
    uint8_t  type;      /* CHAT_TYPE_MINE, _THEIRS or _NOTICE   */
    uint16_t name_len;  /* the leading "<name>:" drawn in the strong ink */
} ChatEntry;

/* The block never paints more rows than this. Twenty messages is the
 * option's ceiling and a wrapped one costs more than a row, so the cap
 * is generous rather than exact. */
#define CHAT_MAX_ROWS  64
#define CHAT_ROW_CHARS 160
/* Enough for one encoded chat frame: a scope, three seats, a turn, a
 * name and the line. */
#define CHAT_FRAME_CAP 512

static void badge_load_sheet(void);

static struct {
    int        initialized;
    Font      *font;
    Font      *bold;        /* the names, when the game has the sheet */

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

    /* The wrapped rows the last row walk produced, each with the entry
     * it came from, whether it opens that entry, and how many of its
     * characters belong to the sender's name. */
    char        rows[CHAT_MAX_ROWS][CHAT_ROW_CHARS];
    int         row_entry[CHAT_MAX_ROWS];
    uint8_t     row_first[CHAT_MAX_ROWS];
    uint8_t     row_name[CHAT_MAX_ROWS];
    int         row_count;
    uint32_t    now_ms;     /* the last expiry pass, for the fade */
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
    /* Names take the game's bold cut of the same face. */
    if (!ch.bold) ch.bold = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    /* The badge sheet is read now, so a running battle reads no files. */
    badge_load_sheet();
    return ch.font ? 0 : -1;
}

static void face_forget(void);

void Chat_Shutdown(void) {
    face_forget();
    if (ch.font) Font_Free(ch.font);
    if (ch.bold) Font_Free(ch.bold);
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

static int chat_push_named(const char *text, int type, int owner_slot,
                           uint32_t now_ms, int name_len) {
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
    e->name_len = (uint16_t)(name_len > 0 && name_len < (int)strlen(e->text) ? name_len : 0);
    ch.head = (ch.head + 1) % CHAT_RING_SLOTS;
    return 1;
}

int Chat_Push(const char *text, int type, int owner_slot, uint32_t now_ms) {
    return chat_push_named(text, type, owner_slot, now_ms, 0);
}

void Chat_Expire(uint32_t now_ms) {
    ch.now_ms = now_ms;
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

    /* A line is a command when its first non space character is '+'.
     * The original runs it and then sends the line on like any other,
     * so everyone sees "Player: +NOWISEE" whether it took or not
     * (legacy:154470-154500). A note for this player alone follows
     * when it could not take. */
    char note[80];
    note[0] = '\0';
    if (*s == '+') {
        ConsoleRun ran = ConsoleCmd_Run(s + 1);
        if (ran == CONSOLE_RAN_REFUSED) {
            snprintf(note, sizeof(note), "Power codes are off in this game.");
        } else if (ran == CONSOLE_RAN_NOT_IN_YET) {
            ConsoleCmdLine l;
            ConsoleCmd_Parse(s + 1, &l);
            snprintf(note, sizeof(note), "+%s is not in yet.", ConsoleCmd_Name(l.id));
        }
    }

    char formatted[CHAT_TEXT_MAX];
    snprintf(formatted, sizeof(formatted), "%s: %s", ch.local_name, s);

    /* One path to the network, the protocol's own chat message. With no
     * session registered the line still shows up locally, which is what
     * the original does in single player: its send loop finds no other
     * active player and nothing leaves the machine. */
    TAK_MsgChat m;
    memset(&m, 0, sizeof(m));
    /* The relay stamps the seat as it passes the line on. Ours is the
     * wire's count, from zero. */
    m.scope     = TAK_CHAT_ALL;
    m.from_seat = (ch.local_slot >= 1 && ch.local_slot <= TAK_NET_SEATS)
                ? (uint8_t)(ch.local_slot - 1) : TAK_NET_SEAT_NONE;
    m.to_seat   = TAK_NET_SEAT_NONE;
    m.turn      = 0;
    snprintf(m.name, sizeof(m.name), "%s", ch.local_name);
    snprintf(m.text, sizeof(m.text), "%s", s);

    ch.last_frame_len = TAK_Msg_ChatEncode(&m, ch.last_frame,
                                           sizeof(ch.last_frame));
    int unsent = 0;
    if (ch.send && ch.last_frame_len > 0)
        unsent = ch.send(ch.last_frame, ch.last_frame_len, ch.send_user) != 0;

    /* Your own line is stored with type 4 and your own slot. Only a
     * received line is type 8, which is why ChatLevel off hides your own
     * and nobody else's (legacy:206003-206004). */
    chat_push_named(formatted, CHAT_TYPE_MINE, ch.local_slot, now_ms,
                    (int)strlen(ch.local_name) + 1);
    if (note[0]) Chat_Push(note, CHAT_TYPE_NOTICE, CHAT_OWNER_SYSTEM, now_ms);
    if (unsent)
        Chat_Push("That line did not reach the other players.", CHAT_TYPE_NOTICE,
                  CHAT_OWNER_SYSTEM, now_ms);
    chat_close();
    return 1;
}

void Chat_OnMessage(const TAK_MsgChat *m, uint32_t now_ms) {
    if (!m) return;
    char formatted[CHAT_TEXT_MAX];
    /* The wire counts seats from zero, the ring players from one. A
     * watcher's line has no seat and reads as a system line. */
    int owner = (m->from_seat < TAK_NET_SEATS) ? (int)m->from_seat + 1
                                                : CHAT_OWNER_SYSTEM;
    int name_len = 0;
    if (m->name[0]) {
        snprintf(formatted, sizeof(formatted), "%s: %s", m->name, m->text);
        name_len = (int)strnlen(m->name, sizeof(m->name)) + 1;
    } else {
        snprintf(formatted, sizeof(formatted), "%s", m->text);
    }
    chat_push_named(formatted, CHAT_TYPE_THEIRS, owner, now_ms, name_len);
}

/* ── Ink ──────────────────────────────────────────────────────────── */

/* Floors of brightness, as relative luminance: the name keeps the
 * colour strong, the message is a paler tint of it, always a step
 * above the name. */
#define CHAT_NAME_LUM  0.34f
#define CHAT_TEXT_LUM  0.64f
#define CHAT_TEXT_STEP 0.12f
/* The plate: a warm near black, about three quarters opaque. */
static const ChatInk k_plate = { 14, 11, 8 };
#define CHAT_PLATE_A   188
/* The halo round each glyph. */
static const ChatInk k_halo = { 6, 5, 4 };
#define CHAT_HALO_A    235

static float srgb_to_linear(uint8_t c) {
    float x = (float)c / 255.0f;
    return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

static uint8_t linear_to_srgb(float x) {
    if (x <= 0.0f) return 0;
    if (x >= 1.0f) return 255;
    float s = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
    int v = (int)(s * 255.0f + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static float ink_luminance(ChatInk c) {
    return 0.2126f * srgb_to_linear(c.r) + 0.7152f * srgb_to_linear(c.g) +
           0.0722f * srgb_to_linear(c.b);
}

/* Mixing toward white in linear light raises luminance by the same
 * share on every channel, so the hue stays put. */
static ChatInk ink_lift(ChatInk c, float floor_lum) {
    float l = ink_luminance(c);
    if (l >= floor_lum) return c;
    float t = (floor_lum + 0.004f - l) / (1.0f - l);
    if (t > 1.0f) t = 1.0f;
    ChatInk o;
    o.r = linear_to_srgb(srgb_to_linear(c.r) * (1.0f - t) + t);
    o.g = linear_to_srgb(srgb_to_linear(c.g) * (1.0f - t) + t);
    o.b = linear_to_srgb(srgb_to_linear(c.b) * (1.0f - t) + t);
    return o;
}

void Chat_PlayerInk(int color_index, ChatInk *name, ChatInk *text) {
    const TakPlayerColor *pc = BattleConfig_PlayerColor(color_index);
    ChatInk base = { pc->r, pc->g, pc->b };
    ChatInk strong = ink_lift(base, CHAT_NAME_LUM);
    float pale = ink_luminance(strong) + CHAT_TEXT_STEP;
    if (pale > 0.92f) pale = 0.92f;
    if (pale < CHAT_TEXT_LUM) pale = CHAT_TEXT_LUM;
    if (name) *name = strong;
    if (text) *text = ink_lift(base, pale);
}

ChatInk Chat_SystemInk(void) {
    ChatInk c = { 238, 218, 170 };
    return c;
}

ChatInk Chat_PlateOver(ChatInk ground) {
    /* The canvas goes over the battle with plain alpha blending. */
    ChatInk o;
    o.r = (uint8_t)((k_plate.r * CHAT_PLATE_A + ground.r * (255 - CHAT_PLATE_A) + 127) / 255);
    o.g = (uint8_t)((k_plate.g * CHAT_PLATE_A + ground.g * (255 - CHAT_PLATE_A) + 127) / 255);
    o.b = (uint8_t)((k_plate.b * CHAT_PLATE_A + ground.b * (255 - CHAT_PLATE_A) + 127) / 255);
    return o;
}

int Chat_ContrastX100(ChatInk a, ChatInk b) {
    float la = ink_luminance(a) + 0.05f, lb = ink_luminance(b) + 0.05f;
    float r = la > lb ? la / lb : lb / la;
    return (int)(r * 100.0f);
}

int Chat_ScaleQuarters(int canvas_h) {
    /* One to one and a quarter on the 480 row canvas, twice the
     * original on a 1080 row screen, at most four times. */
    int q = (canvas_h * 4 + 270) / 540;
    if (q < 5) q = 5;
    if (q > 16) q = 16;
    return q;
}

/* ── The face ───────────────────────────────────────────────────────
 * The game's own face, read back one glyph at a time, upscaled four
 * times by edge directed doubling and resampled to the chat's scale,
 * so the strokes stay crisp and the curves come out smooth. */

#define FACE_FIRST 33
#define FACE_LAST  126
#define FACE_N     (FACE_LAST - FACE_FIRST + 1)

typedef struct ChatGlyph {
    uint8_t *ink;    /* coverage, w * h */
    uint8_t *halo;   /* the dark rim round it, same box */
    int      w, h;
    int      x, y;   /* box corner from the pen and the line top */
} ChatGlyph;

typedef struct ChatBadge {
    uint32_t *px;
    int       w, h;
    uint8_t   tried;
} ChatBadge;

typedef struct ChatFace {
    Font        *measured;    /* the sheet adv and kern were read from */
    int          adv[256];    /* pen advance at one to one */
    signed char  kern[52][52];
    ChatGlyph    g[FACE_N];
} ChatFace;

/* The message face and the name face. */
#define FACE_TEXT 0
#define FACE_NAME 1

static struct {
    int          q;           /* quarters the glyphs and badges are built at */
    ChatFace     face[2];
    GAFFile     *badge_gaf;
    uint32_t     badge_rgba[256];
    int          badge_loaded;
    ChatBadge    badge[TAK_SIDES_MAX][TAK_PLAYER_COLOR_COUNT];
} cf;

static int letter_slot(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    return -1;
}

/* The sheet a face reads: the name face is the bold sheet when the game
 * has one. */
static Font *face_font(int which) {
    return (which == FACE_NAME && ch.bold) ? ch.bold : ch.font;
}

/* Advances and letter pairs as Font_MeasureString counts them, or the
 * six pixel estimate with no font, so a row walks in one pass. */
static void face_measure_one(int which) {
    ChatFace *fc = &cf.face[which];
    Font *font = face_font(which);
    if (fc->measured == font && fc->adv[' ']) return;
    fc->measured = font;
    memset(fc->kern, 0, sizeof fc->kern);
    char s[3] = { 0, 0, 0 };
    for (int c = 1; c < 256; c++) {
        s[0] = (char)c;
        s[1] = 0;
        fc->adv[c] = font ? Font_MeasureString(font, s) : 6;
    }
    if (!font) return;
    for (int a = 0; a < 52; a++) {
        for (int b = 0; b < 52; b++) {
            s[0] = (char)(a < 26 ? 'A' + a : 'a' + a - 26);
            s[1] = (char)(b < 26 ? 'A' + b : 'a' + b - 26);
            int k = Font_MeasureString(font, s) - fc->adv[(unsigned char)s[0]] -
                    fc->adv[(unsigned char)s[1]];
            fc->kern[a][b] = (signed char)(k < -100 ? -100 : (k > 100 ? 100 : k));
        }
    }
}

static void face_measure(void) {
    face_measure_one(FACE_TEXT);
    face_measure_one(FACE_NAME);
}

/* The pen step past character i of s, in the name face while i < split. */
static int face_step(const char *s, int len, int i, int split) {
    const ChatFace *fc = &cf.face[i < split ? FACE_NAME : FACE_TEXT];
    unsigned char c = (unsigned char)s[i];
    unsigned char next = i + 1 < len ? (unsigned char)s[i + 1] : 0;
    int a = letter_slot(c), b = letter_slot(next);
    return fc->adv[c] + ((a >= 0 && b >= 0) ? fc->kern[a][b] : 0);
}

/* Width of s[0..len) at one to one. */
static int face_width1(const char *s, int len, int split) {
    int w = 0;
    for (int i = 0; i < len; i++) w += face_step(s, len, i, split);
    return w;
}

static uint32_t *px_at(SDL_Surface *s, int x, int y) {
    return (uint32_t *)((uint8_t *)s->pixels + y * s->pitch) + x;
}

static int floor_div(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* Scale2x: each pixel becomes four, and a corner takes its neighbours'
 * value where two of them agree across it. */
static void epx2(const uint32_t *s, int w, int h, uint32_t *d) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = s[y * w + x];
            uint32_t a = y > 0 ? s[(y - 1) * w + x] : 0;
            uint32_t b = x < w - 1 ? s[y * w + x + 1] : 0;
            uint32_t c = x > 0 ? s[y * w + x - 1] : 0;
            uint32_t e = y < h - 1 ? s[(y + 1) * w + x] : 0;
            uint32_t e0 = p, e1 = p, e2 = p, e3 = p;
            if (c == a && c != e && a != b) e0 = a;
            if (a == b && a != c && b != e) e1 = b;
            if (e == c && e != b && c != a) e2 = c;
            if (b == e && b != a && e != c) e3 = e;
            uint32_t *o = d + (2 * y) * (2 * w) + 2 * x;
            o[0] = e0;
            o[1] = e1;
            o[2 * w] = e2;
            o[2 * w + 1] = e3;
        }
    }
}

/* A w by h picture whose corner sits at (bx, by) at one to one, scaled
 * to q quarters: four times by Scale2x twice, then sixteen samples a
 * pixel. Colours average weighted by alpha. Returns the picture and
 * its box, or NULL. */
static uint32_t *upscale(const uint32_t *src, int w, int h, int bx, int by, int q,
                         const SDL_PixelFormat *f, int *ow, int *oh, int *ox, int *oy) {
    uint32_t *x2 = (uint32_t *)tak_malloc((size_t)w * h * 4 * sizeof(uint32_t));
    uint32_t *x4 = (uint32_t *)tak_malloc((size_t)w * h * 16 * sizeof(uint32_t));
    if (!x2 || !x4) { tak_free(x2); tak_free(x4); return NULL; }
    epx2(src, w, h, x2);
    epx2(x2, w * 2, h * 2, x4);
    tak_free(x2);
    int w4 = w * 4, h4 = h * 4;
    int x0 = floor_div(bx * q, 4), y0 = floor_div(by * q, 4);
    int x1 = floor_div((bx + w) * q + 3, 4), y1 = floor_div((by + h) * q + 3, 4);
    int W = x1 - x0, H = y1 - y0;
    uint32_t *out = (W > 0 && H > 0) ? (uint32_t *)tak_malloc((size_t)W * H * sizeof(uint32_t)) : NULL;
    if (!out) { tak_free(x4); return NULL; }
    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < 4; j++) {
                int v = floor_div(16 * (Y + y0) + 4 * j + 2, q) - 4 * by;
                if (v < 0 || v >= h4) continue;
                for (int i = 0; i < 4; i++) {
                    int u = floor_div(16 * (X + x0) + 4 * i + 2, q) - 4 * bx;
                    if (u < 0 || u >= w4) continue;
                    uint32_t p = x4[v * w4 + u];
                    uint32_t a = (p & f->Amask) >> f->Ashift;
                    sa += a;
                    sr += a * ((p & f->Rmask) >> f->Rshift);
                    sg += a * ((p & f->Gmask) >> f->Gshift);
                    sb += a * ((p & f->Bmask) >> f->Bshift);
                }
            }
            uint32_t o = 0;
            if (sa) {
                o = ((sr / sa) << f->Rshift) | ((sg / sa) << f->Gshift) |
                    ((sb / sa) << f->Bshift) | ((sa / 16) << f->Ashift);
            }
            out[Y * W + X] = o;
        }
    }
    tak_free(x4);
    *ow = W; *oh = H; *ox = x0; *oy = y0;
    return out;
}

static void face_free_glyphs(void) {
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < FACE_N; i++) {
            tak_free(cf.face[f].g[i].ink);
            tak_free(cf.face[f].g[i].halo);
        }
        memset(cf.face[f].g, 0, sizeof cf.face[f].g);
    }
}

static void face_free_badges(void) {
    for (int s = 0; s < TAK_SIDES_MAX; s++)
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) tak_free(cf.badge[s][c].px);
    memset(cf.badge, 0, sizeof cf.badge);
}

/* The halo's radius in screen pixels, a little over half the stroke. */
static float halo_radius(int q) { return 0.55f + 0.30f * (float)q / 4.0f; }

/* A pixel of the face's own colour: lit, and not its baked shadow. */
static int glyph_lit(uint32_t p, const SDL_PixelFormat *f) {
    int lum = (int)((p & f->Rmask) >> f->Rshift) + (int)((p & f->Gmask) >> f->Gshift) +
              (int)((p & f->Bmask) >> f->Bshift);
    return ((p & f->Amask) >> f->Ashift) && lum >= 3 * 128;
}

/* One glyph: drawn alone at one to one, its light pixels kept and its
 * baked shadow dropped, then scaled and ringed. */
static void face_build_glyph(int which, int c, int q) {
    ChatGlyph *g = &cf.face[which].g[c - FACE_FIRST];
    Font *font = face_font(which);
    int pad = Font_LineHeight(font) + 2;
    int sw = cf.face[which].adv[c] + 2 * pad, sh = 3 * pad;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    SDL_FillRect(s, NULL, 0);
    char str[2] = { (char)c, 0 };
    Font_DrawString(font, s, pad, pad, str);
    const SDL_PixelFormat *f = s->format;
    int bx0 = sw, by0 = sh, bx1 = -1, by1 = -1;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            if (glyph_lit(*px_at(s, x, y), f)) {
                if (x < bx0) bx0 = x;
                if (y < by0) by0 = y;
                if (x > bx1) bx1 = x;
                if (y > by1) by1 = y;
            }
    /* A clear pixel all round, so the doubling sees the glyph's edges. */
    int w1 = bx1 - bx0 + 3, h1 = by1 - by0 + 3;
    uint32_t *m = bx1 < 0 ? NULL : (uint32_t *)tak_malloc((size_t)w1 * h1 * sizeof(uint32_t));
    uint32_t *up = NULL;
    int W = 0, H = 0, X0 = 0, Y0 = 0;
    if (m) {
        memset(m, 0, (size_t)w1 * h1 * sizeof(uint32_t));
        for (int y = by0; y <= by1; y++)
            for (int x = bx0; x <= bx1; x++)
                if (glyph_lit(*px_at(s, x, y), f))
                    m[(y - by0 + 1) * w1 + (x - bx0 + 1)] = f->Amask | f->Rmask | f->Gmask | f->Bmask;
        up = upscale(m, w1, h1, bx0 - 1 - pad, by0 - 1 - pad, q, f, &W, &H, &X0, &Y0);
        tak_free(m);
    }
    SDL_FreeSurface(s);
    if (!up) return;

    float r = halo_radius(q);
    int rp = (int)ceilf(r) + 1, span = 2 * rp + 1;
    int gw = W + 2 * rp, gh = H + 2 * rp;
    int wgt[64];
    g->ink = (uint8_t *)tak_malloc((size_t)gw * gh);
    g->halo = (uint8_t *)tak_malloc((size_t)gw * gh);
    if (!g->ink || !g->halo || span * span > 64) {
        tak_free(up); tak_free(g->ink); tak_free(g->halo);
        memset(g, 0, sizeof *g);
        return;
    }
    memset(g->ink, 0, (size_t)gw * gh);
    memset(g->halo, 0, (size_t)gw * gh);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            g->ink[(y + rp) * gw + x + rp] = (uint8_t)((up[y * W + x] & f->Amask) >> f->Ashift);
    tak_free(up);
    /* The halo: the ink spread by r, its edge softened over a pixel. */
    for (int dy = -rp; dy <= rp; dy++)
        for (int dx = -rp; dx <= rp; dx++) {
            float k = r + 0.5f - sqrtf((float)(dx * dx + dy * dy));
            wgt[(dy + rp) * span + dx + rp] = k <= 0.0f ? 0 : (k >= 1.0f ? 256 : (int)(k * 256.0f));
        }
    for (int y = rp; y < rp + H; y++)
        for (int x = rp; x < rp + W; x++) {
            int a = g->ink[y * gw + x];
            if (!a) continue;
            for (int dy = -rp; dy <= rp; dy++)
                for (int dx = -rp; dx <= rp; dx++) {
                    int v = a * wgt[(dy + rp) * span + dx + rp] >> 8;
                    uint8_t *h = &g->halo[(y + dy) * gw + x + dx];
                    if (v > *h) *h = (uint8_t)v;
                }
        }
    g->w = gw;
    g->h = gh;
    g->x = X0 - rp;
    g->y = Y0 - rp;
}

static void face_forget(void) {
    face_free_glyphs();
    face_free_badges();
    if (cf.badge_gaf) GAF_Close(cf.badge_gaf);
    memset(&cf, 0, sizeof cf);
}

static void face_ensure(int q) {
    face_measure();
    if (cf.q == q) return;
    face_free_glyphs();
    face_free_badges();
    cf.q = q;
    if (!ch.font) return;
    for (int f = 0; f < 2; f++)
        for (int c = FACE_FIRST; c <= FACE_LAST; c++) face_build_glyph(f, c, q);
}

/* ── Pixels ─────────────────────────────────────────────────────────── */

/* Lay a colour at alpha a (0 to 255) over one pixel of a 32 bit
 * surface, straight alpha on both sides. */
static void px_over(uint32_t *p, const SDL_PixelFormat *f, ChatInk c, int a) {
    if (a <= 0) return;
    if (a > 255) a = 255;
    uint32_t v = *p;
    int da = f->Amask ? (int)((v & f->Amask) >> f->Ashift) : 255;
    int r = c.r, g = c.g, b = c.b, oa = a;
    if (a < 255 && da > 0) {
        int k = da * (255 - a) / 255;
        oa = a + k;
        r = (r * a + (int)((v & f->Rmask) >> f->Rshift) * k) / oa;
        g = (g * a + (int)((v & f->Gmask) >> f->Gshift) * k) / oa;
        b = (b * a + (int)((v & f->Bmask) >> f->Bshift) * k) / oa;
    }
    *p = ((uint32_t)r << f->Rshift) | ((uint32_t)g << f->Gshift) | ((uint32_t)b << f->Bshift) |
         (f->Amask ? ((uint32_t)oa << f->Ashift) : 0);
}

/* A rounded plate, its corners antialiased, at alpha a. */
static void draw_plate(SDL_Surface *s, int x, int y, int w, int h, int rad, ChatInk c, int a) {
    const SDL_Rect *cl = &s->clip_rect;
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < cl->y || yy >= cl->y + cl->h) continue;
        int ry = yy < y + rad ? y + rad - yy : (yy >= y + h - rad ? yy - (y + h - rad - 1) : 0);
        for (int xx = x; xx < x + w; xx++) {
            if (xx < cl->x || xx >= cl->x + cl->w) continue;
            int rx = xx < x + rad ? x + rad - xx : (xx >= x + w - rad ? xx - (x + w - rad - 1) : 0);
            int cov = 255;
            if (rx && ry) {
                float d = sqrtf((float)((rx - 0.5f) * (rx - 0.5f) + (ry - 0.5f) * (ry - 0.5f)));
                float k = (float)rad - d + 0.5f;
                if (k <= 0.0f) continue;
                if (k < 1.0f) cov = (int)(k * 255.0f);
            }
            px_over(px_at(s, xx, yy), s->format, c, a * cov / 255);
        }
    }
}

/* One run of text at (x, y), the line top. Characters before split are
 * the name, in the name face and ink a, the rest ink b. alpha fades the
 * whole run. Returns its width. */
static int face_draw(SDL_Surface *s, int x, int y, const char *t, int len, int split,
                     ChatInk a, ChatInk b, int alpha) {
    const SDL_Rect *cl = &s->clip_rect;
    int q = cf.q, pen = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)t[i];
        int px = x + (pen * q + 2) / 4;
        pen += face_step(t, len, i, split);
        if (c < FACE_FIRST || c > FACE_LAST) continue;
        const ChatGlyph *g = &cf.face[i < split ? FACE_NAME : FACE_TEXT].g[c - FACE_FIRST];
        if (!g->ink) continue;
        ChatInk ink = i < split ? a : b;
        int gx = px + g->x, gy = y + g->y;
        for (int yy = 0; yy < g->h; yy++) {
            int sy = gy + yy;
            if (sy < cl->y || sy >= cl->y + cl->h) continue;
            for (int xx = 0; xx < g->w; xx++) {
                int sx = gx + xx;
                if (sx < cl->x || sx >= cl->x + cl->w) continue;
                int hv = g->halo[yy * g->w + xx], iv = g->ink[yy * g->w + xx];
                if (!hv && !iv) continue;
                uint32_t *p = px_at(s, sx, sy);
                if (hv) px_over(p, s->format, k_halo, hv * CHAT_HALO_A / 255 * alpha / 255);
                if (iv) px_over(p, s->format, ink, iv * alpha / 255);
            }
        }
    }
    return (pen * q + 2) / 4;
}

/* ── Badges ──────────────────────────────────────────────────────────
 * The sender's team badge, their side's emblem in their colour, from
 * the side's logo sheet (legacy:206008-206015). */

static void badge_load_sheet(void) {
    if (cf.badge_loaded || !UI_RGBAFormat()) return;
    cf.badge_loaded = 1;
    char stem[32] = "colorlogos2";
    for (int i = 0; i < Sides_Count() && i < TAK_SIDES_MAX; i++) {
        const TakSideInfo *si = Sides_Get(i);
        if (si && si->logogaf[0]) snprintf(stem, sizeof stem, "%s", si->logogaf);
    }
    char gaf[96], pcx[96];
    snprintf(gaf, sizeof gaf, "anims/%s.gaf", stem);
    snprintf(pcx, sizeof pcx, "anims/%s.pcx", stem);
    if (VFS_FileExists(gaf) != 0) {
        snprintf(gaf, sizeof gaf, "data/anims/%s.gaf", stem);
        snprintf(pcx, sizeof pcx, "data/anims/%s.pcx", stem);
    }
    if (UI_LoadGAFWithPalette(gaf, pcx, &cf.badge_gaf, cf.badge_rgba) != 0) cf.badge_gaf = NULL;
}

static const ChatBadge *badge_for(int side, int colour) {
    if (side < 0 || side >= TAK_SIDES_MAX) return NULL;
    if (colour < 0) colour = 0;
    colour %= TAK_PLAYER_COLOR_COUNT;
    ChatBadge *b = &cf.badge[side][colour];
    if (b->tried) return b->px ? b : NULL;
    b->tried = 1;
    const TakSideInfo *si = Sides_Get(side);
    if (!cf.badge_gaf || !si) return NULL;
    /* By name, else the side's own entry, as the setup screens find it. */
    int off = si->logoart[0] ? GAF_FindSequence(cf.badge_gaf, si->logoart) : -1;
    if (off < 0 && side < (int)cf.badge_gaf->num_entries &&
        12u + (uint32_t)(side + 1) * 4u <= cf.badge_gaf->data_size)
        off = (int)*(const uint32_t *)(cf.badge_gaf->data + 12 + side * 4);
    if (off < 0 || (uint32_t)off + sizeof(EntryHeader) > cf.badge_gaf->data_size) return NULL;
    const EntryHeader *eh = (const EntryHeader *)(cf.badge_gaf->data + off);
    /* Twelve frame sheets lead with two greyed states (legacy:136390). */
    int frame = colour + ((int)eh->num_frames >= TAK_PLAYER_COLOR_COUNT + 2 ? 2 : 0);
    if (frame >= (int)eh->num_frames) return NULL;
    int w = 0, h = 0;
    uint32_t *raw = UI_DecodeFrame(cf.badge_gaf, off, frame, cf.badge_rgba, &w, &h);
    if (!raw) return NULL;
    int ox, oy;
    b->px = upscale(raw, w, h, 0, 0, cf.q, UI_RGBAFormat(), &b->w, &b->h, &ox, &oy);
    tak_free(raw);
    return b->px ? b : NULL;
}

static void draw_badge(SDL_Surface *s, int x, int y, const ChatBadge *b, int alpha) {
    const SDL_Rect *cl = &s->clip_rect;
    const SDL_PixelFormat *bf = UI_RGBAFormat();
    for (int yy = 0; yy < b->h; yy++) {
        int sy = y + yy;
        if (sy < cl->y || sy >= cl->y + cl->h) continue;
        for (int xx = 0; xx < b->w; xx++) {
            int sx = x + xx;
            if (sx < cl->x || sx >= cl->x + cl->w) continue;
            uint32_t p = b->px[yy * b->w + xx];
            int a = (int)((p & bf->Amask) >> bf->Ashift);
            if (!a) continue;
            ChatInk c = { (uint8_t)((p & bf->Rmask) >> bf->Rshift),
                          (uint8_t)((p & bf->Gmask) >> bf->Gshift),
                          (uint8_t)((p & bf->Bmask) >> bf->Bshift) };
            px_over(px_at(s, sx, sy), s->format, c, a * alpha / 255);
        }
    }
}

/* ── The block ────────────────────────────────────────────────────── */

/* Break one message into rows no wider than wrap1, counted at one to
 * one, on a space where there is one and mid word where there is not.
 * Rows land in ch.rows from *row onward and *row advances past them. */
static void chat_wrap(const char *text, int name_len, int entry, int wrap1, int *row) {
    int n = (int)strlen(text);
    int start = 0;
    if (wrap1 < 16) wrap1 = 16;
    while (start < n && *row < CHAT_MAX_ROWS) {
        int fit = 0;              /* characters that fit on this row */
        int last_space = -1;
        int pen = 0;              /* width of the fit so far, kerned to the next */
        while (start + fit < n && fit + 1 < CHAT_ROW_CHARS) {
            int i = start + fit;
            unsigned char c = (unsigned char)text[i];
            if (pen + cf.face[i < name_len ? FACE_NAME : FACE_TEXT].adv[c] > wrap1) break;
            if (c == ' ') last_space = fit;
            pen += face_step(text, n, i, name_len);
            fit++;
        }
        if (fit == 0) fit = 1;                  /* always make progress */
        int take = fit;
        if (start + fit < n && last_space > 0) take = last_space;
        memcpy(ch.rows[*row], text + start, (size_t)take);
        ch.rows[*row][take] = '\0';
        ch.row_entry[*row] = entry;
        ch.row_first[*row] = (uint8_t)(start == 0);
        int named = name_len - start;
        ch.row_name[*row] = (uint8_t)(named < 0 ? 0 : (named > take ? take : named));
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

/* The seat an entry speaks for, or 0 for a system line. */
static int entry_seat(const ChatEntry *e) {
    return (e->owner >= 1 && e->owner <= TAK_MAX_PLAYERS) ? e->owner : 0;
}

/* The seat an entry is drawn for and its colour and side, from the
 * battle's own setup, which every machine shares. */
static int entry_style(const ChatEntry *e, int *colour, int *side) {
    int seat = entry_seat(e);
    const GameWorld *w = World_Get();
    if (colour) *colour = (seat && w) ? w->cfg.players[seat - 1].color : 0;
    if (side) *side = (seat && w) ? w->cfg.players[seat - 1].side : -1;
    return seat;
}

int Chat_EntryStyle(int index, int *colour, int *side) {
    const ChatEntry *e = ring_at(index);
    if (!e) {
        if (colour) *colour = 0;
        if (side) *side = -1;
        return 0;
    }
    return entry_style(e, colour, side);
}

/* Where an entry's text starts at one to one: past the badge for a
 * player, at the margin for a system line. */
static int entry_text_x(const ChatEntry *e) {
    return entry_seat(e) ? CHAT_TEXT_X : CHAT_LIST_X;
}

static void chat_build_rows(int canvas_w, int q) {
    ch.row_count = 0;
    face_measure();
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
    for (int k = n_picked - 1; k >= 0; k--) {
        const ChatEntry *e = ring_at(picked[k]);
        if (!e) continue;
        /* The original wraps at the screen less the text's own x and the
         * sidebar. Ours also stops a line at a readable length. */
        int wrap_px = canvas_w - entry_text_x(e) * q / 4 - CHAT_RIGHT_INSET;
        if (wrap_px > 560 * q / 4) wrap_px = 560 * q / 4;
        chat_wrap(e->text, e->name_len, picked[k], wrap_px * 4 / q, &ch.row_count);
    }
}

int Chat_VisibleRows(void) {
    chat_build_rows(640, Chat_ScaleQuarters(480));
    return ch.row_count;
}

const char *Chat_VisibleRow(int row) {
    if (row < 0 || row >= ch.row_count) return "";
    return ch.rows[row];
}

/* The last half second of a line's life fades it out. Ours. */
static int entry_alpha(const ChatEntry *e) {
    int32_t life = (int32_t)(Chat_TextScrollTime() + 1) * 1000;
    int32_t left = life - (int32_t)(ch.now_ms - e->ms);
    if (left >= 500) return 255;
    if (left <= 0) return 0;
    return (int)(left * 255 / 500);
}

static int face_ready(SDL_Surface *off) {
    return off && ch.font && off->format->BytesPerPixel == 4;
}

/* The plain one to one draw, for a surface the scaled path cannot take. */
static void chat_draw_plain(SDL_Surface *off) {
    chat_build_rows(off->w, 4);
    for (int r = 0; r < ch.row_count; r++)
        Font_DrawString(ch.font, off, CHAT_LIST_X,
                        r * CHAT_LINE_PITCH + CHAT_LIST_TOP, ch.rows[r]);
}

void Chat_Draw(SDL_Surface *off) {
    if (!off || !ch.font) return;
    if (!face_ready(off)) { chat_draw_plain(off); return; }
    int q = Chat_ScaleQuarters(off->h);
    chat_build_rows(off->w, q);
    if (ch.row_count <= 0) return;
    face_ensure(q);
    /* Rows that fit above the edit box. Past that the oldest messages
     * give way, so the block never runs under the bottom strip. */
    int room = ((off->h - 8 - CHAT_INPUT_H * q / 4) * 4 / q - CHAT_BADGE_TOP + 1) / CHAT_LINE_PITCH;
    int r = 0;
    while (ch.row_count - r > room) {
        r++;
        while (r < ch.row_count && !ch.row_first[r]) r++;
    }
    int r0 = r;
    if (SDL_MUSTLOCK(off)) SDL_LockSurface(off);
    while (r < ch.row_count) {
        /* One message: its rows share a plate, and its first carries
         * the badge. */
        const ChatEntry *e = ring_at(ch.row_entry[r]);
        int end = r + 1;
        while (end < ch.row_count && !ch.row_first[end]) end++;
        if (!e) { r = end; continue; }
        int alpha = entry_alpha(e);
        if (alpha <= 0) { r = end; continue; }

        ChatInk name_ink = Chat_SystemInk(), text_ink = Chat_SystemInk();
        const ChatBadge *badge = NULL;
        int colour, side;
        if (entry_style(e, &colour, &side)) {
            Chat_PlayerInk(colour, &name_ink, &text_ink);
            if (side >= 0) badge = badge_for(side, colour);
        }
        /* A line with no name, a fall or an order, reads in the strong ink. */
        if (!e->name_len) text_ink = name_ink;

        int tx = entry_text_x(e) * q / 4;
        int widest = 0;
        for (int k = r; k < end; k++) {
            int wpx = face_width1(ch.rows[k], (int)strlen(ch.rows[k]), ch.row_name[k]) * q / 4;
            if (wpx > widest) widest = wpx;
        }
        int top = (CHAT_BADGE_TOP - 1 + ((r - r0) * CHAT_LINE_PITCH)) * q / 4;
        int bottom = (CHAT_BADGE_TOP - 1 + ((end - r0) * CHAT_LINE_PITCH) - 1) * q / 4;
        int px0 = (CHAT_LIST_X - 4) * q / 4;
        draw_plate(off, px0, top, tx + widest + 5 * q / 4 - px0, bottom - top,
                   3 * q / 4, k_plate, CHAT_PLATE_A * alpha / 255);
        if (badge)
            draw_badge(off, CHAT_LIST_X * q / 4,
                       (CHAT_BADGE_TOP + (r - r0) * CHAT_LINE_PITCH) * q / 4, badge, alpha);
        for (int k = r; k < end; k++)
            face_draw(off, tx, (CHAT_LIST_TOP + (k - r0) * CHAT_LINE_PITCH) * q / 4, ch.rows[k],
                      (int)strlen(ch.rows[k]), ch.row_name[k], name_ink, text_ink, alpha);
        r = end;
    }
    if (SDL_MUSTLOCK(off)) SDL_UnlockSurface(off);
}

void Chat_DrawClock(SDL_Surface *off, const char *text) {
    if (!off || !ch.font || !text) return;
    if (!face_ready(off)) {
        /* Right aligned clear of the sidebar, 10 px down (legacy:210353-210361). */
        int x = off->w - Font_MeasureString(ch.font, text) - CHAT_RIGHT_INSET;
        Font_DrawString(ch.font, off, x, 10, text);
        return;
    }
    int q = Chat_ScaleQuarters(off->h);
    face_ensure(q);
    int len = (int)strlen(text);
    int w = face_width1(text, len, 0) * q / 4;
    int x = off->w - w - CHAT_RIGHT_INSET;
    if (SDL_MUSTLOCK(off)) SDL_LockSurface(off);
    draw_plate(off, x - 4 * q / 4, 10 - 3 * q / 4, w + 8 * q / 4, 17 * q / 4, 3 * q / 4,
               k_plate, CHAT_PLATE_A);
    face_draw(off, x, 10, text, len, 0, Chat_SystemInk(), Chat_SystemInk(), 255);
    if (SDL_MUSTLOCK(off)) SDL_UnlockSurface(off);
}

void Chat_DrawInput(SDL_Surface *off) {
    if (!off || !ch.open) return;
    int q = face_ready(off) ? Chat_ScaleQuarters(off->h) : 4;

    /* The console's own layout puts its one widget on the bottom of the
     * screen, whatever the authored y says (legacy:154381-154391). It
     * grows with the text, and never past the sidebar. */
    int pw = CHAT_INPUT_W * q / 4;
    if (pw > off->w - CHAT_INPUT_X - CHAT_RIGHT_INSET) pw = off->w - CHAT_INPUT_X - CHAT_RIGHT_INSET;
    if (pw < CHAT_INPUT_W) pw = CHAT_INPUT_W;
    int ph = CHAT_INPUT_H * q / 4;
    int y = off->h - 8 - ph;
    SDL_Rect panel = { CHAT_INPUT_X, y, pw, ph };

    if (!face_ready(off)) {
        SDL_FillRect(off, &panel, SDL_MapRGBA(off->format, 0, 0, 0, 200));
        if (!ch.font) return;
        int line_h = Font_LineHeight(ch.font);
        if (line_h <= 0) line_h = 16;
        int text_y = y + (CHAT_INPUT_H - line_h) / 2;
        Font_DrawString(ch.font, off, CHAT_INPUT_X + CHAT_INPUT_INSET, text_y, ch.line);
        return;
    }
    face_ensure(q);
    if (SDL_MUSTLOCK(off)) SDL_LockSurface(off);
    ChatInk edge = { 150, 124, 72 };
    int bw = q >= 8 ? 2 : 1;
    draw_plate(off, panel.x, panel.y, panel.w, panel.h, 4 * q / 4, edge, 230);
    draw_plate(off, panel.x + bw, panel.y + bw, panel.w - 2 * bw, panel.h - 2 * bw,
               4 * q / 4 - bw, k_plate, 240);

    /* The line scrolls so the caret stays in the box. */
    int text_x = CHAT_INPUT_X + CHAT_INPUT_INSET * q / 4;
    int room = panel.x + panel.w - CHAT_INPUT_INSET * q / 4 - text_x;
    int caret_px = face_width1(ch.line, ch.caret, 0) * q / 4;
    int shift = caret_px > room ? caret_px - room : 0;
    int text_y = y + (ph - 13 * q / 4) / 2;
    SDL_Rect keep = off->clip_rect;
    SDL_Rect inner = { panel.x + bw, panel.y + bw, panel.w - 2 * bw, panel.h - 2 * bw };
    SDL_SetClipRect(off, &inner);
    ChatInk white = { 244, 240, 232 };
    face_draw(off, text_x - shift, text_y, ch.line, ch.len, 0, white, white, 255);
    SDL_SetClipRect(off, &keep);

    int cw = q >= 8 ? 2 : 1;
    SDL_Rect caret = { text_x - shift + caret_px, text_y + q / 4, cw, 11 * q / 4 };
    if (SDL_MUSTLOCK(off)) SDL_UnlockSurface(off);
    SDL_FillRect(off, &caret, SDL_MapRGBA(off->format, 230, 210, 160, 255));
}
