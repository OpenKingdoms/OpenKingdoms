#ifndef TAK_AI_HTN_H
#define TAK_AI_HTN_H

/* The tactical layer: the goal planner's attack goal decomposed into
 * scout, mass, strike and hold. Pure. The caller reads the wave state
 * off the unit list and the fog, asks what the wave and one member are
 * doing, and turns that into the orders it already gives. Integers
 * only, fixed task order, no randomness. */

/* A wave never waits for more than this many members. */
#define AI_HTN_MAX_LAUNCH 8

typedef enum {
    AI_TASK_NONE = 0,
    AI_TASK_HOLD,     /* nothing to go at, stay put */
    AI_TASK_MASS,     /* wait at the staging point for the wave */
    AI_TASK_SCOUT,    /* one member goes to look at the target */
    AI_TASK_STRIKE,   /* go at the target */
    AI_TASK_COUNT
} AiTask;

typedef struct AiWaveState {
    int target_known;   /* the seat has a wave target */
    int target_seen;    /* and can see it now */
    int marching;       /* a member is already out on a march */
    int members;        /* mobile combat units the seat fields */
    int massed;         /* of them, idle at the staging point */
    int launch;         /* massed members the wave strikes at */
    int patience_due;   /* the wait is up, go with what is here */
} AiWaveState;

/* Members a wave waits for, given what the seat fields. */
int AI_Htn_LaunchCount(int members);

/* The task the wave as a whole is on. */
AiTask AI_Htn_WaveTask(const AiWaveState *s);

/* What one member does under that task. is_scout marks the member the
 * caller picked to look, at_stage that it stands at the staging
 * point. */
AiTask AI_Htn_MemberTask(const AiWaveState *s, int is_scout, int at_stage);

#endif /* TAK_AI_HTN_H */
