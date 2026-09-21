#include "tak_view_shake.h"

static struct {
    int      magnitude;
    int      frames;        /* the length it was started with */
    int      left;
    uint32_t rng;
} sh;

/* The original draws from the C library's generator, 15 bits a draw.
 * This is a generator of its own, so a shake never moves the
 * simulation's or anybody else's. */
static int shake_rand15(void) {
    sh.rng = sh.rng * 1103515245u + 12345u;
    return (int)((sh.rng >> 16) & 0x7fff);
}

void ViewShake_Reset(void) {
    sh.magnitude = sh.frames = sh.left = 0;
    sh.rng = 0x5eed1234u;
}

int ViewShake_Active(void) { return sh.left > 0; }

void ViewShake_Start(int magnitude, int frames) {
    if (magnitude <= 0 || frames <= 0) return;
    if (sh.left <= 0) {
        sh.magnitude = magnitude;
        sh.frames = frames;
    } else {
        sh.frames = (sh.frames + frames) / 2;
        sh.magnitude += magnitude;
    }
    sh.left = sh.frames;
}

void ViewShake_Step(int32_t *out_dx, int32_t *out_dy) {
    int32_t dx = 0, dy = 0;
    if (sh.left > 0 && sh.frames > 0) {
        /* The box is the magnitude scaled by what is left, and the
         * step is a draw across it centred on nought. */
        int box = sh.magnitude * sh.left / sh.frames;
        dx = (int32_t)((int64_t)shake_rand15() * box / 0x8000) - box / 2;
        dy = (int32_t)((int64_t)shake_rand15() * box / 0x8000) - box / 2;
        sh.left--;
    }
    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
}
