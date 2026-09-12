# Putting the battle in the save

Line numbers of the form `legacy:NNNN` are anchors into the legacy
reference, as in every note here.

docs/notes/2026-09-11-save-container.md covers the container, which
carries bytes and knows nothing about a battle.
docs/notes/2026-09-11-save-sections.md covers the light state: which
battle was set up, which map it was on, the world scalars, the
generator and the camera. This note covers the rest, which is the
battle itself.

## What goes in, and how that was decided

include/tak_sim_hash.h already says what the simulation owns, because
it hashes exactly that and nothing else. So the rule is simple. If the
hash covers a field, the file carries it. If the hash leaves it out,
the file leaves it out, and the reason is one of two.

Derived state a load rebuilds: the unit spatial grid, the AI influence
maps, the path plan cache, a COB engine's piece to node table and its
five host callbacks. Local view state two lockstep peers are entitled
to disagree about: the selection, the control groups, the draw order,
the HUD, the camera.

One thing the hash leaves out turned out to need saving anyway, and it
has its own section below.

Drawing is the same line drawn again over projectiles. A shot carries
its damage, its area of effect and its per category damage multipliers,
because those decide what happens when it lands. It does not carry its
art slot, its explosion class slot or its beam colours, because those
index caches filled in first fire order and the numbers do not mean the
same thing in another session. The consequence is worth stating
plainly: a shot that was in the air when the save was written comes
back doing the right damage and drawn as nothing. The sound names come
back, because they are three string table indices and cost nothing.

## The sections

| Id | Required | Holds |
|---|---|---|
| `UNIT` | yes | One record per unit slot, dead slots included |
| `UPTH` | yes | The live prefix of each unit's route |
| `UCOB` | yes | Pieces, statics and all sixteen script threads per unit, each with its whole stack |
| `PROJ` | yes | One record per projectile pool slot |
| `FEAT` | yes | One record per feature, corpses among them |
| `FOGV` | yes | Every player's fog layer |
| `ECON` | yes | 260 bytes: the per player mana pools and their windows |
| `AIST` | yes | The AI's generator, its per player records and its order matrices |
| `OCCU` | yes | The unit occupancy layer, four bytes a cell |

A `UNIT` record is 480 bytes, a `PROJ` record 216, a `FEAT` record 32.

Every one is required. All but `OCCU` carry state the hash covers, so a reader
that quietly stepped over one would bring up a battle that is not the
one that was saved. The fixed width sections spell their layout out as
offset constants with a `_Static_assert` that the last offset plus its
size equals the declared total, the same way `CFGB` and `WRLD` do.

The state layout version goes from 1 to 2. A save written before this
is refused by name rather than read as half a battle.

## Handles

A unit refers to another unit by slot index in nine places:
`Unit.target`, `Unit.build_target`, `Unit.carried_by`, each entry of
`Unit.load_queue`, `Unit.xfer_cargo`, `Projectile.target`,
`Projectile.shooter`, and the AI's target and threat handles.

Slot indices do not survive a reload on their own. What makes them
survive here is that the unit array is append only and never
compacted: a dead unit stays in its slot as a tombstone and nothing is
ever moved down. So slot i is written to record i and read back into
slot i, tombstones and all, and every one of those nine references is
still pointing at the same thing with no remap table and no fixup
pass. A projectile whose firer died mid flight still names a slot, and
that slot is still the same tombstone.

A tombstone carries two fields, its lifecycle byte and its stable id,
and the rest of its record is zero. Everything else in a dead slot is
whatever it held at the moment of death, and a load that believed it
would be wrong from the first casualty onwards.

Stable ids are the second, independent check. They go in the file, and
the counter that hands out the next one travels in the container
header so ids stay unique after a load rather than restarting from one.
The AI keeps both a handle and a stable id for its target and its
threat, which is what lets it notice a handle that no longer means
what it did.

Definitions never travel as indices. A loose file install or a mod
gives the registry a different order, so a unit and every entry of a
factory's production queue name their definition by its record in the
file, and the name is resolved against this installation's registry on
the way back in. `DEFS` has already refused any definition whose
content changed, by name.

## Scripts mid execution

Each live unit owns a COB engine with sixteen thread slots. All sixteen
go in the file, dead ones included, because src/render/units.c reads a
weapon's aim result off a slot `Cob_IsThreadAlive` has already called
dead, and a zeroed dead slot makes a unit whose script said hold fire
shoot instead.

The whole of each thread's stack goes in, not the words below its
stack pointer. That looks like a dead tail and is not one. A COB local
is a stack slot that POP-VAR writes by index with no relation to the
stack pointer, and `Cob_GetThreadArg` reads a finished thread's slots
with no bound at all, which is how `Killed` hands back the corpse it
asked for (legacy:227142) and how `QueryBuildInfo` hands back its
build spot. The hash had the same hole and it is closed there too.

A thread's program counter is a word index into the script the unit's
definition owns, so it only means anything against that exact script.
The definition fingerprint now covers the script for that reason: its
code words, its entry points and their offsets, the piece names that
bind it to the mesh, and the number of static variables. That is not
only for the save. A script is behaviour rather than art, because it
decides when a unit fires, what it hides and what its build stance is,
so a data set whose script moved would play the battle out differently
with no save in the picture.

The piece array is a different matter. It is sized by the unit's
*mesh*, and the definition fingerprint leaves art out on purpose so a
re-skin does not refuse a save. A re-skin with a different node count
would silently give the save a piece array of the wrong length, so the
stored piece count is checked against the live one and a mismatch is
refused by the name of the unit.

The restore order is the trap. `Units_Spawn` allocates the engine,
wires the five host callbacks, then runs `Create` and every thread
immediately, and `Create` has side effects through those callbacks: it
plays sounds and writes the unit's COB port bytes. So a load does not
use `Units_Spawn`. `Units_LoadAttachScript` allocates and binds the
engine and stops there, and the file's threads land on a clean slate.
The loading screen's final phase is told the battle is being restored
(`World_IsRestoring`) and spawns nothing at all, monarchs, campaign
placements and the mana seeding included, because the save carries the
pools that seeding would create.

## Fog

Fog is history. It cannot be recomputed from where the units are now,
because that loses every cell a player explored and can no longer see.
Every seat's layer goes in the file, not just the local player's: the
AI, the minimap and the line of sight gating all read layers the human
never sees. Player one's layer is a compatibility alias and is
re-pointed on load rather than stored twice.

The layers are mostly uniform, so per section deflate flattens them.

There is a second half to fog that is easy to miss. The reveal scan
was 95% of the browser CPU trace, so a unit's revealed cells are
worked out once and re-stamped until it has moved 16 px from wherever
they were worked out. That means the ground a unit lights up follows
that anchor rather than its exact position, and the anchor is a record
of where the unit happened to be when the scan last ran. It is state,
not a cache. It now lives on the unit as fog_x, fog_y, fog_sight and
fog_lit, it is hashed, and it travels in the save. The cell list
stays a cache in src/game/fog.c, because it is a pure function of the
anchor and can be thrown away at any time.

Nothing about live play changes: the anchor moves on exactly the
occasions the cache used to invalidate itself. What changes is that a
load lights up the same ground the saved battle did, and that two
peers comparing hashes would notice if they did not.

## The occupancy layer, which is not derived

The unit occupancy layer reads as derived state, and the hash leaves
it out on that basis. Every cell of it can be restamped from the units
standing on the map, so a load looks like it should be able to build
it back. It cannot. Two footprints can cover the same cell, and the
one that holds it is whichever claimed it first, so the layer is a
record of what happened rather than a function of where everyone is
now. A restamp comes close and is not the same, and a mover that finds
one cell held differently takes a different step within a handful of
ticks.

The hash is still right to leave it out. Two lockstep peers run the
same history and build the same layer, so hashing it would only cost
time. A save has no history, which is the difference.

Four bytes a cell, mostly zero, and per section deflate takes care of
the rest. `occ_version` is not in the file: a load bumps it on purpose
so the clearance cache built against the previous session cannot be
believed.

## The economy

`max_mana` and `regen_per_sec` look derivable from the monarch and the
lodestones a player holds. They are not. They are adjusted in place by
`Economy_AdjustCaps` on every capture and every loss and by
`Economy_OnMonarchSpawn`, and there is no function that recomputes them
from the world. They go in the file.

## The stall ladder, and a gap the hash had

`stall_esc` counts the rungs of the unsticking ladder a wedged unit has
already climbed. It is not a function of anything else the unit
carries, so a load that dropped it would put that unit back at the
bottom. It, `route_serial` and the thirteen stall fields beside it were
not in the simulation hash: the hash landed first and the ladder landed
after it, and nothing folded them in. They are in it now, which is what
lets the round trip test below be believed at all, and it closes the
same gap for lockstep.

## The test that proves it

A test that checks the file parsed is not evidence. src/game/
test_savestate.c runs a real skirmish on a real map with the real
engine, saves part way through, keeps running and records the
simulation hash on every tick, then brings the battle back out of the
file into a fresh world and runs the same ticks again. The two streams
have to agree tick for tick. A field the serialiser dropped shows up as
the tick the two answers part company, which is the same tick a
lockstep peer would report a desync on. The cases beside it are the
half formed states a save is most likely to catch and get wrong: the
first tick of a battle before anything has moved, the tick after a
unit dies while its Killed script is still running and its corpse is
one tick old, a builder feeding a nanoframe, a transport with someone
aboard, shots in the air, and a caster part way through raising a
corpse. One more covers the flag rather than a battle: a load
abandoned between World_BeginLoad and Save_Apply must not leave the
next battle spawning nothing.

src/game/test_savegame.c is the headless half. It opens no window and
needs no game data, so CI runs it, and it round trips the same hash
over a battle it owns itself.

## When a save is refused outright

`Save_Write` refuses a world that has not finished loading. A battle
part way through the loading screen has a map name and little else:
no fog, no occupancy layer, no features, no units. Writing one would
produce a file nothing could load, so the refusal happens while the
player can still be told why.

## Two things that looked derived and were not

Both were found by the tick for tick test rather than by reading, and
both have the same shape. A structure that can be rebuilt from the
present is not the same as a structure that is a function of the
present. The occupancy layer can be restamped from where every unit
stands, but a contested cell belongs to whichever unit claimed it
first. The fog reveal can be rescanned from where every unit stands,
but the scan only re-runs after 16 px of movement, so the ground lit
up belongs to wherever the unit was when it last ran.

Both are invisible to lockstep, because two peers run the same history
and arrive at the same answer. A save has no history. That is the
difference, and it is worth holding on to when the next piece of
derived state comes up for judgement.

## What the UI needs from this

`Save_Write`, `Save_Read`, `Save_Info`, `Save_Apply` and
`Save_ReadClose` are unchanged in shape. What changed is when
`Save_Apply` may be called: it now needs a world that has been through
the loading screen, because it fills in units, fog and features that
only exist once the map is up. The sequence for a load is

1. `Save_Read(path)` and `Save_Info` for the battle configuration, the
   map name and the kingdom.
2. `World_BeginLoad` with those, then `World_SetRestoring(1)`, then run
   the loading screen to the end.
3. `Save_Apply`.
4. Open the battle screen.

The flag is raised after `World_BeginLoad` because three things put it
down and one of them is `World_BeginLoad` itself. The loading screen's
final phase clears it the moment it has acted on it, `World_End`
clears it, and starting any battle clears it. That last one is the
guarantee that matters: a load abandoned between `World_BeginLoad` and
`Save_Apply`, a loading phase that fails, a player who backs out, none
of those has to reach `World_End`, and the next battle would otherwise
spawn nothing and look broken.

A failure from `Save_Apply` leaves the caller holding the teardown.
`Save_Apply` never touches a platform, so it cannot release the map's
GPU textures and cannot call `World_End` itself. Every definition the
save names is checked before anything is written, so a data set that
moved is refused with the world exactly as the loading screen made it.
A refusal past that point leaves a world holding part of a battle, and
the only correct answer is `World_End` and a return to the menu.
Showing the refusal and leaving the half restored world standing is
the one outcome a player must never be given. That is written next to
`Save_Apply` in include/tak_savegame.h, where the caller will read
it.
