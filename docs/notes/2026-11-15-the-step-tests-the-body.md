# The step tests the body (2026-11-15)

The step used to test one point, the unit's centre, against terrain and
water. The route search tests every tile of the footprint. The two
disagreed at every edge of rough ground, and the step let units go
where the search could not see them. This is what the step does now,
what had to change beside it, and what was measured.

## Seen in the original

Catapults were sent up the ramp to the high ground on Abnar's Terrace,
in the original and in the engine, with move orders from about 300 px
away. In the original a catapult stopped short, at the foot of the ramp
or of the cliff, whichever was nearer. Repeated orders brought it closer
but never up. It made for the flattest, closest ground it could reach
and did not go back onto the ground it had stopped short of. On the east
side there is no way up for it at all. In the engine the catapult walked
its centre up the ramp, and its body hung over the cliff edge.

After the change, on a fork of this engine that carries it, compared
against the original again: catapults path as they do there, with no
clipping into cliffs and the same ramps out of their reach. Units on the
edge of ground they cannot path on still get out.

## What the step does now

The step counts the tiles of the body, anchored on the position
rounded to the tile grid (legacy:184166-184186), that stand on ground
the route search would refuse: past the class's slope, a closed mark,
a blocking feature, outside its water depths, or off the map. It
sweeps the whole footprint, as the original's step does
(legacy:219089-219131), and reads the same ground the search plans on
(`TAK_PathBodyBad`). A step that adds to the count is refused. A unit
half on a slope may make for flatter ground and may not go back. The
water window at the centre is kept as it was, since it alone keeps a
boat off dry land. Skipping waypoints asks the same of the next
position, so a large body near a cliff follows its route point by point.

A body crossing a tile corner, its tiles moving along both axes in one
step, must also pass the two bodies either side of the corner. The
original's rule for corners is not established. The engine's search
and its walk out of a trapped start have always asked this of a
diagonal, and without it on the step a unit slipped past a corner into
ground neither could lead it out of. On Valysia City an ARASWORD
crossed one at 6792,856 into four tiles of open ground walled by rock,
and stood there for good. A long march probe in a fork of this engine
found it, before this change.

## What changed beside it

- The walk out of a trapped start follows the step's rule. It never
  takes the body onto more bad ground than it stands on, beside a
  diagonal too, where it used to judge the centre tile and cross shut
  ground within two tiles of the unit. Its nodes are the points a body
  stands on: a tile's centre for an odd footprint, its top left corner
  for an even one. Its first steps, out of the unit's own tile, are
  judged by the step's own test from where the unit stands. After them
  water is tested at every pixel between two points, as the step tests
  it at the centre.
- A unit standing on its ground but a tile off every placement of its
  own cell, which can reach none of them in a line without meeting bad
  ground, a building or a corner, has a trapped start and is walked out
  first. The search begins a route at its cell's placements, and the
  first leg from where the unit really stands crossed the corner.
- A route whose first leg the step would refuse from where the unit
  stands is planned again as a trapped start, walked out first. A unit
  partly on bad ground in a legal cell got a route from the cell's
  placements, and when the step refused its first pixel and both
  slides, every replan gave the same route and it stood for good. The
  centre's step had let it through. A flow field route the step could
  not start goes to a search instead.
- The first point of a compressed route is kept when the line past it
  would cut such a corner, as it is kept when the line would take the
  body off its ground. The body line is anchored on the rounded
  position, as the step anchors the body.
- The first point is also kept when the step would not walk the unit,
  from where it really stands, along the line past it. That check used
  to follow the centre's tile. It now follows the step: no more bad
  ground under the body than it stands on, at a corner or not, and the
  centre never into water it may not enter.
- A pocket is walked out of when its search runs out within 64 cells
  and ground or buildings shut it in. A body wholly on its ground can
  now be in one, so the old test, that part of the body stood off its
  ground, no longer applies. A unit boxed in by a crowd of parked units
  is no pocket and is not walked. The search out of a pocket still
  starts its nearest cell afresh.
- The walk counts each tile's body once and keeps the count, a footprint
  of ground lookups for each tile it reaches. The step counts the body
  once a step, and twice more across a corner. The first step
  after a cache reset can build the planner's ground cache.

## Measured

On a fork of this engine, built with and without the change, a long
march probe counted 40 units sent across three maps:

| March | Without | With |
|---|---|---|
| Abnar's Terrace: units at their goals | 29 of 40 | 29 of 40 |
| Abnar's Terrace: stopped short | 9 | 9 |
| Abnar's Terrace: unit ticks stuck | 22,771 | 42,221 |
| Angvir's Maze: units at their goals | 36 of 40 | 36 of 40 |
| Angvir's Maze: unit ticks stuck | 46,464 | 75,888 |
| Valysia City: units at their goals | 34 of 40 | 35 of 40 |
| Valysia City: stopped short | 4 | 1 |
| Valysia City: left over 320 px behind | 1 | 0 |
| Valysia City: unit ticks stuck | 84,696 | 146,236 |

Without the change an ARAARCH ended the Valysia City march in the
pocket at 6848,848. With it nobody does. The ticks stuck go up: on
Abnar's Terrace and Angvir's Maze mostly the knights' and catapults'
larger bodies, held at the foot of ground they used to walk their
centres onto, and on Valysia City every kind. That is the original's
rule.

`test_pathing` holds the walk out on synthetic ground by the step's
rule. A unit wedged by a corner fails on the search without the trapped
start for it. A unit with a tile of its body on rock, whose route from
its cell cannot start, fails without the walk out for it. A unit half on
the wall of a pocket walks out along a one tile corridor beside that
wall with no more rock under it, which a body wholly in the pocket
cannot. A first point judged from a tile off its placement fails
without the check from the unit. The ground crowd's layout in
`test_movement` does not change.

`test_movement`'s case of a ship parked by a yard's rally point (M-012)
had the ship 150 px east of the rally point. A product that stops short
of the rally point and is in the way of the next one is sent off again
to a spot of its own, which makes it a launch, and a ship in a launch's
way is moved by M-012's rule. Put at 14 places along the rally point's
row, from 200 px west to 280 px east of it, the parked ship was ordered
off at 9 without this change and at 9 with it, every time while the
yard had sent a product off its way. The places differ, and 150 px east is
one of them with the change and not without. The case now parks the
ship 200 px east, where it is ordered off neither with the change nor
without it, and still counts every order. With M-012's rule for ships bound for the rally point
undone, it is ordered off 11 times there.

## A gap one body wide (2026-11-15)

Seen in the original: twelve swordsmen sent through a gap exactly one
body wide. Three to six get through. The rest back up and cycle, then
either find another way or try again later. None stands for good.

With the step testing the body, a 2 by 2 body fits such a gap only with
its centre in a 16 px window across it, where the centre had 32 px. In
test_movement's world, with a wall three tiles thick and a gap at tile
rows 74 and 75, swords that reach the wall face a few pixels outside the
window stop there. They park, and parked units close the gap to the
others' searches. A unit with no route walks straight at its point and
presses square on the wall, and the step slides only when both axes
move. The stall ladder (M-006) still climbed, since it counts ground
closed against the best so far whatever the route does. Its last rung
ended the order, and the units that gave up stood at the face or in the
gap and held the rest back for good.

The last rung now keeps a move order while the ground joins the unit to
its point. The original re-runs its search on a delay and never moves
the unit in the way (legacy:191265-191388). A kept order searches once a
rung, every 240 ticks, and not on the shorter retry a unit with no route
otherwise has. A rung search the tick's budget refused runs on the next
tick, as any other does. The order ends after 30 kept rungs with no
ground closed. A point the body cannot stand on is kept as well: on
Valysia City, a 40 unit army sent to points of their own ended 132 of
240 within 192 px of them that way, against 100 when such points were
never kept. The label check behind the ground test answers yes where it
cannot tell, and the 30 rung bound covers that. A land unit ordered far
out to sea ends its order at the shore in any case. The most any unit
used in the sweep below was 22. A unit whose body comes within 32 px of
a friend that has stopped, or a ship within its arrival reach of one,
still ends its order, which is how a crowd sent to one point packs.

Measured, 2 to 12 swords over ten spawn offsets, at spacings of 32, 40
and 48 px, with the wall whole and with a second gap eight tiles wide
far to the north: 660 runs of 4620 units. Before, 104 runs left 191
units, every one with its order ended. Now no unit is left. 10 go round
by the second gap, 21 get through after tick 3000, and the last is
through by tick 7440. In the 12 sword runs, 2 to 12 are through by tick
1500. The goal there is 600 px past the wall. With it 60 to 150 px past,
1, 3 and 4 of 12 get through and the rest end their orders at the face,
as before this change: they stop beside friends that have stopped.

The rule for stopped friends matters for cost too. Without it, on
Valysia City, a 40 unit army sent to one point stood 164,316 ticks, made
16,818 searches and left 6 orders that never ended, against 36,599,
8,439 and none with it. With 200 swords behind a gap plugged by a hut,
a kept order searching on the shorter retry made 7.6 searches a tick.
Searching once a rung makes 0.84.

The tests are in test_movement.
- a_group_sent_through_a_gap_one_body_wide_all_get_through: before, 7 of
  its 12 got through, and now all 12 do, 5 of them by tick 1500.
- a_fleet_sent_to_one_point_packs_and_ends_its_orders: ten ships of five
  kinds. With the stopped friend judged by centres 64 px apart, two held
  their orders for good.

## Every leg of a walk out (2026-11-15)

Two reviews of the walk out found routes the step would refuse, each now
checked by a test in test_pathing.
- A diagonal between two tiles' points. Up one axis and down the other,
  the body's tiles turn a pixel apart on the two, so it passes the side
  of the axis moving up, and it may not take on more bad ground from
  there. The walk judged the side and the next tile against the tile it
  left. It now walks them in the step's order
  (test_a_walk_out_diagonal_passes_its_side_as_the_step_does).
- The first leg. A unit is seldom on its own tile's point, and the walk
  judged its first steps from that point: a line from the unit to the
  next point could pass more bad ground
  (test_a_walk_out_starts_where_the_unit_stands), and where heights are
  read between corners, the point could be in water with the unit's
  centre dry, so every first step failed and there was no route
  (test_a_walk_out_leaves_from_where_the_unit_stands). The first steps
  are now the step's own test from where the unit stands. A version that
  sent the unit to its own tile's point first did not help: the mover
  passes a point within 16 or 32 px of it without walking to it.
- A walk out whose first leg the step would still refuse is no route.
  A route from a search that the step cannot start was already planned
  again as a walk out.
- A field route for a unit already in its goal's cell, one point, was
  not judged from the unit. It is a search's now when the step cannot
  walk it (test_a_field_route_in_the_goal_cell_is_judged_from_the_unit).

Measured on synthetic ground with lakes, shut tiles, buildings and the
map's edge, for land units, waders, boats and amphibious classes with
bodies of 1 to 5 tiles, from every fourth pixel of a 160 px square to six
goals: 1,296,000 searches and as many field routes, with heights read
one a tile and again between corners. Every leg of every compressed
route, as the planner lays it, was walked by the step's rules from the
unit's exact pixel. No route's leg is refused for water, bad ground or a
corner. After the
first review's fix alone, with heights read between corners, 9,933 walk
outs had a first leg the step refuses for water, and 3,870 routes the
step could walk were lost. Against the planner before both reviews,
61,306 searches find no route now where they found one. 61,294 of them
start with the unit's centre in water it may not enter, and none of the
routes lost was one the step could walk. 6,037 searched routes and
3,758 field routes have a first leg that clips a building, since the
test from the unit is of ground and water alone (M-004). Uncompressed
routes, which only tests ask for, keep a point a cell and their legs are
not judged. In test_path_buffers, 5148 of its 7830 plans find a route,
where 5255 did: 105 fewer start with the unit's centre in forbidden
water, and 2 fewer start dry.

## Not done

- The spots a unit is sent to still judge the centre alone: a factory's
  exit, a ship's launch spot, the place a builder stands off at, and a
  free spot near a point. A product can be sent to a spot its body does
  not fit, and stop short of it. That likely is why the launches above
  moved.
- The first leg is judged on the route the planner lays, not on what
  the mover walks. The mover passes a first point within 32 px, or 16 px
  beside an obstacle, without walking to it. In the brute force above,
  37,015 routes have a first point that near and a line past it the step
  refuses. There the unit is refused, slides, or drops its route and
  plans again.
- A unit whose centre stands in water it may not enter gets a route only
  where the step can take it a pixel onto dry ground, but not always
  there: the walk out's start tests lines only to the points of its 8
  neighbouring nodes. In 60 of the routes lost, a pixel the unit could
  step to has a route the step can walk.
