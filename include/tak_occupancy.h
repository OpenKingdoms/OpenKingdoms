#ifndef TAK_OCCUPANCY_H
#define TAK_OCCUPANCY_H

#include "tak_types.h"

struct GameWorld;

/* ── Unit occupancy layer ─────────────────────────────────────────────
 *
 * The dynamic half of passability. Terrain (slope, water, features) is
 * static and cached per move class in pathing.c; this layer is what
 * structures stamp on top of it, and it changes whenever a building
 * finishes, dies, or opens/closes its yard.
 *
 * One record per 16-px map cell, mirroring the legacy map-cell record
 * that stores the occupying unit id plus a gate-cell flag
 * (legacy:219329, legacy:218012-218018). */

#define TAK_OCC_TILE_PX 16
#define TAK_OCC_GATE    0x01   /* legacy cell gate-cell tag, :218012 */
/* A mobile unit's own footprint (legacy:217933-217971). The mover
 * refuses to step onto one; the planner ignores them, because the
 * legacy pathfinder cost map only ever learns about structures
 * (Terrain_SetBlockedFlag from the yard setter, legacy:219056). */
#define TAK_OCC_MOBILE  0x02

typedef struct TAK_OccCell {
    uint16_t unit_plus1;   /* imprinting unit handle + 1; 0 = free */
    uint8_t  owner;        /* player_id of that unit */
    uint8_t  flags;        /* TAK_OCC_* */
} TAK_OccCell;

/* One structure's footprint imprint. Built by the unit layer so this
 * module never needs to know about the unit array. */
typedef struct TAK_OccStamp {
    int            tx0, ty0;  /* top-left occupancy tile of the footprint */
    int            fx, fz;    /* footprint size in tiles */
    const uint8_t *yard;      /* fz rows of fx yardmap bytes */
    int            yard_open;
    int            is_gate;
    int            handle;    /* unit handle */
    int            owner;     /* player_id */
} TAK_OccStamp;

/* "Is this tile held by a live occupant the imprint must yield to?" */
typedef int (*TAK_OccBusyFn)(void *user, int tx, int ty);

/* A yardmap byte blocks when bit 0x02 is set and the yard is open, or
 * bit 0x04 is set and it is closed (legacy:217987-217988). */
#define TAK_OCC_MASK(open) ((open) ? 0x02 : 0x04)

/* Parse an FBI yardmap into one byte per footprint cell
 * (legacy:163207-163293). bmcode != 0 (mobile) yields an all-0x29 map
 * that never blocks. Returns a heap block of fx*fz bytes, or NULL. */
uint8_t *Occ_BuildYardmap(const char *spec, int bmcode, int fx, int fz);

int  Occ_Ensure(struct GameWorld *w);
void Occ_Clear(struct GameWorld *w);
void Occ_Free(struct GameWorld *w);

/* Imprint (on=1) or lift (on=0) a stamp. Returns 1 when every blocking
 * cell was claimed, 0 when at least one was yielded and the caller
 * should retry (legacy:217994-218006). */
int  Occ_ImprintStamp(struct GameWorld *w, const TAK_OccStamp *st, int on,
                      TAK_OccBusyFn busy, void *user);

/* Would any cell that blocks under `open` be held by a live occupant?
 * Drives the refusable SET YARD_OPEN (legacy:218984-219042). */
int  Occ_AnyBlockingCell(const TAK_OccStamp *st, int open,
                         TAK_OccBusyFn busy, void *user);

/* 0 free, 1 blocked, 2 own gate cell (passable for its owner,
 * legacy:21986-22032). self_plus1 of 0 means "no self". */
int  Occ_QueryTile(const struct GameWorld *w, int tx, int ty,
                   int player_id, int self_plus1);
int  Occ_QueryWorld(const struct GameWorld *w, int32_t wx, int32_t wy,
                    int player_id, int self_plus1);

/* Same, but ignoring mobile occupants. What route planning uses. */
int  Occ_QueryTileStatic(const struct GameWorld *w, int tx, int ty,
                         int player_id);

/* Move a mobile unit's footprint stamp from (old_tx, old_ty) to
 * (tx, ty). Clears only cells still held by this unit and claims the
 * free ones, so a crowd never loses another unit's mark. Returns 1 if
 * every new cell was claimed. */
int  Occ_MoveMobile(struct GameWorld *w, int handle, int owner,
                    int had_old, int old_tx, int old_ty,
                    int tx, int ty, int fx, int fz);
void Occ_LiftMobile(struct GameWorld *w, int handle,
                    int tx, int ty, int fx, int fz);

/* Is this tile tagged as a gate cell, whoever owns it? */
int  Occ_IsGateTile(const struct GameWorld *w, int tx, int ty);

/* Floor-division of a world pixel to an occupancy tile. */
int  Occ_TileOf(int32_t world_px);

#endif /* TAK_OCCUPANCY_H */
