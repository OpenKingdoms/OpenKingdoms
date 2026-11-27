/* fog_mist.c -- the hardware mode's mist (D-053): the overlay's lattice
 * drawn as an even veil under one drifting, swelling cloud, weighted by a blur. */

#include "tak_fog_mist.h"
#include "tak_fog.h"
#include "tak_world.h"
#include "tak_gpu.h"
#include "tak_memory.h"
#include <SDL.h>
#include <string.h>

#define MIST_TEX    512
#define MIST_CHUNK  16383     /* quads a draw, under 16-bit indices */
#define MIST_PAD    1         /* the blur's reach in cells */
/* A side without a set of its own: ground goes MIST_DENSITY of the way to
 * FogColor times a grey cloud of mean MIST_SHADE swinging by MIST_BILLOW. */
#define MIST_DENSITY 0.535f
#define MIST_SHADE   0.58f
#define MIST_BILLOW  0.3f
/* The 3x3 binomial blur of the mist, 0..256, is clear up to MIST_EDGE: the
 * clear area's edge scallops cell by cell and feathers over about a cell. */
#define MIST_EDGE    166
/* A cloud of MIST_TEXEL_C hundredths of a px a texel over an even veil, its
 * share swelling every MIST_SWELL_MS. Fitted to captures, then set by eye. */
#define MIST_TEXEL_C    660
#define MIST_DRIFT_X    -13   /* px a second */
#define MIST_DRIFT_Y    11
#define MIST_SWELL_MS   10250
#define MIST_SWELL_LOW  0.15f
#define MIST_SWELL_HIGH 0.70f
/* A second pass of the cloud drifts its own way and takes MIST2_SHARE of its
 * part, so the billows change shape as they cross, as in the original. */
#define MIST2_TEXEL_C   520
#define MIST2_DRIFT_X   14    /* px a second */
#define MIST2_DRIFT_Y   -14
#define MIST2_OFFSET_U  171
#define MIST2_OFFSET_V  97
#define MIST2_SHARE     0.40f

/* A side's set by its sidedata name, fitted to the original's fog of it. */
typedef struct MistSet { const char *side; float density, shade, billow; } MistSet;
static const MistSet k_sets[] = {
    { "ARAMON", 0.60f, 0.50f, 0.45f },
    { "TAROS",  0.54f, 0.44f, 0.61f },
};
static const MistSet k_set_plain = { "", MIST_DENSITY, MIST_SHADE, MIST_BILLOW };

static const MistSet *mist_set_of(const char *side) {
    for (int k = 0; side && k < (int)(sizeof k_sets / sizeof k_sets[0]); k++)
        if (SDL_strcasecmp(k_sets[k].side, side) == 0) return &k_sets[k];
    return &k_set_plain;
}

static const char *g_side;   /* the side the mist draws for */

static const int k_drift[2][2] = { { MIST_DRIFT_X, MIST_DRIFT_Y }, { MIST2_DRIFT_X, MIST2_DRIFT_Y } };

static GPU_Texture *g_tex;
static uint32_t     g_tex_gen;
static float        g_tex_shade, g_tex_billow;   /* the cloud g_tex holds */

static float       *g_xy, *g_uv;
static uint32_t    *g_col;
static uint16_t    *g_idx;
static uint8_t     *g_flag;     /* 1 where the mist lies, padded */
static uint16_t    *g_row;      /* the blur across */
static uint16_t    *g_w;        /* corner weights, 0..256 */
static uint8_t     *g_black;    /* corner under the opaque black */
static int          g_grid_cap;

static uint32_t mist_hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

/* Value noise on an n by n lattice that wraps, so the tile repeats. */
static float mist_value(float u, float v, int n, uint32_t seed) {
    float fx = u * (float)n, fy = v * (float)n;
    int ix = (int)fx, iy = (int)fy;
    float tx = fx - (float)ix, ty = fy - (float)iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    uint32_t x0 = (uint32_t)(ix % n), x1 = (uint32_t)((ix + 1) % n);
    uint32_t y0 = (uint32_t)(iy % n), y1 = (uint32_t)((iy + 1) % n);
    float v00 = (float)(mist_hash(x0, y0, seed) & 0xffffu) / 65535.0f;
    float v10 = (float)(mist_hash(x1, y0, seed) & 0xffffu) / 65535.0f;
    float v01 = (float)(mist_hash(x0, y1, seed) & 0xffffu) / 65535.0f;
    float v11 = (float)(mist_hash(x1, y1, seed) & 0xffffu) / 65535.0f;
    float a = v00 + (v10 - v00) * tx, b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

/* Five octaves of grey cloud of mean `shade` swinging by `billow`, opaque.
 * Texel 0 holds the plain shade, which the veil samples alone. */
static void mist_fill(uint32_t *px, float shade, float billow) {
    for (int y = 0; y < MIST_TEX; y++) {
        for (int x = 0; x < MIST_TEX; x++) {
            float u = ((float)x + 0.5f) / MIST_TEX, v = ((float)y + 0.5f) / MIST_TEX;
            float s = 0.0f, amp = 0.5f, tot = 0.0f;
            for (int o = 0, n = 8; o < 5; o++, n *= 2) {
                s += amp * mist_value(u, v, n, (uint32_t)o + 1u);
                tot += amp;
                amp *= 0.5f;
            }
            float d = (s / tot - 0.28f) / 0.44f;
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            float g = shade + billow * (2.0f * d - 1.0f);
            g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
            Uint8 lum = (Uint8)(g * 255.0f + 0.5f);
            SDL_Color c = { lum, lum, lum, 255 };
            memcpy(&px[y * MIST_TEX + x], &c, 4);
        }
    }
    Uint8 plain = (Uint8)(shade * 255.0f + 0.5f);
    SDL_Color c0 = { plain, plain, plain, 255 };
    memcpy(&px[0], &c0, 4);
}

/* One texture a renderer, straight alpha, the strength in the vertex colour. */
static GPU_Texture *mist_texture(TAK_Platform *plat) {
    if (plat->renderer_gen == 0) return NULL;
    const MistSet *set = mist_set_of(g_side);
    if (g_tex && g_tex_gen != plat->renderer_gen) {
        GPU_AbandonTexture(g_tex);
        g_tex = NULL;
    }
    if (g_tex && (g_tex_shade != set->shade || g_tex_billow != set->billow)) {
        GPU_FreeTexture(plat, g_tex);
        g_tex = NULL;
    }
    if (!g_tex) {
        uint32_t *px = (uint32_t *)tak_malloc((size_t)MIST_TEX * MIST_TEX * 4);
        if (!px) return NULL;
        g_tex_shade = set->shade;
        g_tex_billow = set->billow;
        mist_fill(px, g_tex_shade, g_tex_billow);
        g_tex = GPU_UploadRGBA(plat, px, MIST_TEX, MIST_TEX);
        tak_free(px);
        if (!g_tex) return NULL;
        GPU_SetTextureBlend(g_tex, GPU_BLEND_ALPHA);
        GPU_SetTextureFilter(g_tex, 1);
        g_tex_gen = plat->renderer_gen;
    }
    return g_tex;
}

static void mist_buffers_free(void) {
    tak_free(g_xy); tak_free(g_uv); tak_free(g_col); tak_free(g_idx);
    g_xy = g_uv = NULL;
    g_col = NULL;
    g_idx = NULL;
}

static int mist_buffers(void) {
    if (g_xy) return 1;
    g_xy = (float *)tak_malloc((size_t)MIST_CHUNK * 8 * sizeof(float));
    g_uv = (float *)tak_malloc((size_t)MIST_CHUNK * 8 * sizeof(float));
    g_col = (uint32_t *)tak_malloc((size_t)MIST_CHUNK * 4 * sizeof(uint32_t));
    g_idx = (uint16_t *)tak_malloc((size_t)MIST_CHUNK * 6 * sizeof(uint16_t));
    if (!g_xy || !g_uv || !g_col || !g_idx) {
        mist_buffers_free();
        return 0;
    }
    for (int q = 0; q < MIST_CHUNK; q++) {
        uint16_t b = (uint16_t)(q * 4);
        uint16_t *ix = &g_idx[q * 6];
        ix[0] = b; ix[1] = (uint16_t)(b + 1); ix[2] = (uint16_t)(b + 2);
        ix[3] = b; ix[4] = (uint16_t)(b + 2); ix[5] = (uint16_t)(b + 3);
    }
    return 1;
}

static int mist_grid(int cells) {
    if (cells <= g_grid_cap) return 1;
    tak_free(g_flag); tak_free(g_row); tak_free(g_w); tak_free(g_black);
    g_flag = (uint8_t *)tak_malloc((size_t)cells);
    g_row = (uint16_t *)tak_malloc((size_t)cells * sizeof(uint16_t));
    g_w = (uint16_t *)tak_malloc((size_t)cells * sizeof(uint16_t));
    g_black = (uint8_t *)tak_malloc((size_t)cells);
    g_grid_cap = (g_flag && g_row && g_w && g_black) ? cells : 0;
    return g_grid_cap != 0;
}

typedef struct MistDraw {
    TAK_Platform      *plat;
    const GPU_Texture *tex;
    int                quads;
} MistDraw;

static void mist_flush(MistDraw *d) {
    if (d->quads > 0)
        GPU_DrawGeometryRaw(d->plat, d->tex, g_xy, g_col, g_uv, d->quads * 4,
                            g_idx, d->quads * 6);
    d->quads = 0;
}

static float mist_unit(float t) {
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

/* A quad w px (at most a tile) by a cell, split at the tile's seam: SDL
 * does not promise wrapped texture coordinates on every backend. */
static void mist_quad(MistDraw *d, float sx, float sy, float w, const float a[4],
                      float u0, float v0, float texel, SDL_Color tint) {
    const float P = (float)MIST_TEX, cell = (float)TAK_FOG_CELL_PX;
    float xs[3] = { 0.0f, w, w }, ys[3] = { 0.0f, cell, cell };
    int nx = 1, ny = 1;
    if (u0 + w / texel > P) { xs[1] = (P - u0) * texel; nx = 2; }
    if (v0 + cell / texel > P) { ys[1] = (P - v0) * texel; ny = 2; }
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            if (d->quads == MIST_CHUNK) mist_flush(d);
            int b = d->quads * 4;
            for (int k = 0; k < 4; k++) {
                float px = xs[i + (k == 1 || k == 2)];
                float py = ys[j + (k >= 2)];
                float fx = px / w, fy = py / cell;
                float top = a[0] + (a[1] - a[0]) * fx;
                float bot = a[3] + (a[2] - a[3]) * fx;
                SDL_Color c = tint;
                c.a = (Uint8)(top + (bot - top) * fy + 0.5f);
                memcpy(&g_col[b + k], &c, 4);
                g_xy[(b + k) * 2]     = sx + px;
                g_xy[(b + k) * 2 + 1] = sy + py;
                /* Clamped: SDL refuses a coordinate a rounding past 0 or 1. */
                float tu = mist_unit((u0 + px / texel - (i ? P : 0.0f)) / P);
                float tv = mist_unit((v0 + py / texel - (j ? P : 0.0f)) / P);
                g_uv[(b + k) * 2]     = tu;
                g_uv[(b + k) * 2 + 1] = tv;
            }
            d->quads++;
        }
    }
}

/* One more than a quad's weight when its four corners share it and are
 * not all under the black, else 0. */
static int mist_flat(const uint16_t *w0, const uint8_t *b0, int gw) {
    const uint16_t *w1 = w0 + gw;
    const uint8_t *b1 = b0 + gw;
    if (w0[0] != w0[1] || w0[0] != w1[0] || w0[0] != w1[1]) return 0;
    if (b0[0] & b0[1] & b1[1] & b1[0]) return 0;
    return w0[0] + 1;
}

/* The drift in texels, wrapped in whole numbers so a long clock keeps
 * its precision. */
static float mist_drift(int px_per_s, int texel_c, uint32_t clock_ms) {
    int64_t period = (int64_t)MIST_TEX * texel_c * 10;
    int64_t at = ((int64_t)px_per_s * (int64_t)clock_ms) % period;
    if (at < 0) at += period;
    return (float)at / (10.0f * (float)texel_c);
}

/* The cloud's share of the density, from MIST_SWELL_LOW up to _HIGH and back. */
static float mist_cloud_share(uint32_t clock_ms) {
    const float t = (float)(clock_ms % MIST_SWELL_MS) / (float)MIST_SWELL_MS;
    const float s = 0.5f - 0.5f * SDL_cosf(6.28318531f * t);
    return MIST_SWELL_LOW + (MIST_SWELL_HIGH - MIST_SWELL_LOW) * s;
}

void FogMist_Render(const GameWorld *world, TAK_Platform *plat, uint32_t clock_ms,
                    const char *side, const uint8_t rgb[3]) {
    if (!world || !plat || !plat->renderer || !rgb) return;
    g_side = side;
    int viewer = Fog_Viewer();
    if (viewer < 1 || viewer > TAK_MAX_PLAYERS || !world->fog_layers[viewer]) return;
    /* No fog of war with Line of Sight off, only the black. */
    if (!world->cfg.line_of_sight) {
        Fog_RenderShroud(world, plat);
        return;
    }
    const uint8_t *layer = world->fog_layers[viewer];
    const int cell = TAK_FOG_CELL_PX, half = cell / 2;
    /* Drawn TAK_FOG_LIFT_PX up, as the overlay is. */
    const int top = world->cam_y + TAK_FOG_LIFT_PX;
    const int c0x = (world->cam_x - half) / cell - 1;
    const int c0y = (top - half) / cell - 1;
    const int c1x = (world->cam_x + world->viewport_w + half) / cell + 1;
    const int c1y = (top + world->viewport_h + half) / cell + 1;
    /* Corners run c0..c1+1, and the blur reads MIST_PAD more each way. */
    const int gw = c1x - c0x + 2, gh = c1y - c0y + 2;
    const int fw = gw + 2 * MIST_PAD, fh = gh + 2 * MIST_PAD;
    GPU_Texture *tex = mist_texture(plat);
    if (!tex || !mist_grid(fw * fh) || !mist_buffers()) {
        Fog_RenderOverlay(world, plat);   /* never leave fogged ground clear */
        return;
    }

    for (int y = 0; y < fh; y++) {
        int cy = c0y - MIST_PAD + y;
        for (int x = 0; x < fw; x++) {
            int cx = c0x - MIST_PAD + x;
            uint8_t s = TAK_FOG_UNEXPLORED;
            if (cx >= 0 && cy >= 0 && cx < world->fog_w && cy < world->fog_h)
                s = layer[cy * world->fog_w + cx];
            g_flag[y * fw + x] = (uint8_t)(s != TAK_FOG_VISIBLE);
        }
    }
    for (int y = 0; y < fh; y++) {
        const uint8_t *f = &g_flag[y * fw];
        for (int x = 0; x < gw; x++)
            g_row[y * fw + x] = (uint16_t)(f[x] + 2 * f[x + 1] + f[x + 2]);
    }
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const uint16_t *c = &g_row[y * fw + x];
            int cx = c0x + x, cy = c0y + y;
            int b = 16 * (c[0] + 2 * c[fw] + c[2 * fw]);
            int wv = (b - MIST_EDGE) * 256 / (256 - MIST_EDGE);
            g_w[y * gw + x] = (uint16_t)(wv < 0 ? 0 : wv > 256 ? 256 : wv);
            g_black[y * gw + x] = (uint8_t)(cx < 0 || cy < 0 || cx >= world->fog_w ||
                cy >= world->fog_h || layer[cy * world->fog_w + cx] == TAK_FOG_UNEXPLORED);
        }
    }

    MistDraw d = { plat, tex, 0 };
    SDL_Color tint = { rgb[0], rgb[1], rgb[2], 0 };
    /* Veil and clouds let 1 - density through between them, so the mean
     * holds while the clouds' share swells. */
    const float clear = 1.0f - mist_set_of(g_side)->density;
    const float share = mist_cloud_share(clock_ms), second = MIST2_SHARE;
    const float alpha[3] = { (1.0f - SDL_powf(clear, 1.0f - share)) * 255.0f / 256.0f,
                             (1.0f - SDL_powf(clear, share * (1.0f - second))) * 255.0f / 256.0f,
                             (1.0f - SDL_powf(clear, share * second)) * 255.0f / 256.0f };
    for (int L = 0; L < 3; L++) {
        /* The veil reads texel 0 alone: no step across, set on its centre. */
        const int tc = L == 2 ? MIST2_TEXEL_C : MIST_TEXEL_C;
        const int *dr = k_drift[L == 2];
        const float ou = L == 2 ? (float)MIST2_OFFSET_U : 0.0f;
        const float ov = L == 2 ? (float)MIST2_OFFSET_V : 0.0f;
        const float texel = L ? (float)tc / 100.0f : 1e9f, P = (float)MIST_TEX;
        const float du = L ? mist_drift(dr[0], tc, clock_ms) + ou : 0.5f;
        const float dv = L ? mist_drift(dr[1], tc, clock_ms) + ov : 0.5f;
        const int run_max = L ? MIST_TEX * tc / (100 * cell) : gw;   /* a tile wide */
        const float k = alpha[L];
        for (int y = 0; y + 1 < gh; y++) {
            for (int x = 0; x + 1 < gw;) {
                const uint16_t *w0 = &g_w[y * gw + x], *w1 = w0 + gw;
                const uint8_t *b0 = &g_black[y * gw + x], *b1 = b0 + gw;
                if ((w0[0] | w0[1] | w1[1] | w1[0]) == 0 ||
                    (b0[0] & b0[1] & b1[1] & b1[0])) {   /* clear, or under the black */
                    x++;
                    continue;
                }
                /* A run of even quads goes as one, which most of the mist is. */
                int run = 1, flat = mist_flat(w0, b0, gw);
                while (flat && run < run_max && x + run + 1 < gw &&
                       mist_flat(w0 + run, b0 + run, gw) == flat) run++;
                float a[4] = { w0[0] * k, w0[run] * k, w1[run] * k, w1[0] * k };
                int wx = (c0x + x) * cell + half, wy = (c0y + y) * cell + half;
                float u0 = SDL_fmodf((float)wx / texel + du, P);
                float v0 = SDL_fmodf((float)wy / texel + dv, P);
                if (u0 < 0.0f) u0 += P;
                if (v0 < 0.0f) v0 += P;
                mist_quad(&d, (float)(wx - world->cam_x), (float)(wy - top),
                          (float)(run * cell), a, u0, v0, texel, tint);
                x += run;
            }
        }
    }
    mist_flush(&d);
    Fog_RenderShroud(world, plat);
}

void FogMist_Shutdown(TAK_Platform *plat) {
    if (g_tex) {
        if (plat && g_tex_gen == plat->renderer_gen) GPU_FreeTexture(plat, g_tex);
        else GPU_AbandonTexture(g_tex);
        g_tex = NULL;
    }
    mist_buffers_free();
    tak_free(g_flag); tak_free(g_row); tak_free(g_w); tak_free(g_black);
    g_flag = NULL; g_row = NULL; g_w = NULL; g_black = NULL;
    g_grid_cap = 0;
}
