#include "tak_ai_facts.h"

static void fact_sides(const AiCond *c, const int32_t *world,
                       const int32_t *start, int64_t *lhs, int64_t *rhs) {
    int64_t r;
    if (c->rhs_kind == AI_RHS_VAR)        r = world[c->rhs];
    else if (c->rhs_kind == AI_RHS_START) r = start ? start[c->rhs] : 0;
    else                                  r = c->rhs;
    *lhs = (int64_t)world[c->var] * (c->lmul ? c->lmul : 1);
    *rhs = r * (c->rmul ? c->rmul : 1);
}

int AI_Fact_Holds(const AiCond *c, const int32_t *world, const int32_t *start) {
    if (!c || !c->used) return 1;
    int64_t l, r;
    fact_sides(c, world, start, &l, &r);
    switch (c->op) {
    case AI_OP_EQ: return l == r;
    case AI_OP_NE: return l != r;
    case AI_OP_LT: return l <  r;
    case AI_OP_LE: return l <= r;
    case AI_OP_GT: return l >  r;
    case AI_OP_GE: return l >= r;
    default:       return 0;
    }
}

int AI_Fact_AllHold(const AiCond *conds, int count, const int32_t *world,
                    const int32_t *start) {
    for (int i = 0; i < count; i++)
        if (!AI_Fact_Holds(&conds[i], world, start)) return 0;
    return 1;
}

/* The distance the left side has to travel. Equality and inequality
 * have no direction to measure, so they are one step away. */
int32_t AI_Fact_Gap(const AiCond *c, const int32_t *world, const int32_t *start) {
    if (AI_Fact_Holds(c, world, start)) return 0;
    int64_t l, r;
    fact_sides(c, world, start, &l, &r);
    int64_t gap;
    switch (c->op) {
    case AI_OP_LT: gap = l - r + 1; break;
    case AI_OP_LE: gap = l - r;     break;
    case AI_OP_GT: gap = r - l + 1; break;
    case AI_OP_GE: gap = r - l;     break;
    default:       gap = 1;         break;
    }
    if (gap < 1) gap = 1;
    if (gap > 0x3fffffff) gap = 0x3fffffff;
    return (int32_t)gap;
}

void AI_Fact_Apply(const AiEffect *effects, int count, int32_t *world) {
    /* Guards read the world the step began in, so one effect cannot
     * switch the next one of the same step off. */
    uint8_t live[AI_FACT_MAX_EFFECTS];
    if (count > AI_FACT_MAX_EFFECTS) count = AI_FACT_MAX_EFFECTS;
    for (int i = 0; i < count; i++) {
        const AiEffect *e = &effects[i];
        live[i] = (uint8_t)(e->used && (e->guard == AI_FACT_NO_GUARD ||
                                         world[e->guard] > 0));
    }
    for (int i = 0; i < count; i++) {
        if (!live[i]) continue;
        const AiEffect *e = &effects[i];
        switch (e->op) {
        case AI_EFF_SET:     world[e->var]  = e->arg;        break;
        case AI_EFF_ADD:     world[e->var] += e->arg;        break;
        case AI_EFF_SUB:     world[e->var] -= e->arg;        break;
        case AI_EFF_SET_VAR: world[e->var]  = world[e->arg]; break;
        case AI_EFF_ADD_VAR: world[e->var] += world[e->arg]; break;
        case AI_EFF_SUB_VAR: world[e->var] -= world[e->arg]; break;
        default: break;
        }
    }
}
