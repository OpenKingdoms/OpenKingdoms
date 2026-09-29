# Archers against melee (2026-09-29)

The owner reported that archers seem too strong. Damage, hit points,
reload, range, speed and hit chance all match the original's numbers,
and a lone archer against a lone swordsman comes out within a few hit
points of what the original's rules give. The difference was in six
rules, all of which now follow the original.

## Melee closes until it can strike

The original's attack mission asks the melee behaviour on every step
whether the target is in reach and walks on while it is not
(legacy:246390-246428). The engine let a walker count itself arrived
within 8 px of its route's end. Against a target standing in depth
behind another, that end is a touching position planned for a 2 by 2
walker, and 8 px short of it the floor snapped cells leave one cell
between the two. The unit stood there in the walking state for as long
as the fight lasted.

A melee attacker now arrives only within 2 px, and when its route ends
short of the target it walks straight at the target while a new route
is found, as a formation move already did. An attack whose route
search gives up keeps its order and starts the search over, as the
original's attack keeps closing.

## Targets drawn at random

Units with `fireatwillrandom = 1` in their unit file (legacy:163097),
the archers, crossbowmen and a few others, draw their target at random
among the enemies in reach (legacy:21060-21251). Up to fifty candidates
are drawn from the simulation's generator. Each scores rand(m)/2 +
rand(m), where m is the largest int over the damage it would take, plus
a turn term for a unit that turns slower than 1000 a frame. The score
is ten times worse for one out of reach or unfinished, and the least
wins. Every other unit still takes the nearest.

## Melee looks as far as its weapon reaches

An idle unit searches out to the larger of its sight and its weapon's
raw range (legacy:21113-21118). Patch 3 (`V3Rocket.hpi`) set every
melee weapon's range to 300, so a swordsman with a sight of 135 takes
on anything within 300. Its reach in a fight is unchanged.

## Shots leave at the script's signal

A weapon's FireWeapon script starts the attack animation and charges
the reload, and the shot or blow leaves when the script sets port 23
(legacy:223397-223400, legacy:245900-245904). The Aramon archer's
attack sets it after 1.32 s of sleeps, the swordsman's after 0.51 to
0.65 s. Port 21 calls the shot off. A target lost during the draw takes
the shot with it: the script hears TargetCleared (legacy:233924-233940),
skips its release and the reload stays spent, and a late port 23
releases nothing. The shot leaves at the signal wherever the target
stands, and a blow strikes only a target still in reach
(legacy:246456-246468). A spell's mana is checked when the draw starts
and paid when the shot leaves (legacy:249341-249342,
legacy:249460-249461), so a caster whose target dies during the cast
keeps it. A script that never sets port 23 still fires the moment its
aim is ready, as before.

## The computer's archers

A-010 used to send the computer's archers to firing positions round
the side of their target. The original's computer never did, so that
half of A-010 is gone and its members go on at the target.

## Crusades Balance

"Use Crusades Units" in the battle menu now loads each unit from
`unitscb/` where the Crusades set has it (legacy:162511-162515). The set
ships in `V3Rocket.hpi` and gives the swordsman 3000 hp in place of
2500, among other changes. It is part of the units group of the data
fingerprint.

## Measured

Two Castles, open ground, Line of Sight off, both seats people.

| Fight | Before | After |
|---|---|---|
| 1 swordsman against 2 archers 40 px apart in depth | archers win, 1 left | swordsman wins with 343 hp |
| 8 idle swordsmen 260 px from 8 archers | archers win, 4 left | swordsmen win, all 8 alive |
| 8 archers choosing among 6 swordsmen | all on one target | four targets |
| first arrow after FireWeapon starts | same tick | 88 ticks, 1.47 s |

Veterancy (attack and armour times 1 + 0.1 per level, legacy:232971,
legacy:235826-235862) is still left out. It would make archers stronger.
