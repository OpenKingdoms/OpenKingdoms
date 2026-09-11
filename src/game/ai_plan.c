#include "tak_ai_plan.h"

#include <string.h>

/* Goals in the order ties are broken. */
static const AiGoal k_goal_order[] = {
    AI_GOAL_ECONOMY, AI_GOAL_DEFEND, AI_GOAL_ATTACK, AI_GOAL_ARMY, AI_GOAL_EXPAND
};

AiActorClass AI_Plan_ActorOf(AiAction action) {
    switch (action) {
    case AI_ACT_BUILD_LODESTONE:
    case AI_ACT_BUILD_FACTORY:
    case AI_ACT_BUILD_TOWER:
        return AI_ACTOR_BUILDER;
    case AI_ACT_TRAIN:
        return AI_ACTOR_FACTORY;
    default:
        return AI_ACTOR_ARMY;
    }
}

static int plan_economy_short(const AiPlanState *s) {
    return s->lodestones + s->lodestones_pending < s->lode_target;
}

/* Army floor: the seen threat, never below 30 (about four troops). */
static int32_t plan_army_want(const AiPlanState *s) {
    return s->threat_total > 30 ? s->threat_total : 30;
}

static int plan_home_short(const AiPlanState *s) {
    return s->army_home * 2 < s->threat_home * 3;
}

/* Goals the plan is asked to reach. Army and home strength are
 * incremental: any step that adds to them counts, since the tick
 * replans from the real state and a full army is many steps away. */
static int plan_goal_met(const AiPlanState *s, const AiPlanState *start,
                         AiGoal goal, AiAction last) {
    switch (goal) {
    case AI_GOAL_ECONOMY:
        return !plan_economy_short(s);
    case AI_GOAL_DEFEND:
        return s->army_home > start->army_home;
    case AI_GOAL_ARMY:
        return s->army > start->army;
    case AI_GOAL_EXPAND:
        return s->free_sites < start->free_sites;
    case AI_GOAL_ATTACK:
        return last == AI_ACT_WAVE;
    default:
        return 1;
    }
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
        return s->army < plan_army_want(s) ? 60 : 0;
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

/* Holding and sending a wave are postures taken now, so they only
 * open a plan; they never stand in for a later step. */
static int plan_action_ok(const AiPlanState *s, const AiPlanCosts *c,
                          AiAction a, int depth) {
    if (!c->allowed[a]) return 0;
    switch (a) {
    case AI_ACT_BUILD_LODESTONE:
        return s->builders_idle > 0 &&
               s->lodestones + s->lodestones_pending < s->lode_target;
    case AI_ACT_BUILD_FACTORY:
        return s->builders_idle > 0 &&
               s->factories + s->factories_pending == 0;
    case AI_ACT_BUILD_TOWER:
        return s->builders_idle > 0 && s->threat_home > 0;
    case AI_ACT_TRAIN:
        /* A starved AI feeds its lodestone first, and keeps feeding it
         * until it stands (legacy:19859 brake). */
        if ((s->mana_pct < 30 || s->stalling) &&
            (plan_economy_short(s) || s->lodestones_pending > 0))
            return 0;
        return s->factories_idle > 0;
    case AI_ACT_HOLD:
        return depth == 0 && s->army > s->army_home;
    case AI_ACT_WAVE:
        return depth == 0 && s->target_known && s->army > 0;
    default:
        return 0;
    }
}

static void plan_apply(AiPlanState *s, const AiPlanCosts *c, AiAction a) {
    switch (a) {
    case AI_ACT_BUILD_LODESTONE:
        s->builders_idle--;
        s->lodestones_pending++;
        if (s->site_near > 0) { s->site_near--; s->free_sites--; }
        break;
    case AI_ACT_BUILD_FACTORY:
        s->builders_idle--;
        s->factories_pending++;
        s->factories_idle++;   /* it will stand idle once built */
        break;
    case AI_ACT_BUILD_TOWER:
        s->builders_idle--;
        s->army_home += c->tower_value;
        break;
    case AI_ACT_TRAIN:
        s->factories_idle--;
        s->army += c->unit_value;
        s->army_home += c->unit_value;
        break;
    case AI_ACT_HOLD:
        s->army_home = s->army;
        break;
    default:
        break;
    }
}

/* first_actor restricts the opening step to one actor class, or any
 * when negative. */
static void plan_search(const AiPlanState *s, const AiPlanState *start,
                        const AiPlanCosts *c, AiGoal goal, int first_actor,
                        AiAction *path, int depth, int32_t cost,
                        AiPlan *best) {
    AiAction last = depth > 0 ? path[depth - 1] : AI_ACT_NONE;
    if (depth > 0 && plan_goal_met(s, start, goal, last)) {
        if (best->step_count == 0 || cost < best->cost ||
            (cost == best->cost && depth < best->step_count)) {
            best->cost = cost;
            best->step_count = depth;
            memcpy(best->steps, path, sizeof(AiAction) * (size_t)depth);
        }
        return;
    }
    if (depth >= AI_PLAN_MAX_STEPS) return;
    if (best->step_count > 0 && cost > best->cost) return;
    for (int a = AI_ACT_BUILD_LODESTONE; a < AI_ACT_COUNT; a++) {
        if (depth == 0 && first_actor >= 0 &&
            (int)AI_Plan_ActorOf((AiAction)a) != first_actor) continue;
        if (!plan_action_ok(s, c, (AiAction)a, depth)) continue;
        AiPlanState next = *s;
        plan_apply(&next, c, (AiAction)a);
        path[depth] = (AiAction)a;
        plan_search(&next, start, c, goal, first_actor, path, depth + 1,
                    cost + c->cost[a], best);
    }
}

static int plan_solve_for(const AiPlanState *s, const AiPlanCosts *c,
                          AiGoal goal, int first_actor, AiPlan *out) {
    memset(out, 0, sizeof(*out));
    out->goal = goal;
    out->priority = AI_Plan_GoalPriority(s, goal);
    if (out->priority <= 0) return 0;
    AiAction path[AI_PLAN_MAX_STEPS];
    plan_search(s, s, c, goal, first_actor, path, 0, 0, out);
    return out->step_count > 0;
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
        AiPlan plan;
        if (!plan_solve_for(s, c, k_goal_order[i], (int)actor, &plan)) continue;
        if (plan.priority <= best_priority) continue;
        best_priority = plan.priority;
        best_action = plan.steps[0];
        if (out_goal) *out_goal = plan.goal;
    }
    return best_action;
}
