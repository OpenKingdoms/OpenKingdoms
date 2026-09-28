# A side takes on only what it sees (2026-09-27)

The owner reported that, with Line of Sight on, units often attack an
enemy the player cannot see yet, ranged units most of all. This note
records why, what the original does and what changed.

## What the original does

A side's units choose targets from a list of enemies, rebuilt every 7
frames, and an enemy goes into that list only when the side's
visibility test passes (legacy:20536-20540). The same test gates
drawing, picking, the minimap and sound (legacy:206797, legacy:237320,
legacy:223583, legacy:152048). It passes when any corner of the unit's
box stands on a cell the side sees now, with Line of Sight on
(legacy:206887-206892), or on a cell the side has explored, with it off
(legacy:206877-206884). So what a player sees and what his units can
take are one set.

Return fire is the one exception. A unit that is hit answers the
shooter with no sight test at all, provided its selected weapon can
take the shooter and reaches it (legacy:15123). It also answers inside
nine tenths of its `maneuverleashlength` plus that reach, which a melee
weapon does not add, when a unit field the reference does not name is
set (legacy:15124-15137). The distance there is the longer side plus a
quarter of the shorter (legacy:254574).

Ranged units routinely outreach their own sight. A Mage Archer sees
250 and shoots 550 (`data/units/arabow.fbi`), a trebuchet sees 200 and
shoots 2700.

## Why units took on what the player could not see

Return fire answered every shooter, at any range and unseen. A unit hit
from the dark turned and fired back, and when the shooter was out of
reach it walked off after an enemy the player had never seen. That is
most of the report.

The draw test and the target test were also two tests. The idle search
counted any corner of the target's footprint and the draw test only its
centre. For a walker, which has no footprint of its own, the two agree.
For a building they did not, so archers and trebuchets shelled a
building still drawn dark. A 7 by 7 keep's corners stand 56 px from its
centre, nearly two fog cells.

## What changed

`Fog_SeatSeesAt` is the one test, current sight with Line of Sight on
and explored ground with it off. The draw gate, picking and the
minimap go through it, and so does the idle search with Line of Sight
on, with the same footprint corners, so a building is drawn exactly
when its side can take it.

With Line of Sight off the idle search still sees the whole map, as
docs/notes/2026-09-15-line-of-sight-off.md records. The original reads
the explored map there too, but of the local machine, which lockstep
cannot copy. Reading each seat's own would be closer to the original
and is left for the owner to decide.

Return fire needs the shooter in reach, as the original does. The
leash branch is taken for every unit, since the field that decides it
is not named. Return fire also needs the shooter seen by the victim's
side, which the original does not ask (D-023). It applies to every
seat, human and computer. The computer player's base defence still
hears of the hit first and marches on the threat.

A target a unit already holds is not dropped when it steps out of
sight. The original keeps shooting what it holds (legacy:11163-11178),
and so does the engine.

## What is not in the engine

The remaster draws the units the engine lists, then hides them again
until its own fog picture, taken every quarter second, agrees. That is
a lag in the front end, outside this repository.

## Tests

`test_line_of_fire` has a ranged unit that waits for a spotter before
it acquires, return fire that waits for the side to see the shooter,
return fire that needs reach, a blade that answers inside its leash,
and a keep drawn exactly when its side can take it.
