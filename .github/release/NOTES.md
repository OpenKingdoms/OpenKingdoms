OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release finds and fixes why multiplayer games fell out of step, and
lets a beaten player stay and watch the rest of the battle. Everyone in a
multiplayer room has to be on this version, so refresh the page after the
update.

## Multiplayer games that fell out of step

Some copies of the game, the GOG one among them, have an objects3d folder
of 20 building models beside the archives, and the game uses those models
in place of the archives' own. A model decides where a blast reaches a
building. So a player with that folder and a player without it played
slightly different battles, and the game stopped with "the players'
battles no longer agree", often many minutes in. The lobby now checks
those models too. When they differ, the game is greyed in the list and
says which folder and how many models, and the host's room says who could
not join and why. Both players need the same files: both include the
objects3d folder, or both leave it out. In the browser, Choose game folder
brings the folder along and Choose game files does not. (#444)

When two battles do fall out of step, they are rebuilt from the game
server's record instead of stopping. The rebuilt player sees "Rejoining
the battle" until they are back at the present, and everyone else plays
on without a pause. A battle that falls out of step again after two
rebuilds is not rebuilt a third time. The computer takes over that
player's army and they watch, and the message line says so. (#443, #444)

## Watching after a defeat

When your monarch falls with Monarch Expendable off, your army goes with
it, and you now stay to watch while the others fight on. The Defeat
banner shows for three seconds, then the whole map shows with no fog.
Tab looks through each player's eyes in turn and then back to the whole
map, as it does for a watcher. In a multiplayer game your own fog used
to stay up, and with nothing of yours left on the map you saw nothing of
the battle. In a skirmish the statistics screen now waits until one side
is left. (#441)

A beaten player gives up their seat to watch, so their battle is checked
like a watcher's and never decides whether the players agree. (#444)

The leaderboard records a beaten player who leaves before the end at
the moment they were beaten, so leaving early never lifts anyone's
place. (#441)

## Messages

The line that names a fallen player now shows every time a player or a
computer falls, the moment its army goes, in a skirmish or a multiplayer
game. Before, it could be missed when the fall came early in a battle or
while a battle was ending. (#441)

A short low horn over a muffled drum now sounds with that line, for every
player who falls. The original asks for a message sound there that its
game files do not include, so a fall used to pass in silence. The new
sound is made by the game itself and follows the sound volume. (#444)

When a multiplayer game stops for good, because the connection to the
game server dropped, the message line now says so instead of leaving the
battle standing still. (#441)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
