# Missions and campaigns

How the original wins and loses a campaign mission, which seats a
mission makes and how they treat each other, which units a mission
allows, and the Book of Deeds, the story screen that offers the
campaigns.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Two lists of conditions

A mission file writes its conditions as plain keys inside
`[GlobalHeader]`, next to `numplayers` and `size`. There is no
conditions section and no nesting. The original reads those keys
(legacy:239243-239850) into two separate lists.

The victory list holds eleven keys, in the order they are read.

| Key | Read at |
|---|---|
| `KillEnemyCommander` | legacy:239266 |
| `DestroyAllUnits` | legacy:239280 |
| `KillAllMobileUnits` | legacy:239294 |
| `BuildUnitType` | legacy:239310 |
| `CaptureUnitType` | legacy:239352 |
| `KillAllOfType` | legacy:239391 |
| `KillUnitType` | legacy:239432 |
| `MoveUnitToRadius` | legacy:239474 |
| `UnitTypePassesX` | legacy:239532 |
| `UnitTypePassesZ` | legacy:239590 |
| `VictoryTimerRunsOut` | legacy:239648 |

The defeat list holds seven.

| Key | Read at |
|---|---|
| `CommanderKilled` | legacy:239663 |
| `AllUnitsKilled` | legacy:239677 |
| `AllUnitsKilledOfType` | legacy:239693 |
| `UnitTypeKilled` | legacy:239734 |
| `DeathTimerRunsOut` | legacy:239776 |
| `AnyUnitPassesX` | legacy:239791 |
| `AnyUnitPassesZ` | legacy:239808 |

Victory needs every condition in the first list at once
(legacy:239922-239950). Each condition keeps a Satisfied flag once it
is met (legacy:240729-240733 for one), and the save carries it, so a
condition met and then lost still counts. `DestroyAllUnits` and
`AllUnitsKilled` are the two that look again at what stands each time
they are asked (legacy:240940-240965). With no victory condition at
all victory never fires. Defeat needs any one condition in the second
list (legacy:239952-239990). The per-player tick reads victory first
and asks about defeat only when victory is not met
(legacy:206640-206672), so a tick that satisfies both is a win. Either
verdict puts a banner over the running battle for 90 ticks at 30 Hz
(legacy:206542-206572), and then the statistics screen opens. How a
skirmish ends is in [gameflow.md](gameflow.md), and what a save keeps
of the conditions is in [saves.md](saves.md).

## Conditions a mission does not name

Two are filled in when a mission names none (legacy:239825-239851). A
mission with no victory condition gets `DestroyAllUnits`, but only
when the game mode is not campaign, so a campaign mission is left as
its file wrote it. A mission with no defeat condition gets
`AllUnitsKilled`, which is where the ordinary rule that a player who
loses everything loses comes from.

`MoveUnitToRadius`, `UnitTypePassesX` and `UnitTypePassesZ` accept the
literal unit type `ANYTYPE`, which is stored as an empty name and
means any type at all (legacy:239493, legacy:239553, legacy:239611).
No shipped mission uses it.

## What the shipped missions use

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
last two take a bare coordinate and ask about the enemy seat's units,
as the next sections describe.

## The mission script

Sixteen of the 48 base missions and seven of the 26 Iron Plague ones
name only defeat conditions. Campaign mode adds no implicit victory
condition, so nothing in the condition lists can win them, and the
original sets its victory flag in one place only (legacy:206549),
reached only from the victory list.

Those missions are won by their mission script. Every campaign mission
can carry one: the original resolves `Missions\<base>.cob` as asset
slot 2 and loads it (legacy:177668). 63 ship, 43 in `missions.hpi` and
20 in `IPMissions.hpi`, and all 23 of the missions above are among
them. The script takes the callbacks `Start`, `UnitCreated`,
`UnitDestroyed`, `TriggerHit` and `Trigger%i`, and offers host
functions including `SetMission` and `SetTrigger`. The per-player tick
asks the script before it reads the conditions at all
(legacy:206644). Whether the script is what writes the forced verdict
pair on the condition object that legacy:240090 and legacy:240122 read
is likely but not established. The engine's map script and order lists
depart from the original in places, see D-018.

## Which seat a condition reads

The original never asks who is an enemy. Each condition reads fixed
seats by number.

The victory conditions that count losses read the second seat alone,
the one `Player2` names. `KillEnemyCommander` (legacy:240270) reads
that seat, and `DestroyAllUnits` (legacy:239280) reads its unit count.
`KillAllMobileUnits` (legacy:240354-240377) and `KillAllOfType`
(legacy:240545-240568) test a unit's owner byte for that seat, number
1 counted from nought, and walk its list, and `KillUnitType`
(legacy:240609) counts that seat's deaths of the type down from the
number the key names.
`AnyUnitPassesX` and `AnyUnitPassesZ` read the same seat
(legacy:241193-241215).

The conditions about the player's own units read the first seat:
`MoveUnitToRadius` (legacy:240671), `UnitTypePassesX` and
`UnitTypePassesZ` (legacy:240795-240822), `BuildUnitType`,
`CommanderKilled` (legacy:240884) and `AllUnitsKilled`
(legacy:240923-240965). `AllUnitsKilledOfType` walks the first two
seats together (legacy:241056-241100), and `UnitTypeKilled` counts
deaths on any seat (legacy:240997-241006). Both hear of a death when
the unit's record ends (legacy:227286), whether it was killed,
captured or removed, and the count goes into the save, so
`takmission32_mt` is won on its seventeenth swordsman and `takx04_ph`
lost on its seventh sailor.

The four passes keys ask whether a unit stands within three squares of
the line, not past it (legacy:240719-240733, legacy:241133-241145). A
mission that starts its unit far beyond the line, as `takmission28_dh`
does with Dern at z 262 for a line at 11, is won by walking it there.

So a passive or neutral seat never counts toward a mission's outcome.
Black Peace (`takmission05_dh`) is won by `KillAllMobileUnits` while
its map keeps wolves on seat 9 and cows, birds, deer and townsfolk on
seat 10, and the herd does not hold the win off.

## The seats a mission's PlayerN lines make

Each `PlayerN` line is lowered and searched for words anywhere in it
(legacy:169018-169095). `strategic` or `passive` says whether the seat
plans, and `opponent`, `ally` or `neutral` what it is to the others. A
line makes a seat only when two of those five words are in it, and
`Player1` is a seat whatever it says. `logo N` is the seat's colour
and a side name is its side.

At the start (legacy:177689-177749) `Player1` takes its logo and side,
and each other seat takes its logo, or its own number counted from
nought when the line names none. Every seat marks a neutral seat as an
ally, while the neutral seat marks no one (legacy:177727-177734). An
ally seat and the player mark each other (legacy:177736-177741). An
opponent is left at war with everyone.

The mark is read by the attacker's own target search (legacy:19267),
so every seat passes a neutral seat's cows and townsfolk over while
the neutral seat's own wolves still go for anyone. A strategic seat's
unit bitten by one does not answer it either (legacy:15101-15103). A
click on a passive seat's unit is an attack all the same
(legacy:186172-186174), and a mission order to attack a type asks
nothing of the mark (legacy:8990-8993).

## The units a mission allows

`Missions\<stem>.tdf` beside the .ota holds one empty section per unit
type (legacy:168660). When it loads, every type is turned off, the
types it names are turned on, and so is every side's monarch
(legacy:164261-164355). A type left off cannot be made at all
(legacy:226872-226874). 64 of the 74 campaign missions ship one, and
every unit a mission places or its script creates is in it or is a
monarch. Six skirmish maps ship one as well, and the original reads
them outside a multiplayer game (legacy:168849-168857).

## The Book of Deeds

The story screen is `bod.gui`, reached from the main menu.

### Which books an install offers

The campaign list is a scan of `camps\*.tdf` (legacy:141576,
legacy:143362), through the find-first and find-next wrapper at
legacy:258585. Each entry is named through the translate table, keyed
by its own lower case file name (legacy:143453), and a lookup that
misses hands back the key it was given (legacy:267931).
`-pretendnoexpansion` cuts the scan down to `book of darien.tdf` alone
before anything else looks at it (legacy:141580-141710,
legacy:143377-143405). The scan as the reference shows it drops only
the blank entry (legacy:143366-143377) and does not filter on whether
a name exists.

The shipped data carries three such files. `data.hpi` has `book of
darien.tdf` with 48 missions. `IPData.hpi` has `the iron plague.tdf`
with 25 and `ipalt.tdf`, which is byte for byte the same file except
that its last mission is `takx26_dh` rather than `takx25_dh`. Only the
first two are named in a translate table, both in `IPEnglish.hpi`'s
`translate/guiexpansion.tdf`:

```
[book of darien.tdf]  English = Book of Darien
[the iron plague.tdf] English = The Iron Plague
```

A base install has no `guiexpansion.tdf` at all, so it names neither,
and one book is the whole list.

From playtesting, the original's chooser on an Iron Plague install
offers Book of Darien and The Iron Plague and never `ipalt.tdf`, so
what leaves it out lies in the directory scan underneath, which the
reference does not show. The chapter art still has a catch-all frame
for a campaign file it does not know by name. The engine offers only a
campaign the translate table names, see D-011.

The list is sorted on the shown name with a case insensitive compare
(legacy:261900-261990, legacy:143490). With more than one book the
Change User button opens `PlayerCampaignDialogue.gui`, a combined
player and campaign chooser, and with one it opens the plain
`PlayerDialogue.gui` (legacy:142640, legacy:143946-143970).

### The page

`BookOf` is authored text, "Book of", and no code ever writes to it.
`BookName` carries the player's own name (legacy:144267), so the book
is the player's, not the campaign's. `ChapterNumber` is the chapter
index plus one (legacy:144452-144458).

`ChapterText` is the localised chapter title, keyed by the campaign
file's `missionname` for that chapter (legacy:144497-144540). Base
game titles are in `english/translate/missions.tdf` and the
expansion's in `ipmissions.tdf`. When the Crusades balance is on, the
original appends the localised "Crusades Balance" in brackets
(legacy:144563-144627).

`ChapterImage` picks a frame of `story1.gaf`'s one entry, `Story1`
(legacy:144460-144495).

| Book | Frame |
| --- | --- |
| `book of darien.tdf` | chapter index plus 1 |
| `the iron plague.tdf` | chapter index plus 0x32 |
| a blank name | chapter index plus 0x32 |
| anything else | 0x31 |

A frame at or past the entry's frame count falls back to 0. The
numbers fit the art exactly. `data.hpi`'s `Story1` has 49 frames for
48 chapters, which leaves 0x31 out of range and so blank on a base
install. `IPData.hpi` replaces the file with one of 75 frames, which
covers 0x31 and the 25 expansion chapters at 0x32 through 0x4A. A
widget in a `.gui` carries at most sixteen frames, far fewer than
`Story1` holds.

### The buttons

- Play starts the open chapter. Holding Shift while the open chapter
  is `takx25_dh` starts `takx26_dh` instead (legacy:143903-143923).
  `takx26_dh` ships complete in `IPMissions.hpi`, and the install has
  no `Takx26` movie.
- Next Chapter and Previous Chapter walk the campaign's mission list,
  which is not itself clipped (legacy:141334-141368). The stop comes
  from the buttons. Next Chapter is live only while there is a next
  mission and the open chapter is below the high water mark
  (legacy:144107-144131).
- Load Game opens the load dialog (legacy:143865-143868), the same one
  the F1 menu and the skirmish lobby open.
- Change User opens one of the two dialogs above.
- Previous goes back to the main menu.

Typing `wasabi` on the screen sets the high water mark to the mission
count, which leaves Next Chapter live all the way to the end
(legacy:144228-144234).

The high water mark also decides where the book opens. Picking a user
reads their progress file, takes the furthest mission it records
(legacy:141660-141680), and walks the book forward to it
(legacy:144385-144400, legacy:144405-144425). The engine keeps the
mark in its settings file rather than a progress file, see D-017.

### Saves that need the expansion

Loading a save reads its summary. A save naming the Iron Plague
campaign, and a skirmish save carrying a `[CreonUnits]` section, are
both refused with `NEED_EXPANSION_TO_PLAY` when the expansion is
absent (legacy:159443-159470, legacy:159596-159612). Creon and the
expansion are in [files.md](files.md), and the save itself is in
[saves.md](saves.md).
