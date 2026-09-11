# The clearance grid and what can sit on it (2026-09-10)

A design note. The first section describes what is in the tree. The rest
sketches how hierarchical search, flow fields and local avoidance would
attach to it, so the next piece of work starts from a base rather than a
blank page. None of the later sections is implemented.

## What is in the tree

`src/game/pathing.c` keeps, per map and move class, two cached layers:

- The passability bitmap on 32 px path cells, as before: terrain slope,
  the water window and the footprint corners.
- The clearance map on 16 px tiles. Each tile stores the side of the
  largest square of tiles, with that tile at its top left, that the move
  class can cross and no structure stands on. Zero means the tile itself
  is blocked. It is the classic true clearance table: one pass from the
  far corner, each open tile is one more than the least of its right,
  lower and diagonal neighbours. Gate cells count as open here whoever
  owns them, and the owner rule is applied live.

The map carries the occupancy version it was built at
(`GameWorld.occ_version`, bumped whenever a structure stamp changes), so
a finished wall or a demolished keep rebuilds it on the next plan. The
build is a few hundred thousand height samples on a large map, once per
change.

A plan is a `TAK_PathQuery`: the move class, the owner, the planner's own
occupancy id and its footprint in tiles. A cell is open when

1. the passability bitmap says so,
2. the clearance at the top left tile of the footprint placed on the
   cell centre is at least the larger footprint side,
3. no structure the owner may not cross and no parked unit other than
   the planner stands on the footprint tiles (never fewer than the
   cell's own four).

Parked units are the dynamic half. A mobile unit with no move under way
that has not made an integer move for a second gets `TAK_OCC_PARKED` on
its footprint cells and counts as an obstacle for everyone else's plans,
the way the original folds a unit with an old move stamp into its cost
grid (legacy:188962-188972). A mover held up in a jam does not park, or
a crowd would lock itself in place. Moving units stay invisible to the
planner and are resolved at step time, as in the original
(legacy:219329-219340). The start cell of a plan is exempt from the live
check: the unit already stands there, and only the ground and what is
built on it decide whether the search must begin elsewhere.

The query can also return only the corners of the route, which is how
the original stores one (legacy:22488-22495) and what the mover's look
ahead steering expects.

`TAK_PathClearanceAt` exposes the map for tests and tools.

## Hierarchical search over the clearance map

The A* runs on 32 px cells with an 8192 node cap, which covers most
orders on shipped maps but not a route across a large map with a long
detour. Two layers would remove the cap:

- Jump point search on the tile grid. The clearance map already answers
  "can a footprint of side k stand here", so a jump point search for a
  unit of side k walks tiles with clearance at least k and prunes the
  straight runs. Forced neighbours are read off the clearance map: a
  neighbour whose clearance drops below k next to one that does not is
  exactly the corner the search must stop at. One search per (class,
  footprint side) shape, no per unit rebuild.
- A coarse graph above it. Partition the map into blocks of 8 by 8
  cells, and for each block boundary keep the entrances that are open
  at each clearance level (a run of boundary tiles with clearance at
  least k gives one entrance for every k it satisfies). Intra block
  distances between entrances are computed with the jump point search
  once and cached with the occupancy version. A long order searches the
  block graph first and refines only the blocks on the way, which is
  what keeps the per tick plan budget honest for a thousand units.

Both layers key their caches on the same occupancy version as the
clearance map, and both hand the mover the same corner list, so the
steer in `walk_tick` does not change.

## Flow fields for groups

A group order today is one A* per unit toward the same goal, throttled
by the per tick budget. A flow field is one search per (goal cell, move
class, footprint side): an integration sweep outward from the goal over
the clearance map, producing for every reachable cell the direction of
the cheapest next step. Every unit in the group then reads its next
direction from the field at its own cell, and the mover's look ahead
steer follows that direction the way it follows a route corner today,
with the aim point placed on the field direction 80 px ahead.

The field is invalidated with the occupancy version like the other
caches, and a group whose members have different footprint sides needs
one field per side, which is bounded by the four shipped sizes. The
budget question moves from "plans per tick" to "fields per tick", which
is a far smaller number.

## Local avoidance between units

The original resolves unit against unit contact at step time: a refused
step, a slide along the tile edge, a capped speed and a delayed replan.
That is what the mover does now and it is faithful. Optimal reciprocal
collision avoidance would sit in front of it, as a velocity choice, not
a replacement:

- Each tick a moving unit gathers the mobile units within a couple of
  footprints from the occupancy layer.
- For each it builds the velocity obstacle from the relative position
  and velocity, takes half the responsibility, and keeps the velocity
  closest to the mover's preferred one, the aim point direction at the
  speed the accelerate or brake rule allows, that lies outside every
  half plane.
- The chosen velocity is then subject to the same turn rate: the mover
  turns toward it at `turnrate` and moves along its heading, so a unit
  never sidesteps faster than it can turn.

The step refusal stays as the last line, so nothing can overlap even
when the velocity choice is wrong. Because the avoidance velocity is
built from state every unit already has, the lockstep simulation stays
deterministic as long as neighbours are gathered in a fixed order.

Recording this as a deviation from the original would be required
before any of the three lands, since the original has none of them.
