#include "tak_battle_config.h"
#include <string.h>

void BattleConfig_SetDefaults(BattleConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->units_per_side        = TAK_UNITS_PER_SIDE_DEFAULT;
    cfg->line_of_sight         = 1;
    cfg->map_revealed          = 0;
    cfg->monarch_expendable    = 0;
    cfg->random_start_locations = 0;
    cfg->power_codes           = 0;
    cfg->slow_game             = 0;

    /* Default lineup: slot 0 = human/Aramon, slot 1 = AI/Taros,
     * remaining slots closed. */
    cfg->players[0].kind = TAK_SLOT_HUMAN;
    cfg->players[0].side = TAK_SIDE_ARAMON;
    cfg->players[0].team = 1;
    cfg->players[0].color = 0;
    strncpy(cfg->players[0].name, "Player", sizeof(cfg->players[0].name) - 1);

    cfg->players[1].kind = TAK_SLOT_AI;
    cfg->players[1].side = TAK_SIDE_TAROS;
    cfg->players[1].team = 2;
    cfg->players[1].color = 1;
    cfg->players[1].ai_difficulty = 1;
    strncpy(cfg->players[1].name, "Al Shane", sizeof(cfg->players[1].name) - 1);

    for (int i = 2; i < TAK_MAX_PLAYERS; i++) {
        cfg->players[i].kind = TAK_SLOT_CLOSED;
    }
    /* Every slot starts on its own colour — otherwise any slot the user
     * opens beyond the first two spawns colour 0 and the whole map
     * renders blue. The setup screen's PlayerColor cell overrides. */
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        cfg->players[i].color = i % 10;
    }
}
