# Pathing and movement

How the original moves its units: the ground a move class may stand
on, the cells a unit holds, yards and gates, the route search, the
steer and the step of a ground unit, ships, and flyers in the air and
coming down. Frames are the original's, 30 to a second.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Sea level

Every depth test, from whether a unit can wade to whether a building
may be sited, compares terrain height with one number, the sea level.
The original takes it from the map: field `0x0C` of the TNT header,
stored once when the map loads and read by everything after that
(legacy:224848, legacy:224912). The only other thing that writes it is
a console command for testing terrain (legacy:36528).

The original never reads the `waterheight` key of a kingdom's entry in
`sidedata.tdf`, which lists 40 for Aramon and 58 for the other three
worlds. It looks like a default for authoring a new map of that world.

For 220 of the 236 maps a stock install offers, the map's own value is
the one `sidedata.tdf` gives its world. Sixteen Iron Plague maps set
another:

| Map | World | World's value | Map's sea level |
| --- | --- | --- | --- |
| Alkhest Quadrille | Taros | 58 | 59 |
| Black Heart Jungle | Zhon | 58 | 54 |
| Crusader's Keep | Creon | 40 | 54 |
| Haunted Waterworks | Taros | 58 | 56 |
| Islands of the Mer Warrior | Veruna | 58 | 50 |
| Isle of Palms | Veruna | 58 | 54 |
| Lake Cuhmoniwanakilya | Aramon | 40 | 43 |
| Lost Lake | Aramon | 40 | 54 |
| Moka's Fingers | Veruna | 58 | 54 |
| No Zhonian Is an Island | Zhon | 58 | 56 |
| Nobia's Temple | Zhon | 58 | 56 |
| Rival Hill | Aramon | 40 | 42 |
| Sand River Plain | Veruna | 58 | 56 |
| The Drafis Bridge | Creon | 40 | 78 |
| The Gardens of Atys | Creon | 40 | 63 |
| Ulasem Arena | Zhon | 58 | 42 |

Campaign maps differ more often: 45 of the 74 disagree with their
world. takmission01_mt is an Aramon map with its sea at 55, 15 height
units above the 40 its world lists.

## Ground the map closes

A TNT map carries a layer of 16-bit values, one per 16 px cell, at
header offset `0x14`. Most cells hold `0xFFFF` for nothing, a feature
cell holds an index into the map's feature name table, and `0xFFFC` is
a mark painted on ground no unit may enter.

When the original loads a map it copies that mark into the cell record
(legacy:225023, legacy:128160). Every ground test then reads it the way
it reads a blocking feature. The movement check over a unit's footprint
refuses any cell holding a mark other than a feature index or a
feature's continuation (legacy:219659), and so do both build placement
checks (legacy:219119, legacy:218822). Build yard cells that skip the
feature test skip the mark too. The slope rule is separate: a cell is
too steep when the highest and lowest of its four corner heights
differ by more than the move class allows (legacy:224592-224622).

Angvir's Maze builds its walls out of four height steps, for example
62, 68, 96 and 125, which is 6, 28 and 29 a cell. A swordsman's class
allows a slope of 30, so by slope alone much of each wall face is a
staircase. The map marks 11533 cells of the wall faces, and the
original keeps units off them. Ground the marks enclose is cut off,
such as the wall tops on Angvir's Maze and a walled yard in its middle.

Of the 283 map files in a stock install, skirmish and campaign
together, 149 carry the mark, and 146 have marked cells that a class
with a slope of 30 could otherwise stand on. That total and the 236
offered maps and 74 campaign maps counted under "Sea level" do not
agree, and which count is right is not settled. One skirmish start, the
third on Terror Hamlet, stands on marked ground, and a monarch put
there walks off it. The base game's skirmish maps with such cells:

| Map | Marked cells | Of them, open to a slope of 30 |
| --- | --- | --- |
| Angvir's Maze | 11533 | 6242 |
| Castle | 672 | 357 |
| Fortresses of Laingen | 1672 | 810 |
| Frey River Plain | 197 | 61 |
| Inner Circle | 2538 | 2538 |
| Ladron's Tarn | 5073 | 3346 |
| Lern Forrest Lakes | 68 | 21 |
| Muntil's Ford Guard | 1202 | 263 |
| Per Mare Per Terras | 16 | 3 |
| Riparian Conflict | 108 | 21 |
| Sewers of Elam | 2253 | 1301 |
| Tarosian Plain | 1236 | 717 |
| Two Castles | 3177 | 2972 |
| Ulin's Folly | 215 | 37 |
| Varro Passage | 117 | 15 |

## Move classes

A unit takes its movement keys from the movement class it names, and
from its own file when it names none (legacy:163184-163206). Its
footprint comes from the resolved class (legacy:163193-163195).

Before parsing, a class is seeded with `MaxWaterDepth` 10000,
`MinWaterDepth` -10000 and 255 for every slope (legacy:187361-187377,
within legacy:187359-187380). The shipped water classes leave out
`MaxWaterDepth` and sail on that default.

A class carries `maxslope`, `badslope` (by default `maxslope` shifted
right by one, that is halved), `maxwaterslope`, `badwaterslope` (by
default half of `maxwaterslope`) and the bad maximum and minimum water
depths (legacy:187426-187467). Three clamps follow the parse, the first
two at legacy:187452-187457:

1. When `maxwaterslope` is less than `maxslope`, `maxslope` becomes
   `maxwaterslope`.
2. When `maxslope` is less than `badslope`, `badslope` becomes
   `maxslope`.
3. When `maxwaterslope` is less than `badwaterslope`, `badwaterslope`
   becomes `maxwaterslope`.

A footprint fails the depth test when its lowest cell height is below
sea level less `maxwaterdepth`, or its highest is above sea level less
`minwaterdepth` (legacy:219149-219157). For the slope test the lowest
cell height against sea level picks the limit, `maxwaterslope` under
water and `maxslope` otherwise, and the limit is compared with the
highest height less the lowest (legacy:219158-219165).

## The cells a unit holds

Each map cell records the id of the unit on it in a ground word, and a
second word holds flyers in the air. A structure stamps its footprint
by its yardmap (see "Yards and gates" below). A mobile unit stamps its
footprint into the same records (legacy:217933-217971), and the move
step refuses a cell another unit holds (legacy:219329-219340). The
records take every mover from the start of a battle, so units block
one another before anything is built. A ship holds the cells of its
move class and nothing else (legacy:163193-163195).

A structure that finds one of its footprint cells held by another live
unit skips that cell and marks itself dirty to try again
(legacy:217994-218006, retried from legacy:217924). The cell stays the
unit's until it walks off. That yield is what frees a unit caught on a
building's ground.

## Yards and gates

### The yardmap

The `yardmap` string is parsed into a byte for each footprint cell,
footprint x by footprint z of the resolved move class
(legacy:163207-163293, legacy:163193-163195). A table maps each
character (legacy:163223-163256). A string shorter than the footprint
repeats its last character (legacy:163259-163262), and a mobile unit is
given an all 0x29 map instead (legacy:163272-163292).

| Character | Byte | Blocks |
| --- | --- | --- |
| `.` | 0x00 | never |
| `y` | 0x29 | never, a yard units walk on |
| `Y` | 0x31 | never |
| `c` | 0x2d | while the yard is closed, a gateway cell |
| `C` | 0x35 | while the yard is closed |
| `o` | 0x2f | always |
| `O` | 0x2b | while the yard is open |
| `w` | 0x37 | always |
| `S` | 0x8f | always, and sets bit 31 of a def flag (legacy:163236-163237) |
| `f` | 0x6f | always, a feature cell |

A cell blocks when its byte has bit 0x02 set and the yard is open, or
bit 0x04 set and the yard is closed. The yard's state picks the bit
that is tested (legacy:217987-217988, legacy:218170, legacy:219021).
FBI `gate=1` sets a def flag (legacy:163110-163112), and `onoffable`
sets another (legacy:163023-163024).

ARANGATE sets `gate=1` and `onoffable=1` in its file, has a footprint
of 14 by 4, and its yardmap is `ooooccccccoooo` on each of the four
rows. The six `c` columns are the gateway. TARNGATE and VERNGATE are
gates too, and no gate's file sets `activatewhenbuilt`.

### Stamping the cells

The stamp pass is legacy:217912-218035. For a building
(legacy:217974-218031), each footprint cell whose byte blocks under the
yard's current state takes the unit's id, and any other cell is
cleared only if it still holds the unit's id (legacy:217989-217991). On
a `gate=1` def, every cell that blocks only while the yard is closed,
bit 0x04 set and bit 0x02 clear as in the `c` and `C` bytes, is also
tagged a gate cell (legacy:218012-218018). The move cost query
(legacy:218984-219042) and the build placement search
(legacy:219098-219148) read those ids.

### Opening and closing a yard

A script sets its yard's state through port 18, YARD_OPEN. The set
value dispatcher's port 18 case (legacy:223375-223378) calls the yard
state setter (legacy:219046-219059).

1. The move cost query scans the cells that would block under the new
   state (legacy:219021 picks the bit). If another live unit is stamped
   on one of them the set is refused without a word, and a GET of port
   18 keeps reading the old value.
2. Otherwise the state is written (legacy:219053), a dirty flag is set,
   the footprint is stamped again and the pathfinder is told that
   blocking changed (legacy:219054-219056).

GET of port 18 reads that stored state back (legacy:223284-223285).
Port 19, BUGGER_OFF, is a separate flag kept beside it. SET writes it
(legacy:223379-223381) and GET reads it back (legacy:223286-223287),
and nothing else in the original reads it. It exists only so the gate
script can show that it is retrying.

### The gate script

The shipped `arangate.cob` works the yard like this.

- `Activate` starts `RequestState(0)` and `Deactivate` starts
  `RequestState(1)`.
- `RequestState` is a state machine. While the wanted state differs
  from the current one it loops `Go` and `Stop`, and a call made while
  it runs only overwrites the wanted state.
- `Go` calls `open`, the door animation of 3 s, hides `Door1` and
  `Door2` and calls `OpenYard`.
- `OpenYard` sets port 18 to 1. While a GET of port 18 still reads 0 it
  raises BUGGER_OFF (port 19 set to 1), sleeps 1500 ms and sets port 18
  again. Once port 18 reads 1 it clears BUGGER_OFF. `CloseYard` is the
  mirror. So a refused set is what drives the retry loop, with
  BUGGER_OFF raised for as long as it lasts.
- `Create` waits for GET 17, BUILD_PERCENT_LEFT, to read 0 and then runs
  `InitState`, which leaves the gate closed.

### Opening on approach

Each tick of the AI brain (legacy:19003-19011), about once a second for
each unit (legacy:18990), a unit whose def carries the gate flag runs
the gate update (legacy:15260-15361). It scans the cells of its
footprint and a one cell border round it (legacy:15286-15330). It
wants the gate open while that ring holds a live unit of the same
owner (legacy:15300) that is mobile (legacy:15301) and moving, its
speed not zero (legacy:15302, want open at legacy:15302-15305). It
also wants it open while such a unit's own footprint covers a gate
cell, so the gate never closes on top of it (legacy:15307-15323). A
friendly unit parked beside the gate does not hold it open unless it
stands in the gateway.

The update is edge triggered against the unit's stored activation
state. Closed and wanted issues an Activate order, open and not wanted
a Deactivate (legacy:15333-15359). The state toggle
(legacy:236384-236422) runs the script's `Activate` or `Deactivate`
threads on the rising or falling edge (legacy:236399-236406).

The `onoffable` button gives a manual toggle order, Active or Inactive
(legacy:151290-151300), and the scan asserts its own wish again on the
next brain tick. The engine departs here and holds a manual order
against the scan for 10 s, so the 3 s door animation is seen.

### Routes through a gate

When the path cost classifier meets a cell of a blocked class it
checks whether the cell is tagged a gate cell and stamped by a live
`gate=1` unit (legacy:21986-22032). If so ownership decides: a unit's
own gate is passable at cost class 4, and a foreign gate stays
blocked. The move step keeps gate blockers in a list apart from hard
blockers (legacy:219336-219340), so a unit walks through its own closed
gate without stopping and the doors open round it.

Selection skips a click on an open gateway (legacy:237917-237923).
Walls and gates are laid with the Shift drag in
[economy.md](economy.md). `bmcode` 1 is not their class: the loader
keeps it only for units (legacy:150077-150086, legacy:162924).

## The route search

### What a step costs

The search charges each step by its direction and by the kind of ground
it steps onto (legacy:22187-22222). A straight step is 16 and a
diagonal 23 (legacy:23535-23543). Ground adds 8 on a road, 24 on open
ground and 48 on bad ground (legacy:22988-22990), so a straight step
onto open ground costs 40 and bad ground adds 24 to that. A step that
turns the route costs more again for a slow turner
(legacy:23003-23192). Nothing charges for height.

Ground is bad when the footprint stands on a cell steeper than the
class's `BadSlope`, under water steeper than `BadWaterSlope`, or in
water outside `BadMinWaterDepth` and `BadMaxWaterDepth`, which keeps a
ship off the shallows (legacy:219720-219743). It is bad as well when
any cell in the ring round the footprint is not good ground, so beside
a cliff edge, deep water or a steep cell (legacy:219761-219791). A cell
steeper than `MaxSlope` is closed.

The engine departs here: its steps cost on a scale of its own with bad
ground in the same proportion and no road or turn charge, and features
do not make the ground beside them bad, since they change during a
battle.

### The search and the route

The search keeps a priority queue over one flat grid and scores a cell
by the octile distance to the goal, 18 for a straight step and 25 for
a diagonal, less a per search slack (legacy:189366-189390). It tests a
footprint at a placement by sweeping every tile of it
(legacy:219089-219131). There is no layer above the grid, no measured
distance, no field shared by a group sent to one place and no
avoidance between units. Unit against unit contact is settled at the
step: a refused step, a slide along the tile edge, a capped speed and a
delayed new search. The engine departs from this in four ways, a
clearance map (M-004), a long route guided by measured ground distance
(M-008), a flow field for a group (M-011) and units that give way to
each other (M-010).

The route is read back by walking a direction byte stored per cell
from the goal to the start (legacy:22438-22530). A point is kept only
where the step direction changes, plus the goal itself, with the start
first (legacy:22488-22495, legacy:22528-22535). Each point is the unit
origin that stands on that cell (legacy:22553-22560). A route holds up
to 64 points in map pixels (legacy:191102-191141). Passed points drop
from the front, and a route with fewer than two points left is no
route (legacy:191209-191236). The steer reads three points: the
segment's start, the current target and the point after it, the last
one repeated when the route is short (legacy:191238-191263).

Before a search has run, and when a search fails, the route is the
straight line from the unit to the goal (legacy:22536-22551,
legacy:191420-191423).

## Steering a ground unit

### The order of work

Each frame the mover does this, in order (legacy:185188-185247).

1. It sets its water flags from the unit's height against sea level.
2. It runs the waypoint check when the route's check timer has expired
   (legacy:185215-185218, the check itself at legacy:184623-184822).
3. It steers, a ground unit by the rule below (legacy:183376-183801)
   and a flyer by a velocity seek of its own (legacy:183869-184070).
4. It moves the unit and handles a refused step
   (legacy:184074-184343).
5. It reports the animation rate and the turn direction to the script
   (legacy:184346-184455, legacy:185076-185130), see
   [2026-09-09-cob-entry-points.md](2026-09-09-cob-entry-points.md).

### The steer

Without a route the unit brakes to a stop and, if a final facing was
ordered, turns in place toward it at `turninplacerate` once stopped
(legacy:183418-183436).

With a route the aim point is the current target pulled back along its
segment so that it sits 80 px ahead of the unit, or the target itself
once that is within 80 px (legacy:183447-183470). The look ahead drops
to 16 px while any of the mover's blocked flags is set
(legacy:183439-183442). Aiming ahead on the segment cuts corners
smoothly and keeps the aim point out of the unit's own turning circle.

The heading turns toward the aim point by at most `turnrate` a frame
(legacy:183472-183474, legacy:183053-183160). The script's turn
direction hook runs when the sign of the turn changes
(legacy:183162-183176).

The speed steps up by `acceleration` when both of these hold, and down
by `brakerate` otherwise (legacy:183475-183753).

- Twice the distance the unit would travel while it turns to face the
  aim point, its speed times the heading error over `turnrate`, is
  less than the distance to the aim point (legacy:183735-183740,
  legacy:183749).
- The stopping distance, the speed squared over twice the lesser of
  `brakerate` and the top speed, is less than the distance to the
  point after the target (legacy:183743-183753).

Speed never goes below zero and is capped by the slope table for the
unit's pitch, and the velocity is the speed along the heading
(legacy:183199-183203, legacy:183350-183372). For a ship the flags its
look ahead sets scale this by 2/3 or 1/3 (legacy:183236-183246).

A horseman (turnrate 1000, maxvelocity 2.9, acceleration and brakerate
10) facing away from its goal fails the turn test, so its speed drops
to zero in one frame and rises to full the next. It pivots at half its
speed on a tight arc until it faces within about 75 degrees of the aim
point, and then runs at full speed. Infantry (turnrate 2500,
maxvelocity 1.1) passes the turn test at any heading error and turns at
full speed.

### The waypoint check

The check runs at an interval in frames set by the unit type's speed
class, a byte derived from `maxvelocity` (legacy:184656-184658,
legacy:162838-162851), and flyers skip it (legacy:184662). Ships first
scan ahead along their heading (legacy:184666-184710). The engine
departs here and reads the speed class as one frame for each cell of
top speed.

If any of the eight neighbouring cells, 16 px away, holds terrain the
unit cannot cross, a structure, or a unit that is slower or headed
more than a quarter turn away, the mover sets its held neighbour flag
and skips the rest (legacy:184714-184728 over legacy:184456-184530). In
that state the route is followed closely with the 16 px look ahead,
and a target is passed only once the unit stands on it
(legacy:191282-191284). The engine passes it within 16 px, see M-001.

Otherwise the check weighs the current target T and the next point N
(legacy:184729-184791).

- T further than 143 px away is kept.
- T within 32 px is passed.
- In between, the check compares the cross track distance from the
  unit to the segment before T with that to the segment from T to N,
  and the heading error to T with that to N. A unit with turnrate 1000
  or more ignores the heading error to N, and one with 500 to 999
  halves it (legacy:184758-184781). When the current segment looks
  better on either measure, T is still passed if the next is within a
  factor of two on both, and kept otherwise. When the next looks better
  on both, T is passed.

After passing a point the check runs again on the new target
(legacy:184798, legacy:184819).

### A refused step

The step moves the unit while its footprint stays on the same tile
(legacy:184160-184165). On a tile change it asks the placement check,
which sweeps the whole body. A refusal sets the blocked bit the first
time and the hard blocked bit the second time running
(legacy:184169-184182). A blocked unit is not frozen. Its position is
pinned at the edge of its tile on the refused axis and keeps sliding on
the other (legacy:184190-184230), its speed is capped at half its top
speed on the first refusal and a fifth after, and its velocity is
turned back onto its heading (legacy:184244-184262,
legacy:184282-184300). The route is kept.

Since the step sweeps the whole body, a catapult will not climb a ramp
its body does not fit on and stops at the foot of it. Where a unit's
body fits no placement of its own cell, the engine walks it out before
the search, see M-004.

### Jams and new searches

A new search is asked for at once on a hard block (legacy:191290,
legacy:191387). Without one the request comes after a random delay
scaled by the speed class (legacy:191300-191360), or at random after
120 frames (legacy:191375-191385). The engine's timings depart here,
see M-002.

The planner does not see a unit on the move. Routes are planned through
a moving crowd and settled at the step (legacy:219329-219340). A unit
that has held its cell for 10 frames is folded into the search grid,
the rim of its footprint slowed and the inside closed, and after 150
frames the inside is closed outright (legacy:188900-188960,
legacy:187765-187880). It leaves the grid as soon as it changes cell
(legacy:188870-188897). Near the searcher the plan closes those cells,
and farther away it asks the live neighbour test (legacy:21986-22032,
legacy:184497-184510). So once a jam is a third of a second old, the
next route goes round it. The engine folds all of this into one rule,
see M-003.

The unit in the way is never touched. Nothing in the mover, the step
test, the neighbour test, the new search or the per frame update writes
to it or gives it an order, and an idle unit keeps its move state, so
it blocks like any slow unit (legacy:184494-184516). The only units
sent on a short move of their own are a factory's finished product and
dropped cargo, which walk off their spot (legacy:12264-12301,
legacy:14601-14629).

## Roads, water and turns

The mover keeps two flags, on a road and under the sea, set every
frame (legacy:185197-185206). A road takes every cell of the footprint
on the map's road mark, 0xFFFB in the feature layer
(legacy:220207-220234, legacy:225027). A unit is under the sea when
its own height is lower than sea level, a floater's height being sea
level less its waterline. Maximum speed, acceleration, braking and
both turn rates are scaled, by `roadmultiplier` on a road or else by
`watermultiplier` under the sea (legacy:183206-183224,
legacy:183053-183160). `roadmultiplier` is 1.2 where the file does not
say. Two Castles has 17 percent of its cells on roads. The unit
script reads the flags through its IN_WATER and ON_ROAD ports, and a
unit on a road plays `walk_road`.

The Rictus does not cross water in the standard unit set. V3Rocket's
moveinfo gives HOVER3 a MaxWaterDepth of 0, and the cell test refuses
a cell with any corner under the sea (legacy:219149-219156). With "Use
Crusades Units" on, its file gives it HOVER2, which has no such limit,
and it does cross. The base game's own moveinfo sets no such limit on
HOVER3.

The mover tells the script TurnDirection, the turn still to make in
degrees, when a turn starts, ends or changes sides, and a turn under a
degree is no turn (legacy:183163-183172). Its bearing reads the
target's position below the pixel, in 16.16, so a target walking past
is a small steady turn. Most scripts walk the legs while TurnDirection
is not 0, and the crossbowman's `walk` poses the arms too. The
crossbowman reloads in 4.7 s.

## Ship hulls

A ship holds the cells of its move class and nothing else
(legacy:163193-163195), and the move step refuses only a cell another
unit holds (legacy:219329-219340). The classes are squares, 32 px for
WATER2 up to 80 px for WATER5, under hulls that are mostly long and
thin. Two galleys can sit bow into stern 64 px apart with 86 px of each
hull inside the other.

The table holds every shipped unit whose move class needs water under
it, from the base game's units and models with Iron Plague's archives
over them. The hull is the drawn extent of the 3DO without the
selection mesh, in whole px rounded out. Length runs bow to stern, and
the beam is twice the widest reach to either side, oars and sails
included, since those are what show when two ships touch. The cell is
the side of the class's square.

| Unit | Name | Class | Cell | Length | Beam | Length over cell |
|---|---|---|---|---|---|---|
| ARATRANS | Ark | WATER5 | 80 | 171 | 108 | 2.1 |
| ARAWAR | War Galley | WATER4 | 64 | 150 | 50 | 2.3 |
| CREIRON | Iron Clad | WATER3 | 48 | 146 | 30 | 3.0 |
| CRESTER | Stern Wheeler | WATER3 | 48 | 129 | 32 | 2.7 |
| CRESUBM | Submersible | WATER2 | 32 | 99 | 54 | 3.1 |
| MONELEM | Sea Serpent | WATER4 | 64 | 95 | 78 | 1.5 |
| MONPIRAN | Piranha | WATER2 | 32 | 53 | 20 | 1.7 |
| NPCBOTL | Sea Shadow | WATER3 | 48 | 127 | 24 | 2.6 |
| NPCRIXX | Solan Rixx Flagship | WATER4 | 64 | 150 | 62 | 2.3 |
| TARCSHIP | Cult of Lokken Ship | WATER3 | 48 | 134 | 102 | 2.8 |
| VERFLAG | Flagship | WATER4 | 64 | 150 | 62 | 2.3 |
| VERHARP | Harpoon Ship | WATER4 | 64 | 137 | 108 | 2.1 |
| VERMAN | Man of War | WATER4 | 64 | 176 | 68 | 2.8 |
| VERSCOUT | Skiff | WATER3 | 48 | 113 | 82 | 2.4 |
| VERTRANS | Transport Ship | WATER4 | 64 | 109 | 108 | 1.7 |
| VERTRE | Trebuchet Ship | WATER5 | 80 | 145 | 158 | 1.8 |
| ZONBAR | Giant Barracuda | WATER3 | 48 | 133 | 82 | 2.8 |
| ZONKRAK | Kraken | WATER4 | 64 | 135 | 78 | 2.1 |

Every hull is longer than its cell, by 21 px for the Piranha up to 112
px for the Man of War, one and a half to three times the cell. Beams
run from half the cell (Sea Shadow) to twice it (Cult of Lokken Ship,
Trebuchet Ship), where oars or a yard reach out amidships. The hover
units (the gods, the Lich, the Mer Warrior, Kirenna, the Priest of
Lihr, Rictus, the Swamp Beast) and the ghost ship, which flies, are not
in the table.

The engine departs here and keeps ships apart by their hulls, see
M-012.

## Air traffic

### Nothing in the mover keeps flyers apart

The flyer mover seeks its target point and nothing else
(legacy:183869-184070, run for a `canfly` unit at
legacy:185232-185240). The step test passes any move by a unit that is
not in ground mode (legacy:219101-219103). Two flyers can share a
point. What parts them is an air traffic score their missions read,
and a landing rule.

The missions follow the `canfly` bit alone (legacy:162986-162987,
legacy:24120-24131). So the five shipped units with `canfly` and no
BeginFlight in their scripts fly, step out and land like the rest.

### The air layer and the crowd score

Once a frame, after every unit has moved, each airborne flyer lifts its
old stamp from the second word of the map cell records and stamps its
footprint there again (legacy:236045-236080, legacy:218519,
legacy:218336-218512). When the cell already holds another airborne
flyer, the two go on each other's list, a count and up to seven ids
(legacy:233431-233490). More than seven marks the flyer as saturated.
The original runs this pass only for the seats a machine simulates.

Then each flyer's crowd score is capped at 10 and moves once
(legacy:236083-236120). It gains 14 when the flyer is saturated, loses
2 when nothing shares its cells, and otherwise gains the number of
flyers that do. It never goes below 0. The engine counts flyers in
pairs instead of keeping an air layer, see M-014.

### The step out

Most VTOL missions, standby, land if can, move, patrol, guard and the
air attack among them, push a step out when the score is over 4
(legacy:24223, 24502, 24710, 24959, 26180, 26426, 26722, 27714, 28375,
29510). Some build and guard missions wait for 7 or 8 (legacy:31271,
31765, 32590). The step out missions' names begin VTOL_StepOut
(legacy:24242 onwards). The mission draws the distance d first, that
is rand(15) plus `footprintx`, times 16 px, which is 32 to 256 for a
Harpy.

The step out (legacy:32733-32880) scores eight bearings round the
flyer's heading. Each costs rand(80) plus 100 straight ahead, 0 at 45
degrees, 100 abeam, 200 at 135 degrees and 300 behind. Each flyer on
the list adds 100 to every bearing within 90 degrees of the way to it,
a half plane test (legacy:29973-30000). The cheapest bearing wins, the
first on a tie. The flyer turns it by rand(0x2000) less 0x1000, up to
22.5 degrees either way, and flies d plus rand(d/4) less rand(d/4) px
that way, until it is within (rand(5) plus 5) times 16 px of the
point, 80 to 144. The parent mission then goes on with its own goal,
so a flock under way sidesteps and carries on.

### Hovering on an attack

A flyer with `hoverattack`, such as the Harpy with `hoverattack = 1`
and `hoverattackdistance = 200`, holds a bearing of its own from its
target at that distance plus rand(8) less 3, picked again every 15 to
29 frames, with a 1 percent chance of a turn of up to rand(2048)
(legacy:30139-30370, keys parsed at legacy:163005-163017). The whole
attack is in
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

## Landing

### When a flyer lands

Stop resets the three weapons, and a unit with `canfly` that is in the
air then gets the land if can mission at its own position
(legacy:8855-8906). A flyer with nothing to do gets there through its
standby mission. With nothing in reach it looks for a target, and when
it finds none an airborne flyer gets the land if can mission at its
standby point. When that mission cannot be made it tries again 7 to 13
frames later (legacy:24427-24600).

A move ends once the flyer is within (rand(5) plus 5) times 16 px of
its point, and no sooner than rand(10) plus 5 frames after the move set
out, the delay the mission waits before it checks
(legacy:25066-25110). The point is first moved onto its footprint's
cell lattice, at most 8 px. The engine keeps the point as clicked, see
M-014. The flyer then stands by, and the standby runs VTOL_LANDIFCAN a
frame or two later (legacy:24427-24633).

### The landing search

An enemy in reach hands the flyer to the attack, and it does not land
(legacy:24257, 24392-24419). A crowded flyer steps out first.
Otherwise the land if can mission runs in stages.

- Stage 0 keeps the stop point as home and picks a random bearing
  (legacy:24260-24275).
- Stage 1 flies on 32 px ahead while the unit still holds more than a
  tenth of its top speed (legacy:24276-24295, 184890-184921).
- Stage 2 tests the spot under the flyer and lands there when it
  passes (legacy:24296-24382). When it fails it tries 12 spots on the
  cell lattice, the k-th rand(129 plus 32k) less (64 plus 16k) px off
  on each axis, out to 240 px, and flies to the first that passes.
  When none does it flies to 160 px from home, a third of a turn round
  from the last try, and tries again there (legacy:24297-24399). A
  spot or a circle point it reaches is tested at once. Its random
  spots are drawn at legacy:24329-24359 and snapped to the cells the
  way the test rounds a corner (legacy:24341-24345).

Only once a spot passes does the flyer run BeginLanding. The spot is
tested once, when the descent starts, and heights never change in a
battle, so a spot cannot turn to water under a landing flyer. A flyer
stopped near a shore finds dry ground within reach and lands there.
Over open sea it circles the stop point and stays in the air, still a
target for weapons that reach flyers.

### The landing test

The test is legacy:220068-220183, and the cell checks below are
legacy:220068-220178. The footprint's corner cell is the cell nearest
its corner, rounded rather than cut down (legacy:220088-220089). A
footprint that leaves the map, or reaches its last row or column, is
refused. Ground the unit's owner has never explored passes untested
(legacy:220095-220102). The fog cell the test reads is offset from the
footprint's corner by a quarter of the footprint's width on both axes.
The map it reads is the explored one, whose bits a side's sight only
ever sets (legacy:167404-167409), not what the side sees now. So a
search can make for sea out of sight, but the flyer only ever comes
down where it hovers, on ground its own sight has explored by then,
and the look there refuses the water. Explored ground is in
[fog.md](fog.md).

Otherwise each footprint cell is refused for any of these.

- A unit other than the flyer in the ground word.
- An airborne flyer other than itself in the air word. An air cell
  marked saturated, 0xffff, passes.
- A blocking feature, a building or a blocked cell mark.
- A lowest corner under the water floor, a highest corner over sea
  level less `minwaterdepth`, or corners that spread further than
  `maxslope`.

The water floor is sea level less `maxwaterdepth`, raised to sea level
for a unit with `canfly` and without `amphibious`
(legacy:220103-220109). That raise is what keeps a flyer with the
default depth of 10000 on dry ground. The keys and their defaults are
those of "Move classes" above. `amphibious` is parsed at legacy:162997.
There is no `canland` key. In the Iron Plague data no flyer is
amphibious and none floats, so no flyer in the shipped games ever lands
in water.

### On the ground

A landed flyer is in ground mode, and ground mode stamps the ground
word (legacy:218217-218250). So two landed flyers never share a cell
and walkers go round one. A flyer in the air stamps only the air word,
and the test for where a building or a summons may go up reads only
the ground word (legacy:219134-219147). So the next of a summons
without end goes up under the flyer that has just taken off from the
spot, and that flyer, its short step ended inside its ring, lands
somewhere else. Summons are in [economy.md](economy.md).

### Cruise height

The cruise target is `cruisealt` over a ceiling, the highest of sea
level and the cell heights in the 128 px buckets round the unit
(legacy:190499-190507, 241336-241344, 224299-224390). So over water a
flyer is always at least `cruisealt` over the surface. The engine
departs over land, where it follows the ground under the unit rather
than the bucket ceiling, which differs only over hills.
