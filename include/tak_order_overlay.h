/*
 * tak_order_overlay.h -- the orders a selection holds, drawn while Shift
 * is down.
 *
 * With Shift held the classic view draws a line from each selected unit
 * of the local player's through the orders it holds, a marker at each,
 * and a translucent ghost for each building it has queued. Nothing is
 * drawn without Shift. It reads the orders through Units_OrdersOf and
 * writes nothing the simulation keeps.
 */
#ifndef TAK_ORDER_OVERLAY_H
#define TAK_ORDER_OVERLAY_H

#include <stdint.h>

struct GameWorld;
struct TAK_Platform;

/* OrderStop.mark: what is drawn at a stop. */
#define ORDER_MARK_NONE    0   /* the line only: a patrol route closing */
#define ORDER_MARK_MOVE    1
#define ORDER_MARK_ATTACK  2   /* on the target unit, or on the ground */
#define ORDER_MARK_PATROL  3
#define ORDER_MARK_RALLY   4   /* where a factory's products gather */
#define ORDER_MARK_BUILD   5
#define ORDER_MARK_ASSIST  6   /* guard or heal */
#define ORDER_MARK_RECLAIM 7   /* reclaim or sweep */
#define ORDER_MARK_ACTION  8   /* raise, load or unload */
#define ORDER_MARK_KINDS   9

/* One stop on a unit's route. The line runs from the unit through every
 * stop in turn. */
typedef struct OrderStop {
    uint8_t mark;      /* ORDER_MARK_* */
    uint8_t queued;    /* behind the order in hand */
    uint8_t ghost;     /* a queued building, drawn as its ghost */
    uint8_t facing;    /* BUILD: quarter turns clockwise */
    int16_t def;       /* BUILD: the building, else -1 */
    int32_t x, y;      /* flat world pixels */
    int32_t target;    /* the unit the marker sits on, -1 for none */
} OrderStop;

/* Room for any unit's route: the order in hand, a transport's pickups,
 * the queued legs and a patrol's way back. */
#define ORDER_OVERLAY_STOPS_MAX 64

/* The stops for one unit, from its order in hand to its last queued
 * one. A patrol route ends back where it started and nothing queued
 * after it is drawn, since it is never reached. A factory's rally comes
 * first and its standing orders after. A target the local player cannot
 * see gives no position away. Writes up to cap and returns how many
 * there are. */
int  OrderOverlay_Plan(int handle, OrderStop *out, int cap);

/* Whether Shift is down for the battle screen this frame. */
void OrderOverlay_SetShift(int held);

/* Draw the routes of the local player's selected units, in the classic
 * view's projection. Returns how many stops were drawn, 0 without
 * Shift. */
int  OrderOverlay_Draw(const struct GameWorld *world,
                       struct TAK_Platform *plat);

#endif /* TAK_ORDER_OVERLAY_H */
