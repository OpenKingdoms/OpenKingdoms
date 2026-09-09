#ifndef TAK_SIMPLE_SCREEN_H
#define TAK_SIMPLE_SCREEN_H

#include "tak_platform.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_font.h"

/*
 * SimpleScreen — a .gui-driven screen with the standard lifecycle:
 * load a single .gui, render it, hover + tooltip strip, route clicks
 * to a small name→state mapping, and return to the configured
 * return_state on ESC or "Previous"/"Cancel".
 *
 * Used by Story (bod.gui), Multiplayer (battlemenumulti.gui), and any
 * future screen that's purely "show this .gui and let me go back".
 * Complex screens (Options with tabs, BattleSetup with BattleConfig)
 * get their own file.
 */

typedef struct {
    const char *name;       /* widget name */
    int         next_state; /* GAMESTATE_* to transition to */
} SimpleScreenClick;

typedef struct {
    /* Configuration — set by owner before Init. */
    const char              *gui_path;        /* "data/guis/bod.gui" */
    int                      self_state;      /* this screen's GAMESTATE_ enum */
    int                      default_return;  /* where ESC/Cancel goes */
    const SimpleScreenClick *click_routes;    /* NULL-terminated; name==NULL terminates */

    /* Runtime — owned by SimpleScreen after Init. */
    int          initialized;
    GUIDialog    dialog;
    GUIRuntime  *rt;
    Font        *tooltip_font;
    int          pending_nextstate;
} SimpleScreen;

int  SimpleScreen_Init(SimpleScreen *s, TAK_Platform *platform);
int  SimpleScreen_Tick(SimpleScreen *s, TAK_Platform *platform, float dt);
void SimpleScreen_Shutdown(SimpleScreen *s);

#endif
