#include "tak_ai_tasknet.h"

#include <string.h>

typedef struct HtnSearch {
    const HtnDomain *d;
    const int32_t   *start;
    HtnPlan          plan;
} HtnSearch;

/* Place the first pending task and then the rest. A primitive goes
 * into the plan and changes the world the rest is planned in. A
 * compound is replaced by the subtasks of the first method that lets
 * the whole of the rest be placed, which is where the search backs
 * up: a method that fits here and strands a later task is dropped for
 * the next one. */
static int htn_solve(HtnSearch *s, const uint8_t *pending, int count,
                     const int32_t *world) {
    if (count == 0) return 1;
    if (++s->plan.expansions > HTN_MAX_EXPANSIONS) return 0;
    const int id = pending[0];
    if (id < 0 || id >= s->d->task_count) return 0;
    const HtnTask *t = &s->d->tasks[id];

    if (!t->compound) {
        if (!AI_Fact_AllHold(t->pre, AI_FACT_MAX_CONDS, world, s->start))
            return 0;
        if (s->plan.step_count >= HTN_MAX_PLAN) return 0;
        int32_t next[AI_FACT_MAX_VARS];
        memcpy(next, world, sizeof(int32_t) * (size_t)s->d->var_count);
        AI_Fact_Apply(t->eff, AI_FACT_MAX_EFFECTS, next);
        s->plan.steps[s->plan.step_count++] = (uint8_t)id;
        if (htn_solve(s, pending + 1, count - 1, next)) return 1;
        s->plan.step_count--;
        return 0;
    }

    for (int m = 0; m < t->method_count && m < HTN_MAX_METHODS; m++) {
        const HtnMethod *me = &t->methods[m];
        if (!AI_Fact_AllHold(me->pre, AI_FACT_MAX_CONDS, world, s->start))
            continue;
        const int subs = me->subtask_count;
        if (subs > HTN_MAX_SUBTASKS || subs + count - 1 > HTN_MAX_PENDING)
            continue;
        if (s->plan.via_count >= HTN_MAX_PENDING) continue;
        uint8_t next[HTN_MAX_PENDING];
        memcpy(next, me->subtasks, (size_t)subs);
        memcpy(next + subs, pending + 1, (size_t)(count - 1));
        s->plan.via_task[s->plan.via_count] = (uint8_t)id;
        s->plan.via_method[s->plan.via_count] = (uint8_t)m;
        s->plan.via_count++;
        if (htn_solve(s, next, subs + count - 1, world)) return 1;
        s->plan.via_count--;
    }
    return 0;
}

int Htn_Plan(const HtnDomain *d, int root_task, const int32_t *start,
             HtnPlan *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!d || !start || !out || !d->tasks) return 0;
    if (d->task_count <= 0 || d->task_count > HTN_MAX_TASKS) return 0;
    if (d->var_count <= 0 || d->var_count > AI_FACT_MAX_VARS) return 0;
    if (root_task < 0 || root_task >= d->task_count) return 0;
    HtnSearch s;
    memset(&s, 0, sizeof(s));
    s.d = d;
    s.start = start;
    uint8_t pending[1] = { (uint8_t)root_task };
    int ok = htn_solve(&s, pending, 1, start);
    *out = s.plan;
    if (!ok) { out->step_count = 0; out->via_count = 0; }
    return ok;
}
