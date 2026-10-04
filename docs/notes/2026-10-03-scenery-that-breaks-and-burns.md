# Scenery that breaks and burns

The original's blasts wound the scenery they reach. A tree falls to a
dead tree and then to a smudge, a wall drops a stage and then lies as
rubble, a building caves in to a ruin, and a fire starter sets the
things that burn alight. The engine had none of it: a placed feature had
no hit points and nothing a shot did reached it. This note is how the
original does it and how the engine follows.

## What a blast reaches

A blast reaches half the weapon's `areaofeffect` (legacy:245089). It
looks at every 16 px cell within that radius and one more, cut to the
map, rows outer and columns inner (legacy:245090-245111). On each cell
it takes the units first (see the shots note) and then the feature.

A feature drawn from a GAF is measured from the cell plus half the
def's footprint, on the ground there. A model, a wreck or a body, is
measured from where it lies, on the cell its record starts on. So a
feature larger than a cell is reached from each of its cells, each
with its own centre, and the cells to its lower right reach a little
further than its middle (legacy:245250-245280, 126554-126575). A
feature takes the hit when the distance in whole pixels is under the
radius, or when the cell is the blast's own (legacy:245281-245283). A
shot with no `areaofeffect` that lands on the ground hits the feature on
that cell and nothing else.

Each feature takes the weapon's whole `damage`, the default one, once
per blast. The original keeps the first cell of each feature it hits in
a list of 64 and passes over the rest of a feature it has already hit
(legacy:245284-245302). A blast that reaches more than 64 features hits
the ones it has no room to note once for each of their cells, and the
engine does the same.

The pass is skipped for a `unitsonly` weapon (legacy:245240) and for a
shot that strikes a unit with an `areaofeffect` under 17, which hits that
unit alone (legacy:245029-245031). The same shot landing on the ground,
or stopped by a feature in its path, does reach scenery, the feature in
its own cell among it.

## Damage and death

An indestructible feature takes nothing (legacy:128756). Any other
counts the damage it has taken in 16 bits, and when the count would
reach the def's `damage` it is destroyed (legacy:128781-128789). A
feature that is dying or burning takes nothing more. A model always
counts it (legacy:128798-128812).

A destroyed GAF feature with a `seqnamedie` plays it once and then its
`featuredead` takes the cell. Without one, or for a model, the next stage
comes at once (legacy:127838-127936). The stage is placed on the same
cell, clearing what stands in its footprint and refusing to land on
anything indestructible (legacy:127935-127955, 128130-128186). It brings
its own `blocking`, so the route planner's cached layers are patched over
the footprint as when any feature comes or goes, and the shot heights
over it are taken again. A stage with no `featuredead` leaves nothing.

A sequence's pictures each show for the duration the GAF gives them in
the original's 30 Hz frames, the second word of each frame entry, and
a sequence that does not loop ends after its last (legacy:255756-255795).
The engine plays them on every second tick, the original's frames. The
shipped deaths are ten to thirteen pictures of two frames each.

## Fire

A `firestarter` weapon's blast on a `flamable` feature sets it burning
instead of wounding it (legacy:128781, 128795). Only a GAF feature that
is not already dying or burning and has a `seqnameburn` can catch
(legacy:127772-127835). It plays its burn sequence with its
`seqnamebackflame` behind it and `seqnamefrontflame` before it, each a
TAF of its own name. With flames the burn loops and ends when both
flames have (legacy:127292-127330, 128501-128598), without them it ends
with the burn sequence. Then `featureburnt` takes the cell
(legacy:128103-128116).

When it catches, a feature takes a spark time between half its
`sparktime` and all of it, in frames (legacy:127801). When that runs out
it throws one spread: every flamable feature whose first cell is within
three cells catches with `spreadchance` percent, and so does one at each
of five steps along the wind (legacy:128021-128088). A step adds twice
the wind's two parts to a position kept in 1/65536 of a cell, and a cell
is tried only when a step reaches a new one. The roll is made only for a
feature that could catch.

In the shipped data that spread never comes. Every burning feature's
flames last 44 to 72 frames and its spark is 75 frames away at the
soonest, so a burn is over first. A fire burns what the flame reached
and stops. A mod whose burn outlasts its spark spreads, and so does
every fire under the remastered rules, which keep a burn going until
its spark (D-036).

The data names `burnweapon = TreeBurn` as a key, but the loader reads
only a `[BurnWeapon]` section (legacy:127389-127401), and no feature
has one, so burning scenery hurts nobody.

## Wind

The wind blows between the map's `minwindspeed` and `maxwindspeed`,
100 and 2000 on a map that names none (legacy:168998-169001). On the
battle's first frame, and whenever its time comes after that, it takes a
speed in that range and, when the speed is not 0, turns by up to an
eighth of a turn either way. Its two parts are the speed through the
original's table of sines, 512 steps of 1/8192. It next changes 3 to 8
seconds later when its east part is less than its north part either
way, else 5 to 14 (legacy:241674-241718).

The units whose file says `wind` or `windgenerator`, flags, sails and
keeps, run their `WindChange` script with the speed and the wind's
heading less their own on the frame after a change, and scripts read the
same through the two GET values that ask for it (legacy:178908-178916,
223290-223293). The skirmish maps blow 25 to 5000, so a fire's downwind
steps go at most a cell.

## Death blasts

A unit file can name the weapon its death bursts with in an
`[EXPLODEAS]` section, and the one a self destruct bursts with in
`[SELFDESTRUCTAS]`. Both are inline weapons, read the way a `[WEAPONn]`
is (legacy:163574-163603). With Iron Plague and the patches mounted,
six units have one: the Grenadier (423 over an area of 189), the
Kamikaze Rat (8000 over 203), the Dirigible (500 over 313), the Bomb
Sprinkler (4000 over 320), the Fire Wagon (180 over 210) and the Shock
Trooper (200 over 189). The Crusades set adds the Giant Orm (10000 over
100). No unit has a `[SELFDESTRUCTAS]`, no building has either, and none
of them starts fires.

The weapon bursts as the unit is taken off the map, when its death
ends: after its death script, or at once for a unit with none
(legacy:227346-227349). Only a finished unit that was struck down
bursts. A frame still being built does not, and neither does a unit
the original removes with no death of its own, as when a side's army
is taken off the map (legacy:227119-227144). A unit already dying when
its side is taken off still bursts (legacy:227574). The engine also
gives no burst to a unit a builder takes apart.

The burst is a blast on the ground where the unit stood, at its
height, through the routine a shell landing there runs
(legacy:245709-245739, 244963-245050). It plays the weapon's hit sound
and explosion, wounds every unit within half its area with the curved
falloff, friend or foe as a shot fired at the ground does (D-024), and
then reaches the scenery. It is the dead unit's player's blast and no
unit fired it, so a kill counts for that player and ranks no unit
(legacy:227300-227327). A death that still owes its blast keeps owing
it across a save. A self destruct would burst `[SELFDESTRUCTAS]`, and
the engine has no self destruct order yet.

## Death pieces

A unit script's EXPLODE asks the host to throw the named piece. The
original does that only when the local player can see the unit, and the
pieces fly on the C library's generator, so it is a picture and draws
nothing from the simulation's generator (legacy:306894-306899). The
engine marks the piece and draws nothing either.

## How it differs

- The original's host decides each hit on scenery and tells the other
  machines (legacy:128759-128776, 128815-128822). Every machine runs the
  same hits here, as with everything else in lockstep.
- The wind's next change is drawn from the C library's generator in the
  original, the stream every picture shares. Here it has a stream of its
  own, started where the original starts its own at the battle's start
  (D-035).
- A save keeps a death or a burn where it was. The original's save
  starts it again from its first picture on load.
