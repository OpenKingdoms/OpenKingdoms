#ifndef TAK_AI_TASKNET_H
#define TAK_AI_TASKNET_H

#include "tak_ai_facts.h"

/* Hierarchical task network planning. A domain is a table of tasks. A
 * primitive task has the conditions it needs and the effects it has,
 * like a goal planner's action. A compound task has methods, each a
 * condition and an ordered list of subtasks, tried in the order they
 * are written. Planning takes the root task apart depth first, left
 * to right, against a world that every primitive updates as it is
 * placed, and when a later subtask cannot be placed it goes back and
 * tries the next method of an earlier one. What comes out is the
 * primitives in the order to do them and the methods that were taken,
 * which is the plan's reason. It knows nothing about the game.
 * Integers only, no allocation, the same answer on every machine. */

#define HTN_MAX_TASKS     24
#define HTN_MAX_METHODS   6
#define HTN_MAX_SUBTASKS  4
#define HTN_MAX_PLAN      8
#define HTN_MAX_PENDING   16
#define HTN_MAX_EXPANSIONS 256

typedef struct HtnMethod {
    const char *name;
    AiCond      pre[AI_FACT_MAX_CONDS];
    uint8_t     subtasks[HTN_MAX_SUBTASKS];
    uint8_t     subtask_count;
} HtnMethod;

typedef struct HtnTask {
    const char *name;
    uint8_t     compound;
    /* A primitive's. */
    AiCond      pre[AI_FACT_MAX_CONDS];
    AiEffect    eff[AI_FACT_MAX_EFFECTS];
    /* A compound's. */
    HtnMethod   methods[HTN_MAX_METHODS];
    uint8_t     method_count;
} HtnTask;

typedef struct HtnDomain {
    const HtnTask *tasks;
    int            task_count;
    int            var_count;
} HtnDomain;

typedef struct HtnPlan {
    uint8_t steps[HTN_MAX_PLAN];            /* primitive task indices */
    int     step_count;
    /* The decomposition: each compound taken apart and the method it
     * was taken apart by, in the order it happened. */
    uint8_t via_task[HTN_MAX_PENDING];
    uint8_t via_method[HTN_MAX_PENDING];
    int     via_count;
    int     expansions;
} HtnPlan;

/* 1 and a plan, or 0 when the root cannot be taken apart in this
 * world. A plan may be empty: a method with no subtasks is a task
 * already done. */
int Htn_Plan(const HtnDomain *d, int root_task, const int32_t *start,
             HtnPlan *out);

#endif /* TAK_AI_TASKNET_H */
