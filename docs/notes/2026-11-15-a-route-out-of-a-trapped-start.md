# A route out of a trapped start (2026-11-15)

A unit whose body fits no placement of its own path cell has a trapped
start. The route search handed such a unit routes it could not walk,
and it leaned on the ground in its way for good. This is what was wrong
and what the search does now.

## How a unit gets there

The step used to test one point, the unit's centre, against terrain and
water, while the search tests every tile of the footprint at a
placement, as the original sweeps a placement (legacy:219089-219131). A
2 by 2 unit sliding along a one tile ridge could push its centre into
the ridge's inside corner with half its body over the ridge. No
placement of its cell then fits. The step now tests the body
(2026-11-15-the-step-tests-the-body.md), but a unit can still start on
such ground: put there by a map, a factory exit or a spot chosen by its
centre.

## What the search did

It began the route from the unit's own cell and let it cross cells with
no legal placement at a heavy price. Three things went wrong.

- The first step off the start cell was never tested. The slide rule,
  one tile from a legal placement to a legal placement, was skipped
  whenever the cell being left had no legal placement, so a legal cell
  beside the start was a free step whatever lay between.
- A crossing took a 32 px cell as crossable when any one of its four
  tiles was walkable, and never asked whether the walkable tiles of two
  cells touched.
- Compression never kept the first laid point. A turn there was lost,
  and the line from the unit to the next point cut across what the
  turn went round.

The test helper that checked a route's first leg judged it cell by cell
too, so the tests could not see a one tile ridge.

## Observed

On Valysia City an ARASWORD stood at 7024,751 for good. Its centre tile
was open, and the tiles west, south and south west of it were shut,
part of a staircase ridge running north west to south east. Its goal
lay 96 px west. The search answered with one point, 6928,752, a line
through the ridge, and the step refused its first pixel. From the same
spot a route to a point to the south went round the ridge's south east
end in 12 points, so the ground was connected. Nearby, four units stood
in a pocket near 6784,848 for good: each cell was legal, each body stood
partly on rock, and each search ran out in nine cells with no route.
Both were found by a long march probe in a fork of this engine.

## What it does now

A start is trapped when the body fits no placement of its own cell,
when a body wholly on its ground can reach none of them in a line
without meeting bad ground, a building or a corner, or when the step
would refuse the first leg of the route the search lays from the cell.
A trapped start is walked out first, body by body by the step's own
rule. The body never takes on more bad ground than it stands on, beside
a diagonal too, and the point it stands on never enters water the mover
keeps it out of. What is built or parked under the body is crossed only
while the body already stands on some, as the mover's `occ_escape`
allows, and the bodies beside a diagonal step must be clear too. The
walk stops at bodies that are a legal placement. Each is a start for
the cell search, at the price of the walk: a tile in the unit's own
cell or in a cell a route may stand in costs what open ground does, and
one in a cell with no placement costs half of `PATH_PINCH_COST`, what
crossing a cell cost before. The cell a walk reaches adds its own half
of bad ground when the search starts from it, as a step into that cell
would. The route begins with the walk's first point and its turns.

A pocket is walked out of the same way. When a search runs out within
64 cells and ground or buildings, not parked units, shut in the cells
it reached, the walk runs from the unit to placements outside those
cells, and the search goes on from there. When that search runs out of
budget, the route goes to the cell it got nearest the goal, not to the
pocket's nearest. Where the body cannot get out, neither can the walk,
so two shelves that touch where nothing fits stay no route, as #96 has
it.

Compression keeps the first point when the line past it, from where
the route begins, would take the body off ground it may stand on or
across a corner either side of which it may not stand. It also keeps
it when the step would not walk the unit along that line from where
it really stands. The route begins at the placement nearest the unit,
not at the unit, so the line from each can differ. It drops the point
otherwise as before.

Structures under the unit's real body are still judged from the
placement's body line, so a unit off its placement can still meet the
corner of a building and slide along it. A flow field route the step
could not start goes to a search instead.

The walk settles at most 4096 tiles in a window 128 tiles each way, and
what it settles counts against the tick's search budget.

## Measured

`test_pathing` holds each case on synthetic ground: the ridge corner,
the same ridge from edge to edge with no way round, a walk out that
turns, a unit in a one tile door that leaves the short way, a walk out
that never crosses water, a pocket too narrow for the body to leave, a
pocket beside a sealed goal on a map too big to search whole, a unit
wedged by a corner, a unit partly on rock whose route cannot start, a
first point that turns, and a first point judged from where the unit
stands. Each leg is checked pixel by pixel by the step's rule. On open
ground nothing changes: no start is trapped and the first point is
dropped as before, and `SIM_PROBE_HASH` does not move.
`test_path_buffers` holds the planner to its reference copy with no
mismatch.

## Not done

- The step tested only the centre when this was first written, so
  units could push into the corner and into pockets. It tests the body
  now: see 2026-11-15-the-step-tests-the-body.md.
- A pocket larger than 64 cells is not walked out of.
