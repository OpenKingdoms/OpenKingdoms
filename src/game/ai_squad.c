/*
 * ai_squad.c -- a squad on the march keeps in step (A-010).
 */

#include "tak_ai_squad.h"

int AI_Squad_ShouldWait(int32_t member_d, int64_t sum_d, int members,
                        int moving) {
    if (members < 3) return 0;
    int64_t mean = sum_d / members;
    int64_t lead = moving ? AI_SQUAD_LEAD : AI_SQUAD_LEAD / 2;
    return (int64_t)member_d < mean - lead;
}
