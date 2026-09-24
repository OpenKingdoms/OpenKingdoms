/*
 * test_ai_planners.c -- the goal planner and the task planner on their
 * own, in small domains that have nothing to do with the game, so what
 * is tested is the search and not a strategy.
 */

#include "tak_ai_facts.h"
#include "tak_ai_goap.h"
#include "tak_ai_tasknet.h"
#include "tak_ai_squad.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_EQ(want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); \
    if (w_ != g_) { \
        fprintf(stderr, "FAIL %s:%d  %s: expected %lld got %lld\n", \
                __FILE__, __LINE__, #got, w_, g_); \
        g_failures++; \
    } \
} while (0)

/* ── Facts ───────────────────────────────────────────────────────── */

static void test_facts_compare_scale_and_measure(void) {
    int32_t start[3] = { 4, 10, 0 };
    int32_t world[3] = { 6, 10, 0 };
    AiCond gt_const   = AI_COND(0, AI_OP_GT, 5);
    AiCond lt_var     = AI_COND_VAR(0, AI_OP_LT, 1);
    AiCond gt_start   = AI_COND_START(0, AI_OP_GT, 0);
    AiCond scaled     = AI_COND_SCALED(0, 2, AI_OP_GT, 1, 1);  /* 12 > 10 */
    AiCond unused;
    memset(&unused, 0, sizeof(unused));
    EXPECT(AI_Fact_Holds(&gt_const, world, start));
    EXPECT(AI_Fact_Holds(&lt_var, world, start));
    EXPECT(AI_Fact_Holds(&gt_start, world, start));
    EXPECT(AI_Fact_Holds(&scaled, world, start));
    EXPECT(AI_Fact_Holds(&unused, world, start));
    /* A gap is how far the left side has to go. */
    AiCond ge9 = AI_COND(0, AI_OP_GE, 9);
    AiCond gt9 = AI_COND(0, AI_OP_GT, 9);
    EXPECT_EQ(3, AI_Fact_Gap(&ge9, world, start));
    EXPECT_EQ(4, AI_Fact_Gap(&gt9, world, start));
    EXPECT_EQ(0, AI_Fact_Gap(&gt_const, world, start));

    /* A guard reads the world the step began in. */
    AiEffect eff[AI_FACT_MAX_EFFECTS] = {
        AI_EFFECT_IF(2, AI_EFF_SUB, 1, 2),      /* off: world[2] is 0 */
        AI_EFFECT_IF(0, AI_EFF_SUB, 6, 0),      /* on, and zeroes its guard */
        AI_EFFECT_IF(1, AI_EFF_ADD, 5, 0),      /* still on */
        AI_EFFECT(2, AI_EFF_SET_VAR, 1),
    };
    AI_Fact_Apply(eff, AI_FACT_MAX_EFFECTS, world);
    EXPECT_EQ(0, world[0]);
    EXPECT_EQ(15, world[1]);
    EXPECT_EQ(15, world[2]);
}

/* ── The goal planner ────────────────────────────────────────────── */

enum { G_GOLD = 0, G_AXE, G_WOOD, G_FIRE, G_MEAL, G_VARS };
enum { A_BUY_AXE = 0, A_CHOP, A_GATHER, A_LIGHT, A_COOK, A_COUNT };

static const GoapAction k_camp[A_COUNT] = {
    [A_BUY_AXE] = { .name = "buy axe",
        .pre = { AI_COND(G_GOLD, AI_OP_GE, 5), AI_COND(G_AXE, AI_OP_EQ, 0) },
        .eff = { AI_EFFECT(G_GOLD, AI_EFF_SUB, 5), AI_EFFECT(G_AXE, AI_EFF_SET, 1) } },
    [A_CHOP] = { .name = "chop",
        .pre = { AI_COND(G_AXE, AI_OP_GE, 1) },
        .eff = { AI_EFFECT(G_WOOD, AI_EFF_ADD, 1) } },
    [A_GATHER] = { .name = "gather",
        .eff = { AI_EFFECT(G_WOOD, AI_EFF_ADD, 1) }, .tag = 1 },
    [A_LIGHT] = { .name = "light",
        .pre = { AI_COND(G_WOOD, AI_OP_GE, 2), AI_COND(G_FIRE, AI_OP_EQ, 0) },
        .eff = { AI_EFFECT(G_WOOD, AI_EFF_SUB, 2), AI_EFFECT(G_FIRE, AI_EFF_SET, 1) } },
    [A_COOK] = { .name = "cook",
        .pre = { AI_COND(G_FIRE, AI_OP_GE, 1) },
        .eff = { AI_EFFECT(G_MEAL, AI_EFF_SET, 1) } },
};
static const GoapDomain k_camp_domain = { k_camp, A_COUNT, G_VARS };
static const int32_t k_camp_cost[A_COUNT] = { 5, 2, 6, 1, 1 };
static const uint8_t k_camp_all[A_COUNT]  = { 1, 1, 1, 1, 1 };

static GoapQuery camp_query(const int32_t *start, int depth) {
    GoapQuery q;
    q.start = start;
    q.cost = k_camp_cost;
    q.allowed = k_camp_all;
    q.first_tag = -1;
    q.max_depth = depth;
    return q;
}

/* The cheapest plan, not the shortest: five steps through the axe cost
 * 11 where four steps of gathering cost 14. */
static void test_goap_takes_the_cheapest_plan_however_long(void) {
    const GoapGoal meal = { .want = { AI_COND(G_MEAL, AI_OP_GE, 1) } };
    int32_t start[G_VARS] = { 5, 0, 0, 0, 0 };
    GoapQuery q = camp_query(start, GOAP_MAX_DEPTH);
    GoapPlan plan;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &meal, &q, &plan));
    EXPECT_EQ(1, plan.complete);
    EXPECT_EQ(5, plan.step_count);
    EXPECT_EQ(11, plan.cost);
    EXPECT_EQ(A_BUY_AXE, plan.steps[0]);
    EXPECT_EQ(A_CHOP, plan.steps[1]);
    EXPECT_EQ(A_CHOP, plan.steps[2]);
    EXPECT_EQ(A_LIGHT, plan.steps[3]);
    EXPECT_EQ(A_COOK, plan.steps[4]);
    printf("(meal with gold: %d steps, cost %d, %d worlds) ",
           plan.step_count, plan.cost, plan.nodes);

    /* No gold, no axe: the dear way is the only way. */
    start[G_GOLD] = 0;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &meal, &q, &plan));
    EXPECT_EQ(4, plan.step_count);
    EXPECT_EQ(14, plan.cost);
    EXPECT_EQ(A_GATHER, plan.steps[0]);

    /* The same question gets the same answer. */
    GoapPlan again;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &meal, &q, &again));
    EXPECT(memcmp(&plan, &again, sizeof(plan)) == 0);

    /* Inside three steps there is no meal, and a meal has no half. */
    q.max_depth = 3;
    EXPECT_EQ(0, Goap_Solve(&k_camp_domain, &meal, &q, &plan));
}

static void test_goap_masks_tags_and_opening_moves(void) {
    const GoapGoal wood = { .want = { AI_COND(G_WOOD, AI_OP_GE, 1) } };
    int32_t start[G_VARS] = { 0, 1, 0, 0, 0 };
    GoapQuery q = camp_query(start, 3);
    GoapPlan plan;
    /* Chopping is cheaper than gathering. */
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &wood, &q, &plan));
    EXPECT_EQ(A_CHOP, plan.steps[0]);
    /* Forbidden, it is not taken. */
    uint8_t no_chop[A_COUNT] = { 1, 0, 1, 1, 1 };
    q.allowed = no_chop;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &wood, &q, &plan));
    EXPECT_EQ(A_GATHER, plan.steps[0]);
    /* A plan that has to open with tag 1 opens with the gatherer. */
    q.allowed = k_camp_all;
    q.first_tag = 1;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &wood, &q, &plan));
    EXPECT_EQ(A_GATHER, plan.steps[0]);

    /* An opening move is not a later step. */
    static const GoapAction two[2] = {
        { .name = "step", .eff = { AI_EFFECT(0, AI_EFF_ADD, 1) } },
        { .name = "leap", .pre = { AI_COND(0, AI_OP_GE, 1) },
          .eff = { AI_EFFECT(0, AI_EFF_ADD, 10) }, .opening_only = 1 },
    };
    const GoapDomain d = { two, 2, 1 };
    const GoapGoal far_goal = { .want = { AI_COND(0, AI_OP_GE, 11) } };
    int32_t zero[1] = { 0 };
    int32_t cost[2] = { 1, 1 };
    uint8_t all[2] = { 1, 1 };
    GoapQuery q2 = { zero, cost, all, -1, 4 };
    EXPECT_EQ(0, Goap_Solve(&d, &far_goal, &q2, &plan));
    int32_t one[1] = { 1 };
    q2.start = one;
    EXPECT_EQ(1, Goap_Solve(&d, &far_goal, &q2, &plan));
    EXPECT_EQ(1, plan.step_count);
}

/* Equal plans: the shorter, then the one earlier in the table. And the
 * same world reached two ways is opened once. */
static void test_goap_breaks_ties_and_prunes_repeats(void) {
    static const GoapAction abc[3] = {
        { .name = "a", .pre = { AI_COND(0, AI_OP_EQ, 0) },
          .eff = { AI_EFFECT(0, AI_EFF_SET, 1) } },
        { .name = "b", .pre = { AI_COND(1, AI_OP_EQ, 0) },
          .eff = { AI_EFFECT(1, AI_EFF_SET, 1) } },
        { .name = "c", .pre = { AI_COND(2, AI_OP_EQ, 0) },
          .eff = { AI_EFFECT(2, AI_EFF_SET, 1) } },
    };
    const GoapDomain d = { abc, 3, 3 };
    const GoapGoal all3 = { .want = { AI_COND(0, AI_OP_EQ, 1),
                                      AI_COND(1, AI_OP_EQ, 1),
                                      AI_COND(2, AI_OP_EQ, 1) } };
    int32_t start[3] = { 0, 0, 0 };
    int32_t cost[3] = { 1, 1, 1 };
    uint8_t all[3] = { 1, 1, 1 };
    GoapQuery q = { start, cost, all, -1, 4 };
    GoapPlan plan;
    EXPECT_EQ(1, Goap_Solve(&d, &all3, &q, &plan));
    EXPECT_EQ(3, plan.step_count);
    EXPECT_EQ(0, plan.steps[0]);
    EXPECT_EQ(1, plan.steps[1]);
    EXPECT_EQ(2, plan.steps[2]);
    /* Eight worlds exist: none, three singles, three pairs, all. Six
     * orderings of three steps would be sixteen without the pruning. */
    EXPECT_EQ(8, plan.nodes);
}

static void test_goap_settles_for_nearer_when_told_it_may(void) {
    GoapGoal stack = { .want = { AI_COND(G_WOOD, AI_OP_GE, 4) } };
    int32_t start[G_VARS] = { 0, 1, 0, 0, 0 };
    GoapQuery q = camp_query(start, 2);
    GoapPlan plan;
    /* Four wood is four steps away and the plan may have two. */
    EXPECT_EQ(0, Goap_Solve(&k_camp_domain, &stack, &q, &plan));
    stack.partial_ok = 1;
    EXPECT_EQ(1, Goap_Solve(&k_camp_domain, &stack, &q, &plan));
    EXPECT_EQ(0, plan.complete);
    EXPECT_EQ(2, plan.step_count);
    EXPECT_EQ(A_CHOP, plan.steps[0]);
    EXPECT_EQ(A_CHOP, plan.steps[1]);
    EXPECT_EQ(2, plan.remaining);
    EXPECT_EQ(4, plan.cost);
    /* Nothing that moves towards it is no plan at all. */
    const GoapGoal gold = { .want = { AI_COND(G_GOLD, AI_OP_GE, 9) },
                            .partial_ok = 1 };
    EXPECT_EQ(0, Goap_Solve(&k_camp_domain, &gold, &q, &plan));
}

/* A search that cannot end still ends: the worlds it may open are
 * counted. */
static void test_goap_is_bounded(void) {
    static const GoapAction up[2] = {
        { .name = "up",   .eff = { AI_EFFECT(0, AI_EFF_ADD, 1) } },
        { .name = "side", .eff = { AI_EFFECT(1, AI_EFF_ADD, 1) } },
    };
    const GoapDomain d = { up, 2, 3 };
    const GoapGoal never = { .want = { AI_COND(2, AI_OP_GE, 1) } };
    int32_t start[3] = { 0, 0, 0 };
    int32_t cost[2] = { 1, 1 };
    uint8_t all[2] = { 1, 1 };
    GoapQuery q = { start, cost, all, -1, GOAP_MAX_DEPTH };
    GoapPlan plan;
    EXPECT_EQ(0, Goap_Solve(&d, &never, &q, &plan));
    EXPECT(plan.nodes <= GOAP_MAX_NODES);
    /* Bad questions are refused rather than searched. */
    EXPECT_EQ(0, Goap_Solve(NULL, &never, &q, &plan));
    q.start = NULL;
    EXPECT_EQ(0, Goap_Solve(&d, &never, &q, &plan));
}

/* ── The task planner ────────────────────────────────────────────── */

enum { H_X = 0, H_Y, H_VARS };
enum { HT_ROOT = 0, HT_PICK, HT_SET1, HT_SET2, HT_NEED2, HT_LOOP, HT_DONE,
       HT_MARK, HT_COUNT };

static const HtnTask k_net[HT_COUNT] = {
    [HT_ROOT] = { .name = "root", .compound = 1, .method_count = 1,
        .methods = { { .name = "pick then need",
                       .subtasks = { HT_PICK, HT_NEED2 }, .subtask_count = 2 } } },
    /* The first method fits where it stands and strands what follows. */
    [HT_PICK] = { .name = "pick", .compound = 1, .method_count = 2,
        .methods = { { .name = "one", .subtasks = { HT_SET1 }, .subtask_count = 1 },
                     { .name = "two", .subtasks = { HT_SET2 }, .subtask_count = 1 } } },
    [HT_SET1]  = { .name = "set 1", .eff = { AI_EFFECT(H_X, AI_EFF_SET, 1) } },
    [HT_SET2]  = { .name = "set 2", .eff = { AI_EFFECT(H_X, AI_EFF_SET, 2) } },
    [HT_NEED2] = { .name = "need 2", .pre = { AI_COND(H_X, AI_OP_EQ, 2) },
                   .eff = { AI_EFFECT(H_Y, AI_EFF_SET, 1) } },
    [HT_LOOP]  = { .name = "loop", .compound = 1, .method_count = 1,
        .methods = { { .name = "again", .subtasks = { HT_LOOP }, .subtask_count = 1 } } },
    [HT_DONE]  = { .name = "done", .compound = 1, .method_count = 2,
        .methods = { { .name = "already", .pre = { AI_COND(H_Y, AI_OP_EQ, 1) } },
                     { .name = "do it", .subtasks = { HT_MARK }, .subtask_count = 1 } } },
    [HT_MARK]  = { .name = "mark", .eff = { AI_EFFECT(H_Y, AI_EFF_SET, 1) } },
};
static const HtnDomain k_net_domain = { k_net, HT_COUNT, H_VARS };

/* The test a planner that only ever takes the first method that fits
 * cannot pass: "one" fits, "need 2" then cannot be placed, and the
 * plan exists only by going back for "two". */
static void test_htn_goes_back_for_another_method(void) {
    int32_t start[H_VARS] = { 0, 0 };
    HtnPlan plan;
    EXPECT_EQ(1, Htn_Plan(&k_net_domain, HT_ROOT, start, &plan));
    EXPECT_EQ(2, plan.step_count);
    EXPECT_EQ(HT_SET2, plan.steps[0]);
    EXPECT_EQ(HT_NEED2, plan.steps[1]);
    /* And it says how it got there: root by its only method, the pick
     * by its second. The dropped first is not in the record. */
    EXPECT_EQ(2, plan.via_count);
    EXPECT_EQ(HT_ROOT, plan.via_task[0]);
    EXPECT_EQ(0, plan.via_method[0]);
    EXPECT_EQ(HT_PICK, plan.via_task[1]);
    EXPECT_EQ(1, plan.via_method[1]);
    printf("(backtracked in %d expansions) ", plan.expansions);
}

static void test_htn_orders_methods_and_knows_a_done_task(void) {
    HtnPlan plan;
    /* Not done: the second method does it. */
    int32_t start[H_VARS] = { 0, 0 };
    EXPECT_EQ(1, Htn_Plan(&k_net_domain, HT_DONE, start, &plan));
    EXPECT_EQ(1, plan.step_count);
    EXPECT_EQ(HT_MARK, plan.steps[0]);
    /* Done already: the first method holds and asks for nothing. */
    int32_t done[H_VARS] = { 0, 1 };
    EXPECT_EQ(1, Htn_Plan(&k_net_domain, HT_DONE, done, &plan));
    EXPECT_EQ(0, plan.step_count);
    EXPECT_EQ(1, plan.via_count);
    EXPECT_EQ(0, plan.via_method[0]);
    /* A primitive as the root is its own plan, when it may be placed. */
    EXPECT_EQ(0, Htn_Plan(&k_net_domain, HT_NEED2, start, &plan));
    int32_t two[H_VARS] = { 2, 0 };
    EXPECT_EQ(1, Htn_Plan(&k_net_domain, HT_NEED2, two, &plan));
    EXPECT_EQ(1, plan.step_count);
}

static void test_htn_is_bounded(void) {
    int32_t start[H_VARS] = { 0, 0 };
    HtnPlan plan;
    /* A task that only ever becomes itself runs out of room and is
     * given up, well inside the expansion count. */
    EXPECT_EQ(0, Htn_Plan(&k_net_domain, HT_LOOP, start, &plan));
    EXPECT(plan.expansions > 0 && plan.expansions <= HTN_MAX_EXPANSIONS);
    EXPECT_EQ(0, plan.step_count);
    EXPECT_EQ(0, Htn_Plan(&k_net_domain, HT_COUNT, start, &plan));
    EXPECT_EQ(0, Htn_Plan(NULL, HT_ROOT, start, &plan));
}

/* A member that has run ahead of its group stands, and goes on once the
 * rest are within half the lead, so it does not stop and start (A-010). */
static void test_squad_a_member_ahead_waits(void) {
    /* Three members at 1000, 1000 and 400 px from the target: the mean
     * is 800, and 400 is more than the lead ahead of it. */
    int64_t sum = 1000 + 1000 + 400;
    EXPECT_EQ(1, AI_Squad_ShouldWait(400, sum, 3, 1));
    EXPECT_EQ(0, AI_Squad_ShouldWait(1000, sum, 3, 1));
    /* 600 is 200 ahead: marching on, but held once it stands. */
    sum = 1000 + 800 + 600;
    EXPECT_EQ(0, AI_Squad_ShouldWait(600, sum, 3, 1));
    EXPECT_EQ(1, AI_Squad_ShouldWait(600, sum, 3, 0));
    /* Two is not a squad. */
    EXPECT_EQ(0, AI_Squad_ShouldWait(0, 2000, 2, 1));
}

/* With nobody about the firing point is the one nearest the member. An
 * enemy standing between the member and the target pushes it round to
 * a side where the push is less (A-010). */
static void test_squad_firing_point_goes_round_the_enemy(void) {
    int32_t x = 0, y = 0;
    int k = AI_Squad_FiringPoint(1000, 1600, 1000, 1000, 200, NULL, 0, &x, &y);
    EXPECT_EQ(4, k);
    EXPECT_EQ(1000, x);
    EXPECT_EQ(1200, y);
    AiSquadCharge guard[3] = {
        { 1000, 1180, 12 }, { 960, 1200, 12 }, { 1040, 1200, 12 },
    };
    k = AI_Squad_FiringPoint(1000, 1600, 1000, 1000, 200, guard, 3, &x, &y);
    printf("  flank at %d,%d (point %d)\n", (int)x, (int)y, k);
    EXPECT(k != 4);
    EXPECT(AI_Squad_Push(x, y, guard, 3) < AI_Squad_Push(1000, 1200, guard, 3));
    /* Not behind the target from the member. */
    EXPECT(y >= 1000);
    /* The push fades to nothing at its reach. */
    EXPECT_EQ(0, (int)AI_Squad_Push(1000 + AI_SQUAD_CHARGE_REACH, 1180, guard, 1));
}

int main(void) {
    test_squad_a_member_ahead_waits();
    test_squad_firing_point_goes_round_the_enemy();
    test_facts_compare_scale_and_measure();
    test_goap_takes_the_cheapest_plan_however_long();
    test_goap_masks_tags_and_opening_moves();
    test_goap_breaks_ties_and_prunes_repeats();
    test_goap_settles_for_nearer_when_told_it_may();
    test_goap_is_bounded();
    test_htn_goes_back_for_another_method();
    test_htn_orders_methods_and_knows_a_done_task();
    test_htn_is_bounded();
    if (g_failures == 0) {
        printf("\nOK  test_ai_planners: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL  test_ai_planners: %d failure(s)\n", g_failures);
    return 1;
}
