/*
 * order_overlay.c -- a selection's orders, drawn while Shift is held.
 *
 * The owner's rule, as in Beyond All Reason: with Shift down each
 * selected unit shows a thin line through the orders it holds, a marker
 * at each and a ghost for each building it has queued. Without Shift
 * nothing is drawn. The route is read through Units_OrdersOf, the same
 * read the remaster's order lines use, and nothing here writes to a
 * unit.
 */

#include "tak_order_overlay.h"

#include "tak_platform.h"
#include "tak_terrain.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <string.h>

static int g_shift;

void OrderOverlay_SetShift(int held) { g_shift = held ? 1 : 0; }
int  OrderOverlay_Shift(void) { return g_shift; }

/* ── the route ────────────────────────────────────────────────────── */

static int overlay_mark(const UnitOrderView *v, int first) {
    if (v->rally && first) return ORDER_MARK_RALLY;
    switch (v->kind) {
        case UNIT_LEG_MOVE:          return ORDER_MARK_MOVE;
        case UNIT_LEG_ATTACK:
        case UNIT_LEG_ATTACK_GROUND:
        case UNIT_LEG_CAPTURE:
        case UNIT_LEG_SPECIAL:       return ORDER_MARK_ATTACK;
        case UNIT_LEG_PATROL:        return ORDER_MARK_PATROL;
        case UNIT_LEG_GUARD:
        case UNIT_LEG_REPAIR:        return ORDER_MARK_ASSIST;
        case UNIT_LEG_RECLAIM:
        case UNIT_LEG_SWEEP:         return ORDER_MARK_RECLAIM;
        case UNIT_LEG_BUILD:         return ORDER_MARK_BUILD;
        default:                     return ORDER_MARK_ACTION;
    }
}

static int overlay_put(OrderStop *out, int cap, int n, const OrderStop *st) {
    if (out && n < cap) out[n] = *st;
    return n + 1;
}

static OrderStop overlay_point(int mark, int32_t x, int32_t y) {
    OrderStop st;
    memset(&st, 0, sizeof st);
    st.mark = (uint8_t)mark;
    st.queued = 1;
    st.def = -1;
    st.x = x;
    st.y = y;
    st.target = -1;
    return st;
}

int OrderOverlay_Plan(int handle, OrderStop *out, int cap) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units || handle < 0 || handle >= count) return 0;
    const Unit *u = &units[handle];
    UnitOrderView v[ORDER_OVERLAY_STOPS_MAX];
    int n = Units_OrdersOf(handle, v, ORDER_OVERLAY_STOPS_MAX);
    if (n <= 0) return 0;
    if (n > ORDER_OVERLAY_STOPS_MAX) n = ORDER_OVERLAY_STOPS_MAX;
    /* The point a patrol in hand comes back through is listed last. */
    int back = v[n - 1].back ? n - 1 : -1;
    int m = back >= 0 ? back : n;

    int k = 0;
    int32_t px = u->world_x, py = u->world_y;
    int ring = 0, ring_points = 0, ring_in_hand = 0;
    int32_t start_x = 0, start_y = 0, first_x = 0, first_y = 0;
    for (int i = 0; i < m; i++) {
        OrderStop st = overlay_point(overlay_mark(&v[i], i == 0), v[i].x, v[i].y);
        st.queued = v[i].queued;
        if (v[i].kind == UNIT_LEG_BUILD) {
            st.def = v[i].def;
            st.facing = v[i].facing;
            st.ghost = v[i].queued && v[i].def >= 0;
        } else if (v[i].target >= 0 && v[i].target < count &&
                   units[v[i].target].alive == UNIT_ALIVE_ACTIVE) {
            const Unit *t = &units[v[i].target];
            if (Units_IsVisibleToLocalPlayer(t)) {
                st.target = v[i].target;
                st.x = t->world_x;
                st.y = t->world_y;
            } else if (!v[i].queued) {
                /* In hand its point follows the target, so it is left out. */
                continue;
            }
        }
        if (v[i].kind == UNIT_LEG_PATROL && !ring) {
            ring = 1;
            ring_points = 1;
            ring_in_hand = i == 0 && back >= 0;
            start_x = ring_in_hand ? v[back].x : px;
            start_y = ring_in_hand ? v[back].y : py;
            first_x = st.x;
            first_y = st.y;
        }
        k = overlay_put(out, cap, k, &st);
        px = st.x;
        py = st.y;
        if (!ring) continue;
        ring_points++;
        if (i + 1 < m && v[i + 1].kind == UNIT_LEG_PATROL) continue;
        /* The route comes back through its start and round again. With
         * two points the way back is the line already drawn. */
        if (ring_in_hand) {
            OrderStop home = overlay_point(ORDER_MARK_PATROL, start_x, start_y);
            k = overlay_put(out, cap, k, &home);
        }
        if (ring_points >= 3) {
            OrderStop close = ring_in_hand
                ? overlay_point(ORDER_MARK_NONE, first_x, first_y)
                : overlay_point(ORDER_MARK_NONE, start_x, start_y);
            k = overlay_put(out, cap, k, &close);
        }
        /* A patrol never ends, so nothing queued behind it is reached. */
        break;
    }
    return k;
}

/* ── drawing ──────────────────────────────────────────────────────── */

typedef struct OverlayColor { uint8_t r, g, b; } OverlayColor;

static const OverlayColor k_mark_color[ORDER_MARK_KINDS] = {
    {  80, 140, 255 },   /* NONE, a patrol route closing */
    {  60, 210,  70 },   /* MOVE */
    { 230,  60,  50 },   /* ATTACK */
    {  80, 140, 255 },   /* PATROL */
    { 240, 210,  70 },   /* RALLY */
    { 150, 230, 150 },   /* BUILD */
    {  70, 200, 230 },   /* ASSIST */
    { 190, 110, 230 },   /* RECLAIM */
    { 240, 150,  50 },   /* ACTION */
};

/* A queued ghost is a whole model, so a frame draws only so many. */
#define OVERLAY_GHOSTS_MAX 64
#define OVERLAY_LINE_ALPHA 190
/* Window pixels past the play area a ghost may start and still show. */
#define OVERLAY_GHOST_MARGIN 128

static void overlay_screen(const GameWorld *w, float tilt, int32_t x, int32_t y,
                           float alt, float *sx, float *sy) {
    *sx = (float)(x - w->cam_x);
    *sy = (float)(y - w->cam_y) -
          ((float)Terrain_SampleHeight(w, x, y) + alt) * tilt;
}

static int overlay_off(float ax, float ay, float bx, float by, int vw, int vh) {
    const float m = 16.0f;
    return (ax < -m && bx < -m) || (ay < -m && by < -m) ||
           (ax > (float)vw + m && bx > (float)vw + m) ||
           (ay > (float)vh + m && by > (float)vh + m);
}

static void overlay_color(SDL_Renderer *r, int mark, uint8_t alpha) {
    const OverlayColor *c = &k_mark_color[mark < ORDER_MARK_KINDS ? mark : 0];
    SDL_SetRenderDrawColor(r, c->r, c->g, c->b, alpha);
}

static void overlay_diamond(SDL_Renderer *r, float x, float y, float k) {
    SDL_FPoint p[5] = { { x, y - k }, { x + k, y }, { x, y + k },
                        { x - k, y }, { x, y - k } };
    SDL_RenderDrawLinesF(r, p, 5);
}

static void overlay_cross(SDL_Renderer *r, float x, float y, float k) {
    SDL_RenderDrawLineF(r, x - k, y - k, x + k, y + k);
    SDL_RenderDrawLineF(r, x - k, y + k, x + k, y - k);
}

static void overlay_plus(SDL_Renderer *r, float x, float y, float k) {
    SDL_RenderDrawLineF(r, x - k, y, x + k, y);
    SDL_RenderDrawLineF(r, x, y - k, x, y + k);
}

static void overlay_square(SDL_Renderer *r, float x, float y, float k) {
    SDL_FRect q = { x - k, y - k, 2.0f * k + 1.0f, 2.0f * k + 1.0f };
    SDL_RenderDrawRectF(r, &q);
}

/* A flag on a pole standing at (x, y). */
static void overlay_flag(SDL_Renderer *r, float x, float y) {
    SDL_RenderDrawLineF(r, x, y, x, y - 12.0f);
    SDL_FPoint p[4] = { { x, y - 12.0f }, { x + 7.0f, y - 9.0f },
                        { x, y - 6.0f }, { x, y - 12.0f } };
    SDL_RenderDrawLinesF(r, p, 4);
}

/* Corner brackets round a unit, half pixels either side of its centre. */
static void overlay_brackets(SDL_Renderer *r, float x, float y, float half) {
    const float l = 4.0f;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float cx = x + (float)sx * half, cy = y + (float)sy * half;
            SDL_RenderDrawLineF(r, cx, cy, cx - (float)sx * l, cy);
            SDL_RenderDrawLineF(r, cx, cy, cx, cy - (float)sy * l);
        }
    }
}

static float overlay_half(const Unit *t) {
    const UnitDef *d = Units_GetDef(t->def_idx);
    int cells = d ? (d->footprint_x > d->footprint_z ? d->footprint_x
                                                    : d->footprint_z) : 1;
    float half = (float)(cells * 8 + 4);
    return half < 8.0f ? 8.0f : half > 64.0f ? 64.0f : half;
}

static void overlay_marker(SDL_Renderer *r, const OrderStop *st, float x,
                           float y, const Unit *target) {
    if (st->mark == ORDER_MARK_NONE) return;
    overlay_color(r, st->mark, 255);
    switch (st->mark) {
        case ORDER_MARK_MOVE:   overlay_diamond(r, x, y, 4.0f); break;
        case ORDER_MARK_PATROL: overlay_square(r, x, y, 3.0f);  break;
        case ORDER_MARK_RALLY:  overlay_flag(r, x, y);          break;
        case ORDER_MARK_BUILD:  overlay_plus(r, x, y, 4.0f);    break;
        case ORDER_MARK_ATTACK:
        case ORDER_MARK_RECLAIM:
            if (target) overlay_brackets(r, x, y, overlay_half(target));
            else overlay_cross(r, x, y, 4.0f);
            break;
        default:
            if (target) overlay_brackets(r, x, y, overlay_half(target));
            else overlay_diamond(r, x, y, 4.0f);
            break;
    }
}

int OrderOverlay_Draw(const GameWorld *world, TAK_Platform *plat) {
    if (!g_shift || !world || !plat || !plat->renderer) return 0;
    int n_sel = 0, count = 0;
    const int *sel = Units_GetSelection(&n_sel);
    const Unit *units = Units_GetActive(&count);
    if (!sel || n_sel <= 0 || !units) return 0;
    SDL_Renderer *r = plat->renderer;
    SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &old_blend);
    const float tilt = Units_GetTanTilt();
    const int me = Units_LocalPlayer();
    const int vw = world->viewport_w, vh = world->viewport_h;
    int drawn = 0, ghosts = 0;
    OrderStop stops[ORDER_OVERLAY_STOPS_MAX];

    for (int s = 0; s < n_sel; s++) {
        int h = sel[s];
        if (h < 0 || h >= count) continue;
        const Unit *u = &units[h];
        /* Another player's orders are his own business. */
        if (u->alive != UNIT_ALIVE_ACTIVE || u->player_id != me) continue;
        int n = OrderOverlay_Plan(h, stops, ORDER_OVERLAY_STOPS_MAX);
        if (n > ORDER_OVERLAY_STOPS_MAX) n = ORDER_OVERLAY_STOPS_MAX;
        if (n <= 0) continue;

        /* Ghosts first, so the route reads over them. */
        for (int i = 0; i < n && ghosts < OVERLAY_GHOSTS_MAX; i++) {
            const OrderStop *st = &stops[i];
            if (!st->ghost) continue;
            float gx, gy;
            overlay_screen(world, tilt, st->x, st->y, 0.0f, &gx, &gy);
            if (gx < -OVERLAY_GHOST_MARGIN || gy < -OVERLAY_GHOST_MARGIN ||
                gx > vw + OVERLAY_GHOST_MARGIN || gy > vh + OVERLAY_GHOST_MARGIN)
                continue;
            Units_RenderBuildGhostFacing(plat, world, st->def, u->team_color_idx,
                                         st->x, st->y, 110, UNITS_GHOST_QUEUED,
                                         st->facing);
            ghosts++;
        }

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        float ax, ay;
        overlay_screen(world, tilt, u->world_x, u->world_y, u->flight_alt,
                       &ax, &ay);
        for (int i = 0; i < n; i++) {
            const OrderStop *st = &stops[i];
            const Unit *t = st->target >= 0 && st->target < count
                          ? &units[st->target] : NULL;
            float bx, by;
            overlay_screen(world, tilt, st->x, st->y, t ? t->flight_alt : 0.0f,
                           &bx, &by);
            if (!overlay_off(ax, ay, bx, by, vw, vh)) {
                overlay_color(r, st->mark, OVERLAY_LINE_ALPHA);
                SDL_RenderDrawLineF(r, ax, ay, bx, by);
                overlay_marker(r, st, bx, by, t);
                drawn++;
            }
            ax = bx;
            ay = by;
        }
    }
    SDL_SetRenderDrawBlendMode(r, old_blend);
    return drawn;
}
