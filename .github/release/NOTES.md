OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

The battlefield now breaks and burns the way the original's does. Blasts
fell trees, knock down walls and cave in buildings, fire catches in the
woods and the wind blows. A new room option, Remastered Battlefield, takes
it further. Flyers spread out instead of piling up, builders help each
other, Zhon summons are much easier to place, and mind control can miss
again. Everyone in a multiplayer room has to be on this version, so
refresh the page after the update.

## Scenery that breaks and burns

Blasts now wound the scenery they reach, as in the original. A tree hit
hard enough plays its death and leaves a stump, a wall drops a stage and
then lies as rubble, and buildings cave in. Each stage blocks the ground
its file says it does, and units find their way round what is left.
Weapons that only hurt units leave scenery alone. (#384)

Fire weapons set trees and other scenery that can burn alight. A burning
tree plays its flames and leaves a burnt stump, and its spark can light
the scenery near it and downwind. With the original's timings most fires
burn out before their spark, so a fire stops where the flame reached, as
it did in 1999. (#384)

The wind blows now. It changes speed and turns a little every few seconds
within the limits the map sets, and flags and sails follow it. (#384)

## Splash and death blasts

A blast reaches half of its weapon's area of effect, with the original's
curved falloff toward the edge. Until now every splash weapon reached
twice as far as in the original and hit harder at range. A small blast
that strikes a unit hits that unit alone. (#382)

Units whose file names a death weapon burst with it as their death ends.
The Grenadier, the Kamikaze Rat, the Dirigible, the Bomb Sprinkler, the
Fire Wagon and the Shock Trooper have one, and the Crusades units add the
Giant Orm. The blast hurts friend and foe alike and breaks scenery, and
what it kills counts for the dead unit's owner. A unit that was never
finished, or one a builder takes apart, never bursts. (#388)

## Remastered Battlefield

A new option, Remastered Battlefield, sits under the original's rules on
the skirmish screen and in the battle room. It is off by default, and the
game list marks a room that plays it. (#385)

With it on, the battlefield works like this:

- Rocks, ruins, spires and grass break too, and leave nothing behind, so
  a path opens. Lodestones and sacred sites never break.
- Spells that only hurt units in the original reach scenery like any
  other blast.
- Fire spreads through the woods of the shipped maps. A burning tree
  throws several sparks before it burns out, they reach further, and the
  wind drives the fire on. A fire usually crosses a wood more slowly than
  infantry walks, and even a wood of ten thousand trees ablaze keeps the
  game running smoothly. (#389, #399, #408)
- Burning scenery hurts units standing in it, 25 damage every half
  second.
- The rubble of walls and buildings blocks the way until a builder sweeps
  it, which pays its mana as clearing does.
- Computer players sweep rubble that has their units stuck and walk their
  units out of fire. (#391)

## Burning buildings

Damaged buildings show the flames and smoke their scripts call for, as in
the original. The flames follow the building's model, and the smoke stops
once the building is repaired. This is the first new feature from outside
the project. Thank you, tgilgs! (#369)

## Flyers

Flyers that share space in the air step apart, as the original's air
traffic does. A flock sent to one point spreads out round it instead of
stacking, and lands with room between its flyers. (#396)

Flyers with a hover attack, such as the Harpy, hold their own bearing
round their target at the distance their file gives and fire as they move.
Against twelve Harpies on one target, one Water Ball now takes at most
three where it took ten. (#412, #413)

Flyers land only on clear, dry ground. A flyer stopped over the sea flies
on or circles until it finds land, and over water it keeps its height
above the surface instead of the sea floor, so dragons are no longer drawn
under the waves. (#397, #409)

A Harpy in the air no longer stands on a building site, so Harpies
summoned without end keep coming. (#403, #404)

## Builders and summons

A second builder or a monarch can help build a frame another builder
started, and the work adds up, so two builders finish in half the time for
the same mana. As in the original, a monarch can help with anything of
yours, other builders only with what they could build themselves, and
nobody helps an ally's frame. The hammer cursor shows only when a selected
unit can help. (#392)

Ctrl on a walking builder's button summons that unit without end, as Zhon
players will know from the original. Click one spot and the builder keeps
summoning there, each finished unit stepping off for the next. The button
shows +++, and a right click on it ends the summons. (#393)

A summons can be placed over units that can walk away. The ghost turns
green, and the summons waits for them to leave its spot before it starts.
(#395)

## Mind control

Mind control shots fly by the original's rules again. A shot leads a
walking target, misses a unit that steps out of its way, ends at its range
and dies with its caster. Mind control can miss as it did in the original
instead of taking nearly every unit it is aimed at. A hit still takes a
recruit four times in five. (#402)

## Wandering spells

The Weather Witch's Tornado works. The Tornado, the gods' Fire and Water
Vortexes and the Hurricane now wander away from the caster as in the
original, turning as they go and hurting whatever they pass on every frame
of their run. A Tornado fells the trees in its path. (#411)

## Minimap

The minimap's buttons work as in the original. Hold the right button on it
to look around, and drag to follow the pointer, whatever you have
selected. With your units selected, a left click on the minimap gives the
order a click on the field would give there. They move to open ground,
attack an enemy standing there, or carry out the command you have armed.
With nothing of yours selected, the left button still looks. (#394, #406)

## 3D view

The 3D view draws scenery dying and burning, with its flames, as the
classic view does. (#390)

## Also

Running the test suite no longer touches your own options. Options and
saved games live in your user folder, and setting `TAK_CONFIG_DIR` keeps
them somewhere else. (#410)

The engine keeps a record of each battle for a front end's end screen, and
a front end can hear each blast, each piece a dying unit throws and what
happens to each piece of scenery, to draw them its own way. (#380, #383,
#386, #387)

## Saved games

Saves from older versions that hold a unit with a death weapon, such as a
Grenadier or a Kamikaze Rat, do not load in this version.

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
