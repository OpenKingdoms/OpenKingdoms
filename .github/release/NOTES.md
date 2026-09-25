OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

## Mods

This release is about mods, and about getting into a friend's game.

TAK Enhanced works. Unpack it into your game folder as its own
instructions say, and its presets show up as mod sets you can choose. In
the browser a Mods list appears above the Play button once your game
folder has mods in it. On the desktop, start with `--mods tak-enhanced`,
or `--list-mods` to see what is there. Only its HPI patches are used, so
the features that come from its replacement exe and DLL, like health bars,
are not part of it here.

A mod can be a plain folder. Make a folder under `Mods`, put a changed unit
file in it at the path it has in the game's archives, and that file wins.
There is nothing to repack. A `mod.tdf` in the folder gives it a name and
a version. [docs/MODDING.md](https://github.com/OpenKingdoms/OpenKingdoms/blob/main/docs/MODDING.md)
has the details.

The main menu says which mod set is in play, and a game you host carries
its name.

## Playing together

Every player's game data is fingerprinted when they connect. A game whose
gameplay data differs from yours is greyed in the list, and selecting it
says why, so a modded game and an unmodded one no longer drift apart ten
minutes in. Mods that only change pictures, sounds or maps play with
anyone. `--data-report` prints the fingerprint and every file behind it.

A new game puts an invite link at the top of its chat and on your
clipboard. Anyone who opens it lands in your game once their game files
are loaded. A desktop build joins the same way with `--join CODE`.

Desktop builds older than this one cannot join games hosted with it, and
the other way round. Update both sides. Saved games from 0.3.3 load in
0.3.4.
