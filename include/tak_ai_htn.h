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
    AI_TASK_COUNT
} AiTask;

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

#endif /* TAK_AI_HTN_H */
