/*
 * hud_text.c -- Renderer-based text drawing for the in-game HUD.
 *
 * Existing Font_DrawString writes to SDL_Surface (used by menus that
 * composite to canvas). The HUD draws straight to the SDL_Renderer,
 * so we need a GPU-side path: convert each Font glyph to a tiny
 * GPU_Texture once at load time, then per-char SDL_RenderCopy at
 * draw time. The total atlas memory is trivial (~95 glyphs * 100
 * px each = ~10 KB per font).
 */

#include "tak_hud_text.h"
#include "tak_font.h"
#include "tak_gpu.h"
#include "tak_memory.h"
#include "tak_ui.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* Mirror Font's ASCII range (font.h). */
#define HT_ASCII_FIRST  32
#define HT_ASCII_LAST   126
#define HT_ASCII_COUNT  (HT_ASCII_LAST - HT_ASCII_FIRST + 1)

struct HUDText {
    Font        *font;
    GPU_Texture *glyph_tex[HT_ASCII_COUNT];
    int          glyph_w[HT_ASCII_COUNT];
    int          glyph_h[HT_ASCII_COUNT];
    int          glyph_oy[HT_ASCII_COUNT];
    int          max_h;
    int          max_oy;
};

/* Internal accessor — Font is a struct; we duplicate its layout
 * inline since the public header (tak_font.h) already exposes the
 * fields via Font_LineHeight + Font_MeasureString in the existing
 * code. To keep this minimal we read what's reachable through the
 * public API and re-decode glyph pixels via an internal copy
 * pulled from Font's loaded GAF.
 *
 * For Sprint 1 we just call Font_DrawString into a temp surface
 * and upload the result on every cache miss. Less efficient than
 * a real glyph atlas, but works without modifying Font internals. */

HUDText *HUDText_Load(TAK_Platform *plat, Font *font) {
    if (!plat || !font) return NULL;
    HUDText *t = (HUDText *)tak_malloc(sizeof(HUDText));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->font  = font;
    t->max_h = Font_LineHeight(font);
    return t;
}

void HUDText_Free(TAK_Platform *plat, HUDText *t) {
    if (!t) return;
    for (int i = 0; i < HT_ASCII_COUNT; i++) {
        if (t->glyph_tex[i]) GPU_FreeTexture(plat, t->glyph_tex[i]);
    }
    tak_free(t);
}

int HUDText_LineHeight(const HUDText *t) {
    return t ? t->max_h : 0;
}

int HUDText_Measure(const HUDText *t, const char *s) {
    return (t && s) ? Font_MeasureString(t->font, s) : 0;
}

/* Draw a string at (x, y) on the renderer. Strategy: render the
 * whole string to a temp SDL_Surface using Font_DrawString (CPU),
 * upload the result as a one-shot GPU texture, blit, free.
 * O(string-length) CPU work per frame; acceptable for a few HUD
 * strings that change infrequently. A future optimisation builds
 * a per-glyph texture cache. */
void HUDText_DrawString(TAK_Platform *plat, HUDText *t,
                        int x, int y, const char *s,
                        SDL_Color tint)
{
    if (!plat || !plat->renderer || !t || !t->font || !s || !*s) return;
    int w = Font_MeasureString(t->font, s);
    int h = Font_LineHeight(t->font);
    if (w <= 0 || h <= 0) return;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return;
    SDL_FillRect(surf, NULL, 0);
    Font_DrawString(t->font, surf, 0, 0, s);

    /* Apply tint by multiplying RGB while preserving alpha. */
    if (tint.r != 255 || tint.g != 255 || tint.b != 255) {
        SDL_LockSurface(surf);
        uint32_t *p = (uint32_t *)surf->pixels;
        int n = w * h;
        for (int i = 0; i < n; i++) {
            uint32_t v = p[i];
            uint8_t a = (uint8_t)((v >> 24) & 0xFF);
            uint8_t b = (uint8_t)((v >> 16) & 0xFF);
            uint8_t g = (uint8_t)((v >>  8) & 0xFF);
            uint8_t r = (uint8_t)( v        & 0xFF);
            r = (uint8_t)((r * tint.r) / 255);
            g = (uint8_t)((g * tint.g) / 255);
            b = (uint8_t)((b * tint.b) / 255);
            p[i] = ((uint32_t)a << 24)
                 | ((uint32_t)b << 16)
                 | ((uint32_t)g <<  8)
                 |  (uint32_t)r;
        }
        SDL_UnlockSurface(surf);
    }

    GPU_Texture *tex = GPU_UploadRGBA(plat, (const uint32_t *)surf->pixels, w, h);
    SDL_FreeSurface(surf);
    if (!tex) return;
    GPU_SetTextureBlend(tex, 1);
    GPU_SetTextureFilter(tex, 0);
    SDL_Rect dst = { x, y, w, h };
    GPU_DrawToWindow(plat, tex, NULL, &dst);
    GPU_FreeTexture(plat, tex);
}
