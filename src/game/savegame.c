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

#include "tak_battle_config.h"
#include "tak_bytes.h"
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

/* The reader understands each section up to this version. */
#define VER_DEFS 1
#define VER_CFGB 1
#define VER_WRLD 1
#define VER_CAMR 1
#define VER_STRT 1
#define VER_SUMM 1

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
    int slots = 0;
    (void)Units_GetActive(&slots);
    hdr.unit_slot_count = (uint32_t)(slots > 0 ? slots : 0);
    hdr.saved_at_utc = (uint64_t)time(NULL);
    /* Two installs can serve different terrain under one name, so the
     * save records which one it was played on. A map the writer cannot
     * resolve leaves the field zero, which the reader reads as unknown
     * rather than as a mismatch. */
    if (TAK_MapFingerprint_FromName(w->map_name, hdr.map_fingerprint) != 0) {
        memset(hdr.map_fingerprint, 0, sizeof(hdr.map_fingerprint));
    }

    DefSet set = { NULL, 0, 0 };
    if (defset_collect(&set, w) != 0) {
        tak_free(set.refs);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    TAK_StringTable *strings = StringTable_New();
    uint8_t *defs = NULL;
    if (strings) {
        defs = (uint8_t *)tak_malloc((size_t)set.count * TAK_DEFS_RECORD_BYTES + 1);
    }
    if (!strings || !defs) {
        StringTable_Free(strings);
        tak_free(defs);
        tak_free(set.refs);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    int written = 0;
    for (int i = 0; i < set.count; i++) {
        const char *name = defref_name(&set.refs[i]);
        uint64_t hash = 0;
        if (!name || defref_hash(&set.refs[i], &hash) != 0) continue;
        int idx = StringTable_Intern(strings, name);
        if (idx < 0) {
            StringTable_Free(strings);
            tak_free(defs);
            tak_free(set.refs);
            set_err(err, err_cap, "Ran out of memory building the save.");
            return -1;
        }
        uint8_t *rec = defs + (size_t)written * TAK_DEFS_RECORD_BYTES;
        tak_put_u16(rec + DEFS_NAME_IDX, (uint16_t)idx);
        tak_put_u8(rec + DEFS_KIND, set.refs[i].kind);
        tak_put_u8(rec + DEFS_PAD, 0);
        tak_put_u64(rec + DEFS_HASH, hash);
        written++;
    }
    tak_free(set.refs);

    size_t strt_len = 0;
    uint8_t *strt = StringTable_Serialize(strings, &strt_len);
    StringTable_Free(strings);
    if (!strt) {
        tak_free(defs);
        set_err(err, err_cap, "Ran out of memory building the save.");
        return -1;
    }

    uint8_t cfgb[TAK_CFGB_BYTES];
    uint8_t wrld[TAK_WRLD_BYTES];
    uint8_t camr[TAK_CAMR_BYTES];
    encode_cfgb(cfgb, &w->cfg);
    encode_wrld(wrld, w);
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
    /* The camera is local view state, so an older reader may skip it. */
    if (rc == 0) rc = Save_AddSection(writer, TAK_SECT_CAMR, VER_CAMR, 0,
                                      camr, sizeof(camr));
    tak_free(strt);
    tak_free(defs);
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

struct TAK_SaveGame {
    TAK_SaveReader *reader;
    TAK_SaveInfo    info;
};

static void declare_known(TAK_SaveReader *r) {
    Save_DeclareKnown(r, TAK_SECT_SUMM, VER_SUMM);
    Save_DeclareKnown(r, TAK_SECT_STRT, VER_STRT);
    Save_DeclareKnown(r, TAK_SECT_DEFS, VER_DEFS);
    Save_DeclareKnown(r, TAK_SECT_CFGB, VER_CFGB);
    Save_DeclareKnown(r, TAK_SECT_WRLD, VER_WRLD);
    Save_DeclareKnown(r, TAK_SECT_CAMR, VER_CAMR);
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

    return sg;
}

const TAK_SaveInfo *Save_Info(const TAK_SaveGame *sg) {
    return sg ? &sg->info : NULL;
}

/* Every definition the save names has to still be here and still be
 * the same, or a unit loads with someone else's statistics. */
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

    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec[TAK_DEFS_RECORD_BYTES];
        memset(rec, 0, sizeof(rec));
        uint16_t take = stored < TAK_DEFS_RECORD_BYTES
                      ? stored : (uint16_t)TAK_DEFS_RECORD_BYTES;
        memcpy(rec, recs + (size_t)i * stored, take);

        const char *name = StringTable_Get(t, (int)tak_get_u16(rec + DEFS_NAME_IDX));
        uint8_t kind = tak_get_u8(rec + DEFS_KIND);
        uint64_t want = tak_get_u64(rec + DEFS_HASH);
        if (!name) {
            StringTable_Free(t);
            set_err(err, err_cap, "This save refers to a name it does not carry.");
            return -1;
        }

        DefRef ref;
        ref.kind = kind;
        ref.index = (kind == TAK_DEF_KIND_UNIT) ? Units_FindDefByName(name)
                                                : Features_FindByName(name);
        if (ref.index < 0) {
            StringTable_Free(t);
            set_err(err, err_cap,
                    "This save needs \"%s\", which this installation does "
                    "not have.", name);
            return -1;
        }
        uint64_t got = 0;
        if (defref_hash(&ref, &got) != 0 || got != want) {
            StringTable_Free(t);
            set_err(err, err_cap,
                    "\"%s\" has changed since this save was written, so the "
                    "battle would not play out the same way.", name);
            return -1;
        }
    }
    StringTable_Free(t);
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
    return 0;
}

void Save_ReadClose(TAK_SaveGame *sg) {
    if (!sg) return;
    Save_Close(sg->reader);
    tak_free(sg);
}
