# Air traffic (2026-10-04)

The owner found that Harpies sent anywhere ended in one pile, so one
Water Ball or Water Blast on the pile hit all of them. The engine treated
a flyer as holding nothing and steered it to the exact point it was sent
to, inside 8 px, and it landed wherever it went idle. This note records
how the original keeps its flyers apart and what the engine now does.

## Nothing in the mover keeps flyers apart

The original's flyer mover seeks its target point and nothing else
(legacy:183869-184070, dispatched for `canfly` at legacy:185232-185240).
The step test passes any move by a unit that is not in ground mode
(legacy:219101-219103). Two flyers can share a point. What parts them is
an air traffic score their missions read, and a landing rule.

## The air layer and the crowd score

Once a frame, after every unit has moved, each airborne flyer lifts its
old stamp from the second word of the map cell records and stamps its
footprint there again (legacy:236045-236080, legacy:218519,
legacy:218336-218512). When the cell already holds another airborne
flyer, the two go on each other's list, a count and up to seven ids
(legacy:233431-233490). More than seven marks the flyer as saturated.

Then each flyer's crowd score is capped at 10 and moves once
(legacy:236083-236120). It gains 14 when the flyer is saturated, loses 2
when nothing shares its cells, and otherwise gains the number of flyers
that do. It never goes below 0.

## The step out

Most VTOL missions, standby, land if can, move, patrol, guard and the
air attack among them, push a step out when the score is over 4
(legacy:24223, 24502, 24710, 24959, 26180, 26426, 26722, 27714, 28375,
29510). Some build and guard missions wait for 7 or 8 (legacy:31271,
31765, 32590). The mission draws the distance first, d =
(rand(15) + footprintx) * 16 px, 32 to 256 for a Harpy.

The step out (legacy:32733-32880) scores eight bearings round the
flyer's heading. Each costs rand(80) plus 100 straight ahead, 0 at 45
degrees, 100 abeam, 200 at 135 degrees and 300 behind. Each flyer on the
list adds 100 to every bearing within 90 degrees of the way to it, a
half plane test (legacy:29973-30000). The cheapest bearing wins, the
first on a tie. The flyer turns it by rand(0x2000) - 0x1000, up to 22.5
degrees either way, and flies d plus rand(d/4) less rand(d/4) px that
way, until it is within (rand(5) + 5) * 16 px, 80 to 144, of the point.
The parent mission then goes on with its own goal, so a flock under way
sidesteps and carries on.

## Landing

A move ends once the flyer is within (rand(5) + 5) * 16 px of its point
(legacy:25066-25110). The flyer then stands by, and the standby runs
VTOL_LANDIFCAN a frame or two later (legacy:24427-24633). That steps out
first when the flyer is crowded. Otherwise it lands where it hovers only
when every cell of its footprint passes the test at
legacy:220068-220178:

- no unit but itself in the ground word,
- no airborne flyer but itself in the air word,
- no blocking feature and no building,
- no corner under its water floor, which is sea level for every
  shipped flyer, and no slope past its maxslope
  (2026-10-04-flyers-land-on-dry-ground.md has the whole test).

When its own spot fails it tries 12 spots on the cell lattice, the k-th
rand(129 + 32k) - (64 + 16k) px off on each axis, out to 240 px, and
flies to the first that passes. When none does it flies to 160 px from
where the search began, a third of a turn round from the last try, and
tries again there (legacy:24297-24399). Only then does it run
BeginLanding.

A landed flyer is in ground mode, and ground mode stamps the ground word
(legacy:218217-218250). So two landed flyers never share a cell and
walkers go round one.

## What the engine does

`air_traffic_pass` in `src/render/units.c` runs on every even tick,
after the combat tick has moved everyone. It counts, for each airborne
flyer, the airborne flyers whose footprints share a cell with its own,
and moves every finished flyer's crowd score by the original's rule. It
counts pairs rather than keeping one holder per cell (M-014). The pass
runs for every seat. The original runs it only for the seats a machine
simulates, which under lockstep is every seat.

`flyer_air_tick` holds the missions' decisions, taken once a frame. An
airborne flyer whose score is over 4 on a move, a patrol, a guard, an
attack or no order, or over 7 at builder work, steps out by the
original's costs and draws. An idle one runs the landing rule above.
Each of these is an air leg, flown in place of the order's goal until
the flyer is within its reach, and a new order ends it. The leg, the
score and the circle are in the state hash and the unit record (version
11).

A flyer's move now ends at a ring (rand(5) + 5) * 16 px from its point.
The flyer makes for the nearest point of the ring and brakes onto it.
A formation move keeps its exact points.

A landed flyer with the flight pair is a mobile occupant, stamped when
it lands and lifted when it takes off.

## Left out

- Hover attack. A Harpy has `hoverattack = 1` and
  `hoverattackdistance = 200`. Such a flyer holds its own bearing from
  its target at that distance plus rand(8) - 3, picked again every 15 to
  29 frames, with a 1 percent chance of a nudge of up to rand(2048)
  (legacy:30139-30370, keys parsed at legacy:163005-163017). The engine
  does not read the keys yet. Attacking flyers that share cells still
  step apart, so a flock at its target spreads out all the same.
- A saturated air cell, 0xffff, passes the landing test in the original.
  The engine has no air cells, so it never comes up.
- The original snaps a move's point to its footprint's cell lattice
  before the ring is drawn. The engine keeps the point as clicked, which
  is at most 8 px off and inside the ring.
- The five units with `canfly` and no BeginFlight in their scripts keep
  out of the air traffic (M-014).
