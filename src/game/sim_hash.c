#include "tak_sim_hash.h"

#include "tak_cob.h"
#include "tak_cob_vm.h"
#include "tak_economy.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_world.h"

/* See tak_sim_hash.h for what is covered and what is deliberately
 * left out. The rules this file has to keep:
 *
 *   - Every value goes in at an explicit width. Never sizeof a struct,
 *     never a pointer, never a native memcpy of a record, because the
 *     same struct is a different size on the 32 bit and 64 bit builds.
 *   - Array lengths go in before their contents, so a shorter array
 *     and a longer one that starts the same cannot collide.
 *   - Only the live prefix of a variable length array is hashed. The
 *     dead tail of a path is whatever the last longer path left there.
 */

static uint32_t hash_weapon(uint32_t h, const UnitWeaponState *ws) {
    h = TAK_HashI32(h, ws->cooldown_ticks);
    h = TAK_HashI32(h, ws->burst_ticks);
    h = TAK_HashI32(h, ws->burst_remaining);
    h = TAK_HashI32(h, ws->burst_target);
    h = TAK_HashI32(h, ws->aim_thread_slot);
    h = TAK_HashI32(h, ws->aim_ticks);
    h = TAK_HashI32(h, ws->aim_target);
    return h;
}

static uint32_t hash_cob_piece(uint32_t h, const CobPiece *p) {
    for (int a = 0; a < 3; a++) {
        h = TAK_HashI32(h, p->rot[a]);
        h = TAK_HashI32(h, p->rot_target[a]);
        h = TAK_HashI32(h, p->rot_speed[a]);
        h = TAK_HashI32(h, p->spin_speed[a]);
        h = TAK_HashI32(h, p->spin_accel[a]);
        h = TAK_HashI32(h, p->pos[a]);
        h = TAK_HashI32(h, p->pos_target[a]);
        h = TAK_HashI32(h, p->pos_speed[a]);
    }
    h = TAK_HashI32(h, p->hidden);
    h = TAK_HashI32(h, p->exploded);
    h = TAK_HashI32(h, p->shadow_off);
    h = TAK_HashI32(h, p->shade_off);
    return h;
}

/* All sixteen slots, dead ones included. src/render/units.c reads a
 * weapon's aim result off a slot that Cob_IsThreadAlive has already
 * reported dead, and the expression treats a missing return value as
 * permission to fire, so a zeroed dead slot makes a unit whose script
 * said hold fire shoot instead. */
static uint32_t hash_cob_thread(uint32_t h, const CobThread *t) {
    h = TAK_HashU32(h, t->pc);
    h = TAK_HashI32(h, t->sleep_remaining);
    h = TAK_HashI32(h, t->wait_piece);
    h = TAK_HashU32(h, t->signal_mask);
    h = TAK_HashI32(h, t->wait_child);
    h = TAK_HashI32(h, t->wait_axis);
    h = TAK_HashI32(h, t->wait_kind);
    h = TAK_HashI32(h, t->sp);
    h = TAK_HashI32(h, t->alive);
    h = TAK_HashI32(h, t->has_return_value);
    h = TAK_HashI32(h, t->return_value);
    int depth = t->sp;
    if (depth < 0) depth = 0;
    if (depth > COB_THREAD_STACK_DEPTH) depth = COB_THREAD_STACK_DEPTH;
    for (int i = 0; i < depth; i++) h = TAK_HashI32(h, t->stack[i]);
    return h;
}

static uint32_t hash_cob(uint32_t h, const CobEngine *e) {
    if (!e) return TAK_HashU32(h, 0xffffffffu);   /* no engine is a state */
    int pieces = e->piece_count;
    if (pieces < 0) pieces = 0;
    h = TAK_HashI32(h, pieces);
    if (e->pieces) {
        for (int i = 0; i < pieces; i++) h = hash_cob_piece(h, &e->pieces[i]);
    }
    /* Per instance script state, derivable from nothing. */
    uint32_t statics = e->script ? e->script->num_static_vars : 0u;
    h = TAK_HashU32(h, statics);
    if (e->static_vars) {
        for (uint32_t i = 0; i < statics; i++) h = TAK_HashI32(h, e->static_vars[i]);
    }
    for (int i = 0; i < COB_THREADS_PER_UNIT; i++) h = hash_cob_thread(h, &e->threads[i]);
    return h;
}

static uint32_t hash_unit(uint32_t h, const Unit *u) {
    /* A dead slot is a tombstone: the save writes only these two and
     * the rest of the record is whatever it held when the unit died. */
    h = TAK_HashI32(h, u->alive);
    h = TAK_HashU32(h, u->stable_id);
    if (u->alive == UNIT_ALIVE_DEAD) return h;

    h = TAK_HashI32(h, u->world_x);
    h = TAK_HashI32(h, u->world_y);
    h = TAK_HashF32(h, u->heading);
    h = TAK_HashF32(h, u->pitch);
    h = TAK_HashF32(h, u->roll);
    h = TAK_HashI32(h, u->velocity);
    h = TAK_HashI32(h, u->health);
    h = TAK_HashI32(h, u->max_health);
    h = TAK_HashI32(h, u->cmd_x);
    h = TAK_HashI32(h, u->cmd_y);
    h = TAK_HashI32(h, u->patrol_x);
    h = TAK_HashI32(h, u->patrol_y);
    h = TAK_HashI32(h, u->target);
    h = TAK_HashI32(h, u->cmd_kind);
    h = TAK_HashI32(h, u->attack_cooldown);
    /* The definition index, not its name. Within one match and between
     * two peers running the same data it is the same number. A save
     * carries the name and resolves it, so a different install changes
     * this and the loader is what has to notice, not the hash. */
    h = TAK_HashU32(h, u->def_idx);
    h = TAK_HashI32(h, u->player_id);
    h = TAK_HashI32(h, u->team_color_idx);
    h = TAK_HashI32(h, u->aggro_mode);
    h = TAK_HashI32(h, u->weapon_slot);
    h = TAK_HashI32(h, u->experience_pts);
    h = TAK_HashI32(h, u->kills);
    h = TAK_HashI32(h, u->build_target);
    h = TAK_HashI32(h, u->reclaim_tile_x);
    h = TAK_HashI32(h, u->reclaim_tile_y);
    h = TAK_HashF32(h, u->reclaim_accum);
    h = TAK_HashI32(h, u->corpse_type);
    h = TAK_HashI32(h, u->raise_mode);
    h = TAK_HashI32(h, u->raise_left);
    h = TAK_HashI32(h, u->carried_by);
    h = TAK_HashI32(h, u->cargo_count);
    h = TAK_HashI32(h, u->cargo_size_used);
    /* The pickup queue, live prefix only, and the drop in progress.
     * carry_seq orders the drop so the last aboard leaves first, and
     * carry_next is the counter that hands it out. */
    int loads = u->load_queue_len;
    if (loads > UNIT_LOAD_QUEUE_MAX) loads = UNIT_LOAD_QUEUE_MAX;
    h = TAK_HashI32(h, loads);
    for (int i = 0; i < loads; i++) h = TAK_HashI32(h, u->load_queue[i]);
    h = TAK_HashI32(h, u->xfer_ticks);
    h = TAK_HashI32(h, u->xfer_cargo);
    h = TAK_HashI32(h, u->xfer_wait);
    h = TAK_HashI32(h, u->unload_stage);
    h = TAK_HashI32(h, u->unload_delay);
    h = TAK_HashI32(h, u->unload_hold);
    h = TAK_HashI32(h, u->unload_tries);
    h = TAK_HashI32(h, u->unload_rests);
    h = TAK_HashI32(h, u->unload_approach);
    h = TAK_HashI32(h, u->carry_seq);
    h = TAK_HashI32(h, u->carry_next);
    h = TAK_HashI32(h, u->unload_gx);
    h = TAK_HashI32(h, u->unload_gy);
    h = TAK_HashI32(h, u->under_construction);
    h = TAK_HashI32(h, u->cob_activation);
    h = TAK_HashI32(h, u->cob_build_stance);
    h = TAK_HashI32(h, u->cob_yard_open);
    h = TAK_HashI32(h, u->cob_bugger_off);
    h = TAK_HashF32(h, u->flight_alt);
    h = TAK_HashI32(h, u->flying);
    h = TAK_HashI32(h, u->sfx_occupy);
    h = TAK_HashI32(h, u->attack_explicit);
    /* The caster's own pool, separate from the player economy. */
    h = TAK_HashF32(h, u->mana);
    h = TAK_HashF32(h, u->mana_max);
    h = TAK_HashI32(h, u->occ_on);
    h = TAK_HashI32(h, u->occ_pending);
    h = TAK_HashI32(h, u->gate_scan_cd);
    h = TAK_HashI32(h, u->gate_hold);
    h = TAK_HashI32(h, u->occ_tx);
    h = TAK_HashI32(h, u->occ_ty);
    h = TAK_HashI32(h, u->occ_fx);
    h = TAK_HashI32(h, u->occ_fz);
    h = TAK_HashF32(h, u->build_hp_accum);
    h = TAK_HashI32(h, u->nano_idle_ticks);
    h = TAK_HashF32(h, u->subpixel_x);
    h = TAK_HashF32(h, u->subpixel_y);
    h = TAK_HashF32(h, u->cur_speed_ppt);
    h = TAK_HashI32(h, u->path_goal_x);
    h = TAK_HashI32(h, u->path_goal_y);
    h = TAK_HashI32(h, u->path_index);
    h = TAK_HashI32(h, u->path_failed);
    h = TAK_HashI32(h, u->path_pending);
    h = TAK_HashI32(h, u->path_wait);
    h = TAK_HashI32(h, u->blocked_ticks);
    h = TAK_HashI32(h, u->wp_stall);
    h = TAK_HashI32(h, u->wp_best_d2);
    h = TAK_HashI32(h, u->path_replan_cd);
    h = TAK_HashI32(h, u->route_seg_x);
    h = TAK_HashI32(h, u->route_seg_y);
    h = TAK_HashI32(h, u->route_check_cd);
    h = TAK_HashI32(h, u->route_flags);
    h = TAK_HashI32(h, u->occ_parked);
    h = TAK_HashI32(h, u->still_ticks);

    /* The live prefix only. The tail holds whatever the last longer
     * path left behind and no save writes it. */
    int path_len = u->path_len;
    if (path_len > UNIT_PATH_MAX_WAYPOINTS) path_len = UNIT_PATH_MAX_WAYPOINTS;
    h = TAK_HashI32(h, path_len);
    for (int i = 0; i < path_len; i++) {
        h = TAK_HashI32(h, u->path_x[i]);
        h = TAK_HashI32(h, u->path_y[i]);
    }

    h = TAK_HashI32(h, u->anim_state);
    h = TAK_HashI32(h, u->walk_thread_slot);
    h = TAK_HashI32(h, u->killed_thread_slot);
    h = TAK_HashI32(h, u->build_thread_slot);
    h = TAK_HashI32(h, u->move_rate_tier);
    h = TAK_HashI32(h, u->turn_dir_sign);
    /* Losing these re-runs Activate on every lodestone after a load,
     * because the latch tests the counter against zero. */
    for (int i = 0; i < UNIT_SCRIPT_EV_COUNT; i++) h = TAK_HashI32(h, u->script_ev[i]);

    for (int i = 0; i < 3; i++) h = hash_weapon(h, &u->weapon_state[i]);

    int queued = u->prod_queue_len;
    if (queued > UNIT_PROD_QUEUE_MAX) queued = UNIT_PROD_QUEUE_MAX;
    h = TAK_HashI32(h, queued);
    for (int i = 0; i < queued; i++) h = TAK_HashI32(h, u->prod_queue[i]);
    h = TAK_HashI32(h, u->rally_set);
    h = TAK_HashI32(h, u->rally_x);
    h = TAK_HashI32(h, u->rally_y);

    h = hash_cob(h, u->cob);
    return h;
}

static uint32_t hash_units(uint32_t h) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (count < 0) count = 0;
    h = TAK_HashI32(h, count);
    if (!units) return h;
    for (int i = 0; i < count; i++) h = hash_unit(h, &units[i]);
    return h;
}

/* Projectile slots are recycled, so the live ones are hashed in slot
 * order and the dead ones contribute nothing but their flag. The art
 * and explosion indices are left out: they come from lazily appended
 * caches in first fire order and a load re-derives both. */
static uint32_t hash_projectiles(uint32_t h) {
    int count = 0;
    const Projectile *p = Units_GetProjectiles(&count);
    if (count < 0) count = 0;
    h = TAK_HashI32(h, count);
    if (!p) return h;
    for (int i = 0; i < count; i++) {
        h = TAK_HashI32(h, p[i].alive);
        if (!p[i].alive) continue;
        h = TAK_HashI32(h, p[i].world_x);
        h = TAK_HashI32(h, p[i].world_y);
        h = TAK_HashF32(h, p[i].sub_x);
        h = TAK_HashF32(h, p[i].sub_y);
        h = TAK_HashF32(h, p[i].dir_x);
        h = TAK_HashF32(h, p[i].dir_y);
        h = TAK_HashF32(h, p[i].speed_ppt);
        h = TAK_HashI32(h, p[i].damage);
        h = TAK_HashI32(h, p[i].area_of_effect);
        h = TAK_HashF32(h, p[i].edge_effectiveness);
        h = TAK_HashI32(h, p[i].target);
        h = TAK_HashI32(h, p[i].shooter);
        h = TAK_HashI32(h, p[i].ttl_ticks);
        h = TAK_HashI32(h, p[i].player_id);
        h = TAK_HashI32(h, p[i].visual_kind);
        h = TAK_HashI32(h, p[i].friendly_fire);
        h = TAK_HashI32(h, p[i].dest_x);
        h = TAK_HashI32(h, p[i].dest_y);
        h = TAK_HashI32(h, p[i].is_beam);
        h = TAK_HashI32(h, p[i].src_x);
        h = TAK_HashI32(h, p[i].src_y);
        h = TAK_HashF32(h, p[i].height);
        h = TAK_HashF32(h, p[i].vel_up_ppt);
        h = TAK_HashF32(h, p[i].gravity_ppt2);
        h = TAK_HashF32(h, p[i].heading);
        h = TAK_HashF32(h, p[i].pitch);
        h = TAK_HashF32(h, p[i].roll);
        h = TAK_HashF32(h, p[i].spin_pitch);
        h = TAK_HashF32(h, p[i].spin_heading);
        h = TAK_HashF32(h, p[i].spin_roll);
        h = TAK_HashI32(h, p[i].src_height);
        h = TAK_HashI32(h, p[i].age_ticks);
        h = TAK_HashI32(h, p[i].color_idx);
    }
    return h;
}

/* Corpses count down in seconds rather than minutes, so a save has to
 * catch those counters mid flight and the hash has to see them. */
static uint32_t hash_features(uint32_t h, const GameWorld *w) {
    h = TAK_HashI32(h, w->feature_count);
    if (!w->features) return h;
    for (int i = 0; i < w->feature_count; i++) {
        const struct MapFeature *f = &w->features[i];
        h = TAK_HashI32(h, f->feat_id);
        h = TAK_HashI32(h, f->tile_x);
        h = TAK_HashI32(h, f->tile_z);
        h = TAK_HashI32(h, f->global_idx);
        h = TAK_HashI32(h, f->world_x);
        h = TAK_HashI32(h, f->world_y);
        h = TAK_HashI32(h, f->heading);
        h = TAK_HashI32(h, f->pitch);
        h = TAK_HashI32(h, f->roll);
        h = TAK_HashI32(h, f->color_idx);
        h = TAK_HashI32(h, f->decompose_ticks);
        h = TAK_HashI32(h, f->sink_ticks);
    }
    return h;
}

/* Every player's layer, not just the local one. Fog is history, not a
 * function of the present, and the AI, the minimap and line of sight
 * gating all read layers the human never sees. */
static uint32_t hash_fog(uint32_t h, const GameWorld *w) {
    h = TAK_HashI32(h, w->fog_w);
    h = TAK_HashI32(h, w->fog_h);
    h = TAK_HashI32(h, w->fog_cell_px);
    size_t cells = (size_t)(w->fog_w > 0 ? w->fog_w : 0) *
                   (size_t)(w->fog_h > 0 ? w->fog_h : 0);
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        const uint8_t *layer = w->fog_layers[p];
        h = TAK_HashI32(h, layer ? 1 : 0);
        if (layer && cells) h = TAK_HashBytes(h, layer, cells);
    }
    return h;
}

static uint32_t hash_economy(uint32_t h, const GameWorld *w) {
    const EconomyState *eco = &w->economy;
    h = TAK_HashI32(h, eco->active_count);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const PlayerEconomy *e = &eco->players[i];
        h = TAK_HashF32(h, e->mana);
        h = TAK_HashI32(h, e->max_mana);
        h = TAK_HashF32(h, e->regen_per_sec);
        h = TAK_HashI32(h, e->spent_last_sec);
        h = TAK_HashI32(h, e->earned_last_sec);
        h = TAK_HashF32(h, e->earned_accum);
        h = TAK_HashF32(h, e->spent_accum);
        h = TAK_HashI32(h, e->ticks_since_window_reset);
    }
    return h;
}

/* World scalars and the per player tallies. The camera is not here:
 * two lockstep peers are entitled to be looking at different parts of
 * the map, and world->occ_version is not here either because a load
 * bumps it on purpose to invalidate the clearance cache. */
static uint32_t hash_world(uint32_t h, const GameWorld *w) {
    h = TAK_HashI32(h, w->skirmish_elapsed_ticks);
    h = TAK_HashI32(h, w->skirmish_game_over);
    h = TAK_HashI32(h, w->skirmish_winner_team);
    h = TAK_HashI32(h, w->skirmish_local_result);
    h = TAK_HashI32(h, w->skirmish_end_tick);
    h = TAK_HashI32(h, w->skirmish_stats_open);
    h = TAK_HashStr(h, w->skirmish_end_reason);
    h = TAK_HashI32(h, w->mission_elapsed_ticks);
    h = TAK_HashI32(h, w->mission_objectives_satisfied);
    h = TAK_HashI32(h, w->mission_victory);
    h = TAK_HashI32(h, w->water_height);
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        const PlayerBattleStats *s = &w->stats[p];
        h = TAK_HashI32(h, s->units_built);
        h = TAK_HashI32(h, s->kills);
        h = TAK_HashI32(h, s->losses);
        h = TAK_HashI32(h, s->score);
        h = TAK_HashI32(h, s->eliminated);
        h = TAK_HashI32(h, s->last_alive_tick);
    }
    return h;
}

uint32_t TAK_SimHash(void) {
    const GameWorld *w = World_Get();
    if (!w) return 0;
    uint32_t h = TAK_SIM_HASH_SEED;
    h = hash_world(h, w);
    /* The simulation generator. Every unit script's RAND draws from it. */
    h = TAK_HashU32(h, World_RandState());
    h = hash_units(h);
    h = hash_projectiles(h);
    h = hash_features(h, w);
    h = hash_fog(h, w);
    h = hash_economy(h, w);
    h = TAK_SimHash_AI(h);
    return h;
}
