/*
 * multiplayer.c -- Multiplayer lobby (GAMESTATE_MULTIPLAYER).
 *
 * Thin wrapper around SimpleScreen for battlemenumulti.gui.
 */

#include "tak_multiplayer.h"
#include "tak_gameloop.h"
#include "tak_simple_screen.h"
#include <string.h>

static SimpleScreen mp;

static const SimpleScreenClick mp_routes[] = {
    { NULL, 0 }
};

int Multiplayer_Init(TAK_Platform *platform) {
    memset(&mp, 0, sizeof(mp));
    mp.gui_path       = "data/guis/battlemenumulti.gui";
    mp.self_state     = GAMESTATE_MULTIPLAYER;
    mp.default_return = GAMESTATE_MENU;
    mp.click_routes   = mp_routes;
    return SimpleScreen_Init(&mp, platform);
}

int Multiplayer_Tick(TAK_Platform *platform, float frame_dt) {
    return SimpleScreen_Tick(&mp, platform, frame_dt);
}

void Multiplayer_Shutdown(void) {
    SimpleScreen_Shutdown(&mp);
}
