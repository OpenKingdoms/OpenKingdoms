/*
 * shot_path.c -- the per cell test a shot in flight runs.
 *
 * The original's test (legacy:245377-245476) takes the shot's cell and
 * height and stops it on the first thing it meets: a unit of another
 * player on the ground or in the air, then a feature taller than the
 * shot, the sea, or the ground itself. The unit layer supplies the
 * bodies through callbacks so this module needs only the map.
 */

#include "tak_shot_path.h"
#include "tak_world.h"
#include "tak_occupancy.h"
#include "tak_features.h"

int ShotPath_CellFloor(const struct GameWorld *w, int32_t x, int32_t y,
                       int *out_floor) {
    if (!w) return 0;
    const TNTFile *t = &w->tnt;
    if (!t->heightmap || t->height_w <= 1 || t->height_h <= 1) return 0;
    if (x < 0 || y < 0) return 0;
    int cx = x / 16, cz = y / 16;
    if (cx >= t->height_w - 1 || cz >= t->height_h - 1) return 0;
    const uint8_t *hm = t->heightmap + cz * t->height_w + cx;
    int lo = hm[0];
    if (hm[1] < lo) lo = hm[1];
    if (hm[t->height_w] < lo) lo = hm[t->height_w];
    if (hm[t->height_w + 1] < lo) lo = hm[t->height_w + 1];
    if (out_floor) *out_floor = lo;
    return 1;
}

int ShotPath_Substeps(int32_t dx, int32_t dy) {
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    int32_t m = ax > ay ? ax : ay;
    int n = (int)((m + 15) / 16);
    return n > 0 ? n : 1;
}

int ShotPath_Test(struct GameWorld *w, int32_t x, int32_t y, int h,
                  int owner, unsigned flags, const TAK_ShotBodies *bodies,
                  int *out_unit) {
    if (out_unit) *out_unit = -1;
    if (!w || !w->tnt.heightmap || w->tnt.height_w <= 1 || w->tnt.height_h <= 1)
        return TAK_SHOT_FLY;
    int ground = 0;
    /* Off the map the shot is gone (legacy:245393-245396). */
    if (!ShotPath_CellFloor(w, x, y, &ground)) return TAK_SHOT_OFFMAP;
    int tx = x / 16, ty = y / 16;

    /* The unit on the ground here, when it is not the shooter's own
     * (legacy:245414-245424, the owner compare at :245419). */
    if (bodies && bodies->ground && w->occ &&
        tx < w->occ_w && ty < w->occ_h) {
        const TAK_OccCell *c = &w->occ[ty * w->occ_w + tx];
        if (c->unit_plus1 &&
            bodies->ground(bodies->user, (int)c->unit_plus1 - 1, owner, h)) {
            if (out_unit) *out_unit = (int)c->unit_plus1 - 1;
            return TAK_SHOT_UNIT;
        }
    }
    /* A flyer whose body holds the shot's height (legacy:245426-245437). */
    if (bodies && bodies->air) {
        int a = bodies->air(bodies->user, x, y, h, owner);
        if (a >= 0) {
            if (out_unit) *out_unit = a;
            return TAK_SHOT_UNIT;
        }
    }
    /* unitsonly stops here (legacy:245440-245443). */
    if (flags & TAK_SHOT_UNITS_ONLY) return TAK_SHOT_FLY;

    /* A feature, as tall as its height byte over the cell floor
     * (legacy:245444-245461). */
    int top = Features_TopAt(w, tx, ty);
    if (top > 0 && h <= ground + (top - 1)) return TAK_SHOT_FEATURE;

    if (h > ground) {
        /* Above the ground and under the sea, unless the weapon flies
         * under water or the map says the sea does not stop shots
         * (legacy:245463-245468). */
        if (!(flags & TAK_SHOT_WATER_WEAPON) && !w->no_sea_level_trigger &&
            h < w->water_height)
            return TAK_SHOT_WATER;
        return TAK_SHOT_FLY;
    }
    /* At or under the ground (legacy:245470-245476). */
    if (flags & TAK_SHOT_GROUND_BOUNCE) return TAK_SHOT_BOUNCE;
    return TAK_SHOT_GROUND;
}
