# Scenery, corpses and the sweep

How the original treats the features on its maps: the body a unit
leaves and how it rots, the blast a death can throw, how blasts break
and burn the scenery, the wind a fire leans with, and what a sweep or a
raise does with what lies on the ground. Frames are the original's, 30
to a second.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Corpses in the data

A unit's FBI names its body in `corpse`, with `corpseadjustx` and
`corpseadjustz`, whole cells that shift the body's footprint origin
(legacy:163152-163159). Only the big keeps and castles use the adjust
keys, because their wreck is not the same shape as the building.

The body is a feature definition under `features/corpses/`. It carries
`object`, a 3DO model, in place of the `filename` and `seqname` pair of
a sprite that map scenery uses, and the loader reads `object` first and
reaches the sprite keys only when it is absent (legacy:127098-127136).
Beside the usual `reclaimable`, `blocking` and `damage` a corpse can
carry `decomposetime`, `resurrectable`, `animatable` and `featuredead`
(legacy:127369-127386, 127524).

Mobile units' corpses carry `resurrectable = 1`, `animatable = 1`,
`blocking = 0` and a `decomposetime` of 30 to 60. Buildings' wrecks
carry `blocking = 1` and none of those three keys, so a wreck never
rots and cannot be raised. No corpse file carries `energy`, so a swept
body pays nothing back. The engine pays a swept feature's `energy`, see
D-021.

## A unit's body

The moment a unit dies the original runs its script
`Killed(severity, corpsetype)` at once, up to its first sleep, and
reads the second argument straight back
(legacy:227142, 306199-306207). The script decides what is left: 1 for
the unit's own corpse, 2 for the wreck that corpse decays to, one step
down `featuredead`, and 0 for nothing. The value starts at 0, so a
script that never writes it leaves no body, and a unit with no script
leaves none either (legacy:227113). A frame still under construction
never leaves a body (legacy:227160). Only the low four bits travel in
the death packet (legacy:227165). The shipped scripts write the value
at the top of `Killed`, before any sleep, and the monarchs write 0.

The body goes down at the destroy step, which runs when the death
script finishes, not when the hit points reach zero
(legacy:227267, 227350). Its footprint origin is the unit's own origin
cell plus the adjust keys (legacy:227423), and it keeps the unit's
exact position, facing and team colour (legacy:128220-128232, 227429).
Placing it clears whatever features stand in its footprint, and an
indestructible one refuses it (legacy:128173-128185, 128843). A body
that lands below sea level sinks instead (legacy:227440-227450). The
engine departs here, see D-004.

## Rot

`decomposetime` is in seconds. The loader scales it by 30 into frames
and keeps the low 16 bits (legacy:127384-127386), and the feature tick
counts each placed body down by one frame with no gating
(legacy:128400-128402). So `decomposetime = 30` lies for thirty
seconds. Sweeping and raising both refresh the count on every step
they work (legacy:32394, 13143).

When the count reaches zero the body loses its reclaimable,
resurrectable and animatable bits and sinks by an eighth of a world
unit a frame, and after 60 frames the record is removed
(legacy:128403-128414). Every cursor check reads the body's own bits as
well as its definition's, so a sinking body takes no orders
(legacy:129476-129480, 129501-129505).

## The blast of a death

A unit file can name the weapon its death bursts with in an
`[EXPLODEAS]` section, and the one a self destruct bursts with in
`[SELFDESTRUCTAS]`. Both are inline weapons, read the way a `[WEAPONn]`
is (legacy:163574-163603). With Iron Plague and the patches mounted six
units have one: the Grenadier (423 over an area of 189), the Kamikaze
Rat (8000 over 203), the Dirigible (500 over 313), the Bomb Sprinkler
(4000 over 320), the Fire Wagon (180 over 210) and the Shock Trooper
(200 over 189). The Crusades set adds the Giant Orm (10000 over 100).
No unit has a `[SELFDESTRUCTAS]`, no building has either, and none of
them starts fires.

The weapon bursts as the unit is taken off the map, when its death
ends: after its death script, or at once for a unit with none
(legacy:227346-227349). Only a finished unit that was struck down
bursts. A frame does not, and neither does a unit the original removes
with no death of its own, as when a side's army is taken off the map
(legacy:227119-227144). A unit already dying when its side is taken off
still bursts (legacy:227574).

The burst is a blast on the ground where the unit stood, at its height,
through the routine a shell landing there runs
(legacy:245709-245739, 244963-245050). It plays the weapon's hit sound
and explosion, wounds every unit within half its area with the curved
falloff, friend or foe as a shot fired at the ground does, and then
reaches the scenery. The engine departs for an ally, see D-024. It is
the dead unit's player's blast and no unit fired it, so a kill counts
for that player and ranks no unit (legacy:227300-227327). A self
destruct bursts `[SELFDESTRUCTAS]` instead, see
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md). In
the engine a building its player sweeps away, which D-015 allows, does
not burst.

## Death pieces

A unit script's EXPLODE asks the host to throw the named piece. The
original does that only when the local player can see the unit, and
the pieces fly on the C library's generator, so they are a picture and
draw nothing from the simulation's generator (legacy:306894-306899).

## What a blast reaches

A blast reaches half the weapon's `areaofeffect` (legacy:245089). It
looks at every 16 px cell within that radius and one more, cut to the
map, rows outer and columns inner (legacy:245090-245111). On each cell
it takes the units first, as
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md) sets
out, and then the feature.

A feature drawn from a GAF is measured from the cell plus half the
def's footprint, on the ground there. A model, a wreck or a body, is
measured from where it lies, on the cell its record starts on. So a
feature larger than a cell is reached from each of its cells, each with
its own centre, and the cells to its lower right reach a little further
than its middle (legacy:245250-245280, 126554-126575). A feature takes
the hit when the distance in whole pixels is under the radius, or when
the cell is the blast's own (legacy:245281-245283). A shot with no
`areaofeffect` that lands on the ground hits the feature on that cell
and nothing else.

Each feature takes the weapon's whole `damage`, the default one, once
per blast. The original keeps the first cell of each feature it hits in
a list of 64 and passes over the rest of a feature it has already hit
(legacy:245284-245302). A blast that reaches more than 64 features hits
the ones it has no room to note once for each of their cells.

The pass is skipped for a `unitsonly` weapon (legacy:245240) and for a
shot that strikes a unit with an `areaofeffect` under 17, which hits
that unit alone (legacy:245029-245031). The same shot landing on the
ground, or stopped by a feature in its path, does reach scenery, the
feature in its own cell among it.

The original's host decides each hit on scenery and tells the other
machines (legacy:128759-128776, 128815-128822). The engine runs every
hit on every machine, see D-035.

## Damage and death

An indestructible feature takes nothing (legacy:128756). Any other
counts the damage it has taken in 16 bits, and when the count would
reach the def's `damage` it is destroyed (legacy:128781-128789). A
feature that is dying or burning takes nothing more. A model always
counts it (legacy:128798-128812).

A destroyed GAF feature with a `seqnamedie` plays it once and then its
`featuredead` takes the cell. Without one, or for a model, the next
stage comes at once (legacy:127838-127936). The stage is placed on the
same cell, clearing what stands in its footprint and refusing to land
on anything indestructible (legacy:127935-127955, 128130-128186). It
brings its own `blocking`. A stage with no `featuredead` leaves
nothing.

A sequence's pictures each show for the duration the GAF gives them in
the original's 30 Hz frames, the second word of each frame entry, and a
sequence that does not loop ends after its last
(legacy:255756-255795). The shipped deaths are ten to thirteen pictures
of two frames each.

The original's save starts a dying or burning feature again from its
first picture on load. The engine's save keeps the picture it had
reached, see D-035.

## Fire

A `firestarter` weapon's blast on a `flamable` feature sets it burning
instead of wounding it (legacy:128781, 128795). Only a GAF feature that
is not already dying or burning and has a `seqnameburn` can catch
(legacy:127772-127835). It plays its burn sequence with its
`seqnamebackflame` behind it and `seqnamefrontflame` before it, each a
TAF of its own name. With flames the burn loops and ends when both
flames have (legacy:127292-127330, 128501-128598), and without them it
ends with the burn sequence. Then `featureburnt` takes the cell
(legacy:128103-128116).

When it catches, a feature takes a spark time between half its
`sparktime` and all of it, in frames (legacy:127801). When that runs
out it throws one spread. Every flamable feature whose first cell is
within three cells catches with `spreadchance` percent, and so does one
at each of five steps along the wind (legacy:128021-128088). A step
adds twice the wind's two parts to a position kept in 1/65536 of a
cell, and a cell is tried only when a step reaches a new one. The roll
is made only for a feature that could catch.

In the shipped data that spread never comes. Every burning feature's
flames last 44 to 72 frames and its spark is 75 frames away at the
soonest, so a burn is over first. A fire burns what the flame reached
and stops, and a mod whose burn outlasts its spark spreads. The
remastered rules keep a burn going through four sparks that reach as
far as six cells and lean with the wind, see D-036.

The data names `burnweapon = TreeBurn` as a key, but the loader reads
only a `[BurnWeapon]` section (legacy:127389-127401), and no feature has
one, so burning scenery hurts nobody.

## Wind

The wind blows between the map's `minwindspeed` and `maxwindspeed`, 100
and 2000 on a map that names none (legacy:168998-169001). On the
battle's first frame, and whenever its time comes after that, it takes
a speed in that range and, when the speed is not 0, turns by up to an
eighth of a turn either way. Its two parts are the speed through the
original's table of sines, 512 steps of 1/8192. It next changes 3 to 8
seconds later when its east part is less than its north part either
way, else 5 to 14 (legacy:241674-241718). That time is drawn from the C
library's generator, the stream every picture shares. The engine gives
the wind a stream of its own, see D-035.

The units whose file says `wind` or `windgenerator`, flags, sails and
keeps, run their `WindChange` script with the speed and the wind's
heading less their own on the frame after a change, and scripts read
the same through the two GET values that ask for it
(legacy:178908-178916, 223290-223293). The skirmish maps blow 25 to
5000, so a fire's downwind steps go at most a cell.

## The sweep click

The sweep order resolves onto the map cell under the click, and each
selected unit makes its own choice, in this order
(legacy:187127-187198). A unit with `canresurrect` over a resurrectable
body gets the raise order, a unit with `cananimate` over an animatable
feature gets the same order to animate, and anyone else with
`canreclaim` over a reclaimable feature gets the sweep. The whole
branch is gated on `canreclaim` (legacy:187127). The engine's sweep
also takes down a building of your own, see D-015.

The original keeps a swept feature a moment before it clears the cell,
and plays no sprite. The engine plays `Death01` where a finished
sweep's feature stood, see D-044. The archives' `anims/deathmagic.gaf`
holds two sequences, `Death01` of 20 frames, a dark red dispersal, and
`PurpleDeath` of 15 frames. Nothing in the game's data ties either to
a sweep.

## Raising the dead

The ground raise order (legacy:12931-13210) walks to the body, turns to
face it, takes the build pose and works. Every step checks the feature
again and fails with the repair chatter if it is gone or rotted
(legacy:13065).

What comes back is the unit whose name is the feature's name up to its
first underscore, so `arasword_dead` brings back ARASWORD. An animation
makes the raiser's `animatetype` instead (legacy:13061-13072).

The work owed is `buildtime * 0.3 / (workertime / 30)` in 16.16 frame
units for a resurrection, and `buildtime / (workertime / 30)` for an
animation (legacy:13076-13078). Each frame takes off the player's
supply fraction times 65536, the same figure that scales a build
(legacy:13087, 39469), so a raise at full supply takes
`buildtime * 9 / workertime` frames. Elsin raising a Swordsman takes
about three seconds, a Horseman about six. The supply fraction is the
treasury's of [economy.md](economy.md). The engine raises at full
supply whatever the treasury holds, see D-003.

Nothing is charged. The only cost check consults the computer player's
unit limits and returns true for a human (legacy:14975-14985).

When the work is done the unit is created on the body's spot for the
raiser's player, facing the way the body lay, and the body is removed
(legacy:13162-13185). A resurrected unit comes back at a tenth of its
hit points, at least one, and the raiser is given a repair order on it
(legacy:13186-13205). An animated unit comes back whole.
