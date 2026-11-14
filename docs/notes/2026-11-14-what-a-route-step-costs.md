# What a route step costs (2026-11-14)

A playtester saw the computer walk all the way round a cliff entry on
Meredoc Keys rather than over a hill (#503). The route search charged a
step ten across open ground, fourteen on the diagonal, and twice the
height it climbed. Nothing in the original charges for height, so any
rise cost far more than the ground it covered and a route went round a
hill whenever it could.

## What the original charges

The original's search charges each step by direction and by the kind
of ground it steps onto (legacy:22187-22222). A straight step is 16 and
a diagonal 23 (legacy:23535-23543). Ground adds 8 on a road, 24 on open
ground and 48 on bad ground (legacy:22988-22990). A step that turns the
route costs more again for a slow turner (legacy:23003-23192).

Ground is bad when the footprint stands on a cell steeper than the
class's `BadSlope`, under water steeper than `BadWaterSlope`, or in
water outside `BadMinWaterDepth` and `BadMaxWaterDepth`, which keeps a
ship off the shallows (legacy:219720-219743), or when any cell in the ring round the
footprint is not good ground, so a cliff edge, deep water or a steep
cell beside it (legacy:219761-219791). A cell steeper than `MaxSlope`
is closed, as before.

On Meredoc Keys, routes across the southern island came out up to 44
percent longer than the same search with the original's charges, the
long way round its plateau instead of up a ramp and across.

## In the engine

The height term is gone. A step costs ten straight and fourteen
diagonal as before, plus three for each end of the step that stands on
bad ground, so six to cross a cell of it. Six over ten is the
original's 24 over 40 for a straight step onto bad ground, and charging
half at each end keeps a step the same cost both ways, which the long
route field needs. Bad ground is worked out once per map and class from
the heightmap and the sea, so every machine and a loaded world agree.

Ours, or not yet done: the road discount and the turn charge are not
ported, and features do not make the ground beside them bad, since
they change during a battle.
