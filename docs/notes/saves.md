# Saves

This note covers the original's saved game, then OpenKingdoms' own
.oksave format: the container, the sections a battle is written into,
what a load restores and what it rebuilds, and the picture the load
dialog shows. Where save files live on disk is in [files.md](files.md).

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## The original's saved game

A save is a HAPI key and value bank, divided into sections and stamped
with the magic `Kingdoms Saved Game v1`. The debug console's save
command writes one to `savegame\<name>.sav` (legacy:1524,
legacy:36857-36868).

None of the save system lives at legacy:195119-199360. That range holds
network packet marshalling (legacy:195119-195460), player colour slot
allocation (legacy:196155-196395) and the persistence of the
Kingdoms.ini settings (legacy:199006-199360).

### Writing and reading

| Operation | Where |
|---|---|
| Write a save, the summary first | `SaveGame_WriteSummary`, legacy:164912-165007 |
| Load a save | `SaveGame_LoadSummary`, legacy:164888 |
| Open the Summary section alone, for the save browser's list | `SaveGame_OpenSummary`, legacy:164823 |
| Open the whole file | legacy:164862 |
| Close | legacy:164850 |

### The bank

| Operation | Where |
|---|---|
| Set up | legacy:253054, legacy:253130 |
| Free | legacy:253063 |
| Open an archive | legacy:253144-253406 |
| Write an entry | legacy:253407 |
| Load an account | legacy:253796 |
| Find or add a section | legacy:253908 |
| Set an int, an int64, a string | legacy:253952, legacy:253977, legacy:254002 |
| Get an int, a float, a string | legacy:254029, legacy:254048, legacy:254067 |
| Ask whether a key exists | legacy:254086 |
| The key table | legacy:254097-254238 |
| The section count | legacy:254264 |
| LZW compress and decompress | legacy:272156, legacy:273081 |
| Audit a file | legacy:254379 |

Getting a key by its index walks the entries as a positional stream of
blobs rather than looking anything up. The section count call returns
the byte length of the current blob key, and it is used as a size
guard.

### The Summary

The Summary is what the save browser reads without opening the rest of
the file. Its keys, in the order they are written:

| Key | Value | Written |
|---|---|---|
| Two keys whose names carry the build date and the build time | zero | always |
| `maxunits` | the FavoriteUnitsPerPlayer option, 20 to 500 | always |
| `Campaign` | campaign id | always |
| `Mission` | mission id | always |
| a key for the player dialog's button | UI selection state | always |
| a key for the side | the player's side | always |
| `Players` | player count | always |
| `Gametype` | the game mode, 1 campaign, 2 skirmish, 3 multiplayer | always |
| `CommanderDeath` | the MonarchDeath battle option | skirmish |
| `Location` | the RandomStartLocations battle option | skirmish |
| `Mapping` | the MappingOn battle option | skirmish |
| `LineOfSight` | the LosOn battle option | skirmish |
| `BetweenMissions` | 1 | between missions only |
| `Victory` | bit 2 of the game state's mission result flags | between missions only |
| `Description` | the save's description | always |
| `Game ID` | game id | always |
| `Game Time` | the elapsed game time counter | always |
| `Radar Image` | a blob of map width times map height times 2 bytes, the radar picture | in a battle, not between missions |

The build date and time are never compared, and the file carries no
schema version, so a save from any build is accepted whatever has
changed (legacy:164927-164930). `maxunits` is also the only
back-compatibility probe on load: an older bank is told apart by that
key being absent. The radar picture is written at
legacy:164966-164975.

Two sections may follow. `[UnitBalance]` holds `V3Units=1`
(legacy:164986). `[CreonUnits]` holds `CreonUnits=1` when any player is
side 7 (legacy:164995), and what that means for a load is in
[files.md](files.md).

### The sections it writes

| Section | Written at | Read at |
|---|---|---|
| `Units` | legacy:227938-228193 | legacy:227585-227619 |
| `PlayerFeatures` | legacy:226100-226132 | legacy:226136-226177 |
| `Mapping`, the fog | legacy:226181-226193 | legacy:226197-226216 |
| `Players` | not established | legacy:207011-207060 |
| `MapScript` | legacy:177901 | legacy:177916 |
| The victory and defeat conditions | legacy:239858-239886 | legacy:239890-239918 |

The routine just before the Players reader, legacy:206939-207010, is
not the Players writer. It adds the line for a fallen player, see
[gameflow.md](gameflow.md).

The camera is written at about legacy:120860-120960, under the keys
`PresetX%d`, `PresetZ%d`, `PresetUsed%d`, `X Position` and
`Z Position`. Each victory and defeat condition saves and loads itself
through its own entry points, in sections named `VictoryCondition_*`
and `DefeatCondition_*`.

### Units

The section's header keys are `Version`, which is 0x2C (44), and
`Number of Units`. The writer walks the whole unit pool, a contiguous
array of fixed stride, and skips any unit that does not carry the
persist flag.

Each unit it keeps becomes a fixed record of 235 bytes (0xEB). The
fields, in order:

| Field | Notes |
|---|---|
| Unit type name | 32 bytes, copied from the type definition |
| Unit id | u16 |
| Is the commander | u8 |
| Build percentage | u16 |
| World position x, y and z | 20.12 fixed point |
| A fourth position value | stored beside those three |
| Heading | two separate stored heading values |
| Health | two values, the current one and its companion |
| Target unit id | checked against the live pool when saved |
| Attacker id | 0xFF means none |
| Weapon states | 8 of them (legacy:228106-228111) |
| Other counters | 9 of them |
| The flag word, repacked | see below (legacy:228126-228137) |
| Build queue or cargo | a count byte, then 7 u16 entries |
| Piece transforms | 3 records of 16 bytes |

The order of the fields and the total of 235 bytes are established. The
width of every field is not, so the record has not been summed to 0xEB
field by field.

The unit's persistent flag word is not stored raw (legacy:228126-228137).
Bits 0 to 9 are shifted left by 4. Bit 0xB is stored, and so are bits
0xC to 0x15. A nibble from a second flag field is stored, then the low
nibble of a third field and then its top nibble. Transient render flags
are dropped on purpose, so they never survive a save and a load.

Two linked lists are walked for orders, the unit's order queue and its
patrol queue, and each node is keyed `u%04xm%04x`
(legacy:228024-228034). The unit's capture state is saved separately
(legacy:228037).

Script state is saved. Each unit gets a `Script%i` key holding its COB
engine's threads: program counters, stacks, locals, signals and sleeps
(legacy:228018-228020). Without it a reload would start every script
from the top.

### Mapping, PlayerFeatures and Players

`Mapping` has one key, `Data`: the per-cell visibility bitmask, one u16
a cell with a bit per player. That is map width times map height times
2 bytes, written out as a quarter of that number of dwords. A load
refuses a blob of the wrong size. Whether that guard compares the byte
count or the dword count is not established.

`PlayerFeatures` has one key, `Plotmap`, of map width times map height
over 2 bytes. Each byte packs two nibbles, the damage and the burn value
of a cell's feature record.

`Players` holds each player's side, colour, ally mask, defeat flag and
economy. Mana is kept here, not with the units.

### Loading

A load (legacy:164888) reads the Summary, then Players, the camera,
network fixups, PlayerFeatures, Mapping and Units, and last restores
each objective's `Satisfied` and `Celebrated` flags.

- The unit loader wants `Version` 0x2C. A record of 0xEB bytes reads
  normally, and one of 0xE9 (233 bytes, the older form) reads with its
  trailing field zeroed (legacy:227605-227609). Any other length is
  skipped. The units are then made at legacy:227623.
- On a version mismatch the unit loader returns without a word, leaving
  the player on the right map with an empty world and no message
  (legacy:227596-227597).
- Objectives are parsed again from the mission file, and the bank
  restores only their two flags, whose meaning is in
  [gameflow.md](gameflow.md).

## The container

This section and the two after it describe OpenKingdoms' .oksave
format, not the original.

A save is a snapshot, not a replay. It writes the simulation's current
values and restores them exactly. The container carries bytes and knows
nothing about units or maps: the state writers hand it finished
payloads and ask for them back by four character code.

### The file

A save is `<slug>.oksave`. It is written as `<slug>.oksave.tmp` and
then moved over the real name, so a crash in the middle of a write
leaves the previous save intact. The digest is there to catch media
damage, not interrupted writes.

Every integer is little endian and every field is written at an
explicit width, never as a struct, because the 32-bit Windows build and
the wasm32 browser build lay a struct out differently from the 64-bit
macOS and Linux builds. Floats travel as their IEEE-754 binary32 bit
pattern and round trip bit for bit, which holds only while no build sets
a fast math flag.

### Header, 240 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8[8] | magic, `4F 4B 53 41 56 45 1A 0A` |
| 8 | u32 | container_version, 1. A reader refuses anything higher |
| 12 | u32 | header_bytes, 240 |
| 16 | u64 | file_bytes, the first truncation check |
| 24 | u32 | schema_version, the layout of the simulation state |
| 28 | u32 | flags. Bit 0 some section is deflated, bit 1 campaign |
| 32 | u8[32] | SHA-256 over the whole file with these 32 bytes read as zero |
| 64 | u32 | section_count |
| 68 | u32 | sim_tick |
| 72 | u32 | sim_state_hash, the simulation hash before the write |
| 76 | u32 | rng_ai, the AI's generator |
| 80 | u8 | determinism_class. 0 float simulation, 1 fixed point |
| 81 | u8 | tick_hz, 60 |
| 82 | u8 | save_kind. 1 skirmish, 2 campaign in battle, 3 between missions |
| 83 | u8 | float_form. 0 binary32 bit patterns, 1 fixed |
| 84 | u32 | unit_stable_id_next, so ids stay unique after a load |
| 88 | u32 | unit_slot_count, dead slots included |
| 92 | u64 | saved_at_utc |
| 100 | char[64] | engine_build, `<version>+<git describe>` |
| 164 | u8[32] | map_fingerprint, all zero when unavailable |
| 196 | u8[44] | reserved, zero |

The build stamp is recorded and never compared. A save from another
build with the same schema loads, because refusing every save on every
release is worse for the player than the small risk the state hash
already catches. A save whose schema version is not the build's is
refused, naming both versions, rather than read as half a battle. A save
from another determinism class is refused, saying why, rather than
converted.

### Section header, 16 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | id, a four character code, in file order |
| 4 | u16 | version |
| 6 | u16 | flags. Bit 0 required, bit 1 deflated, bit 2 record array |
| 8 | u32 | stored_bytes |
| 12 | u32 | plain_bytes, equal to stored_bytes when not deflated |

### The walk

1. A section header or payload that would run past `file_bytes` means
   the file is truncated. Refuse, naming the code.
2. An unknown id with the required bit clear is stepped over by its
   `stored_bytes`, so a newer writer's optional section is invisible to
   an older reader.
3. An unknown id with the required bit set is refused by its code.
4. A known id at a higher version than the reader handles is treated
   by rules 2 and 3 in the same way.
5. A known id at a lower version either has an explicit upgrade path
   or is refused, naming the id and both versions.
6. A record array carries `u32 count` then `u16 fixed_bytes`. A
   matching width reads normally. A smaller stored width reads the
   prefix and zeroes the tail. A larger one reads the known prefix and
   steps over the rest.

Rule 6 is the original's own tolerance (legacy:227605-227609), and it
is the half that lets old saves keep loading, where a version key alone
would make every added field invalidate every save. It binds the
writer: within one section version, fields are append only and zero
must be a safe default for every appended field. A field whose zero is
not safe forces a version bump.

### Compression

Each section is deflated on its own, at level 1, which keeps most of
what level 9 would gain at a small part of its cost. Most of a battle's
bytes are zeros, unused stack slots, empty occupancy cells and uniform
fog, which is why each section is deflated rather than the whole file.
A section that does not shrink by a useful margin is stored plain.
`SUMM` is set aside for a summary a load list could read with one short
header per file, so it is never deflated. The writer emits none, and
readers treat it as optional.

The format is not the original's bank, because nothing else reads these
files. It does not reuse `.sav`, which the original's debug console
command writes, so the two never collide in a shared folder.

### Refusals and portability

Every refusal names what is wrong in the player's terms: not a saved
game, written by a newer version of the game, incomplete, damaged, or
carrying a section this build does not know. A save moves between
Windows, macOS, Linux and the browser in both directions, because
restoring exact bit patterns leaves no second machine to disagree.

## Sections

This section describes the sections of OpenKingdoms' .oksave, not the
original.

| Id | Required | Holds |
|---|---|---|
| `STRT` | yes | The shared string table. Every name in the file is an index into it |
| `DEFS` | yes | One record per definition the battle can still reach, a name index and a content hash |
| `CFGB` | yes | The battle configuration, the session seed the lobby drew among it |
| `WRLD` | yes | Map identity, the world scalars, the simulation generator, the per-player tallies, the diplomacy the battle set and the seats that resigned |
| `UNIT` | yes | One record per unit slot, dead slots included |
| `UPTH` | yes | The live prefix of each unit's route |
| `UCOB` | yes | Pieces, statics and all sixteen script threads of each unit, each with its whole stack |
| `PROJ` | yes | One record per projectile pool slot |
| `FEAT` | yes | One record per feature, corpses among them |
| `FOGV` | yes | Every player's fog layer |
| `ECON` | yes | Each player's mana pool and its windows |
| `AIST` | yes | The AI's generator, its per-player records and order matrices, then each seat's failed build sites |
| `OCCU` | yes | The unit occupancy layer, four bytes a cell |
| `CMDQ` | yes | The orders still waiting for their tick |
| `CAMR` | no | Where the camera sat |
| `THMB` | no | The picture for the load dialog, see below |

Any change to a record's layout bumps the schema version. An `AIST`
that stops before the build sites loads with none. Nothing that writes
or reads a save touches a platform or a window, so a save is written
and read with no window open and no SDL subsystem started.

### What goes in

The simulation hash decides. A field it covers is in the file. A field
it leaves out is either derived state a load rebuilds or local view
state two lockstep peers may disagree about, and the table under State
that looks derived says which for each structure. Every required
section but `OCCU` and `CMDQ` carries state the hash covers, so a reader
that stepped over one would bring up a battle that is not the one that
was saved. Those two are outside the hash and still in the file, for
the reasons below.

The session seed is in `CFGB` because every draw the simulation makes
comes off it, and a battle restarted from a loaded one has to start
from the same number. The diplomacy and the resigned seats are in
`WRLD` and in the hash because they decide who shoots whom, who sees
what and who is counted out.

### The camera

The camera is local view state. Two players watching the same lockstep
battle are entitled to look at different corners of the map, so it is
outside the simulation hash and its section is optional. A reader that
does not care about it steps over it, and a save with none loads onto
wherever the loader put the view.

### The generator

`WRLD` carries the raw state of the simulation's generator, which every
unit script draws through. A load that did not restore it would drift
from the first draw. It cannot be restored by seeding, because seeding
xors the seed with a constant and forces the result odd, as the
original does (legacy:254493), while a running generator is as often
even.

### Definitions

A definition is written by name, never by index. Indices are not
portable across installs: an archive listing is sorted but the loose
file walk is not, so a developer tree, a mod or a changed data set gives
a different registry order. A unit, and every entry of a factory's
production queue, names its definition by its record in `DEFS`, and
the name is resolved against the local registry on the way back in.

Beside each name is a 64-bit hash over the fields that decide how a
battle plays: health, sight, movement, footprint, the yardmap, the mana
figures, every weapon's range, damage, reload and damage categories,
and the unit's script. The script counts as behaviour rather than art,
since it decides when a unit fires, what it hides and what its build
stance is, so the hash covers its code words, its entry points and
their offsets, the piece names that bind it to the mesh and the number
of static variables. Art, icons, sounds and display names are left out,
so re-skinning a unit does not refuse a save that used it. When a hash
no longer matches, the load is refused and names the definition that
moved.

Only definitions the battle can still reach are written. A dead slot's
definition is not followed. A definition reached only through a
production queue is, because the factory is still going to build it.

### The map

The header's fingerprint field carries the fingerprint of the map the
battle was played on, because two installs can serve different terrain
under one name and a save restores exact positions. On load the
fingerprint of the installed map of that name is worked out again and
compared. A mismatch is refused and names the map. A map that does not
resolve is refused and named as missing. A stored fingerprint of all
zeros was written by a build that could not resolve the map, so it reads
as unknown and the check is skipped. The check needs the archives
mounted, which they are from startup on.

### Unit slots and handles

A unit refers to another unit by slot index in nine places: its target,
its build target, the transport carrying it, each entry of its load
queue, the cargo it is passing on, a projectile's target and shooter,
and the AI's target and threat.

Slots are reused, the lowest dead slot first, but a unit never moves
slot and nothing is compacted. Slot i is written to record i and read
back into slot i, tombstones and all, so every one of those references
still points at the same thing with no remap table. A projectile whose
firer died in flight still names a slot, and it is still the same
tombstone.

A tombstone keeps four fields: its lifecycle byte, its stable id and
the two that say where it fell. The rest of its record is zero, because
everything else in a dead slot is whatever it held at the moment of
death. Where it fell matters because reusing the slot reads the dead
unit's last position, so a shot still chasing it lands on the body
rather than at the map's corner. That position is in the hash as well.

Stable ids are the second, independent check. They are in the file,
and the counter for the next one travels in the header. The AI keeps
both a handle and a stable id for its target and its threat, which lets
it notice a handle that no longer means what it did.

### Scripts in mid execution

Each live unit's COB engine has sixteen thread slots, and all sixteen
go in the file, dead ones included. A weapon's aim result is read off a
slot whose thread has already finished, and a zeroed dead slot would
make a unit whose script said hold fire shoot instead.

The whole of each thread's stack goes in. A COB local is a stack slot
that POP-VAR writes by index with no relation to the stack pointer, and
a finished thread's slots are read with no bound at all, which is how
`Killed` hands back the corpse it asked for (legacy:227142) and how
`QueryBuildInfo` hands back its build spot.

A thread's program counter is a word index into the script its
definition owns, which is why the definition hash covers the script.
The piece array is sized by the unit's mesh, which the hash leaves out,
so the stored piece count is checked against the live one and a
mismatch is refused by the unit's name.

A load does not spawn units the ordinary way. Spawning runs `Create`
at once, and `Create` plays sounds and writes the unit's COB port
bytes. A load binds each unit's script engine and stops there, and the
file's threads land on a clean slate. The loading screen's final phase,
told the battle is being restored, spawns nothing at all: no monarchs,
no campaign placements and no mana seeding, because the save carries
the pools that seeding would create.

### Fog

Fog is history. It cannot be worked out from where the units stand,
because that loses every cell a player explored and can no longer see.
Every seat's layer goes in the file, because the AI, the minimap and
the line of sight test read layers the human never sees. Player one's
layer is an alias and is pointed again on load rather than stored
twice.

A unit's revealed cells are worked out once and stamped again until the
unit has moved 16 px from where they were worked out, so the ground it
lights follows that anchor rather than its exact position. The anchor,
with the sight and the lit flag it was taken with, is state: it is
hashed and saved. The cell list is a cache, a function of the anchor.

### State that looks derived

Something that can be rebuilt from the present is not always a function
of the present. Two lockstep peers share a history and build the same
answer, so the hash can leave such state out, but a save has no
history, so the file must carry it. The unit occupancy layer is the
plainest case: two footprints can cover one cell and the cell belongs
to whichever claimed it first, so a restamp comes close and is not the
same, and a mover that finds one cell held differently takes a
different step within a few ticks.

| Structure | On a load | Why |
|---|---|---|
| Stable id to slot index | Rebuilt | A function of the slots, and every command names its units by id |
| Unit spatial grid | Rebuilt | A function of where the units stand |
| Path plan cache | Rebuilt | Derived state, outside the hash |
| Terrain passability cache per move class | Dropped | Terrain only, rebuilt on the next plan |
| Clearance map version | Bumped, never written or hashed | A clearance map from the previous session must not be believed |
| Unit occupancy layer | Restored | A contested cell belongs to whichever unit claimed it first |
| Per-unit fog reveal cell list | Rebuilt | A function of the anchor |
| Fog reveal anchor | Restored | Where the unit stood when the scan last ran |
| Mana cap and regeneration rate | Restored | Adjusted in place on every capture, loss and monarch spawn, never recomputed from the world |
| A wedged unit's rungs on the unsticking ladder, its route serial and the stall fields beside them | Restored | A function of nothing else the unit carries, and in the hash too |
| AI influence maps | Rebuilt | Stamped again on the next think, from the units and the map |
| AI per-player records, order matrices, generator | Restored | Targets, threat and build intent are decisions already taken |
| COB piece to node table and host callbacks | Bound again | A function of the model and the host |
| Build menu cache | Untouched | Keyed by definition, not by battle |
| Mesh bakes, texture atlases, palettes | Untouched | Art built from the data set, never in the hash |
| Selection, control groups, draw order, camera, HUD | Not needed | View state peers may disagree about |
| Sound cooldown counters and the side alarm row | Not needed | Audio, outside the hash |
| Build placement scratch bitmap | Rebuilt | Filled inside each placement query |
| Definition registry order | Never travels | Definitions go by name |

### Projectiles

A shot carries its damage, its area of effect and its per-category
damage multipliers, because those decide what happens when it lands,
and its three sound names as string table indices. It does not carry
its art slot, its explosion class slot or its beam colours, which index
caches filled in first fire order and mean nothing in another session.
An ordinary shot in the air at the save comes back doing the right
damage and drawn as nothing. A wandering shot keeps its art by name.

### Orders still waiting

A player's order is queued for a tick and runs when that tick comes.
The queue runs at the top of a tick and the AI submits for the next one
at the bottom, so a save is normally taken with orders in hand, and
dropping them would cancel every order in flight. `CMDQ` carries the
tick the queue is about to run, its delay, the arrival counter that
orders commands within a seat's tick, and every waiting command with
its own arrival. Only the unit ids a command names are written.

`CMDQ` is outside the simulation hash, because two peers may hold
different commands in flight when one a player has just given has not
reached the others. A restored command need not land in the slot it
came from, since the queue runs by seat and then by arrival, never by
slot.

### Campaign battles

A campaign unit carries no cursor into its placement's order list. The
mission's opening orders run to the end once, in the loading screen's
final phase, and each becomes ordinary unit state on the way: a move
order, a patrol anchor, an owner, a velocity, an attack target or a
building under construction, all of it in `UNIT`. A mission's
objectives are read from the mission file on load, as the terrain is,
and as in the original only whether each has been met and celebrated
travels in the save.

### Loading

A load reads the save's light state for the battle configuration, the
map and its kingdom, begins loading that world with the restoring flag
raised, runs the loading screen to the end, applies the save and opens
the battle. A save is refused while the world has not finished loading,
because a battle part way through the loading screen has a map name and
little else.

The restoring flag is put down by the loading screen's final phase once
it has acted on it, by ending a world, and by starting any battle, so a
load abandoned before the save is applied cannot leave the next battle
spawning nothing. Every definition the save names is checked before
anything is written into the world, so a data set that moved is refused
with the world as the loading screen made it. A refusal after that
point leaves part of a battle, so it marks the world as not loaded, and
the caller still owns the teardown.

## The thumbnail

This section describes OpenKingdoms' save picture. The load dialog
authors a panel called `RadarView`, 127 by 127, and the save dialog one
at 125. The original paints the battle into it from the radar image in
the save's Summary (legacy:164966-164975).

`THMB` is optional, like `CAMR` and for the same reason: it is local
view state, outside the hash, and a reader that does not know it steps
over it. A save with no `THMB` shows the panel as the art authored it.

The payload is six bytes of header, a `u16` width, a `u16` height, a
byte of bytes per pixel and a byte of padding, then width times height
RGB triples, row major. The picture is 128 by 128, 48 KB before the
container deflates it, and most of it is flat fog and flat terrain. The
size travels in the payload, so changing it needs no version bump and
an older file still reads.

The screen that presses Save draws the picture and hands it to the
writer, which copies it, uses it and clears it whether or not that save
carried it. The container never reaches for the renderer, which keeps
it usable with no window.

The picture is the composite the sidebar radar draws, made on the CPU:
the map's overview image through the map's palette, then fog in the
three cases the radar uses, then one dot per unit the viewer can see in
its owner's colour. Fog is gated on Line of Sight exactly as the radar
gates it, so a battle played with it off makes a picture with no fog
shading. A world with no overview image, or no pixel format because no
window was opened, gives no picture, and the save is written without
one rather than refused.

The dialog decodes the selected save's picture once per change of
selection, not once per frame, and paints it over the authored panel at
the panel's own size, nearest neighbour.
