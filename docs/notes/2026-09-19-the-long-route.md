# The long route (2026-09-19)

The first of the three movement tiers issue #60 asks for. It replaces
what a long order guesses about the ground it has not looked at yet.

## What the original does

One flat grid and one estimate. The search keeps a priority queue and
scores a cell by the octile distance to the goal, 18 for a straight
step and 25 for a diagonal, less a per search slack
(legacy:189366-189390). The route is read back by walking a direction
byte stored per cell from the goal to the start, keeping a point only
where the direction changes (legacy:22438-22530). There is no second
layer above the grid and no measured distance anywhere in it.

Ours was the same shape: A star over 32 px cells with an octile
estimate of 10 and 14, a per move class passability bitmap and a
clearance map under it, and an 8192 node cap.

## Why the estimate is the problem

A straight line says nothing about the ground in between. Order a unit
across a map with a lake in the middle and the search opens every cell
in a growing disc, because until it walks into the shore every cell
still looks like progress. On the bench below a route past a wall with
its door on the far side cost thousands of cells opened, and the comb
shaped map hit the 8192 cap and came back with the best it had rather
than a route.

## What landed

Per map and move class, the planner now measures ground distance from
three fixed marks to every cell, and a plan whose straight line is 32
cells or more scores a cell by

    max(straight line, max over marks of |d(mark, goal) - d(mark, cell)|)

The mark term is the triangle inequality. Whatever route exists from a
cell to the goal, following it and then the mark's own route reaches
the mark, so the difference between the two mark distances can never
be more than the route between them. The estimate therefore never
overstates what is left, and a search whose estimate never overstates
still returns a cheapest route. That is the whole reason this was
chosen over the two textbook answers:

- Jump point search prunes straight runs by assuming every step of a
  kind costs the same. Ours do not: a step is charged twice the height
  it climbs, so the symmetry the pruning rests on is not there.
- A block graph with entrances, the shape sketched in the clearance
  grid note, returns a route through the blocks it searched rather
  than the cheapest route, and its intra block distances have to be
  rebuilt whenever anything is built. The owner's bar for this work
  was the same route or a better one.

The marks are chosen by spreading: the cell furthest from the middle
of the map, the cell furthest from that one, then the cell furthest
from both. Distances are measured over the per class terrain bitmap
alone, with the same step costs the search charges and the same rule
against cutting a blocked corner.

Measuring over terrain alone is what makes the estimate safe rather
than what makes it cheap. Clearance, structures and parked units only
ever take cells away from a search, so a live route is never shorter
than the terrain one, and an estimate built from terrain distances
stays under it whatever is built or standing.

## What it does not touch

- A plan under 32 cells of straight line. Those open few cells anyway
  and the field would not pay for itself.
- A plan that starts on ground no route may stand on. Those may cross
  cells the marks never measured, and an estimate that jumps where it
  runs out of measurements is no longer consistent, which would cost
  the search its guarantee.

Both run exactly the search that was there before.

## What it does not fix

A route longer than the node budget. The bench carries a serpentine
whose cheapest route runs to over a thousand cells, and in a corridor
every cell lies on some cheapest route, so a search reaches the 8192
node cap whatever its estimate says. The field does change what comes
back on that map, because the cell it keeps is the one nearest the
goal on the ground rather than the one nearest in a straight line, but
it does not find the route. That is the budget's business and the tie
break's, not the estimate's, and neither is touched here.

## What it costs

The marks are three sweeps of the map per move class plus one to place
them, built the first time that class asks for a long route and held
with the passability bitmap. They are dropped with the rest of the
planner's caches, which happens on a world load and when a blocking
map feature is destroyed.

## What moved

Where several routes cost the same, a different one of them can come
back. Route cost did not: the bench asserts the field's route costs no
more than the flat search's on every scenario. The pinned simulation
probe does not move either, and not by luck. Its two lines are ordered
800 px apart, which is 25 path cells, and the field starts at 32, so
every plan in it runs the flat search exactly as before. Recorded as
M-008 in docs/MANUAL_DEVIATIONS.md.

## What is left of issue #60's movement track

Flow fields for a group moving to one place, optimal reciprocal
collision avoidance for the last few pixels, and potential fields for
squads. All three are separate pieces of work and none of them is
started.
