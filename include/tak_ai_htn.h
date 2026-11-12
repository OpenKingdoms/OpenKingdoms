#ifndef TAK_AI_HTN_H
#define TAK_AI_HTN_H

#include <stdint.h>

/* The tactical layer: what a seat's army does about the goal planner's
 * attack goal. The planning is tak_ai_tasknet.h's. This is the army's
 * domain in it: the wave state the caller reads off the unit list, the
 * fog and the influence maps, the tasks an army can be on, and what
 * one member does under the plan. The caller turns that into the
 * orders it already gives. Integers only, no randomness. */

/* A wave never waits for more than this many members. */
#define AI_HTN_MAX_LAUNCH 8
/* The most members one raid takes from the gathering. */
#define AI_HTN_RAID_SIZE  2

typedef enum {
    AI_TASK_NONE = 0,
    AI_TASK_HOLD,       /* nothing to go at, stay put */
    AI_TASK_MASS,       /* wait at the staging point for the wave */
    AI_TASK_SCOUT,      /* one member goes to look at the target */
    AI_TASK_STRIKE,     /* go at the target */
    AI_TASK_FALL_BACK,  /* members in the field come home */
    AI_TASK_RAID,       /* a few fast members go at the enemy's economy */
    AI_TASK_EVADE,      /* a unit steps off ground that hurts it */
    AI_TASK_ESCORT,     /* a group covers a builder on its way to a pad */
    AI_TASK_HOLD_GROUND,/* a group stands on the ground it went out to */
    AI_TASK_DEFEND,     /* a group goes at what attacks the seat's own */
    AI_TASK_FERRY,      /* a transport carries the wave over the water */
    AI_TASK_HUNT,       /* warships go at the enemy's ships */
    AI_TASK_SHELL,      /* warships stand off the enemy's shore and fire */
    AI_TASK_STAND,      /* warships wait at their berths */
    AI_TASK_GO_HOME,    /* a monarch turns for home */
    AI_TASK_ON_WAY_HOME,/* and keeps on its way there */
    AI_TASK_CLOSE_IN,   /* a monarch closes on a shooter it cannot answer */
    AI_TASK_ARM_PRIMARY,  /* a unit takes up its first attack */
    AI_TASK_ARM_SECONDARY,
    AI_TASK_ARM_SPECIAL,
    AI_TASK_KEEP_ARMS,  /* it keeps the attack in hand */
    AI_TASK_LET_GO,     /* with nothing to shoot it may put a dear one away */
    AI_TASK_COUNT
} AiTask;

/* A cut off seat's warships, read once a think (A-015). */
#define AI_FLEET_SORTIE 2       /* ships that go out together */
typedef struct AiFleetState {
    int     ships;              /* free armed ships */
    int32_t value;              /* their strength */
    int     enemy_ships;        /* enemy ships seen on water they can sail */
    int32_t enemy_value;        /* their strength */
    int     near_home;          /* one of those is off the seat's home */
    int     mark_known;         /* an enemy building can be shelled from water */
} AiFleetState;
AiTask AI_Htn_FleetTask(const AiFleetState *s, const char **reason);

typedef enum {
    AI_ROLE_MEMBER = 0,
    AI_ROLE_SCOUT,      /* the member the caller picked to look */
    AI_ROLE_RAIDER      /* a member the caller picked to raid */
} AiRole;

typedef struct AiWaveState {
    int target_known;   /* the seat has a wave target */
    int target_seen;    /* and can see it now */
    int marching;       /* a member is already out on a march */
    int members;        /* mobile combat units the seat fields */
    int massed;         /* of them, idle at the staging point */
    int launch;         /* massed members the wave strikes at */
    int patience_due;   /* the wait is up, go with what is here */
    /* Strength, in the influence maps' combat value. A zero is a thing
     * not known, and nothing is held back on it. */
    int32_t wave_value;       /* the massed members */
    int32_t enemy_at_target;  /* seen enemy strength around the target */
    int     field;            /* members away from the staging point */
    int32_t field_value;      /* their strength */
    int32_t field_threat;     /* seen enemy strength where they stand */
    int     raid_known;       /* a weak, valuable enemy cell is known */
    int     siege_due;        /* the long wait is up, go whatever waits */
    /* The target lies over water some walker of the seat cannot cross,
     * a transport of the seat is free to carry the wave there, and one is
     * free or on its way (A-013). */
    int     cut_off;
    int     ferry_ready;
    int     ferry_coming;
    int     hold;             /* massed members a ferry waits for */
} AiWaveState;

#define AI_HTN_MAX_STEPS 4

typedef struct AiWavePlan {
    AiTask      steps[AI_HTN_MAX_STEPS];
    int         step_count;
    /* The method the attack was taken apart by, for the trace. */
    const char *reason;
} AiWavePlan;

/* Members a wave waits for, given what the seat fields. */
int AI_Htn_LaunchCount(int members);

/* Plan the army's tasks. Always gives at least one step. */
void AI_Htn_Plan(const AiWaveState *s, AiWavePlan *out);

/* The task the wave as a whole is on: the plan's first step. */
AiTask AI_Htn_WaveTask(const AiWaveState *s);

/* What one member does under a plan. at_stage says it stands at the
 * staging point. */
AiTask AI_Htn_MemberTaskIn(const AiWavePlan *plan, AiRole role, int at_stage);
AiTask AI_Htn_MemberTask(const AiWaveState *s, int is_scout, int at_stage);

const char *AI_Htn_TaskName(AiTask task);

/* A group out in the field, asked each think: what it does once its
 * errand is done, and while it covers a builder. Strength is in the
 * influence maps' combat value, a zero a thing not known. */
typedef struct AiGroupState {
    int     escorting;      /* it covers a builder on its way to a pad */
    int     errand_done;    /* its target fell, or its builder's work ended */
    int     next_known;     /* a next target is known from where it stands */
    int     next_seen;      /* and the seat can see it now */
    int32_t value;          /* the group's strength */
    int32_t enemy_at_next;  /* seen enemy strength around that target */
    int     siege_due;      /* the long wait is up, go whatever waits */
    int     threat;         /* something of the seat's is under attack */
} AiGroupState;

/* AI_TASK_DEFEND, AI_TASK_ESCORT, AI_TASK_STRIKE or AI_TASK_HOLD_GROUND,
 * and the method it was planned by when reason is not NULL. */
AiTask AI_Htn_GroupTask(const AiGroupState *s, const char **reason);

/* A unit's footing, asked before anything else it would do: whether the
 * ground it stands on hurts it, under the remastered rules a burning
 * feature's reach (D-036). */
typedef struct AiFootingState {
    int in_fire;     /* a fire that hurts reaches where it stands */
    int can_move;
} AiFootingState;

/* AI_TASK_EVADE when the unit should step away first, else AI_TASK_NONE. */
AiTask AI_Htn_FootingTask(const AiFootingState *s);

/* A monarch whose loss loses the game, asked each think before its
 * builds (A-017). Its ground is home and what lies near it. */
typedef struct AiMonarchState {
    int outside;        /* it stands past its ground */
    int building_in;    /* on a build the seat sited inside its ground */
    int chase_out;      /* its target is past its ground and its reach */
    int outmatched;     /* seen enemy strength where it stands outweighs ours */
    int hurt;           /* under half its hit points with the enemy near */
    int at_core;        /* it stands at the heart of home */
    int heading_home;   /* it is already walking there */
    int under_fire;     /* hit by what it does not see or cannot reach */
    int idle;           /* it has no order */
    int answerable;     /* the shooter stands on its ground */
    int closing;        /* already walking to where it would see the shooter */
} AiMonarchState;

/* AI_TASK_GO_HOME, AI_TASK_CLOSE_IN, AI_TASK_ON_WAY_HOME, or
 * AI_TASK_NONE when it is free to build and fight. */
AiTask AI_Htn_MonarchTask(const AiMonarchState *s, const char **reason);

/* A computer's unit that can switch attacks, asked each think which to
 * hold (legacy:19008-19242). Wanted is paid for, able to hurt the target
 * and serving it: see ai_arms_wanted in ai.c. */
typedef struct AiWeaponState {
    int target;           /* a live enemy is in its sights */
    int held;             /* the slot in hand */
    int held_kept;        /* the one in hand is cheap and plain, kept idle */
    int secondary_wanted;
    int special_wanted;
    /* The primary in hand would finish the target in two blows. */
    int secondary_spared;
    int special_spared;
} AiWeaponState;

/* AI_TASK_ARM_PRIMARY, _SECONDARY or _SPECIAL, AI_TASK_KEEP_ARMS, or
 * AI_TASK_LET_GO, which the caller takes one think in ten. */
AiTask AI_Htn_WeaponTask(const AiWeaponState *s, const char **reason);

#endif /* TAK_AI_HTN_H */
