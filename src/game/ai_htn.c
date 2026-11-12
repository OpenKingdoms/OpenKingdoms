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
    W_CUT_OFF,
    W_FERRY_READY,
    W_FERRY_COMING,
    W_HOLD,
    W_COUNT
};

enum {
    T_ARMY = 0,     /* the root */
    T_ATTACK,
    T_CROSS,        /* an attack over water (A-013) */
    P_HOLD,
    P_MASS,
    P_SCOUT,
    P_STRIKE,
    P_FALL_BACK,
    P_RAID,
    P_FERRY,
    T_COUNT
};

/* The methods of each task in the order they are tried.
 *
 * An army with members in the field facing more than twice their own
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
 * scout or a raid is not sent while another member is out.
 *
 * A target over water is carried to while a transport is free or coming:
 * a full hold goes aboard on a strike's strength, a launch's worth at
 * half the guard's where it lands when the long wait is up, and
 * otherwise the wave waits for it (A-013). */
static const HtnTask k_tasks[T_COUNT] = {
    [T_ARMY] = {
        .name = "army", .compound = 1, .method_count = 5,
        .methods = {
            { .name = "break off",
              .pre = { AI_COND(W_FIELD, AI_OP_GT, 0),
                       AI_COND(W_SIEGE_DUE, AI_OP_EQ, 0),
                       AI_COND_SCALED(W_FIELD_THREAT, 1, AI_OP_GT,
                                      W_FIELD_VALUE, 2) },
              .subtasks = { P_FALL_BACK, P_MASS }, .subtask_count = 2 },
            { .name = "over the water",
              .pre = { AI_COND(W_CUT_OFF, AI_OP_NE, 0),
                       AI_COND(W_FERRY_COMING, AI_OP_NE, 0) },
              .subtasks = { T_CROSS }, .subtask_count = 1 },
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
    [T_CROSS] = {
        .name = "cross", .compound = 1, .method_count = 3,
        .methods = {
            { .name = "ferry a full hold",
              .pre = { AI_COND(W_FERRY_READY, AI_OP_NE, 0),
                       AI_COND_VAR(W_MASSED, AI_OP_GE, W_HOLD),
                       AI_COND_SCALED(W_WAVE_VALUE, 5, AI_OP_GE,
                                      W_ENEMY_AT_TARGET, 6) },
              .subtasks = { P_FERRY }, .subtask_count = 1 },
            { .name = "ferry after the long wait",
              .pre = { AI_COND(W_FERRY_READY, AI_OP_NE, 0),
                       AI_COND(W_SIEGE_DUE, AI_OP_NE, 0),
                       AI_COND_VAR(W_MASSED, AI_OP_GE, W_LAUNCH),
                       AI_COND_SCALED(W_WAVE_VALUE, 2, AI_OP_GE,
                                      W_ENEMY_AT_TARGET, 1) },
              .subtasks = { P_FERRY }, .subtask_count = 1 },
            { .name = "wait for a boat",
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
    [P_FERRY] = {
        .name = "ferry",
        .eff = { AI_EFFECT(W_MASSED, AI_EFF_SET, 0),
                 AI_EFFECT(W_MARCHING, AI_EFF_SET, 1) },
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
    case P_FERRY:     return AI_TASK_FERRY;
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
    case AI_TASK_EVADE:     return "evade";
    case AI_TASK_ESCORT:    return "escort";
    case AI_TASK_HOLD_GROUND: return "hold the ground";
    case AI_TASK_DEFEND:    return "defend";
    case AI_TASK_FERRY:     return "ferry";
    case AI_TASK_HUNT:      return "hunt";
    case AI_TASK_SHELL:     return "shell";
    case AI_TASK_STAND:     return "stand";
    case AI_TASK_GO_HOME:   return "go home";
    case AI_TASK_ON_WAY_HOME: return "on its way home";
    case AI_TASK_CLOSE_IN:  return "close in";
    case AI_TASK_ARM_PRIMARY: return "primary";
    case AI_TASK_ARM_SECONDARY: return "secondary";
    case AI_TASK_ARM_SPECIAL: return "special";
    case AI_TASK_KEEP_ARMS: return "keep";
    case AI_TASK_LET_GO:    return "let go";
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
    w[W_CUT_OFF]         = s->cut_off ? 1 : 0;
    w[W_FERRY_READY]     = s->ferry_ready ? 1 : 0;
    w[W_FERRY_COMING]    = s->ferry_coming ? 1 : 0;
    w[W_HOLD]            = s->hold > s->launch ? s->hold : s->launch;

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

/* A group in the field (A-012): answer a threat, cover the builder, keep
 * at a live target, then press on from where it stands (legacy:18343) at a
 * target it sees and outweighs by a fifth, or after the long wait, or hold. */
enum {
    G_ESCORTING = 0, G_ERRAND_DONE, G_NEXT_KNOWN, G_NEXT_SEEN, G_VALUE,
    G_ENEMY_AT_NEXT, G_SIEGE_DUE, G_THREAT, G_COUNT
};
enum { T_GROUP = 0, P_ESCORT, P_PRESS_ON, P_HOLD_GROUND, P_DEFEND, GT_COUNT };

static const HtnTask k_group_tasks[GT_COUNT] = {
    [T_GROUP] = {
        .name = "group", .compound = 1, .method_count = 6,
        .methods = {
            { .name = "answer the threat",
              .pre = { AI_COND(G_THREAT, AI_OP_NE, 0) },
              .subtasks = { P_DEFEND }, .subtask_count = 1 },
            { .name = "cover the builder",
              .pre = { AI_COND(G_ESCORTING, AI_OP_NE, 0),
                       AI_COND(G_ERRAND_DONE, AI_OP_EQ, 0) },
              .subtasks = { P_ESCORT }, .subtask_count = 1 },
            { .name = "keep at it",
              .pre = { AI_COND(G_ERRAND_DONE, AI_OP_EQ, 0) },
              .subtasks = { P_PRESS_ON }, .subtask_count = 1 },
            { .name = "press on",
              .pre = { AI_COND(G_NEXT_SEEN, AI_OP_NE, 0),
                       AI_COND_SCALED(G_VALUE, 5, AI_OP_GE, G_ENEMY_AT_NEXT, 6) },
              .subtasks = { P_PRESS_ON }, .subtask_count = 1 },
            { .name = "the long wait is up",
              .pre = { AI_COND(G_NEXT_KNOWN, AI_OP_NE, 0),
                       AI_COND(G_SIEGE_DUE, AI_OP_NE, 0) },
              .subtasks = { P_PRESS_ON }, .subtask_count = 1 },
            { .name = "hold the ground",
              .subtasks = { P_HOLD_GROUND }, .subtask_count = 1 },
        },
    },
    [P_ESCORT]      = { .name = "escort" },
    [P_PRESS_ON]    = { .name = "press on" },
    [P_HOLD_GROUND] = { .name = "hold the ground" },
    [P_DEFEND]      = { .name = "defend" },
};

static const HtnDomain k_group = { k_group_tasks, GT_COUNT, G_COUNT };

AiTask AI_Htn_GroupTask(const AiGroupState *s, const char **reason) {
    if (reason) *reason = "no state";
    if (!s) return AI_TASK_HOLD_GROUND;
    int32_t w[G_COUNT];
    w[G_ESCORTING]     = s->escorting ? 1 : 0;
    w[G_ERRAND_DONE]   = s->errand_done ? 1 : 0;
    w[G_NEXT_KNOWN]    = s->next_known ? 1 : 0;
    w[G_NEXT_SEEN]     = s->next_known && s->next_seen ? 1 : 0;
    w[G_VALUE]         = s->value;
    w[G_ENEMY_AT_NEXT] = s->enemy_at_next;
    w[G_SIEGE_DUE]     = s->siege_due ? 1 : 0;
    w[G_THREAT]        = s->threat ? 1 : 0;
    HtnPlan plan;
    if (!Htn_Plan(&k_group, T_GROUP, w, &plan) || plan.step_count <= 0)
        return AI_TASK_HOLD_GROUND;
    if (reason && plan.via_count > 0)
        *reason = k_group_tasks[T_GROUP].methods[plan.via_method[0]].name;
    switch (plan.steps[0]) {
    case P_ESCORT:   return AI_TASK_ESCORT;
    case P_PRESS_ON: return AI_TASK_STRIKE;
    case P_DEFEND:   return AI_TASK_DEFEND;
    default:         return AI_TASK_HOLD_GROUND;
    }
}

/* A seat's warships: a domain of their own. Enemy ships off home are
 * answered whatever they weigh, a fleet a fifth stronger goes out to the
 * enemy's, and with no ships to fight it stands off a shore and shells it.
 * Otherwise it waits (A-015). */
enum { S_SHIPS = 0, S_VALUE, S_ENEMY_SHIPS, S_ENEMY_VALUE, S_NEAR_HOME, S_MARK_KNOWN, S_COUNT };
enum { T_FLEET = 0, P_HUNT, P_SHELL, P_STAND, ST_COUNT };

static const HtnTask k_fleet_tasks[ST_COUNT] = {
    [T_FLEET] = {
        .name = "fleet", .compound = 1, .method_count = 4,
        .methods = {
            { .name = "answer ships off home",
              .pre = { AI_COND(S_SHIPS, AI_OP_GT, 0),
                       AI_COND(S_NEAR_HOME, AI_OP_NE, 0) },
              .subtasks = { P_HUNT }, .subtask_count = 1 },
            { .name = "hunt the enemy's ships",
              .pre = { AI_COND(S_SHIPS, AI_OP_GE, AI_FLEET_SORTIE),
                       AI_COND(S_ENEMY_SHIPS, AI_OP_GT, 0),
                       AI_COND_SCALED(S_VALUE, 5, AI_OP_GE, S_ENEMY_VALUE, 6) },
              .subtasks = { P_HUNT }, .subtask_count = 1 },
            { .name = "shell the shore",
              .pre = { AI_COND(S_SHIPS, AI_OP_GE, AI_FLEET_SORTIE),
                       AI_COND(S_ENEMY_SHIPS, AI_OP_EQ, 0),
                       AI_COND(S_MARK_KNOWN, AI_OP_NE, 0) },
              .subtasks = { P_SHELL }, .subtask_count = 1 },
            { .name = "stand by",
              .subtasks = { P_STAND }, .subtask_count = 1 },
        },
    },
    [P_HUNT]  = { .name = "hunt" },
    [P_SHELL] = { .name = "shell" },
    [P_STAND] = { .name = "stand" },
};

static const HtnDomain k_fleet = { k_fleet_tasks, ST_COUNT, S_COUNT };

AiTask AI_Htn_FleetTask(const AiFleetState *s, const char **reason) {
    if (reason) *reason = "no state";
    if (!s) return AI_TASK_STAND;
    int32_t w[S_COUNT];
    w[S_SHIPS]       = s->ships;
    w[S_VALUE]       = s->value;
    w[S_ENEMY_SHIPS] = s->enemy_ships;
    w[S_ENEMY_VALUE] = s->enemy_value;
    w[S_NEAR_HOME]   = s->near_home ? 1 : 0;
    w[S_MARK_KNOWN]  = s->mark_known ? 1 : 0;
    HtnPlan plan;
    if (!Htn_Plan(&k_fleet, T_FLEET, w, &plan) || plan.step_count <= 0) return AI_TASK_STAND;
    if (reason && plan.via_count > 0)
        *reason = k_fleet_tasks[T_FLEET].methods[plan.via_method[0]].name;
    switch (plan.steps[0]) {
    case P_HUNT:  return AI_TASK_HUNT;
    case P_SHELL: return AI_TASK_SHELL;
    default:      return AI_TASK_STAND;
    }
}

/* A unit's footing: a domain of its own in the same planner. A unit
 * that stands in a fire that hurts and can walk steps out of it first
 * (A-011). Anything else keeps to what it was doing. */
enum { F_IN_FIRE = 0, F_CAN_MOVE, F_COUNT };
enum { T_FOOTING = 0, P_EVADE, P_KEEP, FT_COUNT };

static const HtnTask k_footing_tasks[FT_COUNT] = {
    [T_FOOTING] = {
        .name = "footing", .compound = 1, .method_count = 2,
        .methods = {
            { .name = "step out of the fire",
              .pre = { AI_COND(F_IN_FIRE, AI_OP_NE, 0),
                       AI_COND(F_CAN_MOVE, AI_OP_NE, 0) },
              .subtasks = { P_EVADE }, .subtask_count = 1 },
            { .name = "keep on",
              .subtasks = { P_KEEP }, .subtask_count = 1 },
        },
    },
    [P_EVADE] = { .name = "evade" },
    [P_KEEP]  = { .name = "keep on" },
};

static const HtnDomain k_footing = { k_footing_tasks, FT_COUNT, F_COUNT };

AiTask AI_Htn_FootingTask(const AiFootingState *s) {
    if (!s) return AI_TASK_NONE;
    int32_t w[F_COUNT];
    w[F_IN_FIRE] = s->in_fire ? 1 : 0;
    w[F_CAN_MOVE] = s->can_move ? 1 : 0;
    HtnPlan plan;
    if (!Htn_Plan(&k_footing, T_FOOTING, w, &plan) || plan.step_count <= 0)
        return AI_TASK_NONE;
    return plan.steps[0] == P_EVADE ? AI_TASK_EVADE : AI_TASK_NONE;
}

/* A monarch the game rides on turns for home past its ground or on a chase
 * off it, and away from home outmatched, shot or hurt. At home it fights,
 * and idle under fire it cannot answer closes on the shooter (A-017). */
enum { M_OUTSIDE = 0, M_BUILDING_IN, M_CHASE_OUT, M_OUTMATCHED, M_HURT, M_AT_CORE,
       M_HEADING_HOME, M_UNDER_FIRE, M_IDLE, M_ANSWERABLE, M_CLOSING, M_COUNT };
enum { T_MONARCH = 0, T_AWAY, T_HOMEWARD, T_ANSWER, P_GO_HOME, P_ON_WAY, P_CLOSE_IN,
       P_FREE, MT_COUNT };

static const HtnTask k_monarch_tasks[MT_COUNT] = {
    [T_MONARCH] = {
        .name = "monarch", .compound = 1, .method_count = 5,
        .methods = {
            { .name = "past its ground",
              .pre = { AI_COND(M_OUTSIDE, AI_OP_NE, 0),
                       AI_COND(M_BUILDING_IN, AI_OP_EQ, 0) },
              .subtasks = { T_HOMEWARD }, .subtask_count = 1 },
            { .name = "a chase leads off its ground",
              .pre = { AI_COND(M_CHASE_OUT, AI_OP_NE, 0) },
              .subtasks = { T_HOMEWARD }, .subtask_count = 1 },
            { .name = "away from the heart of home",
              .pre = { AI_COND(M_AT_CORE, AI_OP_EQ, 0) },
              .subtasks = { T_AWAY }, .subtask_count = 1 },
            { .name = "a shot it cannot answer",
              .pre = { AI_COND(M_UNDER_FIRE, AI_OP_NE, 0),
                       AI_COND(M_ANSWERABLE, AI_OP_NE, 0),
                       AI_COND(M_IDLE, AI_OP_NE, 0) },
              .subtasks = { T_ANSWER }, .subtask_count = 1 },
            { .name = "free",
              .subtasks = { P_FREE }, .subtask_count = 1 },
        },
    },
    [T_AWAY] = {
        .name = "away", .compound = 1, .method_count = 4,
        .methods = {
            { .name = "outmatched away from home",
              .pre = { AI_COND(M_OUTMATCHED, AI_OP_NE, 0) },
              .subtasks = { T_HOMEWARD }, .subtask_count = 1 },
            { .name = "under fire away from home",
              .pre = { AI_COND(M_UNDER_FIRE, AI_OP_NE, 0) },
              .subtasks = { T_HOMEWARD }, .subtask_count = 1 },
            { .name = "hurt away from home",
              .pre = { AI_COND(M_HURT, AI_OP_NE, 0) },
              .subtasks = { T_HOMEWARD }, .subtask_count = 1 },
            { .name = "free",
              .subtasks = { P_FREE }, .subtask_count = 1 },
        },
    },
    [T_HOMEWARD] = {
        .name = "homeward", .compound = 1, .method_count = 2,
        .methods = {
            { .name = "on its way",
              .pre = { AI_COND(M_HEADING_HOME, AI_OP_NE, 0) },
              .subtasks = { P_ON_WAY }, .subtask_count = 1 },
            { .name = "turn for home",
              .subtasks = { P_GO_HOME }, .subtask_count = 1 },
        },
    },
    [T_ANSWER] = {
        .name = "answer", .compound = 1, .method_count = 2,
        .methods = {
            { .name = "closing",
              .pre = { AI_COND(M_CLOSING, AI_OP_NE, 0) },
              .subtasks = { P_ON_WAY }, .subtask_count = 1 },
            { .name = "close on the shooter",
              .subtasks = { P_CLOSE_IN }, .subtask_count = 1 },
        },
    },
    [P_GO_HOME]  = { .name = "go home" },
    [P_ON_WAY]   = { .name = "on its way" },
    [P_CLOSE_IN] = { .name = "close in" },
    [P_FREE]     = { .name = "free" },
};

static const HtnDomain k_monarch = { k_monarch_tasks, MT_COUNT, M_COUNT };

AiTask AI_Htn_MonarchTask(const AiMonarchState *s, const char **reason) {
    if (reason) *reason = "no state";
    if (!s) return AI_TASK_NONE;
    int32_t w[M_COUNT];
    w[M_OUTSIDE]      = s->outside ? 1 : 0;
    w[M_BUILDING_IN]  = s->building_in ? 1 : 0;
    w[M_CHASE_OUT]    = s->chase_out ? 1 : 0;
    w[M_OUTMATCHED]   = s->outmatched ? 1 : 0;
    w[M_HURT]         = s->hurt ? 1 : 0;
    w[M_AT_CORE]      = s->at_core ? 1 : 0;
    w[M_HEADING_HOME] = s->heading_home ? 1 : 0;
    w[M_UNDER_FIRE]   = s->under_fire ? 1 : 0;
    w[M_IDLE]         = s->idle ? 1 : 0;
    w[M_ANSWERABLE]   = s->answerable ? 1 : 0;
    w[M_CLOSING]      = s->closing ? 1 : 0;
    HtnPlan plan;
    if (!Htn_Plan(&k_monarch, T_MONARCH, w, &plan) || plan.step_count <= 0)
        return AI_TASK_NONE;
    if (reason && plan.via_count > 0) {
        int k = plan.via_count > 1 && plan.via_task[1] == T_AWAY ? 1 : 0;
        *reason = k_monarch_tasks[plan.via_task[k]].methods[plan.via_method[k]].name;
    }
    switch (plan.steps[0]) {
    case P_GO_HOME:  return AI_TASK_GO_HOME;
    case P_ON_WAY:   return AI_TASK_ON_WAY_HOME;
    case P_CLOSE_IN: return AI_TASK_CLOSE_IN;
    default:         return AI_TASK_NONE;
    }
}

/* The original's choice of attack (legacy:19008-19242): the special, then
 * the secondary, when wanted and the primary would not finish the target,
 * else the primary. With nothing in its sights a dear one may be put away. */
enum { A_TARGET = 0, A_HELD, A_HELD_KEPT, A_SECONDARY, A_SPECIAL, A_SECONDARY_SPARED,
       A_SPECIAL_SPARED, A_COUNT };
enum { T_ARMS = 0, T_IDLE, T_CHOOSE, P_PRIMARY, P_SECONDARY, P_SPECIAL, P_KEEP_ARMS,
       P_LET_GO, AT_COUNT };

static const HtnTask k_arms_tasks[AT_COUNT] = {
    [T_ARMS] = {
        .name = "arms", .compound = 1, .method_count = 2,
        .methods = {
            { .name = "nothing in its sights",
              .pre = { AI_COND(A_TARGET, AI_OP_EQ, 0) },
              .subtasks = { T_IDLE }, .subtask_count = 1 },
            { .name = "a target",
              .subtasks = { T_CHOOSE }, .subtask_count = 1 },
        },
    },
    [T_IDLE] = {
        .name = "idle", .compound = 1, .method_count = 2,
        .methods = {
            { .name = "a dear attack in hand",
              .pre = { AI_COND(A_HELD, AI_OP_GT, 0),
                       AI_COND(A_HELD_KEPT, AI_OP_EQ, 0) },
              .subtasks = { P_LET_GO }, .subtask_count = 1 },
            { .name = "keep what is in hand",
              .subtasks = { P_KEEP_ARMS }, .subtask_count = 1 },
        },
    },
    [T_CHOOSE] = {
        .name = "choose", .compound = 1, .method_count = 5,
        .methods = {
            { .name = "the primary finishes it",
              .pre = { AI_COND(A_SPECIAL, AI_OP_NE, 0),
                       AI_COND(A_SPECIAL_SPARED, AI_OP_NE, 0) },
              .subtasks = { P_KEEP_ARMS }, .subtask_count = 1 },
            { .name = "the special serves",
              .pre = { AI_COND(A_SPECIAL, AI_OP_NE, 0) },
              .subtasks = { P_SPECIAL }, .subtask_count = 1 },
            { .name = "the primary finishes it",
              .pre = { AI_COND(A_SECONDARY, AI_OP_NE, 0),
                       AI_COND(A_SECONDARY_SPARED, AI_OP_NE, 0) },
              .subtasks = { P_KEEP_ARMS }, .subtask_count = 1 },
            { .name = "the secondary serves",
              .pre = { AI_COND(A_SECONDARY, AI_OP_NE, 0) },
              .subtasks = { P_SECONDARY }, .subtask_count = 1 },
            { .name = "the primary",
              .subtasks = { P_PRIMARY }, .subtask_count = 1 },
        },
    },
    [P_PRIMARY]   = { .name = "primary" },
    [P_SECONDARY] = { .name = "secondary" },
    [P_SPECIAL]   = { .name = "special" },
    [P_KEEP_ARMS] = { .name = "keep" },
    [P_LET_GO]    = { .name = "let go" },
};

static const HtnDomain k_arms = { k_arms_tasks, AT_COUNT, A_COUNT };

AiTask AI_Htn_WeaponTask(const AiWeaponState *s, const char **reason) {
    if (reason) *reason = "no state";
    if (!s) return AI_TASK_KEEP_ARMS;
    int32_t w[A_COUNT];
    w[A_TARGET]           = s->target ? 1 : 0;
    w[A_HELD]             = s->held;
    w[A_HELD_KEPT]        = s->held_kept ? 1 : 0;
    w[A_SECONDARY]        = s->secondary_wanted ? 1 : 0;
    w[A_SPECIAL]          = s->special_wanted ? 1 : 0;
    w[A_SECONDARY_SPARED] = s->secondary_spared ? 1 : 0;
    w[A_SPECIAL_SPARED]   = s->special_spared ? 1 : 0;
    HtnPlan plan;
    if (!Htn_Plan(&k_arms, T_ARMS, w, &plan) || plan.step_count <= 0)
        return AI_TASK_KEEP_ARMS;
    if (reason && plan.via_count > 1)
        *reason = k_arms_tasks[plan.via_task[1]].methods[plan.via_method[1]].name;
    switch (plan.steps[0]) {
    case P_PRIMARY:   return AI_TASK_ARM_PRIMARY;
    case P_SECONDARY: return AI_TASK_ARM_SECONDARY;
    case P_SPECIAL:   return AI_TASK_ARM_SPECIAL;
    case P_LET_GO:    return AI_TASK_LET_GO;
    default:          return AI_TASK_KEEP_ARMS;
    }
}
