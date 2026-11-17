# A route out of a trapped start (2026-11-15)

A unit whose body fits no placement of its own path cell has a trapped
start. The route search handed such a unit routes it could not walk,
and it leaned on the ground in its way for good. This is what was wrong
and what the search does now.

## How a unit gets there

The step tests one point, the unit's centre, against terrain and water
(`unit_step_refused` in `src/render/units.c`). The search tests every
tile of the footprint at a placement, as the original sweeps a
placement (legacy:219089-219131). A 2 by 2 unit sliding along a one
tile ridge can push its centre into the ridge's inside corner with half
its body over the ridge. No placement of its cell then fits.

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

A trapped start is walked out first, tile by tile under the unit's
centre, by the step's own rule. From walkable ground the walk goes only
onto walkable ground and past no corner with shut ground either side.
From shut ground it may cross shut ground within two tiles of the unit,
as the mover's escape hatch lets a centre on it step out. Water is
never crossed, since the mover never relaxes it. What is built or
parked under the body is crossed only while the body already stands on
some, as the mover's `occ_escape` allows, and the bodies beside a
diagonal step must be clear too. The walk stops at tiles that centre a
legal placement. Each is a start for the cell search, at the price of
the walk: a tile in the unit's own cell or in a cell a route may stand
in costs what open ground does, and one in a cell with no placement
costs half of `PATH_PINCH_COST`, what crossing a cell cost before. The
cell a walk reaches adds its own half of bad ground when the search
starts from it, as a step into that cell would. The route begins with
the walk's first tile and its turns.

A pocket is walked out of the same way. When a search runs out within
64 cells and the unit's body at its real position is partly off its
ground, the walk runs from the unit's tile to placements outside the
cells the search reached, and the search goes on from there. When
that search runs out of budget, the route goes to the cell it got
nearest the goal, not to the pocket's nearest. A unit whose body is
all on its ground is not walked, so two shelves that touch where
nothing fits stay no route, as #96 has it.

Compression keeps the first point when the line past it, from where
the route begins, would take the body off ground it may stand on. It
also keeps it when the unit's centre, on the line from where the unit
really stands, would enter a tile the step refuses. The unit's own
tile is not judged, since the step lets a unit off ground it already
stands on. It drops the point otherwise as before.

The body check alone missed such crossings for two reasons. It starts
at the placement nearest the unit, not at the unit. For a one tile
class that is at most half a tile away when the unit's own tile is a
legal placement, and can be further when it is not. And for a one
tile class it tests the tile under x - 8, y - 8, which is not the tile
under the centre whenever x or y is less than 8 past a multiple of 16.
So it could miss a crossing even with the unit standing exactly on its
placement.

Two limits remain. Structures under the unit's real body are still
judged from the placement's body line, so a unit off its placement
can still meet the corner of a building and slide along it. When the
first point is kept, the leg from the unit to it is not checked.

The walk settles at most 4096 tiles in a window 128 tiles each way, and
what it settles counts against the tick's search budget.

## Measured

`test_pathing` holds each case on synthetic ground: the ridge corner,
the same ridge from edge to edge with no way round, a walk out that
turns, a unit in a one tile door that leaves the short way, a walk out
that never crosses water, a pocket with a one tile corridor, a pocket
beside a sealed goal on a map too big to search whole, a first point
that turns, and a first point judged from where the unit stands. The
ridge, the turn, the pocket and the first point that turns fail on the
search as it was. The pocket beside a sealed goal and the first point
judged from the unit fail without their own checks. On open ground
nothing changes: no start is trapped and the first point is dropped as
before, and `SIM_PROBE_HASH` does not move. `test_path_buffers` holds
the planner to its reference copy with no mismatch.

## Not done

- The step still tests only the centre, so units can still push into
  the corner and into pockets. In the original a catapult will not
  climb a ramp its body does not fit on, and stops at the foot of it:
  its step sweeps the whole body. That is a separate change.
- A pocket larger than 64 cells is not walked out of.
