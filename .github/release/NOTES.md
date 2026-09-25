OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

## Smooth in the browser

A big skirmish in the browser could freeze for half a second at a time,
and the sound stuttered with it. The worst tick in an eight player game
went from 565 ms to 24 ms, and no frame now runs long enough to starve
the sound. What was slow:

- a computer player looking for somewhere to build tested every tree
  and rock on the map for every spot it tried
- the check two machines use to agree on a game ran every tick, even
  with nobody to agree with
- the radar drew its fog one square at a time, thousands of draws a
  frame on a map nobody had explored
- a group move worked out routes over the whole map every time anyone
  built anything
- all the computer players thought on the same tick. They now take
  turns through each second, as in the original

## Mana as the original has it

The mana economy was checked against the original line by line, and
these now match it:

- finishing a building raises your mana cap and pays nothing into the
  pool. A finished lodestone used to hand over 1000 mana on top of the
  280 it cost
- a lodestone earns only when it covers its sacred site, and then at
  the site's rate
- a unit given away, captured or raised takes its storage and income to
  its new owner
- sharing mana with an ally now does something: a pool over half full
  passes some of what is over to the players it shares with
- a gift sends what fits in the ally's pool and you keep the rest
- a caster's own mana starts empty and fills over time, so a monarch
  waits a little before its first spell. Units a mission places start
  full unless the mission says otherwise

Clearing trees and rocks still pays mana, which the original does not.

## Also

The front page shows how many players are online and the games open
right now, with a Join button on each, before you pick your game files.

A loaded save heals units on the same ticks the saved game would have.
Saved games from 0.3.4 load in 0.3.5.

Desktop builds older than this one cannot join games hosted with it,
and the other way round. Update both sides.
