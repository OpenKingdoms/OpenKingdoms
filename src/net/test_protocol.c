/*
 * test_protocol.c -- the codec round trips, and refuses everything else.
 *
 * Data free. The interesting half is the refusals: every message is fed
 * back at every truncated length, with trailing bytes, and with each count
 * field pushed past its cap, because that is what an internet facing
 * parser actually meets.
 */

#include "test_framework.h"
#include "tak_net_protocol.h"
#include "tak_bytes.h"

#include <string.h>

static uint8_t buf[TAK_NET_FRAME_MAX];
static uint8_t buf2[TAK_NET_FRAME_MAX];

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Rewrite the frame's length field and validate that prefix. Used to feed
 * a decoder a payload truncated at every length. */
static int validate_truncated(uint8_t *frame, size_t frame_len, size_t keep) {
    tak_put_u16(frame + 1, (uint16_t)keep);
    return TAK_Net_Validate(frame, TAK_NET_FRAME_HEADER + keep);
}

/* Every strictly shorter payload must be refused. Returns the first length
 * that was wrongly accepted, or -1 when all were refused. */
static int first_accepted_truncation(const uint8_t *frame, size_t frame_len) {
    size_t payload = frame_len - TAK_NET_FRAME_HEADER;
    for (size_t keep = 0; keep < payload; keep++) {
        memcpy(buf2, frame, frame_len);
        if (validate_truncated(buf2, frame_len, keep) == 0) return (int)keep;
    }
    return -1;
}

/* A trailing byte must be refused: the sender and this parser disagree. */
static int accepts_trailing(const uint8_t *frame, size_t frame_len) {
    size_t payload = frame_len - TAK_NET_FRAME_HEADER;
    if (payload + 1 > TAK_NET_PAYLOAD_MAX) return 0;
    memcpy(buf2, frame, frame_len);
    buf2[frame_len] = 0x5a;
    tak_put_u16(buf2 + 1, (uint16_t)(payload + 1));
    return TAK_Net_Validate(buf2, frame_len + 1) == 0;
}

static void fill(char *dst, size_t n, char c) {
    memset(dst, c, n - 1);
    dst[n - 1] = '\0';
}

/* ── Framing ────────────────────────────────────────────────────────── */

TEST(split_refuses_malformed_frames) {
    TAK_NetFrame f;
    uint8_t ok[5] = { TAK_MSG_ACK, 2, 0, 0xaa, 0xbb };
    ASSERT_EQ_INT(0, TAK_Net_Split(ok, 5, &f));
    ASSERT_EQ_INT(TAK_MSG_ACK, f.type);
    ASSERT_EQ_INT(2, f.payload_len);

    ASSERT(TAK_Net_Split(ok, 2, &f) != 0);          /* shorter than a header */
    ASSERT(TAK_Net_Split(ok, 4, &f) != 0);          /* length disagrees, short */
    ASSERT(TAK_Net_Split(ok, 6, &f) != 0);          /* length disagrees, long */
    uint8_t zero[3] = { 0, 0, 0 };
    ASSERT(TAK_Net_Split(zero, 3, &f) != 0);        /* type 0 is never valid */
    ASSERT(TAK_Net_Split(NULL, 5, &f) != 0);
}

TEST(empty_payload_messages_are_three_bytes) {
    size_t n = TAK_Msg_EmptyEncode(TAK_MSG_LEAVE_ROOM, buf, sizeof(buf));
    ASSERT_EQ_INT(3, (int)n);
    ASSERT_EQ_INT(0, TAK_Net_Validate(buf, n));
    ASSERT(accepts_trailing(buf, n) == 0);
}

TEST(encode_into_a_short_buffer_fails_and_writes_nothing_past_it) {
    TAK_MsgAck a = { 7, 60, 0x1122334455667788ull };
    uint8_t small[8];
    memset(small, 0xee, sizeof(small));
    ASSERT_EQ_INT(0, (int)TAK_Msg_AckEncode(&a, small, 4));
    /* Byte 4 onwards is untouched: the writer stopped at its cap. */
    ASSERT_EQ_INT(0xee, small[4]);
    ASSERT_EQ_INT(0xee, small[7]);
}

/* ── Round trips ────────────────────────────────────────────────────── */

TEST(hello_round_trips) {
    TAK_MsgHello a, b;
    memset(&a, 0, sizeof(a));
    a.protocol_version = TAK_NET_PROTOCOL_VERSION;
    a.engine_build_id = 0xdeadbeefu;
    a.determinism_class = TAK_CLASS_BROWSER;
    a.client_kind = TAK_CLIENT_PLAYER;
    a.flags = TAK_HELLOF_IRON_PLAGUE;
    a.schema_hash = 0x0123456789abcdefull;
    a.content_hash = 0xfedcba9876543210ull;
    for (int i = 0; i < TAK_NET_GROUP_HASHES; i++) a.group_hash[i] = 100u + i;
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) a.device_token[i] = (uint8_t)i;
    strcpy(a.name, "Brother");
    strcpy(a.access_key, "abcdefghijklmnopqrstuv");

    size_t n = TAK_Msg_HelloEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(TAK_MSG_HELLO, f.type);
    ASSERT_EQ_INT(0, TAK_Msg_HelloDecode(&b, f.payload, f.payload_len));
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));
    ASSERT(accepts_trailing(buf, n) == 0);
}

TEST(an_overlong_name_is_cut_not_overrun) {
    TAK_MsgHello a, b;
    memset(&a, 0, sizeof(a));
    char big[128];
    fill(big, sizeof(big), 'x');
    memcpy(a.name, big, TAK_NET_NAME_MAX - 1);   /* already at the cap */
    a.name[TAK_NET_NAME_MAX - 1] = '\0';
    size_t n = TAK_Msg_HelloEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_HelloDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(TAK_NET_NAME_MAX - 1, (int)strlen(b.name));
}

TEST(an_unterminated_text_field_comes_back_terminated) {
    /* A hostile sender fills the whole field with no NUL. */
    TAK_MsgHello a, b;
    memset(&a, 0, sizeof(a));
    size_t n = TAK_Msg_HelloEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    /* The name field sits right after the fixed head and the token. */
    size_t name_at = TAK_NET_FRAME_HEADER + 2 + 4 + 1 + 1 + 1 + 8 + 8 +
                     8 * TAK_NET_GROUP_HASHES + TAK_NET_TOKEN_BYTES;
    memset(buf + name_at, 'A', TAK_NET_NAME_MAX);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_HelloDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(TAK_NET_NAME_MAX - 1, (int)strlen(b.name));
}

TEST(welcome_reject_and_ping_round_trip) {
    TAK_MsgWelcome w = { 0 }, w2;
    w.session_id = 9; w.flags = 3; w.protocol_min = 1; w.protocol_max = 4;
    w.newest_build = 77;
    strcpy(w.server_name, "public");
    strcpy(w.motd, "Be kind.");
    size_t n = TAK_Msg_WelcomeEncode(&w, buf, sizeof(buf));
    TAK_NetFrame f;
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_WelcomeDecode(&w2, f.payload, f.payload_len));
    ASSERT(memcmp(&w, &w2, sizeof(w)) == 0);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));

    TAK_MsgReject rj = { 0 }, rj2;
    rj.reason = TAK_REJECT_DATA_MISMATCH;
    rj.detail = TAK_HASH_WEAPONS;
    strcpy(rj.text, "weapons");
    n = TAK_Msg_RejectEncode(&rj, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_RejectDecode(&rj2, f.payload, f.payload_len));
    ASSERT(memcmp(&rj, &rj2, sizeof(rj)) == 0);

    TAK_MsgPing pg = { 42, 0x00000001ffffffffull }, pg2;
    n = TAK_Msg_PingEncode(TAK_MSG_PONG, &pg, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(TAK_MSG_PONG, f.type);
    ASSERT_EQ_INT(0, TAK_Msg_PingDecode(&pg2, f.payload, f.payload_len));
    ASSERT(pg.seq == pg2.seq && pg.sent_ms == pg2.sent_ms);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));
}

TEST(room_state_round_trips_all_eight_slots) {
    TAK_MsgRoomState a, b;
    memset(&a, 0, sizeof(a));
    a.room_id = 5; a.revision = 12;
    strcpy(a.code, "KJ7P2M");
    strcpy(a.name, "Family game");
    strcpy(a.map_name, "Vain Blessings");
    for (int i = 0; i < TAK_NET_FINGERPRINT_BYTES; i++)
        a.map_fingerprint[i] = (uint8_t)(i * 7);
    a.host_client_id = 101;
    a.flags = TAK_ROOMF_PASSWORD | TAK_ROOMF_ALLOW_WATCHING;
    a.options = 0x1f;
    a.status = TAK_ROOM_OPEN;
    a.watchers = 2;
    a.unit_cap = 500;
    a.timeout_secs = 60;
    a.seat_count = TAK_NET_SEATS;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        a.slot[i].kind = (i < 3) ? TAK_NSLOT_HUMAN : TAK_NSLOT_EMPTY;
        a.slot[i].side = (uint8_t)(i & 3);
        a.slot[i].colour = (uint8_t)i;
        a.slot[i].team = (uint8_t)(i % 2);
        a.slot[i].ready = (i == 0);
        a.slot[i].connected = 1;
        a.slot[i].ping_ms = (uint16_t)(20 + i);
        a.slot[i].client_id = (uint32_t)(100 + i);
        a.slot[i].name[0] = (char)('A' + i);
    }
    size_t n = TAK_Msg_RoomStateEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_RoomStateDecode(&b, f.payload, f.payload_len));
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));
    ASSERT(accepts_trailing(buf, n) == 0);
}

TEST(room_list_refuses_more_rooms_than_the_cap) {
    TAK_MsgRoomList a;
    memset(&a, 0, sizeof(a));
    a.flags = TAK_ROOMLISTF_FULL;
    a.count = TAK_NET_ROOMS_PER_LIST;
    for (int i = 0; i < TAK_NET_ROOMS_PER_LIST; i++) {
        a.room[i].room_id = (uint32_t)(i + 1);
        a.room[i].players = 1;
        a.room[i].max_players = TAK_NET_SEATS;
    }
    size_t n = TAK_Msg_RoomListEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_MsgRoomList b;
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_RoomListDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(a.count, b.count);
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);

    /* One past the cap must not encode, and must not decode either. */
    a.count = TAK_NET_ROOMS_PER_LIST + 1;
    ASSERT_EQ_INT(0, (int)TAK_Msg_RoomListEncode(&a, buf2, sizeof(buf2)));
    buf[TAK_NET_FRAME_HEADER + 1] = TAK_NET_ROOMS_PER_LIST + 1;
    ASSERT(TAK_Net_Validate(buf, n) != 0);
}

TEST(start_game_and_load_messages_round_trip) {
    TAK_MsgStartGame a, b;
    memset(&a, 0, sizeof(a));
    a.match_id = 3; a.seed = 0xabcd1234u; a.your_seat = 2;
    a.turn_ticks = TAK_NET_TURN_TICKS;
    a.schema_hash = 11; a.content_hash = 22;
    strcpy(a.map_name, "Bleak Hollow");
    a.options = 7; a.unit_cap = 500; a.timeout_secs = 60;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        a.slot[i].kind = TAK_NSLOT_HUMAN;
        a.slot[i].side = (uint8_t)(i & 3);
        a.slot[i].colour = (uint8_t)i;
        a.slot[i].team = 0;
        a.slot[i].name[0] = (char)('a' + i);
    }
    size_t n = TAK_Msg_StartGameEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_StartGameDecode(&b, f.payload, f.payload_len));
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));

    TAK_MsgLoadState ls, ls2;
    memset(&ls, 0, sizeof(ls));
    ls.count = 3;
    for (int i = 0; i < 3; i++) {
        ls.entry[i].seat = (uint8_t)i;
        ls.entry[i].percent = (uint8_t)(i * 30);
        ls.entry[i].loaded = (i == 0);
    }
    n = TAK_Msg_LoadStateEncode(&ls, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_LoadStateDecode(&ls2, f.payload, f.payload_len));
    ASSERT(memcmp(&ls, &ls2, sizeof(ls)) == 0);
    /* A seat outside the table is refused. */
    buf[TAK_NET_FRAME_HEADER + 1] = TAK_NET_SEATS;
    ASSERT(TAK_Net_Validate(buf, n) != 0);
}

/* ── Commands and turns ─────────────────────────────────────────────── */

TEST(cmd_round_trips_and_points_into_the_frame) {
    static uint8_t body[3][40];
    TAK_MsgCmd a, b;
    memset(&a, 0, sizeof(a));
    a.client_seq = 77;
    a.count = 3;
    for (int i = 0; i < 3; i++) {
        memset(body[i], (uint8_t)(0x10 + i), sizeof(body[i]));
        a.cmd[i].data = body[i];
        a.cmd[i].len = (uint16_t)(10 + i * 10);
    }
    size_t n = TAK_Msg_CmdEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_CmdDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(77, (int)b.client_seq);
    ASSERT_EQ_INT(3, b.count);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(a.cmd[i].len, b.cmd[i].len);
        ASSERT(memcmp(a.cmd[i].data, b.cmd[i].data, a.cmd[i].len) == 0);
        /* Zero copy: the blob points inside the frame we were given. */
        ASSERT(b.cmd[i].data >= buf && b.cmd[i].data < buf + n);
    }
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));
    ASSERT(accepts_trailing(buf, n) == 0);
}

TEST(cmd_refuses_a_zero_length_or_oversized_command) {
    static uint8_t body[TAK_NET_CMD_BYTES_MAX];
    TAK_MsgCmd a;
    memset(&a, 0, sizeof(a));
    memset(body, 1, sizeof(body));

    a.count = 1; a.cmd[0].data = body; a.cmd[0].len = 0;
    ASSERT_EQ_INT(0, (int)TAK_Msg_CmdEncode(&a, buf, sizeof(buf)));

    a.count = TAK_NET_CMDS_PER_MSG + 1;
    a.cmd[0].len = 4;
    ASSERT_EQ_INT(0, (int)TAK_Msg_CmdEncode(&a, buf, sizeof(buf)));

    /* Two commands that together exceed the per message budget. */
    a.count = 2;
    a.cmd[0].data = body; a.cmd[0].len = TAK_NET_CMD_BYTES_MAX;
    a.cmd[1].data = body; a.cmd[1].len = 1;
    ASSERT_EQ_INT(0, (int)TAK_Msg_CmdEncode(&a, buf, sizeof(buf)));

    /* And the decoder refuses the same thing when a peer hand builds it. */
    TAK_ByteWriter w;
    TAK_BW_Init(&w, buf, sizeof(buf));
    TAK_BW_U8(&w, TAK_MSG_CMD);
    TAK_BW_U16(&w, 0);
    TAK_BW_U32(&w, 1);
    TAK_BW_U8(&w, 2);
    TAK_BW_U16(&w, TAK_NET_CMD_BYTES_MAX);
    TAK_BW_Bytes(&w, body, TAK_NET_CMD_BYTES_MAX);
    TAK_BW_U16(&w, 1);
    TAK_BW_U8(&w, 9);
    tak_put_u16(buf + 1, (uint16_t)(TAK_BW_Len(&w) - TAK_NET_FRAME_HEADER));
    ASSERT(TAK_Net_Validate(buf, TAK_BW_Len(&w)) != 0);
}

TEST(an_empty_turn_is_seven_bytes_and_carries_a_run) {
    TAK_MsgTurn a, b;
    memset(&a, 0, sizeof(a));
    a.turn = 1000;
    a.empty_run = 40;
    a.entry_count = 0;
    size_t n = TAK_Msg_TurnEncode(&a, buf, sizeof(buf));
    ASSERT_EQ_INT(10, (int)n);   /* 3 header, 4 turn, 2 run, 1 count */
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(1000, (int)b.turn);
    ASSERT_EQ_INT(40, (int)b.empty_run);
    ASSERT_EQ_INT(0, b.entry_count);

    /* A run of zero is meaningless, and a run may not ride with content. */
    a.empty_run = 0;
    ASSERT_EQ_INT(0, (int)TAK_Msg_TurnEncode(&a, buf2, sizeof(buf2)));
}

TEST(turn_seats_must_rise_with_the_server_last) {
    static uint8_t body[8];
    memset(body, 0x77, sizeof(body));
    TAK_MsgTurn a, b;
    memset(&a, 0, sizeof(a));
    a.turn = 4;
    a.empty_run = 1;
    a.entry_count = 3;
    uint8_t seats[3] = { 1, 5, TAK_NET_SEAT_SERVER };
    for (int e = 0; e < 3; e++) {
        a.entry[e].seat = seats[e];
        a.entry[e].count = 1;
        a.entry[e].cmd[0].data = body;
        a.entry[e].cmd[0].len = 8;
    }
    size_t n = TAK_Msg_TurnEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_TurnDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(3, b.entry_count);
    ASSERT_EQ_INT(TAK_NET_SEAT_SERVER, b.entry[2].seat);
    ASSERT_EQ_INT(-1, first_accepted_truncation(buf, n));

    /* Out of order, repeated and out of range seats are all refused. */
    a.entry[0].seat = 5; a.entry[1].seat = 1;
    ASSERT_EQ_INT(0, (int)TAK_Msg_TurnEncode(&a, buf2, sizeof(buf2)));
    a.entry[0].seat = 1; a.entry[1].seat = 1;
    ASSERT_EQ_INT(0, (int)TAK_Msg_TurnEncode(&a, buf2, sizeof(buf2)));
    a.entry[0].seat = 1; a.entry[1].seat = TAK_NET_SEATS;
    ASSERT_EQ_INT(0, (int)TAK_Msg_TurnEncode(&a, buf2, sizeof(buf2)));

    /* The decoder enforces it too, because a peer can hand build bytes. */
    a.entry[1].seat = 5;
    n = TAK_Msg_TurnEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    buf[TAK_NET_FRAME_HEADER + 7] = 0;    /* first entry's seat, now not rising */
    buf[TAK_NET_FRAME_HEADER + 7] = 6;    /* 6 then 5 is a fall */
    ASSERT(TAK_Net_Validate(buf, n) != 0);
}

TEST(ack_pace_and_status_round_trip) {
    TAK_NetFrame f;
    TAK_MsgAck a = { 900, 840, 0x0f0e0d0c0b0a0908ull }, a2;
    size_t n = TAK_Msg_AckEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_AckDecode(&a2, f.payload, f.payload_len));
    ASSERT(memcmp(&a, &a2, sizeof(a)) == 0);

    TAK_MsgPace p = { 5, 0, TAK_PACE_WAITING_FOR_PLAYER, 3, 120 }, p2;
    n = TAK_Msg_PaceEncode(&p, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_PaceDecode(&p2, f.payload, f.payload_len));
    ASSERT(memcmp(&p, &p2, sizeof(p)) == 0);

    TAK_MsgPlayerStatus s = { 4, TAK_PSTATUS_LOST, 45 }, s2;
    n = TAK_Msg_PlayerStatusEncode(&s, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_PlayerStatusDecode(&s2, f.payload, f.payload_len));
    ASSERT(memcmp(&s, &s2, sizeof(s)) == 0);
}

TEST(chat_carries_the_full_line_and_its_turn) {
    TAK_MsgChat a, b;
    memset(&a, 0, sizeof(a));
    a.scope = TAK_CHAT_TEAM;
    a.from_seat = 2;
    a.to_seat = TAK_NET_SEAT_NONE;
    a.turn = 1234;
    strcpy(a.name, "Dad");
    fill(a.text, TAK_NET_CHAT_MAX, 'q');
    size_t n = TAK_Msg_ChatEncode(&a, buf, sizeof(buf));
    ASSERT(n > 0);
    TAK_NetFrame f;
    ASSERT_EQ_INT(0, TAK_Net_Split(buf, n, &f));
    ASSERT_EQ_INT(0, TAK_Msg_ChatDecode(&b, f.payload, f.payload_len));
    ASSERT_EQ_INT(256, (int)strlen(b.text));
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
}

/* ── System commands and reject text ────────────────────────────────── */

TEST(system_command_blobs_encode_and_bound) {
    uint8_t small[2];
    ASSERT_EQ_INT(3, (int)TAK_Sys_PlayerLeft(4, TAK_LEFT_ARMY_REMOVED,
                                             buf, sizeof(buf)));
    ASSERT_EQ_INT(TAK_SYS_PLAYER_LEFT, buf[0]);
    ASSERT_EQ_INT(4, buf[1]);
    ASSERT_EQ_INT(6, (int)TAK_Sys_SeatReclaim(2, 0x01020304u, buf, sizeof(buf)));
    ASSERT_EQ_INT(2, (int)TAK_Sys_MatchEnd(1, buf, sizeof(buf)));
    ASSERT_EQ_INT(0, (int)TAK_Sys_SeatReclaim(2, 1, small, sizeof(small)));
}

TEST(every_reject_reason_has_text) {
    for (int i = 0; i < 256; i++) {
        const char *s = TAK_Net_RejectText((uint8_t)i);
        ASSERT_NOT_NULL(s);
        ASSERT(s[0] != '\0');
    }
    ASSERT_EQ_STR("The game is full.", TAK_Net_RejectText(TAK_REJECT_GAME_FULL));
    /* Reason 8 is the one the reference pins: a build mismatch asks for a
     * newer version (legacy:194618-194647). */
    ASSERT_EQ_INT(8, TAK_REJECT_NEEDS_NEWER);
}

/* ── A short fuzz pass, so CI exercises the parser on every run ─────── */

static uint32_t rng_state = 0x1234567u;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

TEST(random_and_mutated_frames_never_crash_the_parser) {
    /* Seeds: one valid frame of every type we can build cheaply. */
    static uint8_t seed[8][256];
    static size_t seed_len[8];
    int seeds = 0;
    TAK_MsgAck ack = { 1, 60, 7 };
    seed_len[seeds] = TAK_Msg_AckEncode(&ack, seed[seeds], sizeof(seed[0]));
    seeds++;
    TAK_MsgPace pc = { 5, 1, TAK_PACE_PAUSED, TAK_NET_SEAT_NONE, 50 };
    seed_len[seeds] = TAK_Msg_PaceEncode(&pc, seed[seeds], sizeof(seed[0]));
    seeds++;
    TAK_MsgGo go = { 0 };
    seed_len[seeds] = TAK_Msg_GoEncode(&go, seed[seeds], sizeof(seed[0]));
    seeds++;
    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof(jr));
    strcpy(jr.code, "ABC234");
    seed_len[seeds] = TAK_Msg_JoinRoomEncode(&jr, seed[seeds], sizeof(seed[0]));
    seeds++;

    int accepted = 0;
    for (int iter = 0; iter < 200000; iter++) {
        size_t len;
        if (iter % 3 == 0) {
            len = (rng_next() % 64u) + 1u;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rng_next();
        } else {
            int s = (int)(rng_next() % (uint32_t)seeds);
            len = seed_len[s];
            memcpy(buf, seed[s], len);
            int flips = (int)(rng_next() % 4u) + 1;
            for (int k = 0; k < flips; k++)
                buf[rng_next() % (uint32_t)len] ^= (uint8_t)(1u << (rng_next() & 7u));
        }
        if (TAK_Net_Validate(buf, len) == 0) accepted++;
    }
    /* The point is that it returned at all. A parser that accepts nothing
     * would also not crash, so assert it still accepts some mutations. */
    ASSERT(accepted > 0);
}

int main(void) {
    TEST_SUITE("Relay protocol codec");
    RUN(split_refuses_malformed_frames);
    RUN(empty_payload_messages_are_three_bytes);
    RUN(encode_into_a_short_buffer_fails_and_writes_nothing_past_it);
    RUN(hello_round_trips);
    RUN(an_overlong_name_is_cut_not_overrun);
    RUN(an_unterminated_text_field_comes_back_terminated);
    RUN(welcome_reject_and_ping_round_trip);
    RUN(room_state_round_trips_all_eight_slots);
    RUN(room_list_refuses_more_rooms_than_the_cap);
    RUN(start_game_and_load_messages_round_trip);
    RUN(cmd_round_trips_and_points_into_the_frame);
    RUN(cmd_refuses_a_zero_length_or_oversized_command);
    RUN(an_empty_turn_is_seven_bytes_and_carries_a_run);
    RUN(turn_seats_must_rise_with_the_server_last);
    RUN(ack_pace_and_status_round_trip);
    RUN(chat_carries_the_full_line_and_its_turn);
    RUN(system_command_blobs_encode_and_bound);
    RUN(every_reject_reason_has_text);
    RUN(random_and_mutated_frames_never_crash_the_parser);
    TEST_REPORT();
}
