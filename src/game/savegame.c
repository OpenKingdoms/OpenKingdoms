/*
 * savegame.c: the game state inside the .oksave container.
 *
 * The container carries bytes. This is the half that knows what a
 * battle is. Every field goes out at an explicit width through
 * tak_bytes.h and no struct is ever handed to a write call, so a save
 * written by the 32 bit Windows build or the wasm32 browser build
 * opens on a 64 bit macOS or Linux build.
 *
 * Nothing here touches a platform or a window. See
 * docs/notes/2026-09-11-save-sections.md.
 */

#include "tak_savegame.h"

#include "tak_ai.h"
#include "tak_battle_config.h"
#include "tak_bytes.h"
#include "tak_cob_vm.h"
#include "tak_economy.h"
#include "tak_occupancy.h"
#include "tak_features.h"
#include "tak_map_fingerprint.h"
#include "tak_memory.h"
#include "tak_savefile.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── section layouts ──────────────────────────────────────────────── */

/* CFGB, the battle the player set up. */
#define CFGB_MAP_NAME       0u
#define CFGB_MAP_NAME_CAP  96u
#define CFGB_PLAYERS       (CFGB_MAP_NAME + CFGB_MAP_NAME_CAP)
#define CFGB_SLOT_BYTES    52u
#define CFGB_SLOT_NAME_CAP 32u
#define CFGB_OPTIONS       (CFGB_PLAYERS + CFGB_SLOT_BYTES * TAK_MAX_PLAYERS)
#define CFGB_OPTION_COUNT   8u
#define CFGB_END           (CFGB_OPTIONS + CFGB_OPTION_COUNT * 4u)
_Static_assert(CFGB_END == TAK_CFGB_BYTES, "CFGB layout and width disagree");

/* WRLD, the world scalars and the per player tallies. */
#define WRLD_MAP_NAME        0u
#define WRLD_MAP_NAME_CAP   96u
#define WRLD_KINGDOM        (WRLD_MAP_NAME + WRLD_MAP_NAME_CAP)
#define WRLD_KINGDOM_CAP    32u
#define WRLD_END_REASON     (WRLD_KINGDOM + WRLD_KINGDOM_CAP)
#define WRLD_END_REASON_CAP 64u
#define WRLD_SCALARS        (WRLD_END_REASON + WRLD_END_REASON_CAP)
#define WRLD_SCALAR_COUNT   12u
#define WRLD_STATS          (WRLD_SCALARS + WRLD_SCALAR_COUNT * 4u)
#define WRLD_STAT_BYTES     24u
#define WRLD_END            (WRLD_STATS + WRLD_STAT_BYTES * (TAK_MAX_PLAYERS + 1))
_Static_assert(WRLD_END == TAK_WRLD_BYTES, "WRLD layout and width disagree");

/* The scalars, in the order they are written. */
#define WS_WATER_HEIGHT    0u
#define WS_SK_TICKS        1u
#define WS_SK_GAME_OVER    2u
#define WS_SK_WINNER       3u
#define WS_SK_RESULT       4u
#define WS_SK_END_TICK     5u
#define WS_SK_STATS_OPEN   6u
#define WS_MI_TICKS        7u
#define WS_MI_SECONDS      8u
#define WS_MI_OBJECTIVES   9u
#define WS_MI_VICTORY     10u
#define WS_RAND_STATE     11u

#define CAMR_X 0u
#define CAMR_Y 4u
_Static_assert(CAMR_Y + 4u == TAK_CAMR_BYTES, "CAMR layout and width disagree");

/* DEFS, one record per definition the battle reaches. */
#define DEFS_NAME_IDX 0u
#define DEFS_KIND     2u
#define DEFS_PAD      3u
#define DEFS_HASH     4u
_Static_assert(DEFS_HASH + 8u == TAK_DEFS_RECORD_BYTES,
               "DEFS layout and width disagree");

/* ── UNIT, one record per slot ────────────────────────────────────
 *
 * Slots are append only and never compacted, so slot i is written to
 * record i and read back into slot i, dead slots included. That is
 * what keeps Unit.target, build_target, carried_by, load_queue,
 * xfer_cargo and both projectile handles valid with no remap pass.
 *
 * A dead slot is a tombstone: its lifecycle byte and its stable id,
 * and the rest of the record zero. Everything else in a dead slot is
 * whatever it held when the unit died. */
#define U_STABLE_ID       0u
#define U_WORLD_X         4u
#define U_WORLD_Y         8u
#define U_HEADING        12u
#define U_PITCH          16u
#define U_ROLL           20u
#define U_VELOCITY       24u
#define U_HEALTH         28u
#define U_MAX_HEALTH     32u
#define U_CMD_X          36u
#define U_CMD_Y          40u
#define U_PATROL_X       44u
#define U_PATROL_Y       48u
#define U_EXPERIENCE     52u
#define U_RECLAIM_ACCUM  56u
#define U_RAISE_LEFT     60u
#define U_UNLOAD_GX      64u
#define U_UNLOAD_GY      68u
#define U_FLIGHT_ALT     72u
#define U_MANA           76u
#define U_MANA_MAX       80u
#define U_BUILD_HP       84u
#define U_SUBPIXEL_X     88u
#define U_SUBPIXEL_Y     92u
#define U_CUR_SPEED      96u
#define U_PATH_GOAL_X   100u
#define U_PATH_GOAL_Y   104u
#define U_WP_BEST_D2    108u
#define U_STALL_PX      112u
#define U_STALL_PY      116u
#define U_STALL_RT_LEFT 120u
#define U_STALL_RT_BEST 124u
#define U_STALL_RT_MARK 128u
#define U_STALL_LN_BEST 132u
#define U_STALL_LN_MARK 136u
#define U_STALL_TAIL    140u
#define U_STALL_ORD_X   144u
#define U_STALL_ORD_Y   148u
#define U_ROUTE_SEG_X   152u
#define U_ROUTE_SEG_Y   156u
#define U_RALLY_X       160u
#define U_RALLY_Y       164u
/* The definition, as a DEFS record ordinal. A raw registry index does
 * not travel: a loose file install or a mod changes the order. */
#define U_DEF_REF       168u

#define U_W16           172u
#define U_TARGET        (U_W16 +  0u)
#define U_CMD_KIND      (U_W16 +  2u)
#define U_ATTACK_CD     (U_W16 +  4u)
#define U_KILLS         (U_W16 +  6u)
#define U_BUILD_TARGET  (U_W16 +  8u)
#define U_RECLAIM_TX    (U_W16 + 10u)
#define U_RECLAIM_TY    (U_W16 + 12u)
#define U_CARRIED_BY    (U_W16 + 14u)
#define U_CARGO_COUNT   (U_W16 + 16u)
#define U_CARGO_SIZE    (U_W16 + 18u)
#define U_XFER_CARGO    (U_W16 + 20u)
#define U_CARRY_SEQ     (U_W16 + 22u)
#define U_CARRY_NEXT    (U_W16 + 24u)
#define U_GATE_SCAN_CD  (U_W16 + 26u)
#define U_GATE_HOLD     (U_W16 + 28u)
#define U_OCC_TX        (U_W16 + 30u)
#define U_OCC_TY        (U_W16 + 32u)
#define U_NANO_IDLE     (U_W16 + 34u)
#define U_WP_STALL      (U_W16 + 36u)
#define U_PATH_REPLAN   (U_W16 + 38u)
#define U_ROUTE_SERIAL  (U_W16 + 40u)
#define U_STALL_TSERIAL (U_W16 + 42u)
#define U_STALL_TICKS   (U_W16 + 44u)
#define U_ROUTE_CHECK   (U_W16 + 46u)
#define U_STILL_TICKS   (U_W16 + 48u)
#define U_SCRIPT_EV     (U_W16 + 50u)
#define U_W16_END       (U_W16 + 50u + UNIT_SCRIPT_EV_COUNT * 2u)

#define U_B8            U_W16_END
#define U_PLAYER_ID     (U_B8 +  0u)
#define U_COLOR_IDX     (U_B8 +  1u)
#define U_ALIVE         (U_B8 +  2u)
#define U_AGGRO         (U_B8 +  3u)
#define U_WEAPON_SLOT   (U_B8 +  4u)
#define U_CORPSE_TYPE   (U_B8 +  5u)
#define U_RAISE_MODE    (U_B8 +  6u)
#define U_LOADQ_LEN     (U_B8 +  7u)
#define U_XFER_TICKS    (U_B8 +  8u)
#define U_XFER_WAIT     (U_B8 +  9u)
#define U_UNLOAD_STAGE  (U_B8 + 10u)
#define U_UNLOAD_DELAY  (U_B8 + 11u)
#define U_UNLOAD_HOLD   (U_B8 + 12u)
#define U_UNLOAD_TRIES  (U_B8 + 13u)
#define U_UNLOAD_RESTS  (U_B8 + 14u)
#define U_UNLOAD_APPR   (U_B8 + 15u)
#define U_UNDER_CONSTR  (U_B8 + 16u)
#define U_COB_ACT       (U_B8 + 17u)
#define U_COB_STANCE    (U_B8 + 18u)
#define U_COB_YARD      (U_B8 + 19u)
#define U_COB_BUGGER    (U_B8 + 20u)
#define U_FLYING        (U_B8 + 21u)
#define U_SFX_OCCUPY    (U_B8 + 22u)
#define U_ATTACK_EXPL   (U_B8 + 23u)
#define U_OCC_ON        (U_B8 + 24u)
#define U_OCC_PENDING   (U_B8 + 25u)
#define U_OCC_FX        (U_B8 + 26u)
#define U_OCC_FZ        (U_B8 + 27u)
#define U_PATH_LEN      (U_B8 + 28u)
#define U_PATH_INDEX    (U_B8 + 29u)
#define U_PATH_FAILED   (U_B8 + 30u)
#define U_PATH_PENDING  (U_B8 + 31u)
#define U_PATH_WAIT     (U_B8 + 32u)
#define U_BLOCKED_TICKS (U_B8 + 33u)
#define U_STALL_TINDEX  (U_B8 + 34u)
#define U_STALL_ESC     (U_B8 + 35u)
#define U_ROUTE_FLAGS   (U_B8 + 36u)
#define U_OCC_PARKED    (U_B8 + 37u)
#define U_ANIM_STATE    (U_B8 + 38u)
#define U_WALK_THREAD   (U_B8 + 39u)
#define U_KILLED_THREAD (U_B8 + 40u)
#define U_BUILD_THREAD  (U_B8 + 41u)
#define U_MOVE_TIER     (U_B8 + 42u)
#define U_TURN_SIGN     (U_B8 + 43u)
#define U_PRODQ_LEN     (U_B8 + 44u)
#define U_RALLY_SET     (U_B8 + 45u)
#define U_HAS_COB       (U_B8 + 46u)
#define U_B8_END        (U_B8 + 48u)

#define U_WPN           U_B8_END
#define U_WPN_BYTES     18u
#define U_WPN_COOLDOWN   0u
#define U_WPN_BURST_T    4u
#define U_WPN_BURST_REM  8u
#define U_WPN_BURST_TGT 10u
#define U_WPN_AIM_TICKS 12u
#define U_WPN_AIM_TGT   14u
#define U_WPN_AIM_SLOT  16u
#define U_WPN_END       (U_WPN + U_WPN_BYTES * 3u)

#define U_LOAD_QUEUE    U_WPN_END
#define U_PROD_QUEUE    (U_LOAD_QUEUE + UNIT_LOAD_QUEUE_MAX * 2u)
#define U_END           (U_PROD_QUEUE + UNIT_PROD_QUEUE_MAX * 2u)
_Static_assert(U_END == TAK_UNIT_RECORD_BYTES, "UNIT layout and width disagree");

/* PROJ, one record per projectile in flight. Slots are recycled, so
 * each record names its slot rather than standing in slot order. */
#define P_SLOT            0u
#define P_WORLD_X         4u
#define P_WORLD_Y         8u
#define P_SUB_X          12u
#define P_SUB_Y          16u
#define P_DIR_X          20u
#define P_DIR_Y          24u
#define P_SPEED          28u
#define P_DAMAGE         32u
#define P_AOE            36u
#define P_EDGE           40u
#define P_DEST_X         44u
#define P_DEST_Y         48u
#define P_SRC_X          52u
#define P_SRC_Y          56u
#define P_HEIGHT         60u
#define P_VEL_UP         64u
#define P_GRAVITY        68u
#define P_HEADING        72u
#define P_PITCH          76u
#define P_ROLL           80u
#define P_SPIN_PITCH     84u
#define P_SPIN_HEADING   88u
#define P_SPIN_ROLL      92u
#define P_SRC_HEIGHT     96u
#define P_TARGET        100u
#define P_SHOOTER       102u
#define P_TTL           104u
#define P_AGE           106u
#define P_PLAYER_ID     108u
#define P_VISUAL_KIND   109u
#define P_FRIENDLY_FIRE 110u
#define P_IS_BEAM       111u
#define P_COLOR_IDX     112u
#define P_SCALE_COUNT   113u
#define P_HIT_CLASS     114u
#define P_HIT_SOUND     116u
#define P_WATER_SOUND   118u
#define P_SCALES        120u
#define P_SCALE_BYTES     6u
#define P_END           (P_SCALES + P_SCALE_BYTES * TAK_DAMAGE_CATEGORY_MAX)
_Static_assert(P_END == TAK_PROJ_RECORD_BYTES, "PROJ layout and width disagree");

/* FEAT, one record per placed feature, corpses included. */
#define F_FEAT_ID     0u
#define F_TILE_X      2u
#define F_TILE_Z      4u
#define F_HEADING     6u
#define F_PITCH       8u
#define F_ROLL       10u
#define F_COLOR_IDX  12u
#define F_DEF_REF    14u
#define F_WORLD_X    16u
#define F_WORLD_Y    20u
#define F_DECOMPOSE  24u
#define F_SINK       28u
#define F_END        32u
_Static_assert(F_END == TAK_FEAT_RECORD_BYTES, "FEAT layout and width disagree");

/* FOGV. Fog is history and cannot be recomputed from the present, so
 * every seat layer goes in the file, not only the local one. */
#define FOG_W          0u
#define FOG_H          4u
#define FOG_CELL_PX    8u
#define FOG_LAYER_MASK 12u
#define FOG_HEAD_END   16u
_Static_assert(FOG_HEAD_END == TAK_FOGV_HEADER_BYTES,
               "FOGV header layout and width disagree");

/* ECON. max_mana and regen_per_sec look derivable from the monarch
 * and the lodestones, but they are adjusted in place on every capture
 * and loss and there is no recompute from the world. */
#define EC_ACTIVE       0u
#define EC_PLAYERS      4u
#define EC_SLOT_BYTES  32u
#define EC_MANA         0u
#define EC_MAX_MANA     4u
#define EC_REGEN        8u
#define EC_SPENT_LAST  12u
#define EC_EARNED_LAST 16u
#define EC_EARNED_ACC  20u
#define EC_SPENT_ACC   24u
#define EC_WINDOW      28u
#define EC_END         (EC_PLAYERS + EC_SLOT_BYTES * TAK_MAX_PLAYERS)
_Static_assert(EC_END == TAK_ECON_BYTES, "ECON layout and width disagree");

/* One COB piece, and the fixed half of one COB thread. */
#define CP_ROT         0u
#define CP_ROT_TARGET 12u
#define CP_ROT_SPEED  24u
#define CP_SPIN_SPEED 36u
#define CP_SPIN_ACCEL 48u
#define CP_POS        60u
#define CP_POS_TARGET 72u
#define CP_POS_SPEED  84u
#define CP_HIDDEN     96u
#define CP_EXPLODED   97u
#define CP_SHADOW_OFF 98u
#define CP_SHADE_OFF  99u
#define CP_END       100u
_Static_assert(CP_END == TAK_COB_PIECE_BYTES,
               "COB piece layout and width disagree");

#define CT_PC           0u
#define CT_SLEEP        4u
#define CT_RETURN_VAL   8u
#define CT_SIGNAL_MASK 12u
#define CT_WAIT_PIECE  16u
#define CT_WAIT_CHILD  18u
#define CT_WAIT_AXIS   19u
#define CT_WAIT_KIND   20u
#define CT_SP          21u
#define CT_ALIVE       22u
#define CT_HAS_RETURN  23u
#define CT_END         24u
_Static_assert(CT_END == TAK_COB_THREAD_BYTES,
               "COB thread layout and width disagree");

/* The reader understands each section up to this version. */
#define VER_DEFS 1
#define VER_CFGB 1
#define VER_WRLD 1
#define VER_CAMR 1
#define VER_STRT 1
#define VER_SUMM 1
#define VER_UNIT 1
#define VER_UPTH 1
#define VER_UCOB 1
#define VER_PROJ 1
#define VER_FEAT 1
#define VER_FOGV 1
#define VER_ECON 1
#define VER_AIST 1

/* ── small helpers ────────────────────────────────────────────────── */

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* A fixed width text field, NUL padded. Never a struct copy. */
static void put_text(uint8_t *p, size_t cap, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > cap) n = cap;
    if (n) memcpy(p, s, n);
    if (n < cap) memset(p + n, 0, cap - n);
}

static void get_text(char *dst, size_t dst_cap, const uint8_t *p, size_t cap) {
    if (!dst || dst_cap == 0) return;
    size_t n = cap;
    if (n > dst_cap - 1) n = dst_cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* ── the definition hash ──────────────────────────────────────────── */

/* FNV-1a over explicitly widened little endian bytes, so the 32 bit
 * Windows build, the wasm32 browser build and the 64 bit macOS and
 * Linux builds all produce the same number from the same definition.
 *
 * Only fields that change how a battle plays go in. Art, icons, sounds
 * and display names are left out on purpose: re-skinning a unit must
 * not refuse a save. */
#define DEF_HASH_SEED 1469598103934665603ull

static uint64_t h64_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t h64_u32(uint64_t h, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xffu);
    b[1] = (unsigned char)((v >> 8) & 0xffu);
    b[2] = (unsigned char)((v >> 16) & 0xffu);
    b[3] = (unsigned char)((v >> 24) & 0xffu);
    return h64_bytes(h, b, sizeof(b));
}

static uint64_t h64_i32(uint64_t h, int32_t v) { return h64_u32(h, (uint32_t)v); }

/* Floats by bit pattern, moved with memcpy. A pointer cast breaks
 * strict aliasing and a union may move a value rather than the
 * representation. */
static uint64_t h64_f32(uint64_t h, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return h64_u32(h, bits);
}

/* Length first, so two different splits cannot collide. */
static uint64_t h64_str(uint64_t h, const char *s) {
    size_t n = s ? strlen(s) : 0;
    h = h64_u32(h, (uint32_t)n);
    return n ? h64_bytes(h, s, n) : h;
}

static uint64_t hash_weapon(uint64_t h, const UnitWeapon *w) {
    h = h64_str(h, w->name);
    h = h64_str(h, w->type);
    h = h64_str(h, w->damage_type);
    h = h64_str(h, w->subtype);
    h = h64_i32(h, w->range);
    h = h64_i32(h, w->min_range);
    h = h64_i32(h, w->area_of_effect);
    h = h64_f32(h, w->edge_effectiveness);
    h = h64_i32(h, w->burst);
    h = h64_i32(h, w->burst_rate_ticks);
    h = h64_i32(h, w->spray_angle);
    h = h64_i32(h, w->reload_ticks);
    h = h64_i32(h, w->damage);
    h = h64_i32(h, w->mana_per_shot);
    h = h64_i32(h, w->velocity_pps);
    h = h64_i32(h, w->water_weapon);
    h = h64_i32(h, w->to_air_weapon);
    h = h64_i32(h, w->no_air_weapon);
    h = h64_i32(h, w->no_radar);
    h = h64_u32(h, w->los_kind);
    h = h64_u32(h, w->is_los);
    h = h64_i32(h, w->emit_ticks);
    h = h64_u32(h, w->is_gravity);
    h = h64_u32(h, w->lob_preferred);
    h = h64_u32(h, w->dropped);
    h = h64_f32(h, w->gravity_adjust);
    h = h64_i32(h, w->spin_pitch);
    h = h64_i32(h, w->spin_heading);
    h = h64_i32(h, w->spin_roll);
    int scales = w->damage_scale_count;
    if (scales < 0) scales = 0;
    if (scales > TAK_DAMAGE_CATEGORY_MAX) scales = TAK_DAMAGE_CATEGORY_MAX;
    h = h64_i32(h, scales);
    for (int i = 0; i < scales; i++) {
        h = h64_str(h, w->damage_scales[i].category);
        h = h64_f32(h, w->damage_scales[i].scale);
    }
    return h;
}

static uint64_t hash_unit_def(const UnitDef *d) {
    uint64_t h = DEF_HASH_SEED;
    h = h64_str(h, d->unitname);
    h = h64_str(h, d->side);
    h = h64_str(h, d->category);
    h = h64_str(h, d->damage_category);
    h = h64_str(h, d->movement_class);
    h = h64_str(h, d->corpse);
    h = h64_i32(h, d->corpse_adjust_x);
    h = h64_i32(h, d->corpse_adjust_z);
    h = h64_str(h, d->animate_type);
    h = h64_f32(h, d->mogrium_bounty);
    h = h64_i32(h, d->unitnumber);
    h = h64_f32(h, d->buildtime);
    h = h64_i32(h, d->build_cost);
    h = h64_f32(h, d->worker_time);
    h = h64_i32(h, d->build_distance);
    h = h64_f32(h, d->heal_time);
    h = h64_i32(h, d->kill_xp_value);
    h = h64_i32(h, d->noveteran);
    h = h64_i32(h, d->commander);
    h = h64_i32(h, d->is_feature);
    h = h64_i32(h, d->max_health);
    h = h64_i32(h, d->sight_distance);
    h = h64_i32(h, d->radar_distance);
    h = h64_i32(h, d->can_fly);
    h = h64_i32(h, d->cruise_alt);
    h = h64_i32(h, d->activate_when_built);
    h = h64_i32(h, d->floater);
    h = h64_i32(h, d->waterline);
    h = h64_i32(h, d->transport_size);
    h = h64_i32(h, d->transport_capacity);
    h = h64_i32(h, d->transport_size_capacity);
    h = h64_i32(h, d->cant_be_transported);
    h = h64_i32(h, d->transported_size);
    h = h64_i32(h, d->transport_distance);
    h = h64_i32(h, d->min_water_depth);
    h = h64_i32(h, d->max_water_depth);
    h = h64_i32(h, d->bad_min_water_depth);
    h = h64_i32(h, d->bad_max_water_depth);
    h = h64_i32(h, d->bad_slope);
    h = h64_i32(h, d->max_water_slope);
    h = h64_i32(h, d->bad_water_slope);
    h = h64_f32(h, d->max_velocity);
    h = h64_f32(h, d->acceleration);
    h = h64_f32(h, d->brake_rate);
    h = h64_f32(h, d->turn_rate);
    h = h64_u32(h, d->cap_flags);
    h = h64_i32(h, d->max_mana);
    h = h64_f32(h, d->mana_recharge_per_sec);
    h = h64_i32(h, d->mogrium_storage);
    h = h64_f32(h, d->mogrium_income_per_sec);
    h = h64_i32(h, d->footprint_x);
    h = h64_i32(h, d->footprint_z);
    h = h64_i32(h, d->max_slope);
    h = h64_i32(h, d->bmcode);
    h = h64_i32(h, d->is_gate);
    h = h64_i32(h, d->onoffable);
    h = h64_i32(h, d->yardmap_sacred);
    /* The yardmap decides which cells a building blocks. */
    int cells = d->footprint_x * d->footprint_z;
    if (cells < 0) cells = 0;
    h = h64_i32(h, d->yardmap ? cells : 0);
    if (d->yardmap && cells) h = h64_bytes(h, d->yardmap, (size_t)cells);
    int weapons = d->num_weapons;
    if (weapons < 0) weapons = 0;
    if (weapons > 3) weapons = 3;
    h = h64_i32(h, weapons);
    for (int i = 0; i < weapons; i++) h = hash_weapon(h, &d->weapons[i]);
    return h;
}

static uint64_t hash_feature_def(const FeatureDef *f) {
    uint64_t h = DEF_HASH_SEED;
    h = h64_str(h, f->name);
    h = h64_str(h, f->world);
    h = h64_str(h, f->category);
    h = h64_str(h, f->feature_dead);
    h = h64_i32(h, f->footprint_x);
    h = h64_i32(h, f->footprint_z);
    h = h64_i32(h, f->height);
    h = h64_i32(h, f->blocking);
    h = h64_i32(h, f->reclaimable);
    h = h64_i32(h, f->indestructible);
    h = h64_i32(h, f->damage);
    h = h64_f32(h, f->sacred_site);
    h = h64_f32(h, f->energy);
    h = h64_i32(h, f->autoreclaimable);
    h = h64_i32(h, f->decompose_time);
    h = h64_i32(h, f->resurrectable);
    h = h64_i32(h, f->animatable);
    h = h64_i32(h, f->is_building);
    return h;
}

/* ── the referenced definition set ────────────────────────────────── */

/* Every definition the live battle can still reach. A dead slot is a
 * tombstone whose definition index is whatever it held when it died,
 * so it is deliberately not followed. */
typedef struct DefRef {
    uint8_t kind;
    int32_t index;
} DefRef;

typedef struct DefSet {
    DefRef *refs;
    int     count;
    int     cap;
} DefSet;

static int defset_add(DefSet *s, uint8_t kind, int32_t index) {
    if (index < 0) return 0;
    for (int i = 0; i < s->count; i++) {
        if (s->refs[i].kind == kind && s->refs[i].index == index) return 0;
    }
    if (s->count == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        DefRef *r = (DefRef *)tak_realloc(s->refs, sizeof(DefRef) * (size_t)cap);
        if (!r) return -1;
        s->refs = r;
        s->cap = cap;
    }
    s->refs[s->count].kind = kind;
    s->refs[s->count].index = index;
    s->count++;
    return 0;
}

static int defset_collect(DefSet *s, const GameWorld *w) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (units && count > 0) {
        for (int i = 0; i < count; i++) {
            const Unit *u = &units[i];
            if (u->alive == UNIT_ALIVE_DEAD) continue;
            if (defset_add(s, TAK_DEF_KIND_UNIT, (int32_t)u->def_idx) != 0) return -1;
            int queued = u->prod_queue_len;
            if (queued > UNIT_PROD_QUEUE_MAX) queued = UNIT_PROD_QUEUE_MAX;
            for (int q = 0; q < queued; q++) {
                if (defset_add(s, TAK_DEF_KIND_UNIT,
                               (int32_t)u->prod_queue[q]) != 0) return -1;
            }
        }
    }
    if (w && w->features) {
        for (int i = 0; i < w->feature_count; i++) {
            if (defset_add(s, TAK_DEF_KIND_FEATURE,
                           w->features[i].global_idx) != 0) return -1;
        }
    }
    return 0;
}

/* The name a definition is written under. Names travel, indices do
 * not: a loose file install, a mod or a changed data set produces a
 * different registry order. */
static const char *defref_name(const DefRef *r) {
    if (r->kind == TAK_DEF_KIND_UNIT) {
        const UnitDef *d = Units_GetDef(r->index);
        return d ? d->unitname : NULL;
    }
    const FeatureDef *f = Features_GetByIndex(r->index);
    return f ? f->name : NULL;
}

static int defref_hash(const DefRef *r, uint64_t *out) {
    if (r->kind == TAK_DEF_KIND_UNIT) {
        const UnitDef *d = Units_GetDef(r->index);
        if (!d) return -1;
        *out = hash_unit_def(d);
        return 0;
    }
    const FeatureDef *f = Features_GetByIndex(r->index);
    if (!f) return -1;
    *out = hash_feature_def(f);
    return 0;
}

/* ── the open save ────────────────────────────────────────────────── */

struct TAK_SaveGame {
    TAK_SaveReader *reader;
    TAK_SaveInfo    info;
    /* One entry per DEFS record: the index this installation gives
     * that name, and which registry it came from. Filled by
     * check_defs, which has already refused any name that moved. */
    int32_t        *def_index;
    uint8_t        *def_kind;
    uint32_t        def_count;
};

/* The live registry index behind a DEFS ordinal. -1 when the ordinal
 * is out of range or names the other registry. */
static int32_t save_def_index(const TAK_SaveGame *sg, int32_t ordinal,
                              uint8_t kind) {
    if (!sg || !sg->def_index || ordinal < 0 ||
        (uint32_t)ordinal >= sg->def_count) {
        return -1;
    }
    if (sg->def_kind[ordinal] != kind) return -1;
    return sg->def_index[ordinal];
}

/* ── the battle ───────────────────────────────────────────────────
 *
 * Everything the simulation owns, which is exactly what
 * include/tak_sim_hash.h enumerates. Two things are deliberately
 * outside: state a load rebuilds (the occupancy layer, the unit
 * spatial grid, the influence maps, the path plan cache, a COB
 * engine's piece to node table and its host callbacks) and state that
 * is drawing rather than simulation (the selection, the control
 * groups, the draw order, the projectile art and explosion cache
 * slots, the one shot impact sprites). The hash is the line: if it
 * covers a field, the file carries it. */

/* A growable byte buffer for the sections whose length depends on the
 * battle. Every value still goes in at an explicit width. */
typedef struct Buf {
    uint8_t *p;
    size_t   len;
    size_t   cap;
    int      failed;
} Buf;

static uint8_t *buf_claim(Buf *b, size_t n) {
    if (b->failed) return NULL;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t *q = (uint8_t *)tak_realloc(b->p, cap);
        if (!q) { b->failed = 1; return NULL; }
        b->p = q;
        b->cap = cap;
    }
    uint8_t *at = b->p + b->len;
    memset(at, 0, n);
    b->len += n;
    return at;
}

static void buf_free(Buf *b) { tak_free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* A cursor over a section payload that refuses to read past the end. */
typedef struct Cur {
    const uint8_t *p;
    size_t         len;
    size_t         at;
    int            overrun;
} Cur;

static const uint8_t *cur_take(Cur *c, size_t n) {
    if (c->overrun || c->at + n > c->len) { c->overrun = 1; return NULL; }
    const uint8_t *at = c->p + c->at;
    c->at += n;
    return at;
}

/* ── definition references ────────────────────────────────────────── */

/* On the way out, a definition index becomes the ordinal of its DEFS
 * record. On the way back, that ordinal becomes whatever index this
 * installation gives the same name. */
typedef struct DefOrdinals {
    int32_t *unit;
    int      unit_count;
    int32_t *feature;
    int      feature_count;
} DefOrdinals;

static void defords_free(DefOrdinals *o) {
    tak_free(o->unit);
    tak_free(o->feature);
    memset(o, 0, sizeof(*o));
}

static int defords_init(DefOrdinals *o) {
    memset(o, 0, sizeof(*o));
    o->unit_count = Units_GetDefCount();
    o->feature_count = Features_GetCount();
    if (o->unit_count < 0) o->unit_count = 0;
    if (o->feature_count < 0) o->feature_count = 0;
    if (o->unit_count) {
        o->unit = (int32_t *)tak_malloc(sizeof(int32_t) * (size_t)o->unit_count);
        if (!o->unit) return -1;
        for (int i = 0; i < o->unit_count; i++) o->unit[i] = -1;
    }
    if (o->feature_count) {
        o->feature = (int32_t *)tak_malloc(sizeof(int32_t) *
                                           (size_t)o->feature_count);
        if (!o->feature) { defords_free(o); return -1; }
        for (int i = 0; i < o->feature_count; i++) o->feature[i] = -1;
    }
    return 0;
}

static void defords_set(DefOrdinals *o, uint8_t kind, int32_t index,
                        int32_t ordinal) {
    if (kind == TAK_DEF_KIND_UNIT) {
        if (index >= 0 && index < o->unit_count) o->unit[index] = ordinal;
    } else if (index >= 0 && index < o->feature_count) {
        o->feature[index] = ordinal;
    }
}

static int32_t defords_get(const DefOrdinals *o, uint8_t kind, int32_t index) {
    if (kind == TAK_DEF_KIND_UNIT) {
        return (index >= 0 && index < o->unit_count) ? o->unit[index] : -1;
    }
    return (index >= 0 && index < o->feature_count) ? o->feature[index] : -1;
}

/* ── units ────────────────────────────────────────────────────────── */

static void encode_unit(uint8_t *r, const Unit *u, const DefOrdinals *o) {
    memset(r, 0, TAK_UNIT_RECORD_BYTES);
    tak_put_u8(r + U_ALIVE, u->alive);
    tak_put_u32(r + U_STABLE_ID, u->stable_id);
    /* A dead slot is a tombstone. Its remaining fields are whatever it
     * held when the unit died and nothing may read them again. */
    if (u->alive == UNIT_ALIVE_DEAD) return;

    tak_put_i32(r + U_WORLD_X, u->world_x);
    tak_put_i32(r + U_WORLD_Y, u->world_y);
    tak_put_f32(r + U_HEADING, u->heading);
    tak_put_f32(r + U_PITCH, u->pitch);
    tak_put_f32(r + U_ROLL, u->roll);
    tak_put_i32(r + U_VELOCITY, u->velocity);
    tak_put_i32(r + U_HEALTH, u->health);
    tak_put_i32(r + U_MAX_HEALTH, u->max_health);
    tak_put_i32(r + U_CMD_X, u->cmd_x);
    tak_put_i32(r + U_CMD_Y, u->cmd_y);
    tak_put_i32(r + U_PATROL_X, u->patrol_x);
    tak_put_i32(r + U_PATROL_Y, u->patrol_y);
    tak_put_i32(r + U_EXPERIENCE, u->experience_pts);
    tak_put_f32(r + U_RECLAIM_ACCUM, u->reclaim_accum);
    tak_put_i32(r + U_RAISE_LEFT, u->raise_left);
    tak_put_i32(r + U_UNLOAD_GX, u->unload_gx);
    tak_put_i32(r + U_UNLOAD_GY, u->unload_gy);
    tak_put_f32(r + U_FLIGHT_ALT, u->flight_alt);
    tak_put_f32(r + U_MANA, u->mana);
    tak_put_f32(r + U_MANA_MAX, u->mana_max);
    tak_put_f32(r + U_BUILD_HP, u->build_hp_accum);
    tak_put_f32(r + U_SUBPIXEL_X, u->subpixel_x);
    tak_put_f32(r + U_SUBPIXEL_Y, u->subpixel_y);
    tak_put_f32(r + U_CUR_SPEED, u->cur_speed_ppt);
    tak_put_i32(r + U_PATH_GOAL_X, u->path_goal_x);
    tak_put_i32(r + U_PATH_GOAL_Y, u->path_goal_y);
    tak_put_i32(r + U_WP_BEST_D2, u->wp_best_d2);
    tak_put_i32(r + U_STALL_PX, u->stall_px);
    tak_put_i32(r + U_STALL_PY, u->stall_py);
    tak_put_i32(r + U_STALL_RT_LEFT, u->stall_route_left);
    tak_put_i32(r + U_STALL_RT_BEST, u->stall_route_best);
    tak_put_i32(r + U_STALL_RT_MARK, u->stall_route_mark);
    tak_put_i32(r + U_STALL_LN_BEST, u->stall_line_best);
    tak_put_i32(r + U_STALL_LN_MARK, u->stall_line_mark);
    tak_put_i32(r + U_STALL_TAIL, u->stall_tail);
    tak_put_i32(r + U_STALL_ORD_X, u->stall_order_x);
    tak_put_i32(r + U_STALL_ORD_Y, u->stall_order_y);
    tak_put_i32(r + U_ROUTE_SEG_X, u->route_seg_x);
    tak_put_i32(r + U_ROUTE_SEG_Y, u->route_seg_y);
    tak_put_i32(r + U_RALLY_X, u->rally_x);
    tak_put_i32(r + U_RALLY_Y, u->rally_y);
    tak_put_i32(r + U_DEF_REF,
                defords_get(o, TAK_DEF_KIND_UNIT, (int32_t)u->def_idx));

    tak_put_i16(r + U_TARGET, u->target);
    tak_put_i16(r + U_CMD_KIND, u->cmd_kind);
    tak_put_i16(r + U_ATTACK_CD, u->attack_cooldown);
    tak_put_u16(r + U_KILLS, u->kills);
    tak_put_i16(r + U_BUILD_TARGET, u->build_target);
    tak_put_i16(r + U_RECLAIM_TX, u->reclaim_tile_x);
    tak_put_i16(r + U_RECLAIM_TY, u->reclaim_tile_y);
    tak_put_i16(r + U_CARRIED_BY, u->carried_by);
    tak_put_i16(r + U_CARGO_COUNT, u->cargo_count);
    tak_put_i16(r + U_CARGO_SIZE, u->cargo_size_used);
    tak_put_i16(r + U_XFER_CARGO, u->xfer_cargo);
    tak_put_u16(r + U_CARRY_SEQ, u->carry_seq);
    tak_put_u16(r + U_CARRY_NEXT, u->carry_next);
    tak_put_i16(r + U_GATE_SCAN_CD, u->gate_scan_cd);
    tak_put_i16(r + U_GATE_HOLD, u->gate_hold);
    tak_put_i16(r + U_OCC_TX, u->occ_tx);
    tak_put_i16(r + U_OCC_TY, u->occ_ty);
    tak_put_i16(r + U_NANO_IDLE, u->nano_idle_ticks);
    tak_put_i16(r + U_WP_STALL, u->wp_stall);
    tak_put_i16(r + U_PATH_REPLAN, u->path_replan_cd);
    tak_put_u16(r + U_ROUTE_SERIAL, u->route_serial);
    tak_put_u16(r + U_STALL_TSERIAL, u->stall_tail_serial);
    tak_put_i16(r + U_STALL_TICKS, u->stall_ticks);
    tak_put_i16(r + U_ROUTE_CHECK, u->route_check_cd);
    tak_put_u16(r + U_STILL_TICKS, u->still_ticks);
    for (int i = 0; i < UNIT_SCRIPT_EV_COUNT; i++) {
        tak_put_u16(r + U_SCRIPT_EV + (size_t)i * 2u, u->script_ev[i]);
    }

    tak_put_u8(r + U_PLAYER_ID, u->player_id);
    tak_put_u8(r + U_COLOR_IDX, u->team_color_idx);
    tak_put_u8(r + U_AGGRO, u->aggro_mode);
    tak_put_u8(r + U_WEAPON_SLOT, u->weapon_slot);
    tak_put_u8(r + U_CORPSE_TYPE, u->corpse_type);
    tak_put_u8(r + U_RAISE_MODE, u->raise_mode);
    tak_put_u8(r + U_LOADQ_LEN, u->load_queue_len);
    tak_put_u8(r + U_XFER_TICKS, u->xfer_ticks);
    tak_put_u8(r + U_XFER_WAIT, u->xfer_wait);
    tak_put_u8(r + U_UNLOAD_STAGE, u->unload_stage);
    tak_put_u8(r + U_UNLOAD_DELAY, u->unload_delay);
    tak_put_u8(r + U_UNLOAD_HOLD, u->unload_hold);
    tak_put_u8(r + U_UNLOAD_TRIES, u->unload_tries);
    tak_put_u8(r + U_UNLOAD_RESTS, u->unload_rests);
    tak_put_u8(r + U_UNLOAD_APPR, u->unload_approach);
    tak_put_u8(r + U_UNDER_CONSTR, u->under_construction);
    tak_put_u8(r + U_COB_ACT, u->cob_activation);
    tak_put_u8(r + U_COB_STANCE, u->cob_build_stance);
    tak_put_u8(r + U_COB_YARD, u->cob_yard_open);
    tak_put_u8(r + U_COB_BUGGER, u->cob_bugger_off);
    tak_put_u8(r + U_FLYING, u->flying);
    tak_put_u8(r + U_SFX_OCCUPY, u->sfx_occupy);
    tak_put_u8(r + U_ATTACK_EXPL, u->attack_explicit);
    tak_put_u8(r + U_OCC_ON, u->occ_on);
    tak_put_u8(r + U_OCC_PENDING, u->occ_pending);
    tak_put_u8(r + U_OCC_FX, u->occ_fx);
    tak_put_u8(r + U_OCC_FZ, u->occ_fz);
    tak_put_u8(r + U_PATH_LEN, u->path_len);
    tak_put_u8(r + U_PATH_INDEX, u->path_index);
    tak_put_u8(r + U_PATH_FAILED, u->path_failed);
    tak_put_u8(r + U_PATH_PENDING, u->path_pending);
    tak_put_u8(r + U_PATH_WAIT, u->path_wait);
    tak_put_u8(r + U_BLOCKED_TICKS, u->blocked_ticks);
    tak_put_u8(r + U_STALL_TINDEX, u->stall_tail_index);
    tak_put_u8(r + U_STALL_ESC, u->stall_esc);
    tak_put_u8(r + U_ROUTE_FLAGS, u->route_flags);
    tak_put_u8(r + U_OCC_PARKED, u->occ_parked);
    tak_put_u8(r + U_ANIM_STATE, u->anim_state);
    tak_put_u8(r + U_WALK_THREAD, (uint8_t)u->walk_thread_slot);
    tak_put_u8(r + U_KILLED_THREAD, (uint8_t)u->killed_thread_slot);
    tak_put_u8(r + U_BUILD_THREAD, (uint8_t)u->build_thread_slot);
    tak_put_u8(r + U_MOVE_TIER, (uint8_t)u->move_rate_tier);
    tak_put_u8(r + U_TURN_SIGN, (uint8_t)u->turn_dir_sign);
    tak_put_u8(r + U_PRODQ_LEN, u->prod_queue_len);
    tak_put_u8(r + U_RALLY_SET, u->rally_set);
    tak_put_u8(r + U_HAS_COB, u->cob ? 1 : 0);

    for (int w = 0; w < 3; w++) {
        uint8_t *ws = r + U_WPN + (size_t)w * U_WPN_BYTES;
        const UnitWeaponState *s = &u->weapon_state[w];
        tak_put_i32(ws + U_WPN_COOLDOWN, s->cooldown_ticks);
        tak_put_i32(ws + U_WPN_BURST_T, s->burst_ticks);
        tak_put_i16(ws + U_WPN_BURST_REM, s->burst_remaining);
        tak_put_i16(ws + U_WPN_BURST_TGT, s->burst_target);
        tak_put_i16(ws + U_WPN_AIM_TICKS, s->aim_ticks);
        tak_put_i16(ws + U_WPN_AIM_TGT, s->aim_target);
        tak_put_u8(ws + U_WPN_AIM_SLOT, (uint8_t)s->aim_thread_slot);
    }

    /* The live prefix only: past the length both queues hold whatever
     * a longer one left there. */
    int loads = u->load_queue_len;
    if (loads > UNIT_LOAD_QUEUE_MAX) loads = UNIT_LOAD_QUEUE_MAX;
    for (int i = 0; i < loads; i++) {
        tak_put_i16(r + U_LOAD_QUEUE + (size_t)i * 2u, u->load_queue[i]);
    }
    /* The one place a definition index appears outside Unit.def_idx.
     * A queue written as raw indices builds someone else on a
     * different installation. */
    int queued = u->prod_queue_len;
    if (queued > UNIT_PROD_QUEUE_MAX) queued = UNIT_PROD_QUEUE_MAX;
    for (int i = 0; i < queued; i++) {
        int32_t ord = defords_get(o, TAK_DEF_KIND_UNIT,
                                  (int32_t)u->prod_queue[i]);
        tak_put_i16(r + U_PROD_QUEUE + (size_t)i * 2u, (int16_t)ord);
    }
}

/* The record this build reads, taken from a file whose record may be
 * shorter or longer. Short reads zero the tail, long ones step over
 * the rest: that tolerance is what keeps an appended field from
 * refusing an older save. */
static void take_record(uint8_t *dst, size_t dst_bytes, const uint8_t *src,
                        uint16_t stored) {
    memset(dst, 0, dst_bytes);
    size_t n = stored < dst_bytes ? (size_t)stored : dst_bytes;
    memcpy(dst, src, n);
}

static int decode_unit(Unit *u, const uint8_t *r, const TAK_SaveGame *sg,
                       char *err, size_t err_cap) {
    memset(u, 0, sizeof(*u));
    u->alive = tak_get_u8(r + U_ALIVE);
    u->stable_id = tak_get_u32(r + U_STABLE_ID);
    if (u->alive == UNIT_ALIVE_DEAD) return 0;

    int32_t def_ref = tak_get_i32(r + U_DEF_REF);
    int32_t def_idx = save_def_index(sg, def_ref, TAK_DEF_KIND_UNIT);
    if (def_idx < 0) {
        set_err(err, err_cap,
                "This save holds a unit whose definition it does not name.");
        return -1;
    }
    u->def_idx = (uint16_t)def_idx;

    u->world_x = tak_get_i32(r + U_WORLD_X);
    u->world_y = tak_get_i32(r + U_WORLD_Y);
    u->heading = tak_get_f32(r + U_HEADING);
    u->pitch = tak_get_f32(r + U_PITCH);
    u->roll = tak_get_f32(r + U_ROLL);
    u->velocity = tak_get_i32(r + U_VELOCITY);
    u->health = tak_get_i32(r + U_HEALTH);
    u->max_health = tak_get_i32(r + U_MAX_HEALTH);
    u->cmd_x = tak_get_i32(r + U_CMD_X);
    u->cmd_y = tak_get_i32(r + U_CMD_Y);
    u->patrol_x = tak_get_i32(r + U_PATROL_X);
    u->patrol_y = tak_get_i32(r + U_PATROL_Y);
    u->experience_pts = tak_get_i32(r + U_EXPERIENCE);
    u->reclaim_accum = tak_get_f32(r + U_RECLAIM_ACCUM);
    u->raise_left = tak_get_i32(r + U_RAISE_LEFT);
    u->unload_gx = tak_get_i32(r + U_UNLOAD_GX);
    u->unload_gy = tak_get_i32(r + U_UNLOAD_GY);
    u->flight_alt = tak_get_f32(r + U_FLIGHT_ALT);
    u->mana = tak_get_f32(r + U_MANA);
    u->mana_max = tak_get_f32(r + U_MANA_MAX);
    u->build_hp_accum = tak_get_f32(r + U_BUILD_HP);
    u->subpixel_x = tak_get_f32(r + U_SUBPIXEL_X);
    u->subpixel_y = tak_get_f32(r + U_SUBPIXEL_Y);
    u->cur_speed_ppt = tak_get_f32(r + U_CUR_SPEED);
    u->path_goal_x = tak_get_i32(r + U_PATH_GOAL_X);
    u->path_goal_y = tak_get_i32(r + U_PATH_GOAL_Y);
    u->wp_best_d2 = tak_get_i32(r + U_WP_BEST_D2);
    u->stall_px = tak_get_i32(r + U_STALL_PX);
    u->stall_py = tak_get_i32(r + U_STALL_PY);
    u->stall_route_left = tak_get_i32(r + U_STALL_RT_LEFT);
    u->stall_route_best = tak_get_i32(r + U_STALL_RT_BEST);
    u->stall_route_mark = tak_get_i32(r + U_STALL_RT_MARK);
    u->stall_line_best = tak_get_i32(r + U_STALL_LN_BEST);
    u->stall_line_mark = tak_get_i32(r + U_STALL_LN_MARK);
    u->stall_tail = tak_get_i32(r + U_STALL_TAIL);
    u->stall_order_x = tak_get_i32(r + U_STALL_ORD_X);
    u->stall_order_y = tak_get_i32(r + U_STALL_ORD_Y);
    u->route_seg_x = tak_get_i32(r + U_ROUTE_SEG_X);
    u->route_seg_y = tak_get_i32(r + U_ROUTE_SEG_Y);
    u->rally_x = tak_get_i32(r + U_RALLY_X);
    u->rally_y = tak_get_i32(r + U_RALLY_Y);

    u->target = tak_get_i16(r + U_TARGET);
    u->cmd_kind = tak_get_i16(r + U_CMD_KIND);
    u->attack_cooldown = tak_get_i16(r + U_ATTACK_CD);
    u->kills = tak_get_u16(r + U_KILLS);
    u->build_target = tak_get_i16(r + U_BUILD_TARGET);
    u->reclaim_tile_x = tak_get_i16(r + U_RECLAIM_TX);
    u->reclaim_tile_y = tak_get_i16(r + U_RECLAIM_TY);
    u->carried_by = tak_get_i16(r + U_CARRIED_BY);
    u->cargo_count = tak_get_i16(r + U_CARGO_COUNT);
    u->cargo_size_used = tak_get_i16(r + U_CARGO_SIZE);
    u->xfer_cargo = tak_get_i16(r + U_XFER_CARGO);
    u->carry_seq = tak_get_u16(r + U_CARRY_SEQ);
    u->carry_next = tak_get_u16(r + U_CARRY_NEXT);
    u->gate_scan_cd = tak_get_i16(r + U_GATE_SCAN_CD);
    u->gate_hold = tak_get_i16(r + U_GATE_HOLD);
    u->occ_tx = tak_get_i16(r + U_OCC_TX);
    u->occ_ty = tak_get_i16(r + U_OCC_TY);
    u->nano_idle_ticks = tak_get_i16(r + U_NANO_IDLE);
    u->wp_stall = tak_get_i16(r + U_WP_STALL);
    u->path_replan_cd = tak_get_i16(r + U_PATH_REPLAN);
    u->route_serial = tak_get_u16(r + U_ROUTE_SERIAL);
    u->stall_tail_serial = tak_get_u16(r + U_STALL_TSERIAL);
    u->stall_ticks = tak_get_i16(r + U_STALL_TICKS);
    u->route_check_cd = tak_get_i16(r + U_ROUTE_CHECK);
    u->still_ticks = tak_get_u16(r + U_STILL_TICKS);
    for (int i = 0; i < UNIT_SCRIPT_EV_COUNT; i++) {
        u->script_ev[i] = tak_get_u16(r + U_SCRIPT_EV + (size_t)i * 2u);
    }

    u->player_id = tak_get_u8(r + U_PLAYER_ID);
    u->team_color_idx = tak_get_u8(r + U_COLOR_IDX);
    u->aggro_mode = tak_get_u8(r + U_AGGRO);
    u->weapon_slot = tak_get_u8(r + U_WEAPON_SLOT);
    u->corpse_type = tak_get_u8(r + U_CORPSE_TYPE);
    u->raise_mode = tak_get_u8(r + U_RAISE_MODE);
    u->load_queue_len = tak_get_u8(r + U_LOADQ_LEN);
    u->xfer_ticks = tak_get_u8(r + U_XFER_TICKS);
    u->xfer_wait = tak_get_u8(r + U_XFER_WAIT);
    u->unload_stage = tak_get_u8(r + U_UNLOAD_STAGE);
    u->unload_delay = tak_get_u8(r + U_UNLOAD_DELAY);
    u->unload_hold = tak_get_u8(r + U_UNLOAD_HOLD);
    u->unload_tries = tak_get_u8(r + U_UNLOAD_TRIES);
    u->unload_rests = tak_get_u8(r + U_UNLOAD_RESTS);
    u->unload_approach = tak_get_u8(r + U_UNLOAD_APPR);
    u->under_construction = tak_get_u8(r + U_UNDER_CONSTR);
    u->cob_activation = tak_get_u8(r + U_COB_ACT);
    u->cob_build_stance = tak_get_u8(r + U_COB_STANCE);
    u->cob_yard_open = tak_get_u8(r + U_COB_YARD);
    u->cob_bugger_off = tak_get_u8(r + U_COB_BUGGER);
    u->flying = tak_get_u8(r + U_FLYING);
    u->sfx_occupy = tak_get_u8(r + U_SFX_OCCUPY);
    u->attack_explicit = tak_get_u8(r + U_ATTACK_EXPL);
    u->occ_on = tak_get_u8(r + U_OCC_ON);
    u->occ_pending = tak_get_u8(r + U_OCC_PENDING);
    u->occ_fx = tak_get_u8(r + U_OCC_FX);
    u->occ_fz = tak_get_u8(r + U_OCC_FZ);
    u->path_len = tak_get_u8(r + U_PATH_LEN);
    u->path_index = tak_get_u8(r + U_PATH_INDEX);
    u->path_failed = tak_get_u8(r + U_PATH_FAILED);
    u->path_pending = tak_get_u8(r + U_PATH_PENDING);
    u->path_wait = tak_get_u8(r + U_PATH_WAIT);
    u->blocked_ticks = tak_get_u8(r + U_BLOCKED_TICKS);
    u->stall_tail_index = tak_get_u8(r + U_STALL_TINDEX);
    u->stall_esc = tak_get_u8(r + U_STALL_ESC);
    u->route_flags = tak_get_u8(r + U_ROUTE_FLAGS);
    u->occ_parked = tak_get_u8(r + U_OCC_PARKED);
    u->anim_state = tak_get_u8(r + U_ANIM_STATE);
    u->walk_thread_slot = (int8_t)tak_get_u8(r + U_WALK_THREAD);
    u->killed_thread_slot = (int8_t)tak_get_u8(r + U_KILLED_THREAD);
    u->build_thread_slot = (int8_t)tak_get_u8(r + U_BUILD_THREAD);
    u->move_rate_tier = (int8_t)tak_get_u8(r + U_MOVE_TIER);
    u->turn_dir_sign = (int8_t)tak_get_u8(r + U_TURN_SIGN);
    u->prod_queue_len = tak_get_u8(r + U_PRODQ_LEN);
    u->rally_set = tak_get_u8(r + U_RALLY_SET);

    for (int w = 0; w < 3; w++) {
        const uint8_t *ws = r + U_WPN + (size_t)w * U_WPN_BYTES;
        UnitWeaponState *s = &u->weapon_state[w];
        s->cooldown_ticks = tak_get_i32(ws + U_WPN_COOLDOWN);
        s->burst_ticks = tak_get_i32(ws + U_WPN_BURST_T);
        s->burst_remaining = tak_get_i16(ws + U_WPN_BURST_REM);
        s->burst_target = tak_get_i16(ws + U_WPN_BURST_TGT);
        s->aim_ticks = tak_get_i16(ws + U_WPN_AIM_TICKS);
        s->aim_target = tak_get_i16(ws + U_WPN_AIM_TGT);
        s->aim_thread_slot = (int8_t)tak_get_u8(ws + U_WPN_AIM_SLOT);
    }

    if (u->load_queue_len > UNIT_LOAD_QUEUE_MAX) {
        u->load_queue_len = UNIT_LOAD_QUEUE_MAX;
    }
    for (int i = 0; i < u->load_queue_len; i++) {
        u->load_queue[i] = tak_get_i16(r + U_LOAD_QUEUE + (size_t)i * 2u);
    }
    if (u->prod_queue_len > UNIT_PROD_QUEUE_MAX) {
        u->prod_queue_len = UNIT_PROD_QUEUE_MAX;
    }
    for (int i = 0; i < u->prod_queue_len; i++) {
        int32_t ord = tak_get_i16(r + U_PROD_QUEUE + (size_t)i * 2u);
        int32_t idx = save_def_index(sg, ord, TAK_DEF_KIND_UNIT);
        if (idx < 0) {
            set_err(err, err_cap,
                    "This save holds a build queue naming a unit it does "
                    "not carry.");
            return -1;
        }
        u->prod_queue[i] = (int16_t)idx;
    }
    return 0;
}

/* ── unit paths ───────────────────────────────────────────────────── */

/* The live prefix of every route, in its own section: the dead tail of
 * a path is whatever a longer previous plan left there, and a fixed
 * record would carry ninety six waypoints for a unit walking three. */
static void encode_paths(Buf *b, const Unit *units, int count) {
    uint8_t *head = buf_claim(b, 4);
    uint32_t written = 0;
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        if (u->alive == UNIT_ALIVE_DEAD) continue;
        int len = u->path_len;
        if (len > UNIT_PATH_MAX_WAYPOINTS) len = UNIT_PATH_MAX_WAYPOINTS;
        if (len <= 0) continue;
        uint8_t *e = buf_claim(b, 4 + (size_t)len * 8u);
        if (!e) return;
        tak_put_u16(e + 0, (uint16_t)i);
        tak_put_u8(e + 2, (uint8_t)len);
        for (int k = 0; k < len; k++) {
            tak_put_i32(e + 4 + (size_t)k * 8u, u->path_x[k]);
            tak_put_i32(e + 8 + (size_t)k * 8u, u->path_y[k]);
        }
        written++;
    }
    if (head) tak_put_u32(head, written);
}

static int apply_paths(Cur *c, int slot_count, char *err, size_t err_cap) {
    const uint8_t *head = cur_take(c, 4);
    if (!head) {
        set_err(err, err_cap, "This save has a damaged route list.");
        return -1;
    }
    uint32_t n = tak_get_u32(head);
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *h = cur_take(c, 4);
        if (!h) {
            set_err(err, err_cap, "This save has a damaged route list.");
            return -1;
        }
        int slot = (int)tak_get_u16(h + 0);
        int len = (int)tak_get_u8(h + 2);
        const uint8_t *pts = cur_take(c, (size_t)len * 8u);
        if (!pts || len > UNIT_PATH_MAX_WAYPOINTS || slot < 0 ||
            slot >= slot_count) {
            set_err(err, err_cap, "This save has a damaged route list.");
            return -1;
        }
        Unit *u = Units_LoadSlot(slot);
        if (!u) continue;
        for (int k = 0; k < len; k++) {
            u->path_x[k] = tak_get_i32(pts + (size_t)k * 8u);
            u->path_y[k] = tak_get_i32(pts + 4 + (size_t)k * 8u);
        }
    }
    return 0;
}

/* ── unit scripts ─────────────────────────────────────────────────── */

/* Pieces, per instance statics and all sixteen thread slots, dead ones
 * included: src/render/units.c reads a weapon aim result off a slot
 * Cob_IsThreadAlive has already called dead, so a zeroed dead slot
 * makes a unit whose script said hold fire shoot instead. The program
 * counter is a word index into the shared script, which is why the
 * definition fingerprint has to hold for the file to mean anything. */
static void encode_cob(Buf *b, const Unit *units, int count) {
    uint8_t *head = buf_claim(b, 4);
    uint32_t written = 0;
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        if (u->alive == UNIT_ALIVE_DEAD || !u->cob) continue;
        const CobEngine *e = u->cob;
        int pieces = e->piece_count > 0 ? e->piece_count : 0;
        uint32_t statics = e->script ? e->script->num_static_vars : 0u;
        if (!e->pieces) pieces = 0;
        if (!e->static_vars) statics = 0;
        uint8_t *h = buf_claim(b, 8);
        if (!h) return;
        tak_put_u16(h + 0, (uint16_t)i);
        tak_put_u16(h + 2, (uint16_t)pieces);
        tak_put_u32(h + 4, statics);

        for (int k = 0; k < pieces; k++) {
            uint8_t *pb = buf_claim(b, TAK_COB_PIECE_BYTES);
            if (!pb) return;
            const CobPiece *pc = &e->pieces[k];
            for (int a = 0; a < 3; a++) {
                size_t o = (size_t)a * 4u;
                tak_put_i32(pb + CP_ROT + o, pc->rot[a]);
                tak_put_i32(pb + CP_ROT_TARGET + o, pc->rot_target[a]);
                tak_put_i32(pb + CP_ROT_SPEED + o, pc->rot_speed[a]);
                tak_put_i32(pb + CP_SPIN_SPEED + o, pc->spin_speed[a]);
                tak_put_i32(pb + CP_SPIN_ACCEL + o, pc->spin_accel[a]);
                tak_put_i32(pb + CP_POS + o, pc->pos[a]);
                tak_put_i32(pb + CP_POS_TARGET + o, pc->pos_target[a]);
                tak_put_i32(pb + CP_POS_SPEED + o, pc->pos_speed[a]);
            }
            tak_put_u8(pb + CP_HIDDEN, pc->hidden);
            tak_put_u8(pb + CP_EXPLODED, pc->exploded);
            tak_put_u8(pb + CP_SHADOW_OFF, pc->shadow_off);
            tak_put_u8(pb + CP_SHADE_OFF, pc->shade_off);
        }
        for (uint32_t k = 0; k < statics; k++) {
            uint8_t *sv = buf_claim(b, 4);
            if (!sv) return;
            tak_put_i32(sv, e->static_vars[k]);
        }
        for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
            const CobThread *th = &e->threads[t];
            int depth = th->sp;
            if (depth < 0) depth = 0;
            if (depth > COB_THREAD_STACK_DEPTH) depth = COB_THREAD_STACK_DEPTH;
            uint8_t *tb = buf_claim(b, TAK_COB_THREAD_BYTES +
                                       (size_t)depth * 4u);
            if (!tb) return;
            tak_put_u32(tb + CT_PC, th->pc);
            tak_put_i32(tb + CT_SLEEP, th->sleep_remaining);
            tak_put_i32(tb + CT_RETURN_VAL, th->return_value);
            tak_put_u32(tb + CT_SIGNAL_MASK, th->signal_mask);
            tak_put_i16(tb + CT_WAIT_PIECE, th->wait_piece);
            tak_put_u8(tb + CT_WAIT_CHILD, (uint8_t)th->wait_child);
            tak_put_u8(tb + CT_WAIT_AXIS, th->wait_axis);
            tak_put_u8(tb + CT_WAIT_KIND, th->wait_kind);
            tak_put_u8(tb + CT_SP, (uint8_t)depth);
            tak_put_u8(tb + CT_ALIVE, th->alive);
            tak_put_u8(tb + CT_HAS_RETURN, th->has_return_value);
            for (int k = 0; k < depth; k++) {
                tak_put_i32(tb + TAK_COB_THREAD_BYTES + (size_t)k * 4u,
                            th->stack[k]);
            }
        }
        written++;
    }
    if (head) tak_put_u32(head, written);
}

static int apply_cob(Cur *c, int slot_count, char *err, size_t err_cap) {
    const uint8_t *head = cur_take(c, 4);
    if (!head) {
        set_err(err, err_cap, "This save has damaged script state.");
        return -1;
    }
    uint32_t n = tak_get_u32(head);
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *h = cur_take(c, 8);
        if (!h) {
            set_err(err, err_cap, "This save has damaged script state.");
            return -1;
        }
        int slot = (int)tak_get_u16(h + 0);
        int pieces = (int)tak_get_u16(h + 2);
        uint32_t statics = tak_get_u32(h + 4);
        Unit *u = (slot >= 0 && slot < slot_count) ? Units_LoadSlot(slot) : NULL;
        if (!u) {
            set_err(err, err_cap, "This save has damaged script state.");
            return -1;
        }
        CobEngine *eng = u->cob;

        for (int k = 0; k < pieces; k++) {
            const uint8_t *pb = cur_take(c, TAK_COB_PIECE_BYTES);
            if (!pb) {
                set_err(err, err_cap, "This save has damaged script state.");
                return -1;
            }
            if (!eng || !eng->pieces || k >= eng->piece_count) continue;
            CobPiece *pc = &eng->pieces[k];
            for (int a = 0; a < 3; a++) {
                size_t o = (size_t)a * 4u;
                pc->rot[a] = tak_get_i32(pb + CP_ROT + o);
                pc->rot_target[a] = tak_get_i32(pb + CP_ROT_TARGET + o);
                pc->rot_speed[a] = tak_get_i32(pb + CP_ROT_SPEED + o);
                pc->spin_speed[a] = tak_get_i32(pb + CP_SPIN_SPEED + o);
                pc->spin_accel[a] = tak_get_i32(pb + CP_SPIN_ACCEL + o);
                pc->pos[a] = tak_get_i32(pb + CP_POS + o);
                pc->pos_target[a] = tak_get_i32(pb + CP_POS_TARGET + o);
                pc->pos_speed[a] = tak_get_i32(pb + CP_POS_SPEED + o);
            }
            pc->hidden = tak_get_u8(pb + CP_HIDDEN);
            pc->exploded = tak_get_u8(pb + CP_EXPLODED);
            pc->shadow_off = tak_get_u8(pb + CP_SHADOW_OFF);
            pc->shade_off = tak_get_u8(pb + CP_SHADE_OFF);
        }
        /* The piece array is sized by the mesh, and the definition
         * fingerprint leaves art out on purpose so a re-skin does not
         * refuse a save. A re-skin with a different node count is the
         * one case that has to be caught here. */
        if (eng && pieces != eng->piece_count) {
            const UnitDef *d = Units_GetDef(u->def_idx);
            set_err(err, err_cap,
                    "The model for \"%s\" has a different number of pieces "
                    "than when this save was written.",
                    d && d->unitname[0] ? d->unitname : "a unit");
            return -1;
        }
        for (uint32_t k = 0; k < statics; k++) {
            const uint8_t *sv = cur_take(c, 4);
            if (!sv) {
                set_err(err, err_cap, "This save has damaged script state.");
                return -1;
            }
            if (eng && eng->static_vars && eng->script &&
                k < eng->script->num_static_vars) {
                eng->static_vars[k] = tak_get_i32(sv);
            }
        }
        for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
            const uint8_t *tb = cur_take(c, TAK_COB_THREAD_BYTES);
            if (!tb) {
                set_err(err, err_cap, "This save has damaged script state.");
                return -1;
            }
            int depth = (int)tak_get_u8(tb + CT_SP);
            if (depth > COB_THREAD_STACK_DEPTH) depth = COB_THREAD_STACK_DEPTH;
            const uint8_t *stack = cur_take(c, (size_t)depth * 4u);
            if (!stack) {
                set_err(err, err_cap, "This save has damaged script state.");
                return -1;
            }
            if (!eng) continue;
            CobThread *th = &eng->threads[t];
            memset(th, 0, sizeof(*th));
            th->pc = tak_get_u32(tb + CT_PC);
            th->sleep_remaining = tak_get_i32(tb + CT_SLEEP);
            th->return_value = tak_get_i32(tb + CT_RETURN_VAL);
            th->signal_mask = tak_get_u32(tb + CT_SIGNAL_MASK);
            th->wait_piece = tak_get_i16(tb + CT_WAIT_PIECE);
            th->wait_child = (int8_t)tak_get_u8(tb + CT_WAIT_CHILD);
            th->wait_axis = tak_get_u8(tb + CT_WAIT_AXIS);
            th->wait_kind = tak_get_u8(tb + CT_WAIT_KIND);
            th->sp = (uint8_t)depth;
            th->alive = tak_get_u8(tb + CT_ALIVE);
            th->has_return_value = tak_get_u8(tb + CT_HAS_RETURN);
            for (int k = 0; k < depth; k++) {
                th->stack[k] = tak_get_i32(stack + (size_t)k * 4u);
            }
        }
        Units_LoadSyncThreadCount(slot);
    }
    return 0;
}

/* ── projectiles ──────────────────────────────────────────────────── */

/* Live slots only, each naming its own slot: the pool recycles, unlike
 * the unit array. The shooter and target handles survive a same slot
 * restore untouched, a shooter that died since is a tombstone still
 * sitting at the same index, and the per projectile damage scale copy
 * goes in the file because a projectile whose firer is gone can no
 * longer be asked for it.
 *
 * Left out: the art and explosion cache slots and the beam colours.
 * Those are drawing, they index caches built in first fire order, and
 * the hash does not see them. */
static uint16_t intern_or_none(TAK_StringTable *t, const char *s, int *fail) {
    if (!s || !s[0]) return 0xffffu;
    int idx = StringTable_Intern(t, s);
    if (idx < 0) { *fail = 1; return 0xffffu; }
    return (uint16_t)idx;
}

static const char *string_or_empty(const TAK_StringTable *t, uint16_t idx) {
    if (idx == 0xffffu) return NULL;
    return StringTable_Get(t, (int)idx);
}

static void copy_bounded_name(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static int encode_projectiles(uint8_t *recs, const Projectile *pool, int count,
                              TAK_StringTable *strings) {
    int written = 0;
    int fail = 0;
    for (int i = 0; i < count; i++) {
        const Projectile *p = &pool[i];
        if (!p->alive) continue;
        uint8_t *r = recs + (size_t)written * TAK_PROJ_RECORD_BYTES;
        memset(r, 0, TAK_PROJ_RECORD_BYTES);
        tak_put_u32(r + P_SLOT, (uint32_t)i);
        tak_put_i32(r + P_WORLD_X, p->world_x);
        tak_put_i32(r + P_WORLD_Y, p->world_y);
        tak_put_f32(r + P_SUB_X, p->sub_x);
        tak_put_f32(r + P_SUB_Y, p->sub_y);
        tak_put_f32(r + P_DIR_X, p->dir_x);
        tak_put_f32(r + P_DIR_Y, p->dir_y);
        tak_put_f32(r + P_SPEED, p->speed_ppt);
        tak_put_i32(r + P_DAMAGE, p->damage);
        tak_put_i32(r + P_AOE, p->area_of_effect);
        tak_put_f32(r + P_EDGE, p->edge_effectiveness);
        tak_put_i32(r + P_DEST_X, p->dest_x);
        tak_put_i32(r + P_DEST_Y, p->dest_y);
        tak_put_i32(r + P_SRC_X, p->src_x);
        tak_put_i32(r + P_SRC_Y, p->src_y);
        tak_put_f32(r + P_HEIGHT, p->height);
        tak_put_f32(r + P_VEL_UP, p->vel_up_ppt);
        tak_put_f32(r + P_GRAVITY, p->gravity_ppt2);
        tak_put_f32(r + P_HEADING, p->heading);
        tak_put_f32(r + P_PITCH, p->pitch);
        tak_put_f32(r + P_ROLL, p->roll);
        tak_put_f32(r + P_SPIN_PITCH, p->spin_pitch);
        tak_put_f32(r + P_SPIN_HEADING, p->spin_heading);
        tak_put_f32(r + P_SPIN_ROLL, p->spin_roll);
        tak_put_i32(r + P_SRC_HEIGHT, p->src_height);
        tak_put_i16(r + P_TARGET, p->target);
        tak_put_i16(r + P_SHOOTER, p->shooter);
        tak_put_i16(r + P_TTL, p->ttl_ticks);
        tak_put_u16(r + P_AGE, p->age_ticks);
        tak_put_u8(r + P_PLAYER_ID, p->player_id);
        tak_put_u8(r + P_VISUAL_KIND, p->visual_kind);
        tak_put_u8(r + P_FRIENDLY_FIRE, p->friendly_fire);
        tak_put_u8(r + P_IS_BEAM, p->is_beam);
        tak_put_u8(r + P_COLOR_IDX, p->color_idx);
        int scales = p->damage_scale_count;
        if (scales < 0) scales = 0;
        if (scales > TAK_DAMAGE_CATEGORY_MAX) scales = TAK_DAMAGE_CATEGORY_MAX;
        tak_put_u8(r + P_SCALE_COUNT, (uint8_t)scales);
        tak_put_u16(r + P_HIT_CLASS,
                    intern_or_none(strings, p->hit_sound_class, &fail));
        tak_put_u16(r + P_HIT_SOUND,
                    intern_or_none(strings, p->hit_sound, &fail));
        tak_put_u16(r + P_WATER_SOUND,
                    intern_or_none(strings, p->water_sound, &fail));
        for (int k = 0; k < scales; k++) {
            uint8_t *sc = r + P_SCALES + (size_t)k * P_SCALE_BYTES;
            tak_put_u16(sc + 0,
                        intern_or_none(strings, p->damage_scales[k].category,
                                       &fail));
            tak_put_f32(sc + 2, p->damage_scales[k].scale);
        }
        written++;
    }
    return fail ? -1 : written;
}

static void decode_projectile(Projectile *p, const uint8_t *r,
                              const TAK_StringTable *t) {
    memset(p, 0, sizeof(*p));
    p->alive = 1;
    p->world_x = tak_get_i32(r + P_WORLD_X);
    p->world_y = tak_get_i32(r + P_WORLD_Y);
    p->sub_x = tak_get_f32(r + P_SUB_X);
    p->sub_y = tak_get_f32(r + P_SUB_Y);
    p->dir_x = tak_get_f32(r + P_DIR_X);
    p->dir_y = tak_get_f32(r + P_DIR_Y);
    p->speed_ppt = tak_get_f32(r + P_SPEED);
    p->damage = tak_get_i32(r + P_DAMAGE);
    p->area_of_effect = tak_get_i32(r + P_AOE);
    p->edge_effectiveness = tak_get_f32(r + P_EDGE);
    p->dest_x = tak_get_i32(r + P_DEST_X);
    p->dest_y = tak_get_i32(r + P_DEST_Y);
    p->src_x = tak_get_i32(r + P_SRC_X);
    p->src_y = tak_get_i32(r + P_SRC_Y);
    p->height = tak_get_f32(r + P_HEIGHT);
    p->vel_up_ppt = tak_get_f32(r + P_VEL_UP);
    p->gravity_ppt2 = tak_get_f32(r + P_GRAVITY);
    p->heading = tak_get_f32(r + P_HEADING);
    p->pitch = tak_get_f32(r + P_PITCH);
    p->roll = tak_get_f32(r + P_ROLL);
    p->spin_pitch = tak_get_f32(r + P_SPIN_PITCH);
    p->spin_heading = tak_get_f32(r + P_SPIN_HEADING);
    p->spin_roll = tak_get_f32(r + P_SPIN_ROLL);
    p->src_height = tak_get_i32(r + P_SRC_HEIGHT);
    p->target = tak_get_i16(r + P_TARGET);
    p->shooter = tak_get_i16(r + P_SHOOTER);
    p->ttl_ticks = tak_get_i16(r + P_TTL);
    p->age_ticks = tak_get_u16(r + P_AGE);
    p->player_id = tak_get_u8(r + P_PLAYER_ID);
    p->visual_kind = tak_get_u8(r + P_VISUAL_KIND);
    p->friendly_fire = tak_get_u8(r + P_FRIENDLY_FIRE);
    p->is_beam = tak_get_u8(r + P_IS_BEAM);
    p->color_idx = tak_get_u8(r + P_COLOR_IDX);
    int scales = (int)tak_get_u8(r + P_SCALE_COUNT);
    if (scales > TAK_DAMAGE_CATEGORY_MAX) scales = TAK_DAMAGE_CATEGORY_MAX;
    p->damage_scale_count = scales;
    for (int k = 0; k < scales; k++) {
        const uint8_t *sc = r + P_SCALES + (size_t)k * P_SCALE_BYTES;
        copy_bounded_name(p->damage_scales[k].category,
                          sizeof(p->damage_scales[k].category),
                          string_or_empty(t, tak_get_u16(sc + 0)));
        p->damage_scales[k].scale = tak_get_f32(sc + 2);
    }
    copy_bounded_name(p->hit_sound_class, sizeof(p->hit_sound_class),
                      string_or_empty(t, tak_get_u16(r + P_HIT_CLASS)));
    copy_bounded_name(p->hit_sound, sizeof(p->hit_sound),
                      string_or_empty(t, tak_get_u16(r + P_HIT_SOUND)));
    copy_bounded_name(p->water_sound, sizeof(p->water_sound),
                      string_or_empty(t, tak_get_u16(r + P_WATER_SOUND)));
    /* Drawing, rebuilt rather than restored. The caches these index
     * are filled in first fire order, so the numbers do not travel. */
    p->art_kind = UNIT_WEAPON_ART_NONE;
    p->art_idx = -1;
    p->explosion_idx = -1;
}

/* ── features and corpses ─────────────────────────────────────────── */

static void encode_feature(uint8_t *r, const struct MapFeature *f,
                           const DefOrdinals *o) {
    memset(r, 0, TAK_FEAT_RECORD_BYTES);
    tak_put_u16(r + F_FEAT_ID, f->feat_id);
    tak_put_u16(r + F_TILE_X, f->tile_x);
    tak_put_u16(r + F_TILE_Z, f->tile_z);
    tak_put_u16(r + F_HEADING, f->heading);
    tak_put_u16(r + F_PITCH, f->pitch);
    tak_put_u16(r + F_ROLL, f->roll);
    tak_put_i16(r + F_COLOR_IDX, f->color_idx);
    tak_put_i16(r + F_DEF_REF,
                (int16_t)defords_get(o, TAK_DEF_KIND_FEATURE, f->global_idx));
    tak_put_i32(r + F_WORLD_X, f->world_x);
    tak_put_i32(r + F_WORLD_Y, f->world_y);
    tak_put_i32(r + F_DECOMPOSE, f->decompose_ticks);
    tak_put_i16(r + F_SINK, f->sink_ticks);
}

static void decode_feature(struct MapFeature *f, const uint8_t *r,
                           const TAK_SaveGame *sg) {
    memset(f, 0, sizeof(*f));
    f->feat_id = tak_get_u16(r + F_FEAT_ID);
    f->tile_x = tak_get_u16(r + F_TILE_X);
    f->tile_z = tak_get_u16(r + F_TILE_Z);
    f->heading = tak_get_u16(r + F_HEADING);
    f->pitch = tak_get_u16(r + F_PITCH);
    f->roll = tak_get_u16(r + F_ROLL);
    f->color_idx = tak_get_i16(r + F_COLOR_IDX);
    f->global_idx = save_def_index(sg, tak_get_i16(r + F_DEF_REF),
                                    TAK_DEF_KIND_FEATURE);
    f->world_x = tak_get_i32(r + F_WORLD_X);
    f->world_y = tak_get_i32(r + F_WORLD_Y);
    f->decompose_ticks = tak_get_i32(r + F_DECOMPOSE);
    f->sink_ticks = tak_get_i16(r + F_SINK);
}

/* ── fog ──────────────────────────────────────────────────────────── */

static void encode_fog(Buf *b, const GameWorld *w) {
    size_t cells = (size_t)(w->fog_w > 0 ? w->fog_w : 0) *
                   (size_t)(w->fog_h > 0 ? w->fog_h : 0);
    uint32_t mask = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (w->fog_layers[p]) mask |= 1u << p;
    }
    uint8_t *h = buf_claim(b, TAK_FOGV_HEADER_BYTES);
    if (!h) return;
    tak_put_i32(h + FOG_W, w->fog_w);
    tak_put_i32(h + FOG_H, w->fog_h);
    tak_put_i32(h + FOG_CELL_PX, w->fog_cell_px);
    tak_put_u32(h + FOG_LAYER_MASK, mask);
    if (!cells) return;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!w->fog_layers[p]) continue;
        uint8_t *dst = buf_claim(b, cells);
        if (!dst) return;
        memcpy(dst, w->fog_layers[p], cells);
    }
}

static int apply_fog(Cur *c, GameWorld *w, char *err, size_t err_cap) {
    const uint8_t *h = cur_take(c, TAK_FOGV_HEADER_BYTES);
    if (!h) {
        set_err(err, err_cap, "This save has damaged fog of war.");
        return -1;
    }
    int fw = tak_get_i32(h + FOG_W);
    int fh = tak_get_i32(h + FOG_H);
    int cell = tak_get_i32(h + FOG_CELL_PX);
    uint32_t mask = tak_get_u32(h + FOG_LAYER_MASK);
    if (fw != w->fog_w || fh != w->fog_h || cell != w->fog_cell_px) {
        set_err(err, err_cap,
                "The map \"%s\" on this system is a different size than the "
                "one this save was played on.", w->map_name);
        return -1;
    }
    size_t cells = (size_t)(fw > 0 ? fw : 0) * (size_t)(fh > 0 ? fh : 0);
    if (!cells) return 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!(mask & (1u << p))) continue;
        const uint8_t *src = cur_take(c, cells);
        if (!src) {
            set_err(err, err_cap, "This save has damaged fog of war.");
            return -1;
        }
        if (w->fog_layers[p]) memcpy(w->fog_layers[p], src, cells);
    }
    /* Player one's layer is a compatibility alias, re-pointed rather
     * than stored twice. */
    w->fog_state = w->fog_layers[1];
    return 0;
}

/* ── economy ──────────────────────────────────────────────────────── */

static void encode_econ(uint8_t *p, const EconomyState *eco) {
    memset(p, 0, TAK_ECON_BYTES);
    tak_put_i32(p + EC_ACTIVE, eco->active_count);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        uint8_t *s = p + EC_PLAYERS + (size_t)i * EC_SLOT_BYTES;
        const PlayerEconomy *e = &eco->players[i];
        tak_put_f32(s + EC_MANA, e->mana);
        tak_put_i32(s + EC_MAX_MANA, e->max_mana);
        tak_put_f32(s + EC_REGEN, e->regen_per_sec);
        tak_put_i32(s + EC_SPENT_LAST, e->spent_last_sec);
        tak_put_i32(s + EC_EARNED_LAST, e->earned_last_sec);
        tak_put_f32(s + EC_EARNED_ACC, e->earned_accum);
        tak_put_f32(s + EC_SPENT_ACC, e->spent_accum);
        tak_put_i32(s + EC_WINDOW, e->ticks_since_window_reset);
    }
}

static void apply_econ(const uint8_t *p, EconomyState *eco) {
    eco->active_count = tak_get_i32(p + EC_ACTIVE);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const uint8_t *s = p + EC_PLAYERS + (size_t)i * EC_SLOT_BYTES;
        PlayerEconomy *e = &eco->players[i];
        e->mana = tak_get_f32(s + EC_MANA);
        e->max_mana = tak_get_i32(s + EC_MAX_MANA);
        e->regen_per_sec = tak_get_f32(s + EC_REGEN);
        e->spent_last_sec = tak_get_i32(s + EC_SPENT_LAST);
        e->earned_last_sec = tak_get_i32(s + EC_EARNED_LAST);
        e->earned_accum = tak_get_f32(s + EC_EARNED_ACC);
        e->spent_accum = tak_get_f32(s + EC_SPENT_ACC);
        e->ticks_since_window_reset = tak_get_i32(s + EC_WINDOW);
    }
}

/* ── writing ──────────────────────────────────────────────────────── */

static void encode_cfgb(uint8_t *p, const BattleConfig *cfg) {
    memset(p, 0, TAK_CFGB_BYTES);
    put_text(p + CFGB_MAP_NAME, CFGB_MAP_NAME_CAP, cfg->map_name);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        uint8_t *s = p + CFGB_PLAYERS + (size_t)i * CFGB_SLOT_BYTES;
        const PlayerSlot *ps = &cfg->players[i];
        tak_put_i32(s + 0,  (int32_t)ps->kind);
        tak_put_i32(s + 4,  (int32_t)ps->side);
        tak_put_i32(s + 8,  (int32_t)ps->team);
        tak_put_i32(s + 12, (int32_t)ps->color);
        tak_put_i32(s + 16, (int32_t)ps->ai_difficulty);
        put_text(s + 20, CFGB_SLOT_NAME_CAP, ps->name);
    }
    uint8_t *o = p + CFGB_OPTIONS;
    tak_put_i32(o + 0,  (int32_t)cfg->units_per_side);
    tak_put_i32(o + 4,  (int32_t)cfg->line_of_sight);
    tak_put_i32(o + 8,  (int32_t)cfg->map_revealed);
    tak_put_i32(o + 12, (int32_t)cfg->monarch_expendable);
    tak_put_i32(o + 16, (int32_t)cfg->random_start_locations);
    tak_put_i32(o + 20, (int32_t)cfg->power_codes);
    tak_put_i32(o + 24, (int32_t)cfg->slow_game);
    tak_put_i32(o + 28, (int32_t)cfg->crusades_balance);
}

static void decode_cfgb(const uint8_t *p, BattleConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    get_text(cfg->map_name, sizeof(cfg->map_name), p + CFGB_MAP_NAME,
             CFGB_MAP_NAME_CAP);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const uint8_t *s = p + CFGB_PLAYERS + (size_t)i * CFGB_SLOT_BYTES;
        PlayerSlot *ps = &cfg->players[i];
        ps->kind = (TakSlotKind)tak_get_i32(s + 0);
        ps->side = tak_get_i32(s + 4);
        ps->team = tak_get_i32(s + 8);
        ps->color = tak_get_i32(s + 12);
        ps->ai_difficulty = tak_get_i32(s + 16);
        get_text(ps->name, sizeof(ps->name), s + 20, CFGB_SLOT_NAME_CAP);
    }
    const uint8_t *o = p + CFGB_OPTIONS;
    cfg->units_per_side         = tak_get_i32(o + 0);
    cfg->line_of_sight          = tak_get_i32(o + 4);
    cfg->map_revealed           = tak_get_i32(o + 8);
    cfg->monarch_expendable     = tak_get_i32(o + 12);
    cfg->random_start_locations = tak_get_i32(o + 16);
    cfg->power_codes            = tak_get_i32(o + 20);
    cfg->slow_game              = tak_get_i32(o + 24);
    cfg->crusades_balance       = tak_get_i32(o + 28);
}

static void encode_wrld(uint8_t *p, const GameWorld *w) {
    memset(p, 0, TAK_WRLD_BYTES);
    put_text(p + WRLD_MAP_NAME, WRLD_MAP_NAME_CAP, w->map_name);
    put_text(p + WRLD_KINGDOM, WRLD_KINGDOM_CAP, w->map_kingdom);
    put_text(p + WRLD_END_REASON, WRLD_END_REASON_CAP, w->skirmish_end_reason);

    uint8_t *s = p + WRLD_SCALARS;
    tak_put_i32(s + WS_WATER_HEIGHT  * 4, (int32_t)w->water_height);
    tak_put_i32(s + WS_SK_TICKS      * 4, (int32_t)w->skirmish_elapsed_ticks);
    tak_put_i32(s + WS_SK_GAME_OVER  * 4, (int32_t)w->skirmish_game_over);
    tak_put_i32(s + WS_SK_WINNER     * 4, (int32_t)w->skirmish_winner_team);
    tak_put_i32(s + WS_SK_RESULT     * 4, (int32_t)w->skirmish_local_result);
    tak_put_i32(s + WS_SK_END_TICK   * 4, (int32_t)w->skirmish_end_tick);
    tak_put_i32(s + WS_SK_STATS_OPEN * 4, (int32_t)w->skirmish_stats_open);
    tak_put_i32(s + WS_MI_TICKS      * 4, (int32_t)w->mission_elapsed_ticks);
    tak_put_i32(s + WS_MI_SECONDS    * 4, (int32_t)w->mission_elapsed_seconds);
    tak_put_i32(s + WS_MI_OBJECTIVES * 4, (int32_t)w->mission_objectives_satisfied);
    tak_put_i32(s + WS_MI_VICTORY    * 4, (int32_t)w->mission_victory);
    /* The simulation generator. A load that does not restore it drifts
     * from the first draw on, and seeding cannot put it back. */
    tak_put_u32(s + WS_RAND_STATE    * 4, World_RandState());

    for (int i = 0; i <= TAK_MAX_PLAYERS; i++) {
        uint8_t *t = p + WRLD_STATS + (size_t)i * WRLD_STAT_BYTES;
        const PlayerBattleStats *st = &w->stats[i];
        tak_put_i32(t + 0,  st->units_built);
        tak_put_i32(t + 4,  st->kills);
        tak_put_i32(t + 8,  st->losses);
        tak_put_i32(t + 12, st->score);
        tak_put_i32(t + 16, st->eliminated);
        tak_put_i32(t + 20, st->last_alive_tick);
    }
}

static void apply_wrld(const uint8_t *p, GameWorld *w) {
    get_text(w->map_name, sizeof(w->map_name), p + WRLD_MAP_NAME,
             WRLD_MAP_NAME_CAP);
    get_text(w->map_kingdom, sizeof(w->map_kingdom), p + WRLD_KINGDOM,
             WRLD_KINGDOM_CAP);
    get_text(w->skirmish_end_reason, sizeof(w->skirmish_end_reason),
             p + WRLD_END_REASON, WRLD_END_REASON_CAP);

    const uint8_t *s = p + WRLD_SCALARS;
    w->water_height                 = tak_get_i32(s + WS_WATER_HEIGHT  * 4);
    w->skirmish_elapsed_ticks       = tak_get_i32(s + WS_SK_TICKS      * 4);
    w->skirmish_game_over           = tak_get_i32(s + WS_SK_GAME_OVER  * 4);
    w->skirmish_winner_team         = tak_get_i32(s + WS_SK_WINNER     * 4);
    w->skirmish_local_result        = tak_get_i32(s + WS_SK_RESULT     * 4);
    w->skirmish_end_tick            = tak_get_i32(s + WS_SK_END_TICK   * 4);
    w->skirmish_stats_open          = tak_get_i32(s + WS_SK_STATS_OPEN * 4);
    w->mission_elapsed_ticks        = tak_get_i32(s + WS_MI_TICKS      * 4);
    w->mission_elapsed_seconds      = tak_get_i32(s + WS_MI_SECONDS    * 4);
    w->mission_objectives_satisfied = tak_get_i32(s + WS_MI_OBJECTIVES * 4);
    w->mission_victory              = tak_get_i32(s + WS_MI_VICTORY    * 4);
    World_SetRandState(tak_get_u32(s + WS_RAND_STATE * 4));

    for (int i = 0; i <= TAK_MAX_PLAYERS; i++) {
        const uint8_t *t = p + WRLD_STATS + (size_t)i * WRLD_STAT_BYTES;
        PlayerBattleStats *st = &w->stats[i];
        st->units_built     = tak_get_i32(t + 0);
        st->kills           = tak_get_i32(t + 4);
        st->losses          = tak_get_i32(t + 8);
        st->score           = tak_get_i32(t + 12);
        st->eliminated      = tak_get_i32(t + 16);
        st->last_alive_tick = tak_get_i32(t + 20);
    }
    /* Bumped on purpose, so the pathing clearance cache built against
     * the previous session cannot be believed. */
    w->occ_version++;
}

int Save_Write(const char *path, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!path) {
        set_err(err, err_cap, "No file name was given for the save.");
        return -1;
    }
    GameWorld *w = World_Get();
    if (!w) {
        set_err(err, err_cap, "There is no battle in progress to save.");
        return -1;
    }

    int slots = 0;
    const Unit *units = Units_GetActive(&slots);
    if (slots < 0 || !units) slots = 0;
    int proj_slots = 0;
    const Projectile *projs = Units_GetProjectiles(&proj_slots);
    if (proj_slots < 0 || !projs) proj_slots = 0;

    TAK_SaveHeader hdr;
    Save_HeaderInit(&hdr);
    hdr.schema_version = TAK_SAVE_SCHEMA_VERSION;
    /* A campaign map counts its own clock and carries objectives. */
    int campaign = w->mission.objective_count > 0;
    hdr.save_kind = campaign ? TAK_SAVE_KIND_CAMPAIGN_BATTLE
                             : TAK_SAVE_KIND_SKIRMISH;
    if (campaign) hdr.flags |= TAK_SAVE_F_CAMPAIGN;
    hdr.sim_tick = (uint32_t)(campaign ? w->mission_elapsed_ticks
                                       : w->skirmish_elapsed_ticks);
    hdr.sim_state_hash = TAK_SimHash();
    hdr.unit_slot_count = (uint32_t)slots;
    /* Ids have to stay unique after a load, so the counter travels
     * rather than being restarted from the highest id in the file. */
    hdr.unit_stable_id_next = Units_NextStableId();
    hdr.saved_at_utc = (uint64_t)time(NULL);
    /* Two installs can serve different terrain under one name, so the
     * save records which one it was played on. A map the writer cannot
     * resolve leaves the field zero, which the reader reads as unknown
     * rather than as a mismatch. */
    if (TAK_MapFingerprint_FromName(w->map_name, hdr.map_fingerprint) != 0) {
        memset(hdr.map_fingerprint, 0, sizeof(hdr.map_fingerprint));
    }

    DefSet set = { NULL, 0, 0 };
    DefOrdinals ords;
    if (defset_collect(&set, w) != 0 || defords_init(&ords) != 0) {
        tak_free(set.refs);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    TAK_StringTable *strings = StringTable_New();
    uint8_t *defs = NULL;
    uint8_t *unit_recs = NULL;
    uint8_t *proj_recs = NULL;
    uint8_t *feat_recs = NULL;
    uint8_t *ai_bytes = NULL;
    Buf paths = { NULL, 0, 0, 0 };
    Buf cob = { NULL, 0, 0, 0 };
    Buf fog = { NULL, 0, 0, 0 };
    if (strings) {
        defs = (uint8_t *)tak_malloc((size_t)set.count * TAK_DEFS_RECORD_BYTES + 1);
        unit_recs = (uint8_t *)tak_malloc((size_t)slots * TAK_UNIT_RECORD_BYTES + 1);
        proj_recs = (uint8_t *)tak_malloc((size_t)proj_slots *
                                          TAK_PROJ_RECORD_BYTES + 1);
        feat_recs = (uint8_t *)tak_malloc((size_t)(w->feature_count > 0
                                                   ? w->feature_count : 0) *
                                          TAK_FEAT_RECORD_BYTES + 1);
        ai_bytes = (uint8_t *)tak_malloc(TAK_AI_StateBytes());
    }
    if (!strings || !defs || !unit_recs || !proj_recs || !feat_recs || !ai_bytes) {
        StringTable_Free(strings);
        tak_free(defs);
        tak_free(unit_recs);
        tak_free(proj_recs);
        tak_free(feat_recs);
        tak_free(ai_bytes);
        tak_free(set.refs);
        defords_free(&ords);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    int written = 0;
    int oom = 0;
    for (int i = 0; i < set.count; i++) {
        const char *name = defref_name(&set.refs[i]);
        uint64_t hash = 0;
        if (!name || defref_hash(&set.refs[i], &hash) != 0) continue;
        int idx = StringTable_Intern(strings, name);
        if (idx < 0) { oom = 1; break; }
        uint8_t *rec = defs + (size_t)written * TAK_DEFS_RECORD_BYTES;
        tak_put_u16(rec + DEFS_NAME_IDX, (uint16_t)idx);
        tak_put_u8(rec + DEFS_KIND, set.refs[i].kind);
        tak_put_u8(rec + DEFS_PAD, 0);
        tak_put_u64(rec + DEFS_HASH, hash);
        defords_set(&ords, set.refs[i].kind, set.refs[i].index, written);
        written++;
    }
    tak_free(set.refs);

    int proj_written = 0;
    if (!oom) {
        for (int i = 0; i < slots; i++) {
            encode_unit(unit_recs + (size_t)i * TAK_UNIT_RECORD_BYTES,
                        &units[i], &ords);
        }
        encode_paths(&paths, units, slots);
        encode_cob(&cob, units, slots);
        proj_written = encode_projectiles(proj_recs, projs, proj_slots, strings);
        if (proj_written < 0) oom = 1;
        for (int i = 0; i < w->feature_count && w->features; i++) {
            encode_feature(feat_recs + (size_t)i * TAK_FEAT_RECORD_BYTES,
                           &w->features[i], &ords);
        }
        encode_fog(&fog, w);
        TAK_AI_SaveState(ai_bytes);
        hdr.rng_ai = tak_get_u32(ai_bytes);
    }
    defords_free(&ords);

    size_t strt_len = 0;
    uint8_t *strt = oom ? NULL : StringTable_Serialize(strings, &strt_len);
    StringTable_Free(strings);
    if (oom || !strt || paths.failed || cob.failed || fog.failed) {
        tak_free(strt);
        tak_free(defs);
        tak_free(unit_recs);
        tak_free(proj_recs);
        tak_free(feat_recs);
        tak_free(ai_bytes);
        buf_free(&paths);
        buf_free(&cob);
        buf_free(&fog);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    uint8_t cfgb[TAK_CFGB_BYTES];
    uint8_t wrld[TAK_WRLD_BYTES];
    uint8_t camr[TAK_CAMR_BYTES];
    uint8_t econ[TAK_ECON_BYTES];
    encode_cfgb(cfgb, &w->cfg);
    encode_wrld(wrld, w);
    encode_econ(econ, &w->economy);
    tak_put_i32(camr + CAMR_X, w->cam_x);
    tak_put_i32(camr + CAMR_Y, w->cam_y);

    TAK_SaveWriter *writer = Save_BeginWrite(&hdr);
    int rc = writer ? 0 : -1;
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_STRT, VER_STRT,
                                      TAK_SECT_F_REQUIRED, strt, strt_len);
    if (rc == 0) rc = Save_AddRecords(writer, TAK_SECT_DEFS, VER_DEFS,
                                      TAK_SECT_F_REQUIRED, (uint32_t)written,
                                      TAK_DEFS_RECORD_BYTES, defs);
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_CFGB, VER_CFGB,
                                      TAK_SECT_F_REQUIRED, cfgb, sizeof(cfgb));
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_WRLD, VER_WRLD,
                                      TAK_SECT_F_REQUIRED, wrld, sizeof(wrld));
    /* The battle. Every one of these is required: each carries state
     * the simulation hash covers. */
    if (rc == 0) rc = Save_AddRecords(writer, TAK_SECT_UNIT, VER_UNIT,
                                      TAK_SECT_F_REQUIRED, (uint32_t)slots,
                                      TAK_UNIT_RECORD_BYTES, unit_recs);
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_UPTH, VER_UPTH,
                                      TAK_SECT_F_REQUIRED, paths.p, paths.len);
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_UCOB, VER_UCOB,
                                      TAK_SECT_F_REQUIRED, cob.p, cob.len);
    if (rc == 0) rc = Save_AddRecords(writer, TAK_SECT_PROJ, VER_PROJ,
                                      TAK_SECT_F_REQUIRED,
                                      (uint32_t)proj_written,
                                      TAK_PROJ_RECORD_BYTES, proj_recs);
    if (rc == 0) rc = Save_AddRecords(writer, TAK_SECT_FEAT, VER_FEAT,
                                      TAK_SECT_F_REQUIRED,
                                      (uint32_t)(w->feature_count > 0
                                                 ? w->feature_count : 0),
                                      TAK_FEAT_RECORD_BYTES, feat_recs);
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_FOGV, VER_FOGV,
                                      TAK_SECT_F_REQUIRED, fog.p, fog.len);
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_ECON, VER_ECON,
                                      TAK_SECT_F_REQUIRED, econ, sizeof(econ));
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_AIST, VER_AIST,
                                      TAK_SECT_F_REQUIRED, ai_bytes,
                                      TAK_AI_StateBytes());
    /* The camera is local view state, so an older reader may skip it. */
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_CAMR, VER_CAMR, 0,
                                      camr, sizeof(camr));
    tak_free(strt);
    tak_free(defs);
    tak_free(unit_recs);
    tak_free(proj_recs);
    tak_free(feat_recs);
    tak_free(ai_bytes);
    buf_free(&paths);
    buf_free(&cob);
    buf_free(&fog);
    if (rc != 0) {
        Save_EndWrite(writer);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    rc = Save_FinishToFile(writer, path, err, err_cap);
    Save_EndWrite(writer);
    return rc;
}

/* ── reading ──────────────────────────────────────────────────────── */

/* A save written by a build that could not resolve its map carries all
 * zero, which is unknown rather than a mismatch. */
static int fingerprint_is_absent(const uint8_t fp[TAK_SHA256_BYTES]) {
    for (int i = 0; i < TAK_SHA256_BYTES; i++) {
        if (fp[i] != 0) return 0;
    }
    return 1;
}

static void declare_known(TAK_SaveReader *r) {
    Save_DeclareKnown(r, TAK_SECT_SUMM, VER_SUMM);
    Save_DeclareKnown(r, TAK_SECT_STRT, VER_STRT);
    Save_DeclareKnown(r, TAK_SECT_DEFS, VER_DEFS);
    Save_DeclareKnown(r, TAK_SECT_CFGB, VER_CFGB);
    Save_DeclareKnown(r, TAK_SECT_WRLD, VER_WRLD);
    Save_DeclareKnown(r, TAK_SECT_CAMR, VER_CAMR);
    Save_DeclareKnown(r, TAK_SECT_UNIT, VER_UNIT);
    Save_DeclareKnown(r, TAK_SECT_UPTH, VER_UPTH);
    Save_DeclareKnown(r, TAK_SECT_UCOB, VER_UCOB);
    Save_DeclareKnown(r, TAK_SECT_PROJ, VER_PROJ);
    Save_DeclareKnown(r, TAK_SECT_FEAT, VER_FEAT);
    Save_DeclareKnown(r, TAK_SECT_FOGV, VER_FOGV);
    Save_DeclareKnown(r, TAK_SECT_ECON, VER_ECON);
    Save_DeclareKnown(r, TAK_SECT_AIST, VER_AIST);
}

TAK_SaveGame *Save_Read(const char *path, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!path) {
        set_err(err, err_cap, "No file name was given for the save.");
        return NULL;
    }

    TAK_SaveReader *r = Save_OpenFile(path, err, err_cap);
    if (!r) return NULL;

    declare_known(r);
    if (Save_Validate(r, err, err_cap) != 0) {
        Save_Close(r);
        return NULL;
    }

    const TAK_SaveHeader *h = Save_ReaderHeader(r);
    if (h->schema_version != TAK_SAVE_SCHEMA_VERSION) {
        set_err(err, err_cap,
                "This save was written by a different version of the game "
                "(state layout %u, this build reads %u).",
                (unsigned)h->schema_version,
                (unsigned)TAK_SAVE_SCHEMA_VERSION);
        Save_Close(r);
        return NULL;
    }
    if (h->determinism_class != TAK_DETERMINISM_FLOAT) {
        set_err(err, err_cap,
                "This save came from a build whose simulation works "
                "differently and cannot be loaded here.");
        Save_Close(r);
        return NULL;
    }

    size_t len = 0;
    const uint8_t *cfgb = (const uint8_t *)Save_Section(r, TAK_SECT_CFGB, NULL, &len);
    if (!cfgb || len < TAK_CFGB_BYTES) {
        set_err(err, err_cap, "This save is missing the battle it was set up as.");
        Save_Close(r);
        return NULL;
    }
    const uint8_t *wrld = (const uint8_t *)Save_Section(r, TAK_SECT_WRLD, NULL, &len);
    if (!wrld || len < TAK_WRLD_BYTES) {
        set_err(err, err_cap, "This save is missing the state of its world.");
        Save_Close(r);
        return NULL;
    }

    TAK_SaveGame *sg = (TAK_SaveGame *)tak_malloc(sizeof(*sg));
    if (!sg) {
        set_err(err, err_cap, "Ran out of memory reading the save.");
        Save_Close(r);
        return NULL;
    }
    memset(sg, 0, sizeof(*sg));
    sg->reader = r;

    decode_cfgb(cfgb, &sg->info.cfg);
    get_text(sg->info.map_name, sizeof(sg->info.map_name),
             wrld + WRLD_MAP_NAME, WRLD_MAP_NAME_CAP);
    get_text(sg->info.map_kingdom, sizeof(sg->info.map_kingdom),
             wrld + WRLD_KINGDOM, WRLD_KINGDOM_CAP);
    sg->info.schema_version = h->schema_version;
    sg->info.sim_tick = h->sim_tick;
    sg->info.sim_state_hash = h->sim_state_hash;
    sg->info.saved_at_utc = h->saved_at_utc;
    sg->info.save_kind = h->save_kind;
    memcpy(sg->info.engine_build, h->engine_build, sizeof(sg->info.engine_build));
    memcpy(sg->info.map_fingerprint, h->map_fingerprint,
           sizeof(sg->info.map_fingerprint));

    const uint8_t *camr = (const uint8_t *)Save_Section(r, TAK_SECT_CAMR, NULL, &len);
    if (camr && len >= TAK_CAMR_BYTES) {
        sg->info.cam_x = tak_get_i32(camr + CAMR_X);
        sg->info.cam_y = tak_get_i32(camr + CAMR_Y);
        sg->info.has_camera = 1;
    }

    /* Two installs can serve different terrain under one name, and a
     * save restores exact positions, so loading onto the wrong ground
     * puts units inside hills. The map is named in the refusal because
     * it is the one thing the player can act on. */
    if (!fingerprint_is_absent(sg->info.map_fingerprint)) {
        uint8_t here[TAK_MAP_FINGERPRINT_BYTES];
        if (TAK_MapFingerprint_FromName(sg->info.map_name, here) != 0) {
            set_err(err, err_cap,
                    "This save was played on the map \"%s\", which is not "
                    "installed.", sg->info.map_name);
            Save_ReadClose(sg);
            return NULL;
        }
        if (memcmp(here, sg->info.map_fingerprint, sizeof(here)) != 0) {
            set_err(err, err_cap,
                    "The map \"%s\" on this system is not the one this save "
                    "was played on.", sg->info.map_name);
            Save_ReadClose(sg);
            return NULL;
        }
    }

    return sg;
}

const TAK_SaveInfo *Save_Info(const TAK_SaveGame *sg) {
    return sg ? &sg->info : NULL;
}

/* Every definition the save names has to still be here and still be
 * the same, or a unit loads with someone else's statistics. The
 * resolved indices are kept: a unit record names its definition by
 * DEFS ordinal, never by a registry index, because a loose file
 * install or a mod changes the registry order. */
static int check_defs(TAK_SaveGame *sg, char *err, size_t err_cap) {
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(sg->reader, TAK_SECT_DEFS,
                                                        NULL, &count, &stored);
    if (!recs || stored == 0) {
        set_err(err, err_cap, "This save is missing the list of units it used.");
        return -1;
    }

    size_t strt_len = 0;
    const void *strt = Save_Section(sg->reader, TAK_SECT_STRT, NULL, &strt_len);
    TAK_StringTable *t = strt ? StringTable_Parse(strt, strt_len) : NULL;
    if (!t) {
        set_err(err, err_cap, "This save is missing the names it refers to.");
        return -1;
    }

    tak_free(sg->def_index);
    tak_free(sg->def_kind);
    sg->def_index = (int32_t *)tak_malloc(sizeof(int32_t) * (size_t)count + 1);
    sg->def_kind = (uint8_t *)tak_malloc((size_t)count + 1);
    if (!sg->def_index || !sg->def_kind) {
        set_err(err, err_cap, "Ran out of memory reading the save.");
        StringTable_Free(t);
        return -1;
    }
    sg->def_count = count;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec[TAK_DEFS_RECORD_BYTES];
        take_record(rec, sizeof(rec), recs + (size_t)i * stored, stored);

        /* The name belongs to the table, so a message that carries
         * it has to be built before the table is freed. */
        const char *name = StringTable_Get(t, (int)tak_get_u16(rec + DEFS_NAME_IDX));
        uint8_t kind = tak_get_u8(rec + DEFS_KIND);
        uint64_t want = tak_get_u64(rec + DEFS_HASH);
        if (!name) {
            set_err(err, err_cap, "This save refers to a name it does not carry.");
            StringTable_Free(t);
            return -1;
        }

        DefRef ref;
        ref.kind = kind;
        ref.index = (kind == TAK_DEF_KIND_UNIT) ? Units_FindDefByName(name)
                                                : Features_FindByName(name);
        if (ref.index < 0) {
            set_err(err, err_cap,
                    "This save needs \"%s\", which this installation does "
                    "not have.", name);
            StringTable_Free(t);
            return -1;
        }
        uint64_t got = 0;
        if (defref_hash(&ref, &got) != 0 || got != want) {
            set_err(err, err_cap,
                    "\"%s\" has changed since this save was written, so the "
                    "battle would not play out the same way.", name);
            StringTable_Free(t);
            return -1;
        }
        sg->def_index[i] = ref.index;
        sg->def_kind[i] = kind;
    }
    StringTable_Free(t);
    return 0;
}

/* The unit array, its routes and its scripts. Slot i goes back in slot
 * i, dead slots included: that is what keeps every handle the
 * simulation holds pointing at the same thing it did. */
static int apply_units(TAK_SaveGame *sg, char *err, size_t err_cap) {
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(sg->reader, TAK_SECT_UNIT,
                                                        NULL, &count, &stored);
    if (!recs || stored == 0) {
        set_err(err, err_cap, "This save is missing the units it was played with.");
        return -1;
    }
    const TAK_SaveHeader *h = Save_ReaderHeader(sg->reader);
    if (Units_LoadBegin((int)count, h->unit_stable_id_next) != 0) {
        set_err(err, err_cap,
                "This save holds more units than this build can bring up.");
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec[TAK_UNIT_RECORD_BYTES];
        take_record(rec, sizeof(rec), recs + (size_t)i * stored, stored);
        Unit *u = Units_LoadSlot((int)i);
        if (!u) continue;
        if (decode_unit(u, rec, sg, err, err_cap) != 0) return -1;
        if (u->alive == UNIT_ALIVE_DEAD) continue;
        /* The engine is allocated and bound here and left with all
         * sixteen threads clear. Create is not run: the file carries
         * the threads mid execution and Create would replay its side
         * effects on top of them. */
        if (tak_get_u8(rec + U_HAS_COB) && Units_LoadAttachScript((int)i) < 0) {
            set_err(err, err_cap,
                    "This save holds script state this installation cannot "
                    "bring up.");
            return -1;
        }
    }

    size_t len = 0;
    const uint8_t *pth = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_UPTH,
                                                       NULL, &len);
    if (!pth) {
        set_err(err, err_cap, "This save is missing the routes its units were on.");
        return -1;
    }
    Cur pc = { pth, len, 0, 0 };
    if (apply_paths(&pc, (int)count, err, err_cap) != 0) return -1;

    const uint8_t *cb = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_UCOB,
                                                      NULL, &len);
    if (!cb) {
        set_err(err, err_cap, "This save is missing the state of its scripts.");
        return -1;
    }
    Cur cc = { cb, len, 0, 0 };
    if (apply_cob(&cc, (int)count, err, err_cap) != 0) return -1;
    return 0;
}

static int apply_projectiles(TAK_SaveGame *sg, char *err, size_t err_cap) {
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(sg->reader, TAK_SECT_PROJ,
                                                        NULL, &count, &stored);
    if (!recs && count != 0) {
        set_err(err, err_cap, "This save is missing the shots that were in flight.");
        return -1;
    }
    size_t strt_len = 0;
    const void *strt = Save_Section(sg->reader, TAK_SECT_STRT, NULL, &strt_len);
    TAK_StringTable *t = strt ? StringTable_Parse(strt, strt_len) : NULL;
    if (!t) {
        set_err(err, err_cap, "This save is missing the names it refers to.");
        return -1;
    }

    /* The pool count is the high water mark, so it is the highest live
     * slot plus one rather than the number of records. */
    int high = 0;
    for (uint32_t i = 0; i < count && stored; i++) {
        int slot = (int)tak_get_u32(recs + (size_t)i * stored + P_SLOT);
        if (slot + 1 > high) high = slot + 1;
    }
    Projectile *pool = Units_LoadProjectiles(high);
    if (!pool && high > 0) {
        StringTable_Free(t);
        set_err(err, err_cap,
                "This save holds more shots in flight than this build can "
                "bring up.");
        return -1;
    }
    for (uint32_t i = 0; i < count && stored; i++) {
        uint8_t rec[TAK_PROJ_RECORD_BYTES];
        take_record(rec, sizeof(rec), recs + (size_t)i * stored, stored);
        int slot = (int)tak_get_u32(rec + P_SLOT);
        if (slot < 0 || slot >= high) continue;
        decode_projectile(&pool[slot], rec, t);
    }
    StringTable_Free(t);
    return 0;
}

/* Features and the corpses among them. A corpse keeps the exact spot,
 * facing and tilt of the unit that fell, and its countdown runs in
 * seconds, so a save catches these mid flight. */
static int apply_features(TAK_SaveGame *sg, GameWorld *w, char *err,
                          size_t err_cap) {
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(sg->reader, TAK_SECT_FEAT,
                                                        NULL, &count, &stored);
    if (!recs && count != 0) {
        set_err(err, err_cap, "This save is missing the features on its map.");
        return -1;
    }
    if ((int)count > w->feature_cap || !w->features) {
        size_t want = (size_t)count ? (size_t)count : 1u;
        struct MapFeature *grown = (struct MapFeature *)tak_realloc(
            w->features, want * sizeof(*w->features));
        if (!grown) {
            set_err(err, err_cap, "Ran out of memory reading the save.");
            return -1;
        }
        w->features = grown;
        w->feature_cap = (int)want;
    }
    for (uint32_t i = 0; i < count && stored; i++) {
        uint8_t rec[TAK_FEAT_RECORD_BYTES];
        take_record(rec, sizeof(rec), recs + (size_t)i * stored, stored);
        decode_feature(&w->features[i], rec, sg);
    }
    w->feature_count = (int)count;
    return 0;
}

int Save_Apply(TAK_SaveGame *sg, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!sg) {
        set_err(err, err_cap, "There is no save to apply.");
        return -1;
    }
    GameWorld *w = World_Get();
    if (!w) {
        set_err(err, err_cap, "The world has to be brought up before a save "
                              "can be applied to it.");
        return -1;
    }
    if (check_defs(sg, err, err_cap) != 0) return -1;

    size_t len = 0;
    const uint8_t *wrld = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_WRLD,
                                                        NULL, &len);
    if (!wrld || len < TAK_WRLD_BYTES) {
        set_err(err, err_cap, "This save is missing the state of its world.");
        return -1;
    }
    w->cfg = sg->info.cfg;
    apply_wrld(wrld, w);
    if (sg->info.has_camera) {
        w->cam_x = sg->info.cam_x;
        w->cam_y = sg->info.cam_y;
    }

    if (apply_units(sg, err, err_cap) != 0) return -1;
    if (apply_projectiles(sg, err, err_cap) != 0) return -1;
    if (apply_features(sg, w, err, err_cap) != 0) return -1;

    const uint8_t *fog = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_FOGV,
                                                       NULL, &len);
    if (!fog) {
        set_err(err, err_cap, "This save is missing the ground its players "
                              "had explored.");
        return -1;
    }
    Cur fc = { fog, len, 0, 0 };
    if (apply_fog(&fc, w, err, err_cap) != 0) return -1;

    const uint8_t *econ = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_ECON,
                                                        NULL, &len);
    if (!econ || len < TAK_ECON_BYTES) {
        set_err(err, err_cap, "This save is missing its players' mana.");
        return -1;
    }
    apply_econ(econ, &w->economy);

    const uint8_t *ai = (const uint8_t *)Save_Section(sg->reader, TAK_SECT_AIST,
                                                      NULL, &len);
    if (!ai || TAK_AI_LoadState(ai, (unsigned int)len) != 0) {
        set_err(err, err_cap, "This save is missing what its opponents were "
                              "doing.");
        return -1;
    }

    /* Last, because the occupancy stamp reads every restored unit and
     * the spatial grid reads their final positions. */
    Units_LoadFinish();
    return 0;
}

void Save_ReadClose(TAK_SaveGame *sg) {
    if (!sg) return;
    Save_Close(sg->reader);
    tak_free(sg->def_index);
    tak_free(sg->def_kind);
    tak_free(sg);
}
