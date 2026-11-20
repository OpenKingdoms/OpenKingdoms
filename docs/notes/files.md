# Files and the data set

This note covers where OpenKingdoms writes the player's own files, how
the original finds its archives and maps and which copy of a file wins,
and how the files it finds decide whether Iron Plague is present and
what that changes.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Where the player's files are written

This section describes OpenKingdoms, not the original.

One resolver answers for everything the game keeps on the player's
behalf: options.cfg, and saved games in a `saves` subdirectory it
creates on first use.

| Platform | Directory |
|---|---|
| Windows | `%APPDATA%\OpenKingdoms\OpenKingdoms\` |
| macOS | `~/Library/Application Support/OpenKingdoms/OpenKingdoms/` |
| Linux | `$XDG_DATA_HOME/OpenKingdoms/OpenKingdoms/`, else `~/.local/share/OpenKingdoms/OpenKingdoms/` |
| Browser | `/libsdl/OpenKingdoms/OpenKingdoms/` in the in-memory filesystem, mirrored to origin private storage under `prefs/` |

The folder name is doubled because SDL builds the path from an
organisation and an application, and OpenKingdoms is passed as both. It
stays doubled on purpose: changing it would orphan every options.cfg
players already have, for a cosmetic gain.

SDL hands back a backslash path on Windows and a forward slash path
everywhere else. Anything appended to the directory reuses the
separator it already carries, so a composed path never mixes the two.

A save's slug is refused if it carries a separator, a drive colon or a
parent reference. The name the player typed is stored inside the file,
so the slug on disk never has to be anything but a plain file name.

### In a browser

In a browser the preference directory sits in an in-memory filesystem
that is rebuilt from nothing on every page load, so the page keeps the
player's files in origin private storage. The engine reads and writes
with ordinary file calls, the same code as on the desktop. The page
copies storage into memory before the game starts, and copies a file
back out after each write the engine reports. The engine cannot reach
that storage itself, because its synchronous handles are the worker
path and the browser build runs without Asyncify, so C cannot wait on a
promise on the main thread.

The settings restore runs before the page's early return for a build
whose archives were preloaded, since the player's settings have nothing
to do with how the archives arrived. Settings live at the storage root
under `prefs/`, a sibling of the `game/` directory the archives are
cached in and never a child of it, so the Forget my game files link,
which removes `game/`, cannot reach them. A private window has no
origin private storage, and the session then works with settings held
in memory only.

Saved games are in `saves/` under the preference directory, and the
mirror walks one level of subdirectory both ways, so the desktop and
browser layouts have the same shape. Saves are not copied in at boot.
Settings are a few hundred bytes and the game reads them on the way up,
so the boot waits for those, but saves come in when a save or load
dialog opens. Until they arrive the dialog draws an empty list with its
help line saying so, and the load dialog decides whether the directory
is empty only once they are in, so a player with saves is never told
there are none. The wait cannot be avoided, because the storage can be
read only a promise at a time on the main thread.

The write back copies only a file whose size or modification time has
moved. A deleted save is removed from storage as well as from memory,
so it does not come back on the next reload. The page asks for
persistence with `navigator.storage.persist()`, because until an origin
is granted it, its storage can be evicted under disk pressure. A
refusal is shown to the player on a strip over the canvas, and a
warning stays there until it is dismissed. The page also reads
`navigator.storage.estimate()` and warns below 32 MB of headroom. The
bulk of a save is the fog layers, one byte per 32 px cell for each
player, which deflate flattens, and the unit records. Saves are small
next to a browser's quota, so eviction rather than size is the risk.

A save in browser storage is not a file the player owns. The page's
Saved games panel, beside the forget link, writes one out as an
`.oksave` file and reads one back, see D-010. An import never
overwrites: a name already taken gets a `(2)`. An imported file goes to
storage and to the in-memory filesystem both, so the load dialog finds
it without a reload.

## Where maps come from

### Archives

At startup the original walks the game folder for `*.HPI`, then for
`*.UFO`, then the same patterns on the CD drive (legacy:121111,
legacy:121133, legacy:121152). Every archive it finds goes into one
flat list.

When two archives hold the same path, the original does not prefer the
one loaded last. It compares the entry date recorded for the file
inside each archive and takes the newest, keeping the copy it found
first when the dates are equal (legacy:259289). That is what lets a
patch archive override the base game whatever the file is called. An
archive mounted from the CD is consulted only when no archive on the
hard disk has the file.

In the GOG install with Iron Plague, 519 paths appear in more than one
archive. Among them, the date rule gives boneyards.hpi's copies of
seven online lobby screens over data.hpi's, IPSections.hpi's copy of
one terrain section over sections.hpi's, and IPData.hpi's
`ai/default.txt` and `guis/victorycre.gui` over V3Rocket.hpi's. Iron
Plague is the newer release, so those are the copies an Iron Plague
player is meant to get.

### Map packs

A `.kmp` in the game folder's `Maps` folder is an ordinary archive
holding one map under `kmap/`: `<name>.tnt`, `.ota`, `.crt`, `.txt`
and sometimes a `.tdf`. The 181 Darien Crusades maps ship this way.

The original mounts a pack on demand. When the player picks a map it
first tries to mount `Maps\<name>.kmp` and reads the map out of `KMAP\`
if that worked, and only falls back to `Maps\<name>.ota` when there is
no such pack (legacy:168765, legacy:168799). The engine mounts every
pack at startup, and a pack answers only for paths under `kmap/`, see
D-005.

### The map list

The original builds the skirmish and multiplayer list from two
patterns, `Maps\*.ota` and `Maps\*.kmp`, and nothing else
(legacy:167671). Where a map lives decides whether it is listed, not
anything inside it. Campaign maps live in the missions folder, so they
never appear, and no filter on `Type`, `ismission` or the player count
is applied anywhere. The two arguments the list builder takes are both
output lists, one of display names and one of file names, not a
filter. Its three callers are map choosers or the default map pick
(legacy:136061, legacy:138922, legacy:168530).

The GOG install with Iron Plague offers 236 maps: 28 from `maps.hpi`,
2 from `V2Rocket.hpi`, 25 from `IPData.hpi` and 181 map packs. Inside
the archives the maps folder is spelled `maps/`.

### One name from two sources

Names are unique across the four sources in a stock install, but a
player can make a clash by adding a map. The original's order decides.

- The list has one row per name, since the original keys its list by
  the name it shows.
- A map pack wins over the maps folder, because the original looks for
  `Maps\<name>.kmp` before it looks for `Maps\<name>.ota`.
- Between two archives the newest entry wins, by the same rule as any
  other file. The rule is per file, so a map could take its `.tnt` from
  one archive and its `.ota` from another, and the original resolves
  each of a map's files separately too.

## Creon and the expansion

### Which game the files make up

The original decides from the files it finds whether it is running
Iron Plague or the base game, and every Creon rule hangs off that one
answer. The expansion counts as present when
`camps/the iron plague.tdf` and `camps/ipalt.tdf` both open, and the
`-pretendnoexpansion` switch turns the answer off
(legacy:241742-241758, legacy:251988-251990). The answer is worked out
once and kept. Both files ship only in IPData.hpi. V3Rocket.hpi is the
3.0 patch for both games and already carries `creingame.gui`,
`victorycre.gui` and a `hostgame.gui` with an Allow Creon box, so the
Creon .gui files do not mark the expansion.

### The side list

The side table is `gamedata/sidedata.tdf`, read from SIDE0 until a
section is missing (legacy:164568-164578). The base game's copy stops
at SIDE6, and its last three sides, LIFEFORMS, NONPLAYERCHARACTERS and
WANDERING_MONSTERS, have no commander. IPData.hpi's copy adds SIDE7,
CREON, with prefix CRE and commander CRESAGE. A side is stored as its
SIDEn index everywhere, so Creon is 7 in saves and in the settings text
(legacy:164994, legacy:213130-213147).

The side button steps to the next side that has a commander and wraps
at the count (legacy:134955-134972). The side setter then decides
whether the choice stands (legacy:134883-134942).

- In a skirmish any side past the fourth turns back to Aramon when the
  expansion is absent (legacy:134933-134936). A computer row uses the
  same button and the same rule (legacy:136219-136222).
- In multiplayer it also turns back unless the game allows Creon
  (legacy:134910-134923). The host's Allow Creon counts only with the
  expansion present (legacy:134048-134052).
- In the campaign the mission decides and nothing is turned back.

The lobby shows sidedata's name with all but the first letter lowered,
so CREON reads Creon (legacy:136342-136361).

### What a Creon player gets

Each player's monarch is the commander its side names
(legacy:178022-178031). The sidebar is `<nameprefix>ingame.gui`, which
is `creingame.gui` for Creon (legacy:243412-243460). The build sparkle
is the side's `buildsparklygaf` (legacy:164761-164767), `creonbuild`
for Creon. The lobby badge is the side's `logoart` (legacy:164796), and
the end screen badge and the victory dialog follow the prefix
(legacy:153773, legacy:153975). Creon's unit textures are
`textures/cre*.gaf`. Its sidedata names `ara_textures.pal`, and
`cre_textures.pcx` in IPData.hpi holds the same 256 colours.

### The campaign

A mission's `PlayerN` line names that player's side. The game lowers
the line and takes the first side, in SIDEn order, whose name appears
in it (legacy:169026-169095), then gives each player that side as the
mission starts (legacy:177703-177749, legacy:206137). The Iron Plague
campaign puts the player on Creon in 9 of its 26 missions. The base
game's 48 missions never do.

### Saves and joining

A skirmish save marks a Creon player with `[CreonUnits]`
(legacy:164989-164999), see [saves.md](saves.md). Loading a skirmish
save that carries it without the expansion shows
NEED_EXPANSION_TO_PLAY and fails (legacy:159438-159449,
legacy:159611). Joining a LAN game that allows Creon without the
expansion is refused with the same message (legacy:133494-133513). The
online service's Creon Game box and its `5th_race` flag follow the
same test (legacy:57481, legacy:57621, legacy:119665-119678).

These rules are those of the later version of the game program, which
decides from the data it finds. The older Iron Plague program that also
ships is not covered, so a rule that lived only there is not
established.
