#ifndef TAK_HUD_TEXT_H
#define TAK_HUD_TEXT_H

#include "tak_platform.h"
#include <SDL.h>

/* Renderer-based text drawing for the HUD. Wraps the existing
 * surface-based Font system with a per-string GPU upload so we
 * can blit through SDL_Renderer (HUD's render target) instead of
 * SDL_Surface. */

typedef struct HUDText HUDText;
typedef struct Font    Font;

HUDText *HUDText_Load(TAK_Platform *plat, Font *font);
void     HUDText_Free(TAK_Platform *plat, HUDText *t);

int  HUDText_LineHeight(const HUDText *t);
int  HUDText_Measure(const HUDText *t, const char *s);

/* Draw `s` at window pixel (x, y) tinted by `tint`. White tint
 * (255,255,255,255) renders the font's natural colour. */
void HUDText_DrawString(TAK_Platform *plat, HUDText *t,
                        int x, int y, const char *s,
                        SDL_Color tint);

#endif /* TAK_HUD_TEXT_H */
