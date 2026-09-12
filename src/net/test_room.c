/*
 * test_room.c -- the battle room's rules, and every refusal.
 *
 * Data free. One case per rule, including the seven departures from the
 * original recorded as N-002 through N-008.
 */

#include "test_framework.h"
#include "tak_net_room.h"
#include "tak_battle_config.h"
#include "tak_map_fingerprint.h"

#include <string.h>

/* Two synthetic maps that share a name and differ in terrain, which is the
 * case a name check cannot catch. Their fingerprints come from the engine's
 * own map fingerprint, so the room is tested against the real rule. */
static const char MAP_TNT_A[] = "synthetic terrain, version A";
static const char MAP_TNT_B[] = "synthetic terrain, version B";
static const char MAP_OTA[] =
    "[GlobalHeader]\n{\nmissionname=Vain Blessings;\nnumplayers=8;\n}\n";
static uint8_t fp_b[TAK_MAP_FINGERPRINT_BYTES];

#define HOST_ID 1001u
#define BUILD   0xabcd0001u
#define DCLASS  TAK_CLASS_TEST

static uint8_t test_fingerprint[TAK_NET_FINGERPRINT_BYTES];

static void make_cfg(TAK_RoomCfg *c, const char *password) {
    memset(c, 0, sizeof(*c));
    strcpy(c->name, "Family game");
    strcpy(c->map_name, "Vain Blessings");
    memcpy(c->map_fingerprint, test_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    if (password) strcpy(c->password, password);
    c->flags = TAK_ROOMF_LISTED;
    c->max_players = TAK_NET_SEATS;
    c->unit_cap = 500;
    c->timeout_secs = 60;
}

static void open_room(TAK_Room *r, const char *password) {
    TAK_RoomCfg c;
    make_cfg(&c, password);
    TAK_Room_Init(r, 7, 12345u, &c, HOST_ID, "Host", BUILD, DCLASS);
}

static int join(TAK_Room *r, uint32_t id, const char *name, uint8_t *seat) {
    return TAK_Room_Join(r, id, name, "", 0, BUILD, DCLASS, seat);
}

/* What a client does on its own after a map change: report its own
 * fingerprint for the room's map. */
static int have_map(TAK_Room *r, uint32_t id, uint8_t seat, const uint8_t *fp) {
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof(e));
    e.field = TAK_EDIT_HAVE_MAP;
    e.seat = seat;
    if (fp) memcpy(e.fingerprint, fp, TAK_NET_FINGERPRINT_BYTES);
    return TAK_Room_Edit(r, id, &e, NULL);
}

static int edit(TAK_Room *r, uint32_t id, uint8_t field, uint8_t seat,
                uint32_t value, const char *text, TAK_RoomEffect *fx) {
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof(e));
    e.field = field;
    e.seat = seat;
    e.value = value;
    if (text) strncpy(e.text, text, TAK_NET_TEXT_MAX - 1);
    if (field == TAK_EDIT_MAP)
        memcpy(e.fingerprint, test_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    return TAK_Room_Edit(r, id, &e, fx);
}

/* ── Codes ──────────────────────────────────────────────────────────── */

TEST(invite_codes_are_six_readable_characters) {
    char a[TAK_NET_CODE_MAX], b[TAK_NET_CODE_MAX], c[TAK_NET_CODE_MAX];
    TAK_Room_MakeCode(1, a);
    TAK_Room_MakeCode(1, b);
    TAK_Room_MakeCode(2, c);
    ASSERT_EQ_INT(TAK_NET_CODE_LEN, (int)strlen(a));
    ASSERT_EQ_STR(a, b);                 /* the same seed gives the same code */
    ASSERT(strcmp(a, c) != 0);           /* the next room does not look alike */
    for (int seed = 0; seed < 500; seed++) {
        char code[TAK_NET_CODE_MAX];
        TAK_Room_MakeCode((uint32_t)seed, code);
        ASSERT_EQ_INT(TAK_NET_CODE_LEN, (int)strlen(code));
        for (int i = 0; i < TAK_NET_CODE_LEN; i++) {
            /* No I, O, 0 or 1: people read these out loud. */
            ASSERT(strchr(TAK_ROOM_CODE_ALPHABET, code[i]) != NULL);
        }
    }
}

/* ── Joining ────────────────────────────────────────────────────────── */

TEST(the_creator_takes_seat_zero_and_holds_the_host_role) {
    TAK_Room r;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, TAK_Room_SeatOf(&r, HOST_ID));
    ASSERT_EQ_INT(HOST_ID, (int)r.host_client_id);
    ASSERT_EQ_INT(1, TAK_Room_HumanCount(&r));
    ASSERT_EQ_INT(TAK_ROOM_OPEN, r.status);
}

TEST(join_takes_the_lowest_free_seat_and_a_free_colour) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(1, seat);
    ASSERT_EQ_INT(0, join(&r, 3, "Dad", &seat));
    ASSERT_EQ_INT(2, seat);
    /* Three seats, three different colours. */
    ASSERT(r.slot[0].colour != r.slot[1].colour);
    ASSERT(r.slot[1].colour != r.slot[2].colour);
    ASSERT(r.slot[0].colour != r.slot[2].colour);
}

TEST(join_refuses_a_wrong_password_and_a_second_join) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, "hunter2");
    ASSERT_EQ_INT(TAK_REJECT_WRONG_PASSWORD,
                  TAK_Room_Join(&r, 2, "Brother", "hunter3", 0, BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(TAK_REJECT_WRONG_PASSWORD,
                  TAK_Room_Join(&r, 2, "Brother", "", 0, BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(0,
                  TAK_Room_Join(&r, 2, "Brother", "hunter2", 0, BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(1, seat);
    /* The same client cannot sit twice. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  TAK_Room_Join(&r, 2, "Brother", "hunter2", 0, BUILD, DCLASS, &seat));
    ASSERT(r.cfg.flags & TAK_ROOMF_PASSWORD);
}

TEST(join_refuses_a_nameless_player_and_a_full_room) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(TAK_REJECT_NAME_REQUIRED, join(&r, 2, "", &seat));
    for (uint32_t i = 2; i <= TAK_NET_SEATS; i++)
        ASSERT_EQ_INT(0, join(&r, i, "Player", &seat));
    ASSERT_EQ_INT(TAK_NET_SEATS, TAK_Room_HumanCount(&r));
    ASSERT_EQ_INT(TAK_REJECT_GAME_FULL, join(&r, 99, "Late", &seat));
}

TEST(join_refuses_a_mismatched_build_or_determinism_class) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(TAK_REJECT_NEEDS_NEWER,
                  TAK_Room_Join(&r, 2, "Brother", "", 0, BUILD + 1, DCLASS, &seat));
    ASSERT_EQ_INT(TAK_REJECT_DETERMINISM_CLASS,
                  TAK_Room_Join(&r, 2, "Brother", "", 0, BUILD,
                                TAK_CLASS_NATIVE, &seat));
}

TEST(join_refuses_a_room_that_is_no_longer_open) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, test_fingerprint));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_Room_Start(&r, HOST_ID));
    ASSERT_EQ_INT(TAK_ROOM_LOADING, r.status);
    ASSERT_EQ_INT(TAK_REJECT_GAME_CLOSED, join(&r, 3, "Dad", &seat));
}

/* ── N-008: watchers sit outside the eight seats ────────────────────── */

TEST(a_watcher_does_not_eat_a_seat) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(TAK_REJECT_NO_WATCHING,
                  TAK_Room_Join(&r, 2, "Watcher", "", 1, BUILD, DCLASS, &seat));
    r.cfg.flags |= TAK_ROOMF_ALLOW_WATCHING;
    ASSERT_EQ_INT(0,
                  TAK_Room_Join(&r, 2, "Watcher", "", 1, BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, seat);
    ASSERT_EQ_INT(1, TAK_Room_IsWatcher(&r, 2));
    ASSERT_EQ_INT(1, (int)r.watcher_count);
    ASSERT_EQ_INT(1, TAK_Room_HumanCount(&r));   /* still only the host seated */

    /* All eight seats still available with a watcher present. */
    for (uint32_t i = 10; i < 10u + (TAK_NET_SEATS - 1); i++)
        ASSERT_EQ_INT(0, join(&r, i, "Player", &seat));
    ASSERT_EQ_INT(TAK_NET_SEATS, TAK_Room_HumanCount(&r));
}

TEST(watchers_have_their_own_limit) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    r.cfg.flags |= TAK_ROOMF_ALLOW_WATCHING;
    for (uint32_t i = 0; i < TAK_NET_WATCHERS_MAX; i++)
        ASSERT_EQ_INT(0, TAK_Room_Join(&r, 100u + i, "W", "", 1,
                                       BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(TAK_REJECT_GAME_FULL,
                  TAK_Room_Join(&r, 999, "W", "", 1, BUILD, DCLASS, &seat));
}

/* ── Rights and ready ───────────────────────────────────────────────── */

TEST(a_player_edits_only_their_own_row) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    /* Seat 1 cannot touch seat 0. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, 2, TAK_EDIT_SIDE, 0, TAK_SIDE_ZHON, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_SIDE, 1, TAK_SIDE_ZHON, NULL, NULL));
    ASSERT_EQ_INT(TAK_SIDE_ZHON, r.slot[1].side);
    /* And a non host cannot touch the room's own settings. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, 2, TAK_EDIT_MAP, TAK_NET_SEAT_NONE, 0, "Other", NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, 2, TAK_EDIT_KICK, 0, 0, NULL, NULL));
}

TEST(ready_toggles_and_any_edit_to_your_row_clears_it) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(1, r.slot[1].ready);
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, r.slot[1].ready);           /* it toggles, it does not latch */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(1, r.slot[1].ready);
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_TEAM, 1, 2, NULL, NULL));
    ASSERT_EQ_INT(0, r.slot[1].ready);           /* an edit to your own row clears it */
}

/* ── N-007: a host change clears everyone's ready ───────────────────── */

TEST(a_map_or_option_change_clears_everyones_ready) {
    TAK_Room r;
    TAK_RoomEffect fx;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, join(&r, 3, "Dad", &seat));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 3, TAK_EDIT_READY, 2, 0, NULL, NULL));
    ASSERT_EQ_INT(1, r.slot[0].ready);
    ASSERT_EQ_INT(1, r.slot[2].ready);

    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_MAP, TAK_NET_SEAT_NONE,
                          0, "Bleak Hollow", &fx));
    ASSERT_EQ_INT(1, fx.ready_cleared);
    for (int i = 0; i < 3; i++) ASSERT_EQ_INT(0, r.slot[i].ready);
    ASSERT_EQ_STR("Bleak Hollow", r.cfg.map_name);

    /* An option and the unit cap do the same. */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_OPTIONS, TAK_NET_SEAT_NONE,
                          0x1fu, NULL, &fx));
    ASSERT_EQ_INT(1, fx.ready_cleared);
    ASSERT_EQ_INT(0, r.slot[1].ready);

    /* The drop timeout and Allow Watching do not change how a battle
     * plays, so they leave ready alone. */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_TIMEOUT, TAK_NET_SEAT_NONE,
                          120, NULL, &fx));
    ASSERT_EQ_INT(0, fx.ready_cleared);
    ASSERT_EQ_INT(1, r.slot[1].ready);
    ASSERT_EQ_INT(120, (int)r.cfg.timeout_secs);
}

TEST(out_of_range_values_are_refused_and_change_nothing) {
    TAK_Room r;
    open_room(&r, NULL);
    uint32_t rev = r.revision;
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_SIDE, 0, TAK_ROOM_SIDES, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_TEAM, 0, TAK_ROOM_TEAMS + 1, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_COLOUR, 0, TAK_ROOM_COLOURS, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_TIMEOUT, TAK_NET_SEAT_NONE,
                       TAK_ROOM_TIMEOUT_MAX + 1, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_UNIT_CAP, TAK_NET_SEAT_NONE,
                       TAK_UNITS_PER_SIDE_MAX + 1, NULL, NULL));
    /* A refused edit does not move the revision, so clients do not resync
     * over something that never happened. */
    ASSERT_EQ_INT((int)rev, (int)r.revision);
}

TEST(a_colour_request_comes_back_as_the_next_free_one) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    uint8_t host_colour = r.slot[0].colour;
    /* Asking for a colour someone else holds gives the next free one. */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_COLOUR, 1, host_colour, NULL, NULL));
    ASSERT(r.slot[1].colour != host_colour);
}

/* ── N-003: a computer player belongs to the room ───────────────────── */

TEST(anyone_may_add_a_computer_player_and_it_outlives_its_adder) {
    TAK_Room r;
    TAK_RoomLeave lv;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    /* A player who is not the host may fill an empty seat. */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_ADD_COMPUTER, 4, 0, "AI Brother", NULL));
    ASSERT_EQ_INT(TAK_NSLOT_COMPUTER, r.slot[4].kind);
    ASSERT_EQ_INT(0, (int)r.slot[4].client_id);   /* owned by nobody */
    ASSERT_EQ_INT(1, r.slot[4].ready);            /* always ready */
    /* The adder leaves. The computer player stays. */
    TAK_Room_Leave(&r, 2, &lv);
    ASSERT_EQ_INT(1, lv.seat);
    ASSERT_EQ_INT(TAK_NSLOT_COMPUTER, r.slot[4].kind);
    /* Only the host removes one. */
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_REMOVE_COMPUTER, 4, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_NSLOT_EMPTY, r.slot[4].kind);
}

TEST(a_blocked_seat_takes_no_player_until_it_is_opened) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_BLOCK_SLOT, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_NSLOT_BLOCKED, r.slot[1].kind);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(2, seat);                     /* skipped the blocked seat */
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_UNBLOCK_SLOT, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, join(&r, 3, "Dad", &seat));
    ASSERT_EQ_INT(1, seat);
}

TEST(the_host_removes_a_player_but_never_itself) {
    TAK_Room r;
    TAK_RoomEffect fx;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_KICK, 0, 0, NULL, &fx));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_KICK, 1, 0, NULL, &fx));
    ASSERT_EQ_INT(2, (int)fx.removed_client_id);
}

/* ── N-002: the host role moves ─────────────────────────────────────── */

TEST(the_host_role_moves_to_the_lowest_human_seat) {
    TAK_Room r;
    TAK_RoomLeave lv;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, join(&r, 3, "Dad", &seat));
    TAK_Room_Leave(&r, HOST_ID, &lv);
    ASSERT_EQ_INT(1, lv.was_host);
    ASSERT_EQ_INT(0, lv.room_finished);
    ASSERT_EQ_INT(2, (int)lv.new_host_client_id);   /* seat 1, the lowest */
    ASSERT_EQ_INT(2, (int)r.host_client_id);
    ASSERT_EQ_INT(TAK_NSLOT_EMPTY, r.slot[0].kind);
    /* The new host has the host's rights at once. */
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_OPTIONS, TAK_NET_SEAT_NONE, 3, NULL, NULL));
}

TEST(a_room_with_no_human_left_is_finished) {
    TAK_Room r;
    TAK_RoomLeave lv;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_ADD_COMPUTER, 3, 0, "AI", NULL));
    TAK_Room_Leave(&r, HOST_ID, &lv);
    ASSERT_EQ_INT(1, lv.was_host);
    ASSERT_EQ_INT(1, lv.room_finished);
    ASSERT_EQ_INT(0, (int)lv.new_host_client_id);
}

TEST(a_host_that_starts_watching_hands_the_room_on) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);
    r.cfg.flags |= TAK_ROOMF_ALLOW_WATCHING;
    /* Alone, the host may not leave its own seat. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED,
                  edit(&r, HOST_ID, TAK_EDIT_WATCH, 0, 1, NULL, NULL));
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_WATCH, 0, 1, NULL, NULL));
    ASSERT_EQ_INT(1, TAK_Room_IsWatcher(&r, HOST_ID));
    ASSERT_EQ_INT(2, (int)r.host_client_id);
    ASSERT_EQ_INT(TAK_NSLOT_EMPTY, r.slot[0].kind);
}

/* ── The start gate ─────────────────────────────────────────────────── */

TEST(start_needs_the_host_another_human_a_map_and_everyone_ready) {
    TAK_Room r;
    uint8_t seat = 0;
    open_room(&r, NULL);

    /* Only the host asks. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, TAK_Room_CanStart(&r, 999));
    /* One human against computer players is what skirmish is for. */
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_ADD_COMPUTER, 3, 0, "AI", NULL));
    ASSERT_EQ_INT(TAK_REJECT_NEEDS_HUMAN, TAK_Room_CanStart(&r, HOST_ID));

    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, test_fingerprint));
    /* Nobody is ready yet. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_READY, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_NOT_READY, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_Room_CanStart(&r, HOST_ID));

    /* Everyone on one team is not a game. */
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_TEAM, 0, 1, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_TEAM, 1, 1, NULL, NULL));
    r.slot[3].team = 1;
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_ONE_TEAM, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_TEAM, 1, 2, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_Room_CanStart(&r, HOST_ID));
}

TEST(start_needs_a_map_fingerprint_not_just_a_name) {
    TAK_Room r;
    TAK_MsgRoomEdit e;
    uint8_t seat = 0;
    open_room(&r, NULL);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, test_fingerprint));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(0, TAK_Room_CanStart(&r, HOST_ID));

    /* A map name with no fingerprint behind it cannot start. */
    memset(&e, 0, sizeof(e));
    e.field = TAK_EDIT_MAP;
    e.seat = TAK_NET_SEAT_NONE;
    strcpy(e.text, "Nameless");
    ASSERT_EQ_INT(0, TAK_Room_Edit(&r, HOST_ID, &e, NULL));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_MAP_MISSING, TAK_Room_CanStart(&r, HOST_ID));
}

TEST(every_human_must_hold_the_same_map_by_fingerprint) {
    TAK_Room r;
    TAK_RoomEffect fx;
    uint8_t seat = 0;
    /* The two synthetic maps share a name, so a name check passes them. */
    ASSERT(memcmp(test_fingerprint, fp_b, TAK_NET_FINGERPRINT_BYTES) != 0);

    open_room(&r, NULL);
    ASSERT(r.slot[0].flags & TAK_SLOTF_HAS_MAP);     /* the host computed it */
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));

    /* Not reported yet, then reported without the map, then with a
     * different map under the same name: all refused. */
    ASSERT_EQ_INT(TAK_REJECT_MAP_MISSING, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, NULL));
    ASSERT_EQ_INT(TAK_REJECT_MAP_MISSING, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, fp_b));
    ASSERT_EQ_INT(TAK_REJECT_MAP_MISSING, TAK_Room_CanStart(&r, HOST_ID));

    /* Reporting the map is automatic, so it leaves ready alone. */
    ASSERT_EQ_INT(1, r.slot[1].ready);
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, test_fingerprint));
    ASSERT(r.slot[1].flags & TAK_SLOTF_HAS_MAP);
    ASSERT_EQ_INT(0, TAK_Room_CanStart(&r, HOST_ID));

    /* A new map resets every report except the host's. */
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof(e));
    e.field = TAK_EDIT_MAP;
    e.seat = TAK_NET_SEAT_NONE;
    strcpy(e.text, "Vain Blessings");
    memcpy(e.fingerprint, fp_b, TAK_NET_FINGERPRINT_BYTES);
    ASSERT_EQ_INT(0, TAK_Room_Edit(&r, HOST_ID, &e, &fx));
    ASSERT(r.slot[0].flags & TAK_SLOTF_HAS_MAP);
    ASSERT(!(r.slot[1].flags & TAK_SLOTF_HAS_MAP));
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL));
    ASSERT_EQ_INT(0, edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL));
    ASSERT_EQ_INT(TAK_REJECT_MAP_MISSING, TAK_Room_CanStart(&r, HOST_ID));
    ASSERT_EQ_INT(0, have_map(&r, 2, 1, fp_b));
    ASSERT_EQ_INT(0, TAK_Room_CanStart(&r, HOST_ID));

    /* Nobody reports a map for somebody else's seat. */
    ASSERT_EQ_INT(TAK_REJECT_NOT_ALLOWED, have_map(&r, 2, 0, fp_b));
}

/* ── Loading and the match ──────────────────────────────────────────── */

static void start_two(TAK_Room *r) {
    uint8_t seat = 0;
    open_room(r, NULL);
    r->cfg.flags |= TAK_ROOMF_ALLOW_WATCHING;
    join(r, 2, "Brother", &seat);
    join(r, 3, "Dad", &seat);
    have_map(r, 2, 1, test_fingerprint);
    have_map(r, 3, 2, test_fingerprint);
    edit(r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL);
    edit(r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL);
    edit(r, 3, TAK_EDIT_READY, 2, 0, NULL, NULL);
}

TEST(a_watcher_may_join_a_match_under_way_but_a_player_may_not) {
    TAK_Room r;
    uint8_t seat = 0;
    start_two(&r);
    ASSERT_EQ_INT(0, TAK_Room_Start(&r, HOST_ID));
    /* Nobody joins while the worlds are loading. */
    ASSERT_EQ_INT(TAK_REJECT_GAME_CLOSED,
                  TAK_Room_Join(&r, 9, "W", "", 1, BUILD, DCLASS, &seat));
    TAK_Room_Go(&r);
    ASSERT_EQ_INT(TAK_ROOM_IN_PROGRESS, r.status);
    ASSERT_EQ_INT(TAK_REJECT_GAME_CLOSED, join(&r, 8, "Late", &seat));
    ASSERT_EQ_INT(0, TAK_Room_Join(&r, 9, "W", "", 1, BUILD, DCLASS, &seat));
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, seat);
    ASSERT_EQ_INT(1, TAK_Room_IsWatcher(&r, 9));
}

TEST(a_host_who_drops_in_game_hands_the_role_on_and_keeps_the_seat) {
    TAK_Room r;
    start_two(&r);
    ASSERT_EQ_INT(0, TAK_Room_Start(&r, HOST_ID));
    TAK_Room_Go(&r);
    ASSERT_EQ_INT(2, (int)TAK_Room_SetConnected(&r, HOST_ID, 0));
    ASSERT_EQ_INT(2, (int)r.host_client_id);
    ASSERT_EQ_INT(TAK_NSLOT_HUMAN, r.slot[0].kind);   /* the army is still there */
    ASSERT_EQ_INT(0, r.slot[0].connected);
    /* The old host coming back does not take the role back. */
    ASSERT_EQ_INT(2, (int)TAK_Room_SetConnected(&r, HOST_ID, 1));
}

TEST(an_aborted_start_goes_back_to_the_lobby_unready) {
    TAK_Room r;
    start_two(&r);
    ASSERT_EQ_INT(0, edit(&r, HOST_ID, TAK_EDIT_ADD_COMPUTER, 5, 0, "AI", NULL));
    edit(&r, HOST_ID, TAK_EDIT_READY, 0, 0, NULL, NULL);
    edit(&r, 2, TAK_EDIT_READY, 1, 0, NULL, NULL);
    edit(&r, 3, TAK_EDIT_READY, 2, 0, NULL, NULL);
    ASSERT_EQ_INT(0, TAK_Room_Start(&r, HOST_ID));
    TAK_Room_Abort(&r);
    ASSERT_EQ_INT(TAK_ROOM_OPEN, r.status);
    for (int i = 0; i < 3; i++) ASSERT_EQ_INT(0, r.slot[i].ready);
    ASSERT_EQ_INT(1, r.slot[5].ready);                /* a computer stays ready */
}

/* ── Views ──────────────────────────────────────────────────────────── */

TEST(the_snapshot_carries_every_seat_and_a_rising_revision) {
    TAK_Room r;
    TAK_MsgRoomState s1, s2;
    uint8_t seat = 0;
    open_room(&r, NULL);
    TAK_Room_Snapshot(&r, &s1);
    ASSERT_EQ_INT(TAK_NET_SEATS, s1.seat_count);
    ASSERT_EQ_INT(7, (int)s1.room_id);
    ASSERT_EQ_STR(r.code, s1.code);
    ASSERT_EQ_INT(0, join(&r, 2, "Brother", &seat));
    TAK_Room_Snapshot(&r, &s2);
    ASSERT(s2.revision > s1.revision);
    ASSERT_EQ_STR("Brother", s2.slot[1].name);
}

TEST(an_unjoinable_room_is_listed_with_its_reason) {
    TAK_Room r;
    TAK_RoomSummary sum;
    uint8_t seat = 0;
    open_room(&r, NULL);
    TAK_Room_Summary(&r, BUILD, DCLASS, &sum);
    ASSERT_EQ_INT(0, sum.compat);
    ASSERT_EQ_STR("Host", sum.host_name);
    ASSERT_EQ_INT(1, sum.players);

    TAK_Room_Summary(&r, BUILD + 1, DCLASS, &sum);
    ASSERT_EQ_INT(TAK_REJECT_NEEDS_NEWER, sum.compat);
    TAK_Room_Summary(&r, BUILD, TAK_CLASS_NATIVE, &sum);
    ASSERT_EQ_INT(TAK_REJECT_DETERMINISM_CLASS, sum.compat);

    for (uint32_t i = 2; i <= TAK_NET_SEATS; i++)
        ASSERT_EQ_INT(0, join(&r, i, "Player", &seat));
    TAK_Room_Summary(&r, BUILD, DCLASS, &sum);
    ASSERT_EQ_INT(TAK_REJECT_GAME_FULL, sum.compat);
}

int main(void) {
    TAK_MapFiles files;
    memset(&files, 0, sizeof(files));
    files.ota = MAP_OTA; files.ota_size = sizeof(MAP_OTA) - 1;
    files.tnt = MAP_TNT_A; files.tnt_size = sizeof(MAP_TNT_A) - 1;
    if (TAK_MapFingerprint_FromFiles(&files, test_fingerprint) != 0) return 2;
    files.tnt = MAP_TNT_B; files.tnt_size = sizeof(MAP_TNT_B) - 1;
    if (TAK_MapFingerprint_FromFiles(&files, fp_b) != 0) return 2;

    TEST_SUITE("Battle room state machine");
    RUN(invite_codes_are_six_readable_characters);
    RUN(the_creator_takes_seat_zero_and_holds_the_host_role);
    RUN(join_takes_the_lowest_free_seat_and_a_free_colour);
    RUN(join_refuses_a_wrong_password_and_a_second_join);
    RUN(join_refuses_a_nameless_player_and_a_full_room);
    RUN(join_refuses_a_mismatched_build_or_determinism_class);
    RUN(join_refuses_a_room_that_is_no_longer_open);
    RUN(a_watcher_does_not_eat_a_seat);
    RUN(watchers_have_their_own_limit);
    RUN(a_player_edits_only_their_own_row);
    RUN(ready_toggles_and_any_edit_to_your_row_clears_it);
    RUN(a_map_or_option_change_clears_everyones_ready);
    RUN(out_of_range_values_are_refused_and_change_nothing);
    RUN(a_colour_request_comes_back_as_the_next_free_one);
    RUN(anyone_may_add_a_computer_player_and_it_outlives_its_adder);
    RUN(a_blocked_seat_takes_no_player_until_it_is_opened);
    RUN(the_host_removes_a_player_but_never_itself);
    RUN(the_host_role_moves_to_the_lowest_human_seat);
    RUN(a_room_with_no_human_left_is_finished);
    RUN(a_host_that_starts_watching_hands_the_room_on);
    RUN(start_needs_the_host_another_human_a_map_and_everyone_ready);
    RUN(start_needs_a_map_fingerprint_not_just_a_name);
    RUN(every_human_must_hold_the_same_map_by_fingerprint);
    RUN(a_watcher_may_join_a_match_under_way_but_a_player_may_not);
    RUN(a_host_who_drops_in_game_hands_the_role_on_and_keeps_the_seat);
    RUN(an_aborted_start_goes_back_to_the_lobby_unready);
    RUN(the_snapshot_carries_every_seat_and_a_rising_revision);
    RUN(an_unjoinable_room_is_listed_with_its_reason);
    TEST_REPORT();
}
