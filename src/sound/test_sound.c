/*
 * test_sound.c: the parts of the sound layer that need no device and
 * no game data: the viewport volume and pan rule, channel stealing,
 * and the debug recorder tests lean on.
 */

#include "test_framework.h"
#include "tak_sound.h"
#include "tak_game_sound.h"
#include "tak_memory.h"

#include <string.h>

/* Inside the viewport a source plays at 0x7f, anywhere outside at
 * 0x40, with no distance term (legacy:221181-221190). */
TEST(spatialize_volume_is_two_step) {
    int vol = 0, pan = 0;
    TAK_Sound_Spatialize(500, 300, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, vol);
    TAK_Sound_Spatialize(1001, 300, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    /* Just past the bottom edge is as quiet as far across the map. */
    TAK_Sound_Spatialize(500, 601, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    TAK_Sound_Spatialize(500, 9000, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    /* The camera offset moves the window with it. */
    TAK_Sound_Spatialize(2500, 2300, 2000, 2000, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, vol);
}

/* Pan is 64 plus 64 times the offset from the viewport centre over the
 * whole viewport width, clamped (legacy:221191-221194). */
TEST(spatialize_pan_runs_across_viewport_width) {
    int vol = 0, pan = 0;
    TAK_Sound_Spatialize(500, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, pan);
    TAK_Sound_Spatialize(1000, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x60, pan);          /* right edge: half way to hard right */
    TAK_Sound_Spatialize(0, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x20, pan);
    TAK_Sound_Spatialize(-600, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0, pan);             /* clamped */
    TAK_Sound_Spatialize(3000, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, pan);
    /* No viewport at all: centre. */
    TAK_Sound_Spatialize(3000, 0, 0, 0, 0, 0, &vol, &pan);
    ASSERT_EQ_INT(0x40, pan);
}

/* Only a strictly lower priority can be stolen, the lowest first and
 * the oldest among equals (legacy:308165-308203). */
TEST(choose_victim_steals_strictly_lower_priority_only) {
    int      active[4]   = { 1, 1, 1, 0 };
    int      priority[4] = { 4, 2, 2, 0 };
    uint32_t serial[4]   = { 10, 7, 3, 1 };
    /* A priority 4 request: both 2s qualify, the older (serial 3) goes. */
    ASSERT_EQ_INT(2, TAK_Sound_ChooseVictim(4, active, priority, serial, 4));
    /* A priority 2 request finds nothing strictly lower. */
    ASSERT_EQ_INT(-1, TAK_Sound_ChooseVictim(4, active, priority, serial, 2));
    /* A priority 7 request takes the lowest, not the oldest. */
    ASSERT_EQ_INT(2, TAK_Sound_ChooseVictim(4, active, priority, serial, 7));
    priority[0] = 1;
    ASSERT_EQ_INT(0, TAK_Sound_ChooseVictim(4, active, priority, serial, 7));
    /* A free slot is never a victim even at priority 0. */
    ASSERT_EQ_INT(-1, TAK_Sound_ChooseVictim(4, active, priority, serial, 0));
}

/* The recorder keeps the resolved name, the spatial result and whether
 * the wav was found. Without a VFS nothing loads, which is the point:
 * the trigger is still observable. */
TEST(debug_recorder_keeps_events_in_order) {
    GameSound_Init();
    GameSound_DebugRecord(1);
    GameSound_DebugClear();
    GameSound_PlayUI("oktobuild");
    GameSound_PlayWorldWav("CHITGRND.wav", 4, 100, 100, 0, 0, 640, 480);
    GameSound_PlayWorldWav("CHITGRND.wav", 4, 5000, 100, 0, 0, 640, 480);
    ASSERT_EQ_INT(3, GameSound_DebugCount());
    const GameSoundEvent *ev = GameSound_DebugEvent(0);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_STR("oktobuild", ev->name);
    ASSERT_EQ_INT(0, ev->positional);
    ASSERT_EQ_INT(0x7f, ev->volume);
    ASSERT_EQ_INT(7, ev->priority);
    ASSERT_EQ_INT(0, ev->loaded);
    ev = GameSound_DebugEvent(1);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_INT(1, ev->positional);
    ASSERT_EQ_INT(0x7f, ev->volume);
    ASSERT_EQ_INT(4, ev->priority);
    ev = GameSound_DebugEvent(2);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_INT(0x40, ev->volume);
    ASSERT_EQ_INT(0x7f, ev->pan);
    ASSERT_EQ_INT(2, GameSound_DebugCountPrefix("chitgrnd"));
    ASSERT_EQ_INT(2, GameSound_DebugFindPrefix("CHITGRND"));
    ASSERT_EQ_INT(-1, GameSound_DebugFindPrefix("AHIT"));
    GameSound_DebugClear();
    ASSERT_EQ_INT(0, GameSound_DebugCount());
    GameSound_DebugRecord(0);
    GameSound_PlayUI("oktobuild");
    ASSERT_EQ_INT(0, GameSound_DebugCount());
    GameSound_Shutdown();
}

int main(void) {
    TEST_SUITE("sound");
    tak_mem_init();
    RUN(spatialize_volume_is_two_step);
    RUN(spatialize_pan_runs_across_viewport_width);
    RUN(choose_victim_steals_strictly_lower_priority_only);
    RUN(debug_recorder_keeps_events_in_order);
    TEST_REPORT();
}
