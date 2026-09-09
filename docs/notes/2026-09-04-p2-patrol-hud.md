# P2 diagnosis: patrol loop · "Sweep" button · HUD empty state

Line numbers = working tree @ 0537f7e (return-fire commit).

Legacy sections describe behaviour derived from analysis of the retail binary. `:NNNNN`
citations are evidence pointers into a private reference that is not distributed. They
mark where a claim can be re-checked by whoever holds it, and nothing here reproduces
that file's expression. Symbol names marked *(tool-assigned, misleading)* are automatic
labels that do **not** describe what the routine does. The description beside them does.
The transcript in §1 is output from our own test harness.

---

## 1. Patrol "doesn't loop"

### Finding: the engine bounce EXISTS and WORKS, verified empirically

The A↔B swap has been in since 9308747:

- Order issue: `Units_CommandPatrolSelected` src/render/units.c:972-986 sets
  `cmd_kind=PATROL`, `cmd=click`, `patrol=current pos`, clears target/path, kicks walk.
- Tick drive: desired chain units.c:4241-4244 (PATROL → MOVING toward cmd).
- Bounce: MOVING-arrival handler units.c:4328-4336 swaps cmd↔patrol + `unit_clear_path`.

Verified with a temporary tick-to-arrival probe inserted in src/ui/test_ui_screens.c
after the patrol asserts (:419-421), calling `Units_TickEngines()` in a loop
(built `test_ui_screens` Debug, real skirmish map + defs, then reverted):

```
PATROL PROBE: unit=ARASWORD vmax=1.10 A=(1104,2800) B=(1232,2800)
reached_b=407 back_a=944 again_b=1355 cmd_kind=4 ...
```

Unit reached B, returned to A, reached B again, and `cmd_kind` was still PATROL. It loops
forever. Do NOT rewrite the swap. The "moves once" playtest symptom comes from the
paths below (any of which reads as "patrol is broken" in-game).

### Real breakers + fixes

**(1a) Mission-scripted patrol is issued as a plain MOVE, so it moves once and stops.**
src/ui/loading.c:209-214: `MISSION_CMD_PATROL` falls into the `MISSION_CMD_MOVE`
case → `Units_CommandMoveUnit` → `UNIT_CMD_MOVE` → arrival clears to NONE
(units.c:4322-4324). Every campaign/TDF-placed patroller walks its leg once and
idles. Legacy: patrol is a standing order driven by the waypoint driver at :11565+,
order type 9, repeat count −1 (digs combat §7,
docs/notes/2026-09-04-combat-mechanics.md).

- src/render/units.c: add `Units_CommandPatrolUnit(int handle, int32_t x, int32_t y)`
  next to `Units_CommandMoveUnit` (:939-947), with the same body but
  `cmd_kind=UNIT_CMD_PATROL` and `patrol_x/y = world_x/y` (mirror :973-977).
- include/tak_unit.h: declare next to `Units_CommandMoveUnit` (:754).
- src/ui/loading.c:211: give `MISSION_CMD_PATROL` its own case calling it
  (`cmd->a*16, cmd->b*16`). Multi-waypoint TDF patrols degrade to a last-leg 2-point
  bounce. That is acceptable until an order queue exists. Note it in the comment.

**(1b) Patrolling units never engage, and once engaged (via return-fire, 0537f7e)
the patrol is permanently forgotten.** Legacy patrol = standing order that
fights en route and resumes (digs combat §7 :136-140).

- Auto-acquire gate units.c:4097 requires `cmd_kind == UNIT_CMD_NONE` → a patroller
  walks past enemies without firing. Extend the gate to
  `(cmd_kind == UNIT_CMD_NONE || cmd_kind == UNIT_CMD_PATROL)` but when acquiring
  from PATROL set ONLY `u->target`. Do NOT overwrite `cmd_kind = UNIT_CMD_ATTACK`
  (:4110). The desired chain's `else if (u->target >= 0)` branch (:4191) then
  engages while cmd stays PATROL (ATTACKING case :4574+ keys on target, not cmd,
  and the chase branch never touches cmd_x/cmd_y so the route survives).
  Keep the AI-pursuit sub-block (:4122) NONE-only as is.
- Target death/invalid: validation block :4070-4083 already clears only `target`
  for PATROL (cmd list :4075-4079 excludes it) → route resumes next tick. No edit.
- `unit_on_damaged` units.c:500-518 (0537f7e): currently sets
  `victim->cmd_kind = UNIT_CMD_ATTACK` for PATROL victims, which loses the patrol. Same
  rule: when `cmd_kind == UNIT_CMD_PATROL` set only `victim->target`
  (+`unit_clear_path`), and keep the ATTACK promotion for NONE.
- Melee kill clear units.c:3930-3936 (`fire_weapon_shot`): sets shooter
  `cmd_kind = UNIT_CMD_NONE` unconditionally when its target dies, which kills patrol
  after one melee kill. Guard: only reset when `cmd_kind == UNIT_CMD_ATTACK`
  (projectile paths :605-650 don't touch the shooter, so no edit there).

**(1c) Broom-button confusion.** See §2. With a reclaimer/monarch selected the
visible broom button arms UNLOAD mode. The world click then either no-ops
(non-transport is rejected by `Units_CommandUnloadSelected` :1148) or walks once
and stops (:4325-4327), which is easily reported as "patrol/sweep moves once".

---

## 2. HUD "Sweep" button shows the load/unload cursor

### What "Sweep" is

No `SWEEP` widget exists in any .gui (grepped all of data/extracted). The button the
tester saw is **CLEAR**, whose art is a broom (verified by decoding
igcommonbuttons.gaf entry 7 `ClearButton`, 29×29 whisk-broom icon). Legacy CLEAR =
reclaim order (caps `canreclaim`, e.g. araking/arabuild/arapries.fbi), cap parse ok
at units.c:1876.

### Root cause: overlapping rects + visibility-blind first-match hit test

araingame.gui authors two contextual button pairs at the SAME rect
(data/extracted/data/guis/araingame.gui):

| widget | gui line | rect | art |
|---|---|---|---|
| HEAL | :766 | 529,223 29×29 | RepairButton |
| LOAD | :785 | 529,223 29×29 | PickupButton |
| CLEAR | :804 | 599,223 29×29 | ClearButton (broom) |
| UNLOAD | :823 | 599,223 29×29 | DropButton |

Legacy shows at most one of each pair by setting widget visibility in the unit-menu
refresh (:150749-150831, and the routine carries an address-suffixed automatic label, not
a real symbol): LOAD/UNLOAD by transport flag :150749-150775, HEAL :150776-150792, CLEAR
:150793-150808, shared backings :150809-150831. Our HUD does set visibility the same way
(hud.c vis[] :552-584: LOAD/UNLOAD ↔ CAP_TRANSPORT, HEAL ↔ CAP_REPAIR, CLEAR ↔
CAP_RECLAIM), but two things break it:

- Slot rebuild hud.c:719-741 walks `g_button_bindings` (:112-132) with
  `find_widget_rect` (:287-293 → `GUIRuntime_WidgetByName`), which ignores the
  runtime hidden flag, so BOTH members of each pair land in `g_action_slots`.
- Binding order lists LOAD(idx 4)/UNLOAD(idx 5) BEFORE HEAL(6)/CLEAR(7), and
  `HUD_HandleSidebarClick` :1014-1026 takes the FIRST rect hit.

So clicking the visible broom (CLEAR) matches the hidden UNLOAD slot → mode
`HUD_CMD_UNLOAD` → CursorUnload. Clicking the heal hand matches LOAD → Cursorload.
That is exactly the reported "Sweep shows the load/unload cursor".

The cursor table itself is CORRECT, verified against actual cursors.gaf entry
order (probe_hud_gaf): 0 CursorAttack, 1 CursorDefend, 5 Cursorload, 6 CursorMove,
8 CursorPatrol, 15 cursorrepair, 16 CursorUnload, 22 Cursorreclamate. Those match
every `cursor_entry` in g_button_bindings. (The rendering dig's name list
:174EC-1753C is legacy handle-load order, not file order, so don't "fix" indices.)

### Edits

- src/ui/gui_render.c: add getter next to `GUIRuntime_SetWidgetVisible` (:427-435):
  `int GUIRuntime_WidgetHidden(const GUIRuntime *rt, const char *name)`. Same
  stricmp scan, return `caches[i].hidden`, and 0 when not found. Declare in
  include/tak_gui_render.h near :78.
- src/ui/hud.c slot rebuild: after the `find_widget_rect` check (:726) add
  `if (GUIRuntime_WidgetHidden(g_rt, g_button_bindings[i].widget_name)) continue;`
  Only the visible member of each pair gets a slot, and the broom now arms
  `HUD_CMD_CLEAR` with Cursorreclamate. (Legacy pairs never conflict: transports get
  LOAD+UNLOAD, healers/reclaimers get HEAL+CLEAR, on different rects.)
- Behavior on world click is already routed (src/ui/ingame.c:701-703 →
  `Units_CommandReclaimSelected`). Known gap, out of scope here: reclaim only
  targets unit handles. Legacy sweep reclaims map features/corpses too. Leave a
  comment, don't grow this patch.

---

## 3. HUD empty state (n_sel == 0) doesn't match legacy

### Legacy idle state

- Right sidebar: the unit-menu refresh at :151133-151166 hides EVERY UnitMenu
  child EXCEPT `CrystalBall`, `HelpText`, `PositiveM`, `NegativeM`. The UnitMenu
  panel itself (sidebar background, ButtonPanel art, gui :367 rect 512,128,128×352)
  stays visible.
- Bottom bar: the HUD unit-info refresh at :152219 *(tool-assigned
  `NetPacket_Method06`, misleading: this is HUD code, not networking)*, called with a
  null unit (:152277-152296), hides UnitText, HealthBar+HealthBack, ManaBar+ManaBack,
  Experience. KillCount is hidden at :152496-152506. Its aggregate-selection sibling at
  :152512+ does the same bar-hiding for the multi-select state. The InfoPanel strip art
  stays.

### Ours today (hud.c HUD_Draw)

- vis[] :552-584 gates `UnitMenu` (:578) and `CrystalBall` (:579) on
  `UNIT_CAP_BUILDER`. With nothing selected (caps==0) or ANY non-builder selected
  the whole sidebar background art disappears (world/black shows through the
  reserved 128px strip), and the crystal ball never shows for non-builders.
  Both are wrong vs :151146-151152 (always visible).
- Action buttons: hidden when caps==0, which matches legacy idle. OK.
- Bottom bar: player-mana fill :801-817 paints the ManaBar rect UNCONDITIONALLY →
  idle shows a full blue bar. Legacy hides the bar (it's the selected unit's
  gauge). The authored HealthBar/ManaBar/gauge widget art is also never hidden.
- Portrait/health fill/name already gate on selection (:744-795), OK. UnitText/
  ActionText refreshed to "" when nothing selected (:694-699), OK.

### Edits (all src/ui/hud.c unless noted)

- vis[] :577-579: keep `BuildMenu` builder-gated (legacy :150134). Change
  `UnitMenu` → always visible (drop the entry, or `1`). Change `CrystalBall` →
  always visible.
- Add to the same per-frame visibility pass (uses `sel_count`, so move/compute the
  selection fetch (:744-745) above vis[] or read it there):
  `HealthBar`, `ManaBar` (+ gauge widgets `GuageA`/`GuageB`/`Guagea`/
  `GuagesElements` if they render standalone), `Experience`, `UnitLevels`,
  `UnitPic` → visible only when `sel_count > 0`. Mirrors :152277-152296.
- Mana fill :801: add `sel_count > 0` to the condition so the blue fill only draws
  with a selection (stand-in for per-unit mana until PARITY row 138 lands).
- Leave `HelpText`/`PositiveM`/`NegativeM`/BottomBar/EndCap/InfoPanel untouched
  (legacy whitelist :151146-151152, and the +N/−N refresh :701-709 already runs
  always).

---

## Test plan

- Regression: make the §1 probe permanent in src/ui/test_ui_screens.c (after :421,
  before the `Units_CommandStopSelected`): tick `Units_TickEngines()` up to 20000×,
  assert reached B → back at A → B again and `cmd_kind == UNIT_CMD_PATROL`
  throughout (probe code validated this session, thresholds d²≤100).
- Mission patrol: in src/game/test_mission.c or ui test, apply a
  `MISSION_CMD_PATROL` placement and assert the spawned unit's
  `cmd_kind == UNIT_CMD_PATROL` and `patrol_x/y == spawn pos` (today it's MOVE).
- Sweep routing: unit test `GUIRuntime_WidgetHidden` (set visible 0/1 by name,
  assert getter). HUD-level: with a CAP_RECLAIM def selected, run HUD_Draw once,
  then `HUD_HandleSidebarClick` at the CLEAR rect center scaled to window px, then
  assert `HUD_GetCommandMode() == HUD_CMD_CLEAR`, not UNLOAD.
- Empty state: after `Units_SelectSingle`→deselect, HUD_Draw, assert
  `GUIRuntime_WidgetHidden(g_rt,"UnitMenu") == 0` and
  `GUIRuntime_WidgetHidden(g_rt,"HealthBar") == 1`.
