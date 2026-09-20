#include "tak_ai_htn.h"

/* A wave waits for a third of what the seat fields, at least one so a
 * seat with a single unit still presses, and never more than
 * AI_HTN_MAX_LAUNCH. The original ratchets a group's launch threshold
 * on what its owner holds (legacy:16187). */
int AI_Htn_LaunchCount(int members) {
    int n = 1 + (members > 0 ? members : 0) / 3;
    return n > AI_HTN_MAX_LAUNCH ? AI_HTN_MAX_LAUNCH : n;
}

/* The method for an attack goal, its subtasks in the order they are
 * tried. Nothing to go at is a hold, a wave at strength or out of
 * patience strikes, an unseen target with nobody already out gets
 * one member to look at it, and otherwise the wave gathers. The
 * scout is drawn only from a gathering of two or more, so a seat
 * never sends its last member away to look, and one march at a time
 * is enough to hold the next scout back. */
AiTask AI_Htn_WaveTask(const AiWaveState *s) {
    if (!s || !s->target_known) return AI_TASK_HOLD;
    if (s->massed >= s->launch || s->patience_due) return AI_TASK_STRIKE;
    if (!s->target_seen && !s->marching && s->massed >= 2) return AI_TASK_SCOUT;
    return AI_TASK_MASS;
}

/* A member away from the staging point is not called back: it goes on
 * with the wave, and only the ones at home wait. */
AiTask AI_Htn_MemberTask(const AiWaveState *s, int is_scout, int at_stage) {
    AiTask task = AI_Htn_WaveTask(s);
    if (task == AI_TASK_HOLD) return AI_TASK_HOLD;
    if (task == AI_TASK_STRIKE) return AI_TASK_STRIKE;
    if (task == AI_TASK_SCOUT && is_scout) return AI_TASK_SCOUT;
    return at_stage ? AI_TASK_MASS : AI_TASK_STRIKE;
}
