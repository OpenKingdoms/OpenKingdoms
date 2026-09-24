/*
 * ai_squad.c -- potential fields for a squad on the march (A-010).
 */

#include "tak_ai_squad.h"

/* cos and sin of k/16 of a turn, times 1024. */
static const int16_t g_ring[AI_SQUAD_RING_POINTS][2] = {
    { 1024,    0 }, {  946,  392 }, {  724,  724 }, {  392,  946 },
    {    0, 1024 }, { -392,  946 }, { -724,  724 }, { -946,  392 },
    {-1024,    0 }, { -946, -392 }, { -724, -724 }, { -392, -946 },
    {    0,-1024 }, {  392, -946 }, {  724, -724 }, {  946, -392 },
};

static uint32_t squad_isqrt(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

static int32_t squad_dist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t dx = (int64_t)ax - bx, dy = (int64_t)ay - by;
    return (int32_t)squad_isqrt((uint64_t)(dx * dx + dy * dy));
}

int AI_Squad_ShouldWait(int32_t member_d, int64_t sum_d, int members,
                        int moving) {
    if (members < 3) return 0;
    int64_t mean = sum_d / members;
    int64_t lead = moving ? AI_SQUAD_LEAD : AI_SQUAD_LEAD / 2;
    return (int64_t)member_d < mean - lead;
}

int64_t AI_Squad_Push(int32_t x, int32_t y, const AiSquadCharge *enemies,
                      int count) {
    int64_t push = 0;
    for (int i = 0; i < count; i++) {
        int32_t d = squad_dist(x, y, enemies[i].x, enemies[i].y);
        if (d >= AI_SQUAD_CHARGE_REACH) continue;
        push += (int64_t)enemies[i].value * AI_SQUAD_CHARGE_WEIGHT *
                (AI_SQUAD_CHARGE_REACH - d) / AI_SQUAD_CHARGE_REACH;
    }
    return push;
}

int AI_Squad_FiringPoint(int32_t mx, int32_t my, int32_t tx, int32_t ty,
                         int32_t r, const AiSquadCharge *enemies, int count,
                         int32_t *out_x, int32_t *out_y) {
    int best = 0;
    int64_t best_u = 0;
    for (int k = 0; k < AI_SQUAD_RING_POINTS; k++) {
        int32_t px = tx + (int32_t)((int64_t)r * g_ring[k][0] / 1024);
        int32_t py = ty + (int32_t)((int64_t)r * g_ring[k][1] / 1024);
        int64_t u = squad_dist(mx, my, px, py) +
                    AI_Squad_Push(px, py, enemies, count);
        if (k == 0 || u < best_u) { best = k; best_u = u; }
    }
    if (out_x) *out_x = tx + (int32_t)((int64_t)r * g_ring[best][0] / 1024);
    if (out_y) *out_y = ty + (int32_t)((int64_t)r * g_ring[best][1] / 1024);
    return best;
}
