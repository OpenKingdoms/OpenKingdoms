# P2 dig: projectiles invisible; LOS lightning = 1-frame flash

Symptoms: no arrows/cannonballs/magic bolts render in-game; Line-of-Sight magic
(lightning) flashes for ~1 frame. Legacy: projectiles drawn as model/weaponart GAF
(digs rendering §Projectiles), LOS weapons display for emittime, soundstart at fire
(digs combat §4).

Legacy sections describe behaviour derived from analysis of the retail binary. `:NNNNN`
citations are evidence pointers into a private reference that is not distributed — they mark where a claim can be re-checked by whoever holds it, and
nothing here reproduces that file's expression. Symbol names quoted for legacy routines are
analyst/tool-assigned labels, not real symbols.

NOTE ON LINE NUMBERS: `src/render/units.c` was being edited by parallel patch agents
while this dig ran (Units_Render drifted 6089→6118 during the session). Cites below are
function-name + line as of 2026-09-08; re-grep the function name before editing.

## Diagnosis

**Root cause 1 — projectile speed is 16× too fast (fixes "nothing renders").**
- units.c:1955-1956 (FBI weapon parse): `w->velocity_pps = wv * 16` with comment "One TA
  unit = 16 world pixels". Wrong: FBI `weaponvelocity` shares the unit system with FBI
  `range`, and `range` is consumed UNSCALED as world pixels everywhere
  (weapon_effective_range units.c:3790; compared to world-px d² at units.c:4228).
- Data proof: arabow.fbi WEAPON1 `range = 550` / `weaponvelocity = 530` → ~1.04 s flight
  at max range in legacy. araking.fbi WEAPON1 (lightning) `range = 250` /
  `weaponvelocity = 500`. Retail velocities span 10..5000.
- Legacy proof: `weaponvelocity` is read as a float straight into the weapon record with no
  scaling applied (:249987-249989); the per-substep clamp is at :250425; a projectile's
  lifetime is distance / velocity when `weapontimer` is 0 (:245980-245986).
- Effect: spawn_projectile units.c:375 `p->speed_ppt = speed_pps / 60.0f` (the /60 is
  correct for the 60 Hz sim, tak_gameloop.h:22) → ARABOW arrow = 530*16/60 = 141 px/tick.
  Max-range flight = 4 ticks; lightning ≤ 2 ticks. Worse: tick_projectiles (units.c:583)
  runs INSIDE Units_TickEngines right after Units_TickCombat (units.c:4691-4693), so a
  shot spawned this tick immediately moves 141 px and segment-hit-tests
  (point_segment_dist2_i32, 24 px radius, units.c:631-634) — any target within ~165 px
  dies the same tick, before the frame ever renders. Hence 0-2 visible frames.
- NOT the draw pass: render_projectiles (units.c:5774) exists and is called from
  Units_Render (units.c:6133); fog gate is fine (projectile_visible_to_local_player
  units.c:167 returns 1 for player 1's own shots). git log -S: `wv * 16` unchanged since
  db55185, which PREDATES the projectile system (9308747) — never worked, not a regression.

**Root cause 2 — LOS weapons have no behavior branch; they fly as ballistic bolts.**
- `type = Line of Sight` is only sniffed for the placeholder color
  (weapon_visual_kind units.c:3756-3769 → VIS_MAGIC); firing goes through the same
  fire_weapon_shot → spawn_projectile path. With bug 1, lightning's 500*16 velocity over
  250 range = the observed 1-frame flash; with bug 1 fixed alone it would become a slow
  wrong bolt instead of a held beam.
- Legacy: the weapon-type dispatch at :249725-249983 gives Line_of_Sight (lightning,
  fire, bluefire, dieselflame, mindcontrol, turntostone) its own behavior object: an instant
  ray plus an `hweffect` drawn for `emittime` frames at 30 Hz (`emittime` is read at
  :247444 with a default of 30; araking emittime=15 = 0.5 s; retail values 10..55). Weapon
  FBI carries `innercolor/middlecolor/outercolor` RGB triples + `hweffect = lightning`
  (araking.fbi WEAPON1). Neither emittime nor the colors are parsed today
  (weapon parse block units.c:1916-1995).

**Root cause 3 — soundstart neither parsed nor hooked.**
- Parse block units.c:1931-1934 reads only soundhitclass/soundhit. Legacy parses
  `soundstart` into the weapon record (:250159-250166) and plays it positionally at each
  burst emission, at priority 4, gated on the `soundtrigger` flag (:245977-245979).
- Caveat: retail TAK FBIs contain ZERO soundstart/soundtrigger keys (grepped all of
  data/extracted). Audible fire sounds come from COB FireWeapon `play-sound`
  (opcode 0x10072000; host cob_host_play_sound units.c:2942, wired units.c:2778; digs
  save-sound §B), which already runs — fire_weapon_shot starts the FireWeapon thread
  (units.c:3920-3922). So soundstart is a small parity hook, not the audio fix.

## Edits

### E1 — velocity unscale + TTL headroom (fixes arrows/cannon/magic bolts)
1. units.c:1953-1956: `w->velocity_pps = wv;` — delete the `* 16`; comment: velocity is
   world px/sec, same unit system as range (legacy :249987 stores it raw).
2. spawn_projectile units.c:404-406: raise TTL clamp 600 → 1200. (velocity-45 weapons at
   long range need >600 ticks; velocity-10 `subtype=Dropped` egg bombs are a separate
   legacy Ballistic(Dropped) behavior — out of P2 scope, leave a note.)
3. No change to `/ 60.0f` at units.c:375 (60 Hz sim is right; legacy 30 Hz uses v/30).
   Post-fix sanity: ARABOW 8.8 px/tick, ~62 ticks max range ≈ 1 s. Matches legacy feel.

### E2 — LOS beam behavior (fixes lightning duration)
1. include/tak_unit.h: UnitWeapon (line ~128) add: `int32_t emit_ticks;`
   `uint8_t beam_inner[3], beam_middle[3], beam_outer[3];` `uint8_t is_los;`.
   Projectile (line ~172) add: `uint8_t is_beam; int32_t src_x, src_y;`.
2. units.c weapon parse (after :1956): `w->is_los = ascii_contains_ci(w->type, "line of sight");`
   `w->emit_ticks = TDF_ReadInt(tdf,"emittime",30) * 2;` (30 Hz frames → 60 Hz ticks);
   parse the three color keys via TDF_ReadString + sscanf "%d %d %d", defaults
   white / 200,230,255 / 180,200,255 (araking values).
3. fire_weapon_shot (units.c:3912), after the melee branch: if `wp->is_los` — apply damage
   NOW (same code shape as the projectile hit block: weapon_damage_for_category via
   projectile fields, or apply_projectile_area_damage for aoe>0), play hit sound at the
   target (play_projectile_hit_sound semantics, units.c:521), then spawn a beam pool entry:
   alive=1, is_beam=1, src_x/y = shooter pos, world_x/y = dest_x/y = target pos,
   speed_ppt=0, ttl_ticks = wp->emit_ticks, visual copies of beam colors (stash in a small
   `uint8_t beam_rgb[3][3]` on Projectile or index the weapon), then `return` (skip
   spawn_projectile). Same branch in fire_ground_shot (units.c:3981) aimed at cmd_x/y.
4. tick_projectiles (units.c:583) top of loop, after `!p->alive` check:
   `if (p->is_beam) { if (--p->ttl_ticks <= 0) p->alive = 0; continue; }` — no movement,
   no hit test (damage already applied at fire).
5. render_projectiles (units.c:5774): beam branch before the disc code — jagged polyline
   src→dest: 10 segments, perpendicular jitter ±6 px from
   unit_deterministic_noise(i, g_construct_anim_tick, seg) (units.c:328; the tick counter
   increments per rendered frame at units.c:5931, giving free flicker); draw 3 passes
   outer(alpha ~90, 3 offset lines for width — same thickness trick as
   render_selection_rings units.c:5747-5762) → middle → inner(alpha 255, 1 px). Terrain-
   height-correct both endpoints like the disc path (units.c:5785-5787).
6. Keep weapon_visual_kind unchanged (beams never reach the disc renderer).

### E3 — soundstart hook (parity; retail data never sets it)
1. tak_unit.h UnitWeapon: `char start_sound[24];`
2. units.c parse block next to :1933: `copy_bounded(w->start_sound, ..., TDF_ReadString(tdf, "soundstart", ""));`
3. fire_weapon_shot + fire_ground_shot, right after start_weapon_script(FireWeapon):
   `if (wp->start_sound[0]) GameSound_PlayWorldWav(wp->start_sound, 0x7f, u->world_x,
   u->world_y, cam/viewport args as in play_projectile_hit_sound units.c:530-532);`
   Per-shot call sites already repeat per burst (tick_weapon_burst units.c:4011) = legacy
   per-emission cadence (:245977). Do NOT add a soundtrigger gate — legacy required it,
   but with zero retail users the unconditional play is harmless and mod-friendlier;
   note it in the comment.

### E4 — verification
1. test_ui_screens.c: ARABOW fires at a target 400 px away → after 10 sim ticks
   Units_GetProjectiles count ≥ 1 and the projectile is >30 ticks from despawn; target HP
   unchanged until ~45 ticks. ARAKING lightning fire → target damaged on the fire tick,
   beam entry alive for emit_ticks (30), gone after.
2. Manual: skirmish probe — arrow visibly arcs ~1 s to max-range targets; lightning holds
   ~0.5 s with flicker; bow twang from COB FireWeapon still plays.

## Follow-ups (out of P2 scope, note only)
- Real projectile art: legacy draws the weapon `model` 3DO (araarrow.3do exists in
  data/extracted/data/objects3d/) through the normal unit renderer with heading/pitch from
  velocity + spinroll (the projectile angle-setting routine, :250646; digs rendering
  §Projectiles); weaponart GAF for sprite weapons. Replaces the placeholder discs.
- Ballistic(Dropped) egg-bomb behavior (velocity 10-45, subtype=Dropped).
- weapontimer/holdtime — both are fields on the legacy weapon record and are unused by
  retail FBIs; skip until a mission weapon needs them.
