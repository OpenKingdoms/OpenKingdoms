# Single slow ticks in eight-seat battles (2026-10-02)

The remaster's soak plays eight seats on Ulasem Arena, seven of them
computer players, with the map revealed. Most frames there cost 2 to 4
ms a tick, and a few cost hundreds of milliseconds for a handful of
ticks. At normal speed one such tick is a visible hitch. This note
records what ran inside the slow ticks and what changed. Every change
keeps the simulation's outcome bit for bit: the state hash folded over
every tick of a 15 minute battle is the same before and after.

## How it is measured

`TAK_SPIKE_PROBE=15 TAK_PERF_SPIKE=10 test_ui_screens sim_spike_probe`
plays the big8 scenario flat out with the map revealed and prints every
tick over 10 ms with its parts (docs/notes/2026-09-11-perf-probes.md).
`TAK_PERF_HASH=1` folds the state hash of every tick into the window
lines, so two builds can be held to the same battle. `TAK_PERF_MAP` and
`TAK_PERF_SEATS` play it elsewhere. Release build, one Windows desktop.

## What ran in the slow ticks

A dead unit leaves a corpse, and a corpse is a blocking feature. Every
blocking feature that came or went threw away the route planner's whole
cache: the per tile ground map, the placement mask and the long route
distance field of every move class. The next plan of each class built
them again from nothing, about 11 ms for the masks and 33 to 37 ms for
the field on Ulasem Arena. In a battle corpses fall and rot all the
time, so this was the largest single cost, and it landed on whichever
tick next planned a route. The computer player's site checks plan
routes too, so its builder think took the same hit.

A route search that cannot reach its goal opens its whole budget of
8192 cells, which cost about 4 ms. A tick can hold sixteen searches, and
a group sent at a place it cannot reach fills them.

The clearance map, the largest free square at every tile, was rebuilt
for the whole map whenever any structure was placed or removed, about
3 ms for each move class that planned afterwards.

The first plan of each move class in a battle built its layers inside
the tick.

A computer player looking for a place for a shipyard read every cell of
its 6 by 18 footprint at about 9000 candidate sites before finding none
of them on water, about 12 ms for each builder that asked.

## What changed

A blocking feature now redoes the tiles it covers and the cells and
clearance those tiles can reach, and patches the four distance sweeps
behind the field. A sweep keeps every cell that still has a neighbour
giving it its distance and measures the rest again. When the cell a
sweep starts from moves, that sweep is measured again in full. A test
holds 400 random patches, with structures coming and going between
them, against a fresh build of every layer.

The sweeps use a ring of buckets in place of a heap, since every step
costs between 10 and 525.

The structure layer logs what each version change stamped, so a stale
clearance map redoes those tiles and the 255 above and left of them.

Every walking move class gets its layers at load.

The search reads each neighbour's placement mask once per cell it opens
and reads the occupancy layer in place. Its heap keeps the same
comparisons in the same order, so the cells it opens are the same.

The ground test of a building site stops at the first cell that already
fails a test it would only fail harder later.

## Results

Worst tick in each game minute, in ms, before and after, from the same
battle (the hashes match, so it is the same battle tick for tick).

Ulasem Arena, eight computer seats:

| Minute | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Before | 22 | 44 | 19 | 55 | 26 | 59 | 107 | 73 | 59 | 43 | 84 | 63 | 66 | 82 | 21 |
| After | <10 | 14 | 13 | <10 | 16 | 19 | 28 | 29 | 19 | 24 | 28 | 19 | 38 | 35 | 16 |

Ticks over 30 ms fell from 95 to 4 and ticks over 50 ms from 31 to
none. The mean tick fell from 2.68 to 2.30 ms. Loading builds nine
classes' layers in about 240 ms.

Athri Cay, five computer seats:

| Minute | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Before | 13 | 17 | 17 | 23 | 24 | 38 | 23 | 19 | 30 | 30 | 20 | 30 | 37 | 37 | 24 |
| After | <10 | <10 | 11 | 13 | 11 | 22 | 14 | 11 | 16 | 13 | 12 | 17 | 19 | 24 | 15 |

The mean tick on Athri Cay fell from 1.23 to 0.97 ms.

## What is left

The slowest ticks left on Ulasem Arena are searches that cannot reach
their goal, fifteen of them in one tick at about 2.2 ms each. Their
cost is the search itself. The heap reads its keys as they stand, so a
cell whose estimate falls while it waits is not moved, and about one pop
in seven is not the cheapest cell open. Any other queue would open the
cells in another order and plan other routes, so the search cannot be
made cheaper by swapping its queue without changing the outcome. Making
those ticks shorter means opening fewer cells in one tick, which changes
when units get their routes and needs a new build.
