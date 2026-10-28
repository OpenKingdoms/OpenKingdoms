OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release keeps a multiplayer match together when a player's tab goes
to the background or their connection drops, and brings a long list of
the original's battlefield behaviour to the engine. All players need
0.5.7: browser players just reload the page.

## Multiplayer stays together

- A browser tab in the background, a minimised window or one on another
  desktop keeps playing. Before, the game went quiet, the match stopped
  for everyone with the waiting countdown, and the player was cut off.
  (#466)
- A battle that loses its connection now reconnects by itself and is
  handed the match back. While the server still holds the seat it tries
  again every few seconds. Before, the battle stood still without a word
  until the page was reloaded. (#466)
- Orders given after coming back into a match count again. Before, every
  order from a player who had reconnected was quietly ignored. (#466)
- A match can be rejoined or watched however long it runs. A player who
  comes back, a watcher and a player who drops in now take another
  player's game as it stands and the turns after it, so they are back in
  seconds rather than replaying the whole match from the start. (#447)
- A finished match stays finished. After a win, a loss or leaving a
  match, opening Multiplayer goes to the games list instead of back into
  the battle to watch it again, and a decided game leaves the list.
  (#472, #473)

## Loading the page

The page says what it is doing from its first moment, shows how much of
your stored game files it has read, and no longer sits on a black screen
or freezes on a return visit. If the engine cannot download or start, the
page says so. (#468)

## In battle

- Sky Knights and other flyers hold the point they picked instead of
  being pushed back by walkers, and a Dark Priest crowded by flyers goes
  on building, as in the original. (#451, #453)
- A shot at a ground point leaves when the unit's attack script lets it
  go, as a shot at a unit does, and one drawn at a point the order then
  moves out of reach is called off. (#455)
- Radar shows other players' units as blips on the minimap beyond sight,
  as in the original. Mods can give units radardistancejam and
  sightdistancejam to hide units from radar and fog the ground round a
  jammer. (#319)
- The Sea Fort, the Floating Tower and the Iron Plague Navy Yard float at
  their waterline instead of sitting on the sea floor, and a Sea Fort
  launches ship after ship instead of stopping at its first. (#457, #458)
- Damaged units burn and smoke by the original's rules, and steam, the
  purple flash of a change of form, the Pillar of Light's light, and the
  wakes and thrust of units that wade, swim or fly over water are drawn.
  (#381)
- A selected unit that dies no longer takes the orders panel with it, so
  Patrol and the other orders stay for the rest of the selection. (#452)
- The Sacred Fire's Active and Inactive buttons work, and the buttons
  light the state the selection shares. (#456)
- Acolytes, priests, the Pillar of Light and the Sacred Fire heal what
  stands round them, and the Titan and the Shaman raise the armour and
  attack of what is round them, as in the original. Healing is paid for
  in mana at what the healed unit cost to build, so a wounded monarch
  beside a healer can take the whole treasury, as it does in the
  original. (#456)
- A computer player cut off by water builds a transport, or a yard to
  make one, and carries its army over to attack. (#454)
- Units show their action and construction feedback again, and selection
  rings are animated. (#371)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
