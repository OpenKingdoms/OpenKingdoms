# Eight seats on a bigger map (2026-09-27)

The owner asked for eight against eight, as Beyond All Reason plays
it, on maps up to twice as wide and tall as the largest the game ships.
The engine holds eight seats today. This note records the first steps:
room for the units and the ground, measured with eight seats of a
computer player each. Raising the seat count to sixteen comes after,
with the relay and the protocol.

## How it is measured

`--perf-probe big8` plays Ulasem Arena, the largest map with eight
starts, with eight computer seats on their own teams and nobody human.
`--perf-scale 2` repeats the map twice each way and moves each start
into its own copy, so the same eight seats spread over four times the
ground. Each window of 600 ticks prints the simulation's cost by part,
the unit count, the heap and a census of what each seat has built. With
big8 the probe also splits the combat tick, the mover and the computer
player's think into their parts.

Numbers below are milliseconds of simulation a tick, the mean over the
second half of a 14400 tick run on the desktop, with the software
renderer and no window.

## What stood in the way

The unit array held 2000. A seat's own limit is up to 2000 as well, so
one full seat filled the map. It holds 8192 now, and the computer
player's table of which unit is in which group grew with it. A save
written before holds 2048 members and still loads its groups.

The placement test asked every map feature whether it was a sacred site
under each cell of a lodestone, and the computer player asks that of
every pad each time it counts its free sites. At twice the size that
was about 9 ms a think, a cost that grew with the square of the map's
area. The sites are indexed by cell now and the index is rebuilt when
one comes or goes.

The influence map the computer player plans with stopped at 16384
pixels and folded everything past it into its edge cells. It covers
32768 now.

A build site's overlap test, the computer player's search for an enemy
to engage and its test for a mana building beside a pad each walked
every unit. They ask the simulation's spatial grid now, which also
keeps the units that came to stand since it was built this tick.

A group's squad reads, how spread out it is and what enemies stand at
its target, were worked out once for each member. They are worked out
once a think for the group.

## A measurement error found on the way

The first measurement at twice the size had six of the eight seats
build nothing. The probe moved each start sixteen times too far, since
starts are in map squares and not pixels, and put six seats off the
map. That was the whole of "the computer player builds little on tiled
maps".

## Results

| Map | Before | After | AI before | AI after | Units | Seats building |
|---|---|---|---|---|---|---|
| 1x | 2.59 | 2.60 | 0.22 | 0.18 | 154 | 8 of 8 |
| 2x | 10.46 | 8.96 | 2.28 | 0.37 | about 175 | 8 of 8 |

The heap is about 386 MB at 1x and 450 MB at 2x. The worst tick at 2x
fell from 891 to 737 ms.

## What is left

At twice the size the mover is the cost: 8.3 ms a tick, 2.5 ms of it
in route planning. The route search allocates and clears arrays the
size of the whole map on every plan, which grows with the map's area
whatever the route's length. That is the next piece of work.

The computer player still has two counts that grow with its builders
times all the units on the map, ai_count_owned and ai_build_score,
asked once a builder each think. They are small beside the mover today
and come after it.

## The counts made linear (2026-09-29)

ai_count_owned, ai_build_score and the two tests for a frame already
going up now read a census of the thinking seat's units by type. It is
counted in one pass a think, over the slots the think began with. A
build the AI starts makes it stale, and the next question counts again.
A slot added during the think is counted by a scan of the slots past
the census. A tower's site search lists the seat's towers once and not
once for every site it tries.

The answers are the same as before by construction and by test.
test_ai_census runs whole thinks of eight seats over random populations
with the census off and on and compares every order, build and state
hash, and a check mode recounts every census answer by the old scan.
`test_ai_census --bench` times one think of eight seats over 8000 units
with 200 builders. The numbers are in the pull request.

The spatial grid gives way for the walking since it was built by two
steps of the fastest unit loaded and a margin, so a modded unit faster
than any shipped one is never missed.

The browser measurement is still to run.
