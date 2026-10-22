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
- 0x109, steam, the `steam` TAF, a puff like the smoke
  (legacy:161416-161417, legacy:215024-215028).
- 0x107 plays `purpledeath` from deathmagic.gaf and 0x108 the
  `pillaroflight` TAF once where the piece stands, among the one-shot
  animations (legacy:223651-223663, legacy:161490-161496, :33163). Kirenna
  plays 0x107 as she wades in or out, and the Pillar of Light 0x108.
- 0x103, bubbles, calls a routine that does nothing (legacy:223636-223640,
  :214990), so nothing is drawn.
- 0 to 5 are wakes and thrust. Shipped scripts use 2 to 5, and type 2 is
  the wake 97 walkers leave in water. They are their own particle lists
  of up to 100 a unit, drawn as points whose sizes come from a table the
  reference does not carry (legacy:214923-214988, :201685-201760,
  :215189-215230).

The flame sizes come from every TDF in gamedata/damageflames. Each
`smallflame`, `mediumflame` or `largeflame` section adds its `gaf` and
`anim` to that size's list (legacy:202524-202640), and a new flame takes
one from the list at random (legacy:202459-202461). The shipped folder
holds one file with one section for each size, all in flames.gaf.

Flames and smoke are kept per unit, at most 40 flames (legacy:215116) and
10 puffs (legacy:214686), and a request past that makes nothing. A puff
rises 0x1fdb*4 a frame and drifts 8 times the wind's parts a frame. It
holds its first picture 8 frames and each after a random 4 to 7, and it
is gone on reaching a picture drawn at random from the third to the third
last (legacy:201847-201869, 201941-201966). All of this randomness is the
C library's, never the simulation's.

The scripts decide everything else. ARAKEEP waits for construction to
end, burns nowhere at 29 percent health and above, has one level of flame
at 28 and sixteen at 10, spreads them over eight pieces at up to three
levels a piece, emits every lit piece and sleeps 499 ms. Its smoke starts
below 66 percent and comes more often the lower the health. Thirty-three
units carry a SmokeControl, mobile ones such as ARAWAR and VERTRANS too.

## What we draw

The unit host works out the node's world position and starts an
animation in the shared effect pool, which both views and the embed
already draw under their fog rules. It does so only for a unit the local
player can see, by the same test the unit itself is drawn with. A flame
follows its node until its animation ends or its unit is gone, matched by
stable id so a reused unit slot drops it, and one pose of a unit serves
all of its flames in a run of the pool. A unit keeps 40 flames and 10
puffs, counted by its stable id, and its puffs go with it as its flames
do. A puff plays its pictures by the rules above, half a frame's rise and
drift each tick. The flame anims come from every TDF in the folder, the
section names matched exactly as the original matches them, and a flame
draws its anim from its size's list. The shared pool holds 2048 effects.

The effects draw from a C library generator of their own, which the
simulation never reads, so a peer that sees more or less of a battle
stays in step. A unit under construction emits nothing. No effect reads
the simulation's random numbers or changes simulation state, and a
request a unit's count cannot take is dropped without touching the
script.

## The script's pace

The VM stopped a thread after 200 instructions and finished it on the
next tick. The original runs a thread until it sleeps, waits or ends
(legacy:306252-307002). ARAKEEP's flame loop needs more than 200 to reach
its sleep, so it came round every 31 ticks instead of 30. The limit is
now 16384, a guard against a runaway script only. That changes where a
thread stands at the end of a tick, which the simulation hash covers, so
the engine moved to build 22.

## Departures

- The wakes and thrust of types 0 to 5 are not drawn. Their point sizes
  sit in a table the reference text does not carry.

## Tests

test_cob_vm checks the callback's arguments, a piece name matched without
regard to case, a missing callback, a piece out of range and the stack
staying aligned. A batch longer than 200 instructions reaches its sleep in
one tick, and a script that never sleeps still yields.

test_view3d runs the retail keep: no flames at 40, 30 and 29 percent, one
level at 28, sixteen at 10, a batch every 30 ticks and none after repair.
It covers the binding after a save is restored, the wait for
construction, both smoke types, steam and their art, 40 flames and 10
puffs a unit with a second unit keeping its own, an enemy keep out of
sight emitting nothing, a puff's picture holds, its end, its drift and
its rise, a reused unit slot, that flames and smoke are not counted as a
ring's sparkles, that the simulation hash and random state do not move,
and that flames draw in both views.
