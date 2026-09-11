/*
 * cob_vm.c — COB virtual machine (Phase D M2).
 *
 * Per-unit script executor. Dispatch is a switch over the opcode
 * (0x100XX000-encoded). Each thread runs cooperatively, yielding on
 * SLEEP/WAIT/RETURN/unknown.
 *
 * M2 implements just enough opcodes to get monarchs' Create scripts
 * to RETURN cleanly. Unknown opcodes log once and terminate the
 * thread (rather than guessing operand counts and corrupting the PC).
 * M4-M5 fill in the rest.
 */

#include "tak_cob_vm.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Opcode constants ───────────────────────────────────────────────
 *
 * Verified against the legacy interpreter dispatch at
 * legacy:306300-307004 (opType = opcode & 0x100ff000). Names
 * follow BOS conventions (cross-checked against the independent kbot
 * toolkit's formats/scripting/opcodes.go, which agrees
 * with the legacy reference on every animation/thread opcode; the TA:K math
 * block 0x10037000-0x1003b000 differs from kbot's TA table and follows
 * the legacy reference: XOR, NOT, SHL, SAR, MOD). */
#define OP_MOVE            0x10001000  /* piece,axis inline; pop target,speed  */
#define OP_TURN            0x10002000  /* piece,axis inline; pop target,speed  */
#define OP_SPIN            0x10003000  /* piece,axis inline; pop speed,accel   */
#define OP_STOP_SPIN       0x10004000  /* piece,axis inline; pop decel         */
#define OP_SHOW            0x10005000
#define OP_HIDE            0x10006000
/* 0x10007000 / 0x10008000 are render-optimization hints (the legacy reference
 * :306419-306426): host[0xc](piece, 1/0). No piece-cache subsystem here,
 * so they're no-ops — but the piece operand MUST be consumed or the PC
 * drifts (the verlode upside-down bug). */
#define OP_CACHE_PIECE     0x10007000
#define OP_DONT_CACHE_PIECE 0x10008000
#define OP_SHADOW          0x10009000  /* host[0x14](piece, 1)  :306327 */
#define OP_DONT_SHADOW     0x1000a000  /* host[0x14](piece, 0)  :306437 */
#define OP_MOVE_NOW        0x1000b000  /* piece,axis inline; pop pos    :306441 */
#define OP_TURN_NOW        0x1000c000  /* piece,axis inline; pop angle  :306453 */
#define OP_SHADE           0x1000d000  /* host[0x10](piece, 1)  :306430 */
#define OP_DONT_SHADE      0x1000e000  /* host[0x10](piece, 0)  :306468 */
#define OP_EMIT_SFX        0x1000f000  /* piece inline; pop sfx type    :306486 */
#define OP_WAIT_FOR_TURN   0x10011000  /* piece,axis inline; block      :306474 */
#define OP_WAIT_FOR_MOVE   0x10012000  /* piece,axis inline; block      :306316 */
#define OP_SLEEP           0x10013000  /* pop ms; ticks = rate*ms/1000  :306522 */
#define OP_PUSH_CONSTANT   0x10021000  /* &7: 1=const 2=local 4=static  :306535 */
#define OP_ALLOC_LOCAL     0x10022000  /* sp++                          :306531 */
#define OP_POP_VAR         0x10023000  /* &7: 2=local 4=static          :306505 */
#define OP_POP_STACK       0x10024000
#define OP_ADD             0x10031000
#define OP_SUB             0x10032000
#define OP_MUL             0x10033000
#define OP_DIV             0x10034000
#define OP_AND_BIT         0x10035000
#define OP_OR_BIT          0x10036000
#define OP_XOR_BIT         0x10037000  /* :306576 */
#define OP_NOT_BIT         0x10038000  /* :306611 */
#define OP_SHL             0x10039000  /* :306617 */
#define OP_SHR             0x1003a000  /* arithmetic >>                 :306626 */
#define OP_MOD             0x1003b000  /* :306306 */
#define OP_RAND            0x10041000
#define OP_GET_UNIT_VALUE  0x10042000  /* pop port; host[0x54](p,0,0,0,0) :306675 */
#define OP_GET_WITH_ARGS   0x10043000  /* pop 5; host[0x54](p,a,b,c,d)  :306685 */
#define OP_GET_HOST_QUERY  0x10044000  /* pop 1; host[0x58](x); push    :306655 */
#define OP_GET_HOST_QUERY0 0x10045000  /* host[0x5c](); push            :306706 */
#define OP_LT              0x10051000
#define OP_LE              0x10052000
#define OP_GT              0x10053000
#define OP_GE              0x10054000
#define OP_EQ              0x10055000
#define OP_NE              0x10056000
#define OP_LAND            0x10057000
#define OP_LOR             0x10058000
#define OP_LXOR            0x10059000
#define OP_LNOT            0x1005a000
#define OP_START_SCRIPT    0x10061000  /* fn,nargs inline; async        :306799 */
#define OP_CALL_SCRIPT     0x10062000  /* fn,nargs inline; caller blocks :306962 */
#define OP_HOST_ARGS_63    0x10063000  /* ?,count inline; pops count    :306983 */
#define OP_JUMP            0x10064000  /* pc = inline                   :306998 */
#define OP_RETURN          0x10065000  /* end thread; wake callers      :306943 */
#define OP_JUMP_IF_FALSE   0x10066000  /* pop; jump when zero           :306937 */
#define OP_SIGNAL          0x10067000  /* pop mask; kill matching       :306904 */
#define OP_SET_SIGNAL_MASK 0x10068000  /* pop -> thread mask            :306905 */
#define OP_EXPLODE         0x10071000  /* piece inline; pop type        :306894 */
#define OP_PLAY_SOUND      0x10072000  /* name-idx inline; pop arg; push :306851 */
#define OP_MISSION_COMMAND 0x10073000  /* name-idx,argc inline; pop argc; push :306862 */
#define OP_SOUND_CMD_74    0x10074000  /* 2 inline (arg, name-idx)      :306885 */
#define OP_HOST_MARK_75    0x10075000  /* 1 inline; host[0x44]          :306845 */
#define OP_SET_UNIT_VALUE  0x10082000  /* pop value,port; host[0x50]    :306819 */
#define OP_ATTACH_UNIT     0x10083000  /* pop 3; host[0x48]             :306827 */
#define OP_DROP_UNIT       0x10084000  /* pop 1; host[0x4c]             :306838 */

/* ── Tracing ───────────────────────────────────────────────────────── */

/* Set with Cob_SetTrace(1) for verbose per-opcode logging. Off in
 * production. */
static int g_trace = 0;
void Cob_SetTrace(int on) { g_trace = on; }

/* Log unknown opcodes once per (engine, opcode) pair to avoid spam.
 * Keyed loosely — across engines just track values seen. 256 slots is
 * enough for all 51 opcodes plus headroom. */
static uint32_t g_unknown_seen[256];
static int      g_unknown_count = 0;
static int      g_unknown_total = 0;
static void terminate_thread(CobEngine *e, int slot, const char *why);
static int already_logged_unknown(uint32_t op) {
    for (int i = 0; i < g_unknown_count; i++) {
        if (g_unknown_seen[i] == op) return 1;
    }
    if (g_unknown_count < 256) g_unknown_seen[g_unknown_count++] = op;
    return 0;
}

void Cob_ResetDiagnostics(void) {
    memset(g_unknown_seen, 0, sizeof(g_unknown_seen));
    g_unknown_count = 0;
    g_unknown_total = 0;
}

int Cob_GetUnknownOpcodeCount(void) {
    return g_unknown_total;
}

/* ── Engine init / teardown ───────────────────────────────────────── */

/* Case-insensitive string compare for piece-name binding. */
static int cob_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int Cob_EngineInit(CobEngine *e, const CobScript *script,
                    int node_count, const char *const *node_names) {
    if (!e || !script || node_count < 0) return -1;
    if (node_count > 0 && !node_names) return -1;
    memset(e, 0, sizeof(*e));
    e->script = script;
    e->piece_count = node_count;

    if (e->piece_count > 0) {
        e->pieces = (CobPiece *)tak_malloc(sizeof(CobPiece) * e->piece_count);
        if (!e->pieces) return -1;
        memset(e->pieces, 0, sizeof(CobPiece) * e->piece_count);
        /* Pieces start shown. Only a script HIDE takes one off screen,
         * there is no naming rule (legacy:198762-198765). */
    }

    /* Build piece_to_node[] by matching script piece names against
     * the supplied node_names[] (case-insensitive). Unmatched script
     * pieces map to -1; VM ops on those are silently ignored. */
    if (script->num_pieces > 0) {
        e->piece_to_node = (int16_t *)tak_malloc(sizeof(int16_t) * script->num_pieces);
        if (!e->piece_to_node) {
            if (e->pieces) tak_free(e->pieces);
            return -1;
        }
        int unbound = 0;
        for (int p = 0; p < script->num_pieces; p++) {
            int found = -1;
            for (int n = 0; n < node_count; n++) {
                if (cob_stricmp(script->piece_names[p], node_names[n]) == 0) {
                    found = n;
                    break;
                }
            }
            e->piece_to_node[p] = (int16_t)found;
            if (found < 0) unbound++;
        }
        if (unbound > 0) {
            fprintf(stderr, "Cob_EngineInit: %d/%u script pieces unbound\n",
                    unbound, script->num_pieces);
        }
    }

    if (script->num_static_vars > 0) {
        e->static_vars = (int32_t *)tak_malloc(sizeof(int32_t) * script->num_static_vars);
        if (!e->static_vars) {
            if (e->pieces)        tak_free(e->pieces);
            if (e->piece_to_node) tak_free(e->piece_to_node);
            return -1;
        }
        memset(e->static_vars, 0, sizeof(int32_t) * script->num_static_vars);
    }
    return 0;
}

void Cob_EngineFree(CobEngine *e) {
    if (!e) return;
    if (e->pieces)        tak_free(e->pieces);
    if (e->piece_to_node) tak_free(e->piece_to_node);
    if (e->static_vars)   tak_free(e->static_vars);
    e->pieces = NULL;
    e->piece_to_node = NULL;
    e->static_vars = NULL;
    e->script = NULL;
    e->active_thread_count = 0;
}

/* ── Thread management ────────────────────────────────────────────── */

static int alloc_thread_slot(CobEngine *e) {
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) {
        if (!e->threads[i].alive) return i;
    }
    return -1;
}

int Cob_StartThread(CobEngine *e, int script_idx,
                     const int32_t *args, int n_args) {
    if (!e || !e->script) return -1;
    if (script_idx < 0 || script_idx >= e->script->num_scripts) return -1;
    if (n_args < 0 || n_args > COB_THREAD_STACK_DEPTH) return -1;

    int slot = alloc_thread_slot(e);
    if (slot < 0) {
        fprintf(stderr, "Cob: no free thread slots (script '%s')\n",
                e->script->script_names[script_idx]);
        return -1;
    }
    CobThread *t = &e->threads[slot];
    memset(t, 0, sizeof(*t));
    t->alive = 1;
    t->wait_piece = -1;
    t->wait_child = -1;
    t->wait_kind = COB_WAIT_NONE;
    t->pc = e->script->script_offsets[script_idx];
    /* Push args (caller's args[0] becomes deepest on stack). Scripts
     * usually pop in reverse order, so the LAST argument winds up at
     * the bottom of the new pushes — this matches TA convention. */
    for (int i = 0; i < n_args; i++) {
        t->stack[t->sp++] = args[i];
    }
    e->active_thread_count++;
    if (g_trace) {
        fprintf(stderr, "Cob: start thread %d, script[%d]='%s' @pc=%u, %d args\n",
                slot, script_idx, e->script->script_names[script_idx],
                t->pc, n_args);
    }
    return slot;
}

int Cob_StartThreadByName(CobEngine *e, const char *name,
                           const int32_t *args, int n_args) {
    if (!e) return -1;
    int idx = Cob_FindScript(e->script, name);
    if (idx < 0) return -1;
    return Cob_StartThread(e, idx, args, n_args);
}

/* ── Animator pass ────────────────────────────────────────────────── */

/* Shortest signed angular delta from `from` to `to` in TA angle units
 * (65536 = full circle). Result in [-0x8000, 0x7fff] — the same wrap
 * decision the TURN opcode makes (legacy:306383-306390). */
static int32_t angle_delta(int32_t from, int32_t to) {
    return (int32_t)(((to - from + 0x8000) & COB_ANGLE_MASK) - 0x8000);
}

void Cob_AnimatePieces(CobEngine *e) {
    if (!e || !e->pieces) return;
    const int np = e->piece_count;   /* node-indexed; mesh->node_count */
    for (int i = 0; i < np; i++) {
        CobPiece *p = &e->pieces[i];
        for (int a = 0; a < 3; a++) {
            if (p->rot_target[a] == COB_ROT_SPINNING) {
                /* SPIN: accelerate current speed toward spin_speed by
                 * |spin_accel| per tick, then advance with wraparound.
                 * Legacy slots: +0x28 current, +0x34 target, +0x40
                 * accel (legacy:306337-306352, :306403). */
                if (p->rot_speed[a] != p->spin_speed[a]) {
                    int32_t acc = p->spin_accel[a];
                    if (acc < 0) acc = -acc;
                    if (acc == 0 ||
                        (p->rot_speed[a] < p->spin_speed[a] &&
                         p->rot_speed[a] + acc >= p->spin_speed[a]) ||
                        (p->rot_speed[a] > p->spin_speed[a] &&
                         p->rot_speed[a] - acc <= p->spin_speed[a])) {
                        p->rot_speed[a] = p->spin_speed[a];
                    } else if (p->rot_speed[a] < p->spin_speed[a]) {
                        p->rot_speed[a] += acc;
                    } else {
                        p->rot_speed[a] -= acc;
                    }
                }
                if (p->rot_speed[a] != 0) {
                    p->rot[a] = (p->rot[a] + p->rot_speed[a]) & COB_ANGLE_MASK;
                }
            } else if (p->rot_speed[a] != 0) {
                /* TURN: legacy stepping (CobEngine_AnimateObjects,
                 * legacy:307076-307111) — advance in the
                 * speed's sign direction and measure the REMAINING
                 * distance to the target ALONG that direction with
                 * mod-65536 wrap. (A shortest-way snap test reverses
                 * or clips commanded long-way swings — walk/build
                 * loops looked erratic under it.) Snap + stop when
                 * one step covers the remainder. */
                int32_t cur    = p->rot[a] & COB_ANGLE_MASK;
                int32_t target = p->rot_target[a] & COB_ANGLE_MASK;
                int32_t step   = p->rot_speed[a];
                int32_t remain = (step < 0)
                               ? ((cur - target) & COB_ANGLE_MASK)
                               : ((target - cur) & COB_ANGLE_MASK);
                int32_t mag = step < 0 ? -step : step;
                if (mag < remain) {
                    p->rot[a] = (cur + step) & COB_ANGLE_MASK;
                } else {
                    p->rot[a] = target;
                    p->rot_speed[a] = 0;
                }
            }
            /* Translation: linear step with clamp at target. */
            if (p->pos_speed[a] != 0) {
                int32_t next = p->pos[a] + p->pos_speed[a];
                if (p->pos_speed[a] > 0) {
                    if (next >= p->pos_target[a]) {
                        next = p->pos_target[a];
                        p->pos_speed[a] = 0;
                    }
                } else {
                    if (next <= p->pos_target[a]) {
                        next = p->pos_target[a];
                        p->pos_speed[a] = 0;
                    }
                }
                p->pos[a] = next;
            }
        }
    }
}

/* Check whether a thread's wait condition is satisfied; if so, clear it.
 * wait_piece is stored as a node index (translated when the WAIT opcode
 * ran). COB_WAIT_CHILD is cleared by RETURN/terminate of the child, not
 * here. */
static void check_thread_wait(CobEngine *e, CobThread *t) {
    if (t->wait_kind == COB_WAIT_NONE || t->wait_kind == COB_WAIT_CHILD)
        return;
    if (t->wait_piece < 0 || t->wait_piece >= e->piece_count) {
        t->wait_kind = COB_WAIT_NONE;
        return;
    }
    const CobPiece *p = &e->pieces[t->wait_piece];
    int moving;
    if (t->wait_kind == COB_WAIT_TURN) {
        moving = (p->rot_speed[t->wait_axis] != 0);
    } else {
        moving = (p->pos_speed[t->wait_axis] != 0);
    }
    if (!moving) {
        t->wait_kind = COB_WAIT_NONE;
    }
}

void Cob_EngineSetHost(CobEngine *e, void *user,
                        Cob_GetUnitValueFn get_unit_value,
                        Cob_CallFunctionFn call_function) {
    if (!e) return;
    e->host_user           = user;
    e->host_get_unit_value = get_unit_value;
    e->host_call_function  = call_function;
}

void Cob_EngineSetHostSetter(CobEngine *e, Cob_SetUnitValueFn set_unit_value) {
    if (!e) return;
    e->host_set_unit_value = set_unit_value;
}

void Cob_EngineSetHostPlaySound(CobEngine *e, Cob_PlaySoundFn play_sound) {
    if (!e) return;
    e->host_play_sound = play_sound;
}

void Cob_KillAllThreads(CobEngine *e) {
    if (!e) return;
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) {
        e->threads[i].alive = 0;
        e->threads[i].wait_kind = COB_WAIT_NONE;
    }
    e->active_thread_count = 0;
}

void Cob_StopThread(CobEngine *e, int slot) {
    if (!e || slot < 0 || slot >= COB_THREADS_PER_UNIT) return;
    if (!e->threads[slot].alive) return;
    terminate_thread(e, slot, "host stop");
}

int Cob_IsThreadAlive(const CobEngine *e, int slot) {
    if (!e) return 0;
    if (slot < 0 || slot >= COB_THREADS_PER_UNIT) return 0;
    return e->threads[slot].alive ? 1 : 0;
}

int Cob_GetThreadReturn(const CobEngine *e, int slot, int32_t *out_value) {
    if (!e || !out_value) return 0;
    if (slot < 0 || slot >= COB_THREADS_PER_UNIT) return 0;
    const CobThread *t = &e->threads[slot];
    if (!t->has_return_value) return 0;
    *out_value = t->return_value;
    return 1;
}

int Cob_GetThreadArg(const CobEngine *e, int slot, int arg_idx,
                      int32_t *out_value) {
    if (!e || !out_value) return 0;
    if (slot < 0 || slot >= COB_THREADS_PER_UNIT) return 0;
    if (arg_idx < 0 || arg_idx >= COB_THREAD_STACK_DEPTH) return 0;
    /* Args live in the bottom stack slots and a script's out-params are
     * plain POP-VAR writes to them, so the slot still holds the value
     * once the thread ends (legacy:306201-306207). */
    *out_value = e->threads[slot].stack[arg_idx];
    return 1;
}

int Cob_AliveThreadCount(const CobEngine *e) {
    if (!e) return 0;
    int n = 0;
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) {
        if (e->threads[i].alive) n++;
    }
    return n;
}

/* ── Thread cleanup ───────────────────────────────────────────────── */

static void terminate_thread(CobEngine *e, int slot, const char *reason) {
    if (!e->threads[slot].alive) return;
    e->threads[slot].alive = 0;
    if (e->active_thread_count > 0) e->active_thread_count--;
    /* Wake any CALL-SCRIPT caller blocked on this thread. Legacy sets
     * every state-0x2800000 thread whose wait slot matches back to
     * runnable, both on RETURN and on signal-kill
     * (legacy:306953, :306922). */
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) {
        CobThread *w = &e->threads[i];
        if (w->alive && w->wait_kind == COB_WAIT_CHILD &&
            w->wait_child == slot) {
            w->wait_kind = COB_WAIT_NONE;
            w->wait_child = -1;
        }
    }
    if (g_trace) {
        fprintf(stderr, "Cob: thread %d terminated (%s)\n", slot, reason);
    }
}

/* ── Stack helpers ────────────────────────────────────────────────── */

static int push_stack(CobThread *t, int32_t v) {
    if (t->sp >= COB_THREAD_STACK_DEPTH) {
        fprintf(stderr, "Cob: stack overflow (sp=%u)\n", t->sp);
        return -1;
    }
    t->stack[t->sp++] = v;
    return 0;
}

static int pop_stack(CobThread *t, int32_t *out) {
    if (t->sp == 0) {
        fprintf(stderr, "Cob: stack underflow\n");
        return -1;
    }
    *out = t->stack[--t->sp];
    return 0;
}

/* ── Operand fetch ────────────────────────────────────────────────── */

/* Translate a script-piece index to the engine's per-node CobPiece
 * index via the binding map. Returns -1 if the piece is out of range
 * or unbound (no matching mesh node). */
static int script_piece_to_node(const CobEngine *e, uint32_t piece) {
    if (!e->piece_to_node) return -1;
    if (piece >= e->script->num_pieces) return -1;
    return (int)e->piece_to_node[piece];
}

/* Read the next code word as an operand. Advances PC. Caller must
 * guarantee bounds. */
static uint32_t fetch_operand(CobEngine *e, CobThread *t) {
    if (t->pc >= e->script->num_code_words) {
        fprintf(stderr, "Cob: PC out of range fetching operand (pc=%u)\n", t->pc);
        return 0;
    }
    return e->script->code[t->pc++];
}

/* ── Opcode names (for tracing) ───────────────────────────────────── */

static const char *opcode_name(uint32_t op) {
    switch (op) {
        case OP_MOVE:            return "MOVE";
        case OP_TURN:            return "TURN";
        case OP_SPIN:            return "SPIN";
        case OP_STOP_SPIN:       return "STOP-SPIN";
        case OP_SHOW:            return "SHOW";
        case OP_HIDE:            return "HIDE";
        case OP_CACHE_PIECE:     return "CACHE-PIECE";
        case OP_DONT_CACHE_PIECE: return "DON'T-CACHE-PIECE";
        case OP_SHADOW:          return "SHADOW";
        case OP_DONT_SHADOW:     return "DON'T-SHADOW";
        case OP_MOVE_NOW:        return "MOVE-NOW";
        case OP_TURN_NOW:        return "TURN-NOW";
        case OP_SHADE:           return "SHADE";
        case OP_DONT_SHADE:      return "DON'T-SHADE";
        case OP_EMIT_SFX:        return "EMIT-SFX";
        case OP_WAIT_FOR_TURN:   return "WAIT-FOR-TURN";
        case OP_WAIT_FOR_MOVE:   return "WAIT-FOR-MOVE";
        case OP_SLEEP:           return "SLEEP";
        case OP_PUSH_CONSTANT:   return "PUSH-CONSTANT";
        case OP_ALLOC_LOCAL:     return "ALLOC-LOCAL";
        case OP_POP_VAR:         return "POP-VAR";
        case OP_POP_STACK:       return "POP-STACK";
        case OP_ADD:             return "ADD";
        case OP_SUB:             return "SUB";
        case OP_MUL:             return "MUL";
        case OP_DIV:             return "DIV";
        case OP_RAND:            return "RAND";
        case OP_GET_UNIT_VALUE:  return "GET-UNIT-VALUE";
        case OP_GET_WITH_ARGS:   return "GET";
        case OP_GET_HOST_QUERY:  return "GET-HOST-QUERY";
        case OP_GET_HOST_QUERY0: return "GET-HOST-QUERY0";
        case OP_LT: return "LT";
        case OP_LE: return "LE";
        case OP_GT: return "GT";
        case OP_GE: return "GE";
        case OP_EQ: return "EQ";
        case OP_NE: return "NE";
        case OP_LAND: return "LAND";
        case OP_LOR:  return "LOR";
        case OP_LXOR: return "LXOR";
        case OP_LNOT: return "LNOT";
        case OP_AND_BIT: return "AND";
        case OP_OR_BIT:  return "OR";
        case OP_XOR_BIT: return "XOR";
        case OP_NOT_BIT: return "NOT";
        case OP_SHL: return "SHL";
        case OP_SHR: return "SHR";
        case OP_MOD: return "MOD";
        case OP_START_SCRIPT:    return "START-SCRIPT";
        case OP_CALL_SCRIPT:     return "CALL-SCRIPT";
        case OP_HOST_ARGS_63:    return "HOST-ARGS-63";
        case OP_JUMP:            return "JUMP";
        case OP_RETURN:          return "RETURN";
        case OP_JUMP_IF_FALSE:   return "JUMP-IF-FALSE";
        case OP_SIGNAL:          return "SIGNAL";
        case OP_SET_SIGNAL_MASK: return "SET-SIGNAL-MASK";
        case OP_EXPLODE:         return "EXPLODE";
        case OP_PLAY_SOUND:      return "PLAY-SOUND";
        case OP_MISSION_COMMAND: return "MISSION-COMMAND";
        case OP_SOUND_CMD_74:    return "SOUND-CMD-74";
        case OP_HOST_MARK_75:    return "HOST-MARK-75";
        case OP_SET_UNIT_VALUE:  return "SET-UNIT-VALUE";
        case OP_ATTACH_UNIT:     return "ATTACH-UNIT";
        case OP_DROP_UNIT:       return "DROP-UNIT";
        default: return "?";
    }
}

/* ── Per-thread step ──────────────────────────────────────────────── *
 *
 * Execute up to `budget` opcodes. Returns when:
 *   - thread terminates (alive=0)
 *   - thread sleeps (sleep_remaining > 0)
 *   - budget exhausted
 *   - unknown opcode hit (logged, thread terminates)
 */

/* Opcode encoding (R1 finding): the masked value `op & 0x100ff000`
 * selects the opcode class. Bits outside that mask encode an inline
 * operand for opcodes that support it (notably PUSH-CONSTANT packs
 * small immediates into the low 12 bits). Most opcodes leave the
 * inline bits zero. */
#define COB_OPCODE_MASK 0x100ff000u

static void run_thread(CobEngine *e, int slot, int budget) {
    CobThread *t = &e->threads[slot];
    const CobScript *s = e->script;

    int ops = 0;
    while (t->alive && ops < budget) {
        if (t->pc >= s->num_code_words) {
            fprintf(stderr, "Cob: thread %d PC out of range (%u >= %u)\n",
                    slot, t->pc, s->num_code_words);
            terminate_thread(e, slot, "pc oob");
            return;
        }
        uint32_t pc_at = t->pc;
        uint32_t raw = s->code[t->pc++];
        uint32_t op  = raw & COB_OPCODE_MASK;
        uint32_t inl = raw & ~COB_OPCODE_MASK;
        ops++;

        if (g_trace) {
            fprintf(stderr, "  T%d @%u op=0x%08x %-18s sp=%u",
                    slot, pc_at, raw, opcode_name(op), t->sp);
            if (inl) fprintf(stderr, " inl=0x%x", inl);
        }

        switch (op) {
        case OP_PUSH_CONSTANT: {
            /* From decomp the legacy reference ~306535:
             *   low 3 bits encode addressing mode:
             *     1 = literal (value follows in next code word)
             *     2 = local var read (next word is stack-relative idx)
             *     4 = static var read (next word is index into engine's
             *         static_vars table)
             * The next code word is consumed in all three cases. */
            uint32_t mode = inl & 7;
            uint32_t arg  = fetch_operand(e, t);
            int32_t  v    = 0;
            if (mode == 1) {
                v = (int32_t)arg;
            } else if (mode == 2) {
                /* stack-relative: thread's stack[arg] (TA convention).
                 * Bounds-check; out-of-range reads as 0. */
                if (arg < t->sp) v = t->stack[arg];
            } else if (mode == 4) {
                if (arg < e->script->num_static_vars) v = e->static_vars[arg];
            }
            if (g_trace) fprintf(stderr, "  mode=%u arg=%u v=%d\n",
                                  mode, arg, v);
            push_stack(t, v);
        } break;

        case OP_SLEEP: {
            /* Pop sleep duration in milliseconds; convert to ticks via
             * engine-rate constant (60 Hz): ticks = (60 * ms) / 1000.
             * Then yield. Decomp the legacy reference ~306522. */
            int32_t ms;
            pop_stack(t, &ms);
            if (ms < 0) ms = 0;
            t->sleep_remaining = (60 * ms) / 1000;
            if (g_trace) fprintf(stderr, "  ms=%d -> %d ticks\n",
                                  ms, t->sleep_remaining);
            return;  /* yield */
        } break;

        case OP_POP_STACK: {
            int32_t tmp;
            pop_stack(t, &tmp);
            if (g_trace) fprintf(stderr, "  popped=%d\n", tmp);
        } break;

        case OP_ALLOC_LOCAL: {
            /* Allocate a stack slot without pushing a value (decomp
             * :306531: thread[2] = thread[2] + 1). The slot starts as
             * whatever garbage was there; scripts that use it overwrite
             * before reading. */
            if (t->sp < COB_THREAD_STACK_DEPTH) {
                t->sp++;
            } else {
                fprintf(stderr, "Cob: ALLOC-LOCAL overflow\n");
            }
            if (g_trace) fprintf(stderr, "  sp now %u\n", t->sp);
        } break;

        case OP_SHOW:
        case OP_HIDE: {
            uint32_t piece = fetch_operand(e, t);
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d\n", piece, node);
            if (node >= 0) {
                e->pieces[node].hidden = (op == OP_HIDE);
            }
        } break;

        case OP_TURN_NOW: {
            /* Snap rot[axis] to the popped angle (& 0xffff); clears any
             * in-flight TURN/SPIN (legacy:306453-306464). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t target;
            pop_stack(t, &target);
            target &= COB_ANGLE_MASK;
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u angle=%d\n",
                                  piece, node, axis, target);
            if (node >= 0 && axis < 3) {
                CobPiece *p = &e->pieces[node];
                p->rot[axis]        = target;
                p->rot_target[axis] = target;
                p->rot_speed[axis]  = 0;
                p->spin_speed[axis] = 0;
                p->spin_accel[axis] = 0;
            }
        } break;

        case OP_MOVE_NOW: {
            /* Snap pos[axis] to the popped value; clears any in-flight
             * MOVE (legacy:306441-306450). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t target;
            pop_stack(t, &target);
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u pos=%d\n",
                                  piece, node, axis, target);
            if (node >= 0 && axis < 3) {
                e->pieces[node].pos[axis]        = target;
                e->pieces[node].pos_target[axis] = target;
                e->pieces[node].pos_speed[axis]  = 0;
            }
        } break;

        case OP_TURN: {
            /* Turn piece to angle at speed, shortest way; direction sign
             * comes from the ±0x8000-wrapped delta
             * (legacy:306370-306391). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t target, speed_per_sec;
            pop_stack(t, &target);
            pop_stack(t, &speed_per_sec);
            target &= COB_ANGLE_MASK;
            int32_t speed_per_tick = speed_per_sec / 60;
            if (speed_per_tick == 0 && speed_per_sec != 0)
                speed_per_tick = speed_per_sec > 0 ? 1 : -1;
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u target=%d sps=%d\n",
                                  piece, node, axis, target, speed_per_sec);
            if (node >= 0 && axis < 3) {
                CobPiece *p = &e->pieces[node];
                p->rot[axis] &= COB_ANGLE_MASK;
                p->rot_target[axis] = target;
                p->spin_speed[axis] = 0;
                p->spin_accel[axis] = 0;
                int32_t delta = angle_delta(p->rot[axis], target);
                if (delta == 0) {
                    speed_per_tick = 0;
                } else {
                    if (speed_per_tick < 0) speed_per_tick = -speed_per_tick;
                    if (delta < 0) speed_per_tick = -speed_per_tick;
                }
                p->rot_speed[axis] = speed_per_tick;
            }
        } break;

        case OP_SPIN: {
            /* Continuous rotation: rot_target = spinning sentinel; pop
             * target speed then accel; zero accel jumps straight to the
             * target speed (legacy:306337-306352). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t speed_per_sec, accel_per_sec;
            pop_stack(t, &speed_per_sec);
            pop_stack(t, &accel_per_sec);
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u speed=%d accel=%d\n",
                                  piece, node, axis, speed_per_sec, accel_per_sec);
            if (node >= 0 && axis < 3) {
                CobPiece *p = &e->pieces[node];
                p->rot_target[axis] = COB_ROT_SPINNING;
                p->spin_speed[axis] = speed_per_sec / 60;
                p->spin_accel[axis] = accel_per_sec / 60;
                if (p->spin_accel[axis] == 0) {
                    p->rot_speed[axis] = p->spin_speed[axis];
                }
            }
        } break;

        case OP_MOVE: {
            /* Move piece to position at speed
             * (legacy:306353-306367). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t target, speed_per_sec;
            pop_stack(t, &target);
            pop_stack(t, &speed_per_sec);
            int32_t speed_per_tick = speed_per_sec / 60;
            if (speed_per_tick == 0 && speed_per_sec != 0)
                speed_per_tick = speed_per_sec > 0 ? 1 : -1;
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u target=%d sps=%d\n",
                                  piece, node, axis, target, speed_per_sec);
            if (node >= 0 && axis < 3) {
                CobPiece *p = &e->pieces[node];
                p->pos_target[axis] = target;
                if (target < p->pos[axis] && speed_per_tick > 0)      speed_per_tick = -speed_per_tick;
                else if (target > p->pos[axis] && speed_per_tick < 0) speed_per_tick = -speed_per_tick;
                else if (target == p->pos[axis])                       speed_per_tick = 0;
                p->pos_speed[axis] = speed_per_tick;
            }
        } break;

        case OP_STOP_SPIN: {
            /* Decelerate spin to zero; zero decel stops instantly
             * (legacy:306397-306412). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int32_t  decel_per_sec;
            pop_stack(t, &decel_per_sec);
            int node = script_piece_to_node(e, piece);
            if (node >= 0 && axis < 3) {
                CobPiece *p = &e->pieces[node];
                p->rot_target[axis] = COB_ROT_SPINNING;
                p->spin_speed[axis] = 0;
                p->spin_accel[axis] = -(decel_per_sec / 60);
                if (p->spin_accel[axis] == 0) {
                    p->rot_speed[axis] = 0;
                }
            }
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u decel=%d\n",
                                  piece, node, axis, decel_per_sec);
        } break;

        case OP_SHADOW:
        case OP_DONT_SHADOW: {
            /* Piece shadow-casting flag: host[0x14](piece, 1/0)
             * (legacy:306327, :306437). */
            uint32_t piece = fetch_operand(e, t);
            int node = script_piece_to_node(e, piece);
            if (node >= 0) {
                e->pieces[node].shadow_off = (op == OP_DONT_SHADOW);
            }
            if (g_trace) fprintf(stderr, "  piece=%u node=%d shadow=%d\n",
                                  piece, node, op == OP_SHADOW);
        } break;

        case OP_SHADE:
        case OP_DONT_SHADE: {
            /* Piece shading flag: host[0x10](piece, 1/0)
             * (legacy:306430, :306468). */
            uint32_t piece = fetch_operand(e, t);
            int node = script_piece_to_node(e, piece);
            if (node >= 0) {
                e->pieces[node].shade_off = (op == OP_DONT_SHADE);
            }
            if (g_trace) fprintf(stderr, "  piece=%u node=%d shade=%d\n",
                                  piece, node, op == OP_SHADE);
        } break;

        case OP_EMIT_SFX: {
            /* host[0x30](piece_inline, popped sfx type)
             * (legacy:306486-306490). No particle system yet —
             * consume operands to keep PC/stack aligned. */
            uint32_t piece = fetch_operand(e, t);
            int32_t sfx_type;
            pop_stack(t, &sfx_type);
            (void)piece;
            if (g_trace) fprintf(stderr, "  piece=%u type=%d (no-op)\n",
                                  piece, sfx_type);
        } break;

        case OP_EXPLODE: {
            /* host[0x34](piece_inline, popped explosion type)
             * (legacy:306894-306899). Mark the piece so render
             * and effect code can react. */
            uint32_t piece = fetch_operand(e, t);
            int32_t how;
            pop_stack(t, &how);
            int node = script_piece_to_node(e, piece);
            if (node >= 0) {
                e->pieces[node].exploded = 1;
            }
            if (g_trace) fprintf(stderr, "  piece=%u node=%d how=%d\n",
                                  piece, node, how);
        } break;

        case OP_WAIT_FOR_TURN:
        case OP_WAIT_FOR_MOVE: {
            /* Two inline operands (piece, axis); block the thread until
             * that piece-axis animation settles. Legacy thread states
             * 0x2100000 / 0x2200000 (legacy:306474, :306316). */
            uint32_t piece = fetch_operand(e, t);
            uint32_t axis  = fetch_operand(e, t);
            int node = script_piece_to_node(e, piece);
            if (g_trace) fprintf(stderr, "  piece=%u node=%d axis=%u\n",
                                  piece, node, axis);
            if (node < 0 || axis >= 3) break;
            int moving;
            if (op == OP_WAIT_FOR_TURN) {
                moving = (e->pieces[node].rot_speed[axis] != 0);
            } else {
                moving = (e->pieces[node].pos_speed[axis] != 0);
            }
            if (!moving) break;
            t->wait_piece = (int16_t)node;     /* store node index, not script piece */
            t->wait_axis  = (uint8_t)axis;
            t->wait_kind  = (op == OP_WAIT_FOR_TURN) ? COB_WAIT_TURN
                                                     : COB_WAIT_MOVE;
            return;
        } break;

        case OP_JUMP: {
            /* pc = inline word (legacy:306998). */
            uint32_t target = fetch_operand(e, t);
            if (g_trace) fprintf(stderr, "  -> %u\n", target);
            t->pc = target;
        } break;

        case OP_JUMP_IF_FALSE: {
            /* Pop condition; branch to the inline target when it is
             * ZERO, fall through otherwise (legacy:306937-306941
             * — `if (opcode != 0) skip; else pc = inline`). */
            uint32_t target = fetch_operand(e, t);
            int32_t v;
            pop_stack(t, &v);
            if (g_trace) fprintf(stderr, "  cond=%d target=%u\n", v, target);
            if (v == 0) t->pc = target;
        } break;

        case OP_HOST_MARK_75: {
            /* host[0x44](inline word); no stack effect
             * (legacy:306845-306849). */
            uint32_t arg = fetch_operand(e, t);
            (void)arg;
            if (g_trace) fprintf(stderr, "  arg=%u (host no-op)\n", arg);
        } break;

        case OP_RETURN: {
            /* End of thread: pop the return value (if any), kill the
             * thread, and wake CALL-SCRIPT callers blocked on it
             * (legacy:306943-306960). There is no intra-thread
             * call stack in the original — script calls are threads. */
            if (t->sp > 0) {
                t->return_value = t->stack[t->sp - 1];
                t->has_return_value = 1;
                if (g_trace) fprintf(stderr, "  return value=%d\n",
                                      t->return_value);
            } else {
                t->has_return_value = 0;
            }
            if (g_trace) fprintf(stderr, "  thread end\n");
            terminate_thread(e, slot, "RETURN");
            return;
        } break;

        case OP_START_SCRIPT:
        case OP_CALL_SCRIPT: {
            /* Operands: function index, n_args. Args are popped from
             * the CALLER's stack and become the child's locals; the
             * child inherits the caller's signal mask
             * (legacy:306812, :306976). CALL-SCRIPT additionally
             * blocks the caller until the child RETURNs (:306978-306981
             * state 0x2800000). */
            uint32_t fn_idx = fetch_operand(e, t);
            uint32_t n_args = fetch_operand(e, t);
            if (g_trace) fprintf(stderr, "  fn=%u n_args=%u\n", fn_idx, n_args);
            int32_t args[16];
            if (n_args > 16) n_args = 16;
            for (uint32_t i = 0; i < n_args; i++) {
                /* Pop in reverse so args[0] is the deepest pushed. */
                pop_stack(t, &args[n_args - 1 - i]);
            }
            int child = Cob_StartThread(e, (int)fn_idx, args, (int)n_args);
            if (child >= 0) {
                e->threads[child].signal_mask = t->signal_mask;
                if (op == OP_CALL_SCRIPT) {
                    t->wait_kind  = COB_WAIT_CHILD;
                    t->wait_child = (int8_t)child;
                    return;   /* yield until the child RETURNs */
                }
            }
        } break;

        case OP_POP_VAR: {
            /* Addressing mode in the low 3 bits, like PUSH-CONSTANT:
             * 2 = write local (stack slot), 4 = write static
             * (legacy:306505-306519). */
            uint32_t mode = inl & 7;
            uint32_t var_idx = fetch_operand(e, t);
            int32_t v;
            pop_stack(t, &v);
            if (g_trace) fprintf(stderr, "  mode=%u var=%u value=%d\n",
                                  mode, var_idx, v);
            if (mode == 2) {
                if (var_idx < COB_THREAD_STACK_DEPTH) t->stack[var_idx] = v;
            } else if (mode == 4) {
                if (var_idx < s->num_static_vars) e->static_vars[var_idx] = v;
            } else if (var_idx < s->num_static_vars) {
                e->static_vars[var_idx] = v;
            }
        } break;

        case OP_PLAY_SOUND: {
            /* host[0x38](sound-name[inline idx], popped arg) → push
             * result (legacy:306851-306860). The name is
             * resolved through the v6 sub-header string table before
             * the host sees it, matching the legacy dispatch at
             * :306851 (nameTable[nameIdx]). */
            uint32_t name_idx = fetch_operand(e, t);
            int32_t arg = 0;
            pop_stack(t, &arg);
            int32_t snd_ret = 0;
            if (e->host_play_sound && s->sound_names &&
                name_idx < s->num_sound_names && s->sound_names[name_idx]) {
                snd_ret = e->host_play_sound(e->host_user,
                                             s->sound_names[name_idx], arg);
            }
            push_stack(t, snd_ret);
            if (g_trace) fprintf(stderr, "  sound#%u arg=%d ret=%d\n",
                                  name_idx, arg, snd_ret);
        } break;

        case OP_SOUND_CMD_74: {
            /* host[0x40](inline arg, sound-name[inline idx]); no stack
             * effect (legacy:306885-306890). */
            uint32_t arg = fetch_operand(e, t);
            uint32_t name_idx = fetch_operand(e, t);
            (void)arg; (void)name_idx;
            if (g_trace) fprintf(stderr, "  arg=%u sound#%u (no-op)\n",
                                  arg, name_idx);
        } break;

        case OP_MISSION_COMMAND: {
            /* host[0x3c](name[inline idx], inline argc, popped args) →
             * push result (legacy:306862-306882). Forward to the
             * host callback with the name index as fn id. */
            uint32_t name_idx = fetch_operand(e, t);
            uint32_t argc = fetch_operand(e, t);
            int32_t args[8] = {0};
            int n_args = argc > 8 ? 8 : (int)argc;
            for (uint32_t i = 0; i < argc; i++) {
                int32_t v;
                pop_stack(t, &v);
                if ((int)i < n_args) args[n_args - 1 - (int)i] = v;
            }
            int32_t ret = 0;
            if (e->host_call_function) {
                ret = e->host_call_function(e->host_user, (int)name_idx,
                                            n_args, args);
            }
            push_stack(t, ret);
            if (g_trace) fprintf(stderr, "  cmd#%u argc=%u -> %d\n",
                                  name_idx, argc, ret);
        } break;

        case OP_GET_UNIT_VALUE:
        case OP_GET_WITH_ARGS: {
            /* GET port queries: 0x10042000 pops the port only;
             * 0x10043000 pops port + 4 args. Both route through
             * host[0x54] (legacy:306675-306703). */
            int n_args = (op == OP_GET_UNIT_VALUE) ? 1 : 5;
            int32_t args[5] = {0};
            for (int i = 0; i < n_args; i++) {
                /* Pop in stack order: top of stack = args[0] (port). */
                pop_stack(t, &args[i]);
            }
            int32_t port = args[0];
            int32_t ret = 1;
            if (e->host_call_function) {
                ret = e->host_call_function(e->host_user, port, n_args, args);
            }
            push_stack(t, ret);
            if (g_trace) fprintf(stderr, "  port=%d nargs=%d -> %d\n",
                                  port, n_args, ret);
        } break;

        case OP_RAND: {
            /* Pop (lo, hi), push pseudo-random in [lo, hi].
             * M2 stub: return midpoint deterministically. M5 wires
             * the real Sim_Rand. */
            int32_t hi, lo;
            pop_stack(t, &hi);
            pop_stack(t, &lo);
            push_stack(t, (lo + hi) / 2);
            if (g_trace) fprintf(stderr, "  [%d,%d] -> %d\n", lo, hi, (lo+hi)/2);
        } break;

        case OP_GET_HOST_QUERY: {
            /* Pop one value, host[0x58](x), push result
             * (legacy:306655-306661). Ground-height flavored in
             * observed scripts — route through the unit-value host. */
            int32_t param;
            pop_stack(t, &param);
            int32_t v = 0;
            if (e->host_get_unit_value) {
                v = e->host_get_unit_value(e->host_user, (int)param);
            }
            push_stack(t, v);
            if (g_trace) fprintf(stderr, "  param=%d -> %d\n", param, v);
        } break;

        case OP_GET_HOST_QUERY0: {
            /* No stack input: host[0x5c]() → push
             * (legacy:306706-306710). */
            int32_t v = 0;
            if (e->host_get_unit_value) {
                v = e->host_get_unit_value(e->host_user, -1);
            }
            push_stack(t, v);
            if (g_trace) fprintf(stderr, "  -> %d\n", v);
        } break;

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_AND_BIT: case OP_OR_BIT: case OP_XOR_BIT:
        case OP_SHL: case OP_SHR: case OP_MOD:
        case OP_LT: case OP_LE: case OP_GT: case OP_GE:
        case OP_EQ: case OP_NE: case OP_LAND: case OP_LOR: case OP_LXOR: {
            int32_t b, a;
            pop_stack(t, &b);
            pop_stack(t, &a);
            int32_t r = 0;
            switch (op) {
                case OP_ADD: r = a + b; break;
                case OP_SUB: r = a - b; break;
                case OP_MUL: r = a * b; break;
                case OP_DIV: r = (b != 0) ? a / b : 0; break;
                case OP_AND_BIT: r = a & b; break;
                case OP_OR_BIT:  r = a | b; break;
                case OP_XOR_BIT: r = a ^ b; break;
                case OP_SHL: r = a << (b & 31); break;
                case OP_SHR: r = a >> (b & 31); break;
                case OP_MOD: r = (b != 0) ? a % b : 0; break;
                case OP_LT: r = (a <  b); break;
                case OP_LE: r = (a <= b); break;
                case OP_GT: r = (a >  b); break;
                case OP_GE: r = (a >= b); break;
                case OP_EQ: r = (a == b); break;
                case OP_NE: r = (a != b); break;
                case OP_LAND: r = (a && b); break;
                case OP_LOR:  r = (a || b); break;
                case OP_LXOR: r = (!!a != !!b); break;
            }
            push_stack(t, r);
            if (g_trace) fprintf(stderr, "  %d,%d -> %d\n", a, b, r);
        } break;

        case OP_NOT_BIT: case OP_LNOT: {
            int32_t v;
            pop_stack(t, &v);
            push_stack(t, (op == OP_NOT_BIT) ? ~v : !v);
            if (g_trace) fprintf(stderr, "\n");
        } break;

        case OP_CACHE_PIECE:
        case OP_DONT_CACHE_PIECE: {
            /* Render-cache hints. Legacy: the legacy reference ~306419-26
             *   case 0x10007000:  host[12](piece, 1);  pc += 2;
             *   case 0x10008000:  host[12](piece, 0);  pc += 2;
             * One operand (piece), no stack pops. We don't model the
             * piece cache, so just consume the operand and continue. */
            uint32_t piece = fetch_operand(e, t);
            (void)piece;
            if (g_trace) fprintf(stderr, "  cache piece=%u (no-op)\n", piece);
        } break;

        case OP_HOST_ARGS_63: {
            /* Two inline operands, second is a count; pops that many
             * values into a host arg buffer with no other effect
             * (legacy:306983-306996). Preserve PC/stack
             * alignment. */
            uint32_t callback_id = fetch_operand(e, t);
            uint32_t n_args = fetch_operand(e, t);
            if (n_args > COB_THREAD_STACK_DEPTH) n_args = COB_THREAD_STACK_DEPTH;
            for (uint32_t ai = 0; ai < n_args; ai++) {
                int32_t tmp;
                pop_stack(t, &tmp);
            }
            if (g_trace) fprintf(stderr, "  callback=%u n_args=%u\n",
                                  callback_id, n_args);
        } break;

        case OP_SIGNAL: {
            /* Pop a mask and terminate every live thread whose signal
             * mask intersects it — INCLUDING this one (legacy scans all
             * 16 slots and stops running when it kills itself,
             * legacy:306904-306935). Waiting callers of killed
             * threads are woken by terminate_thread. */
            int32_t mask;
            pop_stack(t, &mask);
            int killed_self = 0;
            for (int ti = 0; ti < COB_THREADS_PER_UNIT; ti++) {
                CobThread *ot = &e->threads[ti];
                if (!ot->alive) continue;
                if (((uint32_t)mask & ot->signal_mask) != 0) {
                    terminate_thread(e, ti, "signal");
                    if (ti == slot) killed_self = 1;
                }
            }
            if (g_trace) fprintf(stderr, "  mask=0x%x%s\n", (unsigned)mask,
                                  killed_self ? " (self)" : "");
            if (killed_self) return;
        } break;

        case OP_SET_SIGNAL_MASK: {
            int32_t mask;
            pop_stack(t, &mask);
            t->signal_mask = (uint32_t)mask;
            if (g_trace) fprintf(stderr, "  mask=0x%x\n", (unsigned)mask);
        } break;

        case OP_SET_UNIT_VALUE: {
            /* Pop value then port; host[0x50](port, value)
             * (legacy:306819-306825). */
            int32_t value, port;
            pop_stack(t, &value);
            pop_stack(t, &port);
            if (e->host_set_unit_value) {
                e->host_set_unit_value(e->host_user, (int)port, value);
            }
            if (g_trace) fprintf(stderr, "  port=%d value=%d\n", port, value);
        } break;

        case OP_ATTACH_UNIT: {
            /* Pop three values; host[0x48]
             * (legacy:306827-306835). Transport attach — no
             * host hook yet. */
            int32_t a, b, c;
            pop_stack(t, &a);
            pop_stack(t, &b);
            pop_stack(t, &c);
            if (g_trace) fprintf(stderr, "  (%d,%d,%d) (no-op)\n", c, b, a);
        } break;

        case OP_DROP_UNIT: {
            /* Pop one value; host[0x4c]
             * (legacy:306838-306841). */
            int32_t a;
            pop_stack(t, &a);
            if (g_trace) fprintf(stderr, "  %d (no-op)\n", a);
        } break;

        default:
            /* Opcode 0 = NUL bytes past the end of valid code (a
             * thread that fell off due to a missed RETURN or
             * unimplemented opcode). Terminate quietly. */
            if (op != 0 && !already_logged_unknown(op)) {
                fprintf(stderr, "Cob: unknown opcode 0x%08x at pc=%u (thread %d)\n",
                        op, pc_at, slot);
            }
            if (op != 0) g_unknown_total++;
            if (g_trace) fprintf(stderr, "  (unknown — terminating thread)\n");
            terminate_thread(e, slot, "unknown opcode");
            return;
        }
    }

    if (g_trace && t->alive) {
        fprintf(stderr, "Cob: thread %d budget exhausted at pc=%u\n", slot, t->pc);
    }
}

void Cob_RunThreadNow(CobEngine *e, int slot) {
    if (!e || !e->script) return;
    if (slot < 0 || slot >= COB_THREADS_PER_UNIT) return;
    if (!e->threads[slot].alive) return;
    run_thread(e, slot, COB_OPS_PER_TICK_LIMIT);
}

int Cob_RunScriptSync(CobEngine *e, const char *name,
                       int32_t *args_inout, int n_args) {
    if (!e || !e->script || !name) return -1;
    if (n_args < 0 || n_args > COB_THREAD_STACK_DEPTH) return -1;
    if (n_args > 0 && !args_inout) return -1;
    int slot = Cob_StartThreadByName(e, name, args_inout, n_args);
    if (slot < 0) return -1;
    /* Legacy runs the thread inline to completion (legacy:306199) and
     * then reads the arg slots back (legacy:306201-306207). */
    run_thread(e, slot, COB_OPS_PER_TICK_LIMIT);
    /* Query scripts never sleep in shipped data. One that yields anyway
     * would leak its slot, so end it here rather than let it linger. */
    if (e->threads[slot].alive) terminate_thread(e, slot, "sync run");
    for (int i = 0; i < n_args; i++)
        args_inout[i] = e->threads[slot].stack[i];
    return 0;
}

void Cob_RunAllThreads(CobEngine *e) {
    if (!e || !e->script) return;
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) {
        CobThread *t = &e->threads[i];
        if (!t->alive) continue;
        /* Sleep wins over wait. Decrement sleep first; if still
         * sleeping, skip dispatch. */
        if (t->sleep_remaining > 0) {
            t->sleep_remaining--;
            continue;
        }
        /* If waiting on a turn/move, check whether the target is
         * reached. If yes, clear wait state and proceed. */
        if (t->wait_kind != COB_WAIT_NONE) {
            check_thread_wait(e, t);
            if (t->wait_kind != COB_WAIT_NONE) continue;  /* still waiting */
        }
        run_thread(e, i, COB_OPS_PER_TICK_LIMIT);
    }
}

/* External tracing toggle (declared above with g_trace). */
extern void Cob_SetTrace(int on);
