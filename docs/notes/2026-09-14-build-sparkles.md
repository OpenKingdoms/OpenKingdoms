# Build sparkles (2026-09-14)

What the original does with the sparkles that play on a building
while it is built, and on a raiser and a body while a unit is raised.
Line anchors are `:NNNNN` into the legacy reference.

## The ring

Every unit's render record carries a particle ring, made when the
record is (:198530-198576). The ring is sized from the model's box in
the unit type, not from the footprint. Its radius is half the box's
diagonal in the x and z plane, whole pixels, from half extents taken
as max minus min over two (:198540-198551). When bit 0x2000000 of the
unit's flag word is set the radius is the larger of the two half
extents instead of the diagonal (:198547-198555). We could not find
where that bit is set, so ours always takes the diagonal. The ring's
height is the box's top minus its bottom (:198573-198576).

The ring holds as many particles as its radius in whole pixels
(:198576, the count argument at :201382-201419), except that a unit
whose `bmcode` is 1 holds a quarter of that (:198557-198560). The
quarter applies to the count only, the radius the particles stand on
stays the full half diagonal. Every mobile unit ships with `bmcode`
1, so a raiser's ring is as wide as its model but holds few. A debug
speed setting divides the count further and clamps it to at least 1
(:198561-198571), which ours ignores. Adding particles to a
full ring adds nothing, the request is cut to the free slots and there
are none (:201434-201436). Nothing is replaced. The flow continues
because each particle ends at its own time, see below.

## Emission

Each 30 Hz frame in which the builder's construction step did work,
the site's ring is asked for two particles, one with flag 0 and then
one with flag 1 (:12245-12262, again at :12526-12540 and
:13515-13526). The wrapper adds exactly one particle a call and takes
the side's build sparkle animation when none is named
(:198992-199002). A raise asks the raiser's ring for one and the
body's ring for one a frame (:13129-13133). The body's ring is made
from the feature's data, its height a byte in the feature type and its
count a quarter of its radius (:128232-128239). The radius comes from
a float the legacy reference does not show, so ours keeps the
footprint for the body's ring.

Ours runs 60 Hz ticks, so it adds one particle a tick and alternates
the two flags, the same rate a second.

## Each particle

Two random draws a particle (:201441-201455). The first is the angle,
a 15 bit random doubled into a 16 bit turn, and the position on the
ring comes from sine and cosine of it. The second is the speed: the
random doubled and quartered into a 16.16 value and offset by 2.0, so
each particle moves between 2.0 and 4.0 pixels a frame and every
particle has a speed of its own. Ours halves that a tick.

The flag decides the direction (:201355-201374). Flag 0 starts the
particle at the ring's height, the top of the model, moving down, with
a limit of 0. Flag 1 starts it on the ground moving up, with a limit
of the unit's own world height plus the ring's height. That limit
adds an absolute height to a relative one, so on ground 200 px high a
riser climbs 200 px past the top of the model, and at a hilltop site
the risers reach half the screen. That is what the original does, and
ours does the same, since our world y is the terrain height. If the
owner prefers a riser that stops at the model's top, the riser's stop
height in `spawn_ring_sparkle` is the one line to change.

Each frame the particle is checked before it moves: a faller ends once
its height is below 1, a riser once it is above its limit
(:201340-201351). Otherwise it moves by its speed and steps its
animation. The animation runs at the GAF's own frame delay and loops
while the entry's loop flag is set (:255756-255794), which it is for
every build sparkle in the data, so a particle plays its pictures
round and round until its height ends it. There is no fade and no
lifetime in frames. Ours hard codes the loop and two frames a picture,
which is what every shipped sparkle sheet carries.

Particles with a negative z draw before the unit's model and the rest
after it, each at the unit's position plus its own, projected with the
usual half height (:201487-201543).

## What ours did before

Every sparkle rose from the ground at one speed and died when its
pictures ran out, the ring was sized from the footprint, and the
emitter added one sparkle a piece of the model a tick. The ring filled
in a few ticks, everything died together, and the ring refilled in a
burst, which read as a rising circle with pauses.
