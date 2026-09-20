#ifndef TAK_AI_FACTS_H
#define TAK_AI_FACTS_H

#include <stdint.h>

/* The planners' shared vocabulary. A world is a row of integers, a
 * condition compares one of them with a constant, with another, or
 * with what it was when planning began, and an effect writes one. The
 * goal planner (tak_ai_goap.h) and the task planner (tak_ai_htn.h)
 * both describe their domains in these, so a domain is data and the
 * search knows nothing about the game. Integers only, no allocation. */

#define AI_FACT_MAX_VARS    40
#define AI_FACT_MAX_CONDS   6
#define AI_FACT_MAX_EFFECTS 6
#define AI_FACT_NO_GUARD    0xFF

typedef enum {
    AI_OP_EQ = 0, AI_OP_NE, AI_OP_LT, AI_OP_LE, AI_OP_GT, AI_OP_GE
} AiFactOp;

typedef enum {
    AI_RHS_CONST = 0,   /* rhs is a number */
    AI_RHS_VAR,         /* rhs names a variable of the same world */
    AI_RHS_START        /* rhs names a variable of the starting world */
} AiFactRhs;

/* var * lmul OP rhs * rmul. A multiplier of 0 reads as 1, so a plain
 * comparison needs neither. used marks the slots of a fixed array. */
typedef struct AiCond {
    uint8_t used;
    uint8_t var;
    uint8_t op;
    uint8_t rhs_kind;
    int32_t rhs;
    int32_t lmul, rmul;
} AiCond;

typedef enum {
    AI_EFF_SET = 0, AI_EFF_ADD, AI_EFF_SUB,
    AI_EFF_SET_VAR, AI_EFF_ADD_VAR, AI_EFF_SUB_VAR
} AiEffectOp;

/* guard names a variable that must be above zero, read before any
 * effect of the same step is applied, or AI_FACT_NO_GUARD. */
typedef struct AiEffect {
    uint8_t used;
    uint8_t var;
    uint8_t op;
    uint8_t guard;
    int32_t arg;
} AiEffect;

/* Builders for a domain table. */
#define AI_COND(v, o, n)        { 1, (v), (o), AI_RHS_CONST, (n), 0, 0 }
#define AI_COND_VAR(v, o, w)    { 1, (v), (o), AI_RHS_VAR, (w), 0, 0 }
#define AI_COND_START(v, o, w)  { 1, (v), (o), AI_RHS_START, (w), 0, 0 }
#define AI_COND_SCALED(v, lm, o, w, rm) \
                                { 1, (v), (o), AI_RHS_VAR, (w), (lm), (rm) }
#define AI_EFFECT(v, o, n)      { 1, (v), (o), AI_FACT_NO_GUARD, (n) }
#define AI_EFFECT_IF(v, o, n, g) { 1, (v), (o), (g), (n) }

/* 1 when the condition holds in world, start being the world planning
 * began in. An unused slot holds. */
int     AI_Fact_Holds(const AiCond *c, const int32_t *world,
                      const int32_t *start);
int     AI_Fact_AllHold(const AiCond *conds, int count,
                        const int32_t *world, const int32_t *start);
/* How far the condition is from holding, 0 when it holds. */
int32_t AI_Fact_Gap(const AiCond *c, const int32_t *world,
                    const int32_t *start);
void    AI_Fact_Apply(const AiEffect *effects, int count, int32_t *world);

#endif /* TAK_AI_FACTS_H */
