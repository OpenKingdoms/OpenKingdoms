#include "tak_battle_config.h"
#include <string.h>

/* The ten authored player colours, in frame order. RGB is the dominant
 * colour of each team-logo frame, so the swatch we draw when the art is
 * missing matches the art that would have been drawn. */
static const TakPlayerColor bc_player_colors[TAK_PLAYER_COLOR_COUNT] = {
    { "Blue",   176, 176, 255 },
    { "Red",    255,  51,  51 },
    { "White",  255, 255, 255 },
    { "Green",   64, 255, 113 },
    { "Navy",    89,  89, 255 },
    { "Maroon", 206,  10, 206 },
    { "Gold",   255, 225,   0 },
    { "Black",  127, 127, 127 },
    { "Orange", 255, 140,   0 },
    { "Brown",  157, 122,  70 },
};

const TakPlayerColor *BattleConfig_PlayerColor(int index) {
    int n = TAK_PLAYER_COLOR_COUNT;
    if (index < 0) index = 0;
    return &bc_player_colors[index % n];
}

int BattleConfig_NextFreeColor(const BattleConfig *cfg,
                               int slot_index, int from) {
    int n = TAK_PLAYER_COLOR_COUNT;
    if (!cfg) return 0;
    if (from < 0) from = 0;
    int want = from % n;
    /* Legacy scans the slot table for the candidate, bumps past it and
     * retries, giving up after one pass (legacy:135368). */
    for (int guard = 0; guard < n; guard++) {
        int taken = 0;
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            if (i == slot_index) continue;
            if (cfg->players[i].kind == TAK_SLOT_CLOSED) continue;
            if (cfg->players[i].color == want) { taken = 1; break; }
        }
        if (!taken) return want;
        want++;
        if (want >= n) want = 0;
    }
    return from % n;
}

/* Its own stream, so dealing the starts leaves the simulation's draws
 * where they were. */
static uint32_t start_draw(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    uint32_t x = *state;
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void BattleConfig_AssignStarts(const BattleConfig *cfg, int count,
                               int out[TAK_MAX_PLAYERS]) {
    int taken[TAK_MAX_PLAYERS] = { 0 };
    int free_starts[TAK_MAX_PLAYERS];
    int nfree = 0;
    if (count < 0) count = 0;
    if (count > TAK_MAX_PLAYERS) count = TAK_MAX_PLAYERS;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        out[i] = -1;
        if (!cfg || cfg->players[i].kind == TAK_SLOT_CLOSED) continue;
        int want = cfg->players[i].start_pos - 1;
        if (want >= 0 && want < count && !taken[want]) {
            out[i] = want;
            taken[want] = 1;
        }
    }
    for (int k = 0; k < count; k++)
        if (!taken[k]) free_starts[nfree++] = k;
    /* The seats with no claim, and the starts the fixed deal gives them. */
    int m = 0;
    for (int i = 0; i < TAK_MAX_PLAYERS && cfg; i++)
        if (cfg->players[i].kind != TAK_SLOT_CLOSED && out[i] < 0) m++;
    if (m > nfree) m = nfree;
    if (cfg && cfg->random_start_locations && m > 1) {
        /* The original trades those starts among those seats: from
         * three on in one cycle, so none keeps its own, and two swap on
         * a coin flip (legacy:195522-195576). */
        uint32_t state = cfg->seed ^ 0x53544152u;
        if (m >= 3 || (start_draw(&state) & 1u)) {
            for (int k = 1; k < m; k++) {
                int j = (int)(start_draw(&state) % (uint32_t)k);
                int t = free_starts[k];
                free_starts[k] = free_starts[j];
                free_starts[j] = t;
            }
        }
    }
    int next = 0;
    for (int i = 0; i < TAK_MAX_PLAYERS && cfg; i++) {
        if (cfg->players[i].kind == TAK_SLOT_CLOSED || out[i] >= 0) continue;
        if (next < nfree) out[i] = free_starts[next++];
    }
}

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
    cfg->crusades_balance      = 0;
    cfg->remastered            = 0;
    cfg->seed                  = 0;   /* the lobby draws a real one */

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
        cfg->players[i].color = i % TAK_PLAYER_COLOR_COUNT;
    }
}
