# Build queues, rally points and Shift orders (2026-09-29)

The owner reported that factory build queues sometimes stopped
producing and that rally points sometimes would not set. He also asked
for the build buttons' Shift and Ctrl clicks and for Shift to queue
orders, which the manual describes and the engine did not have. This
note records what the original does, what was wrong here and what the
engine does now.

## Why a queue stopped

A factory trained only while its own order was the build. Anything that
replaced that order dropped the product in hand, left its frame on the
pad and left the rest of the queue waiting for a unit that would never
finish. Four ways in were found, each now a test in
`src/render/test_orders.c` that fails on the code before this change:

1. Patrol on a factory made it patrol, which a building cannot do.
2. Stop cleared the product in hand.
3. Guard gave it a guard order, and the build stopped being worked on.
4. At the unit limit a finished product's successor could not be
   placed. It was taken off the queue and lost, and nothing started the
   rest.

The original keeps a factory's build orders apart from its other
orders. A new order that is not queued clears the unit's order list
except for the orders flagged to survive (legacy:181670-181678), and a
factory's rally has to be set with a Move while it trains, so its build
orders are among those that survive. When a product cannot be placed
the original waits 7 to 21 frames and looks again (legacy:9374-9382),
and at the unit limit it says so and waits the same way
(legacy:9467-9483).

The engine now keeps training apart from the factory's order. Each tick
a factory builds the product in hand whatever order came since, and an
idle factory with a queue starts the next one. A product that cannot
start stays on the queue and the factory tries again 28 ticks later,
the middle of the original's wait. Stop on a factory drops its rally
and standing orders and leaves the training alone. Guard is refused by
a unit that cannot move.

## Why a rally did not take

A product walked to the rally only when the factory itself finished it.
A builder helping at the pad finished the soldier as often as not, and
then the soldier stood on the pad. The original hands a finished
product its factory's orders in the factory's own order handler
(legacy:9512-9526), so it does not matter who laid the last brick. Now
a factory that finds its product finished by someone else releases it
the same way.

The classic view sets a rally with a left click on the ground, and a
click within 48 px of any unit is taken as a click on that unit. The
front end of the remaster picks units itself, so its clicks now say
which unit is under them, or none, and one it has judged open ground is
never taken for the unit beside it.

## The build buttons

The original's build button handler (legacy:150046-150101) passes a
count to the queue: one for a click, five with Shift, and ten million
with Ctrl, negative for the right button. The queue itself
(legacy:181790-181866) is a list of runs, each one def and a count.

- An add joins the last run when it is the same def and appends a new
  run otherwise.
- Ten million or more is a run without end, drawn as `+++` on the button
  (legacy:149925-149930). Nothing is added behind one, since it would
  never be reached.
- A removal takes from the last run of that def first and works back,
  and the product in training goes last. Reaching a run without end, or
  a count of ten million, takes every one of that def, the one in
  training included. That is how the manual's "right-click on the unit
  icon" cancels a Ctrl click.
- A count that finishes is taken down only when its unit is done
  (legacy:9524-9527), and a run without end never is.

The engine keeps the same runs, up to 32 of them and 9999 in a finite
run. The build command carries the count in its argument. The classic
sidebar reads Shift and Ctrl when a build button is clicked.

## Shift queues orders

The manual's section on orders queues moves, builds, heals and patrols
with Shift, and a click with Shift keeps the order armed for the next
one (legacy:243648 and 243685 keep the mode while Shift is down). Any
unit order now carries a queue bit. A queued order waits behind the one
in hand and the ones before it, up to sixteen, and each is taken when
the one before it is done. One that can no longer be carried out, a
target gone or a site taken, is passed over. A queued building's frame
goes up only when its turn comes.

Patrol points given with Shift make one route. The unit walks them in
the order given and back through the point it started from, round and
round. The original's own rule for more than two points was not found,
so this is the engine's reading of the manual's example of patrolling
between two points queued with Shift.

A factory takes Move and Patrol as standing orders for its products, as
the manual describes, and Shift adds more that each product carries on
with after the rally.

## A factory still being built

The original never lets a frame be selected, so it never takes an
order. The remaster can let the player queue units in a factory that is
still going up, and they start once it is finished. That is D-027. The
classic view never sends it.

## What is left out

A mobile builder's build buttons in the original take a right click to
clear every queued building of that kind. The classic sidebar does not
do that yet. The classic view does not yet draw a unit's queued orders
while Shift is held.
