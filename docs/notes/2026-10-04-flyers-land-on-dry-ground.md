# Flyers land on dry ground (2026-10-04)

The owner found that flyers could land on water. A dragon stopped or
left idle over the sea came down onto the sea floor and stood there 40
under the water on Two Castles, and every view drew it there, because
the views draw the height the simulation gives them. The original never
lands a flyer on water. The air traffic change (see
2026-10-04-air-traffic.md) brought in the original's landing search and
a landing rule that refuses water. This note records the rest of the
original's landing test and mission, which the engine now follows too.

## When a flyer lands

Stop resets the three weapons, and a unit with `canfly` that is in the
air then gets the land-if-can mission at its own position
(legacy:8855-8906). A flyer with nothing to do gets there through its
standby mission. With nothing in reach it looks for a target, and when
it finds none an airborne flyer gets the land-if-can mission at its
standby point. When that mission cannot be made it tries again 7 to 13
frames later (legacy:24427-24600).

An enemy in reach hands the flyer to the attack, and it does not land
(legacy:24257, 24392-24419). Stage 0 keeps the stop point as home and
picks a random bearing (legacy:24260-24275). Stage 1 flies on 32 px
ahead while the unit still holds more than a tenth of its top speed
(legacy:24276-24295, 184890-184921). Stage 2 tests the spot under it,
and on a failure searches 12 random spots out to 240 px and circles 160
px round home a third of a turn at a time (legacy:24296-24382).

## The landing test

The test is legacy:220068-220183. A footprint that leaves the map, or
reaches its last row or column, is refused. Ground the unit's owner has
never explored passes untested (legacy:220095-220102). The fog cell it
reads is offset from the footprint's corner by a quarter of the
footprint's width on both axes. Otherwise each footprint cell is refused
for a blocking feature, a blocked cell mark, or another unit on it. It
is also refused when its lowest corner is under the water floor, its
highest corner is over sea level less `minwaterdepth`, or its corners
spread further than `maxslope`. The water floor is sea level less
`maxwaterdepth`, raised to sea level for a unit with `canfly` and
without `amphibious` (legacy:220103-220109). That raise is what keeps a
flyer with the default depth of 10000 on dry ground.

The keys come from the movement class when the unit names one, and from
its own file otherwise (legacy:163184-163206), with defaults of 10000,
-10000 and 255 (legacy:187359-187380). `amphibious` is parsed at
legacy:162997. There is no `canland` key. In the Iron Plague data no
flyer is amphibious and none floats, so no flyer in the shipped games
ever lands in water.

## What happens

A flyer stopped near a shore finds dry ground within reach and lands
there. Over open sea it circles the stop point and stays in the air,
still a target for weapons that reach flyers. The spot is tested once,
when the descent starts, and heights never change in a battle, so a
spot cannot turn to water under a landing flyer.

## In the engine

`flyer_spot_clear` in units.c is the whole test. A stopped flyer keeps
its speed, and an idle one still faster than a tenth of its top speed
flies a glide leg 32 px ahead before it looks. A landing leg, a glide, a
spot or a circle, ends when the flyer gets an order or a target, and the
search starts over once it is idle again. Engine build 32.

## Cruise height over water

The original's cruise target is `cruisealt` over a ceiling, the highest
of sea level and the cell heights in the 128 px buckets round the unit
(legacy:190499-190507, 241336-241344, 224299-224390). So over water a
flyer is always at least `cruisealt` over the surface. The engine held
a flyer `cruisealt` over the ground under it, the sea floor over water,
so over deep water a hovering flyer such as the ghost ship sat under the
surface.

A flyer, and a hoverer, now stands on the higher of the ground and the
sea, plus its altitude. `unit_base_height` gives the simulation that
height, for the shots it fires and the shots aimed at it, and
`Units_DrawnAlt` gives every view the same height over the ground under
it. Over land the engine still follows the ground under the unit rather
than the bucket ceiling, which is a difference only over hills. Engine
build 33.
