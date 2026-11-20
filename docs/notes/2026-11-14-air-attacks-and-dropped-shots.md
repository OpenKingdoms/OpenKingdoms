# Air attacks and dropped shots (2026-11-14)

The 11-13 playtest found the Dirigible hovering in front of its target
while its stones crawled along the ground, the Iron Beak's eggs just as
slow, Parrots stopping at the corners of a patrol and Musketeers
stuttering as they fired. This note records what the original does and
what the engine now does.

## Two air attacks

A flyer's attack on something on the ground takes one of two missions.
One with `hoverattack` holds a point round its target, which
2026-10-10-hover-attack.md describes. One without it attacks in runs
(legacy:28491-29118). A target that can fly, or a weapon in hand with
the `dropped` key, sends both elsewhere (legacy:28524-28545,
legacy:30040-30052).

The run goes round these stages.

- It takes off, turns its weapons off and sets them on the target or
  the ground point, after which they fire on their own whenever they
  reach (legacy:28642-28684).
- It flies at the target's point. The move ends within half its
  `attackrunlength`, or half its weapon's range when it has none, and
  every 15 frames the flyer makes again for where the target is now,
  or steps out (rand(10) + rand(10) + rand(10) + footprintx) * 16 px when
  its crowd score is 5 or more (legacy:28685-28716, legacy:29039-29117).
- It flies on past the target to a point the run length and 128 +
  rand(128) px beyond it, or three times the weapon's range when it has
  no run length, until within 128 + rand(128) px of it. On its first
  pass, a flyer still further than half its range from the target picks
  that point again 15 frames later (legacy:28717-28790).
- It turns a quarter turn to one side at random and flies the weapon's
  range that way, or rand(128) plus half the run length, until within
  64 + rand(128) px (legacy:28791-28853).
- It comes round at the target again (legacy:28854-28924).

The armed flyers without `hoverattack` are the Dirigible, the Gryphon,
the Fallen Angel, the Aerial Juggernaut and the Wrath of Tammuz. The
Dirigible, the Gryphon and the Wrath of Tammuz, whose Hurricane reaches
500 px, have no run length and fly runs three times their range long.
The Fallen Angel and the Aerial Juggernaut carry run lengths of 400 and
200. The original chooses between the two attacks on `hoverattack`
alone (legacy:28524-28545). The Iron Beak, the dragons and the Harpy
have it and hover.

The engine had no runs. A flyer without `hoverattack` closed to its
range and stood there firing, which is what the playtest saw.
`run_attack_tick` in `src/render/units.c` now flies the stages above,
keeping its stage in `hover_state` (UNIT_RUN_IN, UNIT_RUN_PAST,
UNIT_RUN_TURN), its passes in `hover_steps`, its leg's reach draw in
`hover_events` and its point in `hover_x` and `hover_y`, so a save and
the state hash carry it with no new field.

Ours, and left out (D-049):

- The original hands a target that fails a reach test on the path to
  a formation move. The engine flies its run at it from the start.
- The original sends a flyer near the map's edge 800 px back in. The
  engine keeps the run's points its turning circle and 32 px inside the
  map, maxvelocity over turnrate, 156 px for a Dirigible, so it turns
  before it reaches the edge.
- The engine's walk slows a flyer whose heading is far off its way,
  where the original's flyer mover keeps its speed and swings round.
  A Dirigible, which turns 100 a frame, slows almost to a stop at the
  quarter turn.
- A flyer without `hoverattack` attacking a flyer still stands off it
  and fires, as before.

## Dropped shots

`type = Ballistic` with `subtype = Dropped` is its own behaviour object
(legacy:249737-249746). It leaves level and nose down, and its speed
across the ground is the run to its aim over the time its fall takes,
so it lands on its aim from wherever it was dropped
(legacy:246799-246829). Its weaponvelocity, 15 on the Dirigible and 10
on the Iron Beak, plays no part. The engine flew these shots flat along
the ground at weaponvelocity, a quarter of a pixel a tick, so a stone
took about 15 seconds to cross 200 px and most never arrived. They now
fall under the original's gravity, set to land on the aim, and meet
what they fly into like any other shell. A Dirigible's stone from
200 px up lands in just under two seconds.

## Patrol legs

The original's flyer on patrol flies legs that end within 128 px of the
point each makes for (legacy:26275), so it never brakes onto a point.
Its legs go round the order point at its weapon's range plus 160 px, a
shape the engine does not copy (D-049). The engine walked a flyer to
within 8 px of each patrol point, braking to a stop there and turning
round from a standstill. A flyer on patrol now takes its next leg once
within 128 px of its point, at speed, unless the next point is as near.
A Parrot on a 500 px patrol stopped seven times in 30 seconds and now
never drops under 60 percent of its top speed. I found no change to
the flight code since September that made flyers less nimble. The stop
at each point was there all along.

## Untextured faces

Each side names a palette in gamedata/sidedata.tdf, read with its shade
tables at legacy:164721-164729, and the engine draws a model's textures
in it. All eight side palettes are the same colours. The engine drew a
model's untextured faces in the map's palette instead, so their colour
changed from map to map. Colour 172 is (39,19,0) in the side palette,
(24,66,16) on an Aramon map and (189,156,132) on a Veruna one. The
Harpoon Ship's harpoon is mostly such faces, so on Veruna maps it read
as gold. Every untextured face now takes its side's palette, the same
one its textures use, and a model with no side the first side's. That
the original's own blit draws these faces in that palette is our
reading, not proven: textures and faces share its 8 bit unit buffer.

## A shot's animation

D-042 lets an aim that never answers fire after two seconds. Musketeers,
grenadiers and trebuchets hold AimWeapon while their FireWeapon plays,
the shot and the reload together, so the exception fired them again
while the last shot's animation still ran and two animations fought
over the same pieces. The exception now waits for the weapon's
FireWeapon to end, as the original's fire gate always waits for the
aim (legacy:249328-249338), and does not ask the script again in the
meantime. A Musketeer fires every 7.06 seconds, the
length of its script's shot and reload, where its file says 6.2 and it
stuttered on every second shot.
