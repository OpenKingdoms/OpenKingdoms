#include "tak_ai_goap.h"

#include <string.h>

typedef struct GoapNode {
    int32_t world[AI_FACT_MAX_VARS];
    int32_t g;
    int32_t gap;
    uint8_t steps[GOAP_MAX_DEPTH];
    uint8_t depth;
    uint8_t open;
} GoapNode;

/* One search at a time: the simulation is single threaded and the
 * scratch is too large for a browser's stack. */
static GoapNode g_nodes[GOAP_MAX_NODES];

/* Shorter first, then the one whose actions come first in the table. */
static int goap_path_before(const GoapNode *a, const GoapNode *b) {
    if (a->depth != b->depth) return a->depth < b->depth;
    for (int i = 0; i < a->depth; i++)
        if (a->steps[i] != b->steps[i]) return a->steps[i] < b->steps[i];
    return 0;
}

static int goap_cheaper(const GoapNode *a, const GoapNode *b) {
    if (a->g != b->g) return a->g < b->g;
    return goap_path_before(a, b);
}

static int32_t goap_goal_gap(const GoapGoal *goal, const int32_t *world,
                             const int32_t *start) {
    int64_t sum = 0;
    for (int i = 0; i < AI_FACT_MAX_CONDS; i++)
        sum += AI_Fact_Gap(&goal->want[i], world, start);
    return sum > 0x3fffffff ? 0x3fffffff : (int32_t)sum;
}

/* A world already reached at least as cheaply and as early makes this
 * one redundant. */
static int goap_redundant(const GoapNode *n, int count, size_t world_bytes) {
    for (int i = 0; i < count; i++) {
        const GoapNode *o = &g_nodes[i];
        if (o->g > n->g || o->depth > n->depth) continue;
        if (memcmp(o->world, n->world, world_bytes) != 0) continue;
        if (o->g == n->g && o->depth == n->depth &&
            goap_path_before(n, o)) continue;
        return 1;
    }
    return 0;
}

int Goap_Solve(const GoapDomain *d, const GoapGoal *goal, const GoapQuery *q,
               GoapPlan *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!d || !goal || !q || !out || !q->start || !q->cost || !q->allowed)
        return 0;
    if (d->action_count <= 0 || d->action_count > GOAP_MAX_ACTIONS) return 0;
    if (d->var_count <= 0 || d->var_count > AI_FACT_MAX_VARS) return 0;
    int max_depth = q->max_depth;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > GOAP_MAX_DEPTH) max_depth = GOAP_MAX_DEPTH;
    const size_t world_bytes = sizeof(int32_t) * (size_t)d->var_count;

    int count = 1;
    GoapNode *root = &g_nodes[0];
    memset(root, 0, sizeof(*root));
    memcpy(root->world, q->start, world_bytes);
    root->gap = goap_goal_gap(goal, root->world, q->start);
    root->open = 1;
    const int32_t start_gap = root->gap;
    int best_partial = -1;

    for (;;) {
        int pick = -1;
        for (int i = 0; i < count; i++) {
            if (!g_nodes[i].open) continue;
            if (pick < 0 || goap_cheaper(&g_nodes[i], &g_nodes[pick])) pick = i;
        }
        if (pick < 0) break;
        GoapNode *n = &g_nodes[pick];
        n->open = 0;
        if (n->depth > 0 && n->gap == 0) {
            out->step_count = n->depth;
            memcpy(out->steps, n->steps, n->depth);
            out->cost = n->g;
            out->complete = 1;
            out->nodes = count;
            return 1;
        }
        if (n->depth >= max_depth) continue;
        for (int a = 0; a < d->action_count && count < GOAP_MAX_NODES; a++) {
            const GoapAction *act = &d->actions[a];
            if (!q->allowed[a]) continue;
            if (act->opening_only && n->depth > 0) continue;
            if (n->depth == 0 && q->first_tag >= 0 &&
                (int)act->tag != q->first_tag) continue;
            if (!AI_Fact_AllHold(act->pre, AI_FACT_MAX_CONDS, n->world,
                                 q->start)) continue;
            GoapNode *c = &g_nodes[count];
            *c = *n;
            AI_Fact_Apply(act->eff, AI_FACT_MAX_EFFECTS, c->world);
            c->steps[c->depth++] = (uint8_t)a;
            c->g = n->g + (q->cost[a] > 0 ? q->cost[a] : 0);
            c->gap = goap_goal_gap(goal, c->world, q->start);
            c->open = 1;
            if (goap_redundant(c, count, world_bytes)) continue;
            if (c->gap < start_gap &&
                (best_partial < 0 ||
                 c->gap < g_nodes[best_partial].gap ||
                 (c->gap == g_nodes[best_partial].gap &&
                  goap_cheaper(c, &g_nodes[best_partial])))) {
                best_partial = count;
            }
            count++;
        }
    }

    out->nodes = count;
    if (!goal->partial_ok || best_partial < 0) return 0;
    const GoapNode *b = &g_nodes[best_partial];
    out->step_count = b->depth;
    memcpy(out->steps, b->steps, b->depth);
    out->cost = b->g;
    out->complete = 0;
    out->remaining = b->gap;
    return 1;
}
