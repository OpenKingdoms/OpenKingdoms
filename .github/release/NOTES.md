OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release makes a multiplayer battle that falls out of step catch up in
seconds instead of replaying the whole match. All players need 0.5.5:
browser players just reload the page.

## Rejoining in seconds

When one player's battle falls out of step with the others, it no longer
starts again from the first second and replays the whole match behind the
"Rejoining the battle" screen, which late in a long game took a minute or
more. The game server now asks another player's game for its battle as it
stands, passes it on, and the player who fell out of step carries on from
there with only the last few seconds to catch up. Their camera and the
units they had selected stay as they were, and everyone else plays on
without a pause. When two players disagree and there is no way to tell
which is right, the host's battle settles it, once a game. Such a game
still goes on the record but does not count for the standings, since a
host could change a battle that way. If a battle cannot be brought back
this way, for instance because the other player left at that moment, the
old way of replaying the match is still there as a fallback, and a battle
that keeps falling out of step is never sent round in a loop. (#446)

## Smaller changes

- A unit fading out of a magic death is now kept in a saved game, so a
  battle saved at that moment loads exactly as it was. (#446)
- A game on another version of OpenKingdoms now says to reload the page
  or update the game, and a full game says it is full. (#446)
- In the browser, the "Add 3D models" button now takes .3do models picked
  one by one, as dropping them on the page already did. (#446)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
