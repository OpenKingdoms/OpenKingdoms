# Behaviour note: combat / unit mechanics (2026-09-04)

Derived from behavioural analysis of the retail binary. `:NNNNN` citations are
evidence pointers into a private reference that is not distributed — they mark where a claim can be re-checked by whoever holds it,
and nothing here reproduces that file's expression.

Function names below are **our** descriptive labels unless marked otherwise.
Names marked *(tool-assigned, misleading)* are automatic labels that do
not describe what the routine does; the description beside them does. Legacy
in-memory struct offsets have been replaced by field names — we define our own
structures, so the offsets carry no implementation value. The handful of fields
whose meaning is not yet established are listed under Open items, where the
offset is kept because it is the only handle on them.

The legacy simulation runs at **30 Hz**, and FBI rate keys convert at ×1/30.
⚠️ `include/tak_unit.h:145` says "60 Hz ticks" for `burst_rate_ticks` —
re-check that comment.

**Per-unit state**, by role:

| Field | Meaning |
|---|---|
| Mission | Current order/mission code |
| Weapon slots | Three of them, each with its own aim, reload and flags |
| Position | X / Y / Z, 16.16 fixed-point |
| Heading | Facing |
| My transport | Back-pointer to the transport carrying this unit |
| Carried head / next | Intrusive list of units this unit carries |
| Definition | Pointer to the unit type |
| Player | Owning player record |
| Cloak-suppressed-until | Frame number before which cloaking cannot re-engage |
| Unit mana | The unit's own mana pool (cloaking spends from this, not the player pool) |
| XP | Accumulated experience points |
| Veteran override | When set, forces max rank |
| Owner index | Player/colour index, also used for team colour |
| Kills | Kill count |
| Build fraction **remaining** | 1.0 when placed → 0.0 when complete |
| HP + HP fraction accumulator | Health, plus an 8-bit fractional carry for sub-1 HP heals |
| Health % cache | Current and previous health percentage, refreshed every 30 frames |
| State bits | Activated · cloaked · actively-building |
| Flags | Visible-to-local · want-cloak · cloak-suppressed · hp-changed · under-construction · alive · plus the active weapon slot in the top two bits |

## 1. XP / Veterancy (COMPLETE)

**Level** — `Unit_GetVeteranLevel` *(tool-assigned name `Minimap_GetSize`,
misleading)* :232934:

1. The `noveteran` definition flag (:163080) forces level 0.
2. Otherwise, if the unit's veteran-override field is set, it is the max rank.
3. Otherwise level = XP ÷ `experiencepoints` (definition key, **default 666**,
   :162918), clamped to a global rank cap.

**Multiplier** (:232971) = 1.0 + level × a global per-level scale — or the
override field used verbatim when it is set. The UI rank shown to the player
(:179332) is `max(1, (int)multiplier)`. **COB port 32 returns the raw level**
(:223313, case 0x20) — not the multiplier and not the UI rank.

**Award on kill** — `Unit_OnDeath` *(tool-assigned name
`Render_DrawCursor`, misleading)* :227303-227328:

- The killer's kill count increments.
- The killer's XP increases by the victim type's `experiencepoints`.
- Two counters on the killer's player record increment.
- A loss counter on the victim's owner increments.
- **No self-credit and no team credit** — the award is skipped when the
  victim's owner index equals the killer's (:227322).

**Bonuses.** `manapershot` is discounted by the veteran multiplier (:250539).
The projectile model swaps to `veteranmodel` when the weapon's `veteranlevel`
is at or below the unit's level (:246620, :246834, :246902). Two further
per-unit modifiers are scaled by the multiplier at :235859 — their meaning is
still open.

**OPEN:** the global rank cap and the global per-level scale must be read out
of the PE `.data` section. Both have console setters (:38338, :38324).

## 2. Cloaking — OUR BUG: `cancloak` DOES NOT EXIST

The gate is **`cloakcost` > 0** (:9760, :9773, :208663). `units.c:1498` reads a
nonexistent `cancloak` key.

FBI keys (:162947-162963):

| Key | Meaning |
|---|---|
| `init_cloaked` | Definition flag — unit starts cloaked |
| `cloakcost` | Mana per frame while cloaked and stationary (×1/30 from the FBI value) |
| `cloakcostmoving` | Mana per frame while moving; **defaults to `cloakcost`** |
| `mincloakdistance` | Enemy proximity that suppresses cloak; **forced to 80 when `cloakcost` > 0 and the key is 0** (:163619) |

**Drain, per frame** (:8700-8736). If the unit wants to cloak, is not
suppressed, and the current frame has passed its cloak-suppressed-until stamp:

1. Pick the moving cost if the unit is moving, otherwise the stationary cost.
2. If the unit's own mana pool covers it, spend it and turn cloak on (the
   script state toggle for cloak).
3. If it does not, set a lockout of the current frame + 90.

The unit's mana pool regenerates at the definition's `ManaRechargeRate` once
the unit is built (:8708). Toggling cloak (:9757, :9770) sets or clears the
want-cloak flag and calls the script's `StartCloaking` / `StopCloaking`.

**Decloak lockouts:**

| Cause | Lockout | Cite |
|---|---|---|
| Firing a weapon | **+600 frames (20 s)** | :245900 |
| Building / mobile-building | +300 | :12259, :12539 |
| Repairing | +300 | :12764 |
| Yard build | +300 | :13135 |
| Reclaim / capture | +300 | :32139, :32374 |
| Enemy inside `mincloakdistance` | +90, and sets the cloak-suppressed flag | :208660-208673 |
| Out of mana | +90 | :8726 |

**Visual.** The cloaked state bit puts the unit into render mode 5/6
(translucent) at :197442-197457, subject to a ghost definition flag and a
global see-all. The local player's own cloaked units also draw a white circle
at `mincloakdistance` (:180534).

## 3. Radar / detection — NO sonar, NO stealth flag in TAK

Only two range keys exist: `sightdistance` and `radardistance`
(:162920-162923).

`Game_UpdateVisibility` *(tool-assigned name `Terrain_FlushDirtyList`,
misleading)* :208607-208711 runs only when more than one player is in the game,
in four passes:

1. Clear the visible-to-local and cloak-suppressed flags on everything, then
   set visible-to-local on all own and allied units.
2. **Radar.** For each local unit that is alive, built, unparalyzed and has a
   non-zero radar range, visit every unit within that radius (converted to
   16.16) and set visible-to-local on enemies — **unless the enemy is
   currently cloaked** (:208580). Radar does **not** see through cloak, and
   **there is no blip state**: visibility is a single boolean and a detected
   unit renders as its full model.
3. Cloak proximity suppression.
4. True line of sight, for units not already flagged visible and not cloaked,
   against the local LOS map.

**Radius iterator** (:220374-220459): a spatial hash with its own dimensions,
10-byte cells and an intrusive list head per bucket; the bucket index is the
16.16 position shifted right by 23, i.e. **buckets are 128 world units (8 map
cells) square**. Distance is **2D, X/Z only**. The walk also follows each
unit's carried-unit chain, so units inside transports are visited.

**LOS cell mapping is height-compensated:**
`fogCellX = worldX ÷ 32`, `fogCellZ = (worldZ − worldY ÷ 2) ÷ 32`
(:208685, :208695, :250583-250596, minimap :208475). Fog reveal, the minimap
and the fire gates all use this same mapping — getting it wrong is silently
wrong everywhere there is a slope.

`LOS_UpdateAll` :167179 initialises the global explored mask (one 16-bit word
per fog cell, one bit per player) to 0x00 when `MappingOn` is set and 0xFF when
it is not, and fills each player's per-cell visibility byte array with 0 when
`LosOn` is set and 1 when it is not. An LOS entry snapshot (:167249) records
position, the definition's eye height and the sight range.

**Minimap blips** (:208442-208527) draw for units that are alive, either
fog-off or flagged visible, and not currently carried by a transport. Colours
bucket into air / floater / builder (:208481). Debug circles at :180559-180609.

## 4. Weapons

**TA fields that are ABSENT in TAK:** `tolerance`, `pitchtolerance`,
`targetmoveerror`, `accuracy`, `energypershot`, `metalpershot`.
**Present instead:** `AimTolerance`, `ManaPerShot`, `dontleadtargets`.

Weapon definition, by TDF key (:249668-250434):

| Key / field | Meaning |
|---|---|
| behaviour vtable | Per-weapon-type dispatch table |
| per-category damage | A red-black tree keyed by damage category (:250486), falling back to a default damage value |
| `areaofeffect` | Splash radius |
| `edgeeffectiveness` | Splash falloff at the edge |
| `range` | Maximum range, **floored at 2** |
| `minrange` | Minimum range |
| `reloadtime` | In frames |
| `switchreloadtime` | Reload applied when switching to this slot |
| `turnrate` | Turret turn rate |
| `burst` / `burstrate` | Shots per burst, and the gap between them |
| `sprayangle` | Burst spread |
| `randomdecay` | — |
| `holdtime` | — |
| `AimTolerance` | Aim gate, see below |
| `hoverattackdistance` / `hoverattackaltitude` / `airtoair` | Air attack geometry |
| velocity + substeps | Projectile speed and its substep count |
| `ManaPerShot` | Mana cost per shot |
| damage type | Low nibble of the flag word: 1 normal, 2 fire, 3 explosion, 4 paralyzer (:250385) |

`weapontype` dispatch (:249725-249983): Melee, Ballistic (Dropped),
Line_of_Sight (lightning, fire, bluefire, dieselflame, mindcontrol,
turntostone, ...), Guided, Wandering.

**Velocity substepping** (:250425): the per-frame velocity is split into
substeps of **at most 16 world units** each, rounded up.

**Per-frame tick** — `Unit_UpdateWeapons` :245872-245923, per slot:

1. Honour the weapon-switching selector.
2. Decrement the reload timer.
3. Update aim and target (:249255) → check aim settled (:249328) → fire
   (:249388).
4. On the script's shot flag for that slot: apply the **+600 frame cloak
   lockout**, spawn the projectile (:249423), and **auto-downshift to a cheaper
   weapon slot when mana cannot cover the cost** (:233957).

Slot initialisation (:245659) hands the script's `SetMaxReloadTime` the reload
time converted from frames to milliseconds (× 1000 ÷ 30).

**Aim gates** (:249328-249384). With mana available and the reload timer at 0,
aim angles are computed (:249211) and the shot is considered settled when
**both** the heading error is within `AimTolerance` **and** the pitch error is
within `AimTolerance ÷ 2`. If the script's `AimWeapon` then returns true, one
further gate applies: the body-facing error between the unit's heading and the
target heading must be within `max(AimTolerance, 0x200)`. Only then does it
fire. A 3-bit retry counter lives in the slot's flags; when aim fails it
reissues `AimWeapon` and reloads the counter to 7 (:249308).

**Lead.** `dontleadtargets` selects a frozen target position instead of the
live one (:249211-249250). There is **no** `targetmoveerror`.

**Reload cadence** (:249388): the reload timer is `reloadtime` plus a jitter
drawn uniformly on ±accuracy from the 15-bit engine RNG (range 0x8000).
**The accuracy term itself is a dropped FPU expression — OPEN**, needs the
assembly around 0x00530xxx.

**Launch** (:246602-246637): heading = the unit's heading plus the slot's aim
heading (aim-relative — confirmed), pitch from the slot, speed from the weapon.

`burst`, `burstrate` and `sprayangle` are consumed inside the per-weapontype
spawn methods (:246400-249100), which are heavily mangled — **OPEN**, needs a
follow-up dig.

## 5. Nanoframe decay / refund (COMPLETE) — our `units.c:1096` forfeits mana: DEVIATION

The build fraction on a unit is the amount **remaining** (COB port 0x11 returns
1 − it, :223278).

`Building_TickConstruction(builder, target, speedMultiplier)` :39451-39527 is
the single build/decay routine. The player's mana resource carries:

| Field | Meaning |
|---|---|
| Current | Mana in hand |
| Maximum | Storage cap |
| Starvation throttle | 0..1 fraction, scales all build work this tick |
| Income accumulator | — |
| Demand accumulator | — |
| Income-disable | When set, income is suppressed |
| Lifetime total | Double-precision running total |

**Building.** Rate = starvation throttle × speed multiplier × the definition's
inverse build time. Spend = rate × `buildcost`. The demand accumulator is
updated as it goes. When current mana is short, the work proceeds at a
proportionally reduced rate rather than stopping. On completion HP is set to
`maxdamage` and the unit's position is finalised. **At every call site the
speed multiplier is the builder's `workertime` × 1/30.**

**Decay** (a negative speed multiplier). Progress-remaining increases instead
of decreasing, and **the mana is refunded in full proportion** — into current
mana, the income accumulator and the lifetime total. At a remaining fraction of
1.0 or more the unit is destroyed (mission code 0xb).

Entry point `Nanoframe_Decay(unit, dt)` :39531 calls the tick with the unit as
its own builder and a speed multiplier of `dt × −0.5`. Its only caller is
:9643, inside the abandoned-nanoframe state machine (:9629-9657): state 0 gives
a **300-frame (10 s) grace period**, after which decay runs at 1 per frame —
i.e. **half the nominal build rate**.

**Repair.** `Unit_ApplyHeal(builder, target, rate, chargeMana)` :39540-39576
costs starvation throttle × rate × the **target's** inverse build time ×
the **target's** `buildcost`. Sub-1 HP amounts accumulate in the target's 8-bit
fractional HP accumulator. The heal fires event 0xc.

**Economy tick** (:235930-235985):

- Income = Σ `mogriumincome` × 1/30, suppressed while the income-disable field
  is set.
- Maximum = Σ `mogriumstorage`, floored at 1.0.
- **Starvation throttle = min(1, currentMana ÷ (demand × 1/30))**, where demand
  is summed over every unit currently flagged actively-building as
  `workertime` × inverse build time × `buildcost` **of its target**.

## 6. Transports

FBI keys (:162870-162879): `transportsize`, `transportcapacity`,
`transportsizecapacity`, `transportedsize` (**defaults to footprintX ×
footprintZ**, :163196), `cantbetransported` (definition flag),
`cantransport` (definition flag), `transportdistance`.

`Transport_CanLoadUnit` :233612-233649 requires **all** of:

1. Both units alive, built and unparalyzed.
2. Neither is in the "being loaded/unloaded" state.
3. **Same owner** — there is no allied loading.
4. The passenger cannot fly and is not `cantbetransported`.
5. The transport is `cantransport`.
6. The passenger has a mission.
7. **Passenger `transportedsize` ≤ transport `transportsize`** (a per-unit
   size gate).
8. The current carried count is below `transportcapacity` (counted by walking
   the carried chain, :233578).
9. **Σ loaded `transportedsize` + the new passenger ≤ `transportsizecapacity`**
   (:233595).
10. A submersion check (:233637).

Linkage is intrusive: the transport holds the head of the carried list, each
carried unit holds the next, and each carried unit holds a back-pointer to its
transport.

Pickup runs at :14365-14483 (mutual order match, a squared-distance gate on
`transportdistance`, and it gathers other eligible units in range); unload at
:14487 onwards with a drop-point scatter (:24329-24359). COB entries used:
`BeginTransport` / `EndTransport` (:24300), `BECARRIED` (:179284),
`VTOL_StepOut*` (:24242 onwards), `BeginLanding`. **TAK has no
`TransportPickup` / `TransportDrop` pieces** — that is a TA-ism. The air
variant is at :27164.

## 7. Patrol / Guard

Capability flags: `canguard`, `canpatrol` (:163033). Standing orders parse into
six definition bits (:162926-162946).

**Patrol.** The waypoint driver is at :11565 onwards. On an unreachable
waypoint an unreachable flag is set; a 1-in-3 random draw aborts the order
outright, and the flag clears again with probability 1/15 per attempt.

**Wander step** (:13799-13884): a random heading, at a radius of
footprintX × 16 plus a per-order radius field, taken around the order's anchor.
Give-up compares `3 × Random(4)` against a retry counter in the order record
that increments on every attempt.

**Guard** (:9604-9700) **mirrors the guarded unit's order chain**: move orders
are re-issued as order type 2, patrol as type 9, and repeat is set to −1 when
the source order repeats. It falls back to a plain follow when there is nothing
to mirror. Area guard (:12837-12927) is an axis-aligned box held in the order
record, scanned on a 16-unit snap, as order type 0xc.

## 8. Wall-drag (COMPLETE)

**Ghost snap** (:39263-39305). The placement grid quantum is **16 world units**
(a half-tile), and the snap is footprint-centred with round-to-nearest: take
the mouse world position, subtract footprint × 8 world units (half the
footprint, since footprints are counted in 16-unit cells), add 8 world units
for rounding, and divide by 16. A global placement-valid flag says whether the
snapped cell can be built on.

**Drag** (:243846-243868). With Shift held, the UI in build mode, the left
mouse button down and the placement valid, **one build order is queued per
newly-snapped cell during the drag**. Duplicates are impossible because placing
the nanoframe clears the valid flag for that cell.

**Order issue** (:39143-39258): the order goes to every selected builder, as
`MOBILEBUILD` / `VTOL_MOBILEBUILD` / `HelpBuild`. Repeat is either 1, or
10000000 together with the continuous flag. **Ctrl-continuous requires Ctrl
held AND `bmcode` == 1** (:150077-150086; `bmcode` parsed at :162924) — that is
the wall/linear build class. There is exactly one ghost; no multi-ghost
preview.

## 9. `maxwaterslope` — CONFIRMED + missing clamps

**Depth test** (:219149-219157): fail if the minimum cell height is below
`waterLevel − maxwaterdepth`; fail if the maximum cell height is above
`waterLevel − minwaterdepth`.

**Slope test** (:219158-219165): the *minimum* cell height versus the water
level selects which limit applies — underwater uses `maxwaterslope`, otherwise
`maxslope` — and the limit is compared against (maximum height − minimum
height).

**MoveInfo** (:187426-187467) carries `maxslope`, `badslope` (defaulting to
`maxslope >> 1`), `maxwaterslope`, `badwaterslope` (defaulting to
`maxwaterslope >> 1`), and bad maximum/minimum water depths. **Three clamps are
applied after parsing, and we are missing them:**

1. If `maxwaterslope` < `maxslope`, then `maxslope` = `maxwaterslope`.
2. If `maxslope` < `badslope`, then `badslope` = `maxslope`.
3. If `maxwaterslope` < `badwaterslope`, then `badwaterslope` = `maxwaterslope`.

## 10. Healing

**Self-heal** (:236280-236287): requires a non-zero `healtime`, and the unit
alive, built, unparalyzed and below max HP. It runs **every 8 frames**, healing
`healtime × 8 ÷ 30` with mana charging **off** — a net rate of `healtime` HP
per second, free.

**Worker repair**: `workertime × 1/30` per frame with mana charging **on**, at
:32674 and :13512 — so it is mana-charged and subject to the starvation
throttle.

**There are no heal weapons** — the special weapon types are mindcontrol,
turntostone, lightning, fire and paralyzer.

The health percentage cache refreshes every 30 frames (:236256). COB port 4
returns the live value, `hp × 100 ÷ maxdamage` (:223212).

## 11. `mogriumbounty`

Parsed at :162910 as a float — **not** scaled by 1/30. On a kill
(:227316-227319) the killer's player mana increases by the bounty and the
lifetime total does too. **It bypasses the maximum-mana clamp**, unlike the
normal resource-add path (:8661). It is subject to the same no-self / no-team
guards as the XP award.

## 12. Game speed / pause

`SetLevel` :131724-131804 clamps the speed level to 0..20 (minimum 1 in
multiplayer), writes both a requested and an effective level, toasts
"Game Speed Normal" or "Game Speed ±N", and sends network packet 0x18. Keys at
:131808, :131819.

Ticks per frame = round(frame time in 30ths × level × 0.1 × the network
slowdown factor + a carry accumulator). While paused it is 0. The result is
capped at 5 per frame, with hysteresis: above 10 the engine auto-slows, and
below −100 it recovers (:131830).

**The single-player menu auto-pauses** (:154886 sets, :154938 clears) — but
only when the game mode is not 3. The game-over / shutdown flag word is **not**
the pause flag (:227347).

## Open items

- Veterancy: the global rank cap and the global per-level scale must be read
  out of the PE `.data` section (console setters :38338, :38324).
- Reload jitter: the accuracy term is a dropped FPU expression; needs the
  assembly near 0x00530xxx.
- `sprayangle` / `burstrate` / `burst` consumption inside the per-weapontype
  spawn methods (:246400-249100).
- Two per-unit fields scaled by the veteran multiplier at :235859 have no
  established meaning. They are at unit offsets +0xe0 and +0xe4 — the offsets
  are kept only because they are the only handle on them.

## Ranked gaps (impact order)

1. **Cloaking** absent + the `cancloak` bug (`units.c:1498`). Very high. §2.
2. **Nanoframe decay + refund** (`units.c:1096` forfeits the mana — a
   deviation) + the starvation throttle + the demand accumulator. Very high.
3. **Veterancy end to end** (level calc, port 32, mana discount, model swap).
   Blocked on two `.data` constants. High.
4. **Radar / detection model** (4-pass visibility recompute; no blip state;
   radar does not defeat cloak). Remove the sonar/stealth plans. High.
5. **LOS cell mapping** — fog cell = (X ÷ 32, (Z − Y/2) ÷ 32). A one-line fix,
   and silently wrong everywhere there is a slope. High.
6. **AimTolerance gating** (heading within tol, pitch within tol/2, body gate
   at max(tol, 0x200), 7-tick retry). High.
7. `maxwaterslope` + the MoveInfo clamps. S effort.
8. Transport size rules (`transportedsize` / `transportsizecapacity` /
   same-owner / canfly / submersion). M.
9. Wall-drag (16-unit snap, one order per cell, Ctrl requires `bmcode` == 1).
   S-M.
10. `mogriumbounty` (unclamped add). S.
11. Weapon cadence (burst / burstrate / sprayangle in the spawn methods —
    OPEN; reload jitter — OPEN; auto-downshift; substepping). M.
12. Guard order-mirroring. M.
13. Patrol wander / give-up. M.
14. Game speed / pause. S-M.
15. Healing cadence precision (8-frame chunks). S.
16. Cleanup: delete the `cancloak` / sonar / stealth / `tolerance` /
    `pitchtolerance` / `targetmoveerror` / `energypershot` parse paths; fix the
    `burst_rate_ticks` 60 Hz comment. XS.
