#ifndef TAK_AI_PLAN_H
#define TAK_AI_PLAN_H

#include <stdint.h>

/* Goal-oriented planning for the AI's build and attack choices. Pure:
 * the caller reads the state off the economy, the influence maps and
 * the unit list, prices the actions from the profile, and executes the
 * first step of the plan through the existing build and order paths.
 * Integers only, fixed action order, no randomness. */

typedef enum {
    AI_GOAL_NONE = 0,
    AI_GOAL_ECONOMY,    /* hold a lodestone count */
    AI_GOAL_DEFEND,     /* strength at home against the threat there */
    AI_GOAL_ARMY,       /* an army sized to the threat */
    AI_GOAL_EXPAND,     /* secure a nearby sacred site */
    AI_GOAL_ATTACK,     /* send a wave at the weakest enemy */
    AI_GOAL_COUNT
} AiGoal;

typedef enum {
    AI_ACT_NONE = 0,
    AI_ACT_BUILD_LODESTONE,
    AI_ACT_BUILD_FACTORY,
    AI_ACT_BUILD_TOWER,
    AI_ACT_TRAIN,
    AI_ACT_HOLD,
    AI_ACT_WAVE,
    AI_ACT_COUNT
} AiAction;

typedef enum {
    AI_ACTOR_BUILDER = 0,   /* idle mobile builder */
    AI_ACTOR_FACTORY,       /* idle production structure */
    AI_ACTOR_ARMY,          /* the idle combat units */
    AI_ACTOR_COUNT
} AiActorClass;

typedef struct AiPlanState {
    int32_t mana_pct;          /* mana over its cap, 0..100 */
    int     stalling;          /* legacy:19859 stall test */
    int     lodestones;        /* finished */
    int     lodestones_pending;
    int     lode_target;       /* wanted, already capped by the profile */
    int     factories;
    int     factories_pending;
    int     builders_idle;
    int     factories_idle;
    int32_t army;              /* own mobile combat value */
    int32_t army_home;         /* the part of it near home */
    int32_t threat_home;       /* seen enemy combat value at home */
    int32_t threat_total;      /* seen enemy combat value everywhere */
    int32_t exposure;          /* largest exposure on the map */
    int     free_sites;        /* unclaimed sacred sites known */
    int     site_near;         /* one of them within reach */
    int     target_known;      /* a wave target exists */
    int32_t enemy_weak;        /* largest weakness among seen enemy cells */
} AiPlanState;

typedef struct AiPlanCosts {
    int32_t cost[AI_ACT_COUNT];      /* mana cost scaled by profile weight */
    uint8_t allowed[AI_ACT_COUNT];   /* weight above zero and limit not hit */
    int32_t unit_value;              /* combat value one training adds */
    int32_t tower_value;             /* combat value one tower adds */
} AiPlanCosts;

#define AI_PLAN_MAX_STEPS 3

typedef struct AiPlan {
    AiGoal   goal;
    int      priority;
    AiAction steps[AI_PLAN_MAX_STEPS];
    int      step_count;
    int32_t  cost;
} AiPlan;

/* 0 when the goal is met or does not apply. */
int      AI_Plan_GoalPriority(const AiPlanState *s, AiGoal goal);
/* Cheapest sequence of at most three actions reaching the goal. */
int      AI_Plan_Solve(const AiPlanState *s, const AiPlanCosts *c,
                       AiGoal goal, AiPlan *out);
/* The opening step for this actor class of the highest priority goal
 * that has a plan opening with it. AI_ACT_NONE when nothing is wanted. */
AiAction AI_Plan_NextAction(const AiPlanState *s, const AiPlanCosts *c,
                            AiActorClass actor, AiGoal *out_goal);
AiActorClass AI_Plan_ActorOf(AiAction action);

#endif /* TAK_AI_PLAN_H */
