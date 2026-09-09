# P2 — Factory build spots (QueryBuildInfo) + naval product water gating

Patch plan. Two issues: (1) factory products must spawn at the COB QueryBuildInfo piece,
not the factory centre; (2) naval products (ships) must only start when the spot has
enough water for the PRODUCT's move class, and naval-yard/user placement must respect
water depth. All line numbers verified on disk 2026-09-08 (post-f91e853).

Legacy sections describe behaviour derived from analysis of the retail binary. `:NNNNN`
citations are evidence pointers into a private reference that is not distributed — they mark where a claim can be re-checked by whoever holds it, and
nothing here reproduces that file's expression. Symbol names marked *(tool-assigned,
misleading)* are automatic labels that do **not** describe what the routine does; the
description beside them does. Fenced `c` blocks below are TAK-RE implementation code (proposed), not
legacy code.

---

## Issue 1 — spawn at the COB build spot

### Legacy mechanism

Factory production is phase 2 of the construction mission handler, :9342-9456. Per
production start:

1. :9347-9350 — the factory's COB `QueryBuildInfo` script is run **synchronously**, through
   the same helper the engine uses to invoke weapon-fire scripts (:306129, dispatching to
   :306142-306208): allocate a thread, seed its four deepest stack slots with the four call
   arguments, run it to completion, then read those four slots back. Argument 0 is the
   out-param — the script writes the build-spot **piece index** into its local 0
   (`piecenum = N`).
2. :9351-9362 — piece index → world position. The piece's model-space offset is accumulated
   up the parent chain (:185790-185836): walk node → parent, adding the static node offset
   plus any animated translation, rotating by the parent's angles; the top level additionally
   applies the unit's heading angles, and z is negated. That offset is added to the unit's
   world position (:185840-185859), and the result is stored on the production order. The
   two routines in this pair are labelled `RBTree_Insert` / `RBTree_Remove`
   *(tool-assigned, misleading)* — they are the piece-transform chain and the
   get-piece-world-position helper, nothing to do with trees.
3. :9363-9373 — the build-placement search runs with the **PRODUCT** def (resolved from the
   product def index recorded on the order) and the product's own footprint, centred on the
   spot. On failure (:9374-9382) the mission waits a random 7-22 frame delay, stays in
   phase 2, and **re-runs QueryBuildInfo on the next attempt** — this is how VERASY's
   alternating dock arm re-picks a side.
4. :9396-9407 — the product unit is created at the spot; :9429-9430 the product's `getbuilt`
   script runs and it is given its exit-walk move order; :9435-9446 the factory starts its
   build animation.

### Data oracles (shipped assets, verified with cob_inspect / probe_3do)

- `scripts/vercastl.cob` QueryBuildInfo (pc 2273-2280): sets out-arg 0 to 1 and returns —
  piece [01] = `emitbuild`. 3DO: `EmitBuild` offset (-51181, 0, -8557023); at the
  fixed south placement heading that lands ~180 px south of centre — just past the
  doors (footprintz 20 → half-extent 160 px, `Doorleft` z ≈ −99 px).
- `scripts/verasy.cob` QueryBuildInfo (pc 2190-2218): toggles static variable 1, TURN-NOWs
  piece 1 about axis y to ±16384, then sets out-arg 0 to 1 — piece [01] = `emitbuild`
  (child of `buildrot`, offset (0,0,−7045120) ≈ 148 px in facing direction).
  Stateful: each call flips the arm — query must run **once per production start**,
  and piece position must be read **after** the script runs.
- `scripts/zontrain.cob` has NO QueryBuildInfo → fallback to unit centre
  (legacy: a missing script leaves the out-arg untouched → piece −1 → offset (0,0,0)).

### Current code

- src/render/units.c:1312 `Units_BeginBuildingForUnit`; factory branch 1336-1343
  spawns the product at the factory centre (TODO(parity) at :1334). No terrain
  check at all in this branch.
- Callers all funnel here: idle enqueue units.c:1377-1382, cancel-advance
  :1421-1428, complete-advance :4557-4568, AI src/game/ai.c:328-336, HUD
  src/ui/hud.c:1005.
- Exit walk at completion units.c:4517-4554: rally wins, else 4-dir scan from the
  **factory centre** (4542-4543).
- Piece transforms already exist: `compose_node_xforms` units.c:4927 (parent-chain
  compose, model frame), model→world map in submit_run units.c:5161-5165:
  `rx = -(ch*mx + sh*mz); rz = -(sh*mx - ch*mz); wx = ux + rx*ta; wz = uz + rz*ta`
  (ta = g_ta_scale 0.000021, units.c:669). Same math already duplicated at
  :5496-5498 (ghost).
- COB VM: script args are bottom stack slots (`Cob_StartThread` cob_vm.c:252-276,
  args[0] deepest); POP-VAR mode 2 writes `t->stack[idx]` (cob_vm.c:988-1005);
  `terminate_thread` (cob_vm.c:457) does NOT clear the stack → out-args readable
  after death. `run_thread` (cob_vm.c:607) is the sync executor; SLEEP/WAIT yield
  via `return` (cob_vm.c:657-668, :901-914). TURN-NOW applies piece rot immediately
  (cob_vm.c:699-713) — verasy's arm state is current when we read the piece pos.

### Edits

E1. include/tak_cob_vm.h (after Cob_StartThread, ~line 173): declare

```c
/* Run a script synchronously on a fresh thread and copy its arg locals
 * back out (legacy CobEngine_InvokeFireEvent, legacy:306142-306208).
 * Scripts return out-params by writing their arg slots (QueryBuildInfo
 * piecenum). 0 on success, -1 if script missing / no free slot. */
int Cob_RunScriptSync(CobEngine *e, const char *name,
                      int32_t *args_inout, int n_args);
```

E2. src/render/cob_vm.c (near Cob_GetThreadReturn, ~line 445): implement:
start thread via `Cob_StartThreadByName(e, name, args_inout, n_args)`; if slot < 0
return -1; `run_thread(e, slot, COB_OPS_PER_TICK_LIMIT)`; if still alive
`terminate_thread(e, slot, "sync")` (queries never sleep in shipped data); copy
`e->threads[slot].stack[0..n_args-1]` into args_inout; return 0.

E3. src/render/units.c near g_ta_scale (:669): add

```c
/* Sim-side copy of the default TA_SCALE. The render tunable ('-'/'=')
 * must not leak into sim results. */
#define UNIT_MODEL_TO_WORLD 0.000021f
```

E4. src/render/units.c forward decls (~:142): add
`static int unit_factory_build_spot(Unit *f, int32_t *out_x, int32_t *out_y);`
and `static int unit_water_depth_ok(const struct GameWorld *w, const UnitDef *d, int32_t x, int32_t y);`
(second one is Issue 2, needed above :1224).

E5. src/render/units.c after compose_node_xforms (~:5037): implement

```c
/* QueryBuildInfo piece -> world spot (legacy :9347-9362). Returns 0 when
 * the factory has no usable spot (no cob/mesh/script/piece) — caller
 * falls back to the factory centre. Runs the script (side effects are
 * legacy behavior: verasy swings its dock arm per call). */
static int unit_factory_build_spot(Unit *f, int32_t *out_x, int32_t *out_y) {
    if (!f || !f->cob || !f->cob->script) return 0;
    const UnitDef *fd = Units_GetDef(f->def_idx);
    int c = f->team_color_idx; if (c < 0 || c > 11) c = 0;
    const UnitMesh *m = fd ? fd->mesh_per_color[c] : NULL;
    if (!m) return 0;
    if (Cob_FindScript(f->cob->script, "QueryBuildInfo") < 0) return 0;
    int32_t qa[4] = { 0, 0, 0, 0 };            /* legacy pushes 4 (:306156-306193) */
    if (Cob_RunScriptSync(f->cob, "QueryBuildInfo", qa, 4) != 0) return 0;
    int piece = qa[0];
    if (piece < 0 || piece >= f->cob->script->num_pieces) return 0;
    int node = f->cob->piece_to_node[piece];
    if (node < 0 || node >= m->node_count) return 0;
    NodeXform xf[UNIT_MESH_MAX_NODES];         /* 128*48B = 6 KB stack, fine */
    compose_node_xforms(m, f->cob->pieces, xf);
    float mx = xf[node].trans[0], mz = xf[node].trans[2];
    float ch = cosf(f->heading), sh = sinf(f->heading);
    /* Same heading+mirror map as submit_run (:5161-5165). */
    float rx = -(ch * mx + sh * mz);
    float rz = -(sh * mx - ch * mz);
    *out_x = f->world_x + (int32_t)lroundf(rx * UNIT_MODEL_TO_WORLD);
    *out_y = f->world_y + (int32_t)lroundf(rz * UNIT_MODEL_TO_WORLD);
    return 1;
}
```

NOTE: must be defined after NodeXform (:1699) and compose_node_xforms (:4927);
forward decl (E4) keeps call sites at :1338 legal. Mesh is baked at spawn
(Units_Spawn → ensure_mesh_baked, :2700), so live factories always have one;
headless tests without atlases fall back to centre.

E6. src/render/units.c:1336-1343 — replace the factory branch:

```c
    int factory_production =
        (bd->max_velocity > 0.0f && ud->max_velocity <= 0.0f);
    if (factory_production) {
        /* Spawn at the COB build spot (:9347-9362). A stateful script
         * (verasy dock arm) offers a different spot per call — legacy
         * retries on a timer (:9374-9382); two bounded attempts cover
         * both arm sides. */
        GameWorld *w = World_Get();
        world_x = u->world_x;
        world_y = u->world_y;
        int placed = 0;
        for (int attempt = 0; attempt < 2 && !placed; attempt++) {
            int32_t sx, sy;
            if (!unit_factory_build_spot(u, &sx, &sy)) break;
            if (!unit_water_depth_ok(w, bd, sx, sy)) continue;
            world_x = sx; world_y = sy; placed = 1;
        }
        /* Product depth gate also applies to the centre fallback —
         * ships need water wherever they materialise (:9363-9373 runs
         * Terrain_FindBuildPlacement with the PRODUCT def). */
        if (!placed && !unit_water_depth_ok(w, bd, world_x, world_y))
            return -1;
    } else if (!Units_IsBuildSiteClear(building_def_idx, world_x, world_y)) {
        return -1;
    }
```

E7. src/render/units.c:1352 — product heading: after the `Unit *bu` block, add
`if (factory_production) bu->heading = u->heading;` (product faces the way the
yard faces → walks straight out past the spot; placement builds keep
build_heading_for_def).

E8. src/render/units.c:4534-4553 — exit walk, non-rally branch: before the 4-dir
scan, try continuing outward along the spawn-spot direction:

```c
    int32_t dx = bt->world_x - u->world_x;
    int32_t dy = bt->world_y - u->world_y;
    if (dx != 0 || dy != 0) {
        float len = sqrtf((float)dx * dx + (float)dy * dy);
        int32_t ex = bt->world_x + (int32_t)(dx * 48.0f / len);
        int32_t ey = bt->world_y + (int32_t)(dy * 48.0f / len);
        if (unit_terrain_walkable(wgw, btd, ex, ey)) {
            bt->cmd_kind = UNIT_CMD_MOVE; bt->cmd_x = ex; bt->cmd_y = ey;
            bt->target = -1; unit_clear_path(bt);
        }
    }
    if (bt->cmd_kind != UNIT_CMD_MOVE) { /* existing 4-dir scan 4535-4552,
        but centre the scan on bt->world_x/y instead of u->world_x/y */ }
```

(Keep the scan's per-candidate `unit_terrain_walkable(wgw, btd, …)` — it already
uses the product def, so ships path into water, footmen onto land.)

Optional cleanup: cob_host_call_function case 7 PIECE_XZ (units.c:3029-3036)
can now return real piece coords via the same compose path (separate change;
needs piece index plumbed from args).

---

## Issue 2 — naval products / water-depth gating

### Legacy mechanism

- The build-placement search (:219074) applies a depth rule at :219149-219157
  (dig: combat-mechanics.md §9): per footprint tile, tile minH < water −
  **maxwaterdepth** → fail; tile maxH > water − **minwaterdepth** → fail. The two depth
  bounds are taken from the **resolved move class**, not from the def's own keys
  (:163199-163202); a def with no `movementclass` gets a scratch class synthesised from
  its own FBI keys (:163182-163192, class reader :187426-187467, which chains defaults).
- The factory path checks the **PRODUCT** def at the build spot (:9363-9373) —
  today TAK-RE checks nothing in the factory branch, and the placement branch
  (`Units_IsBuildSiteClear`, units.c:1224) checks only slope/features/unit-AABB
  (Terrain_IsWalkable, :1237) — no water at all. So land buildings place on flat
  water and ships "build" on land.
- Shipped data: naval yards are LAND structures — vercastl.fbi / verasy.fbi author
  `maxwaterdepth = 0` (no minwaterdepth); ships carry water via movementclass
  WATER2..WATER5 (MinWaterDepth 13-15, moveinfo.tdf). VERASY's canbuild list is
  all ships; VERCASTL mixes GROUND2/3, HOVER2, one flyer. ARABUILD (mobile) can
  placement-build ARAWAR (WATER4); ZONTRAIN (mobile) placement-builds ZONKRAK
  (WATER4).

### Latent bug found (must fix first)

src/game/moveinfo.c:31 defaults `MaxWaterDepth` to **0**. WATER2-5 and HOVER2/3
author no MaxWaterDepth → mc->max_water_depth = 0 → `depth > max` fails in ANY
water at src/game/pathing.c:127-135 and units.c:3462-3474. Ships/hovers currently
cannot legally stand/path anywhere wet. Legacy chained defaults resolve these to
"unbounded" (hovercraft demonstrably cross deep water). Fix: default 255.

### Edits

F1. src/game/moveinfo.c:31 — `mc->max_water_depth = TDF_ReadInt(tdf, "MaxWaterDepth", 255);`
(comment: classes that author only MinWaterDepth are unbounded above — WATER*/HOVER*;
0 default landlocked every ship/hover). :33-34 BadMaxWaterDepth default already
follows max_water_depth — keep.

F2. src/game/test_moveinfo.c (~:52) — add
`ASSERT_EQ_INT(255, water3->max_water_depth);` and for HOVER2 if convenient.

F3. src/render/units.c — factor the water gate out of unit_terrain_walkable
(:3462-3474) into

```c
/* [min,max] water-depth window for the def's resolved move class
 * (legacy :219149-219157; class resolve :163199-163202). */
static int unit_water_depth_ok(const GameWorld *w, const UnitDef *def,
                               int32_t x, int32_t y) {
    if (!def || def->can_fly) return 1;
    const MoveClassDef *mc = unit_move_class(w, def);
    int min_wd = mc ? mc->min_water_depth : (def ? def->min_water_depth : 0);
    int max_wd = mc ? mc->max_water_depth : (def ? def->max_water_depth : 0);
    if (!w || w->water_height <= 0) return min_wd <= 0;  /* dry map: no ships */
    int raw_h = Terrain_SampleHeight(w, x, y) + 32;      /* undo display bias */
    int depth = w->water_height - raw_h;
    if (depth < 0) depth = 0;
    if (depth > max_wd) return 0;
    if (min_wd > 0 && depth < min_wd) return 0;
    return 1;
}
```

unit_terrain_walkable (:3462-3474) becomes `if (!unit_water_depth_ok(w, def, x, y)) return 0;`
(keep its Terrain_IsWalkable + can_fly shortcut as-is). NOTE definition order:
place unit_water_depth_ok next to unit_move_class (:3437) and rely on the E4
forward decl for the :1224/:1338 call sites. unit_move_class takes `const GameWorld*`
— fine.

F4. src/render/units.c:1235-1239 (`Units_IsBuildSiteClear` sample loop) — add the
water gate per sample:

```c
    for (int sy2 = y0; sy2 <= y1; sy2 += 16) {
        for (int sx2 = x0; sx2 <= x1; sx2 += 16) {
            if (!Terrain_IsWalkable(world, sx2, sy2, d->max_slope)) return 0;
            if (!unit_water_depth_ok(world, d, sx2, sy2)) return 0;
        }
    }
```

Effects, all automatic:
- HUD placement ghost (src/ui/hud.c:1076) turns red over water for land
  buildings and over land for builder-placed ships — the user-facing naval-yard
  placement fix. Naval yards (maxwaterdepth 0) place on land at the shore,
  exactly like legacy; their dock buildspot reaches the water.
- AI siting (src/game/ai.c:303-316, :495) skips invalid tiles.
- ZONTRAIN→ZONKRAK / ARABUILD→ARAWAR placement path now requires water.

F5. Factory-branch product gate — already in E6 (`unit_water_depth_ok(w, bd, spot)`
with bd = PRODUCT def). This is the direct fix for "ships buildable on land".

F6. Queue-advance robustness — a refused product no longer wedges silently:
- units.c:1421-1428 (cancel-advance) and :4557-4568 (complete-advance): capture
  the Begin return; on failure `fprintf(stderr, "Build: %s needs water at the
  build spot\n", …)` and loop to the next queued def until one starts or the
  queue empties (wrap the existing pop in a `while (u->prod_queue_len > 0)` that
  breaks on success). Legacy stalls+retries instead; dropping-with-log is the
  simpler deviation — note it in the code comment.

F7. src/ui/hud.c:1005-1006 — check the enqueue result:
`if (Units_FactoryEnqueue(sel[0], bs->def_idx) == 0) GameSound_PlayUI("addbuild");
else GameSound_PlayUI("MenuButton");` (idle factory with a landlocked dock now
refuses immediately; don't play the success cue).

F8. src/ui/test_ui_screens.c:2021-2042 (production spot-check) — VERASY spawned
on land can no longer start any entry (all products are WATER ships). Amend: after
the m-loop, if `started < 0`, tolerate the failure iff every menu entry's def
resolves to a move class with `min_water_depth > 0`
(`TAK_MoveInfo_Find(&World_Get()->moveinfo, pd->movement_class)`); print
`"(naval-only factory on land: %s) "` and continue instead of asserting.
Other builders keep the hard ASSERT.

---

## Verification

1. `cmake --build build --target tak-re test_moveinfo test_ui_screens test_ai test_cob_vm`
2. ctest: test_moveinfo (new 255 assert), test_ui_screens (amended production
   check), full suite for regressions.
3. Manual: two castles / any coastal map —
   - VERCASTL: queue a musketeer → nanoframe appears ~180 px in front of the
     doors (not under the castle), fades in ≥50% HP, walks outward on finish;
     rally still wins when set.
   - VERASY placed on a shore with water to the south: ships spawn at the dock
     spot in water and sail off; VERASY inland: ship buttons refuse (no addbuild
     cue), stderr log on queue advance.
   - Placement ghost: land building red over water; VERASY red over water,
     green on shore; ARABUILD's ARAWAR ghost green only on water.
   - Hover (verlihr) paths across water again (moveinfo default fix).

## Risks / notes

- UNIT_MODEL_TO_WORLD duplicates the default g_ta_scale; if the render tunable is
  changed at runtime the spawn spot no longer matches the drawn dock pixel-perfectly
  (sim determinism > visual nicety).
- Stateful QueryBuildInfo scripts run once per Begin attempt (2 bounded attempts)
  vs legacy's indefinite timed retry (:9374-9382). Yard-occupancy (unit-overlap)
  clearance at the spot is NOT checked (legacy does, :219134-219147) — unchanged
  from today (centre spawn also never checked); revisit with the occupancy grid.
- Factories never rendered AND never spawned through Units_Spawn would lack a
  baked mesh → centre fallback; all real paths go through Units_Spawn (:2700).
- Move-class footprint override (legacy :163193-163195 replaces def footprint with
  class footprint for classed units) is still unimplemented — noted, out of scope.
