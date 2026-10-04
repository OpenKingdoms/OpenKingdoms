# Damaged building flames and smoke

A building's own script decides when it burns. The retail scripts run a
DamageFlameControl and a SmokeControl loop that read the unit's health,
choose pieces and sizes, ask for an effect with EMIT-SFX and sleep. The
VM used to take EMIT-SFX's operands and do nothing with them. It now
maps the script piece to a model node through the existing name mapping
and hands the node and the effect type to the unit host, as the original
hands them to its host (legacy:306486-306490). A missing callback or an
unbound piece does nothing.

## What the original draws

The host draws an effect only for a unit the local player can see, its
own or one in sight (legacy:223583, 206797). For a building the types are:

- 0x101, white smoke, the `bigsmoke` animation (legacy:161422-161424),
  a truecolor TAF of 10 pictures that ships in V3Rocket.hpi.
- 0x102, black smoke, `smoke01` in anims/smoke.gaf (legacy:161419-161421).
- 0x104, 0x105 and 0x106, a small, medium or large flame
  (legacy:223642-223650).

The flame sizes come from every TDF in gamedata/damageflames. Each
`smallflame`, `mediumflame` or `largeflame` section adds its `gaf` and
`anim` to that size's list (legacy:202524-202640), and a new flame takes
one from the list at random (legacy:202459-202461). The shipped folder
holds one file with one section for each size, all in flames.gaf.

Flames and smoke are kept per unit, at most 40 flames (legacy:215116) and
10 puffs (legacy:214686). A puff drifts with the wind as it rises, holds
each picture a random 4 to 7 frames and ends on a random picture
(legacy:201846-201870, 201956-201962). All of this randomness is the C
library's, never the simulation's.

The scripts decide everything else. ARAKEEP waits for construction to
end, burns nowhere at 29 percent health and above, has one level of flame
at 28 and sixteen at 10, spreads them over eight pieces at up to three
levels a piece, emits every lit piece and sleeps 499 ms. Its smoke starts
below 66 percent and comes more often the lower the health. Thirty-three
units carry a SmokeControl, mobile ones such as ARAWAR and VERTRANS too.

## What we draw

The unit host works out the node's world position and starts a one-shot
animation in the shared effect pool, which both views and the embed
already draw under their fog rules. A flame follows its node until its
animation ends or its unit is gone, matched by stable id so a reused unit
slot drops it, and one pose of a unit serves all of its flames in a run
of the pool. A puff rises a quarter pixel a tick from where it started.
A unit under construction emits nothing. No effect reads the simulation's
random numbers or changes simulation state, and a request the budget
cannot take is dropped without touching the script.

## The script's pace

The VM stopped a thread after 200 instructions and finished it on the
next tick. The original runs a thread until it sleeps, waits or ends
(legacy:306252-307002). ARAKEEP's flame loop needs more than 200 to reach
its sleep, so it came round every 31 ticks instead of 30. The limit is
now 16384, a guard against a runaway script only. That changes where a
thread stands at the end of a tick, which the simulation hash covers, so
the engine moved to build 22.

## Departures

Left for #381:

- The pool holds at most 128 flames and 64 puffs across the map, where
  the original keeps 40 and 10 a unit, and a unit out of sight still
  emits. About five keeps at low health fill the flame budget.
- A puff does not drift with the wind, plays every picture at 4 ticks and
  starts at the first.
- Only damageflames.tdf is read, and each size uses the first anim its
  section names, which is the same for the shipped data.

## Tests

test_cob_vm checks the callback's arguments, a piece name matched without
regard to case, a missing callback, a piece out of range and the stack
staying aligned. A batch longer than 200 instructions reaches its sleep in
one tick, and a script that never sleeps still yields.

test_view3d runs the retail keep: no flames at 40, 30 and 29 percent, one
level at 28, sixteen at 10, a batch every 30 ticks and none after repair.
It covers the binding after a save is restored, the wait for
construction, both smoke types and their art, the budgets, a reused unit
slot, that flames and smoke are not counted as a ring's sparkles, that
the simulation hash and random state do not move, and that flames draw in
both views.
