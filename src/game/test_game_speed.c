/* Game speed: the level, the range, the on screen line, the timer that
 * runs on it, and the proof that none of it reaches the simulation.
 *
 * Headless and data free, so CI runs it.
 */

#include "tak_game_speed.h"
#include "tak_gameloop.h"
#include "tak_ingame_keys.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "test_framework.h"

#include <SDL.h>
#include <math.h>
#include <string.h>

/* -- the level ------------------------------------------------------- */

static void speed_fixture(void) {
    GameSpeed_Reset();
    GameSpeed_SetAvailable(1);
    GameSpeed_SetStrings("Game Speed Normal", "Game Speed");
    GameSpeed_SetMessageSeconds(5.0);
}

TEST(level_starts_normal) {
    speed_fixture();
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetEffectiveLevel());
    ASSERT(fabs(GameSpeed_Multiplier() - 1.0) < 1e-9);
}

TEST(one_press_is_one_step) {
    speed_fixture();
    GameSpeed_Increase();
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
    GameSpeed_Increase();
    ASSERT_EQ_INT(12, GameSpeed_GetLevel());
    GameSpeed_Decrease();
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
}

TEST(the_top_of_the_range_is_double_speed) {
    speed_fixture();
    for (int i = 0; i < 40; i++) GameSpeed_Increase();
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MAX, GameSpeed_GetLevel());
    ASSERT(fabs(GameSpeed_Multiplier() - 2.0) < 1e-9);
}

TEST(the_bottom_of_the_range_is_a_full_stop) {
    speed_fixture();
    for (int i = 0; i < 40; i++) GameSpeed_Decrease();
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MIN, GameSpeed_GetLevel());
    ASSERT(fabs(GameSpeed_Multiplier() - 0.0) < 1e-9);
}

TEST(a_level_outside_the_range_clamps) {
    speed_fixture();
    GameSpeed_SetLevel(500);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MAX, GameSpeed_GetLevel());
    GameSpeed_SetLevel(-500);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MIN, GameSpeed_GetLevel());
}

TEST(a_networked_battle_has_no_speed_control) {
    GameSpeed_Reset();
    GameSpeed_SetAvailable(0);
    GameSpeed_Increase();
    GameSpeed_Increase();
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
    GameSpeed_Decrease();
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
    GameSpeed_SetLevel(3);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
    ASSERT_EQ_STR("", GameSpeed_Message());
}

/* -- the line on screen ---------------------------------------------- */

TEST(normal_speed_prints_its_own_string) {
    speed_fixture();
    GameSpeed_SetLevel(9);
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_NORMAL);
    ASSERT_EQ_STR("Game Speed Normal", GameSpeed_Message());
}

TEST(a_faster_speed_prints_a_signed_offset) {
    speed_fixture();
    GameSpeed_SetLevel(15);
    ASSERT_EQ_STR("Game Speed +5", GameSpeed_Message());
}

/* Two spaces, and that is not a typo. The original formats "%s %c%d"
 * with a sign character that is a plus when the offset is positive and a
 * space when it is negative, because the number brings its own minus
 * (legacy:131785-131787). A negative offset therefore prints the space
 * from the format and the space from the sign. */
TEST(a_slower_speed_prints_a_negative_offset) {
    speed_fixture();
    GameSpeed_SetLevel(7);
    ASSERT_EQ_STR("Game Speed  -3", GameSpeed_Message());
}

TEST(the_line_ages_out) {
    speed_fixture();
    GameSpeed_SetLevel(15);
    ASSERT_EQ_STR("Game Speed +5", GameSpeed_Message());
    GameSpeed_AgeMessage(4.9);
    ASSERT_EQ_STR("Game Speed +5", GameSpeed_Message());
    GameSpeed_AgeMessage(0.2);
    ASSERT_EQ_STR("", GameSpeed_Message());
}

TEST(setting_the_same_level_again_says_nothing) {
    speed_fixture();
    GameSpeed_SetLevel(15);
    GameSpeed_AgeMessage(99.0);
    GameSpeed_SetLevel(15);
    ASSERT_EQ_STR("", GameSpeed_Message());
}

TEST(the_strings_come_from_the_translate_table) {
    speed_fixture();
    GameSpeed_SetStrings("Tempo Normal", "Spielgeschw.");
    GameSpeed_SetLevel(12);
    ASSERT_EQ_STR("Spielgeschw. +2", GameSpeed_Message());
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_NORMAL);
    ASSERT_EQ_STR("Tempo Normal", GameSpeed_Message());
}

/* -- the machine that cannot keep up --------------------------------- */

TEST(sustained_saturation_walks_the_effective_level_down) {
    speed_fixture();
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_MAX);
    ASSERT_EQ_INT(20, GameSpeed_GetEffectiveLevel());
    /* Eleven frames that spent the whole budget (legacy:242410-242414). */
    for (int i = 0; i < 11; i++) GameSpeed_NoteFrame(8, 8);
    ASSERT_EQ_INT(19, GameSpeed_GetEffectiveLevel());
    /* The number the player chose does not move. */
    ASSERT_EQ_INT(20, GameSpeed_GetLevel());
}

TEST(recovery_walks_the_effective_level_back_up) {
    speed_fixture();
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_MAX);
    for (int i = 0; i < 11; i++) GameSpeed_NoteFrame(8, 8);
    ASSERT_EQ_INT(19, GameSpeed_GetEffectiveLevel());
    for (int i = 0; i < 101; i++) GameSpeed_NoteFrame(2, 8);
    ASSERT_EQ_INT(20, GameSpeed_GetEffectiveLevel());
}

TEST(recovery_never_passes_the_requested_level) {
    speed_fixture();
    GameSpeed_SetLevel(12);
    for (int i = 0; i < 500; i++) GameSpeed_NoteFrame(0, 8);
    ASSERT_EQ_INT(12, GameSpeed_GetEffectiveLevel());
}

/* -- the timer ------------------------------------------------------- */

/* Ticks produced by `frames` frames of `frame_dt` wall time at `speed`. */
static int ticks_over(double speed, double frame_dt, int frames) {
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, speed);
    int ticks = 0;
    for (int f = 0; f < frames; f++) {
        Timer_Advance(&t, frame_dt);
        while (Timer_ConsumeTick(&t)) ticks++;
    }
    return ticks;
}

TEST(a_tick_is_always_the_same_length) {
    Timer t;
    Timer_Init(&t);
    double dt = t.sim_dt;
    Timer_SetSpeed(&t, 2.0);
    ASSERT(t.sim_dt == dt);
    Timer_SetSpeed(&t, 0.1);
    ASSERT(t.sim_dt == dt);
}

TEST(double_speed_runs_twice_the_ticks) {
    int one = ticks_over(1.0, 1.0 / 60.0, 600);
    int two = ticks_over(2.0, 1.0 / 60.0, 600);
    ASSERT(one >= 595 && one <= 601);
    ASSERT(two >= 1195 && two <= 1201);
}

TEST(a_full_stop_runs_no_ticks) {
    ASSERT_EQ_INT(0, ticks_over(0.0, 1.0 / 60.0, 600));
}

/* The catch-up cap is the trap. A fixed cap of four ticks per frame is
 * spent by one frame of a 20 fps machine at double speed, and from there
 * the surplus is thrown away every frame, so the readout says 2x and the
 * battle runs slower than that with nothing telling the player. The cap
 * has to scale with the speed. */
TEST(the_catch_up_cap_scales_with_the_speed) {
    Timer t;
    Timer_Init(&t);
    int base = t.max_ticks_per_frame;
    Timer_SetSpeed(&t, 2.0);
    ASSERT_EQ_INT(base * 2, t.max_ticks_per_frame);
    Timer_SetSpeed(&t, 1.0);
    ASSERT_EQ_INT(base, t.max_ticks_per_frame);
    /* Below normal the cap never drops under the normal budget. */
    Timer_SetSpeed(&t, 0.1);
    ASSERT_EQ_INT(base, t.max_ticks_per_frame);
}

TEST(double_speed_on_a_slow_machine_really_is_double) {
    /* 100 frames of 1/20 s is five seconds of wall time. At 2x that has
     * to be ten seconds of battle, 600 ticks. A fixed cap of four gives
     * 400 and the feature silently does nothing. */
    int slow = ticks_over(2.0, 1.0 / 20.0, 100);
    ASSERT(slow >= 595 && slow <= 601);
}

/* -- the keys -------------------------------------------------------- */

static uint8_t kb[SDL_NUM_SCANCODES];
static uint8_t kb_prev[SDL_NUM_SCANCODES];

static void tap(SDL_Scancode sc, int alt) {
    memset(kb, 0, sizeof(kb));
    memset(kb_prev, 0, sizeof(kb_prev));
    kb[sc] = 1;
    if (alt) kb[SDL_SCANCODE_LALT] = 1;
    InGame_ApplySpeedKeys(kb, kb_prev);
}

/* Four characters, two keys on a US layout, plus the keypad pair that
 * produces the same characters (keys.tdf SYMBOL_2B, SYMBOL_3D for up,
 * SYMBOL_2D, SYMBOL_5F for down). */
TEST(the_speed_keys_change_the_speed) {
    speed_fixture();
    tap(SDL_SCANCODE_EQUALS, 0);
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
    tap(SDL_SCANCODE_KP_PLUS, 0);
    ASSERT_EQ_INT(12, GameSpeed_GetLevel());
    tap(SDL_SCANCODE_MINUS, 0);
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
    tap(SDL_SCANCODE_KP_MINUS, 0);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
}

TEST(a_held_speed_key_steps_once) {
    speed_fixture();
    memset(kb, 0, sizeof(kb));
    memset(kb_prev, 0, sizeof(kb_prev));
    kb[SDL_SCANCODE_EQUALS] = 1;
    InGame_ApplySpeedKeys(kb, kb_prev);
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
    memcpy(kb_prev, kb, sizeof(kb_prev));
    for (int i = 0; i < 30; i++) InGame_ApplySpeedKeys(kb, kb_prev);
    ASSERT_EQ_INT(11, GameSpeed_GetLevel());
}

/* Alt is the debug modifier and no shipped binding answers it. */
TEST(alt_and_a_speed_key_is_not_a_speed_change) {
    speed_fixture();
    tap(SDL_SCANCODE_EQUALS, 1);
    tap(SDL_SCANCODE_MINUS, 1);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
}

TEST(the_speed_keys_do_nothing_in_a_networked_battle) {
    GameSpeed_Reset();
    GameSpeed_SetAvailable(0);
    tap(SDL_SCANCODE_EQUALS, 0);
    tap(SDL_SCANCODE_EQUALS, 0);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_NORMAL, GameSpeed_GetLevel());
}

TEST(the_keys_walk_the_whole_range_and_stop_at_both_ends) {
    speed_fixture();
    for (int i = 0; i < 30; i++) tap(SDL_SCANCODE_EQUALS, 0);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MAX, GameSpeed_GetLevel());
    for (int i = 0; i < 60; i++) tap(SDL_SCANCODE_MINUS, 0);
    ASSERT_EQ_INT(GAME_SPEED_LEVEL_MIN, GameSpeed_GetLevel());
}

/* -- determinism ----------------------------------------------------- */

/* The simulation state sim_hash.c reads. The real ones are file scope
 * statics inside units.c and ai.c, so this stands in for them the same
 * way test_sim_hash.c does, and the number that comes out is the engine
 * composite over a real world, a real unit array and the real random
 * stream. */
#define SIM_UNITS 4

static GameWorld  sim_world;
static Unit       sim_units[SIM_UNITS];
static uint32_t   sim_ai_state;

GameWorld *World_Get(void) { return &sim_world; }

/* The engine tick, kept so a save's round trip can be checked. */
static uint32_t g_stub_sim_tick;
uint32_t Units_SimTick(void) { return g_stub_sim_tick; }
void Units_SetSimTick(uint32_t tick) { g_stub_sim_tick = tick; }

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = SIM_UNITS;
    return sim_units;
}

const Projectile *Units_GetProjectiles(int *out_count) {
    if (out_count) *out_count = 0;
    return NULL;
}

uint32_t TAK_SimHash_AI(uint32_t h) { return TAK_HashU32(h, sim_ai_state); }
/* No mission runs here. */
uint32_t TAK_SimHash_Mission(uint32_t h) { return h; }

/* One tick of a battle. It moves units, spends mana and draws from the
 * simulation generator, and it has no idea what speed it is running at,
 * which is the whole point. */
static void sim_tick(void) {
    for (int i = 0; i < SIM_UNITS; i++) {
        Unit *u = &sim_units[i];
        u->world_x += (int32_t)World_Rand(64u) - 32;
        u->world_y += (int32_t)World_Rand(64u) - 32;
        u->heading += (float)World_Rand(64u) * 0.01f;
        u->health -= (int32_t)World_Rand(3u);
        if (u->health < 0) u->health = 100;
    }
    sim_world.economy.players[1].mana += (float)World_Rand(16u);
    sim_world.skirmish_elapsed_ticks++;
    sim_ai_state = sim_ai_state * 1664525u + 1013904223u;
}

static void sim_reset(void) {
    memset(&sim_world, 0, sizeof(sim_world));
    memset(sim_units, 0, sizeof(sim_units));
    for (int i = 0; i < SIM_UNITS; i++) {
        sim_units[i].stable_id = (uint32_t)(i + 1);
        sim_units[i].world_x = 100 * (i + 1);
        sim_units[i].world_y = 50 * (i + 1);
        sim_units[i].health = 100;
        sim_units[i].max_health = 100;
        sim_units[i].alive = UNIT_ALIVE_ACTIVE;
        sim_units[i].player_id = 1;
        sim_units[i].target = -1;
    }
    sim_ai_state = 0x5eedu;
    World_SeedRand(0xabcdu);
}

/* Run the real timer at `speed` until `want` ticks have been simulated,
 * and hand back the simulation hash. */
static uint32_t run_battle(double speed, int want) {
    sim_reset();
    Timer t;
    Timer_Init(&t);
    Timer_SetSpeed(&t, speed);
    int ticks = 0;
    int frames = 0;
    while (ticks < want && frames < 200000) {
        Timer_Advance(&t, 1.0 / 60.0);
        frames++;
        while (ticks < want && Timer_ConsumeTick(&t)) {
            sim_tick();
            ticks++;
        }
    }
    return TAK_SimHash();
}

/* The hash has to move with a single tick, or the cases below would
 * pass on a number that never changes. */
TEST(one_more_tick_is_a_different_hash) {
    ASSERT(run_battle(1.0, 900) != run_battle(1.0, 901));
}

TEST(the_same_battle_hashes_the_same_at_any_speed) {
    uint32_t at_normal = run_battle(1.0, 1200);
    ASSERT(at_normal == run_battle(0.5, 1200));
    ASSERT(at_normal == run_battle(1.3, 1200));
    ASSERT(at_normal == run_battle(2.0, 1200));
}

TEST(a_battle_run_through_the_level_hashes_the_same) {
    speed_fixture();
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_NORMAL);
    uint32_t at_normal = run_battle(GameSpeed_Multiplier(), 900);
    GameSpeed_SetLevel(GAME_SPEED_LEVEL_MAX);
    ASSERT(at_normal == run_battle(GameSpeed_Multiplier(), 900));
    GameSpeed_SetLevel(3);
    ASSERT(at_normal == run_battle(GameSpeed_Multiplier(), 900));
}

/* -- main ------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    TEST_SUITE("The level");
    RUN(level_starts_normal);
    RUN(one_press_is_one_step);
    RUN(the_top_of_the_range_is_double_speed);
    RUN(the_bottom_of_the_range_is_a_full_stop);
    RUN(a_level_outside_the_range_clamps);
    RUN(a_networked_battle_has_no_speed_control);

    TEST_SUITE("The line on screen");
    RUN(normal_speed_prints_its_own_string);
    RUN(a_faster_speed_prints_a_signed_offset);
    RUN(a_slower_speed_prints_a_negative_offset);
    RUN(the_line_ages_out);
    RUN(setting_the_same_level_again_says_nothing);
    RUN(the_strings_come_from_the_translate_table);

    TEST_SUITE("The machine that cannot keep up");
    RUN(sustained_saturation_walks_the_effective_level_down);
    RUN(recovery_walks_the_effective_level_back_up);
    RUN(recovery_never_passes_the_requested_level);

    TEST_SUITE("The timer");
    RUN(a_tick_is_always_the_same_length);
    RUN(double_speed_runs_twice_the_ticks);
    RUN(a_full_stop_runs_no_ticks);
    RUN(the_catch_up_cap_scales_with_the_speed);
    RUN(double_speed_on_a_slow_machine_really_is_double);

    TEST_SUITE("The keys");
    RUN(the_speed_keys_change_the_speed);
    RUN(a_held_speed_key_steps_once);
    RUN(alt_and_a_speed_key_is_not_a_speed_change);
    RUN(the_speed_keys_do_nothing_in_a_networked_battle);
    RUN(the_keys_walk_the_whole_range_and_stop_at_both_ends);

    TEST_SUITE("Determinism");
    RUN(one_more_tick_is_a_different_hash);
    RUN(the_same_battle_hashes_the_same_at_any_speed);
    RUN(a_battle_run_through_the_level_hashes_the_same);

    TEST_REPORT();
}
