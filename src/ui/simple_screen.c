/*
 * simple_screen.c -- shared implementation for .gui-driven screens.
 *
 * Non-complex screens (Story, Multiplayer, minor dialogs) all share the
 * same pattern: load a .gui, render it, hover tooltip, map named clicks
 * to state transitions, ESC → back. This file supplies the pattern so
 * new screens are ~10 lines of config instead of ~80 lines of boilerplate.
 */

#include "tak_simple_screen.h"
#include "tak_ui.h"
#include "tak_blit.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

int SimpleScreen_Init(SimpleScreen *s, TAK_Platform *platform) {
    (void)platform;
    if (!s || !s->gui_path) return -1;

    if (GUIDialog_Load(&s->dialog, s->gui_path) != 0) {
        fprintf(stderr, "SimpleScreen: failed to parse %s\n", s->gui_path);
        return -1;
    }
    s->rt = GUIRuntime_Create(&s->dialog);
    if (!s->rt) { GUIDialog_Free(&s->dialog); return -1; }

    s->tooltip_font = Font_Load("data/fonts/b_times new roman (100b)",
                                  UI_RGBAFormat());
    s->pending_nextstate = -1;
    s->initialized = 1;
    return 0;
}

void SimpleScreen_Shutdown(SimpleScreen *s) {
    if (!s || !s->initialized) return;
    if (s->rt)            GUIRuntime_Destroy(s->rt);
    if (s->tooltip_font)  Font_Free(s->tooltip_font);
    GUIDialog_Free(&s->dialog);
    s->rt = NULL;
    s->tooltip_font = NULL;
    s->initialized = 0;
}

static int resolve_click(const SimpleScreen *s, const char *name) {
    /* Built-in routes that every screen honors. */
    if (tak_stricmp(name, "Cancel") == 0 ||
        tak_stricmp(name, "Previous") == 0 ||
        tak_stricmp(name, "Return") == 0) {
        return s->default_return;
    }
    /* Per-screen overrides. */
    if (s->click_routes) {
        for (const SimpleScreenClick *r = s->click_routes; r->name; r++) {
            if (tak_stricmp(name, r->name) == 0) return r->next_state;
        }
    }
    return -1;   /* no transition */
}

int SimpleScreen_Tick(SimpleScreen *s, TAK_Platform *platform, float dt) {
    (void)dt;
    if (!s || !s->initialized) return s ? s->self_state : 0;

    int wx, wy, mx, my;
    SDL_GetMouseState(&wx, &wy);
    if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
        mx = -1; my = -1;
    }
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);

    char clicked[64];
    int got = GUIRuntime_Update(s->rt, mx, my, mouse_down, clicked, sizeof(clicked));
    if (got) {
        int next = resolve_click(s, clicked);
        if (next >= 0) {
            s->pending_nextstate = next;
        } else {
            fprintf(stderr, "SimpleScreen(%s): unhandled click '%s'\n",
                    s->gui_path, clicked);
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) s->pending_nextstate = s->default_return;

    /* Clear backdrop so a dialog smaller than 640x480 doesn't ghost. */
    SDL_Surface *off = UI_Offscreen();
    SDL_Rect full = { 0, 0, 640, 480 };
    SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 12, 12, 18, 255));

    GUIRuntime_Render(s->rt);
    if (s->after_render) s->after_render(s);

    /* HelpText strip — hovered widget's tooltip. */
    if (s->tooltip_font) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(s->rt);
        if (hw && hw->tooltip[0]) {
            const GUIWidget *help = GUIDialog_FindByName(&s->dialog, "HelpText");
            SDL_Rect r = help ? help->rect : (SDL_Rect){ 208, 452, 224, 30 };
            int tw = Font_MeasureString(s->tooltip_font, hw->tooltip);
            Font_DrawString(s->tooltip_font, off,
                             r.x + (r.w - tw) / 2, r.y + 4, hw->tooltip);
        }
    }

    UI_Present(platform);

    int next = (s->pending_nextstate >= 0) ? s->pending_nextstate : s->self_state;
    s->pending_nextstate = -1;
    return next;
}
