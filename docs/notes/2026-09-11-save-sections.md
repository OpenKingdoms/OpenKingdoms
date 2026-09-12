# The first game state in a save

Line numbers of the form `legacy:NNNN` are anchors into the legacy
reference, as in every note here.

The container in docs/notes/2026-09-11-save-container.md carries bytes
and knows nothing about a battle. This note covers the other half, the
sections that say what the battle was. It is the light state only:
which battle was set up, which map it was played on, the world scalars,
the simulation generator and the camera. Units, scripts, projectiles,
fog and the AI land in their own sections on top of it.

Nothing in src/game/savegame.c touches a platform or a window. A save is
written and read with no window open and no SDL subsystem started, which
is what lets the round trip run as a plain test and what a dedicated
server would use later.

## The sections

| Id | Required | Holds |
|---|---|---|
| `STRT` | yes | The shared string table. Every name in the file is an index into it |
| `DEFS` | yes | One record per definition the battle can still reach, as a name index and a content hash |
| `CFGB` | yes | `BattleConfig` field by field |
| `WRLD` | yes | Map identity, the world scalars, the generator and the per player tallies |
| `CAMR` | no | Where the camera sat |

Each is written at an explicit width. No struct is ever handed to a
write call, so the 32 bit Windows build, the wasm32 browser build and
the 64 bit macOS and Linux builds all read each other's files. The
widths are spelled out as offset constants with a `_Static_assert` that
the last offset plus its size equals the declared total, so a field
added without moving the total breaks the build rather than writing a
save nobody can read back.

`CFGB` is 544 bytes, `WRLD` is 456, `CAMR` is 8 and a `DEFS` record is
12.

## Why the camera is its own section

The camera is local view state. Two players watching the same lockstep
battle are entitled to be looking at different corners of the map, so
`cam_x` and `cam_y` are deliberately outside the simulation hash. That
makes the camera optional rather than required: a reader that does not
care about it steps over the section, and a save carrying no camera
loads onto wherever the loader put the view.

## Why the generator is in `WRLD`

`World_RandState` is the simulation generator every unit script draws
through. A load that does not restore it drifts from the first draw on.
It cannot be restored by seeding, because `World_SeedRand` xors its
argument with a constant and forces the result odd (legacy:254493),
while a running generator is as often even. The save therefore carries
the raw state and puts it back through `World_SetRandState`, which this
work adds for that purpose.

`occ_version` is the opposite case. The loader bumps it on purpose so
the pathing clearance cache built in the previous session cannot be
believed, so it is not written and not hashed.

## What `DEFS` is for

A definition is written by name, never by index. Indices are not
portable across installs: an archive listing is sorted, but the loose
file walk is not, so a developer tree, a mod or a changed data set
produces a different registry order. Names survive all of that.

Beside each name is a 64 bit hash over the fields that decide how a
battle plays: health, sight, movement, footprint, the yardmap, the mana
figures and every weapon's range, damage, reload and damage categories.
Art, icons, sounds and display names are deliberately left out, so
re-skinning a unit does not refuse a save that used it. When a hash no
longer matches, the load is refused and names the definition that moved,
rather than loading a battle whose units quietly have different
statistics.

Only definitions the battle can still reach are written. A dead slot is
a tombstone whose definition index is whatever it held when the unit
died, so it is not followed. A definition reached only through a
production queue is, because the factory is still going to build it.

## The map fingerprint

The header's 32 byte field carries the fingerprint of the map the battle
was played on, taken through `TAK_MapFingerprint_FromName`. Two installs
can serve different terrain under one name, and a save restores exact
positions, so loading onto the wrong terrain puts units inside hills.

A map the writer cannot resolve leaves the field zero, which a reader
has to read as unknown rather than as a mismatch. The check that refuses
a save whose map has changed comes next.

## What is deliberately not here yet

The header's `rng_ai` and `unit_stable_id_next` stay zero. They belong
with the `AIST` and `UNIT` sections and are filled when those land.
`SUMM` is not written either: the load list that reads it is a later
piece of work, and the container already treats it as optional.
