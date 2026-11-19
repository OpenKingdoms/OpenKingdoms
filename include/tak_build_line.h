#ifndef TAK_BUILD_LINE_H
#define TAK_BUILD_LINE_H

#include <stdint.h>

/*
 * The line of buildings a Shift drag lays with a building armed. Pure,
 * so the rule needs no world.
 */

/* The most a drag lays: what a builder holds, the order in hand and
 * its 32 queued (UNIT_MOVE_LEGS_MAX). */
#define BUILD_LINE_MAX 33

typedef struct BuildLineCell {
    int32_t x, y;
} BuildLineCell;

/* Top left cells of fx by fz footprints laid end to end, with no gap at
 * any angle, from cell (ax, ay) toward (bx, by). Returns the count. */
int BuildLine_Cells(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                    int fx, int fz, BuildLineCell *out, int max);

/* Each spot's build arg at the default facing: behind the last for Shift,
 * ahead of all for Ctrl+Shift. A summons without end sends what its click
 * would, Ctrl dropping Shift (legacy:39177-39180). */
uint16_t BuildLine_OrderArg(int front, int endless);

#endif
