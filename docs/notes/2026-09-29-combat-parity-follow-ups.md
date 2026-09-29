# Combat parity follow-ups (2026-09-29)

Four rules #334 left out, each now the original's, with the computer's
stance and the roam stance they needed. The `legacy:` anchors are where
a maintainer re-checks the claim.

## Veterans hit harder and take less

The original multiplies a unit's attack and its armour each by 1 + 0.1
per veteran level, with the level capped at 10 (legacy:232971-232984,
legacy:235826-235862). A hit is the weapon's figure times the attacker's
attack and divided by the victim's armour, so a level 5 swordsman strikes
for 150 percent and takes two thirds of each blow. The engine already
kept the level, as experience over `experiencepoints` with `noveteran`
units never ranking. It now scales every hit by both levels. The sum is
done in tenths in 64 bit integers, so no float enters the simulation.
The level comes from experience, which is already hashed and saved, so
there is no new state.

## A unit is born with its standing order

`standingunitorder` in the unit file is the stance a unit starts with
(legacy:162926-162947). 1 is hold position, which the archers carry, and
2 is maneuver, which the melee units carry. The numbers are the same as
the stance the engine keeps and a mission's `o N` sets: 1 is defensive
and 2 offensive, with 0 passive. A unit whose file does not say starts
offensive, as every unit did before. A building takes its stance when it
is finished.

Holding position now holds. A defensive unit looked only as far as its
weapon reached, but once it had a target it walked after it. It now lets
go of a target it picked for itself once that is out of reach, and looks
again, as a unit that cannot move already did (D-025). Its search takes
only what it reaches, and it answers only a shooter it reaches. The
leash is `maneuverleashlength`, the maneuver stance's, so a unit holding
position has none. An attack its player orders is followed as before.

Maneuver is the offensive stance, which chases what it takes on. The
engine reads the leash for return fire and for a search during a fight. Whether the original also
turns a maneuvering unit back when a chase passes the leash is not
established here, so a unit on maneuver still chases until its target
dies or is lost.

## Melee is a weapon's type

The original treats a weapon as melee when its `type` is Melee
(legacy:249726). The engine guessed from the weapon's name and from a
reach over 64 with no speed. The goblin's Dead Guy Bone, reach 50, was
taken for a thrown shot. The type alone decides now.

## A fight a unit took on for itself looks again

The attack handler waits between its checks. With the target more than a
footprint and a half away, which is every ranged fight, the wait is
rand(5) + rand(5) + 4 of the original's frames, 4 to 12
(legacy:11485-11509, the dispatcher at legacy:182148-182175). At the end
of each wait a unit that is not holding position (legacy:11517), is on
fire at will (legacy:182080) and is in an attack it took on for itself,
on maneuver or as return fire, runs the standard target search again
(legacy:182079-182083, then legacy:21060) on half its checks. It draws
rand(2) before the search and searches only on 0 (legacy:11517), then
draws rand(10) once the search has found a target and takes it only on 0
(legacy:11519) and only inside its leash (legacy:11520). So about 1
check in 20 takes what the search finds. A random picker among three
targets switches on about 1 check in 30, and one that takes the nearest
keeps its target unless another is nearer. `fireatwillrandom` has no
part in when this happens. It only changes how the search scores what it
finds (legacy:21170), so a random picker draws and any other unit takes
the nearest. An attack order is never searched again.

The handler returns at once for a unit with no mover or with `canfly`
(legacy:11351-11355), so towers and flyers never look again this way,
even the dragons that are born on maneuver. A melee chase has its own
search with its own gate and wait (legacy:11189-11195). That is not
ported yet, so a melee unit does not look again this way either, as
before this change.

The engine keeps the wait on the unit in 60 Hz ticks, 8 to 24, and draws
it from the simulation's generator in two statements, in the same order
on every machine. The two gate draws come from the same generator, in
the original's order: the draw of 2, the search, then the draw of 10,
and then the two draws of the next wait. The generator's state is saved
and hashed, so a replay and a reload take the same targets. The wait is
saved and hashed too. Offensive is the only engine stance that is both
on fire at will and not holding position. A
patrol's pick counts as a fight the unit took on for itself. The wait
with the target nearer than a footprint and a half is not established
here, so the engine uses the same wait there.

The archers carry `standingunitorder = 1` and are born holding position,
so on their own they never look again. A maneuvering ranged unit without
`fireatwillrandom` now does.

## The computer's army goes out offensive

The original's AI sets every unit it commits to a fight to the offensive
stance (legacy:18889, legacy:18905). Ours now does the same when it
launches an attack group, when it splits off a raid, when it sends a
member at a group's target and when a unit answers a hit on the base.
Without it the computer's archers, priests, mages, catapults and monarch
would hold position all game. A unit it leaves at home keeps the stance
it was born with.

## A unit whose file gives no standing order roams

Without `standingunitorder` a unit starts offensive and moves by
`standingmoveorder`, 2, roam, when the file does not say
(legacy:162930-162934). Roam has no leash (legacy:13733-13737), so such a
unit answers a shooter and takes a new target at any distance. Any other
`standingmoveorder` keeps the maneuver leash here. What its other values
mean is not established here.
