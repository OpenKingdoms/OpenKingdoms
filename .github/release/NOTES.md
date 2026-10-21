OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release lets players whose game folders differ play multiplayer
together again. All players need 0.5.6: browser players just reload the
page.

## Loose models no longer keep players apart

Some copies of the game, the GOG one among them, have an objects3d folder
of 20 building models beside the archives, and others do not. Those models
decide where a blast reaches a unit, so two players whose folders differed
fell out of step minutes into a match, and 0.5.4 and 0.5.5 refused to put
them in the same game. A multiplayer battle now uses only the models in
the game's archives, whatever the game folder holds. So do watchers, a
battle brought back into step from another player's game, and the replay
of a match. Players with and without the folder can host and join each
other again, and the lobby no longer asks anyone to remove it. A skirmish
or a campaign still uses the folder's models as before. In the browser,
models added with "Add 3D models" or with the game folder are used outside
multiplayer only.

If you moved your objects3d folder out of the game folder to play 0.5.4
or 0.5.5, you can put it back. (#449)

## Smaller changes

- A player on another version of OpenKingdoms who joins by code or link
  is now told to reload or update before anything is said about their
  game data. (#449)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
