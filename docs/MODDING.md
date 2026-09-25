# Modding

OpenKingdoms reads mods the way the original reads its own data, from HPI
archives, and it adds two things the original never had. A mod can be a
plain folder of loose files, so a changed unit needs no repacking. And a
player can keep several mods side by side and choose which set to play,
without copying archives in and out of the game folder.

Everything here works on the desktop builds and in the browser.

---

## Where mods live

Mods go in a `Mods` folder inside your game folder, next to the game's own
`.hpi` files:

```
Total Annihilation Kingdoms/
  totala1.hpi
  ...
  Mods/
    TAKEnhanced_v1.3.6.hpi       (archives named by a TAK Enhanced preset)
    Tough Swords/                (a folder mod)
      mod.tdf
      units/arasword.fbi
  TAKEnhanced/
    Presets/
      tak-enhanced.preset.json
```

Nothing in `Mods` is used until you choose a mod set. The game on its own,
called vanilla, is always one of the choices.

## TAK Enhanced

TAK Enhanced installs a `Mods` folder of HPI patches and a
`TAKEnhanced/Presets` folder that says which of them go together. Unpack
its release into the game folder as its own instructions say. OpenKingdoms
reads the presets and offers each one whose mods are switched on as a mod
set, under the preset's own name.

Only the HPI patches are used. TAK Enhanced also replaces `Kingdoms.exe`
and adds `TAKEnhanced.dll`, which bring health bars, a melee fix and a
higher unit cap to the original game. Those are changes to the original
program, so they do nothing here. OpenKingdoms ignores both files.

## A mod of your own

Make a folder under `Mods` and put files in it at the same paths they have
inside the game's archives. `units/arasword.fbi` in the folder replaces the
Aramon swordsman's unit file. Anything the folder does not have comes from
the game as usual.

A `mod.tdf` in the folder gives the mod a name and a version for the menus:

```
name=Tough Swords
version=0.1
```

Without one the folder's name is used. A folder mod can also hold `.hpi` or
`.ufo` archives. They are read in name order, and the loose files in the
folder win over them, so you can ship an archive and fix one file beside
it while you work.

Edit a file, start the game again, and the change is in. There is no
packing step.

## Choosing a mod set

In the browser, pick your game folder as usual. If it has mods in it, a
Mods list appears above the Play button. Your choice is remembered for the
next visit.

On the desktop, start the game with `--mods` and the set's id. The choice
is saved, so later starts use it until you choose again:

```
tak-re --list-mods
tak-re --mods tak-enhanced
tak-re --mods vanilla
```

`--list-mods` prints what it found, with the id to use:

```
vanilla                  Vanilla (game, 0)
other-mod                Other Mod (preset, 3)
tak-enhanced             TA:K Enhanced (preset, 13)
tough-swords             Tough Swords 0.1 (folder, 1)
```

`--mod-root <dir>` looks for `Mods` and the presets in another folder
rather than the game folder, so a collection of mods can live apart from
the install.

The main menu's version line names the mod set in play, for example
"OpenKingdoms v 0.3.4 with TA:K Enhanced".

## Which file wins

The order, from first looked at to last:

1. Loose files in a folder mod. With more than one folder in a set, the
   last listed wins.
2. Archives in the mod set. A later one wins over an earlier one.
3. The game's own archives, where the newest file wins as in the original.
4. The loose extracted tree set with `TAK_DATA_DIR`, for files no archive
   has.

A mod archive always wins over the game, whatever dates its files carry.
An archive that will not open is skipped with a warning in the log.

## Mods and multiplayer

Every player's game data is fingerprinted when they connect, in five
parts: units, weapons, features, scripts and AI. Maps, sounds, pictures and
models are not in it, so a mod that only changes how things look or sound
plays with anyone.

A game hosted with a mod set carries the set's name, for example
"Zach's game, TA:K Enhanced". Selecting a game in the list says whether its
data matches yours. A game whose gameplay data differs is greyed and cannot
be joined, because the two machines would drift apart. To play a modded
game, everyone chooses the same mod set.

`--data-report` prints the fingerprint and every file behind it, which is
the quickest way to find out why two installs disagree.
