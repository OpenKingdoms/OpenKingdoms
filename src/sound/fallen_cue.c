/*
 * fallen_cue.c -- the sound a fallen player is announced with.
 *
 * Our own addition (D-043): a low horn call over a muffled drum hit,
 * about a second long, made here from sines so no audio file ships
 * with it. It is a 16 bit mono WAV image for the sound layer.
 */

#include "tak_fallen_cue.h"
#include "tak_bytes.h"

#include <math.h>
#include <string.h>

#define CUE_RATE    22050
#define CUE_SECONDS 1.1
#define CUE_FRAMES  ((int)(CUE_RATE * CUE_SECONDS))

static double env(double t, double attack, double hold, double release) {
    if (t < attack) return t / attack;
    if (t < attack + hold) return 1.0;
    double r = (t - attack - hold) / release;
    return r >= 1.0 ? 0.0 : (1.0 - r) * (1.0 - r);
}

size_t TAK_FallenCue_Wav(uint8_t *out, size_t cap) {
    const size_t data_bytes = (size_t)CUE_FRAMES * 2u;
    const size_t total = 44u + data_bytes;
    if (!out || cap < total) return 0;
    memcpy(out, "RIFF", 4);
    tak_put_u32(out + 4, (uint32_t)(total - 8));
    memcpy(out + 8, "WAVEfmt ", 8);
    tak_put_u32(out + 16, 16);
    tak_put_u16(out + 20, 1);                 /* PCM */
    tak_put_u16(out + 22, 1);                 /* mono */
    tak_put_u32(out + 24, CUE_RATE);
    tak_put_u32(out + 28, CUE_RATE * 2);
    tak_put_u16(out + 32, 2);
    tak_put_u16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    tak_put_u32(out + 40, (uint32_t)data_bytes);

    const double two_pi = 6.283185307179586;
    double phase = 0.0;
    for (int i = 0; i < CUE_FRAMES; i++) {
        double t = (double)i / CUE_RATE;
        /* The horn: a low A that sags a little as it fades, warm
         * harmonics and a slow waver. */
        double f = 110.0 * (1.0 - 0.03 * (t > 0.55 ? (t - 0.55) / 0.55 : 0.0));
        f *= 1.0 + 0.004 * sin(two_pi * 5.0 * t);
        phase += two_pi * f / CUE_RATE;
        double horn = sin(phase) + 0.55 * sin(2.0 * phase) +
                      0.28 * sin(3.0 * phase) + 0.12 * sin(4.0 * phase);
        horn *= env(t, 0.09, 0.45, 0.55) * 0.42;
        /* The drum: a soft thump that dies in a fifth of a second. */
        double drum = sin(two_pi * 58.0 * t) * exp(-t * 18.0) * 0.55;
        double s = horn + drum;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        tak_put_i16(out + 44 + (size_t)i * 2u, (int16_t)(s * 26000.0));
    }
    return total;
}

size_t TAK_FallenCue_WavBytes(void) {
    return 44u + (size_t)CUE_FRAMES * 2u;
}
