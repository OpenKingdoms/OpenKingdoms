/*
 * web_stats.c -- what the browser page counts about play (tak_web_stats.h).
 *
 * Each event goes to the page as one small JSON object. The page's
 * counting script decides whether it goes any further.
 */

#include "tak_web_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, web_stats_post, (const char *kind, const char *json), {
    try {
        if (typeof Module !== 'undefined' && Module['okStats'])
            Module['okStats'](UTF8ToString(kind), JSON.parse(UTF8ToString(json)));
    } catch (e) {}
});

static WebStats_Sink g_sink = web_stats_post;
#else
/* A desktop build has no page to tell. */
static WebStats_Sink g_sink;
#endif

#define TEXT_MAX 120

static struct {
    char mode[16], sub[16];
    char campaign[64], mission[64];
    char began[16];          /* the battle under way, "" when none */
    int  watching;           /* ... seen from no seat */
} g;

void WebStats_SetSink(WebStats_Sink sink) { g_sink = sink; }

/* ── One event as JSON ────────────────────────────────────────────────── */

typedef struct Js { char p[768]; size_t n; } Js;

static void js_raw(Js *j, const char *s) {
    size_t k = strlen(s);
    if (j->n + k >= sizeof j->p) return;
    memcpy(j->p + j->n, s, k + 1);
    j->n += k;
}

static void js_key(Js *j, const char *key) {
    js_raw(j, j->n > 1 ? ",\"" : "\"");
    js_raw(j, key);
    js_raw(j, "\":");
}

/* A string, escaped, and cut at TEXT_MAX bytes. */
static void js_text(Js *j, const char *key, const char *v) {
    js_key(j, key);
    js_raw(j, "\"");
    size_t k = 0;
    for (; v && *v && k < TEXT_MAX; v++, k++) {
        unsigned char c = (unsigned char)*v;
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; e[2] = 0; }
        else if (c < 0x20 || c >= 0x7f) snprintf(e, sizeof e, "\\u%04x", c);
        else { e[0] = (char)c; e[1] = 0; }
        js_raw(j, e);
    }
    js_raw(j, "\"");
}

static void js_num(Js *j, const char *key, int v) {
    char e[16];
    snprintf(e, sizeof e, "%d", v);
    js_key(j, key);
    js_raw(j, e);
}

static void post(const char *kind, Js *j) {
    js_raw(j, "}");
    if (g_sink) g_sink(kind, j->p);
}

static void copy(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

/* ── The events ───────────────────────────────────────────────────────── */

void WebStats_Starting(const char *mode, const char *sub) {
    copy(g.mode, sizeof g.mode, mode);
    copy(g.sub, sizeof g.sub, sub);
    g.campaign[0] = g.mission[0] = '\0';
}

void WebStats_Mission(const char *campaign, const char *mission) {
    copy(g.campaign, sizeof g.campaign, campaign);
    copy(g.mission, sizeof g.mission, mission);
}

static const char *side_name(int side) {
    switch (side) {
    case TAK_SIDE_ARAMON: return "aramon";
    case TAK_SIDE_TAROS:  return "taros";
    case TAK_SIDE_VERUNA: return "veruna";
    case TAK_SIDE_ZHON:   return "zhon";
    case TAK_SIDE_CREON:  return "creon";
    default:              return "other";
    }
}

static const char *level_name(int level) {
    static const char *const k_level[] = { "easy", "normal", "hard", "brutal" };
    return level >= 0 && level < 4 ? k_level[level] : "other";
}

void WebStats_BattleBegan(const BattleConfig *cfg, const char *map, int local_player,
                          int network, int replaying, const char *mod) {
    /* Both ways into a match name it, and a session in a match is one
     * whatever was named. */
    int match = network || strcmp(g.mode, "multiplayer") == 0;
    const char *mode = replaying ? "replay" : match ? "multiplayer" : g.mode[0] ? g.mode : "skirmish";
    const char *sub = "";
    if (strcmp(mode, "multiplayer") == 0)
        sub = strcmp(g.mode, "multiplayer") == 0 && g.sub[0] ? g.sub : "join";
    else if (strcmp(mode, "load") == 0)
        sub = g.sub;
    int campaign = strcmp(mode, "campaign") == 0;

    Js j = { "{", 1 };
    js_text(&j, "mode", mode);
    if (sub[0]) js_text(&j, "sub", sub);
    if (campaign) {
        js_text(&j, "camp", g.campaign[0] ? g.campaign : "other");
        js_text(&j, "mission", g.mission[0] ? g.mission : map);
    } else {
        js_text(&j, "map", map);
    }
    if (cfg && !campaign) {
        int ai = 0, humans = 0, level = -1, mixed = 0;
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            const PlayerSlot *p = &cfg->players[i];
            if (p->kind == TAK_SLOT_HUMAN) humans++;
            if (p->kind != TAK_SLOT_AI) continue;
            if (ai++ && p->ai_difficulty != level) mixed = 1;
            level = p->ai_difficulty;
        }
        js_num(&j, "ai", ai);
        if (ai) js_text(&j, "lvl", mixed ? "mixed" : level_name(level));
        if (strcmp(mode, "multiplayer") == 0) js_num(&j, "humans", humans);
        js_num(&j, "rb", cfg->remastered ? 1 : 0);
    }
    js_text(&j, "mod", mod && mod[0] ? mod : "vanilla");
    int watching = strcmp(sub, "watch") == 0;
    const char *side = "none";
    if (!watching && cfg && local_player >= 1 && local_player <= TAK_MAX_PLAYERS)
        side = side_name(cfg->players[local_player - 1].side);
    js_text(&j, "side", side);
    post("start", &j);
    copy(g.began, sizeof g.began, mode);
    g.watching = watching || replaying;
}

void WebStats_BattleEnded(const WebStatsResult *r, const char *how) {
    if (!g.began[0]) return;
    const char *out = how;
    if (!out) {
        if (r && r->victory) out = "win";
        else if (r && r->defeat) out = "loss";
        else if (r && r->over) out = g.watching ? "over" : r->local > 0 ? "win" : r->local < 0 ? "loss" : "draw";
        else out = "quit";
    }
    Js j = { "{", 1 };
    js_text(&j, "mode", g.began);
    js_text(&j, "out", out);
    js_num(&j, "secs", r && r->ticks > 0 ? r->ticks / 60 : 0);
    post("end", &j);
    g.began[0] = '\0';
}

void WebStats_Error(const char *what) {
    Js j = { "{", 1 };
    js_text(&j, "msg", what);
    post("error", &j);
}
