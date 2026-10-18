OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release fixes Kirenna's Water Blast, which 0.5.1 broke, and adds the
Asset Atlas, a page for modders that browses the game's art. Everyone in a
multiplayer room has to be on this version, so refresh the page after the
update.

## Combat

Kirenna's Water Blast goes off again when you cast it at a spot she has to
walk to. Since 0.5.1 she walked into reach and stood there with her mana
full. A unit whose script never says it has aimed now fires after two
seconds once it has stopped with its target in reach, as it did before
0.5.1. Every other weapon still waits for its script as in the original.
(#438)

## Asset Atlas

[openkingdoms.net/atlas.html](https://openkingdoms.net/atlas.html) lists
every picture, animation and 3D model in your copy of the game, with
search, a preview and a 3D view. It reads the game files the game page
already keeps in your browser, or asks for your game folder. Nothing
leaves your machine.

Models and scenery that have a Stars of Darien remaster stand beside it
under one camera. You can keep notes, ratings and tags on any asset and
share them as a file, export a picture, an animation's frames or a model,
and see which file a new model has to replace. (#437)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
