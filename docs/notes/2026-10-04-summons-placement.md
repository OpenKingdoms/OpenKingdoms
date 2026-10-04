# Placing a summons (2026-10-04)

The owner played Zhon and found he had to find a free spot for every
unit of his army. Every summons the ghost showed red over a unit, the
click refused, and a Shift summons on the spot of the one before was
dropped. This note records how the original places a summons and what
the engine now does.

## The original

A Zhon builder summons a unit the way any builder puts up a building.
The cursor and the order both snap to the 16 px cell lattice, at the
cell times 16 plus 8 for each cell of the footprint (legacy:39183-39190,
12028-12033). A def with a movement class takes its footprint from the
class (legacy:163193-163195), so an orc snaps as 3 by 3 and a giant as 4
by 4, where the harpy keeps the 2 by 2 its own file gives.

The loader gives a def with a `bmcode` a yard of `0x29` on every cell
and never reads its `YardMap` (legacy:163206-163208, 163272-163292). The
cursor's test looks for a unit on a cell only when the yard byte there
has bit `0x2` or `0x4` (legacy:218800-218811), and `0x29` has neither.
Its bit `0x1` does meet the cells a building's yard marks when the
building is placed (legacy:218185-218191, 218797). So a summons is
accepted on top of standing units and on an earlier frame at the same
spot, and refused on a building, on ground it cannot take, on a
blocking feature and at the map's edge.

The order itself never lets two units overlap. Once the builder is in
reach it tests the spot counting every unit (legacy:12088). If that
fails it tests again with units that can move let through
(legacy:12096). When only movers are in the way the builder looks again
10 frames later, says it is waiting at the sixth look and gives up
after 30 looks, about 10 seconds (legacy:12097-12116). Anything that
will not move ends the order at once (legacy:12098), and so does the
unit limit (legacy:12162-12172).

A plain summons is not moved off its spot when it is done
(legacy:12555-12559). So a second plain summons on the same spot waits
for the first unit to leave and gives up if it stays. The Ctrl run of
docs/notes/2026-10-04-a-summons-without-end.md, which steps each unit
off before the next, is how an army comes out of one spot.

## The engine

The engine had one site test for the cursor, the click and the order,
and it refused every cell any unit held. The test is now split.

`Units_IsBuildSiteClearFacing` is the cursor's test, and the ghost, the
classic click and the remaster's click all ask it. For a def with a
`bmcode` it lets units that walk through, and their frames too, and
still refuses a building's cells. It reads no yardmap for such a def.

`Units_IsBuildSiteFreeFacing` counts every unit, and a frame goes up
only where it passes. The computer's site search uses it, so the
computer never sets a summons to wait on its own army.

`Units_OrderBuild` takes a summons whose spot only movers hold. The
builder holds the order with no frame yet, walks to the site, and
looks again every 20 ticks. The frame goes up once the spot is free,
and the order ends after 30 looks made within reach, or at once when
something that will not move is in the way. A builder that cannot get
there gives up the walk as it would any order's. A summons queued with Shift is held the same
way when its turn comes, and a mission script's build step gets the
wait too. The held order is in the save and the state hash.

Both the snap and the tests take a walking def's footprint from its
move class, so the ghost, the order and the cells the unit then holds
agree.

## Left as it was

A frame here goes up before the builder gets there (M-009), so a spot
that clears while the builder walks gets its frame at once, where the
original would look only on arrival. The original's chatter for waiting
and for giving up has no counterpart yet, so both are silent. That is
M-015.

A summons is still tested for slope against the def's own `maxslope`,
where the original takes the slope from the move class. That was left
for a change of its own.
