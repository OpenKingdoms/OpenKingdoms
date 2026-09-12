# Creon and the data set

The original decides from the files it finds whether it is running Iron
Plague or the base game, and every Creon rule hangs off that one answer.
This note records those rules and where the engine follows them.

## Which game the files make up

The expansion counts as present when `camps/the iron plague.tdf` and
`camps/ipalt.tdf` both open through the file layer, and the
`-pretendnoexpansion` switch turns the answer off (legacy:241742-241758,
legacy:251988-251990). The answer is worked out once and kept. Both files
ship only in IPData.hpi. V3Rocket.hpi is the 3.0 patch for both games and
already carries `creingame.gui`, `victorycre.gui` and a `hostgame.gui`
with an Allow Creon box, so the Creon .gui files do not mark the
expansion. The engine asks the same question in
`TAK_DataSet_HasIronPlague`, natively and in the browser.

## The side list

The side table is `gamedata/sidedata.tdf`, read from SIDE0 until a section
is missing (legacy:164568-164578). The base game's copy stops at SIDE6.
Its last three sides, LIFEFORMS, NONPLAYERCHARACTERS and
WANDERING_MONSTERS, have no commander. IPData.hpi's copy adds SIDE7 CREON
with prefix CRE and commander CRESAGE. A side is stored as its SIDEn
index everywhere, so Creon is 7 in saves and in the settings text
(legacy:164994, legacy:213130-213147).

The side button steps to the next side that has a commander and wraps at
the count (legacy:134955-134972). The side setter then decides whether
the choice stands (legacy:134883-134942).

- In a skirmish any side past the fourth turns back to Aramon when the
  expansion is absent (legacy:134933-134936). A computer row uses the same
  button and the same rule (legacy:136219-136222).
- In multiplayer it also turns back unless the game allows Creon
  (legacy:134910-134923). The host's Allow Creon counts only with the
  expansion present (legacy:134048-134052).
- In the campaign the mission decides and nothing is turned back.

The lobby shows sidedata's name with all but the first letter lowered, so
CREON reads Creon (legacy:136342-136361).

## What a Creon player gets

Each player's monarch is the commander its side names
(legacy:178022-178031). The sidebar is `<nameprefix>ingame.gui`, which is
`creingame.gui` for Creon (legacy:243412-243460). The build sparkle is the
side's `buildsparklygaf` (legacy:164761-164767), `creonbuild` for Creon.
The lobby badge is the side's `logoart` (legacy:164796) and the end screen
badge and victory dialog follow the prefix (legacy:153773,
legacy:153975). Creon's unit textures are `textures/cre*.gaf`. Its
sidedata names `ara_textures.pal`, and `cre_textures.pcx` in IPData.hpi
holds the same 256 colours.

## The campaign

A mission's `PlayerN` line names that player's side. The game lowers the
line and takes the first side, in SIDEn order, whose name appears in it
(legacy:169026-169095), then gives each player that side as the mission
starts (legacy:177703-177749, legacy:206137). The Iron Plague campaign
puts the player on Creon in 9 of its 26 missions. The base game's 48
missions never do.

## Saves and joining

A skirmish save gains `[CreonUnits] CreonUnits=1` when any player is side
7 (legacy:164989-164999). Loading a skirmish save that carries it without
the expansion shows NEED_EXPANSION_TO_PLAY and fails
(legacy:159438-159449, legacy:159611). Joining a LAN game that allows
Creon without the expansion is refused with the same message
(legacy:133494-133513). The online service's Creon Game box and its
`5th_race` flag follow the same test (legacy:57481, legacy:57621,
legacy:119665-119678).

## Limits of the reference

The reference is the later executable that switches on data. The older
`ironplague.icd` in the install lacks every one of these strings, so any
rule that lived only there cannot be confirmed from it.
