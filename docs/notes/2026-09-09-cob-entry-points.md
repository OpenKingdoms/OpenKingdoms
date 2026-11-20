# Unit scripts

How the original runs a unit's COB script: which entry points it
invokes, when, with what arguments and in which shape, how a script's
piece turns and moves land on the model, and how a flyer's landing
script is brought to its end.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate departures
are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md), cited by ID.

## The entry points

A unit's animation is the script's business. The original tells the
script what happened, once, at the moment it happens, and decides
nothing about the pose itself. Building is a state bit on the unit.
Raising the bit invokes `StartBuilding` once (legacy:9435) and clearing
it invokes `StopBuilding` once (legacy:236408). Between the two edges no
script is invoked at all, and the construction tick only moves hit
points. ARAKING's `StartBuilding` sets its in-build-stance flag, calls
its `startbuild` pose routine once and returns, so Elsin raises his
sword once and holds it for as long as he builds.

The original invokes an entry point in one of three shapes. An async
call queues a thread and returns, and the thread runs on later frames.
An async and run call does the same, then steps every live thread once
before it returns, so a short script completes inside the call. A sync
call allocates a thread, runs it in place and copies four stack slots
back out, which is how the original reads a value from a script. A
thread runs until it sleeps, waits or ends, however many instructions
that takes (legacy:306252-307002).

The argument counts below are the counts the original passes. Where one
looks generous for the work, it is because the invoke helper always
writes four slots and trims the stack to the declared count.

| Entry point | When the original invokes it | Arguments | Shape | How often |
|---|---|---|---|---|
| `Create` | Right after the script is attached to a new unit (legacy:226779) | 0 | async and run | once a unit |
| `Activate` | Rising edge of the unit's activation state bit (legacy:236402) | 0 | async and run | once an edge |
| `Deactivate` | Falling edge of the same bit (legacy:236405) | 0 | async and run | once an edge |
| `StartBuilding` | When a build order enters its construction phase, right after the building bit is set (legacy:9435), and on the order packet path (legacy:179312) | 2 | async | once a construction |
| `StopBuilding` | Falling edge of the building bit, by completion, cancel or interruption (legacy:236408) | 0 | async and run | once a construction |
| `QueryBuildInfo` | Once for each product, when the build order picks the build pad (legacy:9347) | 4 in and out | sync | once a product |
| `MoveRate` | From the movement step, only when the speed tier changes (legacy:184448) | 1, the tier | async and run | once a change |
| `TurnDirection` | From the movement step, only when the sign of the turn changes (legacy:183171) | 1, the sign | async and run | once a change |
| `setSFXoccupy` | When the surface class under the unit changes (legacy:185108) | 1 | async and run | once a change |
| `AimWeapon` | For each aim request, latched so a request cannot invoke it again (legacy:249312), mirrored on the remote path (legacy:195295) | 3, the heading, the pitch and the weapon index | async | once an aim request |
| `FireWeapon` | At the shot (legacy:249415), mirrored on the remote path (legacy:195314) | 1, the weapon index | async | once a shot |
| `RockUnit` | At the shot, only when the weapon has recoil (legacy:249499) | 2, the x and z impulse | async | once a shot |
| `TargetCleared` | When a weapon's target is dropped (legacy:233936) | 1, the weapon index | async | once a drop |
| `SwitchWeapon` | When the active weapon changes (legacy:233973) | 1, the weapon index | async | once a change |
| `SetMaxReloadTime` | At weapon setup, with the reload time in milliseconds (legacy:245687) | 1 | async | once |
| `HitByWeapon` | On taking a hit (legacy:233825) | 4, the damage class, x offset, z offset and amount | async | once a hit |
| `Killed` | On death (legacy:227142), with a replicated form on the remote path (legacy:227241) | Sync form: severity in, corpse type out. Async form: 3 | sync, or async and run | once a death |
| `Dying` | Right after `Killed`, only when the severity is above zero and the script defines it (legacy:227256) | 1 | async and run | once a death |
| `QueryWeapon` | Whenever the original needs a weapon's muzzle piece (legacy:185948) | 2 in and out, the piece out and the weapon index | sync | on demand |
| `AimFrom` | Whenever it needs a weapon's aim-from piece (legacy:185989) | 2 in and out | sync | on demand |
| `SweetSpot` | Whenever it needs the unit's aim-at piece (legacy:186011) | 1 in and out | sync | on demand |
| `QueryNanoPiece` | Whenever it needs the build beam's piece (legacy:186028) | 1 in and out | sync | on demand |
| `QueryBlood` | Whenever it needs the blood emit piece (legacy:126194, legacy:186059) | 1 in and out | sync | on demand |
| `QueryLandingPad` | Transport landing (legacy:27914) | 4 in and out | sync | on demand |
| `WindChange` | When the wind vector changes (legacy:178913) | 2 | async | once a change |
| `StartCloaking`, `StopCloaking` | On the cloak toggle (legacy:9763, legacy:9776) | 0 | async | once an edge |
| `BeginFlight` | Take-off (legacy:24126) | 0 | async and run | once an edge |
| `BeginLanding` | At the top of the landing descent (legacy:24302) | 0 | async and run | once an edge |
| `EndTransport` | A transport's unload is finished (legacy:24300) | 0 | async and run | once an edge |

The original waits on a script only in the sync rows. Everywhere else it
queues the thread and moves on, which is why a script is free to sleep,
loop or start further threads without stalling the simulation.

### Building

`StartBuilding` may loop inside some scripts. A nailing or hammering
animation is written as a loop in the script and runs until
`StopBuilding` signals it to stop. A script that poses once and one that
loops both work only because the entry point is invoked exactly once.
No `StartBuilding` or `StopBuilding` in the shipped data reads a
parameter. The original still passes two to `StartBuilding`, and
nothing observable depends on their values.

### Moving

Total Annihilation's `StartMoving` and `StopMoving` pair does not exist
in Kingdoms. None of the 187 shipped scripts defines either name, and
the original never looks for them. Kingdoms has `MoveRate` in their
place, which takes a speed tier. The sixteen scripts that define it
read the argument and branch on whether the rate is above zero to swap
between the moving pose and the idle pose, starting their own walk
cycle from inside the script. A further 116 define `TurnDirection` and
keep its sign to lean the walk cycle. The rest leave both to their own
watcher threads.

`Create` starts long-lived watcher threads (`MoveWatcher`,
`MeleeControl`, `StatusControl`, `SmokeControl` and others) that poll
unit values in their own loops and drive `walk` and `restore`
themselves. The original only signals transitions, and anything that
repeats does so because a script asked for it. A walk loop ends itself
on the signal `MoveRate(0)` raises. Every shipped script that has a
`walk` calls or starts it itself. The engine departs only for a script
that never does, whose `walk` it keeps running itself, and no shipped
script is such a one.

### Aiming and firing

`AimWeapon` gets the heading, the pitch and the weapon index, and
scripts branch on the index. An archer tower picks which of its two
bowmen to turn from that argument alone. `FireWeapon` gets the weapon
index only.

The original keeps the aim heading on the weapon's fire request and
hands it straight to `AimWeapon` (legacy:249312). The heading is the
unit's heading minus the angle to the target, a counter-clockwise
difference, plus half a turn (0x8000). Scripts such as ARASSH do signed
arithmetic on it, so it is a signed angle. Twenty-three of the
twenty-four shipped scripts that turn a piece from `AimWeapon`, ARAAT
and VERHARP among them, subtract half a turn from the argument before
they turn, and only one does not. The half turn is the convention, not
a correction for one model's rest pose.

The geometry shows why. A 3DO piece has a translation offset but no
rest rotation, so at zero rotation its frame is its parent's, and its
world heading is the unit's heading minus its own turn, minus because
of the left-handed frame described under Piece axes. A piece turned to
`arg - 0x8000` faces `unit heading - arg + 0x8000`, which is the
target's direction when `arg` is `unit heading - target heading +
0x8000`.

The 103 scripts whose `AimWeapon` turns nothing take their aim from the
attack animation. ARABOW's `AimWeapon` only signals, waits and sets its
reload time, and the aim shows in the arm turns of its `attack1`.

The XZ_ATAN value a script reads from port 12 has the same sense, the
unit's heading minus the angle, in 16 bits.

The engine passes zero for the pitch.

### Death

`Killed` gets the severity of the death, from 1 to 100, and death
scripts branch on it to choose how violently the unit comes apart. How
the severity is worked out, which causes force a corpse type and when
`Dying` runs are in
[2026-09-04-rendering-visuals.md](2026-09-04-rendering-visuals.md),
under Death and corpses.

## Piece axes

The 3DO and COB model frame is left-handed: +x is the model's left, +y
is up and +z is back. The shipped models show it. In ARASWORD and
ARABOW LegUL sits at +x and LegUR at -x, and capes hang toward +z.

At load the original negates x and z of every vertex and every piece
offset in the 3DO tree (legacy:270790-270815). That is half a turn
about y, not a mirror. A script's MOVE is stored raw on the piece
(legacy:223426-223438) and added raw to the flipped offset when the
piece matrix is built (legacy:198927-198930, legacy:364077-364100). The
piece matrix is the parent's matrix, times a translation by the offset
plus the script position, times the rotation, and the rotation applies
z first, then x, then y, with the y angle negated
(legacy:364101-364160). The unit's own angles enter the same way at the
root (legacy:198681-198683). Only the projection negates z
(legacy:197681).

Read in the authored 3DO frame, that comes to these rules.

- A TURN or SPIN plays with its sense negated on every axis against a
  right-handed reading. Rotations are pseudovectors, and under a map of
  determinant -1 a turn by an angle about an axis becomes a turn by
  minus that angle about the mapped axis.
- A script translation (x, y, z) lands at (-x, y, -z). Only a move on y
  keeps its sign.
- A piece turned on more than one axis composes as Ry(-y) Rx(-x)
  Rz(-z). A turn on one axis comes out the same in any order.
- A positive turn about x swings a piece that hangs below its parent
  toward +z, backward.
- A script's linear values share the 3DO's fixed point space, 65536 to
  a pixel, with no scale between them. `move ... to [-0.6]` compiles to
  -98304, and a Hip offset reads y 1894644 in the same space.

The shipped scripts confirm the sense of a turn. In ARASWORD's `walk`
and `attack1`, each of these turns shows in the original as the
right-handed reading negated.

| Piece | Turn in the script | What the original shows |
|---|---:|---|
| Knee, about x | +41.13 degrees | The shin flexes backward |
| Elbow | -88.95 degrees | The forearm comes forward |
| ArmUR, about z | +23.55 degrees | The arm swings out from the body |
| Torso, about y | -12.3 degrees | The torso winds to the right |

The Aramon gate shows the translations. ARANGATE's `open` moves Door1
to x +50 and Door2 to x -50, and in the 3DO Door1's vertices lie on the
negative x side of the gateway, so taken literally the two halves would
cross in the middle. With x flipped each door origin moves 50 px to its
own side and the pair end 100 px apart, sliding outward into the walls
as in the original, and they return to the centre on close.

## Landing

An Iron Beak whose status reads "Seeking to land" has its tail down and
its wings up, fluttering a little, and drifts down. That look holds for
the whole descent, and the landed pose comes at touchdown and not
before.

The Iron Beak's script keeps two things apart. `setSFXoccupy` tells it
whether it is in the air, and 5 means in the air. `BeginLanding` clears
the script's own flight flag and starts `land`, which holds the descent
pose, wings up and tail down, for as long as the script is told 5, and
takes the landed pose once it is told anything else. So `land` is the
descent, and the occupy value is what ends it.

`BeginLanding` runs at the top of the descent (legacy:24302), and a
transport hears `EndTransport` just before it (legacy:24298-24302). The
occupy state is sent at legacy:185079-185112, which does not show when
a landing flyer stops being 5. That it stays 5 until touchdown is taken
from the original's look.

`land` draws no random numbers. While the script is told 5 its flight
watcher keeps cycling and drawing them, and its flight routines return
at once because `BeginLanding` cleared the flight flag. A flyer killed
during the descent is still in the air for its script, so a script that
tells an airborne death from a grounded one takes the airborne branch
of `Killed`. Of the 27 shipped flyers that have scripts, nine make that
distinction.
