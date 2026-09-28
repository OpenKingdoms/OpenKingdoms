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
intended target is met first, within 24 px as it always was, then the
cell. The first thing met puts the shot where it happened.

Straight shots fly a line from the muzzle to the aim point instead of
hugging the ground, and every flying shot now leaves the QueryWeapon
piece. Lightning and flame trace their ray when they fire and the beam
ends where it stopped, at the height it reached. The drawing reads that
height.

A unit's model top and bottom come from the first bake of its model,
which every spawn makes on every machine, and stay on the unit
definition when the meshes are dropped. A unit with no model counts 32
px tall.

The feature heights come from a grid of one entry per cell, derived
from the feature list and rebuilt when it changes. The height is cut to
the byte the original keeps (legacy:127053). So a feature authored 300
tall, as the largest trees and rocks are, stops shots at 44 over its
floor, and one authored 200 at 200.

What a struck unit takes: a shot with an areaofeffect splashes where it
stopped, and one without hits the unit that stopped it, target or not.
An ally that stops a shot takes nothing (D-024). A shot that ends on a
feature, the sea or the ground does the harm of its splash and nothing
else. A shot fired at a point on the ground keeps the burst it always
had.

## What is still left out

- Shots do not damage features. The original's burst damages a tree
  or a rock it lands in or near (legacy:245240-245300), which needs
  feature hit points and the `featuredead` chain.
- The body test stops at the model's span. The original follows it
  with a test against the model's pieces (legacy:236960-236975).
- The original leads a moving target by its speed over the flight time
  (legacy:234043-234055). The engine aims where the target is.
- The original gives the struck unit the full hit, with no splash, when
  the areaofeffect is under 17 (legacy:245029-245031). The engine
  splashes whenever the areaofeffect is above zero, as before.
- Units neither hold fire nor move for a clear line, which is the
  original's behaviour too.
- Dropped ordnance, the egg bombs of the flyers, still hugs the ground
  on its way down and meets nothing. The original drops it as a shell
  of its own that falls from the carrier (legacy:246794), which the
  engine does not fly yet.

## Determinism

The cell test is integer math. Heights compare through `floorf`, which
is exact on every platform. Substep positions are integer divisions.
Flyers are listed in slot order. The feature grid is derived from the
feature list, so it is neither saved nor hashed. The shot's pass flags
are hashed and saved, in version 2 of the shot record, where a version 1
shot reads back as one that is never stopped, which is how every shot
flew then. The muzzle and the aim point read the model's pieces from
whichever colour of it has been baked, since every colour holds the same
pieces. A unit given to another seat can have no bake in its new colour
on a machine that has not drawn it, and before this the muzzle fell back
to the ground there. `TAK_ENGINE_BUILD_ID` is 4. The pinned simulation
probe moved from `fb0c90af` to `06beb1ec`, because the probe's archers
now aim at half their targets' height and meet the units in their way.

## Tests

`test_line_of_fire` builds a synthetic world with no game data: a
ridge, a rock, a wall, a sea and a keep, drawn into the heightmap, the
feature list and the occupancy layer. It fires an arrow, a bolt, a
fireball, lightning, a flame and a lobbed stone across them and checks
where each one ended and who took the damage.

With game data, `the_castle_wall_takes_the_keeps_cannonballs` puts a
stronghold inside the castle on Two Castles and a knight outside it,
below the castle's wall. The cannonballs burst on the wall and the
knight is not hurt. Several older screen cases stood their targets on
that same side of the wall, where the shots now stop, and they now
stand them west of the shooter on the castle's floor.
