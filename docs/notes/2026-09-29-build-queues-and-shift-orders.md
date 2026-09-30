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
orders. A unit holds two order lists, and the second is run on its own
(legacy:182203-182245). A new order that is not queued clears the first
list except for the orders flagged to survive, and leaves the second
alone (legacy:181670-181678, 180761-180795). A factory's rally has to
be set with a Move while it trains, so its build orders are among those a new order
leaves. Stop is such an order, issued the same way as the rest
(legacy:151390-151401), so Stop on a factory clears its rally and
standing orders and the training goes on. When a product cannot be
placed the original waits 7 to 21 frames and looks again
(legacy:9374-9382), and at the unit limit it says so and waits the same
way (legacy:9467-9483).

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
click within 48 px of any unit was taken as a click on that unit, so a
click near the soldiers standing at the old rally selected one of them.
The original picks a unit only where the click falls inside the box of
its model seen from above, turned with the unit and drawn where the
unit stands (legacy:237815-237922). The engine now picks the same way,
with the footprint standing in for a unit whose model is not loaded.
The front end of the remaster picks units itself, so its clicks say
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
- A count is taken down when its unit is done (legacy:9524-9527) or its
  frame is lost, killed or taken, on the pad (legacy:9293-9303), so a
  lost unit is not trained again. A run without end is never taken
  down.

The engine keeps the same runs, up to 32 of them and 9999 in a finite
run. It takes a unit off the count when that unit's frame goes up, one
tick sooner than the original, which comes to the same thing since the
frame is then either finished or lost. A factory queues units only. A
building def on a barracks' queue would never start and would hold up
everything behind it. The build command carries the count in its
argument. The classic sidebar reads Shift and Ctrl when a build button
is clicked.

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

A click with Ctrl changes the order in hand and keeps the queued ones
behind it, which the manual gives as the way to send a builder
elsewhere without losing its build queue.

## A factory still being built

The original never lets a frame be selected, so it never takes an
order. The engine did let one be selected, and a click on the ground
then played the Move voice for an order the frame refused. Now no frame
is selected by a click, a box or a group, and a click on your own or an
ally's frame, when it resumes no building, is a click on the ground
there. An enemy's frame can still be attacked. The remaster can let the
player queue units in a factory that is still going up, and they start
once it is finished. That is D-027. The classic view never sends it.

## A builder's build buttons

In the original a right click on the build button of a builder that
walks sends the same queue call as a factory's, with the count for
every one (legacy:150067-150093). For such a builder the call walks
its order list from the head and takes off every build order of that
kind (legacy:39336-39351, 181838-181866). The head is the order in
hand, and the removal unlinks it like any other (legacy:180806-180826).
The engine does the same through the factory dequeue command, so every
machine in a match applies it on the same tick. The builder's build
orders of the kind come off from the head, the one in hand first, and
the Ctrl count, which is all the sidebar sends a builder, takes every
one. Moves, other kinds of building and every other order stay where
they were.

When the order in hand goes the builder stops and takes its next order,
if it has one. Its frame stays on the map as it stands. Nothing is
refunded and nothing is taken off, as in the original. Here a builder's
frame goes up as soon as the building becomes its order, so the one in
hand always leaves a frame. Before this change the same right click
reached the factory's cancel and took the builder's frame off the map
with its mana. A builder helping another's frame holds the same order
here as one that placed it, so the right click drops that help too.

## Shift shows the orders

While Shift is held the classic view draws each selected unit's orders,
as Beyond All Reason does. That is the owner's rule, and D-030. It
reads the orders through `Units_OrdersOf`, the same read the remaster's
order lines use, and writes nothing to a unit, so it cannot change what
the simulation does.

- A thin line runs from the unit through the order in hand and then
  each queued one.
- A move is a green diamond, an attack red brackets on its target, or
  a cross on the ground, a patrol point a blue square and a factory's
  rally a yellow flag. Guard and heal, reclaim, and the rest have
  markers of their own.
- A patrol's route is drawn back through the point it started from and
  round to its first point, the way the unit walks it. Nothing queued
  behind a patrol is drawn, since a patrol never ends.
- A queued building shows as a translucent ghost of itself at its spot
  and facing, in the player's colours.
- Only the local player's own units show anything. An order on a
  target the player cannot see gives no position away. The order in
  hand leaves that stop out, and a queued one shows where it was given.

The cost stays with the selection. Nothing is read or drawn without
Shift, a unit's route is at most sixty four stops, lines wholly off the
screen are skipped, and a frame draws at most sixty four ghosts, each
posed once and kept for later frames. The cache holds a pose for every
ghost a frame can draw and the placement cursor's, seventy two in all.
Posing a new ghost runs its Create for up to six hundred script ticks,
so a frame poses at most two new queued ghosts and the rest show on the
frames after. The placement cursor never waits. A target that is dying
is hidden in the fog the same as a live one.
