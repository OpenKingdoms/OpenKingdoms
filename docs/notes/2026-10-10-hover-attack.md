# Hover attack (2026-10-10)

Issue #401. After #396 a flock of Harpies sent onto one target no longer
ended as one stack, but it still bunched at the edge of its reach, so one
Water Ball on the bunch took several of them. The original's flyers with
`hoverattack = 1` do not attack like that. This note records the
original's hover attack and what the engine now does.

## The keys

A unit reads `hoverattack` into a flag, `hoverattackdistance` and
`hoverattackaltitude`, the last defaulting to `cruisealt`
(legacy:163005-163011). A weapon reads its own `hoverattackdistance` and
`hoverattackaltitude`, both 0 when unset (legacy:250022-250024). The
attack takes the weapon in hand's value and falls back to the unit's when
it is 0 (legacy:30150-30163). No shipped weapon sets either key. In the
base game's units the keys are on aradrag, tardrag, verdrag, zondrag,
zondrake, zonharp (200), zonhunt, zonflies, tarbeak, tarknigh, tarpries,
tarprie2 and tarship.

The attack goes another way when the weapon in hand has the `dropped`
key (legacy:250054-250055, tested at legacy:30051). That is a key of its
own, not `subtype = Dropped`, and no shipped weapon sets it.

## The mission

The attack is a mission with six stages, run once a frame. The mission
runner wakes a stage when its delay is up or when one of the events it
waits on comes in (legacy:182119-182192).

- Stage 0 runs BeginFlight, counts no step outs yet and takes the
  target's position (legacy:30164-30184).
- Stage 1 turns the weapons off. Within the hover distance plus 160 px
  of the target it goes straight on to stage 2. Otherwise it flies at
  the target, to within the hover distance plus 144, and looks again in
  15 frames or when that move ends (legacy:30185-30222).
- Stage 2 sets the weapon in hand on the target, or on the ground point
  (legacy:30223-30253). From here the weapons fire on their own as the
  flyer moves.
- Stage 3 picks the flyer's point. The bearing is the one from the
  target to the flyer, one time in a hundred turned by rand(2048) of a
  65536 turn either way. The distance is the flyer's own while that is
  within 16 px of the hover distance, one time in a hundred redrawn all
  the same, else the hover distance plus rand(8) - 3. The flyer flies
  to that point off the target, at its hover height, and stage 4 waits
  15 + rand(15) frames (legacy:30254-30378).
- Stage 4 wakes when the delay is up, when the flyer fires, when a shot
  of its lands without hurting the other side, or when it reaches its
  point. With a crowd score under 5 it goes back to stage 3 a frame
  later, except that after such a shot, one time in two, with a unit
  target that stands still, it goes to stage 5 (legacy:30616-30635).
- Stage 5 draws a bearing at random and flies to the hover distance
  plus rand(8) - 3 along it, and stage 3 runs again once it gets there
  (legacy:30381-30449).

A flyer holding its point reaches it again each frame it is picked, so it
picks again every other frame, and the one in a hundred turns walk it
slowly round the target. A flyer whose shot misses a target that
stands still, is stopped short or does it no harm flies round it to a
bearing drawn at random one time in two.

## Crowded

At stage 4 a flyer whose crowd score is 5 or more counts a step out. One
that took its own target may, one time in two, turn on another enemy and
start over. Otherwise it steps out (rand(10) + rand(10) + rand(10) +
footprint) * 16 px, 32 to 464 px for a Harpy against 32 to 256 for the
other missions' step out. The attack then ends if rand(5) + 1 + rand(5)
is under the count, else it goes back to stage 1 a frame later
(legacy:30636-30705). The first step out never ends it, and the chance
grows to 15 in 25 by the sixth.

## Events

Shots fired raise 0x10000, 0x20000 or 0x40000 on the shooter by weapon
kind (legacy:249395-249414). A shot that lands adds up the harm it did
to other players' units and to its own player's, and raises 0x1000000
when the first is more than twice the second, else one of 0x200000,
0x400000 or 0x800000 (legacy:15038-15060, called from legacy:245317 and
legacy:245370). A mind control hit on a unit it may take counts as harm
whether the roll takes it or not (legacy:247761-247798). A
move ending raises 0x100 on the mission (legacy:191274). Stage 3
waits on 0xe70768 and stage 5 and stage 1 on 0x768, which takes in the
move's end and leaves out shots. So a shot that hurts the enemy never
wakes stage 4, and one that hits the ground, is stopped by the
shooter's own side or does no harm does.

## What the engine does

`hover_attack_tick` in `src/render/units.c` runs the stages for a flyer
with `hoverattack` that has a target or an attack on the ground, once it
is in the air. A flyer with the flight pair that took its own target and
can already reach it still stays down and fires from there, as before.
The units with `canfly` and no BeginFlight hover from the start, so they
attack this way at once.

- Stage 1 drives the flyer at the target and, once it is within the hover
  distance plus 160 px, sets its weapons on and picks a point.
- The point is kept as an offset from a unit target, so the flyer
  follows it as the original's move does. It is reached within 8 px,
  where the walk stops for it, not the original's 4 (D-039).
  Between points the flyer is moving, and at its point it holds, faces
  the target and attacks.
- The weapon in hand fires whenever it is ready, its aim is done and the
  target is in reach, on the move as well as at the point.
- Firing, a shot landing without hurting the other side more than
  twice its own and reaching the point are kept as bits in
  `hover_events`, which stage 4 and stage 5 read and clear. Each shot's
  harm is added up by side as it lands, beams included.
- Stage 4's step out uses the hover attack's own distance and its give
  up. While the flyer is still beyond the hover distance plus 160 px the
  other missions' step out applies as before.
- The flyer climbs to its `hoverattackaltitude` while it holds points.
  D-026 still holds its fire until it reaches the height it is making
  for.

The stage, the frames to the next look, the step outs, the events and the
point are in the state hash and the unit record (version 12).

## Measured

Twelve real Harpies on Two Castles sent to attack one spot of open
ground, counted every half second from 20 s to 60 s
(`test_ui_screens harpies_attacking_one_spot_spread_round_it`):

| | before | after |
|---|---|---|
| others within 50 px of a Harpy, on average | 7.46 | 0.96 |
| attacking Harpies' distance from the spot, on average | 50 px | 192 px |
| most Harpies one Water Ball takes (50 px) | 10 | 3 |
| most Harpies one Water Blast takes (250 px) | 12 | 12 |
| still attacking at 60 s | 12 | 10 |

Before, ten of the twelve ended within a pixel of the spot, flying step
out legs they never finished. After, two had given the attack up. A Water
Blast reaches 250 px, so one cast on the spot still takes a ring 200 px
out, as it would in the original.

The data free cases in `test_movement` give twelve Harpies on a still
enemy 0.35 others within 50 px on average, 94% of the samples 170 to 240
px from it, over six of the eight octants round it. A Harpy on a ground
point from 600 px east holds 190 to 201 px east of it, and one following
a walker fires 15 of its 20 shots on the move.

## Left out

- The far approach. The original hands a target that fails a reach test
  on the unit's path to a formation move, which steps out of crowds by
  the common rule (legacy:30139, legacy:30467-30502,
  legacy:235678-235762). The engine
  takes the hover distance plus 160 px as that line (D-039).
- A flyer near the map's edge is sent 800 px back in by every VTOL
  mission (legacy:30054-30117). The engine keeps flyers on the map by
  clamping their points, as it does for the other air legs.
