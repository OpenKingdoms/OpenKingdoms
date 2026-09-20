#include "tak_ai_plan.h"
#include "tak_ai_goap.h"

#include <string.h>

/* The strategic domain: what a seat builds and whether its army holds
 * or goes. The search is tak_ai_goap.h's. This file is the world the
 * search reads, the actions it may take in it, the goals it is asked
 * for and how much each goal matters right now. */

/* Goals in the order ties are broken. */
static const AiGoal k_goal_order[] = {
    AI_GOAL_ECONOMY, AI_GOAL_DEFEND, AI_GOAL_ATTACK, AI_GOAL_ARMY, AI_GOAL_EXPAND
};

/* The planner's world, one integer each. */
enum {
    V_BUILD_EFF = 0,
    V_LODE_HAVE,            /* finished and pending */
    V_LODE_TARGET,
    V_LODE_PENDING,
    V_FACTORIES_PENDING,
    V_BUILDERS_IDLE,
    V_FACTORIES_IDLE,
    V_ARMY,
    V_ARMY_HOME,
    V_THREAT_HOME,
    V_FREE_SITES,
    V_SITE_NEAR,
    V_TARGET_KNOWN,
    V_UNIT_VALUE,           /* what one training adds */
    V_TOWER_VALUE,          /* what one tower adds */
    V_WAVED,                /* a wave went this plan */
    V_COUNT
};

#define ACT(a) ((a) - 1)    /* AiAction to its row below */

/* The rows follow AiAction, which is also the order ties go in. */
static const GoapAction k_actions[AI_ACT_COUNT - 1] = {
    [ACT(AI_ACT_BUILD_LODESTONE)] = {
        .name = "build lodestone",
        .pre = { AI_COND(V_BUILDERS_IDLE, AI_OP_GT, 0),
                 AI_COND_VAR(V_LODE_HAVE, AI_OP_LT, V_LODE_TARGET) },
        .eff = { AI_EFFECT(V_BUILDERS_IDLE, AI_EFF_SUB, 1),
                 AI_EFFECT(V_LODE_PENDING, AI_EFF_ADD, 1),
                 AI_EFFECT(V_LODE_HAVE, AI_EFF_ADD, 1),
                 /* On a pad when one is in reach, and then the pad is
                  * no longer free. */
                 AI_EFFECT_IF(V_SITE_NEAR, AI_EFF_SUB, 1, V_SITE_NEAR),
                 AI_EFFECT_IF(V_FREE_SITES, AI_EFF_SUB, 1, V_SITE_NEAR) },
        .tag = AI_ACTOR_BUILDER,
    },
    /* A structure pick waits until the pool covers 70 percent of what
     * the frames already standing ask for (legacy:17201, :17270). An
     * income building is exempt, as it is at the order's own gate
     * (legacy:12127). One frame at a time and never a permanent stop:
     * the producer count grows on a ratchet as the match runs
     * (legacy:16254-16266), up to the per-type limit in allowed[]. */
    [ACT(AI_ACT_BUILD_FACTORY)] = {
        .name = "build factory",
        .pre = { AI_COND(V_BUILDERS_IDLE, AI_OP_GT, 0),
                 AI_COND(V_BUILD_EFF, AI_OP_GE, 70),
                 AI_COND(V_FACTORIES_PENDING, AI_OP_EQ, 0) },
        .eff = { AI_EFFECT(V_BUILDERS_IDLE, AI_EFF_SUB, 1),
                 AI_EFFECT(V_FACTORIES_PENDING, AI_EFF_ADD, 1),
                 /* It will stand idle once built. */
                 AI_EFFECT(V_FACTORIES_IDLE, AI_EFF_ADD, 1) },
        .tag = AI_ACTOR_BUILDER,
    },
    [ACT(AI_ACT_BUILD_TOWER)] = {
        .name = "build tower",
        .pre = { AI_COND(V_BUILDERS_IDLE, AI_OP_GT, 0),
                 AI_COND(V_BUILD_EFF, AI_OP_GE, 70),
                 AI_COND(V_THREAT_HOME, AI_OP_GT, 0) },
        .eff = { AI_EFFECT(V_BUILDERS_IDLE, AI_EFF_SUB, 1),
                 AI_EFFECT(V_ARMY_HOME, AI_EFF_ADD_VAR, V_TOWER_VALUE) },
        .tag = AI_ACTOR_BUILDER,
    },
    /* Training waits for 7/30 of the same measure (legacy:17991). */
    [ACT(AI_ACT_TRAIN)] = {
        .name = "train",
        .pre = { AI_COND(V_FACTORIES_IDLE, AI_OP_GT, 0),
                 AI_COND(V_BUILD_EFF, AI_OP_GE, 23) },
        .eff = { AI_EFFECT(V_FACTORIES_IDLE, AI_EFF_SUB, 1),
                 AI_EFFECT(V_ARMY, AI_EFF_ADD_VAR, V_UNIT_VALUE),
                 AI_EFFECT(V_ARMY_HOME, AI_EFF_ADD_VAR, V_UNIT_VALUE) },
        .tag = AI_ACTOR_FACTORY,
    },
    /* Holding and sending a wave are postures taken now, so they only
     * open a plan. They never stand in for a later step. */
    [ACT(AI_ACT_HOLD)] = {
        .name = "hold",
        .pre = { AI_COND_VAR(V_ARMY, AI_OP_GT, V_ARMY_HOME) },
        .eff = { AI_EFFECT(V_ARMY_HOME, AI_EFF_SET_VAR, V_ARMY) },
        .opening_only = 1,
        .tag = AI_ACTOR_ARMY,
    },
    [ACT(AI_ACT_WAVE)] = {
        .name = "wave",
        .pre = { AI_COND(V_TARGET_KNOWN, AI_OP_NE, 0),
                 AI_COND(V_ARMY, AI_OP_GT, 0) },
        .eff = { AI_EFFECT(V_WAVED, AI_EFF_SET, 1) },
        .opening_only = 1,
        .tag = AI_ACTOR_ARMY,
    },
};

static const GoapDomain k_domain = {
    k_actions, AI_ACT_COUNT - 1, V_COUNT
};

/* What each goal wants of the world. Army and home strength are
 * incremental: any step that adds to them counts, since the tick
 * replans from the real state and a full army is many steps away. The
 * economy names a number, and a seat that cannot reach it this plan
 * still takes the plan that gets nearest, so one builder two
 * lodestones short builds one. */
static const GoapGoal k_goals[AI_GOAL_COUNT] = {
    [AI_GOAL_ECONOMY] = {
        .want = { AI_COND_VAR(V_LODE_HAVE, AI_OP_GE, V_LODE_TARGET) },
        .partial_ok = 1,
    },
    [AI_GOAL_DEFEND] = {
        .want = { AI_COND_START(V_ARMY_HOME, AI_OP_GT, V_ARMY_HOME) },
    },
    [AI_GOAL_ARMY] = {
        .want = { AI_COND_START(V_ARMY, AI_OP_GT, V_ARMY) },
    },
    [AI_GOAL_EXPAND] = {
        .want = { AI_COND_START(V_FREE_SITES, AI_OP_LT, V_FREE_SITES) },
    },
    [AI_GOAL_ATTACK] = {
        .want = { AI_COND(V_WAVED, AI_OP_GE, 1) },
    },
};

static void plan_world(const AiPlanState *s, const AiPlanCosts *c,
                       int32_t *w) {
    memset(w, 0, sizeof(int32_t) * V_COUNT);
    w[V_BUILD_EFF]         = s->build_eff;
    w[V_LODE_HAVE]         = s->lodestones + s->lodestones_pending;
    w[V_LODE_TARGET]       = s->lode_target;
    w[V_LODE_PENDING]      = s->lodestones_pending;
    w[V_FACTORIES_PENDING] = s->factories_pending;
    w[V_BUILDERS_IDLE]     = s->builders_idle;
    w[V_FACTORIES_IDLE]    = s->factories_idle;
    w[V_ARMY]              = s->army;
    w[V_ARMY_HOME]         = s->army_home;
    w[V_THREAT_HOME]       = s->threat_home;
    w[V_FREE_SITES]        = s->free_sites;
    w[V_SITE_NEAR]         = s->site_near;
    w[V_TARGET_KNOWN]      = s->target_known ? 1 : 0;
    w[V_UNIT_VALUE]        = c->unit_value;
    w[V_TOWER_VALUE]       = c->tower_value;
}

AiActorClass AI_Plan_ActorOf(AiAction action) {
    if (action <= AI_ACT_NONE || action >= AI_ACT_COUNT) return AI_ACTOR_ARMY;
    return (AiActorClass)k_actions[ACT(action)].tag;
}

const char *AI_Plan_ActionName(AiAction action) {
    if (action <= AI_ACT_NONE || action >= AI_ACT_COUNT) return "none";
    return k_actions[ACT(action)].name;
}

static int plan_economy_short(const AiPlanState *s) {
    return s->lodestones + s->lodestones_pending < s->lode_target;
}

/* Army rank: the seen threat, never below 30 (about four troops).
 * A rank against the other goals, not a size the army is finished at. */
static int32_t plan_army_want(const AiPlanState *s) {
    return s->threat_total > 30 ? s->threat_total : 30;
}

static int plan_home_short(const AiPlanState *s) {
    return s->army_home * 2 < s->threat_home * 3;
}

int AI_Plan_GoalPriority(const AiPlanState *s, AiGoal goal) {
    if (!s) return 0;
    switch (goal) {
    case AI_GOAL_ECONOMY:
        if (s->lode_target <= 0 || !plan_economy_short(s)) return 0;
        /* Mana below 30 percent or a stall is the original's cue for a
         * mana building, one at a time (legacy:19859 rules 4, 5, 11). */
        if ((s->mana_pct < 30 || s->stalling) && s->lodestones_pending == 0)
            return 100;
        return 40;
    case AI_GOAL_DEFEND:
        if (s->exposure <= 0 || !plan_home_short(s)) return 0;
        return 90;
    case AI_GOAL_ARMY:
        /* Never finished. The original draws from a weighted build
         * list every pass and stops only where a per-type limit bites
         * (legacy:21281-21294, :21339-21343), which allowed[] carries,
         * so this is a rank against the other goals and not an end. */
        return s->army < plan_army_want(s) ? 60 : 35;
    case AI_GOAL_EXPAND:
        if (s->site_near <= 0 || s->exposure > 0) return 0;
        return 50;
    case AI_GOAL_ATTACK:
        if (!s->target_known || s->army <= 0) return 0;
        return s->enemy_weak > 0 ? 70 : 45;
    default:
        return 0;
    }
}

/* first_actor restricts the opening step to one actor class, or any
 * when negative. */
static int plan_solve_for(const AiPlanState *s, const AiPlanCosts *c,
                          AiGoal goal, int first_actor, AiPlan *out) {
    memset(out, 0, sizeof(*out));
    out->goal = goal;
    if (goal <= AI_GOAL_NONE || goal >= AI_GOAL_COUNT) return 0;
    out->priority = AI_Plan_GoalPriority(s, goal);
    if (out->priority <= 0) return 0;

    int32_t world[V_COUNT];
    plan_world(s, c, world);
    GoapQuery q;
    q.start     = world;
    q.cost      = &c->cost[1];      /* AiAction rows start at 1 */
    q.allowed   = &c->allowed[1];
    q.first_tag = first_actor;
    q.max_depth = AI_PLAN_MAX_STEPS;
    GoapPlan plan;
    if (!Goap_Solve(&k_domain, &k_goals[goal], &q, &plan)) return 0;
    out->step_count = plan.step_count;
    out->cost = plan.cost;
    out->complete = plan.complete;
    out->nodes = plan.nodes;
    for (int i = 0; i < plan.step_count; i++)
        out->steps[i] = (AiAction)(plan.steps[i] + 1);
    return 1;
}

int AI_Plan_Solve(const AiPlanState *s, const AiPlanCosts *c,
                  AiGoal goal, AiPlan *out) {
    if (!s || !c || !out) return 0;
    return plan_solve_for(s, c, goal, -1, out);
}

AiAction AI_Plan_NextAction(const AiPlanState *s, const AiPlanCosts *c,
                            AiActorClass actor, AiGoal *out_goal) {
    if (out_goal) *out_goal = AI_GOAL_NONE;
    if (!s || !c) return AI_ACT_NONE;
    AiAction best_action = AI_ACT_NONE;
    int best_priority = 0;
    for (size_t i = 0; i < sizeof(k_goal_order) / sizeof(k_goal_order[0]); i++) {
        /* A goal that cannot outrank the one in hand is not searched. */
        if (AI_Plan_GoalPriority(s, k_goal_order[i]) <= best_priority) continue;
        AiPlan plan;
        if (!plan_solve_for(s, c, k_goal_order[i], (int)actor, &plan)) continue;
        best_priority = plan.priority;
        best_action = plan.steps[0];
        if (out_goal) *out_goal = plan.goal;
    }
    return best_action;
}
