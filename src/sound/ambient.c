/*
 * ambient.c: ambient feature sounds.
 *
 * Every ten frames the original walks the map cells inside the
 * viewport and, while a mixer channel is free, counts down a timer on
 * each feature whose definition names a SoundClass. When a timer runs
 * out the class plays flat at volume 0x40, priority 1, and the timer
 * re-arms to SoundDelay plus or minus SoundVariance. Only a lit cell
 * counts (legacy:128619-128712). The timers are presentation state
 * and never touch the simulation.
 */

#include "tak_ambient.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_sound.h"
#include "tak_game_sound.h"
#include "tak_memory.h"

#include <stdlib.h>

#define AMBIENT_STEP_TICKS 20   /* ten legacy frames at 30 Hz */

/* One countdown per emitter, in steps. The original keeps it in the
 * emitter's own cell (legacy:128676-128706), so it is keyed by cell
 * and def here: feature slots move whenever a body falls or rots. */
typedef struct {
    uint16_t tile_x, tile_z;
    int32_t  def;
    uint16_t steps;
} AmbientTimer;

static AmbientTimer *g_timers = NULL;
static int           g_timer_count = 0;
static int           g_timer_cap = 0;
static uint32_t      g_tick = 0;

void Ambient_Reset(void) {
    if (g_timers) tak_free(g_timers);
    g_timers = NULL;
    g_timer_count = 0;
    g_timer_cap = 0;
    g_tick = 0;
}

/* Delay plus a uniform draw in [-variance, +variance], in steps, at
 * least one step (legacy:128698-128706). Presentation randomness, so
 * the C runtime generator is the right one. */
static uint16_t ambient_rearm(const FeatureDef *fd) {
    int ticks = fd->sound_delay_ticks;
    if (fd->sound_variance_ticks > 0) {
        int span = fd->sound_variance_ticks * 2;
        ticks += (int)(((long long)rand() * span) / ((long long)RAND_MAX + 1))
                 - fd->sound_variance_ticks;
    }
    int steps = ticks / AMBIENT_STEP_TICKS;
    if (steps < 1) steps = 1;
    if (steps > 0x7ff) steps = 0x7ff;
    return (uint16_t)steps;
}

/* The emitter's countdown, unarmed (0) the first time it is seen.
 * NULL only when memory runs out. */
static AmbientTimer *ambient_timer_for(const struct MapFeature *mf) {
    for (int i = 0; i < g_timer_count; i++) {
        AmbientTimer *t = &g_timers[i];
        if (t->tile_x == mf->tile_x && t->tile_z == mf->tile_z &&
            t->def == mf->global_idx) return t;
    }
    if (g_timer_count >= g_timer_cap) {
        int cap = g_timer_cap ? g_timer_cap * 2 : 32;
        AmbientTimer *grown = (AmbientTimer *)tak_realloc(
            g_timers, (size_t)cap * sizeof(AmbientTimer));
        if (!grown) return NULL;
        g_timers = grown;
        g_timer_cap = cap;
    }
    AmbientTimer *t = &g_timers[g_timer_count++];
    t->tile_x = mf->tile_x;
    t->tile_z = mf->tile_z;
    t->def = mf->global_idx;
    t->steps = 0;
    return t;
}

void Ambient_Tick(const struct GameWorld *world) {
    if (!world || !world->loaded) return;
    g_tick++;
    if (g_tick % AMBIENT_STEP_TICKS != 0) return;
    int budget = TAK_Sound_FreeChannels();
    if (budget <= 0) return;

    for (int i = 0; i < world->feature_count; i++) {
        const struct MapFeature *mf = &world->features[i];
        if (mf->global_idx < 0) continue;
        const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
        if (!fd || !fd->sound_class[0] || fd->sound_delay_ticks <= 0) continue;

        int32_t wx = (int32_t)mf->tile_x * 16 + fd->footprint_x * 8;
        int32_t wy = (int32_t)mf->tile_z * 16 + fd->footprint_z * 8;
        if (wx < world->cam_x || wx > world->cam_x + world->viewport_w ||
            wy < world->cam_y || wy > world->cam_y + world->viewport_h) continue;
        if (world->cfg.line_of_sight && !Fog_IsVisible(world, wx, wy)) continue;

        AmbientTimer *t = ambient_timer_for(mf);
        if (!t) continue;
        if (t->steps == 0) {
            t->steps = ambient_rearm(fd);
            continue;
        }
        if (--t->steps > 0) continue;
        GameSound_PlayClass2D(fd->sound_class, NULL, 0x40, 1);
        t->steps = ambient_rearm(fd);
        if (--budget <= 0) return;
    }
}
