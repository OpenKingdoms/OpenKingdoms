#ifndef TAK_COB_VM_H
#define TAK_COB_VM_H

#include "tak_types.h"
#include "tak_cob.h"

/* ── COB virtual machine (Phase D M2+) ─────────────────────────────
 *
 * One CobEngine per Unit instance. Owns:
 *   - 16 CobThread slots (cooperative; runs in tick order).
 *   - One CobPiece per piece declared by the script.
 *   - One int32_t per static variable.
 *
 * The script bundle (CobScript) is owned by the UnitDef and shared
 * across instances.
 *
 * All animation state is fixed-point int32 for determinism. Floats
 * are not used inside the sim. The animator pass (Cob_AnimatePieces,
 * landing in M4) advances current toward target each tick.
 *
 * Tick semantics (per PROJECT_PLAN.md §1.1, MANUAL_DEVIATIONS D-001):
 *   - We run at 60 Hz; TAK's native is 30 Hz.
 *   - Speed values from MOVE/ROTATE opcodes are units/second.
 *     Per-tick delta = speed / 60. (TAK divided by 30 → twice as much
 *     per tick. We divide by 60 → half per tick, applied twice as
 *     often → identical per-second motion, smoother.)
 *   - SLEEP is in milliseconds, converted at runtime: ticks = ms*60/1000.
 */

/* ── Thread state ─────────────────────────────────────────────────── */

#define COB_THREADS_PER_UNIT     16
#define COB_THREAD_STACK_DEPTH   40
#define COB_THREAD_RETURN_DEPTH  8
#define COB_OPS_PER_TICK_LIMIT   200   /* anti-runaway clamp */

/* Wait-condition kinds for thread blocking. Mirrors the legacy thread
 * states (legacy:306474 0x2100000 wait-turn, :306316 0x2200000
 * wait-move, :306979 0x2800000 wait-for-called-script). */
#define COB_WAIT_NONE     0
#define COB_WAIT_TURN     1   /* until pieces[wait_piece].rot_speed[wait_axis] == 0 */
#define COB_WAIT_MOVE     2   /* until pieces[wait_piece].pos_speed[wait_axis] == 0 */
#define COB_WAIT_CHILD    3   /* until thread wait_child RETURNs (CALL-SCRIPT) */

typedef struct CobThread {
    uint32_t pc;                                /* word index into code */
    int32_t  stack[COB_THREAD_STACK_DEPTH];     /* operand stack */
    int32_t  sleep_remaining;                   /* sim ticks until resume */
    int16_t  wait_piece;                        /* piece idx; -1 = not waiting */
    uint32_t signal_mask;                       /* SET-SIGNAL-MASK / SIGNAL */
    int8_t   wait_child;                        /* thread slot; -1 = none */
    uint8_t  wait_axis;                         /* 0..2 */
    uint8_t  wait_kind;                         /* COB_WAIT_* */
    uint8_t  sp;                                /* stack pointer (next free) */
    uint8_t  alive;                             /* 0 = slot free */
    uint8_t  has_return_value;                  /* final RETURN left a stack value */
    uint8_t  _pad[2];
    int32_t  return_value;                      /* final top-of-stack return */
} CobThread;

/* ── Piece state ──────────────────────────────────────────────────── */

#define COB_AXIS_X 0
#define COB_AXIS_Y 1
#define COB_AXIS_Z 2

/* Fixed-point conventions (legacy-verified):
 *   rotation: TA angle units — 65536 = one full circle (the dispatch
 *             masks TURN targets & 0xffff and uses ±0x8000 for the
 *             shortest-way decision, legacy:306376/306387).
 *             The renderer converts with 2π/65536.
 *   position: int32_t in 1/65536-th-of-model-unit
 *   speed:    same units per second; divided by the tick rate (60)
 *             when an opcode stores it (legacy divides by engine[1]).
 */

#define COB_FIXED_ONE      (1 << 16)
#define COB_ANGLE_MASK     0xffff
/* rot_target sentinel while a SPIN is active (legacy stores 0xffffffff
 * at the turn-target slot, legacy:306340). TURN targets are
 * masked to 0..65535 so the sentinel cannot collide. */
#define COB_ROT_SPINNING   (-1)

typedef struct CobPiece {
    int32_t rot[3];          /* current rotation, TA angle units */
    int32_t rot_target[3];   /* TURN target, or COB_ROT_SPINNING */
    int32_t rot_speed[3];    /* current per-tick angular speed (signed) */
    int32_t spin_speed[3];   /* SPIN target speed, per tick (+0x34 slot) */
    int32_t spin_accel[3];   /* SPIN accel toward spin_speed, per tick (+0x40) */
    int32_t pos[3];           /* current local translation */
    int32_t pos_target[3];
    int32_t pos_speed[3];    /* per-tick (signed) */
    uint8_t hidden;           /* 0=visible, 1=hidden (HIDE/SHOW opcodes) */
    uint8_t exploded;         /* 1=exploded by EXPLODE (0x10071000) */
    uint8_t shadow_off;       /* DONT-SHADOW (0x1000a000) */
    uint8_t shade_off;        /* DONT-SHADE (0x1000e000) */
} CobPiece;

/* ── Engine ───────────────────────────────────────────────────────── */

/* Host callback signatures (Phase D M5b). The VM dispatches
 * GET-UNIT-VALUE and CALL-FUNCTION through these so unit-specific
 * state (velocity, heading, etc.) and engine builtins (SetMaxReloadTime
 * etc.) can be answered by the host (units.c). */
typedef int32_t (*Cob_GetUnitValueFn)(void *user, int param);
typedef int32_t (*Cob_CallFunctionFn)(void *user, int fn_id,
                                       int n_args, const int32_t *args);
/* PLAY-SOUND (0x10072000): host receives the resolved sound name from
 * the script's v6 name table plus the popped arg (legacy flags: bits
 * 0-2 = category, doubling as priority; category 7 = global/UI; bit 5
 * = loop). Return value is pushed back to the script. */
typedef int32_t (*Cob_PlaySoundFn)(void *user, const char *sound_name,
                                   int32_t arg);

/* SET-VALUE (0x10082000): host receives (port, value); no result. */
typedef void    (*Cob_SetUnitValueFn)(void *user, int port, int32_t value);

typedef struct CobEngine {
    const CobScript    *script;          /* not owned */
    CobThread           threads[COB_THREADS_PER_UNIT];
    CobPiece           *pieces;          /* owned, sized by piece_count (node_count) */
    int16_t            *piece_to_node;   /* owned, [script->num_pieces]; -1 = unbound */
    int32_t            *static_vars;     /* owned, num_static_vars long */
    int                 piece_count;     /* node_count from the bound mesh */
    uint16_t            active_thread_count;
    uint16_t            _pad;
    /* Host callbacks; NULL = use stub (returns 0 / 1). */
    void               *host_user;
    Cob_GetUnitValueFn  host_get_unit_value;
    Cob_CallFunctionFn  host_call_function;
    Cob_SetUnitValueFn  host_set_unit_value;
    Cob_PlaySoundFn     host_play_sound;
} CobEngine;

/* Set the host callbacks after Cob_EngineInit. user is forwarded as
 * the first arg of each callback. NULL fns fall back to stubs. */
void Cob_EngineSetHost(CobEngine *e, void *user,
                        Cob_GetUnitValueFn get_unit_value,
                        Cob_CallFunctionFn call_function);

/* Optional SET-VALUE host hook (yard-open, activation, …). */
void Cob_EngineSetHostSetter(CobEngine *e, Cob_SetUnitValueFn set_unit_value);

/* Optional PLAY-SOUND host hook (unit voices, deaths, weapon cues). */
void Cob_EngineSetHostPlaySound(CobEngine *e, Cob_PlaySoundFn play_sound);

/* Kill all alive threads for an engine — used when a unit dies
 * (Killed script runs as a fresh thread on a cleared slate). */
void Cob_KillAllThreads(CobEngine *e);
void Cob_StopThread(CobEngine *e, int slot);

/* Set up an engine bound to a script and a piece-name table (typically
 * the names of every 3DO node in the unit's mesh, in mesh order). The
 * engine's pieces[] is sized by node_count. The script's piece-name
 * table is matched against node_names[] to build piece_to_node[];
 * unmatched script pieces map to -1 and TURN-PIECE/etc on those is
 * silently ignored. Returns 0 on success, -1 on alloc failure. */
int  Cob_EngineInit(CobEngine *e, const CobScript *script,
                     int node_count, const char *const *node_names);

/* Tear down. NULL-safe. */
void Cob_EngineFree(CobEngine *e);

/* Spawn a thread starting at the named script. Pushes args onto the
 * new thread's stack before execution (consistent with how TAK passes
 * parameters to scripts like HitByWeapon). Returns the thread slot
 * index, or -1 if the script doesn't exist or all 16 slots are full. */
int  Cob_StartThreadByName(CobEngine *e, const char *name,
                            const int32_t *args, int n_args);

/* Same, but by script index. Faster path once the index is cached. */
int  Cob_StartThread(CobEngine *e, int script_idx,
                      const int32_t *args, int n_args);

/* Run a script to completion on a fresh thread NOW and copy its arg
 * slots back out (legacy:306142-306208). Scripts return out-params by
 * writing their arg locals. QueryBuildInfo writes the build-spot piece
 * into local 0. args_inout carries the seed values in and the script's
 * values out. Returns 0 on success, -1 if the script is missing or no
 * thread slot is free. Side effects (TURN-NOW etc.) apply immediately,
 * so a caller reading piece state afterwards sees the script's work. */
int  Cob_RunScriptSync(CobEngine *e, const char *name,
                        int32_t *args_inout, int n_args);

/* Run one sim tick: every alive thread executes until it yields
 * (SLEEP, WAIT, RETURN) or hits the COB_OPS_PER_TICK_LIMIT. Sleeping
 * threads decrement their counter. Wait conditions are checked before
 * dispatch — a thread whose wait condition is met wakes up.
 *
 * Caller should call Cob_AnimatePieces before this each tick so wait
 * conditions can be re-evaluated against fresh piece state. */
void Cob_RunAllThreads(CobEngine *e);

/* Animate piece state: per piece per axis, advance current toward
 * target by the per-tick delta (already pre-divided when the
 * ROTATE/MOVE opcode stored the speed). When a target is reached,
 * speed is cleared to 0 (which is also how WAIT-FOR-TURN/MOVE
 * detects completion).
 *
 * Call once per sim tick BEFORE Cob_RunAllThreads. */
void Cob_AnimatePieces(CobEngine *e);

/* Number of alive threads. For tests / diagnostics. */
int  Cob_AliveThreadCount(const CobEngine *e);

/* Whether thread slot N is currently alive (still executing). Used by
 * units.c to detect when walk-cycle scripts have terminated and need
 * restarting while a unit is still moving. */
int  Cob_IsThreadAlive(const CobEngine *e, int slot);

/* Read the final value left on a thread's stack when its outermost
 * RETURN ended. Returns 1 when a value is available, 0 otherwise.
 * Dead slots retain the value until the slot is reused. */
int  Cob_GetThreadReturn(const CobEngine *e, int slot, int32_t *out_value);

/* Read back one of the args a thread was started with. Scripts return
 * out-params by writing their arg locals, so this is how an async
 * script's answer is collected once it has run: Killed writes the
 * corpse type into arg 1 (legacy:227142). Returns 1 on success. */
int  Cob_GetThreadArg(const CobEngine *e, int slot, int arg_idx,
                       int32_t *out_value);

/* Run one live thread now, until it yields or ends, without waiting
 * for the tick. Killed is invoked this way at the kill instant so its
 * corpse out-param can be read straight back, and the thread lives
 * on through later ticks if it slept (legacy:227142, 306199). */
void Cob_RunThreadNow(CobEngine *e, int slot);

/* Diagnostics used by corpus tests and tooling. Unknown opcodes are
 * counted even when duplicate values are log-suppressed. */
void Cob_ResetDiagnostics(void);
int  Cob_GetUnknownOpcodeCount(void);

#endif /* TAK_COB_VM_H */
