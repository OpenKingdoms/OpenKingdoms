/*
 * build_line.c -- the spots a Shift drag with a building armed lays.
 *
 * Separate from ingame.c so the rule needs no world.
 */

#include "tak_build_line.h"
#include "tak_commands.h"

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

/* num / den rounded half away from zero, den > 0. */
static int32_t round_div(int64_t num, int64_t den) {
    return (int32_t)(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den));
}

int BuildLine_Cells(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                    int fx, int fz, BuildLineCell *out, int max) {
    if (!out || max <= 0) return 0;
    if (fx < 1) fx = 1;
    if (fz < 1) fz = 1;
    int32_t dx = bx - ax, dy = by - ay;
    /* The longer axis in footprints, x on a tie. */
    int x_major = (int64_t)iabs32(dx) * fz >= (int64_t)iabs32(dy) * fx;
    int32_t dmaj = x_major ? dx : dy, dmin = x_major ? dy : dx;
    int32_t fmaj = x_major ? fx : fz, fmin = x_major ? fz : fx;
    /* The spot nearest the release ends the line. */
    int64_t n = ((int64_t)iabs32(dmaj) + fmaj / 2) / fmaj;
    /* A step on the other axis is at most a footprint, so no gap opens. */
    if (dmin > n * fmin) dmin = (int32_t)(n * fmin);
    if (dmin < -n * fmin) dmin = (int32_t)(-n * fmin);
    int32_t sgn = dmaj < 0 ? -1 : 1;
    int count = 0;
    for (int64_t i = 0; i <= n && count < max; i++) {
        int32_t maj = (int32_t)(sgn * i * fmaj);
        int32_t mn = n > 0 ? round_div((int64_t)dmin * i, n) : 0;
        out[count].x = ax + (x_major ? maj : mn);
        out[count].y = ay + (x_major ? mn : maj);
        count++;
    }
    return count;
}

uint16_t BuildLine_OrderArg(int front, int endless) {
    if (endless)
        return (uint16_t)(TAK_CMD_ARG_ENDLESS | (front ? 0u : TAK_CMD_ARG_QUEUE));
    return (uint16_t)(front ? TAK_CMD_ARG_FRONT : TAK_CMD_ARG_QUEUE);
}
