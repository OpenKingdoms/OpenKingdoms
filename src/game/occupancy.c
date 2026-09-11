/*
 * occupancy.c -- the dynamic unit occupancy layer.
 *
 * Structures stamp their yardmap into a per-16-px-cell grid; movement
 * and A* consult it on top of the static terrain passability cache.
 * The legacy engine keeps the same information in its map-cell record
 * (occupying unit id + gate-cell flag) and re-imprints whenever a
 * building appears, dies, or toggles its yard.
 */

#include "tak_occupancy.h"
#include "tak_world.h"
#include "tak_memory.h"

#include <string.h>

int Occ_TileOf(int32_t world_px) {
    if (world_px >= 0) return (int)(world_px / TAK_OCC_TILE_PX);
    return -(int)(((-world_px) + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX);
}

/* legacy:163223-163256. Unrecognised characters (row separators) are
 * skipped without consuming a cell, and the final character repeats to
 * fill a short string (legacy:163259-163262). */
static int yard_byte_for(char c) {
    switch (c) {
        case '.': return 0x00;
        case 'y': return 0x29;
        case 'Y': return 0x31;
        case 'c': return 0x2d;
        case 'C': return 0x35;
        case 'o': return 0x2f;
        case 'O': return 0x2b;
        case 'w': return 0x37;
        case 'S': return 0x8f;
        case 'f': return 0x6f;
        default:  return -1;
    }
}

uint8_t *Occ_BuildYardmap(const char *spec, int bmcode, int fx, int fz) {
    if (fx <= 0 || fz <= 0) return NULL;
    size_t n = (size_t)fx * (size_t)fz;
    uint8_t *map = (uint8_t *)tak_malloc(n);
    if (!map) return NULL;
    /* Mobile defs get a map that never blocks (legacy:163272-163292). */
    if (bmcode != 0) { memset(map, 0x29, n); return map; }
    /* A building with no yardmap key blocks its whole footprint. */
    if (!spec || !spec[0]) { memset(map, 0x2f, n); return map; }

    const char *p = spec;
    size_t i = 0;
    while (i < n && *p) {
        int b = yard_byte_for(*p);
        if (b < 0) { p++; continue; }
        map[i++] = (uint8_t)b;
        if (p[1] != '\0') p++;
    }
    while (i < n) map[i++] = 0x2f;
    return map;
}

int Occ_Ensure(struct GameWorld *w) {
    if (!w) return 0;
    if (w->occ) return 1;
    if (w->map_pixels_w <= 0 || w->map_pixels_h <= 0) return 0;
    int ow = (w->map_pixels_w + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int oh = (w->map_pixels_h + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    if (ow <= 0 || oh <= 0) return 0;
    w->occ = (TAK_OccCell *)tak_calloc((size_t)ow * (size_t)oh,
                                       sizeof(TAK_OccCell));
    if (!w->occ) return 0;
    w->occ_w = ow;
    w->occ_h = oh;
    return 1;
}

void Occ_Clear(struct GameWorld *w) {
    if (!w || !w->occ) return;
    w->occ_version++;
    memset(w->occ, 0,
           (size_t)w->occ_w * (size_t)w->occ_h * sizeof(TAK_OccCell));
}

void Occ_Free(struct GameWorld *w) {
    if (!w) return;
    if (w->occ) tak_free(w->occ);
    w->occ = NULL;
    w->occ_w = 0;
    w->occ_h = 0;
}

int Occ_ImprintStamp(struct GameWorld *w, const TAK_OccStamp *st, int on,
                     TAK_OccBusyFn busy, void *user) {
    if (!w || !w->occ || !st || !st->yard) return 1;
    int mask = TAK_OCC_MASK(st->yard_open);
    w->occ_version++;   /* structures changed: clearance maps go stale */
    uint16_t id = (uint16_t)(st->handle + 1);
    int complete = 1;
    for (int row = 0; row < st->fz; row++) {
        int ty = st->ty0 + row;
        if (ty < 0 || ty >= w->occ_h) continue;
        for (int col = 0; col < st->fx; col++) {
            int tx = st->tx0 + col;
            if (tx < 0 || tx >= w->occ_w) continue;
            TAK_OccCell *c = &w->occ[(size_t)ty * w->occ_w + tx];
            int b = st->yard[(size_t)row * st->fx + col];
            if (!on || !(b & mask)) {
                /* Clear only cells still ours (legacy:217989-217991). */
                if (c->unit_plus1 == id) {
                    c->unit_plus1 = 0;
                    c->owner = 0;
                    c->flags = 0;
                }
                continue;
            }
            /* A cell held by someone else is yielded and the imprint
             * retried, so a unit caught inside a finished structure is
             * never sealed in (legacy:217994-218006). */
            if (c->unit_plus1 && c->unit_plus1 != id) { complete = 0; continue; }
            if (busy && busy(user, tx, ty))           { complete = 0; continue; }
            c->unit_plus1 = id;
            c->owner = (uint8_t)st->owner;
            /* Gate defs tag every cell that blocks only when the yard is
             * closed (legacy:218012-218018). */
            c->flags = (uint8_t)((st->is_gate && (b & 6) == 4)
                                 ? TAK_OCC_GATE : 0);
        }
    }
    return complete;
}

int Occ_AnyBlockingCell(const TAK_OccStamp *st, int open,
                        TAK_OccBusyFn busy, void *user) {
    if (!st || !st->yard || !busy) return 0;
    int mask = TAK_OCC_MASK(open);
    for (int row = 0; row < st->fz; row++) {
        for (int col = 0; col < st->fx; col++) {
            if (!(st->yard[(size_t)row * st->fx + col] & mask)) continue;
            if (busy(user, st->tx0 + col, st->ty0 + row)) return 1;
        }
    }
    return 0;
}

int Occ_QueryTile(const struct GameWorld *w, int tx, int ty,
                  int player_id, int self_plus1) {
    if (!w || !w->occ) return 0;
    if (tx < 0 || ty < 0 || tx >= w->occ_w || ty >= w->occ_h) return 0;
    const TAK_OccCell *c = &w->occ[(size_t)ty * w->occ_w + tx];
    if (!c->unit_plus1) return 0;
    if (self_plus1 && c->unit_plus1 == (uint16_t)self_plus1) return 0;
    /* Own closed gate stays passable; a foreign one blocks
     * (legacy:21986-22032 reclassifies the cell by ownership). */
    if ((c->flags & TAK_OCC_GATE) && c->owner == (uint8_t)player_id) return 2;
    return 1;
}

int Occ_QueryWorld(const struct GameWorld *w, int32_t wx, int32_t wy,
                   int player_id, int self_plus1) {
    return Occ_QueryTile(w, Occ_TileOf(wx), Occ_TileOf(wy),
                         player_id, self_plus1);
}

int Occ_QueryTileStatic(const struct GameWorld *w, int tx, int ty,
                        int player_id) {
    if (!w || !w->occ) return 0;
    if (tx < 0 || ty < 0 || tx >= w->occ_w || ty >= w->occ_h) return 0;
    const TAK_OccCell *c = &w->occ[(size_t)ty * w->occ_w + tx];
    if (!c->unit_plus1) return 0;
    /* Mobiles are resolved at move time, never in the route plan. */
    if (c->flags & TAK_OCC_MOBILE) return 0;
    if ((c->flags & TAK_OCC_GATE) && c->owner == (uint8_t)player_id) return 2;
    return 1;
}

int Occ_QueryTilePlan(const struct GameWorld *w, int tx, int ty,
                      int player_id, int self_plus1) {
    if (!w || !w->occ) return 0;
    if (tx < 0 || ty < 0 || tx >= w->occ_w || ty >= w->occ_h) return 0;
    const TAK_OccCell *c = &w->occ[(size_t)ty * w->occ_w + tx];
    if (!c->unit_plus1) return 0;
    if (c->flags & TAK_OCC_MOBILE) {
        if (!(c->flags & TAK_OCC_PARKED)) return 0;
        if (self_plus1 && c->unit_plus1 == (uint16_t)self_plus1) return 0;
        return 1;
    }
    if ((c->flags & TAK_OCC_GATE) && c->owner == (uint8_t)player_id) return 2;
    return 1;
}

void Occ_SetMobileParked(struct GameWorld *w, int handle,
                         int tx, int ty, int fx, int fz, int parked) {
    if (!w || !w->occ) return;
    uint16_t id = (uint16_t)(handle + 1);
    for (int row = 0; row < fz; row++) {
        int cy = ty + row;
        if (cy < 0 || cy >= w->occ_h) continue;
        for (int col = 0; col < fx; col++) {
            int cx = tx + col;
            if (cx < 0 || cx >= w->occ_w) continue;
            TAK_OccCell *c = &w->occ[(size_t)cy * w->occ_w + cx];
            if (c->unit_plus1 != id || !(c->flags & TAK_OCC_MOBILE)) continue;
            if (parked) c->flags |= TAK_OCC_PARKED;
            else        c->flags &= (uint8_t)~TAK_OCC_PARKED;
        }
    }
}

int Occ_IsGateTile(const struct GameWorld *w, int tx, int ty) {
    if (!w || !w->occ) return 0;
    if (tx < 0 || ty < 0 || tx >= w->occ_w || ty >= w->occ_h) return 0;
    const TAK_OccCell *c = &w->occ[(size_t)ty * w->occ_w + tx];
    return (c->unit_plus1 != 0) && (c->flags & TAK_OCC_GATE) != 0;
}

/* ── Mobile footprints ────────────────────────────────────────────────
 *
 * Legacy imprints a moving unit's whole footprint rect into the same
 * map cells it uses for buildings (legacy:217933-217971), with no
 * yardmap mask. That per-cell occupant id is what stops two units
 * settling on the same ground: the move step refuses a cell another
 * unit holds (legacy:219329-219340). Cost here is O(footprint) and
 * only on the ticks a unit actually changes cell. */

void Occ_LiftMobile(struct GameWorld *w, int handle,
                    int tx, int ty, int fx, int fz) {
    if (!w || !w->occ) return;
    uint16_t id = (uint16_t)(handle + 1);
    for (int row = 0; row < fz; row++) {
        int cy = ty + row;
        if (cy < 0 || cy >= w->occ_h) continue;
        for (int col = 0; col < fx; col++) {
            int cx = tx + col;
            if (cx < 0 || cx >= w->occ_w) continue;
            TAK_OccCell *c = &w->occ[(size_t)cy * w->occ_w + cx];
            if (c->unit_plus1 != id) continue;
            c->unit_plus1 = 0;
            c->owner = 0;
            c->flags = 0;
        }
    }
}

int Occ_MoveMobile(struct GameWorld *w, int handle, int owner,
                   int had_old, int old_tx, int old_ty,
                   int tx, int ty, int fx, int fz) {
    if (!w || !w->occ) return 0;
    if (had_old && old_tx == tx && old_ty == ty) return 1;
    if (had_old) Occ_LiftMobile(w, handle, old_tx, old_ty, fx, fz);
    uint16_t id = (uint16_t)(handle + 1);
    int complete = 1;
    for (int row = 0; row < fz; row++) {
        int cy = ty + row;
        if (cy < 0 || cy >= w->occ_h) continue;
        for (int col = 0; col < fx; col++) {
            int cx = tx + col;
            if (cx < 0 || cx >= w->occ_w) continue;
            TAK_OccCell *c = &w->occ[(size_t)cy * w->occ_w + cx];
            /* Never evict: a structure or another unit keeps its cell
             * and this one simply is not recorded there. */
            if (c->unit_plus1 && c->unit_plus1 != id) { complete = 0; continue; }
            c->unit_plus1 = id;
            c->owner = (uint8_t)owner;
            c->flags = TAK_OCC_MOBILE;
        }
    }
    return complete;
}
