# Wandering shots (2026-10-10)

Four weapons in the shipped data are `type = Wandering`: the Weather
Witch's Tornado (TARWITCH), the Fire Vortex (TARGOD), the Water Vortex
(VERGOD) and the Hurricane (ZONGOD). The engine had no such weapon. It
flew each one as a plain slow shot to its target, so the Tornado did
nothing the original's does. This note records what the original does
and what the engine now does.

## The keys

The weapon loader gives a Wandering weapon its own behaviour object
(legacy:249968-249979). That object reads `wanderstartart`,
`wanderloopart` and `wanderendart`, three animations from the anims
folder, with the loop art set to repeat and the other two to play once.
It reads `duration` and `variationtime` in seconds and keeps them in
frames, times 30, and `maxvariation` as a whole number
(legacy:249000-249044). Like every weapon it keeps `weaponvelocity`
in 1/65536 px a frame, times 65536/30, cut into as many substeps as it
takes to keep each at 16 px or less (legacy:249987-249989,
legacy:250426-250433). `builduptime` and `decaytime` are in the files
but nothing reads them.

The shipped numbers, as V3Rocket.hpi gives them over the base game:

| Weapon | Unit | weaponvelocity | duration | variationtime | maxvariation | areaofeffect | damage |
|---|---|---|---|---|---|---|---|
| Tornado | TARWITCH | 45 | 9 | 2 | 5 | 30 | 50 |
| Fire Vortex | TARGOD | 80 | 8 | 2 | 8 | 180 | 12500 |
| Water Vortex | VERGOD | 45 | 9 | 1.5 | 4 | 180 | 15000 |
| Hurricane | ZONGOD | 45 | 6 | 0.5 | 2 | 240 | 7000 |

The base game's gods carry smaller vortexes, 45 px a second with an
areaofeffect of 30. Every one of the twelve animations holds each
picture for two frames.

## Setting out

When the weapon fires, the shot seeds a stream of its own from the
frame number, the frame's high byte in the top of the seed and its low
byte in the second (legacy:249049-249055). It takes the way from its
caster to the point it was aimed at as a unit vector and sets out 32 px
along it, on the ground there (legacy:249059-249094). Its velocity is
weaponvelocity along that way. Two spreads are kept for its turns:
`maxvariation` times the south part of the way for the east velocity,
and times the east part for the south one. So a shot cast due east
drifts only north and south, and one cast on a diagonal both ways. It
starts on the next frame, or later by `nimbustime` when the weapon has
`nimbus` and the caster's side has nimbus art, and no shipped weapon
has a nimbustime.

## The frames

Each frame the shot does one of four things (legacy:249099-249174).

Before its first frame it waits on its caster, and if the caster is
dead or dying by then the shot is gone with it (legacy:249103-249111).

On its first frame it shows its start art. While that plays it stays
where it is and hurts nothing. A picture holds the frames its file
gives it, then the next follows, and the start and end art end after
their last picture (legacy:255756-255795).

When the start art ends its run begins (legacy:248966-248981). The loop
art shows, the run's length and the frames to its first turn count
from that frame, and it turns. A turn is the velocity it set out with
plus a draw from each spread, centred, east first: the stream's next
number scaled to twice the spread, less the spread, in px a frame
(legacy:248972-248980). The stream is the minimal standard generator,
kept unsigned (legacy:248985-248996).

On each frame of the run it moves by its velocity once for each
substep, which with more than one substep moves it by their square, and
no shipped weapon has more than one (legacy:249141-249147). When the
frames to its turn run out it turns again, and the next turn is
`variationtime` later (legacy:249148-249156). Then it bursts where it
is, the same blast a shot makes when it lands, called with no unit
struck and without its explosion art (legacy:249158, the blast at
legacy:244963-245050). That plays its hit sound, or its water sound
over the sea, shakes the view when the weapon shakes, and deals the
splash: every unit whose model is within half the areaofeffect, but
its caster, takes its share by the falloff. Then the scenery within
that reach takes the weapon's damage, unless the weapon is `unitsonly`,
and none of the four is (legacy:245236-245302). Where the map lets
shots through the sea, a burst over the sea bed does nothing
(legacy:244975-244984). So the Tornado deals up to 50 on each of 270
frames to whatever it passes, and fells the trees in its way.

When the run's frames are spent it shows its end art and drifts on
with its last velocity, hurting nothing, until that art ends, and is
gone. Nothing else ends it: it never meets the ground, a ridge or a
unit on its way, and it runs on after its caster falls.

## The engine

`wander_launch` and `wander_frame` in `src/render/units.c` do the above
on the engine's frame ticks, the even ones, in 16.16 as the original
does. The phases and the picture each art is at are on the projectile,
and the views draw the art of the phase at that picture, so the start,
the loop and the end show as they do in the original. Each burst goes
through the blast hook, with the firing unit's def and weapon slot and
no unit struck. Under the remastered rules (D-036) the bursts break the
scenery any blast breaks under them, rocks among it.

The state is in the hash and the save, the art by name since the art
slots are numbered in first use order. Engine build 38.

## Not done

The original also plays its side's nimbus art on the caster when a
`nimbus` weapon fires. The engine draws no nimbus for any weapon.
