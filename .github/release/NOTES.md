OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release brings back the original's select keys, fixes lodestones that
refused to go down beside a busy sacred site, and makes sure the site always
runs the newest engine. Everyone in a room has to be on this version, so
refresh the page after the update.

## Select keys

The original's select keys are in, as its key file binds them:

- Ctrl+Z adds every unit of the types you have selected, across the whole
  map. It only ever adds to the selection.
- Ctrl+A selects all your units, and Ctrl+U your units on the screen.
- Ctrl with a letter selects a kind of unit: B builders, E melee, F
  factories, G magic, M your monarch, N boats, R ballistic, W armed and Y
  flyers. Hold Shift as well to add them to the selection.
- Ctrl+Shift with a number adds that group to the selection.

With Ctrl held, W, A, S and D no longer scroll the map. In a browser, Ctrl+W
and Ctrl+N still belong to the browser. The README lists every key. (#378)

## Lodestones beside a sacred site

A builder working on one lodestone could stop a few pixels over the edge of
the next sacred site, and that site then refused every lodestone. As in the
original, a unit now only blocks the ground it stands on, so the next site
stays free. (#378)

## Always the newest engine

After an update, a returning visitor used to get the old engine for one
more visit while the new one downloaded, and a tab left open never updated.
The site now checks for a new version before it starts, and an open tab
takes the update the next time it is on the menu or the game list, never in
a room or a battle. With no network, the game still starts from what the
browser saved. (#377)

## Also

The engine can keep a lost skirmish going between the computers, for a
front end that lets a beaten player watch the rest of the battle. (#376)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
