# Combat parity follow-ups (2026-09-29)

Four rules #334 left out, each now the original's. The `legacy:` anchors
are where a maintainer re-checks the claim.

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
engine reads the leash only for return fire. Whether the original also
turns a maneuvering unit back when a chase passes the leash is not
established here, so a unit on maneuver still chases until its target
dies or is lost.

## Melee is a weapon's type

The original treats a weapon as melee when its `type` is Melee
(legacy:249726). The engine guessed from the weapon's name and from a
reach over 64 with no speed. The goblin's Dead Guy Bone, reach 50, was
taken for a thrown shot. The type alone decides now.

## A random picker looks again during a fight

A unit with `fireatwillrandom` in a ranged fight it chose looks for a
new random target on about one in twenty of its attack mission's checks
(legacy:11516-11524). The engine takes the check to run once a frame, so
it checks on every second tick, the original's 30 Hz frame, and draws
the one in twenty from the simulation's generator before the search, in
the same order on every machine. A target it picks in place of the one
it held makes it lose a shot it was drawing, as any lost target does. An
attack its player ordered is never changed.
