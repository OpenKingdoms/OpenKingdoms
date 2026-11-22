/* Do two builds of this engine agree on the float maths the simulation
 * leans on? Lockstep needs every machine to reach the same state from
 * the same orders, and the movement code calls sinf, cosf, atan2f and
 * sqrtf about sixty times over. Those come from each platform's own
 * libm, which is where two builds are free to differ.
 *
 * Prints one hash per operation and one for the lot. Same number on
 * two platforms means those operations agree bit for bit.
 *
 * Not wired to a build: it is run by hand on each platform and the
 * numbers compared. docs/notes/2026-09-04-multiplayer-hapinet.md holds
 * what they were when this was written.
 *
 *   cl /O2 tools/float_probe.c && float_probe.exe
 *   gcc -O2 tools/float_probe.c -lm -o float_probe && ./float_probe
 *   emcc -O2 tools/float_probe.c -o probe.js && node probe.js
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t h_fnv;

static void feed(float v) {
    uint32_t bits;
    /* The bit pattern, not the value: two floats that print the same
     * can still differ in the last place. */
    unsigned char *p = (unsigned char *)&v;
    bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    for (int i = 0; i < 4; i++) {
        h_fnv ^= (bits >> (i * 8)) & 0xffu;
        h_fnv *= 16777619u;
    }
}

static uint32_t take(void) {
    uint32_t h = h_fnv;
    h_fnv = 2166136261u;
    return h;
}

int main(void) {
    h_fnv = 2166136261u;
    uint32_t all = 2166136261u;

    /* Headings over a whole turn, the way a unit's facing moves. */
    for (int i = 0; i < 65536; i += 7) {
        float a = (float)i * (6.2831853f / 65536.0f);
        feed(sinf(a));
        feed(cosf(a));
    }
    uint32_t h_trig = take();

    /* Distance between two units, the shape the target scan uses. */
    for (int i = 1; i < 20000; i += 3) {
        float dx = (float)(i % 977) - 488.0f;
        float dy = (float)(i % 613) - 306.0f;
        feed(sqrtf(dx * dx + dy * dy));
    }
    uint32_t h_dist = take();

    /* The angle from one unit to another, which aim and steering use. */
    for (int i = 1; i < 20000; i += 3) {
        float dx = (float)(i % 977) - 488.0f;
        float dy = (float)(i % 613) - 306.0f;
        feed(atan2f(dy, dx));
    }
    uint32_t h_aim = take();

    /* A position built up a tick at a time, which is where a last place
     * difference turns into a different cell. */
    float px = 1024.0f, py = 2048.0f;
    for (int t = 0; t < 60000; t++) {
        float a = (float)(t % 65536) * (6.2831853f / 65536.0f);
        px += sinf(a) * 1.7f;
        py -= cosf(a) * 1.7f;
        if (px > 6000.0f) px -= 6000.0f;
        if (py < 0.0f) py += 6000.0f;
        feed(px);
        feed(py);
    }
    uint32_t h_walk = take();

    uint32_t parts[4] = { h_trig, h_dist, h_aim, h_walk };
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 4; b++) {
            all ^= (parts[i] >> (b * 8)) & 0xffu;
            all *= 16777619u;
        }
    }

    printf("trig  %08x\n", h_trig);
    printf("dist  %08x\n", h_dist);
    printf("aim   %08x\n", h_aim);
    printf("walk  %08x\n", h_walk);
    printf("all   %08x\n", all);
    return 0;
}
