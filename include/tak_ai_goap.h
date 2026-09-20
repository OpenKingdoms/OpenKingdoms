#ifndef TAK_AI_GOAP_H
#define TAK_AI_GOAP_H

#include "tak_ai_facts.h"

/* Goal oriented action planning. A domain is a table of actions, each
 * with the conditions it needs and the effects it has, and a goal is a
 * set of conditions wanted of the world. The solver is a best first
 * search forward from the starting world on cost so far, so the plan
 * it gives is the cheapest one inside its bounds, and it is the same
 * plan on every machine: ties go to the shorter plan and then to the
 * one whose actions come first in the table. It knows nothing about
 * the game. Integers only, no allocation. */

#define GOAP_MAX_ACTIONS 24
#define GOAP_MAX_DEPTH   6
#define GOAP_MAX_NODES   384

typedef struct GoapAction {
    const char *name;
    AiCond      pre[AI_FACT_MAX_CONDS];
    AiEffect    eff[AI_FACT_MAX_EFFECTS];
    /* A posture taken now rather than a step towards something, so it
     * may only open a plan. */
    uint8_t     opening_only;
    /* The domain's own grouping, read back through the query. */
    uint8_t     tag;
} GoapAction;

typedef struct GoapDomain {
    const GoapAction *actions;
    int               action_count;
    int               var_count;
} GoapDomain;

typedef struct GoapGoal {
    AiCond  want[AI_FACT_MAX_CONDS];
    /* When nothing inside the bounds reaches the goal, take the plan
     * that ends nearest to it, provided it ends nearer than it began. */
    uint8_t partial_ok;
} GoapGoal;

typedef struct GoapQuery {
    const int32_t *start;               /* var_count values */
    const int32_t *cost;                /* per action, never negative */
    const uint8_t *allowed;             /* per action */
    int            first_tag;           /* the opening action's tag, or -1 */
    int            max_depth;           /* 1..GOAP_MAX_DEPTH */
} GoapQuery;

typedef struct GoapPlan {
    uint8_t steps[GOAP_MAX_DEPTH];      /* indices into the domain */
    int     step_count;
    int32_t cost;
    int     complete;                   /* 0 for a partial plan */
    int32_t remaining;                  /* the goal's gap where it ends */
    int     nodes;                      /* worlds the search opened */
} GoapPlan;

/* 1 and a plan of at least one step, or 0. */
int Goap_Solve(const GoapDomain *d, const GoapGoal *g, const GoapQuery *q,
               GoapPlan *out);

#endif /* TAK_AI_GOAP_H */
