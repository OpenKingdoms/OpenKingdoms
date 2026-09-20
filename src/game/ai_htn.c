#include "tak_ai_htn.h"
#include "tak_ai_tasknet.h"

#include <string.h>

/* A wave waits for a third of what the seat fields, at least one so a
 * seat with a single unit still presses, and never more than
 * AI_HTN_MAX_LAUNCH. The original ratchets a group's launch threshold
 * on what its owner holds (legacy:16187). */
int AI_Htn_LaunchCount(int members) {
    int n = 1 + (members > 0 ? members : 0) / 3;
    return n > AI_HTN_MAX_LAUNCH ? AI_HTN_MAX_LAUNCH : n;
}

/* The army's world, one integer each. */
enum {
    W_TARGET_KNOWN = 0,
    W_TARGET_SEEN,
    W_MARCHING,
    W_MASSED,
    W_LAUNCH,
    W_PATIENCE_DUE,
    W_SIEGE_DUE,
    W_WAVE_VALUE,
    W_ENEMY_AT_TARGET,
    W_FIELD,
    W_FIELD_VALUE,
    W_FIELD_THREAT,
    W_RAID_KNOWN,
    W_COUNT
};

enum {
    T_ARMY = 0,     /* the root */
    T_ATTACK,
    P_HOLD,
    P_MASS,
    P_SCOUT,
    P_STRIKE,
    P_FALL_BACK,
    P_RAID,
    T_COUNT
};

/* The methods of each task in the order they are tried.
 *
 * An army with members in the field facing half again their own
 * strength breaks off and gathers, unless the long wait is up. An
 * army with a target attacks. Anything else holds.
 *
 * An attack strikes when the gathering is at strength in numbers and
 * is a fifth stronger than what it can see waiting for it. Out of
 * patience it goes on less, so long as it is not outnumbered two to
 * one, and when the long wait is up it goes whatever waits, because a
 * seat that never comes out loses to one that does. Short of all
 * that, a target nobody can see gets one member to look at it, a weak
 * and valuable corner of the enemy gets a raid while the wave
 * gathers, and otherwise the wave gathers. One march at a time: a
 * scout or a raid is not sent while another member is out. */
static const HtnTask k_tasks[T_COUNT] = {
    [T_ARMY] = {
        .name = "army", .compound = 1, .method_count = 3,
        .methods = {
            { .name = "break off",
              .pre = { AI_COND(W_FIELD, AI_OP_GT, 0),
                       AI_COND(W_SIEGE_DUE, AI_OP_EQ, 0),
                       AI_COND_SCALED(W_FIELD_THREAT, 2, AI_OP_GT,
                                      W_FIELD_VALUE, 3) },
              .subtasks = { P_FALL_BACK, P_MASS }, .subtask_count = 2 },
            { .name = "attack",
              .pre = { AI_COND(W_TARGET_KNOWN, AI_OP_NE, 0) },
              .subtasks = { T_ATTACK }, .subtask_count = 1 },
            { .name = "nothing to go at",
              .subtasks = { P_HOLD }, .subtask_count = 1 },
        },
    },
    [T_ATTACK] = {
        .name = "attack", .compound = 1, .method_count = 6,
        .methods = {
            { .name = "at strength",
              .pre = { AI_COND_VAR(W_MASSED, AI_OP_GE, W_LAUNCH),
                       AI_COND_SCALED(W_WAVE_VALUE, 5, AI_OP_GE,
                                      W_ENEMY_AT_TARGET, 6) },
              .subtasks = { P_STRIKE }, .subtask_count = 1 },
            { .name = "out of patience",
              .pre = { AI_COND(W_PATIENCE_DUE, AI_OP_NE, 0),
                       AI_COND_SCALED(W_WAVE_VALUE, 2, AI_OP_GE,
                                      W_ENEMY_AT_TARGET, 1) },
              .subtasks = { P_STRIKE }, .subtask_count = 1 },
            { .name = "the long wait is up",
              .pre = { AI_COND(W_SIEGE_DUE, AI_OP_NE, 0) },
              .subtasks = { P_STRIKE }, .subtask_count = 1 },
            { .name = "look first",
              .pre = { AI_COND(W_TARGET_SEEN, AI_OP_EQ, 0) },
              .subtasks = { P_SCOUT, P_MASS }, .subtask_count = 2 },
            { .name = "raid while gathering",
              .pre = { AI_COND(W_RAID_KNOWN, AI_OP_NE, 0) },
              .subtasks = { P_RAID, P_MASS }, .subtask_count = 2 },
            { .name = "gather",
              .subtasks = { P_MASS }, .subtask_count = 1 },
        },
    },
    [P_HOLD] = { .name = "hold" },
    [P_MASS] = { .name = "mass" },
    /* The scout is drawn only from a gathering of two or more, so a
     * seat never sends its last member away to look. */
    [P_SCOUT] = {
        .name = "scout",
        .pre = { AI_COND(W_MARCHING, AI_OP_EQ, 0),
                 AI_COND(W_MASSED, AI_OP_GE, 2) },
        .eff = { AI_EFFECT(W_MASSED, AI_EFF_SUB, 1),
                 AI_EFFECT(W_MARCHING, AI_EFF_SET, 1) },
    },
    [P_STRIKE] = {
        .name = "strike",
        .eff = { AI_EFFECT(W_MASSED, AI_EFF_SET, 0),
                 AI_EFFECT(W_MARCHING, AI_EFF_SET, 1) },
    },
    [P_FALL_BACK] = {
        .name = "fall back",
        .pre = { AI_COND(W_FIELD, AI_OP_GT, 0) },
        .eff = { AI_EFFECT(W_FIELD, AI_EFF_SET, 0) },
    },
    /* A raid leaves at least as many at home as it takes. */
    [P_RAID] = {
        .name = "raid",
        .pre = { AI_COND(W_MARCHING, AI_OP_EQ, 0),
                 AI_COND(W_MASSED, AI_OP_GE, 2 * AI_HTN_RAID_SIZE) },
        .eff = { AI_EFFECT(W_MASSED, AI_EFF_SUB, AI_HTN_RAID_SIZE),
                 AI_EFFECT(W_MARCHING, AI_EFF_SET, 1) },
    },
};

static const HtnDomain k_domain = { k_tasks, T_COUNT, W_COUNT };

static AiTask htn_task_of(int primitive) {
    switch (primitive) {
    case P_HOLD:      return AI_TASK_HOLD;
    case P_MASS:      return AI_TASK_MASS;
    case P_SCOUT:     return AI_TASK_SCOUT;
    case P_STRIKE:    return AI_TASK_STRIKE;
    case P_FALL_BACK: return AI_TASK_FALL_BACK;
    case P_RAID:      return AI_TASK_RAID;
    default:          return AI_TASK_NONE;
    }
}

const char *AI_Htn_TaskName(AiTask task) {
    switch (task) {
    case AI_TASK_HOLD:      return "hold";
    case AI_TASK_MASS:      return "mass";
    case AI_TASK_SCOUT:     return "scout";
    case AI_TASK_STRIKE:    return "strike";
    case AI_TASK_FALL_BACK: return "fall back";
    case AI_TASK_RAID:      return "raid";
    default:                return "none";
    }
}

void AI_Htn_Plan(const AiWaveState *s, AiWavePlan *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->steps[0] = AI_TASK_HOLD;
    out->step_count = 1;
    out->reason = "no state";
    if (!s) return;

    int32_t w[W_COUNT];
    w[W_TARGET_KNOWN]    = s->target_known ? 1 : 0;
    w[W_TARGET_SEEN]     = s->target_seen ? 1 : 0;
    w[W_MARCHING]        = s->marching ? 1 : 0;
    w[W_MASSED]          = s->massed;
    w[W_LAUNCH]          = s->launch;
    w[W_PATIENCE_DUE]    = s->patience_due ? 1 : 0;
    w[W_SIEGE_DUE]       = s->siege_due ? 1 : 0;
    w[W_WAVE_VALUE]      = s->wave_value;
    w[W_ENEMY_AT_TARGET] = s->enemy_at_target;
    w[W_FIELD]           = s->field;
    w[W_FIELD_VALUE]     = s->field_value;
    w[W_FIELD_THREAT]    = s->field_threat;
    w[W_RAID_KNOWN]      = s->raid_known ? 1 : 0;

    HtnPlan plan;
    if (!Htn_Plan(&k_domain, T_ARMY, w, &plan) || plan.step_count <= 0) {
        out->reason = "no plan";
        return;
    }
    out->step_count = 0;
    for (int i = 0; i < plan.step_count && i < AI_HTN_MAX_STEPS; i++)
        out->steps[out->step_count++] = htn_task_of(plan.steps[i]);
    if (plan.via_count > 0) {
        int last = plan.via_count - 1;
        out->reason = k_tasks[plan.via_task[last]].methods[plan.via_method[last]].name;
    }
}

AiTask AI_Htn_WaveTask(const AiWaveState *s) {
    AiWavePlan plan;
    AI_Htn_Plan(s, &plan);
    return plan.steps[0];
}

static int htn_plan_has(const AiWavePlan *plan, AiTask task) {
    for (int i = 0; i < plan->step_count; i++)
        if (plan->steps[i] == task) return 1;
    return 0;
}

/* A member away from the staging point is not called back to wait: it
 * goes on with the wave, and only the ones at home wait. It is called
 * back when the army breaks off. */
AiTask AI_Htn_MemberTaskIn(const AiWavePlan *plan, AiRole role, int at_stage) {
    if (!plan || plan->step_count <= 0) return AI_TASK_HOLD;
    if (plan->steps[0] == AI_TASK_HOLD) return AI_TASK_HOLD;
    if (htn_plan_has(plan, AI_TASK_STRIKE)) return AI_TASK_STRIKE;
    if (htn_plan_has(plan, AI_TASK_FALL_BACK))
        return at_stage ? AI_TASK_MASS : AI_TASK_FALL_BACK;
    if (role == AI_ROLE_SCOUT && htn_plan_has(plan, AI_TASK_SCOUT))
        return AI_TASK_SCOUT;
    if (role == AI_ROLE_RAIDER && htn_plan_has(plan, AI_TASK_RAID))
        return AI_TASK_RAID;
    return at_stage ? AI_TASK_MASS : AI_TASK_STRIKE;
}

AiTask AI_Htn_MemberTask(const AiWaveState *s, int is_scout, int at_stage) {
    AiWavePlan plan;
    AI_Htn_Plan(s, &plan);
    return AI_Htn_MemberTaskIn(&plan, is_scout ? AI_ROLE_SCOUT : AI_ROLE_MEMBER,
                               at_stage);
}
