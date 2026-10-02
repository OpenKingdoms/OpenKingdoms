/* The planner beside its copy from before plan buffers were kept
 * (test_pathing_reference.c): same routes over seeded random maps, then
 * a bench of both. --bench runs the long bench alone. */
#include "tak_pathing.h"
#include "tak_world.h"
#include "tak_terrain.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Terrain as test_pathing.c has it: raw height less 32, and a tile is
 * walkable when no neighbour steps more than the slope. */
int Terrain_SampleHeight(const struct GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    if (!world || !world->tnt.heightmap) return 0;
    if (world_x < 0) world_x = 0;
    if (world_y < 0) world_y = 0;
    int tx = (int)(world_x / 16);
    int ty = (int)(world_y / 16);
    if (tx >= world->tnt.height_w) tx = world->tnt.height_w - 1;
    if (ty >= world->tnt.height_h) ty = world->tnt.height_h - 1;
    return (int)world->tnt.heightmap[ty * world->tnt.height_w + tx] - 32;
}

int Terrain_IsWalkable(const struct GameWorld *world,
                       int32_t world_x, int32_t world_y,
                       int max_slope) {
    if (!world || world_x < 0 || world_y < 0 ||
        world_x >= world->map_pixels_w || world_y >= world->map_pixels_h) {
        return 0;
    }
    if (max_slope <= 0) max_slope = 12;
    int h0 = Terrain_SampleHeight(world, world_x, world_y);
    static const int off[4][2] = { {16,0}, {-16,0}, {0,16}, {0,-16} };
    for (int i = 0; i < 4; i++) {
        int32_t sx = world_x + off[i][0];
        int32_t sy = world_y + off[i][1];
        if (sx < 0 || sy < 0 || sx >= world->map_pixels_w ||
            sy >= world->map_pixels_h) {
            continue;
        }
        int dh = Terrain_SampleHeight(world, sx, sy) - h0;
        if (dh < 0) dh = -dh;
        if (dh > max_slope) return 0;
    }
    return 1;
}

void Terrain_WalkableTiles(const struct GameWorld *world, int max_slope,
                           uint8_t *out, int tw, int th) {
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            out[ty * tw + tx] = (uint8_t)Terrain_IsWalkable(
                world, tx * 16 + 8, ty * 16 + 8, max_slope);
        }
    }
}

void Terrain_WalkableTilesRect(const struct GameWorld *world, int max_slope,
                               uint8_t *out, int tw, int th,
                               int x0, int y0, int x1, int y1) {
    for (int ty = y0 < 0 ? 0 : y0; ty <= y1 && ty < th; ty++) {
        for (int tx = x0 < 0 ? 0 : x0; tx <= x1 && tx < tw; tx++) {
            out[ty * tw + tx] = (uint8_t)Terrain_IsWalkable(
                world, tx * 16 + 8, ty * 16 + 8, max_slope);
        }
    }
}

/* The planner before its buffers were kept (test_pathing_reference.c). */
int Ref_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query, TAK_Path *out_path);
int Ref_PathPlanFlow(const struct GameWorld *world,
                     int32_t start_x, int32_t start_y,
                     int32_t goal_x, int32_t goal_y,
                     const TAK_PathQuery *query, TAK_Path *out_path);
void Ref_PathCacheReset(void);
void Ref_PathDebugGetCounters(TAK_PathDebugCounters *out);
void Ref_PathDebugUseDistanceField(int on);

static int g_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* A whole number in [lo, hi]. */
static int rng_in(uint32_t *s, int lo, int hi) {
    return lo + (int)(rng_next(s) % (uint32_t)(hi - lo + 1));
}

static void reset_both(void) {
    TAK_PathCacheReset();
    Ref_PathCacheReset();
}

/* ── Random maps ───────────────────────────────────────────────────
 * Hills, cliffs, ridges, lakes, closed boxes, and on most maps
 * structures, gates of three owners and parked units. */

#define RAW_GROUND 64
#define RAW_RIDGE  230
#define SEA_LEVEL  30   /* ground is 32 above it after the -32 bias */

typedef struct RandWorld {
    GameWorld w;
    int parked_handles[64];
    int parked_n;
    uint32_t seed;
} RandWorld;

static void raise_rect(GameWorld *w, int tx0, int ty0, int tx1, int ty1,
                       int raw, int add) {
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            if (tx < 0 || ty < 0 || tx >= w->tnt.height_w ||
                ty >= w->tnt.height_h) continue;
            uint8_t *h = &w->tnt.heightmap[ty * w->tnt.height_w + tx];
            int v = add ? *h + raw : raw;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            *h = (uint8_t)v;
        }
    }
}

static void stamp_structure(GameWorld *w, int handle, int owner, int tx,
                            int ty, int fx, int fz, int gate, int open) {
    static uint8_t yard[16];
    memset(yard, gate ? 0x2d : 0x2f, sizeof(yard));
    TAK_OccStamp st;
    memset(&st, 0, sizeof(st));
    st.tx0 = tx;
    st.ty0 = ty;
    st.fx = fx;
    st.fz = fz;
    st.yard = yard;
    st.yard_open = open;
    st.is_gate = gate;
    st.handle = handle;
    st.owner = owner;
    Occ_ImprintStamp(w, &st, 1, NULL, NULL);
}

/* sparse divides the ridges, cliffs and lakes by ten, which leaves
 * ground wide enough for a search to run past its node budget. */
static int rand_world_build(RandWorld *r, uint32_t seed, int px_w, int px_h,
                            int sparse) {
    memset(r, 0, sizeof(*r));
    r->seed = seed;
    uint32_t s = seed;
    GameWorld *w = &r->w;
    w->map_pixels_w = px_w;
    w->map_pixels_h = px_h;
    w->tnt.height_w = px_w / 16 + 1;
    w->tnt.height_h = px_h / 16 + 1;
    int hw = w->tnt.height_w, hh = w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)malloc((size_t)hw * (size_t)hh);
    if (!w->tnt.heightmap) return 0;
    memset(w->tnt.heightmap, RAW_GROUND, (size_t)hw * (size_t)hh);
    int area = hw * hh;
    int thin = sparse ? 10 : 1;

    int hills = area / 300 + rng_in(&s, 0, 4);
    for (int i = 0; i < hills; i++) {
        int x = rng_in(&s, 0, hw - 1), y = rng_in(&s, 0, hh - 1);
        int x1 = x + rng_in(&s, 1, 8), y1 = y + rng_in(&s, 1, 8);
        int add = rng_in(&s, -6, 10);
        raise_rect(w, x, y, x1, y1, add, 1);
    }
    int cliffs = area / 1500 / thin + rng_in(&s, 0, 2);
    for (int i = 0; i < cliffs; i++) {
        int x = rng_in(&s, 0, hw - 1), y = rng_in(&s, 0, hh - 1);
        int x1 = x + rng_in(&s, 2, 12), y1 = y + rng_in(&s, 2, 12);
        int add = rng_in(&s, 25, 60);
        raise_rect(w, x, y, x1, y1, add, 1);
    }
    int ridges = area / 1200 / thin + rng_in(&s, 0, 3);
    for (int i = 0; i < ridges; i++) {
        int x = rng_in(&s, 0, hw - 1), y = rng_in(&s, 0, hh - 1);
        int len = rng_in(&s, 4, hw > hh ? hw : hh);
        int thick = rng_in(&s, 0, 2);
        if (rng_next(&s) & 1) raise_rect(w, x, y, x + len, y + thick, RAW_RIDGE, 0);
        else                  raise_rect(w, x, y, x + thick, y + len, RAW_RIDGE, 0);
    }
    /* Closed boxes: a goal inside is out of reach, a start inside is
     * trapped with whatever ground the box holds. */
    int boxes = rng_in(&s, 0, 2);
    for (int i = 0; i < boxes; i++) {
        int x0 = rng_in(&s, 0, hw - 1), y0 = rng_in(&s, 0, hh - 1);
        int x1 = x0 + rng_in(&s, 4, 14), y1 = y0 + rng_in(&s, 4, 14);
        raise_rect(w, x0, y0, x1, y0 + 1, RAW_RIDGE, 0);
        raise_rect(w, x0, y1 - 1, x1, y1, RAW_RIDGE, 0);
        raise_rect(w, x0, y0, x0 + 1, y1, RAW_RIDGE, 0);
        raise_rect(w, x1 - 1, y0, x1, y1, RAW_RIDGE, 0);
    }
    if (rng_in(&s, 0, 2) != 0) {
        w->water_height = SEA_LEVEL;
        int lakes = area / 900 / thin + rng_in(&s, 1, 3);
        for (int i = 0; i < lakes; i++) {
            int x = rng_in(&s, 0, hw - 1), y = rng_in(&s, 0, hh - 1);
            /* Raw 34 to 58 is 2 to 28 deep, raw 0 to 20 is open sea. */
            int raw = (rng_next(&s) & 1) ? rng_in(&s, 34, 58) : rng_in(&s, 0, 20);
            int x1 = x + rng_in(&s, 2, 20), y1 = y + rng_in(&s, 2, 20);
            raise_rect(w, x, y, x1, y1, raw, 0);
        }
    }

    if (rng_in(&s, 0, 3) == 0) return 1;       /* no occupancy layer */
    if (!Occ_Ensure(w)) return 0;
    int handle = 1;
    int ow = w->occ_w, oh = w->occ_h;
    int buildings = ow * oh / 250 + rng_in(&s, 0, 3);
    for (int i = 0; i < buildings; i++) {
        int owner = rng_in(&s, 1, 3);
        int tx = rng_in(&s, 0, ow - 1), ty = rng_in(&s, 0, oh - 1);
        int fx = rng_in(&s, 1, 4), fz = rng_in(&s, 1, 4);
        stamp_structure(w, handle++, owner, tx, ty, fx, fz, 0, 0);
    }
    int walls = rng_in(&s, 0, 3);
    for (int i = 0; i < walls; i++) {
        int tx = rng_in(&s, 0, ow - 1), ty = rng_in(&s, 0, oh - 1);
        int len = rng_in(&s, 3, 12), vertical = (int)(rng_next(&s) & 1);
        int owner = rng_in(&s, 1, 3);
        for (int k = 0; k < len; k++) {
            int gate = k == len / 2;
            stamp_structure(w, handle++, owner, vertical ? tx : tx + k * 2,
                            vertical ? ty + k * 2 : ty, 2, 2, gate,
                            gate ? (int)(rng_next(&s) & 1) : 0);
        }
    }
    int units = ow * oh / 150 + rng_in(&s, 0, 4);
    for (int i = 0; i < units; i++) {
        int h = handle++;
        int tx = rng_in(&s, 0, ow - 1), ty = rng_in(&s, 0, oh - 1);
        int fp = rng_in(&s, 1, 3);
        Occ_MoveMobile(w, h, rng_in(&s, 1, 3), 0, 0, 0, tx, ty, fp, fp);
        if (rng_in(&s, 0, 2) != 0) {
            Occ_SetMobileParked(w, h, tx, ty, fp, fp, 1);
            if (r->parked_n < 64) r->parked_handles[r->parked_n++] = h;
        }
    }
    return 1;
}

static void rand_world_free(RandWorld *r) {
    Occ_Free(&r->w);
    free(r->w.tnt.heightmap);
    r->w.tnt.heightmap = NULL;
}

/* Move classes a query picks from, and NULL for the fallback slope. */
#define CLASS_COUNT 7
static MoveClassDef g_classes[CLASS_COUNT];

static void classes_init(void) {
    memset(g_classes, 0, sizeof(g_classes));
    static const int spec[CLASS_COUNT][5] = {
        /* fx fz slope min_water max_water */
        { 1, 1, 12, 0,   0 },    /* walker */
        { 2, 2, 20, 0,  10 },    /* wades the shallows */
        { 3, 2, 30, 0,   0 },    /* wide and not square */
        { 4, 4, 40, 0, 255 },    /* amphibious and big */
        { 2, 2, 60, 8, 255 },    /* a boat */
        { 1, 1, 80, 0,   0 },    /* a climber */
        { 1, 2,  8, 0,   4 },    /* narrow, timid on slopes */
    };
    for (int i = 0; i < CLASS_COUNT; i++) {
        g_classes[i].footprint_x = spec[i][0];
        g_classes[i].footprint_z = spec[i][1];
        g_classes[i].max_slope = spec[i][2];
        g_classes[i].min_water_depth = spec[i][3];
        g_classes[i].max_water_depth = spec[i][4];
    }
}

/* A point on the map, near it, or just off an edge. */
static int32_t rand_coord(uint32_t *s, int extent) {
    int roll = rng_in(s, 0, 19);
    if (roll == 0) return -rng_in(s, 1, 80);
    if (roll == 1) return extent + rng_in(s, 0, 80);
    if (roll == 2) return rng_in(s, 0, 1) ? 0 : extent - 1;
    return rng_in(s, 0, extent - 1);
}

typedef struct PlanCase {
    int32_t sx, sy, gx, gy;
    TAK_PathQuery q;
} PlanCase;

static void rand_case(uint32_t *s, const RandWorld *r, PlanCase *pc) {
    const GameWorld *w = &r->w;
    memset(pc, 0, sizeof(*pc));
    int k = rng_in(s, 0, CLASS_COUNT);
    pc->q.move_class = k < CLASS_COUNT ? &g_classes[k] : NULL;
    static const int slopes[4] = { 8, 12, 20, 30 };
    pc->q.fallback_max_slope = slopes[rng_in(s, 0, 3)];
    pc->q.player_id = rng_in(s, 0, 3);
    /* Each footprint is a cache layer of its own, and past sixteen the
     * caches are dropped, so a few and not many. */
    if (rng_in(s, 0, 7) == 0) {
        pc->q.footprint_x = rng_in(s, 1, 3);
        pc->q.footprint_z = pc->q.footprint_x == 3 ? 2 : pc->q.footprint_x;
    }
    pc->q.compress = rng_in(s, 0, 3) != 0;
    pc->q.goal_is_unit = rng_in(s, 0, 3) == 0;
    pc->sx = rand_coord(s, w->map_pixels_w);
    pc->sy = rand_coord(s, w->map_pixels_h);
    if (r->parked_n > 0 && rng_in(s, 0, 4) == 0) {
        /* Planning as a parked unit, from where it stands. */
        int h = r->parked_handles[rng_in(s, 0, r->parked_n - 1)];
        pc->q.self_plus1 = h + 1;
        for (int i = 0; w->occ && i < w->occ_w * w->occ_h; i++) {
            if (w->occ[i].unit_plus1 == (uint16_t)(h + 1)) {
                pc->sx = (i % w->occ_w) * 16 + 8;
                pc->sy = (i / w->occ_w) * 16 + 8;
                break;
            }
        }
    } else if (rng_in(s, 0, 6) == 0) {
        pc->q.self_plus1 = rng_in(s, 1, 200);
    }
    int roll = rng_in(s, 0, 9);
    if (roll < 4) {
        pc->gx = pc->sx + rng_in(s, -12, 12) * 32;
        pc->gx += rng_in(s, -15, 15);
        pc->gy = pc->sy + rng_in(s, -12, 12) * 32;
        pc->gy += rng_in(s, -15, 15);
    } else if (roll < 8) {
        pc->gx = rand_coord(s, w->map_pixels_w);
        pc->gy = rand_coord(s, w->map_pixels_h);
    } else if (roll < 9) {
        pc->gx = pc->sx;
        pc->gy = pc->sy;
    } else {
        pc->gx = rng_in(s, 0, 1) ? -rng_in(s, 0, 200) : w->map_pixels_w + rng_in(s, 0, 200);
        pc->gy = rand_coord(s, w->map_pixels_h);
    }
}

/* ── Comparing the two ──────────────────────────────────────────── */

typedef struct Tally {
    int plans, routes, none, pinched, capped, flows, flow_routes;
    int mismatches;
} Tally;

static int same_path(const TAK_Path *a, const TAK_Path *b) {
    if (a->count != b->count || a->start_x != b->start_x ||
        a->start_y != b->start_y) return 0;
    for (int i = 0; i < a->count && i < TAK_PATH_MAX_WAYPOINTS; i++) {
        if (a->x[i] != b->x[i] || a->y[i] != b->y[i]) return 0;
    }
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void report_mismatch(Tally *t, const char *what, const RandWorld *r,
                            int index, const PlanCase *pc, int n_new,
                            int n_ref, const TAK_Path *pn,
                            const TAK_Path *pr) {
    t->mismatches++;
    if (t->mismatches > 5) return;
    fprintf(stderr, "  %s differs: world seed %08x (%d x %d px), case %d, "
            "(%d,%d) to (%d,%d) class %d fp %dx%d player %d self %d "
            "compress %d unit %d: new %d pts start (%d,%d), before %d pts "
            "start (%d,%d)\n", what, (unsigned)r->seed,
            r->w.map_pixels_w, r->w.map_pixels_h, index,
            (int)pc->sx, (int)pc->sy, (int)pc->gx, (int)pc->gy,
            pc->q.move_class ? (int)(pc->q.move_class - g_classes) : -1,
            pc->q.footprint_x, pc->q.footprint_z, pc->q.player_id,
            pc->q.self_plus1, pc->q.compress, pc->q.goal_is_unit,
            n_new, (int)pn->start_x, (int)pn->start_y,
            n_ref, (int)pr->start_x, (int)pr->start_y);
}

/* One query through both planners. The nodes each search expanded
 * must match as well as the route. */
static void compare_plan(Tally *t, const RandWorld *r, int index,
                         const PlanCase *pc) {
    TAK_Path pn, pr;
    memset(&pn, 0xA5, sizeof(pn));
    memset(&pr, 0xA5, sizeof(pr));
    TAK_PathDebugCounters n0, n1, r0, r1;
    TAK_PathDebugGetCounters(&n0);
    Ref_PathDebugGetCounters(&r0);
    int nn = TAK_PathPlanQuery(&r->w, pc->sx, pc->sy, pc->gx, pc->gy,
                               &pc->q, &pn);
    int nr = Ref_PathPlanQuery(&r->w, pc->sx, pc->sy, pc->gx, pc->gy,
                               &pc->q, &pr);
    TAK_PathDebugGetCounters(&n1);
    Ref_PathDebugGetCounters(&r1);
    t->plans++;
    if (nn > 0) t->routes++;
    else t->none++;
    if (nr > 0 && (pr.start_x & 31) != 16) t->pinched++;
    if (r1.work - r0.work > 8192) t->capped++;
    if (nn != nr || !same_path(&pn, &pr) ||
        n1.work - n0.work != r1.work - r0.work) {
        report_mismatch(t, "route", r, index, pc, nn, nr, &pn, &pr);
    }
}

static void compare_flow(Tally *t, const RandWorld *r, int index,
                         const PlanCase *pc) {
    TAK_Path pn, pr;
    memset(&pn, 0xA5, sizeof(pn));
    memset(&pr, 0xA5, sizeof(pr));
    int nn = TAK_PathPlanFlow(&r->w, pc->sx, pc->sy, pc->gx, pc->gy,
                              &pc->q, &pn);
    int nr = Ref_PathPlanFlow(&r->w, pc->sx, pc->sy, pc->gx, pc->gy,
                              &pc->q, &pr);
    t->flows++;
    if (nn > 0) t->flow_routes++;
    if (nn != nr || !same_path(&pn, &pr)) {
        report_mismatch(t, "flow route", r, index, pc, nn, nr, &pn, &pr);
    }
}

/* Plans on one map, many in a row with no reset between them, and a
 * group's worth of field routes to a few shared goals. */
static void run_world(Tally *t, RandWorld *r, uint32_t *s, int plans) {
    int32_t goals[3][2];
    for (int k = 0; k < 3; k++) {
        goals[k][0] = rng_in(s, 0, r->w.map_pixels_w - 1);
        goals[k][1] = rng_in(s, 0, r->w.map_pixels_h - 1);
    }
    for (int i = 0; i < plans; i++) {
        PlanCase pc;
        rand_case(s, r, &pc);
        compare_plan(t, r, i, &pc);
        if (rng_in(s, 0, 4) == 0) {
            int k = rng_in(s, 0, 2);
            pc.gx = goals[k][0];
            pc.gy = goals[k][1];
            compare_flow(t, r, i, &pc);
        }
    }
}

static int rand_extent(uint32_t *s, int lo_cells, int hi_cells) {
    /* Not always a whole number of cells or tiles, so the last row and
     * column of cells are part cells. */
    int px = rng_in(s, lo_cells, hi_cells) * 32;
    return px + (rng_in(s, 0, 2) ? 0 : rng_in(s, 1, 31));
}

static void test_routes_match_the_planner_before(void) {
    classes_init();
    reset_both();
    Tally t;
    memset(&t, 0, sizeof(t));
    uint32_t s = 0x2468ace1u;
    static RandWorld worlds[2];

    /* A run of maps, each loaded the way a battle loads one, with
     * the caches dropped first. */
    for (int m = 0; m < 24; m++) {
        reset_both();
        uint32_t seed = rng_next(&s);
        int px_w = rand_extent(&s, 2, 72), px_h = rand_extent(&s, 2, 72);
        RandWorld *r = &worlds[0];
        if (!rand_world_build(r, seed, px_w, px_h, m % 4 == 0)) {
            EXPECT(0);
            return;
        }
        run_world(&t, r, &s, 250);
        rand_world_free(r);
    }

    /* A small map and a big one planned on in turn with no reset, so
     * the buffers grow under the small map and are reused for it. */
    for (int m = 0; m < 4; m++) {
        reset_both();
        uint32_t seed_a = rng_next(&s), seed_b = rng_next(&s);
        int aw = rand_extent(&s, 4, 30), ah = rand_extent(&s, 4, 30);
        int bw = rand_extent(&s, 60, 140), bh = rand_extent(&s, 60, 140);
        if (!rand_world_build(&worlds[0], seed_a, aw, ah, 0) ||
            !rand_world_build(&worlds[1], seed_b, bw, bh, m & 1)) {
            EXPECT(0);
            return;
        }
        for (int round = 0; round < 6; round++) {
            run_world(&t, &worlds[round & 1], &s, 40);
        }
        rand_world_free(&worlds[0]);
        rand_world_free(&worlds[1]);
    }

    /* Searches past the node budget: the walled pen in the middle holds
     * open ground nothing outside it reaches. */
    for (int m = 0; m < 3; m++) {
        reset_both();
        RandWorld *r = &worlds[0];
        uint32_t seed = rng_next(&s);
        int px_w = rand_extent(&s, 120, 160), px_h = rand_extent(&s, 120, 160);
        if (!rand_world_build(r, seed, px_w, px_h, 1)) {
            EXPECT(0);
            return;
        }
        GameWorld *w = &r->w;
        int cx = w->tnt.height_w / 2, cy = w->tnt.height_h / 2;
        raise_rect(w, cx - 9, cy - 9, cx + 9, cy + 9, RAW_RIDGE, 0);
        raise_rect(w, cx - 7, cy - 7, cx + 7, cy + 7, RAW_GROUND, 0);
        run_world(&t, r, &s, 60);
        for (int i = 0; i < 30; i++) {
            PlanCase pc;
            rand_case(&s, r, &pc);
            pc.gx = (cx + rng_in(&s, -5, 5)) * 16 + 8;
            pc.gy = (cy + rng_in(&s, -5, 5)) * 16 + 8;
            compare_plan(&t, r, 1000 + i, &pc);
        }
        rand_world_free(r);
    }

    /* The flat search alone, without the long route field. */
    TAK_PathDebugUseDistanceField(0);
    Ref_PathDebugUseDistanceField(0);
    for (int m = 0; m < 4; m++) {
        reset_both();
        RandWorld *r = &worlds[0];
        uint32_t seed = rng_next(&s);
        int px_w = rand_extent(&s, 30, 90), px_h = rand_extent(&s, 30, 90);
        if (!rand_world_build(r, seed, px_w, px_h, m & 1)) {
            EXPECT(0);
            return;
        }
        run_world(&t, r, &s, 150);
        rand_world_free(r);
    }
    TAK_PathDebugUseDistanceField(1);
    Ref_PathDebugUseDistanceField(1);

    reset_both();

    printf("  %d plans: %d routes, %d with none, %d out of a pinch, "
           "%d past the node budget; %d field routes, %d found\n",
           t.plans, t.routes, t.none, t.pinched, t.capped, t.flows,
           t.flow_routes);
    EXPECT(t.mismatches == 0);
    /* The maps have to reach the cases worth comparing. */
    EXPECT(t.routes > t.plans / 4);
    EXPECT(t.none > t.plans / 20);
    EXPECT(t.pinched > 50);
    EXPECT(t.capped > 5);
    EXPECT(t.flow_routes > 100);
}

/* ── The bench ─────────────────────────────────────────────────────
 * Replans within 12 cells, walks within 40, marches anywhere, and a mix
 * of 70, 25 and 5 in a hundred. Caches are warm, so it times plans. */

static double now_ms(void) {
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int bench_world(RandWorld *r, int cells_side) {
    memset(r, 0, sizeof(*r));
    GameWorld *w = &r->w;
    w->map_pixels_w = cells_side * 32;
    w->map_pixels_h = cells_side * 32;
    w->tnt.height_w = w->map_pixels_w / 16 + 1;
    w->tnt.height_h = w->map_pixels_h / 16 + 1;
    int hw = w->tnt.height_w, hh = w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)malloc((size_t)hw * (size_t)hh);
    if (!w->tnt.heightmap) return 0;
    memset(w->tnt.heightmap, RAW_GROUND, (size_t)hw * (size_t)hh);
    w->water_height = SEA_LEVEL;
    uint32_t s = 0x13579bdfu ^ (uint32_t)cells_side;
    int blocks = cells_side * cells_side / 400;
    for (int i = 0; i < blocks; i++) {
        int x = rng_in(&s, 0, hw - 1), y = rng_in(&s, 0, hh - 1);
        int kind = rng_in(&s, 0, 3);
        if (kind == 0) {
            raise_rect(w, x, y, x + rng_in(&s, 4, 30), y + 1, RAW_RIDGE, 0);
        } else if (kind == 1) {
            raise_rect(w, x, y, x + 1, y + rng_in(&s, 4, 30), RAW_RIDGE, 0);
        } else if (kind == 2) {
            int x1 = x + rng_in(&s, 4, 16), y1 = y + rng_in(&s, 4, 16);
            int raw = rng_in(&s, 0, 50);
            raise_rect(w, x, y, x1, y1, raw, 0);
        } else {
            int x1 = x + rng_in(&s, 2, 10), y1 = y + rng_in(&s, 2, 10);
            int add = rng_in(&s, 2, 9);
            raise_rect(w, x, y, x1, y1, add, 1);
        }
    }
    if (!Occ_Ensure(w)) return 0;
    int handle = 1;
    for (int i = 0; i < blocks / 2; i++) {
        int owner = rng_in(&s, 1, 8);
        int tx = rng_in(&s, 0, w->occ_w - 1), ty = rng_in(&s, 0, w->occ_h - 1);
        int fx = rng_in(&s, 2, 4), fz = rng_in(&s, 2, 4);
        stamp_structure(w, handle++, owner, tx, ty, fx, fz, 0, 0);
    }
    return 1;
}

enum { ORDER_REPLAN, ORDER_BASE, ORDER_MARCH, ORDER_MIX, ORDER_KINDS };
static const char *const g_order_names[ORDER_KINDS] = {
    "replan, 12 cells", "base, 40 cells", "march, any", "battle mix"
};

static void bench_cases(PlanCase *cases, int n, int cells_side, int kind,
                        uint32_t seed) {
    uint32_t s = seed;
    int px = cells_side * 32;
    for (int i = 0; i < n; i++) {
        PlanCase *pc = &cases[i];
        memset(pc, 0, sizeof(*pc));
        int k = rng_in(&s, 0, 3);
        pc->q.move_class = &g_classes[k == 3 ? 5 : k];
        pc->q.fallback_max_slope = 12;
        pc->q.player_id = rng_in(&s, 1, 8);
        pc->q.compress = 1;
        pc->sx = rng_in(&s, 0, px - 1);
        pc->sy = rng_in(&s, 0, px - 1);
        int order = kind;
        if (kind == ORDER_MIX) {
            int roll = rng_in(&s, 0, 99);
            order = roll < 70 ? ORDER_REPLAN : roll < 95 ? ORDER_BASE : ORDER_MARCH;
        }
        int reach = order == ORDER_REPLAN ? 12 : order == ORDER_BASE ? 40 : 0;
        if (reach) {
            pc->gx = pc->sx + rng_in(&s, -reach, reach) * 32;
            pc->gy = pc->sy + rng_in(&s, -reach, reach) * 32;
        } else {
            pc->gx = rng_in(&s, 0, px - 1);
            pc->gy = rng_in(&s, 0, px - 1);
        }
        if (pc->gx < 0) pc->gx = 16;
        if (pc->gy < 0) pc->gy = 16;
        if (pc->gx >= px) pc->gx = px - 16;
        if (pc->gy >= px) pc->gy = px - 16;
    }
}

static double bench_pass(const RandWorld *r, const PlanCase *cases, int n,
                         int after, int *routes) {
    TAK_Path path;
    int found = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        const PlanCase *pc = &cases[i];
        int got = after
            ? TAK_PathPlanQuery(&r->w, pc->sx, pc->sy, pc->gx, pc->gy, &pc->q, &path)
            : Ref_PathPlanQuery(&r->w, pc->sx, pc->sy, pc->gx, pc->gy, &pc->q, &path);
        if (got > 0) found++;
    }
    double t = now_ms() - t0;
    *routes = found;
    return t;
}

/* Each kind of order through both planners in turn, best pass kept. */
static void bench_map(int cells_side, int plans, int passes) {
    RandWorld r;
    if (!bench_world(&r, cells_side)) { EXPECT(0); return; }
    PlanCase *cases = (PlanCase *)malloc((size_t)plans * sizeof(PlanCase));
    if (!cases) { EXPECT(0); rand_world_free(&r); return; }
    reset_both();
    for (int kind = 0; kind < ORDER_KINDS; kind++) {
        bench_cases(cases, plans, cells_side, kind,
                    0xbe5c0de5u ^ (uint32_t)(cells_side * 7 + kind));
        /* Every route the same, and the caches warm for both. */
        int mismatches = 0;
        for (int i = 0; i < plans; i++) {
            const PlanCase *pc = &cases[i];
            TAK_Path pn, pr;
            memset(&pn, 0, sizeof(pn));
            memset(&pr, 0, sizeof(pr));
            int nn = TAK_PathPlanQuery(&r.w, pc->sx, pc->sy, pc->gx, pc->gy,
                                       &pc->q, &pn);
            int nr = Ref_PathPlanQuery(&r.w, pc->sx, pc->sy, pc->gx, pc->gy,
                                       &pc->q, &pr);
            if (nn != nr || !same_path(&pn, &pr)) mismatches++;
        }
        EXPECT(mismatches == 0);

        TAK_PathDebugCounters c0, c1;
        TAK_PathDebugGetCounters(&c0);
        double before = -1.0, after = -1.0;
        int routes_before = 0, routes_after = 0;
        for (int p = 0; p < passes; p++) {
            double b = bench_pass(&r, cases, plans, 0, &routes_before);
            double a = bench_pass(&r, cases, plans, 1, &routes_after);
            if (before < 0 || b < before) before = b;
            if (after < 0 || a < after) after = a;
        }
        TAK_PathDebugGetCounters(&c1);
        EXPECT(routes_before == routes_after);
        double nodes = (double)(c1.work - c0.work) / (double)plans /
                       (double)passes;
        char ratio[16] = "   under a tick";
        if (after > 0) snprintf(ratio, sizeof(ratio), "%5.1fx", before / after);
        printf("  bench %3d cells a side  %-17s %5d plans  %7.0f nodes a plan  "
               "before %7.3f ms  after %7.3f ms a plan  %s\n",
               cells_side, g_order_names[kind], plans, nodes,
               before / plans, after / plans, ratio);
        fflush(stdout);
    }
    TAK_PathDebugCounters c;
    TAK_PathDebugGetCounters(&c);
    printf("  bench %3d cells a side  buffers held %u KB\n", cells_side,
           (unsigned)(c.plan_bytes / 1024u));
    reset_both();
    free(cases);
    rand_world_free(&r);
}

static void test_plan_bench(int full) {
    classes_init();
    /* A middling shipped map, the largest, and twice that each way.
     * Timings only, a shared machine cannot promise a speed, so the
     * short run is the big map alone, to show the bench still runs. */
    if (!full) {
        bench_map(480, 40, 1);
        return;
    }
    bench_map(128, 2000, 3);
    bench_map(240, 2000, 3);
    bench_map(480, 2000, 3);
}

int main(int argc, char **argv) {
    int full = argc > 1 && strcmp(argv[1], "--bench") == 0;
    if (!full) test_routes_match_the_planner_before();
    test_plan_bench(full);
    if (g_failures) {
        fprintf(stderr, "%d path buffer tests failed\n", g_failures);
        return 1;
    }
    printf("path buffer tests passed\n");
    return 0;
}
