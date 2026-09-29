/*
 * map_browser.c -- the lobby's map search, filters and order, and the
 * start positions on a map's picture. See tak_map_browser.h.
 */

#include "tak_map_browser.h"
#include "tak_font.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_platform.h"
#include "tak_tdf.h"
#include "tak_translate.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

/* ── A map's .ota ───────────────────────────────────────────────────── */

static int parse_players(const char *text) {
    int best = 0;
    for (const char *p = text ? text : ""; *p; ) {
        while (*p && (*p < '0' || *p > '9')) p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (v > best) best = v;
    }
    return best;
}

int TAK_MapSummary_Read(const char *key, TAK_MapSummary *out) {
    memset(out, 0, sizeof(*out));
    char path[512];
    if (!key || TAK_Maps_FindFile(key, "ota", path, sizeof(path)) != 0) return -1;
    TDFFile *tdf = TDF_Open(path);
    if (!tdf || TDF_Load(tdf) != 0) { if (tdf) TDF_Close(tdf); return -1; }
    int number[TAK_MAP_STARTS_MAX];
    if (TDF_PushSection(tdf, "GlobalHeader") == 0) {
        int x = 0, y = 0;
        if (sscanf(TDF_ReadString(tdf, "size", ""), " %d %*1[xX] %d", &x, &y) == 2 &&
            x > 0 && y > 0) {
            out->size_x = x;
            out->size_y = y;
        }
        out->max_players = parse_players(TDF_ReadString(tdf, "numplayers", ""));
        /* The same walk the loader makes over the specials. */
        if (TDF_PushSection(tdf, "Map Data") == 0 &&
            TDF_PushSection(tdf, "specials") == 0) {
            const char *name = TDF_GetFirstSection(tdf);
            while (name && out->start_count < TAK_MAP_STARTS_MAX) {
                if (TDF_PushSection(tdf, name) == 0) {
                    const char *what = TDF_ReadString(tdf, "specialwhat", "");
                    if (what && strncmp(what, "StartPos", 8) == 0 &&
                        what[8] >= '1' && what[8] <= '9') {
                        int k = out->start_count++;
                        number[k] = what[8] - '0';
                        out->start_x[k] = TDF_ReadInt(tdf, "XPos", 0);
                        out->start_z[k] = TDF_ReadInt(tdf, "ZPos", 0);
                    }
                    TDF_PopSection(tdf);
                }
                name = TDF_GetNextSection(tdf);
            }
        }
    }
    TDF_Close(tdf);
    for (int i = 1; i < out->start_count; i++) {       /* by number, stable */
        int n = number[i], x = out->start_x[i], z = out->start_z[i], k = i;
        while (k > 0 && number[k - 1] > n) {
            number[k] = number[k - 1];
            out->start_x[k] = out->start_x[k - 1];
            out->start_z[k] = out->start_z[k - 1];
            k--;
        }
        number[k] = n;
        out->start_x[k] = x;
        out->start_z[k] = z;
    }
    return 0;
}

/* ── The query ──────────────────────────────────────────────────────── */

void TAK_MapQuery_Fold(const char *in, char *out, size_t cap) {
    size_t n = 0;
    int space = 1;
    if (!cap) return;
    for (const char *p = in ? in : ""; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c == '\'') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
            space = 0;
        } else if (!space) {
            out[n++] = ' ';
            space = 1;
        }
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

int TAK_MapQuery_SizeClass(int size_x, int size_y) {
    int a = size_x * size_y;
    return a <= 64 ? TAK_MAPSIZE_SMALL : a <= 144 ? TAK_MAPSIZE_MEDIUM
         : a <= 256 ? TAK_MAPSIZE_LARGE : TAK_MAPSIZE_HUGE;
}

const char *TAK_MapQuery_SizeName(int size) {
    static const char *names[TAK_MAPSIZE_COUNT] = {
        "Any size", "Small", "Medium", "Large", "Huge"
    };
    return size >= 0 && size < TAK_MAPSIZE_COUNT ? names[size] : names[0];
}

const char *TAK_MapQuery_SortName(int sort) {
    static const char *names[TAK_MAPSORT_COUNT] = {
        "Name, A to Z", "Name, Z to A", "Fewest players", "Most players",
        "Smallest", "Largest"
    };
    return sort >= 0 && sort < TAK_MAPSORT_COUNT ? names[sort] : names[0];
}

int TAK_MapQuery_StepPlayers(int players, int by) {
    int i = players <= 0 ? 0 : players - 1;
    i = ((i + by) % 8 + 8) % 8;
    return i == 0 ? 0 : i + 1;
}

static int by_name(const TAK_MapRow *a, const TAK_MapRow *b) {
    int c = tak_stricmp(a->display, b->display);
    return c ? c : tak_stricmp(a->key, b->key);
}

static int row_order(const TAK_MapRow *a, const TAK_MapRow *b, int sort) {
    int c = 0;
    switch (sort / 2) {
    case 1: c = a->players - b->players; break;
    case 2: c = a->size_x * a->size_y - b->size_x * b->size_y; break;
    default: break;
    }
    if (!c) c = by_name(a, b);
    return (sort & 1) ? -c : c;
}

static int has_words(const char *fold, const char *words) {
    char word[40];
    const char *p = words;
    while (*p) {
        while (*p == ' ') p++;
        size_t n = 0;
        while (p[n] && p[n] != ' ' && n + 1 < sizeof(word)) { word[n] = p[n]; n++; }
        word[n] = '\0';
        p += n;
        while (*p && *p != ' ') p++;
        if (n && !strstr(fold, word)) return 0;
    }
    return 1;
}

int TAK_MapQuery_Run(const TAK_MapRow *rows, int n, const TAK_MapQuery *q,
                     int *out) {
    char words[40];
    TAK_MapQuery_Fold(q ? q->text : "", words, sizeof(words));
    int count = 0;
    for (int i = 0; i < n; i++) {
        const TAK_MapRow *r = &rows[i];
        if (q && q->players > 0 && r->players != q->players) continue;
        if (q && q->size != TAK_MAPSIZE_ANY &&
            TAK_MapQuery_SizeClass(r->size_x, r->size_y) != q->size) continue;
        if (!has_words(r->fold, words)) continue;
        /* Insertion keeps it stable and the lists are a few hundred. */
        int k = count++;
        while (k > 0 && row_order(&rows[out[k - 1]], r, q ? q->sort : 0) > 0) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = i;
    }
    return count;
}

/* ── The browser ────────────────────────────────────────────────────── */

int MapBrowser_Load(TAK_MapBrowser *b, const TranslateTable *tt) {
    MapBrowser_Free(b);
    TAK_MapEntry *found = NULL;
    int n = 0;
    if (TAK_Maps_Scan(&found, &n) != 0) return -1;
    if (n > 0) {
        b->rows = (TAK_MapRow *)tak_malloc(sizeof(TAK_MapRow) * (size_t)n);
        b->shown = (int *)tak_malloc(sizeof(int) * (size_t)n);
        if (!b->rows || !b->shown) { TAK_Maps_Free(found); MapBrowser_Free(b); return -1; }
    }
    for (int i = 0; i < n; i++) {
        TAK_MapRow *r = &b->rows[i];
        memset(r, 0, sizeof(*r));
        snprintf(r->key, sizeof(r->key), "%s", found[i].key);
        if (tt) Translate_MapName(tt, r->key, r->display, sizeof(r->display));
        else snprintf(r->display, sizeof(r->display), "%s", r->key);
        TAK_MapQuery_Fold(r->display, r->fold, sizeof(r->fold));
        TAK_MapSummary s;
        if (TAK_MapSummary_Read(r->key, &s) == 0) {
            r->players = s.start_count > 0 ? s.start_count : s.max_players;
            r->size_x = s.size_x;
            r->size_y = s.size_y;
        }
    }
    b->count = n;
    TAK_Maps_Free(found);
    /* The rows keep name order, so an index into them is stable while
     * the query changes. */
    TAK_MapQuery all;
    memset(&all, 0, sizeof(all));
    int k = TAK_MapQuery_Run(b->rows, b->count, &all, b->shown);
    TAK_MapRow *sorted = n > 0 ? (TAK_MapRow *)tak_malloc(sizeof(TAK_MapRow) * (size_t)n) : NULL;
    if (sorted) {
        for (int i = 0; i < k; i++) sorted[i] = b->rows[b->shown[i]];
        tak_free(b->rows);
        b->rows = sorted;
    }
    MapBrowser_Refresh(b);
    return 0;
}

void MapBrowser_Free(TAK_MapBrowser *b) {
    tak_free(b->rows);
    tak_free(b->shown);
    TAK_MapQuery q = b->q;
    memset(b, 0, sizeof(*b));
    b->q = q;
}

void MapBrowser_Refresh(TAK_MapBrowser *b) {
    b->shown_count = b->shown ? TAK_MapQuery_Run(b->rows, b->count, &b->q, b->shown) : 0;
}

int MapBrowser_ShownAt(const TAK_MapBrowser *b, int row) {
    for (int i = 0; i < b->shown_count; i++) if (b->shown[i] == row) return i;
    return -1;
}

static int in_rect(const SDL_Rect *r, int x, int y) {
    SDL_Point pt = { x, y };
    return r->w > 0 && SDL_PointInRect(&pt, r);
}

int MapBrowser_StripPress(TAK_MapBrowser *b, const TAK_MapStrip *s,
                          int mx, int my, int by) {
    if (in_rect(&s->players, mx, my)) {
        b->q.players = TAK_MapQuery_StepPlayers(b->q.players, by);
    } else if (in_rect(&s->size, mx, my)) {
        b->q.size = ((b->q.size + by) % TAK_MAPSIZE_COUNT + TAK_MAPSIZE_COUNT) % TAK_MAPSIZE_COUNT;
    } else if (in_rect(&s->sort, mx, my)) {
        b->q.sort = ((b->q.sort + by) % TAK_MAPSORT_COUNT + TAK_MAPSORT_COUNT) % TAK_MAPSORT_COUNT;
    } else if (in_rect(&s->search, mx, my) && by < 0) {
        b->q.text[0] = '\0';                /* a right click clears it */
    } else {
        return 0;
    }
    MapBrowser_Refresh(b);
    return 1;
}

int MapBrowser_Type(TAK_MapBrowser *b, const TAK_Platform *p, int backspace) {
    int changed = 0;
    size_t n = strlen(b->q.text);
    if (backspace && n) { b->q.text[--n] = '\0'; changed = 1; }
    for (int i = 0; p && i < p->text_in_len; i++) {
        if (n + 1 >= sizeof(b->q.text)) break;
        b->q.text[n++] = p->text_in[i];
        b->q.text[n] = '\0';
        changed = 1;
    }
    if (changed) MapBrowser_Refresh(b);
    return changed;
}

static void frame_box(SDL_Surface *off, const SDL_Rect *r) {
    SDL_FillRect(off, r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    uint32_t edge = SDL_MapRGBA(off->format, 90, 70, 40, 255);
    SDL_Rect e = *r; e.h = 1; SDL_FillRect(off, &e, edge);
    e.y = r->y + r->h - 1; SDL_FillRect(off, &e, edge);
    e = *r; e.w = 1; SDL_FillRect(off, &e, edge);
    e.x = r->x + r->w - 1; SDL_FillRect(off, &e, edge);
}

static void centred(SDL_Surface *off, Font *font, const SDL_Rect *r, const char *t) {
    int w = Font_MeasureString(font, t);
    Font_DrawString(font, off, r->x + (r->w - w) / 2,
                    Font_CenterY(font, r->y, r->h), t);
}

void MapBrowser_DrawStrip(const TAK_MapBrowser *b, const TAK_MapStrip *s,
                          SDL_Surface *off, Font *font) {
    if (!off || !font) return;
    char text[64];
    frame_box(off, &s->search);
    snprintf(text, sizeof(text), "%s_", b->q.text[0] ? b->q.text : "Search maps ");
    Font_DrawString(font, off, s->search.x + 4,
                    Font_CenterY(font, s->search.y, s->search.h), text);
    frame_box(off, &s->players);
    if (b->q.players > 0) snprintf(text, sizeof(text), "%d players", b->q.players);
    else snprintf(text, sizeof(text), "Any players");
    centred(off, font, &s->players, text);
    frame_box(off, &s->size);
    centred(off, font, &s->size, TAK_MapQuery_SizeName(b->q.size));
    frame_box(off, &s->sort);
    centred(off, font, &s->sort, TAK_MapQuery_SortName(b->q.sort));
}

/* ── Start positions ────────────────────────────────────────────────── */

void StartMarks_FromConfig(TAK_StartMarks *m, const TAK_MapSummary *map,
                           const BattleConfig *cfg) {
    memset(m, 0, sizeof(*m));
    m->count = map ? map->start_count : 0;
    for (int i = 0; i < m->count; i++) {
        m->x[i] = map->start_x[i];
        m->z[i] = map->start_z[i];
        m->colour[i] = -1;
    }
    if (!cfg) return;
    int dealt[TAK_MAX_PLAYERS];
    BattleConfig_AssignStarts(cfg, m->count, dealt);
    for (int s = 0; s < TAK_MAX_PLAYERS; s++) {
        int k = dealt[s];
        if (k < 0) continue;
        int claimed = cfg->players[s].start_pos == k + 1;
        /* With random starts the rest are not known until the battle. */
        if (!claimed && cfg->random_start_locations) continue;
        m->colour[k] = cfg->players[s].color;
        m->claimed[k] = claimed;
    }
}

SDL_Rect StartMap_Fit(SDL_Rect area, int content_w, int content_h) {
    SDL_Rect r = area;
    if (content_w < 1 || content_h < 1) return r;
    if (content_w * area.h >= content_h * area.w) {
        r.w = area.w;
        r.h = (content_h * area.w + content_w / 2) / content_w;
    } else {
        r.h = area.h;
        r.w = (content_w * area.h + content_h / 2) / content_h;
    }
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = area.x + (area.w - r.w) / 2;
    r.y = area.y + (area.h - r.h) / 2;
    return r;
}

void StartMap_Point(SDL_Rect content, int map_w, int map_h,
                    const TAK_StartMarks *m, int i, int *x, int *y) {
    if (map_w < 1) map_w = 1;
    if (map_h < 1) map_h = 1;
    *x = content.x + m->x[i] * content.w / map_w;
    *y = content.y + m->z[i] * content.h / map_h;
}

#define MARK_R 6

int StartMap_Hit(SDL_Rect content, int map_w, int map_h,
                 const TAK_StartMarks *m, int x, int y) {
    int best = -1, best_d = (MARK_R + 2) * (MARK_R + 2) + 1;
    for (int i = 0; i < m->count; i++) {
        int px, py;
        StartMap_Point(content, map_w, map_h, m, i, &px, &py);
        int d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

void StartMap_Draw(SDL_Surface *off, SDL_Rect content, int map_w, int map_h,
                   const TAK_StartMarks *m, Font *font) {
    if (!off) return;
    uint32_t ink = SDL_MapRGBA(off->format, 24, 16, 8, 255);
    uint32_t vellum = SDL_MapRGBA(off->format, 232, 214, 170, 255);
    for (int i = 0; i < m->count; i++) {
        int px, py;
        StartMap_Point(content, map_w, map_h, m, i, &px, &py);
        uint32_t fill = vellum;
        if (m->colour[i] >= 0) {
            const TakPlayerColor *c = BattleConfig_PlayerColor(m->colour[i]);
            fill = SDL_MapRGBA(off->format, c->r, c->g, c->b, 255);
        }
        /* A claim is a full jewel, a start the rule deals only a ring. */
        SDL_Rect outer = { px - MARK_R, py - MARK_R, MARK_R * 2 + 1, MARK_R * 2 + 1 };
        SDL_FillRect(off, &outer, ink);
        SDL_Rect inner = { outer.x + 1, outer.y + 1, outer.w - 2, outer.h - 2 };
        SDL_FillRect(off, &inner, fill);
        if (m->colour[i] >= 0 && !m->claimed[i]) {
            SDL_Rect hole = { inner.x + 2, inner.y + 2, inner.w - 4, inner.h - 4 };
            SDL_FillRect(off, &hole, vellum);
        }
        if (font) {
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            int w = Font_MeasureString(font, num);
            Font_DrawString(font, off, px - w / 2, Font_CenterY(font, outer.y, outer.h), num);
        }
    }
}

/* ── Claims on a lineup ─────────────────────────────────────────────── */

static int holder_of(const BattleConfig *cfg, int start) {
    for (int s = 0; s < TAK_MAX_PLAYERS; s++)
        if (cfg->players[s].kind != TAK_SLOT_CLOSED &&
            cfg->players[s].start_pos == start + 1) return s;
    return -1;
}

int StartClaims_Take(BattleConfig *cfg, int seat, int start, int count) {
    if (seat < 0 || seat >= TAK_MAX_PLAYERS || start < 0 || start >= count) return 0;
    PlayerSlot *p = &cfg->players[seat];
    if (p->kind == TAK_SLOT_CLOSED) return 0;
    if (p->start_pos == start + 1) { p->start_pos = 0; return 1; }
    int h = holder_of(cfg, start);
    if (h >= 0) cfg->players[h].start_pos = p->start_pos;
    p->start_pos = start + 1;
    return 1;
}

int StartClaims_Move(BattleConfig *cfg, int from, int to, int count) {
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) return 0;
    int h = holder_of(cfg, from);
    if (h < 0) {
        /* Nobody claimed it, so move whoever the rule deals it to. */
        int dealt[TAK_MAX_PLAYERS];
        BattleConfig_AssignStarts(cfg, count, dealt);
        for (int s = 0; s < TAK_MAX_PLAYERS && h < 0; s++)
            if (dealt[s] == from && !cfg->random_start_locations) h = s;
        if (h < 0) return 0;
    }
    int other = holder_of(cfg, to);
    if (other >= 0 && other != h) cfg->players[other].start_pos = cfg->players[h].start_pos;
    cfg->players[h].start_pos = to + 1;
    return 1;
}

void StartClaims_Tidy(BattleConfig *cfg, int count) {
    int seen = 0;
    for (int s = 0; s < TAK_MAX_PLAYERS; s++) {
        PlayerSlot *p = &cfg->players[s];
        int k = p->start_pos - 1;
        if (p->start_pos <= 0) continue;
        if (p->kind == TAK_SLOT_CLOSED || k >= count || (seen & (1 << k))) {
            p->start_pos = 0;
            continue;
        }
        seen |= 1 << k;
    }
}
