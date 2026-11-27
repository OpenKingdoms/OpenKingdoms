# Economy and building

How the original pays for work and puts things up: the treasury and the
mana a kill pays, a frame and the builders on it, a frame left alone,
where a building may go and how a build order reaches it, the Zhon
summons, and how a factory opens its yard and trains. Frames are the
original's, 30 to a second. The build buttons and the runs of a
factory's queue are in [ui.md](ui.md), and what the computer player
chooses to build is in [2026-09-04-legacy-ai.md](2026-09-04-legacy-ai.md).

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## The treasury

Each player's mana is one pool. It keeps the mana in hand, a maximum, a
supply fraction from 0 to 1 that scales all build work this frame, an
income accumulator, a demand accumulator, a flag that suppresses income
while it is set, and a lifetime total kept in double precision.

The economy tick runs every frame (legacy:235930-235985).

- Income is the sum of every unit's `mogriumincome` over 30, and
  nothing comes in while the income flag is set.
- The maximum is the sum of `mogriumstorage`, and never less than 1.0.
- The supply fraction is the mana in hand over this frame's demand,
  capped at 1. Demand is summed over every unit that is building, as
  its `workertime` times its target's `buildcost` over its target's
  `buildtime`, and taken over 30 for one frame.

A treasury that cannot cover what is asked of it pays every consumer
the same fraction, worked out once a frame (legacy:235959-235977). Work
on a starved frame goes on at that fraction of its rate and never
stops. The fraction measures the pool against what is asked of it, not
how full the pool is, and the computer player's note calls it build
efficiency. A factory's training order and a walking builder's order
drop some products while it is low, for every player, as "Unit,
factory and builder think" in
[2026-09-04-legacy-ai.md](2026-09-04-legacy-ai.md) sets out.

When a unit is killed, its killer's player gains the unit's
`mogriumbounty`. The key is read as a float (legacy:162910) and not
divided by 30. The bounty goes into the mana in hand and the lifetime
total (legacy:227316-227319), and it passes over the maximum, where the
usual way mana is added stops at it (legacy:8661). No bounty is paid
for a unit of your own or of your team, the same guards the experience
award has.

## Building a frame

A unit going up is a frame. The build fraction it carries is the part
still to do, and script port 0x11 returns 1 minus it (legacy:223278).

One routine, `Building_TickConstruction`, both builds and decays
(legacy:39451-39527). Each builder calls it on its frame every frame,
and builders share nothing but the treasury (legacy:39451-39505). The
rate is the supply fraction times a speed multiplier times the inverse
of the frame's `buildtime`, and every builder passes its `workertime`
over 30 as the multiplier. The spend is the rate times the frame's
`buildcost`, and the demand accumulator is kept up as it goes. So each
builder adds its `workertime` over the frame's `buildtime` every second
and pays the same share of `buildcost`. On completion the hit points
are set to `maxdamage` and the unit's position is made final.

Several builders on one frame add up. The rate is the sum of their
workertimes, with no cap and no falling off. A frame takes its
`buildtime` over the summed `workertime` in seconds and costs its
`buildcost` however many work on it, so the drain on the treasury rises
with the number of builders. An Aramon Barracks (`buildcost` 1955,
`buildtime` 600) takes one builder of `workertime` 10 sixty seconds at
32.6 mana a second, and Elsin with a Mage Builder thirty seconds at
65.2 mana a second, 1955 mana either way. That is the manual's "extra
speed comes at an extra price in resources".

## A frame left alone

Decay is the same routine with a negative multiplier. The part still to
do grows, and the mana comes back in full proportion, into the mana in
hand, the income accumulator and the lifetime total. At a remaining
fraction of 1.0 or more the unit is destroyed (mission code 0xb).

The decay call runs the routine with the frame as its own builder and a
multiplier of minus 0.5 for each frame (legacy:39531-39534). Its only
caller is legacy:9643, in the abandoned frame's own order
(legacy:9629-9657). An interrupted frame stays where it stands, waits
330 frames and then gives back half a build unit a frame until it is
gone.

A frame's silhouette is drawn with an alpha that follows how far the
build has got (legacy:197470-197489). At a few percent it draws next to
nothing, so an interrupted lodestone can look like an empty site for
those eleven seconds.

The engine departs here, see M-009.

## Repair

A builder mending a unit pays the supply fraction times the rate times
the target's `buildcost` over the target's `buildtime`
(legacy:39540-39576). Hit points short of a whole one build up in the
target's 8-bit fraction, and the heal raises event 0xc. Self heal and
how fast a unit mends are in
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

## Placing a building

The cursor's ghost snaps to the 16 px cell lattice, centred on the
footprint and rounded to the nearest cell. It takes the pointer's world
position, less 8 px for each cell of the footprint, plus 8, over 16
(legacy:39263-39305). The order snaps the same way, to the cell times 16
plus 8 for each cell of the footprint (legacy:39183-39190, 12028-12033).
A def with a movement class takes its footprint from the class
(legacy:163193-163195), so an orc snaps as 3 by 3 and a giant as 4 by 4,
where the harpy keeps the 2 by 2 its own file gives. A placement valid
flag says whether the snapped cell can be built on. There is one ghost
and no preview of more. Every building is placed at the one fixed
heading, facing south. The engine lets a building be placed turned, see
D-022.

The site search (legacy:219074) judges each cell of the footprint. A
cell fails when its lowest point lies more than `maxwaterdepth` under
the water, or its highest point less than `minwaterdepth` under it
(legacy:219149-219157), and the slope rule follows, as
[pathing.md](pathing.md) sets out. Both depths come from the def's
resolved move class, not from its own keys (legacy:163199-163202). A def
with no `movementclass` gets a scratch class made from its own FBI keys
(legacy:163182-163192), read the way a class is, defaults and all
(legacy:187426-187467). A class starts from `MaxWaterDepth` 10000,
`MinWaterDepth` -10000 and 255 for every slope (legacy:187363-187375).
The search also refuses a cell a unit holds (legacy:219134-219147).

In the shipped data the naval yards are land structures. `vercastl.fbi`
and `verasy.fbi` give `maxwaterdepth = 0` and no `minwaterdepth`, so a
yard stands on land at the shore and its dock's build spot reaches the
water. Ships carry their water in `movementclass` WATER2 to WATER5,
whose `MinWaterDepth` is 13 to 15 in `moveinfo.tdf`. WATER2 to WATER5,
HOVER2 and HOVER3 give no `MaxWaterDepth`, so they keep the class
default, and hovercraft cross deep water. VERASY's build list is all
ships, and VERCASTL's mixes GROUND2 and GROUND3, HOVER2 and one flyer.
Two walking builders place ships: ARABUILD builds ARAWAR and ZONTRAIN
builds ZONKRAK, both WATER4, so their sites need water.

With Shift held, the interface in build mode, the left button down and
the placement valid, a drag queues one build order for each cell it
newly snaps to (legacy:243846-243868). There are no doubles, since
placing one clears the valid flag for its cell.

A placing click gives the order to every selected builder that can
build the def, a limited one only for a def on its own list
(legacy:39198-39256), as a build order for a walking builder or a flying
one. The build menu shows only while one unit is selected
(legacy:153423-153437), and a change of selection ends the placement
(legacy:242517-242527), so in the original that is one builder. The
engine shows the first selected unit's menu for a group and sends each
builder of it whose own list holds the def, see M-013.

## A build order

A build order has no frame at first. The builder walks until its route
ends within its `builddistance` of the site, and the order is dropped
with UNITCHATTER_CANNOTREACHCONSTRUCT when it cannot get that near
(legacy:12063-12070). Only then does it test the site (legacy:12088),
create the frame (legacy:12142), hand it to the builder and start work
(legacy:12147-12171). Nothing goes up before the builder arrives, and
only the builder holding the order can start it. A site nobody has
begun holds no cell, so nobody is kept off it. Whoever gets there first
raises it, and a later builder finds it blocked and its order ends
(legacy:12096-12116).

The test at the site comes in two (legacy:12088-12124). The first
counts every unit on the site (legacy:12088), and when it fails a second
lets units that can move through (legacy:12096). When only the second
passes, the builder looks again 10 frames later, says it is waiting at
the sixth look and gives up after 30 looks, about 10 seconds
(legacy:12097-12116). When even the second fails, something that will
not move holds the site and the order ends at once (legacy:12098). The
unit limit ends it at once too (legacy:12162-12172).

A flyer in the air puts no stamp on the ground cells, and both tests
read only those stamps (legacy:218217-218250, 218800, 219134). So
neither counts a flyer in the air or one that never lands, and a
summons or a building goes up under a Harpy overhead without waiting
for it. A flyer that has landed holds its cells like any walker.

The engine waits and gives up in silence, see M-015. A builder of the
same player sent to the same site joins the frame in the engine once
another raises it, where the original's reports the target blocked, see
M-013.

## Helping build

Help is an order of its own on a frame that exists, so a site with no
frame has nothing to help. The original asks one question before a unit
joins the work on a frame (legacy:233556-233574), and all of these must
hold.

- The frame exists, is not the helper, is alive and is not dying.
- The frame is still under construction.
- The frame belongs to the helper's own player. The test compares the
  two players, so an ally's frame does not count (legacy:233564).
- The helper has the FBI `builder` line (legacy:162955) and can move.
  Factories and castles never help.
- When the helper has the FBI `builderlimited` line (read at
  legacy:163099-163100), the frame's type is on the helper's own build
  list (legacy:233569-233572, the list test at legacy:233192-233210).

The same question is asked again when the order starts
(legacy:12392-12412), and it decides the cursor.

Every mobile builder that is not a monarch carries `builderlimited` in
the shipped data, in the base game and in Iron Plague, and the monarchs
do not. So a monarch helps any frame of its own player, a building it
could not start itself and a soldier in a barracks among them, while a
Mage Builder helps only the types on its own list.

With the left-click interface the cursor over your own frame is the
animated hammer, `cursorrepair`, when any selected unit passes the test
(legacy:186541-186545). A frame is never selected (legacy:186553), so
with nobody to help it the cursor is the ground's, a move for a
selection that can walk and the plain pointer otherwise
(legacy:186603). The manual (p. 59) describes the same thing: "move the
cursor over the sparkling image of the under-construction Garrison.
You'll see an animated hammer. Left-click".

A click orders each selected unit for itself (legacy:238458-238660). A
unit that passes the test joins the work, and a walker that does not
goes to the ground under the click. Shift queues the order behind the
ones in hand. An enemy's frame can be attacked.

### A guard helps

A guard runs through these checks in order whenever it looks
(legacy:14035-14255). When the unit it guards has just been hit by an
enemy unit it can fire on, it attacks that unit, whatever its own
stance, with a plain attack that has no leash (legacy:181888-181915).
When it cannot attack what hit the unit it guards, it turns its weapons
on it from where it stands (legacy:14053-14098). Otherwise, when it
walks, it mends the unit it guards if that is damaged and standing
still (legacy:233526-233553), or helps build it if it is a frame, and
then it joins the build, the help or the repair the guarded unit is at,
when the tests above let it. Each of these goes ahead of the guard,
which resumes once it is done. With nothing to do it walks up to within
the longer side of the other unit's footprint, five cells and its own
footprint of it, in cells of 16 px (legacy:14011-14022).

The unit record keeps the last enemy unit that hit a unit and the tick
it did, for its guards (legacy:233765-233768), and the original wakes a
guard with an event on that hit. The engine has the guard answer a hit
from that tick or the one before. How a guard follows the orders of the
unit it guards is in
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

## A summons without end

Zhon makes its army with builders that walk. A Zhon player holds Ctrl
on a builder's button for a unit, clicks one spot, and the builder
summons that unit there over and over until told to stop.

A left click on a walking builder's button arms placement, as it does
for any building (legacy:150058-150087). With Ctrl held and a product
whose `bmcode` is 1, the click also sets a repeat flag. Without Ctrl,
or for any other product, the flag is cleared
(legacy:150077-150084, 150077-150086). Shift does nothing at the
button. `bmcode` is read at legacy:162924, and the loader keeps it at 1
only for a def with a `turnrate` and a `brakerate`, turning any other
value to 0 (legacy:162855, 162865, 163608-163617). So the repeat is for
units, the Zhon harpy and every other flyer among them, and never for a
building.

The order the click gives counts ten million with the repeat flag set,
and one without it (legacy:39237). Shift and Ctrl held at the click
normally queue the order or change the one in hand. With the repeat
flag set Ctrl drops both, so a player who keeps Ctrl down through both
clicks replaces what the builder was doing (legacy:39177-39180). Shift
alone still queues it. A building queued with Shift is never given to a
builder that holds an order of ten million, since it would never be
reached (legacy:39208-39219), and neither is any other order given with
Shift alone (legacy:238562-238574). The placement ends after this one
click even with Shift held (legacy:242531-242540).

The order meets its site as any build order does. When the unit is
finished, an order of any count but ten million ends there. One of ten
million instead sends the new unit off the spot and starts again a
frame later on the same spot (legacy:12272-12315). The unit steps aside
the way a transport's cargo does once it is set down
(legacy:14601-14629). Its radius is 16 px times one plus three rolls of
0 to 2, and four more for a builder that flies, so 16 to 112 px, or 80
to 176 px. The unit goes to a spot between that radius plus 16 px for
each cell of its footprint and three times as far (legacy:13858-13868).

The order never ends by itself. A right click on the button takes every
build order of that def (legacy:150087-150093, 181838-181866), and Stop
or any new order without Shift ends it too. The button reads `+++`
while the builder holds such an order (legacy:149922-149929).

## Placing a summons

A summons is placed the way a building is, on the lattice above. The
loader gives a def with a `bmcode` a yard of `0x29` on every cell and
never reads its `YardMap` (legacy:163206-163208, 163272-163292). The
cursor's test looks for a unit on a cell only where the yard byte has
bit `0x2` or `0x4` (legacy:218800-218811), and `0x29` has neither. Its
bit `0x1` does meet the cells a building's yard marks when the building
is placed (legacy:218185-218191, 218797). So a summons is accepted on
top of standing units and on an earlier frame at the same spot, and
refused on a building, on ground it cannot take, on a blocking feature
and at the map's edge.

The order itself never lets two units overlap, through the two tests
under "A build order". A plain summons is not moved off its spot when
it is done (legacy:12555-12559), so a second plain summons on the same
spot waits for the first unit to leave and gives up if it stays. The
Ctrl run, which steps each unit off before the next, is how an army
comes out of one spot.

The engine tests a summons for slope against the def's own `maxslope`,
where the original takes the slope from the move class.

## Factories

### The yard opens before it trains

A factory's build order has steps (legacy:9316-9340). The first sets
the unit's activation bit, whose rising edge runs the script's
`Activate` (legacy:236402). The next waits until the script sets
INBUILDSTANCE (legacy:179347-179358). Only then does it ask
`QueryBuildInfo` and make the product. When the count runs out it
clears the bit, which runs `Deactivate` (legacy:9323-9328, 236405).
Between two runs of different units with more on the queue it clears
the bit and sets it again, so the doors shut and open. The engine
leaves the bit set there.

All eight factories in the shipped data script this much the same way.
`Activate` goes through `RequestState` to `Go`, which plays the doors,
calls `OpenYard` and sets INBUILDSTANCE. `Deactivate` reaches `Stop`,
which clears INBUILDSTANCE and calls `CloseYard`, the Temple in the
other order. `OpenYard` and `CloseYard` set YARD_OPEN and try again
every 1.5 seconds while the set is refused. A run's first product goes
up after the doors, about 1.4 seconds after the order for the Abyss
from a shut yard, and up to about six seconds from one still shutting.

A yardmap `c` cell blocks only while the yard is shut
(legacy:217987-217988). A set of YARD_OPEN is refused while a live unit
holds a cell that would then block (legacy:218984-219059). So a pad is
open ground from before the product goes up until the last unit has
left it, the unit there holds its own cell, and a shot in that cell
meets that unit (legacy:245414-245424). A unit standing on the yard
keeps it from shutting, and a factory told to train meanwhile waits,
since its script is still in `Stop`. BUGGER_OFF, which the scripts
raise meanwhile, is only ever read back by a script (legacy:223287), so
the unit stays until someone moves it. The yardmap and the yard's state
are in [pathing.md](pathing.md).

The engine sends the side's idle units off the yard's cells once
something waits to train, and for ships that is part of M-012.

### The build spot

Training is stage 2 of the construction order (legacy:9342-9456). Each
time a product starts:

1. The factory's `QueryBuildInfo` runs at once (legacy:9347-9350),
   through the helper that runs the weapon scripts (legacy:306129,
   which dispatches to legacy:306142-306208). The helper takes a
   thread, seeds its four deepest stack slots with four arguments
   (legacy:306156-306193), runs it to the end and reads the four slots
   back. Argument 0 is seeded with -1 (legacy:9348), and the script
   writes the build spot's piece index into it (`piecenum = N`). A
   script that never writes it, or a factory with no `QueryBuildInfo`,
   leaves -1, which means no pad, and the offset is (0, 0, 0), the
   factory's centre. Piece 0 is a legal answer.
2. The piece's offset is gathered up the parent chain
   (legacy:185790-185836), from each node to its parent, adding the
   static offset and any animated translation and turning by the
   parent's angles. Only the parents' angles count
   (legacy:185805-185820), so a piece's own turn never moves its own
   origin. The top level adds the unit's heading and negates z, and the
   offset is added to the unit's position (legacy:185840-185859). The
   result is kept on the order as the spot (legacy:9351-9362).
3. The placement search runs with the product's def, taken from the
   index the order records, and the product's own footprint, centred on
   the spot (legacy:9363-9373). So a ship needs water at the dock, and
   a yard whose spot is dry never starts one and keeps trying. When the
   search fails the order waits a random 7 to 21 frames, stays in stage
   2 and asks `QueryBuildInfo` again on the next try
   (legacy:9374-9382). At the unit limit the factory says so and waits
   the same way (legacy:9467-9483).
4. The product is created at the spot (legacy:9396-9407). Its
   `getbuilt` script runs and it is given its walk out
   (legacy:9429-9430), and the factory starts its build animation
   (legacy:9435-9446).

The shipped scripts answer like this.

- `scripts/vercastl.cob`'s `QueryBuildInfo` (pc 2273-2280) sets argument
  0 to 1 and returns, and piece 1 is `emitbuild`. In the 3DO `EmitBuild`
  sits at (-51181, 0, -8557023), which at the fixed south heading
  lands about 180 px south of the centre, just past the doors. The
  footprint is 20 cells deep, so its half is 160 px, and `Doorleft` is
  at about -99 px in z.
- `scripts/verasy.cob`'s `QueryBuildInfo` (pc 2190-2218) flips static
  variable 1, turns piece 1 at once about y to plus or minus 16384 and
  sets argument 0 to 1. Piece 1 is `emitbuild`, a child of `buildrot`
  at (0, 0, -7045120), about 148 px along the facing. The turn is of
  `emitbuild` itself and not of its parent `buildrot` (piece 19), so it
  never moves the spot. `buildrot` is aimed once in `Create` from unit
  value 27. The dock's spot does not alternate, and one query each time
  a product starts is all there is.
- `scripts/zontrain.cob` has no `QueryBuildInfo`, so its spot is the
  unit's centre.

### Build queues and rally points

A unit holds two order lists, and the second is run on its own
(legacy:182203-182245). A new order that is not queued clears the first
list, apart from the orders flagged to survive, and leaves the second
alone (legacy:181670-181678, 180761-180795). A factory's rally is set
with a Move while it trains, so its build orders are among those a new
order leaves. Stop is issued the same way as the rest
(legacy:151390-151401), so Stop on a factory clears its rally and
standing orders and the training goes on.

The original hands a finished product its factory's orders in the
factory's own order handler (legacy:9512-9526), so the product goes to
the rally whoever laid the last brick.

A left click on the ground sets the rally. The original takes a click
as one on a unit only where it falls inside the box of the unit's model
seen from above, turned with the unit and drawn where it stands
(legacy:237815-237922), so a click beside the units standing at the old
rally sets the new one.

A factory still going up cannot be selected, so it takes no order and
no queue. The engine lets a front end queue units in one, see D-027.
