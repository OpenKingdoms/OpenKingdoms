# Water sits where the map says (2026-09-11)

Every depth test in the engine, from whether a unit can wade to whether a
building may be sited, compares terrain height against one number. The
engine took that number from the kingdom's entry in `sidedata.tdf`, which
lists a `waterheight` of 40 for Aramon and 58 for the other three worlds.

The original does not read that key at all. It takes the value out of the
map: field `0x0C` of the TNT header, stored once when the map loads and
read by everything after that (legacy:224848, legacy:224912). The only
other thing that writes it is a console command for testing terrain
(legacy:36528). `waterheight` in `sidedata.tdf` looks like a default for
authoring a new map of that world rather than anything the game reads.

## What this changes

For 220 of the 236 maps a stock install offers, the map's own value is
exactly the one its world lists, so the water does not move. Sixteen Iron
Plague maps authored a different sea level and now get it:

| map | world | was | now |
| --- | --- | --- | --- |
| Alkhest Quadrille | Taros | 58 | 59 |
| Black Heart Jungle | Zhon | 58 | 54 |
| Crusader's Keep | Creon | 40 | 54 |
| Haunted Waterworks | Taros | 58 | 56 |
| Islands of the Mer Warrior | Veruna | 58 | 50 |
| Isle of Palms | Veruna | 58 | 54 |
| Lake Cuhmoniwanakilya | Aramon | 40 | 43 |
| Lost Lake | Aramon | 40 | 54 |
| Moka's Fingers | Veruna | 58 | 54 |
| No Zhonian Is an Island | Zhon | 58 | 56 |
| Nobia's Temple | Zhon | 58 | 56 |
| Rival Hill | Aramon | 40 | 42 |
| Sand River Plain | Veruna | 58 | 56 |
| The Drafis Bridge | Creon | 40 | 78 |
| The Gardens of Atys | Creon | 40 | 63 |
| Ulasem Arena | Zhon | 58 | 42 |

Campaign maps move much more often, since 45 of the 74 disagree with
their world. takmission01_mt is an Aramon map at 55 where the sheet says
40, which is 15 height units of shoreline that were dry and should not
have been.

`test_tnt` walks every map and holds the list above, so a future change
that moves anyone else's coastline has to say so.
