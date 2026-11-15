# How a campaign mission is won and lost

Line numbers are anchors into the legacy reference, as in every note here.

## Two lists, not one

A mission file writes its conditions as plain keys inside
`[GlobalHeader]`, next to `numplayers` and `size`. There is no
conditions section and no nesting. The original reads those keys at
:239243-239850 and builds two separate lists.

The victory list, eleven keys, in the order they are read:

- `KillEnemyCommander` :239266
- `DestroyAllUnits` :239280
- `KillAllMobileUnits` :239294
- `BuildUnitType` :239310
- `CaptureUnitType` :239352
- `KillAllOfType` :239391
- `KillUnitType` :239432
- `MoveUnitToRadius` :239474
- `UnitTypePassesX` :239532
- `UnitTypePassesZ` :239590
- `VictoryTimerRunsOut` :239648

The defeat list, seven keys:

- `CommanderKilled` :239663
- `AllUnitsKilled` :239677
- `AllUnitsKilledOfType` :239693
- `UnitTypeKilled` :239734
- `DeathTimerRunsOut` :239776
- `AnyUnitPassesX` :239791
- `AnyUnitPassesZ` :239808

Victory needs every condition in the first list at once, :239922-239950.
Each condition keeps a Satisfied flag once it is met, :240729-240733
for one, and the save carries it, so a condition met and then lost
still counts. DestroyAllUnits and AllUnitsKilled are the two that look
again at what stands each time they are asked :240940-240965.
With no victory condition at all it never fires. Defeat needs any one
condition in the second list, :239952-239990. The per-player tick reads
victory first and only asks about defeat when victory is not met,
:206640-206672, so a tick that satisfies both is a win. Either verdict
puts a banner over the running battle for 90 ticks at the original's
30 Hz, :206542-206572, and then the statistics screen opens.

## The conditions a mission does not name

Two are filled in at :239825-239851. A mission with no victory
condition gets `DestroyAllUnits`, but only when the game mode is not
campaign, so a campaign mission is left as its file wrote it. A mission
with no defeat condition gets `AllUnitsKilled`. The second is where the
ordinary rule that you lose when you lose everything comes from.

## ANYTYPE

`MoveUnitToRadius`, `UnitTypePassesX` and `UnitTypePassesZ` accept the
literal unit type `ANYTYPE`, which is stored as an empty name and means
any type at all, :239493, :239553, :239611. No shipped mission uses it.

## What the shipped corpus says

74 of the 129 mission files carry placed units. 68 of those name at
least one condition and 6 name none. The most common key by a wide
margin is `AllUnitsKilled`, in 29 files, and it is a defeat condition
every time. `DestroyAllUnits` is next at 22 files and is a victory
condition every time. The two appear together in 14 files, which is
what the split is for: destroy the enemy to win, lose your own army to
lose.

Four keys are read by the original and used by no shipped mission:
`BuildUnitType`, `CaptureUnitType`, `AnyUnitPassesX` and
`AnyUnitPassesZ`. The first two ask whether the player owns a unit of
the named type, which covers building one and capturing one alike. The
last two take a bare coordinate and ask whether any enemy unit has
passed it.

## Missions that name no victory condition

Sixteen of the 48 base missions and seven of the 26 Iron Plague ones
name only defeat conditions. Campaign mode adds no implicit victory
condition, so nothing in the condition lists can win them, and the
engine sets its victory flag in one place only, :206549, reached only
from the victory list.

Those missions are won by their mission script. Every campaign mission
can carry one: the original resolves `Missions\<base>.cob` as asset
slot 2 and loads it at :177668. 63 of them ship, 43 in `missions.hpi`
and 20 in `IPMissions.hpi`, and all 23 of the missions above are among
them. The script engine takes the callbacks `Start`, `UnitCreated`,
`UnitDestroyed`, `TriggerHit` and `Trigger%i`, and offers host
functions including `SetMission` and `SetTrigger`. It is also what the
per-player tick asks before it reads the conditions at all, :206644,
and it is the likely writer of the forced verdict pair on the condition
object that :240090 and :240122 read.

So there is nothing to decide here and no deviation to record. Those
missions become winnable when mission scripting lands. Until then they
run without a way to win, which is the same behaviour the rules above
give them.

## Which side a condition reads

The original never asks who is an enemy. Each condition reads fixed
seats by number. The victory conditions that count losses read the
second seat alone, the one Player2 names: `KillEnemyCommander`
:240270, `DestroyAllUnits` :239280 reads that seat's unit count, and
`KillAllMobileUnits` :240354-240377 and `KillAllOfType` :240545-240568
test the owner byte for seat 1 and walk that seat's list, and
`KillUnitType` :240609 counts that seat's deaths of the type down from
the number the key names. `AnyUnitPassesX` and `AnyUnitPassesZ` read
the same seat :241193-241215.

The conditions about the player's own units read the first seat:
`MoveUnitToRadius` :240671, `UnitTypePassesX` and `UnitTypePassesZ`
:240795-240822, `BuildUnitType`, `CommanderKilled` :240884 and
`AllUnitsKilled` :240923-240965. `AllUnitsKilledOfType` walks the first two
seats together :241056-241100, and `UnitTypeKilled` counts deaths on
any seat :240997-241006. Both hear of a death when the unit's record
ends :227286, whether it was killed, captured or removed, and the count
goes into the save, so takmission32_mt is won on its seventeenth
swordsman and takx04_ph lost on its seventh sailor.

The four passes keys ask whether a unit stands within three squares of
the line, not past it :240719-240733, :241133-241145. A mission that
starts its unit far beyond the line, as takmission28_dh does with Dern
at z 262 for a line at 11, is won by walking it there.

So a passive or neutral seat never counts toward a mission's outcome.
Black Peace (takmission05_dh) is won by `KillAllMobileUnits` while its
map keeps wolves on seat 9 and cows, birds, deer and townsfolk on
seat 10, and the herd does not hold the win off.

## The seats a mission's PlayerN lines make

Each `PlayerN` line is lowered and searched for words anywhere in it
:169018-169095. `strategic` or `passive` says whether the seat plans,
`opponent`, `ally` or `neutral` what it is to the others, and the line
makes a seat only when two of those five words are in it. Player1 is a
seat whatever it says. `logo N` is the seat's colour and a side name is
its side.

At the start :177689-177749 Player1 takes its logo and side, and each
other seat takes its logo, or its own number counted from nought when
the line names none. Every seat marks a neutral seat as an ally, while
the neutral seat marks no one :177727-177734. An ally seat and the
player mark each other :177736-177741. An opponent is left at war with
everyone.

The mark is read by the attacker's own target search :19267, so every
seat passes a neutral seat's cows and townsfolk over while the neutral
seat's own wolves still go for anyone. A click on a passive seat's unit
is an attack all the same :186172-186174, and a mission order to attack
a type asks nothing of the mark :8990-8993.

## The units a mission allows

`Missions\<stem>.tdf` beside the .ota holds one empty section per unit
type :168660. When it loads, every type is turned off, the types it
names are turned on, and so is every side's monarch :164261-164355. A
type left off cannot be made at all :226872-226874. 64 of the 74 campaign
missions ship one, and every unit a mission places or its script creates
is in it or is a monarch. Six skirmish maps ship one as well and the
original reads them outside a multiplayer game :168849-168857, which is
not followed here yet.
