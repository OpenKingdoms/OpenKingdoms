# Release notes draft

Battles now play much closer to the original. Arrows and spells stop on
hills, walls and the units in their way, and nobody targets what their
side cannot see. Swordsmen close in and beat archers the way they did in
1999. Factories keep training through any order, rally points take, and
the build buttons and Shift queues work as the manual describes. Before a
match you can claim your start position and search the map list. Everyone
in a multiplayer room has to be on this version, so refresh the page after
the update.

## Shots and targeting

Arrows, bolts, fireballs, lightning and flame now stop on the first hill,
wall, tall tree or rock, stretch of sea or enemy unit in their path, as in
the original. An ally's unit or wall stops a shot but takes no harm from
it, and your own walls never stop your own shots. Catapults still lob over
walls, and the area spells, the wandering spells, Individual Mind Control
and dropped bombs still pass over the ground. A unit that picked a target
for itself and keeps hitting something in the way lets it go for a while,
but an attack you order yourself keeps firing. (#328)

A unit only picks targets its side can see. A unit that is hit still fires
back at a shooter within its reach, as in the original. A watch tower no
longer holds on to a shooter it can neither hit nor reach. (#328)

No shot or splash harms the unit that fired it. A flyer that has taken off
holds its fire until it reaches its cruising height. (#329)

## Melee and archers

Six combat rules now follow the original, and archers are no longer too
strong. A swordsman closes all the way to its target, even one standing
behind another, and keeps closing when a route is hard to find. An idle
melee unit takes on any enemy within its weapon's reach of 300 px, not
only what it can see. Archers and crossbowmen pick their targets at random
among the enemies in reach. Each arrow or blow leaves at the point in its
animation where the original released it, so the first arrow lands after
about 1.9 seconds, and a shot whose target dies during the draw is lost.
A caster whose target dies mid cast keeps the mana. The computer's archers
no longer circle round to the side of a target. (#334)

The Use Crusades Units option now works. It loads the Crusades balance
set, so a swordsman has 3000 hit points. A game saved with that option on
an earlier version will not load, since it was played with the standard
set. (#334)

## Factories, orders and selection

The build buttons work as in the original. A click queues one unit, Shift
and click queues five, and Ctrl and click trains that unit without end,
shown as `+++` on the button. A right click takes off the last one queued,
and on a Ctrl run it clears that unit from the queue. (#332)

Factories no longer stop training. Stop, Guard or Patrol on a factory
leaves the queue running, and a queue at the unit limit waits for room
instead of losing a unit. Every unit a factory finishes walks to the rally
point, even when a helping builder finished it. (#332)

Hold Shift to queue orders. Moves, attacks, patrols, guards, heals, builds
and the other unit orders wait behind the one in hand, up to sixteen.
Patrol points given with Shift make one route, and Ctrl with an order
changes the order in hand while keeping the queue behind it. A factory
takes Move and Patrol as standing orders its units carry out. (#332)

A building still going up can no longer be selected, as in the original,
and a click on your own unfinished building is a click on the ground. A
click picks a unit only inside its outline seen from above, so a click
beside a crowd lands on the ground and moves the rally where you meant.
(#332)

## Setting up a game

The map's picture in the skirmish screen shows its start positions. Click
one to claim it, click again to give it back, or drag a player from one to
another. In a multiplayer room this works in the Map and View Map dialogs.
The host can move anyone and players can move themselves. Players without
a claim get the starts left over in seat order, or at random with Random
Start Locations ticked, and a seat behind a closed one now gets the start
the original would give it. (#333)

The map list in the skirmish screen and the room's map chooser has a
search box, with choosers for the number of players, the map's size and
the sort order. Each row shows the map's start count and size. (#333)

## Maps, buildings and larger games

A building can now stand flush against a map mark or a blocking feature
on its east or south side, and is refused on every edge row of the map,
not just two. Ground that dips under the sea at a far corner of a cell now
counts. A transport no longer sets a unit down on a blocking feature.
(#326)

The unit pool holds 8192 units, up from 2000, so several full sides fit on
one map, and the computer player's work no longer grows with the square of
the map's size. (#331)

In the 3D view, the flat floors of keeps and castles now show whole
instead of sinking into the ground. (#324)

A front end can move a group in formation, each unit to its own point with
an optional facing, a shared pace and Shift queueing. The classic view
does not send these orders yet. (#327)

## The original's view at any screen size

A battle can now draw one game pixel to one screen pixel at your window's
size, with the sidebar, minimap and bottom strip at the original's size
and in its places, so 1280x600 shows what the original showed at 1280x600.
The Resolution slider on the Visual options page picks this or Fit, the
old view that stretches the 640x480 battle screen over the window. New
players start on the original's view. If you already have saved options
you stay on Fit until you move the slider. In a browser the slider picks
how many screen pixels make one game pixel. The pause screen's objectives
now sit flush left under the chapter title as in the original. (#336)

## Multiplayer compatibility

Everyone in a room must be on the same version. These changes alter how a
battle plays out, so a room hosted on an older or newer version is greyed
in the list with the reason. After an update, refresh the page before you
host or join, and have everyone else in the room do the same. Desktop
players need the matching release.

## For testers

The desktop build takes `--map`, `--seed`, `--los`, `--scout` and
`--fog-dump` to start a fixed skirmish and write out the fog of war for
comparison. (#325) `--mission` starts a campaign mission straight away and
`--scale original|fit` picks the battle's scale. (#336)
