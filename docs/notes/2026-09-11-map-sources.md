# Where maps come from, and which copy wins (2026-09-11)

The engine used to mount only the `.hpi` archives in the game folder, so
players saw a fraction of the maps they own. The original mounts more than
that, and it finds maps in more than one place.

## What the original mounts

At startup it walks the game folder for `*.HPI`, then for `*.UFO`, then the
same pattern on the CD drive (legacy:121111, legacy:121133, legacy:121152).
Every archive it finds goes into one flat list.

When two archives hold the same path it does not prefer the one loaded last.
It looks at the entry date recorded for the file inside each archive and
takes the newest, keeping the copy it found first when the dates are equal
(legacy:259289). That is what makes a patch archive override the base game
whatever the file is called. An archive mounted from the CD is consulted
only when no archive on the hard disk has the file.

OpenKingdoms now does the same. `VFS_Init` mounts `*.hpi`, then `*.ufo`,
then the map packs (below), each group in name order, and a lookup takes the
newest entry with the earlier mount winning a tie.

This changes which copy of ten files the GOG install resolves to, out of 519
paths that appear in more than one archive. 233 of the affected paths hold
identical bytes in both archives. The ten that differ are seven online lobby
screens where `boneyards.hpi` now wins over `data.hpi`, one terrain section
where `IPSections.hpi` wins over `sections.hpi`, and `ai/default.txt` plus
`guis/victorycre.gui` where `IPData.hpi` wins over `V3Rocket.hpi`. Iron
Plague is the newer release of the two, so that is the sheet an Iron Plague
player is meant to get.

## Map packs

A `.kmp` in the game folder's `Maps` folder is an ordinary archive holding
one map under `kmap/`: `<name>.tnt`, `.ota`, `.crt`, `.txt` and sometimes a
`.tdf`. The 181 Darien Crusades maps ship this way.

The original mounts a pack on demand. When the player picks a map it first
tries to mount `Maps\<name>.kmp` and reads the map out of `KMAP\` if that
worked, and only falls back to `Maps\<name>.ota` if there is no such pack
(legacy:168763, legacy:168799). OpenKingdoms mounts every pack at startup
instead, which is simpler and gives the same answer, with one guard: a pack
only ever serves paths under `kmap/`. Anything else inside it is ignored, so
dropping a map into `Maps` can never rewrite units, weapons or art.

## Which maps the chooser offers

The original builds the skirmish and multiplayer list from two patterns,
`Maps\*.ota` and `Maps\*.kmp`, and nothing else (legacy:167670). The list is
therefore decided by where a map lives, not by anything inside it. Campaign
maps live in the missions folder, so they can never appear, and no filter on
`Type`, `ismission` or player count is applied anywhere. The two arguments
the builder takes are both output lists, one of display names and one of
file names, not a filter. All three of its callers are map choosers or the
default-map pick (legacy:136061, legacy:138922, legacy:168530).

OpenKingdoms lists the same sources through `TAK_Maps_Scan`:

- `kmap/*.ota`, the mounted map packs
- `maps/*.ota`, the maps folder as the archives spell it
- `maps/Maps/*.ota`, the same folder in an extracted loose tree

The GOG install with Iron Plague gives 236 maps: 28 from `maps.hpi`, 2 from
`V2Rocket.hpi`, 25 from `IPData.hpi` and 181 map packs.

## Two sources, one map name

Names are unique across the four sources in a stock install, but a player
can create a clash by adding a map. The rule follows the original's order:

- One row in the chooser per name, since the original keys its list by the
  name it shows.
- A map pack wins over the maps folder, because the original looks for
  `Maps\<name>.kmp` before it looks for `Maps\<name>.ota`.
- Between two archives the newest entry wins, by the same rule as any other
  file. This is per file, so in the pathological case a map could take its
  `.tnt` from one archive and its `.ota` from another. The original resolves
  each of a map's files separately too.

`TAK_Maps_FindFile` applies that order for every map file the engine reads,
and also falls through to the missions folder so campaign maps load by the
same call.
