# Shots meet what they fly into (2026-09-27)

Playing the remaster, the owner reported that magic and ballistic shots
went through hills and walls. They did. Only a ballistic shell fired at
a point on the ground ever looked at the ground under it. A shot at a
unit tested nothing but its distance to that unit, so an arrow flew
through a ridge, and every flat shot was pinned to the terrain height,
so a bolt or a fireball could not be stopped by anything at all.
Lightning and flame hit their target the moment they were fired.

The original stops them. This note records how, what the engine does
now, and what it still leaves out.

## The original's test

Every flying shot splits its speed into steps of 16 px or less
(legacy:250425-250433) and runs one test after each step, on the map
cell it has reached (legacy:245377-245476). The order is fixed:

1. Off the map, the shot is gone with no burst (legacy:245393-245396).
2. The unit standing in the cell stops it when that unit's owner is
   not the shot's owner (legacy:245419) and the shot's height lies
   between the unit's base and the top of its model
   (legacy:236955-236958). The shooter's own units, buildings and walls
   never stop its shots. An ally's do.
3. A flyer over the cell stops it when the shot's height is inside the
   flyer's model (legacy:245426-245437).
4. A weapon with `unitsonly` stops looking here (legacy:245440-245443).
5. A feature in the cell stops it when the shot is no higher than the
   cell's floor plus the feature's height (legacy:245444-245461). The
   floor is the lowest of the cell's four corners (legacy:224590-224622).
6. A shot above the ground and under the sea bursts on the water,
   unless the weapon is a `waterweapon` or the map sets
   `nosealeveltrigger` (legacy:245463-245468).
7. At or under the floor it bursts, or bounces with a quarter of its
   vertical speed for `groundbounce` (legacy:245470-245476).

A shot aims at its target's SweetSpot piece, the piece the unit's
script names for the purpose, or piece 0 when the script names none
(legacy:186005-186013, legacy:234031-234042). 162 of the 217 shipped
scripts name one. The rest are walls, lodestones and the like.

The ballistic shell runs the test on every step of its arc
(legacy:246642-246690). A straight shot, which is a Line of Sight
weapon with no special subtype or a Guided one, leaves the QueryWeapon
piece on a line to its aim point and keeps that line
(legacy:246851-246925, legacy:246927-247010). Lightning flies the same
way (legacy:247147-247160). A flame walks its whole range through the
test on its first frame to find where it stops (legacy:247535-247575).
The mind control, stone and frost rays are straight shots. Remote
Effect spells and Wandering shots run no test on the way
(legacy:249842-249950, legacy:249099-249160).

A shot whose target dies keeps flying. Nothing in the original ends
it early.

There is no line of fire check before a shot. The fire gate looks at
range, reload, mana and facing (legacy:249328-249420). A unit keeps
shooting into a hill.

## What the engine does now

`src/game/shot_path.c` is the test above, integer only and on the
float guard's list. The unit layer answers for bodies through two
callbacks. The occupancy layer already records one unit per 16 px
cell, for buildings and for movers, which is what the second step
reads. Flyers are gathered once a tick before anything fires.

A shot still moves a whole tick at a time, exactly as before, and the
test then walks that move in steps of 16 px or less. On each step the
intended target is met first, then the cell. The target is met within
24 px in three dimensions, counting from its body between the model's
bottom and top, so a rock thrown high passes over a unit that walked in
under its arc. Where the target is met, the cells on the way there are
tested first, no more than 8 px apart, so a unit right behind a one
cell wall or crest is not struck through it. The first thing met puts
the shot where it happened. Only a shot
fired at a point on the ground has a fuse at its aim point. One whose
target dies flies on past where it was aimed to whatever it meets, and
a splash does not go off over the place a dead flyer was.

Straight shots fly a line from the muzzle to the aim point instead of
hugging the ground, and every flying shot now leaves the QueryWeapon
piece. Lightning and flame trace their ray when they fire and the beam
ends where it stopped, at the height it reached. The drawing reads that
height. A beam that finds the shot pool full draws nothing, and its ray
is still traced. It hits its target only when nothing stands in the
way, an enemy in the way takes the hit instead, and whatever stops it
counts toward letting the target go as a drawn beam's would.

A unit's model top and bottom are read from its 3DO file the first
time a shot asks, the same vertices the drawing bakes, and kept on the
unit definition. The simulation never reads them from a bake, so a
machine that never draws a unit, or a loaded game whose units have not
been drawn yet, gets the same numbers. A unit with no model, or a model
with no drawn point, counts 0 to 32 px. Since the models now decide
shots, the data fingerprint carries `objects3d/*.3do` in its units group
(docs/MULTIPLAYER.md, "The data fingerprint").

The muzzle and the aim point are asked of the unit's script as it runs,
QueryWeapon and SweetSpot, and a query borrows a free script slot. It
hands the slot back exactly as it found it. The lowest free slot is
often a finished AimWeapon whose answer the aim code has not read yet,
and a query that left it cleared turned a hold into leave to fire. The
pose that turns a piece into a place is composed in memory the unit
layer holds for the purpose, so an allocation that fails on one machine
cannot move a shot there.

The feature heights come from a grid of one entry per cell, derived
from the feature list. A feature placed or removed updates only its own
cells, and anything else that rewrites the list has the grid built again
before the next shot. The height is cut to the byte the original keeps
(legacy:127053). So a feature authored 300 tall, as the largest trees
and rocks are, stops shots at 44 over its floor, and one authored 200 at
200.

What a struck unit takes: a shot with an areaofeffect of 17 or more
bursts where it stopped, and one under 17 hits the unit that stopped it,
target or not, and nothing else (legacy:245029-245031). An ally that
stops a shot takes nothing (D-024). A shot that ends on a feature, the
sea or the ground does the harm of its burst and nothing else. A shot
fired at a point on the ground keeps the burst it always had.

A burst reaches half the areaofeffect (legacy:245089). Each unit is
measured from the burst to the nearest point of its model's box, the
box set unturned on the unit where it stands, in whole pixels
(legacy:245164-245209). Inside the radius a unit takes the whole hit at
0 and edge + (1 - edge) x (d / r - 1)^2 of it further out, where edge is
the weapon's edgeeffectiveness (legacy:245213-245217). A catapult's ball,
1250 with an edge of 0.1 over an areaofeffect of 100, gives 845 at 10 px,
406 at 25 and 125 at 49, and nothing from 50.

## A unit that keeps hitting the hill

The original has no check before a shot, and neither does the engine,
but a shot that stops now matters. A unit counts the shots in a row at
a target it picked for itself that stop short of it, on the ground, a
feature, the sea, or a unit or wall of a player who is not its enemy.
A shot that ends past its aim point, the way a miss at a moving target
does, is not counted, and one that strikes an enemy starts the count
again. At three the unit lets the target go and passes it over for ten
seconds, in its own search and in what `Units_CanAttackTarget` tells
the computer player and a mission script (D-025). The skip holds only
while neither the unit nor the target has moved more than 32 px from
where it stood, and a hit from the target it passed over ends it, so a
unit always answers that target's fire. A player's own attack order
keeps firing, as the original's does, and so does a mission script's,
whoever owns the unit. The count, the target it belongs to, the target
passed over and where both stood are in the state hash and the save.

A unit that cannot move lets go of a target it picked as soon as that
target is out of its reach or inside its minrange, and its search takes
only what its weapon reaches past its minrange. A tower could hold a
target it had no way to shoot for as long as that target lived.

## What is still left out

- The body test stops at the model's span. The original follows it
  with a test against the model's pieces (legacy:236960-236975).
- The original leads a moving target by its speed over the flight time
  (legacy:234043-234055). The engine aims where the target is.
- Units neither hold fire nor move for a clear line before they shoot,
  which is the original's behaviour too.
- Dropped ordnance, the egg bombs of the flyers, still hugs the ground
  on its way down and meets nothing. The original drops it as a shell
  of its own that falls from the carrier (legacy:246794), which the
  engine does not fly yet.
- Remote Effect spells (17 of them, Earthquake, Hail Shower, Firestorm,
  Ring of Fire, Tsunami and the Wind and Fire Waves among them) and
  Wandering shots (the Tornado and the vortexes) still land behind
  ridges and walls, as in the original. So does Individual Mind
  Control, which is `unitsonly`.
- The flyer test walks every flyer on every step of a shot. Flyers are
  few, and the unit grid would serve if they are not.

## Determinism

The cell test is integer math. Heights compare through `floorf`, which
is exact on every platform. Substep positions are integer divisions.
Flyers are listed in slot order. The feature grid is derived from the
feature list, so it is neither saved nor hashed. The shot's pass flags
are hashed and saved, in version 2 of the shot record, where a version 1
shot reads back as one that is never stopped, which is how every shot
flew then. A unit's model span is a pure function of its 3DO file, which
the fingerprint covers. The muzzle and the aim point read the model's
pieces from whichever colour of it has been baked, since every colour
holds the same pieces, and every spawn bakes one. A unit given to
another seat can have no bake in its new colour on a machine that has
not drawn it, and before this the muzzle fell back to the ground there.
The unit record is version 4. The D-025 count, the target passed over
and where both stood sit at its end, from byte 624, after the 137 bytes
of a formation move that version 3 added, a 9 byte header and 8 legs of
16. A version 3 record reads back passing nothing over. A build that
writes version 3 refuses a version 4 save.

`TAK_ENGINE_BUILD_ID` is 5, one past the formation moves' 4. The pinned
simulation probe moved from `7c26822d` to `553e9b56`, because the
probe's archers now aim at half their targets' height and meet the
units in their way.
`test_line_of_fire` pins a second answer, a volley with Line of Sight
on, a computer's shooters, armed targets, a ridge, walls and a rock,
so every platform in CI has to agree on the fog and the letting go as
well.

## Tests

`test_line_of_fire` builds a synthetic world with no game data: a
ridge, a rock, a wall, a sea and a keep, drawn into the heightmap, the
feature list and the occupancy layer. It fires an arrow, a bolt, a
fireball, lightning, a flame and a lobbed stone across them and checks
where each one ended and who took the damage. A splashing bolt whose
target dies flies past its aim over a bystander who takes nothing, and
lightning with the shot pool full hits a target in the open and not
one behind a rock, strikes an enemy wall in its way, and counts a rock
toward letting its target go. Lightning and a bolt at a post right
behind a one cell crest end on the crest, and a lobbed rock passes over
a unit that walked in under it. A quick bolt behind a ridge lets its
target go after three shots and takes one it can hit, keeps firing
under the player's own order and a mission script's, and lets a
computer's order go. It answers the target it passed over when that
target hits it, and takes it again once either has moved 40 px. `test_cob_vm` checks that a
query run between an AimWeapon's end and its reading leaves the answer
where it was. `test_hpi_vfs` checks that a changed model moves the
units group, and that a carriage return in a model counts.

With game data, `the_castle_wall_takes_the_keeps_cannonballs` puts a
stronghold inside the castle on Two Castles and a knight outside it,
below the castle's wall. The cannonballs burst on the wall and the
knight is not hurt. Several older screen cases stood their targets on
that same side of the wall, where the shots now stop, and they now
stand them west of the shooter on the castle's floor.
