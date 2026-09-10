# COB script entry points: who calls what, and how often (2026-09-09)

Symptom that started this: Elsin (ARAKING) raises his sword to build, then
lowers and raises it again, forever, for as long as he is building. Legacy
raises it once and holds it.

The animation is entirely the script's business. ARAKING's `StartBuilding`
sets the in-build-stance flag, calls the `startbuild` pose routine once, and
returns. Nothing in it loops. The loop was ours. The engine treated
`StartBuilding` like a background animation thread and restarted it every
tick that it was not running, so a script that finishes in one cycle got
restarted 60 times a second.

The legacy engine never does that. It models "is building" as a per-unit
state bit and fires the two scripts on the **edges** of that bit: raising it
invokes `StartBuilding` once (:9435), clearing it invokes `StopBuilding` once
(:236408). Between the edges no script is invoked at all. The construction
tick only moves hitpoints.

## The entry-point table

Three invocation shapes exist. **Async** queues a thread and returns, and the
thread runs on subsequent frames. **Async+run** does the same but then steps
every live thread once before returning, so a short script completes inside
the call. **Sync** allocates a thread, executes it in place, and copies four
stack slots back out. That last one is how the engine reads values *from* a
script.

Argument counts below are the counts legacy actually passes. Where a count
looks generous for the work, it is because the invoke helper always writes
four slots and trims the stack to the declared count.

| Entry point | When legacy invokes it | Args | Shape | Cadence |
|---|---|---|---|---|
| `Create` | Immediately after the script is attached to a new unit (:226779) | 0 | async+run | once per unit |
| `Activate` | Rising edge of the unit's activation state bit (:236402) | 0 | async+run | once per edge |
| `Deactivate` | Falling edge of the same bit (:236405) | 0 | async+run | once per edge |
| `StartBuilding` | The moment a build order enters its construction phase, right after the building state bit is set (:9435), and on the order-packet path (:179312) | 2 | async | once per construction |
| `StopBuilding` | Falling edge of the building state bit, whether by completion, cancel or interruption (:236408) | 0 | async+run | once per construction |
| `QueryBuildInfo` | Once per product, when the build order picks the build pad (:9347) | 4 in/out | **sync** | once per product |
| `MoveRate` | From the movement step, only when the speed tier changes (:184448) | 1 (tier) | async+run | once per change |
| `TurnDirection` | From the movement step, only when the sign of the turn changes (:183171) | 1 (sign) | async+run | once per change |
| `setSFXoccupy` | When the surface class under the unit changes (:185108) | 1 | async+run | once per change |
| `AimWeapon` | Per aim request, latched so a request cannot re-invoke it (:249312), mirrored on the remote path (:195295) | 3 (heading, pitch, weapon index) | async | once per aim request |
| `FireWeapon` | At the shot (:249415), mirrored on the remote path (:195314) | 1 (weapon index) | async | once per shot |
| `RockUnit` | At the shot, only when the weapon has recoil (:249499) | 2 (x, z impulse) | async | once per shot |
| `TargetCleared` | When a weapon's target is dropped (:233936) | 1 (weapon index) | async | once per drop |
| `SwitchWeapon` | When the active weapon changes (:233973) | 1 (weapon index) | async | once per change |
| `SetMaxReloadTime` | Weapon setup, reload time in milliseconds (:245687) | 1 | async | once |
| `HitByWeapon` | On taking a hit (:233825) | 4 (damage class, x offset, z offset, amount) | async | once per hit |
| `Killed` | On death (:227142), with a replicated form on the remote path (:227241) | sync form: severity in, corpse type out. async form: 3 | **sync** or async+run | once per death |
| `Dying` | Right after `Killed`, only when severity is above zero and the script defines it (:227256) | 1 | async+run | once per death |
| `QueryWeapon` | Whenever the engine needs a weapon's muzzle piece (:185948) | 2 in/out (piece out, weapon index) | **sync** | on demand |
| `AimFrom` | Whenever the engine needs a weapon's aim-from piece (:185989) | 2 in/out | **sync** | on demand |
| `SweetSpot` | Whenever the engine needs the unit's aim-at piece (:186011) | 1 in/out | **sync** | on demand |
| `QueryNanoPiece` | Whenever the engine needs the build-beam piece (:186028) | 1 in/out | **sync** | on demand |
| `QueryBlood` | Whenever the engine needs the blood emit piece (:126194, :186059) | 1 in/out | **sync** | on demand |
| `QueryLandingPad` | Transport landing (:27914) | 4 in/out | **sync** | on demand |
| `WindChange` | When the wind vector changes (:178913) | 2 | async | once per change |
| `StartCloaking` / `StopCloaking` | On the cloak toggle (:9763, :9776) | 0 | async | once per edge |
| `BeginFlight` | Take-off (:24126) | 0 | async+run | once per edge |
| `BeginLanding` | Landing approach (:24302) | 0 | async+run | once per edge |
| `EndTransport` | Transport unload finished (:24300) | 0 | async+run | once per edge |

The engine waits on a script only in the **sync** rows. Everywhere else it
queues the thread and moves on, which is why a script is free to sleep, loop,
or start further threads without stalling the simulation.

## There is no `StartMoving` or `StopMoving` in Kingdoms

Total Annihilation's `StartMoving` and `StopMoving` pair does not exist here.
Not one of the 187 scripts in the shipped data defines either name, and the
engine never looks for them. Kingdoms replaced them with `MoveRate`, which
takes a tier. The sixteen scripts that define it all read the argument and
branch on `rate > 0` to swap between the moving pose and the idle pose,
starting their own walk cycle from inside the script. A further 116 define
`TurnDirection` and stash its sign to lean the walk cycle. The rest leave
both to their own watcher threads.

That is the second half of the same design. `Create` starts long-lived
watcher threads (`MoveWatcher`, `MeleeControl`, `StatusControl`,
`SmokeControl` and friends) which poll unit values in their own loops and
drive `walk` and `restore` themselves. The engine's job is only to signal
transitions. Anything that repeats is repeating because a script asked it to.

## The half-turn built into the aim heading

A second report landed while this was being written: strongholds and archer
towers face away from what they are shooting. It is the same class of
problem, the engine deciding something the data should decide, and it lives
in `AimWeapon`'s first argument.

Legacy stores the aim heading on the weapon's fire request and hands that
value straight to `AimWeapon` (:249312), along with the pitch and the weapon
index. The scripts that own a turret take that first argument and turn a
piece to it directly. Twenty-three of the twenty-four scripts in the shipped
data that turn a piece from `AimWeapon` subtract half a turn from the
argument first, and only one does not. That is not a per-model quirk, it is
the convention, and it means the engine's aim angle sits half a turn away
from the direction the piece must end up facing.

The rest is geometry we can check rather than argue about. A 3DO piece has a
translation offset but no rest rotation, so a piece's frame at zero rotation
is exactly its parent's frame, and a piece's world heading is the unit's
heading minus its own turn value. It is minus because the model frame is
left-handed and the angle scale is negated, per
`docs/notes/2026-09-04-animation-axis-conjugation.md`. Turning a piece to
`arg - 0x8000` therefore lands it at `unit heading - arg + 0x8000`. We were
passing `unit heading - target heading`, which puts the piece at
`target heading + 0x8000`: exactly backwards, every time, for every turret.
Passing `unit heading - target heading + 0x8000` lands it on the target.

Measured with an Aramon archer tower and an Aramon stronghold, each shooting
a wall placed due east, and the tower shooting one placed due south: the
piece pointed 180.0 degrees away before the change and 0.0 degrees away after
it, in all three cases. The 103 scripts that turn nothing inside `AimWeapon`
are unaffected, because their aim visual comes from the attack animation.
That is why this went unnoticed on archers and swordsmen.

The pitch argument is still passed as zero. Nothing in the reports turns on
it yet, and we do not track the vertical separation it would be computed
from.

## Which entry points may loop

`StartBuilding` legitimately loops in some scripts. A nailing or hammering
animation is written as a loop inside the script and runs until
`StopBuilding` signals it to stop. That is the script's decision and costs
the engine nothing, because the engine is not restarting it. Both designs
work only if the engine invokes the entry point exactly once.

The corpus is unambiguous about the arguments to the build pair: no
`StartBuilding` or `StopBuilding` in the shipped data reads a parameter at
all. Legacy still passes two to `StartBuilding`, so we do too, but nothing
observable depends on their values.

## What we changed

`src/render/units.c`:

- `StartBuilding` is invoked once when the builder enters the building state
  and `StopBuilding` once when it leaves, matching the legacy state bit. The
  per-tick re-invocation is gone, and so is the duplicate invocation that
  fired on the entry tick.
- `MoveRate` and `TurnDirection` are now driven, both edge-triggered on a
  cached value the way legacy caches them. Scripts without them are
  unaffected.
- `AimWeapon` gets three arguments (heading, pitch, weapon index) instead of
  two. The third was previously read as whatever was left on the stack, and
  scripts branch on it. An archer tower picks which of its two bowmen to turn
  from that argument alone.
- `AimWeapon`'s heading argument carries the half turn the turret scripts
  subtract back out, so turrets face their targets instead of away.
- `FireWeapon` gets one argument, the weapon index, instead of two.
- `Killed` gets a real severity, derived from overkill against maximum
  hitpoints and clamped to 1..100 the way legacy derives it, instead of a
  constant zero. Death scripts branch on it to choose how violently to come
  apart.
- The engine stops the walk thread it started when a unit leaves the moving
  state. Legacy never has to, because its walk loops end themselves on the
  signal `MoveRate(0)` raises. Left running, ours walked in place.
- Issuing a move order no longer stacks another walk thread on a unit that is
  already walking. Sixteen thread slots go quickly when orders arrive in
  bursts, and a unit that runs out stops animating entirely.
- `Activate` is latched on the engine's own invocation count rather than on
  the ACTIVATION port value. The port belongs to the script, and a script
  that cleared it got `Activate` restarted every tick.

Unchanged on purpose: the `walk` thread is still kept alive by the engine
rather than by a script-side watcher, because our host does not yet serve
every unit value the watcher threads poll. That is a known deviation, not a
loop the engine is inventing.

## Guard tests

`src/ui/test_ui_screens.c`, `cob_entry_points_fire_once`: a builder is given
a real construction and ticked for several hundred sim ticks. It asserts
`StartBuilding` invoked exactly once, the raised sword-arm piece angles
unchanged after the first cycle, `StopBuilding` invoked exactly once when the
order ends, and one movement start plus one movement stop across a move
order. Every engine-driven script start is funnelled through one call, so the
counts are of what the engine actually asked for rather than of what the
state machine intended. Restoring the per-tick re-invocation fails it.

`tower_aim_faces_target`: an archer tower and a stronghold each shoot a wall
placed due east, and the tower shoots one placed due south. It asserts the
aim piece's world heading lands within thirty degrees of the target's
direction and is still there a second later. Reverting the half turn puts all
three at 180.0 degrees.
