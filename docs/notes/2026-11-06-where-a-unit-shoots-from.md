# Where a unit shoots from (2026-11-06)

Playtesters reported that units stand shooting at a standing stone, that
enemies ignore what walks right past them, that crossbowmen jitter and
that Swamp Beasts are no faster in water. All four come down to rules
of the original's attack and mover that were missing or misread.

## A shot tells its shooter how it went

When a shot ends, the original compares what it did to enemies and to
its own side (legacy:245317, legacy:245370). More than twice as much to
enemies sets one flag on the shooter (legacy:15046-15048). Anything else
sets a wasted flag for that weapon (legacy:15050-15059).

The ranged attack handler keeps a level from 0 to 5 in its mission
(legacy:11103-11131). A good shot may bring it down: on a draw of 2 it
loses up to its own value. A wasted shot with the target in reach takes
up to four draws to raise it, of 2, 3, 3 and 2, never past 4. The level
then says what the unit does (legacy:11198-11311).

| Level | What it does |
|---|---|
| 0, 1 | Walks only if the target is out of reach |
| 2 | Walks to within a range of about two thirds of its reach |
| 3 | Walks to within about an eighth of its reach |
| 4 | Goes to a point round the target, a sixth of a turn either side of where it stands, inside its reach, and the level becomes 5 |
| 5 | Waits for that walk to end, and the level becomes 4 |

Levels 2 and 3 climb when the unit already stands that close, on a draw
of 3 that is not 0. A shot that ends during a walk is not judged until
the walk is over (legacy:11214), and a new order starts the level over
(legacy:11091-11093). Its weapon stays on its target through all of
it and fires when the walk happens to face it.

The engine keeps the level, the shots counted for D-025 and a walking
bit in the unit's `blocked_shots`, and the point in `cmd_x`, `cmd_y`,
which an attack does not otherwise read. A patrol's are its route, so a
patrol stops at level 4. The radii are the middle of the original's
draws rather than the draws, which leaves nothing more to save.

Two things differ. Only a shot that stops short counts as wasted here,
where the original also counts a plain miss. And a walker lets go after
six short shots of a target it took on for itself, which is D-025. A
player's own order is never let go, as before, and a unit holding
position with a target it took on for itself does not walk, as the
original's does not run this handler for it.

## Looking again

The two attack handlers each have their own rule, and only while the
target is out of reach. The blade's is the one the follow-ups note had
down for every ranged unit: at the end of a wait, a draw of 2 before the
search and a draw of 10 for a different target inside the leash, so
about one check in twenty (legacy:11517-11520). A ranged unit with its
target out of reach leaves level 0 on a draw of 2 (legacy:11109-11113),
and at each wasted shot or wait it searches unless a draw of its level
plus one comes up 0 (legacy:11189-11195). With its target in reach and
its shots landing it does not look at all.

The computer's orders count as a fight of its own here. Its groups go
by move orders and take their targets by search (legacy:18487-18530), so
its army no longer walks past everything on the way to the one target
the engine's planner sent it at.

The search also takes nothing the weapon cannot hurt
(legacy:21165-21167). The nearest search did not ask, so a wolf beside a
lodestone, which its table gives 0, stood at the lodestone for good.

## The ground a mover stands on

The mover keeps two flags, on a road and under the sea, set every frame
(legacy:185197-185206). A road takes every cell of the footprint on the
map's road mark, 0xFFFB in the feature layer (legacy:220207-220234,
legacy:225027), and a unit is under the sea when its own height is lower
than sea level, a floater's being sea level less its waterline. Maximum
speed, acceleration, braking and both turn rates are all scaled, by
`roadmultiplier` on a road, or else by `watermultiplier` under the sea
(legacy:183206-183224, legacy:183053-183160). `roadmultiplier` is 1.2
where the file does not say. Two Castles has 17 percent of its cells on
roads. The script ports for them were already answering IN_WATER, and
ON_ROAD now answers too, so a unit plays `walk_road`.

The Rictus does not cross water in the standard unit set. V3Rocket's
moveinfo gives HOVER3 a MaxWaterDepth of 0, and the cell test refuses a
cell with any corner under the sea (legacy:219149-219156). With "Use
Crusades Units" on, its file gives it HOVER2, which has no such limit,
and it does. Both are the data's and neither was changed. The extracted
data tree used for development resolves moveinfo to the base game's
copy, where HOVER3 has no limit, so a Rictus does cross there and only
there.

## TurnDirection and the reload

TurnDirection gets the turn still to make in degrees, when a turn starts,
ends or changes sides, and a turn under a degree is no turn
(legacy:183163-183172). The engine sent the sign of each tick's change,
so a unit tracking a walker sent it nearly every tick. Its bearing now
also reads the target's position below the pixel, as the original's 16.16
does, so a target walking past is a small steady turn and not a pixel's
jump every other tick. Most scripts walk the legs while it is not 0, and the crossbowman's `walk` poses the arms
too, so it stuttered through its aim and draw. Its rate of fire was never
short: 13.0 draws a minute against a 4.7 second reload in every duel and
group measured.

A script also hears its longest reload once, in milliseconds, and keeps
twice that as how long a pose holds after an aim (legacy:245659-245687).
The engine never told it, so the script's own default of 5 seconds
applied to every weapon, however long its reload.
