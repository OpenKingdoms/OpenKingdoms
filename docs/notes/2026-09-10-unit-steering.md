# Ground unit steering and route following (2026-09-10)

Symptom that started this: a horseman given a plain move order on open
ground would loop once or twice before heading off (issue #26).

`:NNNNN` citations are evidence pointers into a private reference that is
not distributed. They mark where a claim can be re-checked by whoever holds
it. Function names below are our own labels.

## The per frame order of work

Each frame the mover does, in this order (:185188-185247):

1. Sets its water flags from the unit's height against the sea level.
2. Runs the waypoint check when the route's check timer has expired
   (:185215-185218, the check itself is at :184623-184822).
3. Steers: ground units through the rule below (:183376-183801), flyers
   through a separate velocity seek (:183869-184070) that this note does
   not cover.
4. Integrates the position and handles a refused step (:184074-184343).
5. Reports the animation rate and the turn direction to the script
   (:184346-184455, :185076-185130).

## The route

A route is a list of up to 64 points in map pixels (:191102-191141). The
search result is walked back from the goal and a point is kept only where
the step direction changes, plus the goal itself, with the start first
(:22488-22495, :22528-22535). Each point is the unit origin that stands
on that cell (:22553-22560). Passed points are dropped from the front,
and a route with fewer than two points left is no route (:191209-191236).
The three points the steer reads are the segment start, the current
target and the point after it, the last one repeated when the route is
short (:191238-191263).

Before a search has run, and when a search fails, the route is the
straight line from the unit to the goal (:22536-22551, :191420-191423).

## The ground steer

Without a route the unit brakes to a stop and, if a final facing was
ordered, turns in place toward it at `turninplacerate` once stopped
(:183418-183436).

With a route:

- The aim point is the current target pulled back along its segment so
  that it sits 80 px ahead of the unit, or the target itself once it is
  within 80 px (:183447-183470). The look ahead drops to 16 px while any
  of the mover's blocked flags is set (:183439-183442). Aiming ahead on
  the segment is what cuts corners smoothly and keeps the aim point out
  of the unit's own turning circle.
- The heading turns toward the aim point by at most `turnrate` per frame
  (:183472-183474, :183053-183160). The turn direction script hook fires
  when the sign of the turn changes (:183162-183176).
- The speed steps up by `acceleration` when both of these hold, and down
  by `brakerate` otherwise (:183475-183753):
  - twice the distance the unit would travel while turning to face the
    aim point, that is `speed * heading error / turnrate`, is less than
    the distance to the aim point (:183735-183740, :183749), and
  - the stopping distance `speed^2 / (2 * min(brakerate, max speed))` is
    less than the distance to the point after the target (:183743-183753).
- Speed never goes below zero, is capped by the slope table for the
  unit's pitch, and the velocity is the speed along the heading
  (:183199-183203, :183350-183372). With the 2/3 and 1/3 factors that the
  look ahead flags apply for ships (:183236-183246).

The consequence for a horseman (turnrate 1000, maxvelocity 2.9,
acceleration and brakerate 10): facing away from its goal, the turn test
fails, the speed drops to zero in one frame, then rises to full the next,
and the unit pivots at half its speed on a tight arc until it faces
within about 75 degrees of the aim point, after which it runs at full
speed. Infantry (turnrate 2500, maxvelocity 1.1) passes the turn test at
any heading error and turns at full speed.

## The waypoint check

Runs every "speed class" frames, a byte per unit type derived from
`maxvelocity` (:184656-184658, :162838-162851), and flyers skip it
(:184662). Ships first scan ahead along their heading (:184666-184710).

If any of the eight neighbouring cells, 16 px away, holds terrain the
unit cannot cross, a structure, or a unit that is slower or headed more
than a quarter turn away, the mover sets its "held neighbour" flag and
skips the rest (:184714-184728 over :184456-184530). In that state the
route is followed closely with the 16 px look ahead, and a target is
passed only once the unit stands on it (:191282-191284).

Otherwise, with the current target T and the next point N
(:184729-184791):

- T further than 143 px: keep it.
- T within 32 px: pass it.
- In between: compare the cross track distance from the unit to the
  segment before T with that to the segment T to N, and the heading
  error to T with that to N. A unit with turnrate 1000 or more ignores
  the heading error to N, one with 500 to 999 halves it
  (:184758-184781). When the current segment looks better on either
  measure, T is still passed if the next is within a factor of two on
  both, else kept. When the next looks better on both, T is passed.
- After passing a point the check runs again on the new target
  (:184798, :184819).

## A refused step

The integrator moves the unit when its footprint stays on the same tile
(:184160-184165). On a tile change it asks the placement check, and a
refusal sets the "blocked" bit the first time and the "hard blocked" bit
the second time running (:184169-184182). A blocked unit is not frozen:
its position is pinned at the edge of its current tile on the refused
axis and keeps sliding on the other (:184190-184230), its speed is capped
at half the maximum on the first refusal and a fifth after, and its
velocity is realigned with the heading (:184244-184262, :184282-184300).
The route is kept.

A new search is requested at once on a hard block (:191290, :191387),
after a randomised delay scaled by the speed class when the block came
from another unit (:191300-191360), and otherwise at random after 120
frames (:191375-191385). A unit that has not moved for a while is folded
into the search's cost grid as an obstacle (:188962-188972), so the new
route goes around it.

## What OpenKingdoms does

`walk_tick` in `src/render/units.c` follows the rules above: the aim
point 80 px ahead (16 px while blocked or crowded), the turn at
`turnrate`, the accelerate or brake decision, movement along the heading,
the waypoint check on the speed class interval, the refused step with
its slide and speed cap, and the delayed replan.

Two faults found on the way are fixed as well. The occupancy layer was
only created when the first structure was stamped, so in a skirmish with
no building yet mobile units never blocked one another and a crowd
ordered onto one point stacked. The layer now exists from the first
mover, as the map cell records do in the original. And a refused step
used to clear both sub pixel accumulators, so a unit whose refused axis
dominated never gathered a whole pixel on the free axis and sat still.

Differences, all recorded in `docs/MANUAL_DEVIATIONS.md`:

- The planning grid is 32 px cells, so near obstacles a target counts as
  passed within 16 px rather than only when stood on.
- The unit blocked replan waits a fixed two seconds plus a small per
  unit offset instead of the original's randomised delay, and a route
  that makes no progress toward its target for 2.5 s is dropped and
  planned again.
- A unit with nothing to do counts as a planning obstacle after standing
  still for one second, and a unit held up in a jam never does. The
  original's threshold on the move stamp is not recovered.
- The slope speed table and the ship look ahead are not implemented.
- The speed class interval is read as one frame per cell of top speed.
