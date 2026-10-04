# Several builders on one frame (2026-10-04)

The owner asked that a second builder, or a monarch, can join the work on
a building or unit another builder has started, that the work goes
faster in the original's balance, and that the hammer shows over the
frame when a builder is selected. The engine already let several
builders work on one frame, and their work already added up the way the
original's does. This note records the rules the original keeps for who
may help, how the help adds up, and what changed.

## Who may help

The original asks one question before a unit joins the work on a frame
(legacy:233556-233574). All of these must hold.

- The frame exists, is not the helper, is alive and is not dying.
- The frame is still under construction.
- The frame belongs to the helper's own player. The test compares the
  two players, so an ally's frame does not count (legacy:233564).
- The helper has the FBI `builder` line (legacy:162955) and can move.
  Factories and castles never help.
- When the helper has the FBI `builderlimited` line (read at
  legacy:163099-163100), the frame's type must be on the helper's own
  build list (legacy:233569-233572, list test at legacy:233192-233210).

Every mobile builder that is not a monarch carries `builderlimited` in
the shipped data, in the base game and in Iron Plague. The monarchs do
not. So a monarch helps any frame of its own player, a building it could
not start itself and a soldier in a barracks among them, while a Mage
Builder helps only the types on its own list.

The same question is asked again when the order starts
(legacy:12392-12412), and it decides the cursor.

## How the help adds up

Each builder makes its own call on the frame every frame of the
original's 30 a second, and nothing is shared between them but the
treasury (legacy:39451-39505). Each call adds the builder's
`workertime` over the frame's `buildtime` to the frame and pays the same
share of the frame's `buildcost`. A treasury that cannot cover what is
asked of it pays every consumer the same fraction, worked out once a
frame (legacy:235959-235977).

So the rate is the sum of the helpers' workertimes, with no cap and no
falling off. A frame takes buildtime over the summed workertime, and it
costs its buildcost however many work on it. The drain on the treasury
rises with the number of builders. An Aramon Barracks (buildcost 1955,
buildtime 600) takes one builder of workertime 10 sixty seconds at 32.6
mana a second, and Elsin with a Mage Builder thirty seconds at 65.2 mana
a second, 1955 mana either way. That is the manual's "extra speed comes
at an extra price in resources".

## The cursor and the click

With the left-click interface the cursor over your own frame is the
animated hammer, `cursorrepair`, when any selected unit passes the test
above (legacy:186541-186545). A frame is never selected
(legacy:186553), so with nobody to help it the cursor is the ground's,
a move for a selection that can walk and the plain pointer otherwise
(legacy:186603). The manual (p. 59) describes the same thing: "move the
cursor over the sparkling image of the under-construction Garrison.
You'll see an animated hammer. Left-click".

A click orders each selected unit for itself (legacy:238458-238660). A
unit that passes the test joins the work, and a walker that does not
goes to the ground under the click. Shift queues the order behind the
ones in hand.

## What changed

- `builderlimited` is read into the unit definition.
- `Units_CanHelpBuild` asks the original's question, and the help
  order, a queued help order, the hammer cursor and the click all use
  it. Before, any builder that could walk helped any frame that was not
  an enemy's, an ally's included, and the hammer showed for any builder.
- Over a frame nobody selected can help, the cursor is the ground's
  rather than the select hand.
- A click on a frame sends the other walkers in the selection to the
  spot, where before they were given nothing.

Engine build 30 records the change, since an order a client accepted
before may now be refused.

## Left for later

- When several builders are selected and a building is placed, the
  original sends every one that may build it (legacy:39194-39256), and a
  builder that finds the frame already standing joins it
  (legacy:12040-12052). The engine gives the site to the first.
- Guarding a builder helps its build in the original (legacy:25917-26040).
- The hammer and a repair for a finished, damaged unit with a builder
  selected (legacy:233526-233553), and allies for the armed heal.
