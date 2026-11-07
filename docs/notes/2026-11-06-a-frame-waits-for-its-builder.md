# A frame waits for its builder (2026-11-06)

Two playtest reports turned out to be one thing.

A builder could be told to help a build that was placed but not started.
Another builder standing there could start it and the first could then
cancel, so a build began far from the builder who placed it. That is
strongest with a captured unit or a monarch, since a monarch helps any
frame of its own player, so a captured builder at home could raise its
side's buildings beside a monarch anywhere on the map (#477).

A lodestone site could also refuse a player for no reason they could see.
The computer ordered a lodestone at a site, the player got there first
and could not build, and later an enemy builder walked over and built it
(#478).

## What the original does

A build order has no frame. The builder walks first, its route ending
within its `builddistance` of the site, and the order is dropped with
UNITCHATTER_CANNOTREACHCONSTRUCT when it cannot get that near
(legacy:12063-12070). Only then does it test the site (legacy:12088) and
create the frame (legacy:12142), hand it to the builder and start work
(legacy:12147-12171). So:

- Range: nothing goes up until the builder has arrived.
- Who may start it: only the builder holding the order. Help is a
  separate order on a frame that exists (legacy:233556-233574), and a site
  with no frame has nothing to help.
- Who is kept off: nobody. A site nobody has begun holds no cell. Whoever
  gets there first raises it, and a later builder finds it blocked and
  its order ends (legacy:12096-12116).
- Units on the site: the first test counts every unit and the second only
  what cannot move. With only movers in the way the builder looks again 10
  frames later, up to 30 times (legacy:12088-12124).

An interrupted frame stays where it stands. It waits 330 frames and then
gives back half a build unit a frame until it is gone
(legacy:9629-9657, 39531-39534). At a few percent it draws next to
nothing, since the silhouette's alpha follows how far the build has got
(legacy:197470-197489), so an interrupted lodestone can look like an
empty site for those eleven seconds. That is the original's and it is
kept, and the engine's ten second grace from the last work is close to it
(M-009).

## What the engine did

`Units_OrderBuild` spawned the frame when the order was given, at any
distance, with 1 hit point and an alpha of zero (M-009 as it stood). So a
frame stood with no builder near it, any builder allowed by
`Units_CanHelpBuild` could feed it, and it held its cells against everyone
while its own builder walked. A computer player's order did the same, so
its unseen, invisible frame refused the player who got there first.

## What it does now

A walking builder's order puts nothing on the ground, and the frame goes
up once the builder is in reach and clear of the site's cells. The order
is judged when given on what the builder's side knows, and for real on
arrival. The computer player goes through the same order, a mission
script's build step already did, and a factory still trains in its yard. A
structure that cannot walk takes no building beyond its reach. The held
order, its def, facing and looks are in the save and the state hash.

A summons used to go up at any distance once its spot cleared. It waits
for the builder too.

## Ours

The computer player used to rely on the frame to reserve a site and to
count a build under way. It now counts a builder on its way as that frame,
and its site searches pass over a site one of its own builders is walking
to and keep its tower spacing from one. A later builder of the same player
that finds a frame of the same building on its site joins it, where the
original's reports the target blocked. A builder standing on its own site
walks off to its goal before it looks, and one still on it there drops the
order. The Shift overlay draws the building in hand as a ghost until its
frame is up.
