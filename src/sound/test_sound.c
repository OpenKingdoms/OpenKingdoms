/*
 * test_sound.c: the parts of the sound layer that need no device and
 * no game data: the viewport volume and pan rule, channel stealing,
 * and the debug recorder tests lean on.
 */

#include "test_framework.h"
#include "tak_sound.h"
#include "tak_game_sound.h"
#include "tak_memory.h"
#include "tak_hpi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define tsnd_mkdir(p) _mkdir(p)
#define tsnd_rmdir(p) _rmdir(p)
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#define tsnd_mkdir(p) mkdir((p), 0755)
#define tsnd_rmdir(p) rmdir(p)
#endif

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

static void put_le(uint8_t *p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* A short silent 16-bit mono wav, built here so the test needs no
 * game data. */
static int write_silent_wav(const char *path, uint32_t frames) {
    uint8_t h[44];
    uint32_t data_bytes = frames * 2;
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);          /* PCM */
    put_le(h + 22, 1, 2);          /* mono */
    put_le(h + 24, 22050, 4);
    put_le(h + 28, 44100, 4);      /* bytes per second */
    put_le(h + 32, 2, 2);          /* block align */
    put_le(h + 34, 16, 2);         /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_bytes, 4);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(h, 1, sizeof(h), f);
    for (uint32_t i = 0; i < data_bytes; i++) fputc(0, f);
    return fclose(f) == 0 ? 0 : -1;
}

#define CACHE_TEST_ROOT "test_sound_vfs"
#define CACHE_TEST_WAV  CACHE_TEST_ROOT "/sounds/cacheprobe.wav"

static void cache_test_cleanup(void) {
    VFS_Shutdown();
    remove(CACHE_TEST_WAV);
    tsnd_rmdir(CACHE_TEST_ROOT "/sounds");
    tsnd_rmdir(CACHE_TEST_ROOT);
}

/* Every name asked for stays cached, found or not. After six hundred
 * missing names a real wav decodes once, a second play allocates
 * nothing, and shutdown frees it. The cache used to stop at 512, so a
 * later name decoded again on every play and was never freed. */
TEST(wav_cache_keeps_every_name) {
    tsnd_mkdir(CACHE_TEST_ROOT);
    tsnd_mkdir(CACHE_TEST_ROOT "/sounds");
    ASSERT_EQ_INT(0, write_silent_wav(CACHE_TEST_WAV, 64));
    ASSERT_EQ_INT(0, VFS_Init(CACHE_TEST_ROOT, CACHE_TEST_ROOT));
    GameSound_Init();
    char name[32];
    for (int i = 0; i < 600; i++) {
        snprintf(name, sizeof(name), "nosuchwav%03d", i);
        GameSound_PlayUI(name);
    }
    GameSound_DebugRecord(1);
    GameSound_DebugClear();
    TakMemStats before, first, second, end;
    tak_mem_get_stats(&before);
    GameSound_PlayUI("cacheprobe");
    tak_mem_get_stats(&first);
    GameSound_PlayUI("cacheprobe");
    tak_mem_get_stats(&second);
    GameSound_DebugRecord(0);
    GameSound_Shutdown();
    tak_mem_get_stats(&end);
    int loaded = GameSound_DebugCount() == 2 &&
                 GameSound_DebugEvent(0)->loaded &&
                 GameSound_DebugEvent(1)->loaded;
    cache_test_cleanup();
    ASSERT(loaded);
    ASSERT_EQ_INT((int)first.live_alloc_count, (int)second.live_alloc_count);
    ASSERT((int)end.live_alloc_count <= (int)before.live_alloc_count);
}

int main(void) {
    TEST_SUITE("sound");
    tak_mem_init();
    RUN(spatialize_volume_is_two_step);
    RUN(spatialize_pan_runs_across_viewport_width);
    RUN(choose_victim_steals_strictly_lower_priority_only);
    RUN(debug_recorder_keeps_events_in_order);
    RUN(wav_cache_keeps_every_name);
    TEST_REPORT();
}
