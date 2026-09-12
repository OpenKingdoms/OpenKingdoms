/*
 * command_queue.c: the one road from a player action to the
 * simulation.
 *
 * Nothing applies an order the moment it is clicked any more. A
 * command is queued for a tick and runs when that tick comes round,
 * in an order eight machines agree on: seat first, then arrival.
 */

#include "tak_command_queue.h"

#include "tak_battle_config.h"
#include "tak_command_exec.h"

#include <string.h>

typedef struct QueueEntry {
    TAK_GameCommand cmd;
    uint32_t        arrival;   /* the order it was handed in */
    uint8_t         used;
} QueueEntry;

static QueueEntry g_queue[TAK_CMD_QUEUE_MAX];
static int        g_queue_count;
static uint32_t   g_queue_arrival;
static uint32_t   g_queue_tick;
static uint32_t   g_queue_delay;
static int        g_queue_applied;
static TAK_GameCommand g_queue_last;
static TAK_CmdQueueObserver g_queue_observer;
static void *g_queue_observer_user;

void TAK_CmdQueue_SetObserver(TAK_CmdQueueObserver fn, void *user) {
    g_queue_observer = fn;
    g_queue_observer_user = user;
}

void TAK_CmdQueue_Reset(uint32_t delay_ticks) {
    memset(g_queue, 0, sizeof(g_queue));
    memset(&g_queue_last, 0, sizeof(g_queue_last));
    g_queue_count = 0;
    g_queue_arrival = 0;
    g_queue_tick = 0;
    g_queue_delay = delay_ticks;
    g_queue_applied = 0;
}

uint32_t TAK_CmdQueue_Delay(void)            { return g_queue_delay; }
void     TAK_CmdQueue_SetDelay(uint32_t d)   { g_queue_delay = d; }
uint32_t TAK_CmdQueue_Tick(void)             { return g_queue_tick; }
int      TAK_CmdQueue_AppliedCount(void)     { return g_queue_applied; }
const TAK_GameCommand *TAK_CmdQueue_LastApplied(void) { return &g_queue_last; }

static int queue_push(const TAK_GameCommand *cmd) {
    if (!cmd) return -1;
    if (!TAK_CommandTypeIsValid(cmd->type)) return -1;
    if (cmd->unit_count > TAK_COMMAND_MAX_UNITS) return -1;
    if (cmd->seat < 1 || cmd->seat > TAK_MAX_PLAYERS) return -1;
    if (g_queue_count >= TAK_CMD_QUEUE_MAX) return -1;
    for (int i = 0; i < TAK_CMD_QUEUE_MAX; i++) {
        if (g_queue[i].used) continue;
        g_queue[i].cmd = *cmd;
        g_queue[i].arrival = g_queue_arrival++;
        g_queue[i].used = 1;
        g_queue_count++;
        return 0;
    }
    return -1;
}

int TAK_CmdQueue_Submit(uint8_t seat, const TAK_GameCommand *cmd) {
    if (!cmd) return -1;
    TAK_GameCommand copy = *cmd;
    copy.seat = seat;
    copy.tick = g_queue_tick + g_queue_delay;
    return queue_push(&copy);
}

int TAK_CmdQueue_SubmitAt(const TAK_GameCommand *cmd) {
    if (!cmd) return -1;
    /* A tick already gone by would never run, so it lands on the next
     * one instead of vanishing. */
    TAK_GameCommand copy = *cmd;
    if (copy.tick < g_queue_tick) copy.tick = g_queue_tick;
    return queue_push(&copy);
}

int TAK_CmdQueue_Pending(void) {
    return g_queue_count;
}

int TAK_CmdQueue_Run(void) {
    int applied = 0;
    /* Seat by seat, and inside a seat in the order the commands were
     * handed in. Slot order in the array is not an order two machines
     * would agree on, so it is never used. */
    for (int seat = 1; seat <= TAK_MAX_PLAYERS; seat++) {
        for (;;) {
            int best = -1;
            for (int i = 0; i < TAK_CMD_QUEUE_MAX; i++) {
                if (!g_queue[i].used) continue;
                if (g_queue[i].cmd.seat != (uint8_t)seat) continue;
                if (g_queue[i].cmd.tick != g_queue_tick) continue;
                if (best < 0 || g_queue[i].arrival < g_queue[best].arrival) {
                    best = i;
                }
            }
            if (best < 0) break;
            g_queue_last = g_queue[best].cmd;
            g_queue[best].used = 0;
            g_queue_count--;
            if (g_queue_observer) {
                g_queue_observer(&g_queue_last, g_queue_observer_user);
            }
            TAK_CommandExec_Apply(&g_queue_last);
            g_queue_applied++;
            applied++;
        }
    }
    g_queue_tick++;
    return applied;
}
