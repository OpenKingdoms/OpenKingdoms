# Where features block, kept per tile (2026-11-15)

`Terrain_IsWalkable` answered the feature half of its question by
walking the whole feature list and testing each blocking footprint. A
point test cost one pass over every feature on the map, and the spot
pickers, `Units_CanStandAt` and the step ask it many times a tick.
Nothing about what it answers changes here. This note records the cost
and how the faster answer stays the same answer.

## What it cost

200,000 points spread over each map, the same points every run,
32-bit MSVC Release, ns per call, the range over several runs. Before
is a build of main and this build with the grid off:

| Map | Features | `Units_CanStandAt` before | after | `Terrain_IsWalkable` before | after |
| --- | --- | --- | --- | --- | --- |
| Abnar's Terrace | 88 | 580-660 | 41.8-45.1 | 500-540 | 9.8-11.4 |
| Angvir's Maze | 147 | 810-990 | 44.0-83.4 | 750-900 | 10.0-26.8 |
| Valysia City | 438 | 3,100-4,700 | 57.8-71.2 | 2,500-4,400 | 14.4-22.7 |

Every run finds the same number of open points on each map.

A skirmish between two computer players, 54,000 ticks on each map,
timed in quarters of 13,500 ticks. These are the same binary with the
grid on and off, two runs each, ms a tick:

| Map | Quarter | Grid off | Grid on |
| --- | --- | --- | --- |
| Abnar's Terrace | 2nd | 0.245, 0.234 | 0.218, 0.232 |
| Angvir's Maze | 3rd | 0.838, 0.789 | 0.747, 0.889 |
| Angvir's Maze | 4th | 0.920, 1.100 | 0.879, 0.800 |
| Valysia City | 3rd | 1.413, 1.512 | 1.044, 1.124 |
| Valysia City | 4th | 1.753, 1.758 | 1.013, 0.997 |

On Valysia City, late in the battle, a tick costs about 40 percent
less. On the smaller maps the difference is within the noise. Every
battle ends on the same state hash with the grid on, off, and on the
build before the change: 0xdb00ef89 on Abnar's Terrace, 0x5f3f210f on
Angvir's Maze and 0xfe561d7d on Valysia City.

## What it does now

`src/render/terrain.c` keeps a byte per 16 px tile of the whole map, set
where a blocking feature's footprint covers the tile. It is stamped
from the same `Features_InstanceBlocks` and `Features_InstanceFootprint`
the walk used, and a point past the slope and impassable tests reads
its tile. A footprint starts and ends on tile edges, so the tile answers
for every pixel in it. Points off the map still fail the slope test
first, and the area a build site search stamps still answers before
the grid.

The grid is the planner's kind of cache and follows the same changes.
`features.c` already tells the planner when a placement, a removal or a
next stage changes what blocks. At each of those it now also tells the
grid which tiles to stamp again from the list, and the count and array
the list had before the change. A list the grid was not stamped from is
not patched. The grid is dropped and stamped afresh instead.

- `Features_AddInstanceFacing`: corpses, wrecks, debris and every other
  placement, with the features it clears removed first.
- `Features_RemoveInstance`: a reclaim, a sweep, a body that has rotted,
  a stage with nothing after it.
- `feat_replace`: a feature destroyed, felled or burnt to its next
  stage, the old and new footprints both, and rubble under the
  remastered rules.
- `Features_NoteListReplaced` and `Features_MarkChanged` drop it: a map
  loaded, a save applied, a world ended, new defs. A sacred site placed
  mid-battle drops it too, since `Features_AddInstanceFacing` calls
  `Features_NoteListReplaced` for one. The next lookup stamps the whole
  map again. That costs time, not correctness, and sacred sites are rare.

A lookup also checks the world, the array, the count, the map's size and
the remastered rules, and stamps the grid afresh when any of them
moved. That does not catch a record written in place that keeps the
array and count, such as a def swapped with no report. Such a write
must call `Features_MarkChanged`, as `tak_features.h` asks, and no game
code writes in place without it. The grid is integers only, never saved
and never hashed, and a load stamps it again from the loaded list. If
its memory cannot be had, the walk answers until the grid is next
dropped.

## Ours

The test `the_walkable_grid_answers_as_the_feature_list_does` in
`test_line_of_fire` asks both ways at nine points of every tile, one
tile past the map each way, while features are written whole, moved in
place, removed, swept, placed, turned and changed stage, under both
rules and across a save. `Terrain_DebugWalkGrid(0)` gives the walk back
for that comparison.

A possible follow-up, not done here: `Terrain_BlockingBegin` and
`Terrain_WalkableTilesRect` still walk every feature, and could read the
grid instead.
