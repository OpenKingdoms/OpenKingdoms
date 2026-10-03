OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release gives ships room to sit side by side and fixes attacking
flyers. Everyone in a room has to be on this version, so refresh the page
after the update.

## Ships keep apart

In the original, every ship gets a small square on the map, while its hull
is up to three times longer. So fleets piled on top of each other. A ship
now takes up the shape of its own hull, long and thin, so a fleet sent to
one spot settles side by side without overlapping, and ships moving
together stay clear of each other.

A shipyard waits for its pad to be clear before it starts the next ship,
and sends each finished ship out to open water. Route planning still uses
the old squares, so narrow straits stay open, though a fleet takes a little
longer to file through one. Land, hover and flying units are unchanged.
(#373)

## Attacking flyers

In the 3D view, pointing at a dragon or another flyer found nothing unless
the camera was at its default tilt, so you got the plain pointer and a
click became a move. A flyer is now picked where it is drawn, in the air,
from any camera angle.

The cursor now follows the original's rules for each selected unit. You get
the attack cursor when any of them can hit the flyer, and the red cursor
when none of them can, as with cannoneers and war galleys. On a click, the
units that can hit it attack, armed units that can't keep what they were
doing, and unarmed ones walk to the spot.
(#374)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
