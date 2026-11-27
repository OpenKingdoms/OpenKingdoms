OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release answers the latest playtest reports, with five changes from
Danvern. Online rooms share player-made maps and run scripted maps,
keys can be rebound, a group of builders works together, and the
computer plays its side of the campaign missions and crosses water to
reach a player on an island. All players need 0.7.0: browser players
just reload the page.

## Online play

- A player without the room's map gets the host's copy before the
  battle when a player made it. A map that ships with the game or an
  expansion is never sent, and the room says which release the player
  needs. Choosing the same map again no longer clears who has it. (#551)
- Scripted maps such as King of the Hill play their own start units and
  rules online when the room's Use Map Script box is ticked. (#550)
- Options opens from the online room, and the room keeps up with chat
  and the host's start while it is open. (#547)

## Controls and display

- Keys can be rebound in Options, starting from the current layout, the
  original's or a modern one. (#543)
- The arrow keys, Home, End, Page Up, Page Down and the mouse wheel walk
  the map lists. (#547)
- The Resolution slider works during a battle, and the browser offers
  real screen sizes scaled to fit the page. Fit stays the default. (#549)
- A group of builders all take a build order and raise the building
  together. (#552)
- Holding Shift also shows the orders of the unit under the pointer, and
  with a builder in focus, the buildings every other builder has
  queued. (#552)
- The under-attack alarm sounds for units off screen, selected or not,
  with each kingdom's own sound. (#546)

## Units, fog and the classic view

- Units on high ground see further, and the fog is drawn where the
  original draws it. (#541)
- Walls are hidden on explored ground out of sight. (#542)
- A unit under construction holds the pose its script gives it. (#540)
- A unit stays cloaked near neutral units. (#539)

## Campaign and the computer

- Each campaign mission opens on its kingdom's shield before the
  mission's own clip. (#546)
- The computer plays its side of a campaign mission once each unit's
  orders let it go, so the Aramon mage of Nightmare Legions builds and
  the Zhon of Unwelcome Guests move. Buildings a mission places and units
  its script makes stand where the original puts them. (#548)
- A computer cut off by water ferries its army across even when a few of
  its units can wade, its warships escort the crossing, its armed
  transports fire back and its flying transports land clear of the
  strongest defences. (#553)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
