#ifndef TAK_NET_PROTOCOL_H
#define TAK_NET_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "tak_map_fingerprint.h"

/*
 * The wire between a client and the relay. Design in docs/MULTIPLAYER.md.
 *
 * Framing: one logical message per transport frame, little endian, and
 * every frame starts
 *
 *     u8  type
 *     u16 payload length
 *     payload
 *
 * so a client that does not know a type can skip it by its length. The
 * server refuses anything over TAK_NET_FRAME_MAX.
 *
 * Text fields are fixed width and NUL padded rather than length prefixed,
 * so decoding never allocates and a short or unterminated field from a
 * hostile sender still comes back as a valid C string.
 *
 * Command payloads are opaque to this layer. The relay stamps the seat and
 * copies bytes. It never parses a command, because it holds no simulation,
 * and that also keeps the relay independent of the command wire version.
 */

#define TAK_NET_PROTOCOL_VERSION      1
#define TAK_NET_PROTOCOL_MIN          1

#define TAK_NET_FRAME_HEADER          3u
#define TAK_NET_FRAME_MAX             65536u
#define TAK_NET_PAYLOAD_MAX           (TAK_NET_FRAME_MAX - TAK_NET_FRAME_HEADER)

#define TAK_NET_SEATS                 8
#define TAK_NET_WATCHERS_MAX          8
#define TAK_NET_SEAT_NONE             0xffu
/* The seat the relay itself speaks on inside a turn bundle. */
#define TAK_NET_SEAT_SERVER           0xffu

#define TAK_NET_NAME_MAX              16   /* the original's 15 plus a NUL */
#define TAK_NET_ROOM_NAME_MAX         32
#define TAK_NET_MAP_NAME_MAX          64   /* matches BattleConfig.map_name */
#define TAK_NET_PASSWORD_MAX          32
#define TAK_NET_KEY_MAX               32   /* server access key */
#define TAK_NET_CODE_LEN              6    /* invite code characters */
#define TAK_NET_CODE_MAX              (TAK_NET_CODE_LEN + 1)
#define TAK_NET_TOKEN_BYTES           16   /* 128 bit device token */
#define TAK_NET_CHAT_MAX              257  /* the original's 256 plus a NUL */
#define TAK_NET_MOTD_MAX              256
#define TAK_NET_SERVER_NAME_MAX       32
#define TAK_NET_TEXT_MAX              64   /* reject detail, edit text */
#define TAK_NET_GROUP_HASHES          5    /* units, weapons, features, scripts, ai */
/* The map fingerprint is the engine's own, never a second hash. */
#define TAK_NET_FINGERPRINT_BYTES     TAK_MAP_FINGERPRINT_BYTES
#define TAK_NET_ROOMS_PER_LIST        32

/* One CMD message holds up to 64 commands inside a 4 KB budget. A move
 * order for 256 units is about a kilobyte, so the budget covers far more
 * than a person can issue in one 50 ms turn, and eight seats of it still
 * fit inside one frame as a turn bundle. */
#define TAK_NET_CMDS_PER_MSG          64
#define TAK_NET_CMD_BYTES_MAX         4096u
#define TAK_NET_TURN_BYTES_MAX        (TAK_NET_SEATS * TAK_NET_CMD_BYTES_MAX)

/* Turn cadence. 3 ticks is 50 ms at 60 Hz, 20 turns a second. */
#define TAK_NET_TURN_TICKS            3
#define TAK_NET_TURN_MS               50
/* Clients hash on ticks divisible by this. */
#define TAK_NET_HASH_TICKS            60

/* ── Message types ──────────────────────────────────────────────────────
 * Grouped in sixteens so a later addition never disturbs an existing
 * number. 0 is never a valid type. */
typedef enum TAK_NetMsg {
    TAK_MSG_NONE            = 0,

    TAK_MSG_HELLO           = 1,
    TAK_MSG_WELCOME         = 2,
    TAK_MSG_REJECT          = 3,
    TAK_MSG_PING            = 4,
    TAK_MSG_PONG            = 5,

    TAK_MSG_LIST_ROOMS      = 16,
    TAK_MSG_ROOM_LIST       = 17,
    TAK_MSG_CREATE_ROOM     = 18,
    TAK_MSG_JOIN_ROOM       = 19,
    TAK_MSG_LEAVE_ROOM      = 20,
    TAK_MSG_ROOM_EDIT       = 21,
    TAK_MSG_ROOM_STATE      = 22,
    TAK_MSG_CHAT            = 23,
    TAK_MSG_START           = 24,

    TAK_MSG_START_GAME      = 32,
    TAK_MSG_LOAD_PROGRESS   = 33,
    TAK_MSG_LOAD_STATE      = 34,
    TAK_MSG_LOADED          = 35,
    TAK_MSG_GO              = 36,

    TAK_MSG_CMD             = 48,
    TAK_MSG_TURN            = 49,
    TAK_MSG_ACK             = 50,
    TAK_MSG_PACE            = 51,
    TAK_MSG_PLAYER_STATUS   = 52
} TAK_NetMsg;

/* ── Reject reasons ─────────────────────────────────────────────────────
 * 3 through 10 are the original's, in the order its own string table
 * gives them. Reason 8 is corroborated: the host rejects a build id
 * mismatch with 8, and 8 is the message asking for a newer version
 * (legacy:194618-194647). 1 and 2 are the original's and are not
 * identified here, so they stay reserved rather than being guessed at.
 * Ours start at 16. */
typedef enum TAK_NetReject {
    TAK_REJECT_NONE              = 0,
    TAK_REJECT_RESERVED_1        = 1,
    TAK_REJECT_RESERVED_2        = 2,
    TAK_REJECT_GAME_CLOSED       = 3,   /* locked or already in progress */
    TAK_REJECT_WRONG_PASSWORD    = 4,
    TAK_REJECT_GAME_FULL         = 5,
    TAK_REJECT_LOST_CONNECTION   = 6,
    TAK_REJECT_MISSING_UNIT      = 7,
    TAK_REJECT_NEEDS_NEWER       = 8,
    TAK_REJECT_NO_WATCHING       = 9,
    TAK_REJECT_CREATOR_LEFT      = 10,

    TAK_REJECT_DATA_MISMATCH     = 16,
    TAK_REJECT_ENGINE_VERSION    = 17,
    TAK_REJECT_SERVER_PASSWORD   = 18,
    TAK_REJECT_BANNED            = 19,
    TAK_REJECT_RATE_LIMITED      = 20,
    TAK_REJECT_PROTOCOL_VERSION  = 21,
    TAK_REJECT_NO_SUCH_ROOM      = 22,
    TAK_REJECT_NAME_REQUIRED     = 23,
    TAK_REJECT_DETERMINISM_CLASS = 24,
    TAK_REJECT_NOT_ALLOWED       = 25,  /* the editor lacks the right */
    /* One per start gate, so the room can show the original's own message
     * for whichever one refused. */
    TAK_REJECT_NOT_READY         = 26,  /* a human has not pressed Go */
    TAK_REJECT_NEEDS_HUMAN       = 27,  /* no other human in the room */
    TAK_REJECT_ONE_TEAM          = 28,  /* everyone is on the same team */
    TAK_REJECT_MAP_MISSING       = 29,  /* no map, or a human lacks it */
    TAK_REJECT_REMOVED_BY_HOST   = 30
} TAK_NetReject;

/* Which hash group differed, carried in a reject's detail byte. */
typedef enum TAK_NetHashGroup {
    TAK_HASH_UNITS = 0,
    TAK_HASH_WEAPONS,
    TAK_HASH_FEATURES,
    TAK_HASH_SCRIPTS,
    TAK_HASH_AI
} TAK_NetHashGroup;

/* A room's determinism class. Only clients of the same class share a
 * room, which is what lets browser players start before the fixed point
 * conversion makes native builds agree with each other. */
typedef enum TAK_NetClass {
    TAK_CLASS_UNKNOWN = 0,
    TAK_CLASS_BROWSER = 1,   /* one wasm binary, one libm */
    TAK_CLASS_NATIVE  = 2,   /* only once the conversion lands */
    TAK_CLASS_TEST    = 3    /* the in process harness */
} TAK_NetClass;

typedef enum TAK_NetClientKind {
    TAK_CLIENT_PLAYER  = 0,
    TAK_CLIENT_WATCHER = 1,
    TAK_CLIENT_HEADLESS = 2
} TAK_NetClientKind;

typedef enum TAK_NetRoomStatus {
    TAK_ROOM_OPEN = 0,
    TAK_ROOM_LOCKED,
    TAK_ROOM_LOADING,
    TAK_ROOM_IN_PROGRESS,
    TAK_ROOM_ENDED
} TAK_NetRoomStatus;

typedef enum TAK_NetSlotKind {
    TAK_NSLOT_EMPTY = 0,
    TAK_NSLOT_HUMAN,
    TAK_NSLOT_COMPUTER,
    TAK_NSLOT_BLOCKED
} TAK_NetSlotKind;

typedef enum TAK_NetPlayerStatus {
    TAK_PSTATUS_CONNECTED = 0,
    TAK_PSTATUS_LAGGING,
    TAK_PSTATUS_LOST,
    TAK_PSTATUS_CATCHING_UP,
    TAK_PSTATUS_DROPPED
} TAK_NetPlayerStatus;

typedef enum TAK_NetPaceReason {
    TAK_PACE_NORMAL = 0,
    TAK_PACE_SPEED_CHANGE,
    TAK_PACE_PAUSED,
    TAK_PACE_WAITING_FOR_PLAYER,   /* the governor, with a seat named */
    TAK_PACE_DESYNC_HALT
} TAK_NetPaceReason;

/* Room flag bits, shared by CREATE_ROOM, the summary and the snapshot. */
#define TAK_ROOMF_PASSWORD        0x0001u
#define TAK_ROOMF_LISTED          0x0002u
#define TAK_ROOMF_ALLOW_WATCHING  0x0004u
#define TAK_ROOMF_IRON_PLAGUE     0x0008u
#define TAK_ROOMF_HOST_PACES_ONLY 0x0010u
#define TAK_ROOMF_AI_TAKES_OVER   0x0020u   /* on a drop, else the army goes */

/* HELLO flag bits. */
#define TAK_HELLOF_IRON_PLAGUE    0x01u
#define TAK_HELLOF_WANTS_REJOIN   0x02u

/* ── Fields a ROOM_EDIT may change ──────────────────────────────────── */
typedef enum TAK_NetEditField {
    TAK_EDIT_NONE = 0,
    /* Your own row. Each of these clears your ready. */
    TAK_EDIT_SIDE,
    TAK_EDIT_COLOUR,        /* a request, the server picks the next free */
    TAK_EDIT_TEAM,
    TAK_EDIT_NAME,
    TAK_EDIT_WATCH,         /* leave your seat and watch, when allowed */
    TAK_EDIT_READY,         /* toggles, it does not latch */
    /* Your fingerprint for the room's map, all zero when you lack it. Sent
     * by the client on its own after a map change, so it leaves ready
     * alone. This is the original's matching map gate. */
    TAK_EDIT_HAVE_MAP,
    /* The host's. Map, options and the cap clear everyone's ready. */
    TAK_EDIT_MAP = 32,
    TAK_EDIT_OPTIONS,
    TAK_EDIT_UNIT_CAP,
    TAK_EDIT_TIMEOUT,
    TAK_EDIT_ALLOW_WATCHING,
    TAK_EDIT_ADD_COMPUTER,
    TAK_EDIT_REMOVE_COMPUTER,
    TAK_EDIT_BLOCK_SLOT,
    TAK_EDIT_UNBLOCK_SLOT,
    TAK_EDIT_KICK
} TAK_NetEditField;

/* ── System commands the relay injects into the turn stream ───────────
 * They ride on TAK_NET_SEAT_SERVER so every simulation applies them on
 * the same tick as the player commands around them. */
typedef enum TAK_NetSysCmd {
    TAK_SYS_NONE = 0,
    TAK_SYS_PLAYER_LEFT,     /* u8 seat, u8 disposition */
    TAK_SYS_SEAT_RECLAIM,    /* u8 seat, u32 client id */
    TAK_SYS_MATCH_END        /* u8 reason */
} TAK_NetSysCmd;

typedef enum TAK_NetLeftAs {
    TAK_LEFT_COMPUTER_TAKES_OVER = 0,
    TAK_LEFT_ARMY_REMOVED,
    TAK_LEFT_RESIGNED
} TAK_NetLeftAs;

/* ── Messages ───────────────────────────────────────────────────────── */

typedef struct TAK_MsgHello {
    uint16_t protocol_version;
    uint32_t engine_build_id;
    uint8_t  determinism_class;
    uint8_t  client_kind;
    uint8_t  flags;
    uint64_t schema_hash;
    uint64_t content_hash;
    uint64_t group_hash[TAK_NET_GROUP_HASHES];
    uint8_t  device_token[TAK_NET_TOKEN_BYTES];
    char     name[TAK_NET_NAME_MAX];
    char     access_key[TAK_NET_KEY_MAX];
} TAK_MsgHello;

typedef struct TAK_MsgWelcome {
    uint32_t session_id;
    uint32_t flags;
    uint16_t protocol_min;
    uint16_t protocol_max;
    uint32_t newest_build;
    char     server_name[TAK_NET_SERVER_NAME_MAX];
    char     motd[TAK_NET_MOTD_MAX];
} TAK_MsgWelcome;

typedef struct TAK_MsgReject {
    uint8_t reason;
    uint8_t detail;
    char    text[TAK_NET_TEXT_MAX];
} TAK_MsgReject;

typedef struct TAK_MsgPing {
    uint32_t seq;
    uint64_t sent_ms;    /* the sender's own clock, echoed unchanged */
} TAK_MsgPing;

typedef TAK_MsgPing TAK_MsgPong;

typedef struct TAK_MsgListRooms {
    uint32_t filter_flags;
} TAK_MsgListRooms;

typedef struct TAK_RoomSummary {
    uint32_t room_id;
    char     code[TAK_NET_CODE_MAX];
    char     name[TAK_NET_ROOM_NAME_MAX];
    char     host_name[TAK_NET_NAME_MAX];
    char     map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t  map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    uint8_t  players;
    uint8_t  max_players;
    uint8_t  watchers;
    uint8_t  status;
    uint32_t flags;
    uint32_t engine_build_id;
    uint8_t  determinism_class;
    /* Zero when this client may join. Otherwise the reason, so the list
     * can grey the row and say why instead of hiding it. */
    uint8_t  compat;
} TAK_RoomSummary;

typedef struct TAK_MsgRoomList {
    uint8_t         flags;      /* bit 0: a full listing, not an update */
    uint8_t         count;
    TAK_RoomSummary room[TAK_NET_ROOMS_PER_LIST];
} TAK_MsgRoomList;
#define TAK_ROOMLISTF_FULL 0x01u

typedef struct TAK_MsgCreateRoom {
    char     name[TAK_NET_ROOM_NAME_MAX];
    char     password[TAK_NET_PASSWORD_MAX];
    char     map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t  map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    uint32_t flags;
    uint32_t options;
    uint8_t  max_players;
    uint16_t unit_cap;
    uint16_t timeout_secs;
} TAK_MsgCreateRoom;

typedef struct TAK_MsgJoinRoom {
    uint32_t room_id;                  /* 0 means join by code */
    char     code[TAK_NET_CODE_MAX];
    char     password[TAK_NET_PASSWORD_MAX];
    uint8_t  as_watcher;
} TAK_MsgJoinRoom;

typedef struct TAK_MsgRoomEdit {
    uint8_t  field;
    uint8_t  seat;                     /* TAK_NET_SEAT_NONE when not a slot */
    uint32_t value;
    char     text[TAK_NET_TEXT_MAX];
    /* TAK_EDIT_MAP and TAK_EDIT_HAVE_MAP: the map's own fingerprint travels
     * with its name, because two installs can hold different maps under
     * one name. */
    uint8_t  fingerprint[TAK_NET_FINGERPRINT_BYTES];
} TAK_MsgRoomEdit;

typedef struct TAK_NetSlot {
    uint8_t  kind;
    uint8_t  side;
    uint8_t  colour;
    uint8_t  team;
    uint8_t  ready;
    uint8_t  connected;
    uint8_t  load_percent;
    uint8_t  flags;          /* bit 0 loaded */
    uint16_t ping_ms;
    uint32_t client_id;      /* 0 for an empty or computer seat */
    char     name[TAK_NET_NAME_MAX];
} TAK_NetSlot;
#define TAK_SLOTF_LOADED  0x01u
#define TAK_SLOTF_HAS_MAP 0x02u   /* reported the room's map fingerprint */

typedef struct TAK_MsgRoomState {
    uint32_t    room_id;
    uint32_t    revision;
    char        code[TAK_NET_CODE_MAX];
    char        name[TAK_NET_ROOM_NAME_MAX];
    char        map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t     map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    uint32_t    host_client_id;
    uint32_t    flags;
    uint32_t    options;
    uint8_t     status;
    uint8_t     watchers;
    uint16_t    unit_cap;
    uint16_t    timeout_secs;
    uint8_t     seat_count;
    TAK_NetSlot slot[TAK_NET_SEATS];
} TAK_MsgRoomState;

typedef enum TAK_NetChatScope {
    TAK_CHAT_ROOM = 0,
    TAK_CHAT_ALL,
    TAK_CHAT_TEAM,
    TAK_CHAT_DIRECT,
    TAK_CHAT_WATCHERS,
    TAK_CHAT_SYSTEM
} TAK_NetChatScope;

typedef struct TAK_MsgChat {
    uint8_t  scope;
    uint8_t  from_seat;      /* TAK_NET_SEAT_NONE from a watcher or the server */
    uint8_t  to_seat;
    uint32_t turn;           /* server stamped, 0 on the way up */
    char     name[TAK_NET_NAME_MAX];
    char     text[TAK_NET_CHAT_MAX];
} TAK_MsgChat;

typedef struct TAK_MsgStartGame {
    uint32_t match_id;
    uint32_t seed;
    uint8_t  your_seat;
    uint8_t  turn_ticks;
    uint8_t  flags;
    uint64_t schema_hash;
    uint64_t content_hash;
    char     map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t  map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    uint32_t options;
    uint16_t unit_cap;
    uint16_t timeout_secs;
    struct {
        uint8_t kind;
        uint8_t side;
        uint8_t colour;
        uint8_t team;
        char    name[TAK_NET_NAME_MAX];
    } slot[TAK_NET_SEATS];
} TAK_MsgStartGame;

typedef struct TAK_MsgLoadProgress {
    uint8_t percent;
} TAK_MsgLoadProgress;

typedef struct TAK_MsgLoadState {
    uint8_t count;
    struct { uint8_t seat; uint8_t percent; uint8_t loaded; }
        entry[TAK_NET_SEATS];
} TAK_MsgLoadState;

typedef struct TAK_MsgLoaded {
    uint64_t world_hash;
} TAK_MsgLoaded;

typedef struct TAK_MsgGo {
    uint32_t first_turn;
} TAK_MsgGo;

/* A command as it crosses this layer: bytes we do not interpret. Decode
 * points into the caller's frame buffer and copies nothing, so the blobs
 * are valid only while that buffer is. */
typedef struct TAK_CmdBlob {
    const uint8_t *data;
    uint16_t       len;
} TAK_CmdBlob;

typedef struct TAK_MsgCmd {
    uint32_t    client_seq;
    uint8_t     count;
    TAK_CmdBlob cmd[TAK_NET_CMDS_PER_MSG];
} TAK_MsgCmd;

typedef struct TAK_TurnEntry {
    uint8_t     seat;        /* 0..7, or TAK_NET_SEAT_SERVER */
    uint8_t     count;
    TAK_CmdBlob cmd[TAK_NET_CMDS_PER_MSG];
} TAK_TurnEntry;

/* Seats appear in strictly increasing order, with the server's entry last.
 * Two relays given the same inputs therefore produce the same bytes, which
 * is what makes a turn log comparable. */
typedef struct TAK_MsgTurn {
    uint32_t      turn;
    uint16_t      empty_run;   /* this turn and the next run-1 are empty */
    uint8_t       entry_count;
    TAK_TurnEntry entry[TAK_NET_SEATS + 1];
} TAK_MsgTurn;

#define TAK_NET_NO_HASH 0xffffffffu

typedef struct TAK_MsgAck {
    uint32_t last_turn;
    uint32_t hash_tick;      /* TAK_NET_NO_HASH when this ack has no hash */
    uint64_t state_hash;
} TAK_MsgAck;

typedef struct TAK_MsgPace {
    uint8_t  speed_level;
    uint8_t  paused;
    uint8_t  reason;
    uint8_t  seat;           /* whom we are waiting for, or none */
    uint16_t turn_period_ms;
} TAK_MsgPace;

typedef struct TAK_MsgPlayerStatus {
    uint8_t  seat;
    uint8_t  status;
    uint16_t countdown_secs;
} TAK_MsgPlayerStatus;

/* ── Framing ────────────────────────────────────────────────────────── */

typedef struct TAK_NetFrame {
    uint8_t        type;
    const uint8_t *payload;
    uint16_t       payload_len;
} TAK_NetFrame;

/* Split one received frame. Returns 0 on success. Refuses a short frame,
 * a length that disagrees with the buffer, and type 0. */
int TAK_Net_Split(const void *data, size_t len, TAK_NetFrame *out);

/* Number of whole bytes a frame of this payload size needs. */
size_t TAK_Net_FrameSize(size_t payload_len);

/* ── Encode and decode ──────────────────────────────────────────────────
 * Encode writes a whole frame and returns its length, or 0 when it does
 * not fit. Decode reads a payload, as handed back by TAK_Net_Split, and
 * returns 0 on success. A decode that leaves trailing bytes fails, so a
 * sender and this parser cannot silently disagree about a message. */

size_t TAK_Msg_HelloEncode(const TAK_MsgHello *m, void *out, size_t cap);
int    TAK_Msg_HelloDecode(TAK_MsgHello *m, const void *p, size_t len);

size_t TAK_Msg_WelcomeEncode(const TAK_MsgWelcome *m, void *out, size_t cap);
int    TAK_Msg_WelcomeDecode(TAK_MsgWelcome *m, const void *p, size_t len);

size_t TAK_Msg_RejectEncode(const TAK_MsgReject *m, void *out, size_t cap);
int    TAK_Msg_RejectDecode(TAK_MsgReject *m, const void *p, size_t len);

size_t TAK_Msg_PingEncode(uint8_t type, const TAK_MsgPing *m,
                          void *out, size_t cap);
int    TAK_Msg_PingDecode(TAK_MsgPing *m, const void *p, size_t len);

size_t TAK_Msg_ListRoomsEncode(const TAK_MsgListRooms *m, void *out, size_t cap);
int    TAK_Msg_ListRoomsDecode(TAK_MsgListRooms *m, const void *p, size_t len);

size_t TAK_Msg_RoomListEncode(const TAK_MsgRoomList *m, void *out, size_t cap);
int    TAK_Msg_RoomListDecode(TAK_MsgRoomList *m, const void *p, size_t len);

size_t TAK_Msg_CreateRoomEncode(const TAK_MsgCreateRoom *m, void *out, size_t cap);
int    TAK_Msg_CreateRoomDecode(TAK_MsgCreateRoom *m, const void *p, size_t len);

size_t TAK_Msg_JoinRoomEncode(const TAK_MsgJoinRoom *m, void *out, size_t cap);
int    TAK_Msg_JoinRoomDecode(TAK_MsgJoinRoom *m, const void *p, size_t len);

size_t TAK_Msg_EmptyEncode(uint8_t type, void *out, size_t cap);

size_t TAK_Msg_RoomEditEncode(const TAK_MsgRoomEdit *m, void *out, size_t cap);
int    TAK_Msg_RoomEditDecode(TAK_MsgRoomEdit *m, const void *p, size_t len);

size_t TAK_Msg_RoomStateEncode(const TAK_MsgRoomState *m, void *out, size_t cap);
int    TAK_Msg_RoomStateDecode(TAK_MsgRoomState *m, const void *p, size_t len);

size_t TAK_Msg_ChatEncode(const TAK_MsgChat *m, void *out, size_t cap);
int    TAK_Msg_ChatDecode(TAK_MsgChat *m, const void *p, size_t len);

size_t TAK_Msg_StartGameEncode(const TAK_MsgStartGame *m, void *out, size_t cap);
int    TAK_Msg_StartGameDecode(TAK_MsgStartGame *m, const void *p, size_t len);

size_t TAK_Msg_LoadProgressEncode(const TAK_MsgLoadProgress *m,
                                  void *out, size_t cap);
int    TAK_Msg_LoadProgressDecode(TAK_MsgLoadProgress *m,
                                  const void *p, size_t len);

size_t TAK_Msg_LoadStateEncode(const TAK_MsgLoadState *m, void *out, size_t cap);
int    TAK_Msg_LoadStateDecode(TAK_MsgLoadState *m, const void *p, size_t len);

size_t TAK_Msg_LoadedEncode(const TAK_MsgLoaded *m, void *out, size_t cap);
int    TAK_Msg_LoadedDecode(TAK_MsgLoaded *m, const void *p, size_t len);

size_t TAK_Msg_GoEncode(const TAK_MsgGo *m, void *out, size_t cap);
int    TAK_Msg_GoDecode(TAK_MsgGo *m, const void *p, size_t len);

size_t TAK_Msg_CmdEncode(const TAK_MsgCmd *m, void *out, size_t cap);
int    TAK_Msg_CmdDecode(TAK_MsgCmd *m, const void *p, size_t len);

size_t TAK_Msg_TurnEncode(const TAK_MsgTurn *m, void *out, size_t cap);
int    TAK_Msg_TurnDecode(TAK_MsgTurn *m, const void *p, size_t len);

size_t TAK_Msg_AckEncode(const TAK_MsgAck *m, void *out, size_t cap);
int    TAK_Msg_AckDecode(TAK_MsgAck *m, const void *p, size_t len);

size_t TAK_Msg_PaceEncode(const TAK_MsgPace *m, void *out, size_t cap);
int    TAK_Msg_PaceDecode(TAK_MsgPace *m, const void *p, size_t len);

size_t TAK_Msg_PlayerStatusEncode(const TAK_MsgPlayerStatus *m,
                                  void *out, size_t cap);
int    TAK_Msg_PlayerStatusDecode(TAK_MsgPlayerStatus *m,
                                  const void *p, size_t len);

/* ── System commands ────────────────────────────────────────────────── */

/* Write one system command blob. Returns its length, or 0 when it does
 * not fit. These are the bodies the relay puts on TAK_NET_SEAT_SERVER. */
size_t TAK_Sys_PlayerLeft(uint8_t seat, uint8_t left_as, void *out, size_t cap);
size_t TAK_Sys_SeatReclaim(uint8_t seat, uint32_t client_id,
                           void *out, size_t cap);
size_t TAK_Sys_MatchEnd(uint8_t reason, void *out, size_t cap);

/* Reject text the client shows. Never NULL. */
const char *TAK_Net_RejectText(uint8_t reason);

/* Compare two secrets held in fixed width fields, a room password or a
 * server key, taking the same time however much of them matches. */
int TAK_Net_SecretEqual(const char *a, const char *b, size_t field);

/* One pass over an arbitrary frame: split it, decode it into the right
 * struct, and throw it away. The fuzz target and the relay's own input
 * validation both go through this, so anything the fuzzer reaches is
 * reachable in production. Returns 0 when the frame decoded cleanly. */
int TAK_Net_Validate(const void *data, size_t len);

#endif /* TAK_NET_PROTOCOL_H */
