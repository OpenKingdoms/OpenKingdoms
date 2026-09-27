# Ground the map closes (2026-09-27)

A TNT map carries a layer of 16-bit values, one per 16 px cell, at header
offset `0x14`. Most cells hold `0xFFFF` for nothing, and a feature cell
holds an index into the map's feature name table. A third value, `0xFFFC`,
is a mark the map author paints on ground no unit may enter.

When the original loads a map it copies that mark into the cell record
(legacy:225023, legacy:128160). Every ground test then reads it the way it
reads a blocking feature: the movement check over a unit's footprint
refuses any cell holding a mark other than a feature index or a feature's
continuation (legacy:219659), and so do both build placement checks
(legacy:219119, legacy:218822). The slope rule is separate and unchanged:
a cell is too steep when the highest and lowest of its four corner heights
differ by more than the move class allows (legacy:224592-224622).

The engine read the layer only for features and ignored the mark, so the
slope rule alone decided where a unit could stand.

## What the maps rely on it for

Angvir's Maze builds its walls out of four height steps, for example 62,
68, 96 and 125, which is 6, 28 and 29 a cell. A swordsman's class allows
30, so by slope alone much of each wall face is a staircase. The author
painted 11533 cells of the wall faces with the mark and the original keeps
units off them. Without it units walked up the faces wherever the steps
came in under 30 and stood on the wall tops.

## What this changes

A marked cell is now closed in `Terrain_IsWalkable` and in the tile bitmap
the planner caches, so the planner, the mover and building placement all
see it. Build yard cells that skip the feature test skip the mark too, as
they do in the original.

Of the 283 map files in a stock install, skirmish and campaign together,
149 carry the mark and 146 have marked cells a slope of 30 would have let
a unit stand on. Only marked cells change, so a ramp the author left open
stays open. One skirmish start, the third on Terror Hamlet, stands on
marked ground, and a monarch put there walks off it. The base game's skirmish maps that change, counted in
cells a class with a slope of 30 could stand on before:

| map | marked cells | cells that close |
| --- | --- | --- |
| Angvir's Maze | 11533 | 6242 |
| Castle | 672 | 357 |
| Fortresses of Laingen | 1672 | 810 |
| Frey River Plain | 197 | 61 |
| Inner Circle | 2538 | 2538 |
| Ladron's Tarn | 5073 | 3346 |
| Lern Forrest Lakes | 68 | 21 |
| Muntil's Ford Guard | 1202 | 263 |
| Per Mare Per Terras | 16 | 3 |
| Riparian Conflict | 108 | 21 |
| Sewers of Elam | 2253 | 1301 |
| Tarosian Plain | 1236 | 717 |
| Two Castles | 3177 | 2972 |
| Ulin's Folly | 215 | 37 |
| Varro Passage | 117 | 15 |

Some ground the marks enclose is cut off as a result, such as wall tops
on Angvir's Maze and a walled yard in its middle. The original cuts it
off the same way.
