# Spells draw from their data

Reported from play: the fire wagon's flame came out as lightning, and
Elsin's third spell looked nothing like the original. The engine side
is `src/render/units.c`, with the pictures drawn by the effect passes
of both views.

## What the data says

Every weapon block sits inline in its unit's FBI. Three families were
drawn wrong or not at all, and each is keyed by fields the original
reads.

A flame is a Line of Sight weapon with `subtype = fire` (the game data
also names `bluefire` and `dieselflame`). Its block carries only
`emittime` and `range`: no art, no colours. The original's flame
behaviour reads `emittime` and nothing else, then emits one particle a
tick from the muzzle toward the strike point with its velocity
jittered a tenth either way. Every dragon, the Taros knight, mage and
spout and the Zhon drake breathe this way. The engine had been drawing
these with the lightning ray.

A Remote Effect spell with `radiusart0..2` lays rings: `ringcount`
rings, one every `ringdelay` seconds after `builduptime`, each made
of `spritecount` copies of that ring's radiusart sprite spreading out
to `areaofeffect` over `ringduration`. Earthen Wave, Earthquake, Ring
and Wave of Fire, Tsunami, Water Blast, Wind Wave, Shockring, Death
Aura and Area Mind Control are all this one mechanism with their own
art (`ring_fx_red`, `ring_fx_white`, `ring_fx_blue`, `ring_fx_green`,
`ring_fx_purple`, `TsunamiExplode`). The engine had been drawing a
bright disc flying to the target.

A Remote Effect spell with `subtype = hailstorm` rains its
`weaponart` at `particlespersecond` for `duration` seconds over
`areaofeffect`, each drop bursting with the weapon's
`explosionclass`. Hail Shower, Fire Storm and Ice Storm are this one
with `iceballspin` or `meteor`. The engine had been flying a single
sprite to the target.

## What the engine does now

The weapon parser keeps the ring and rain keys and resolves their art
slots once. When such a spell fires, its shot still flies and lands
exactly as before, so the simulation and its hash are untouched, but
the shot is marked hidden and the spell's own pictures are laid where
it lands as effects: ring sprites with an outward velocity and a start
delay, or drops that fall from four hundred pixels up and burst on
landing. A flame's beam still deals its damage at fire and holds for
`emittime`, but it sheds a `flame` particle a tick along its line
instead of drawing a ray. Effects gained a ground velocity, a start
delay and a landing burst; both views skip an effect that is still
waiting, a hidden shot and a flame beam's ray.

Nothing is keyed by unit name. A weapon gets the picture its own
fields describe.

Each flame particle starts where the ray leaves, the piece the weapon's
QueryWeapon names (a dragon's head), and runs down to the height the
ray stopped at. It used to start 12 px over the ground under the head.
It never starts below those 12 px. Eight units breathe fire. The six
flying ones start 158 to 219 px up at the head. The TARSPOUT piece
sits 7 px up. The TARMAGE QueryWeapon names its staff piece, whose
origin is authored at the mage's feet. Both of those keep the 12 px.
The shot keeps the height it left at as drawing only data, out of the
hash and the save. A beam loaded from a save has no such record and
starts its flames at the old 12 px.

## How it is checked

Three cases in `test_view3d`, each picking its weapon from the data
rather than by name: a flame weapon fires at the ground and after
twelve ticks holds one flame beam and at least six flame particles; a
ring spell lays exactly `ringcount` times `spritecount` sprites of its
own radiusart, moving outward from the strike point, with its shot
hidden; a storm queues `particlespersecond` times `duration` drops of
its weaponart, each set to burst with its explosionclass. All three
failed against seams that returned nothing before the change.
