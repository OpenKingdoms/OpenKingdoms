OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release works through a round of playtest reports: building at a
distance, invisible lodestones, units that could not be hurt, a campaign
mission that could not be won, and computer players that sat on their
islands. All players need 0.5.8: browser players just reload the page.

## Building

- A building goes up only once its builder stands at the site, as in the
  original. Before, an order put an invisible frame down straight away.
  That let a builder start a building from across the map, left invisible
  lodestones behind an interrupted order, and could keep you off a
  lodestone site a computer player had only ordered. A later builder of
  the same side joins a frame it finds there. (#488)
- A factory opens its yard before it trains, and the doors play. A unit
  on a factory's pad can be hit now, so a unit stuck there is no longer
  safe until the building falls. Idle units are sent off a yard. (#490)

## Transports and the campaign

- Units aboard a transport stay in the battle. Builders on a ship keep
  paying in their mana. Riders die with a lost transport, as in the
  original, and that includes a monarch aboard. (#489)
- Mission 3 of the Book of Darien is no longer lost the moment Joreth
  boards the transport. (#489)
- Seven campaign missions whose script commands did nothing now run them.
  (#489)

## In battle

- Crossbowmen and other ranged units stop stuttering as they aim. (#491)
- A melee unit on its way to a far target takes what walks past it. Units
  on a move order still walk on, which the original does not do. (#491)
- A ranged unit whose shots are blocked walks closer until they land, and
  a fixed unit such as a Trebuchet passes over a target it cannot hit for
  a minute. (#491)
- Water and roads now set a unit's speed. Most ground units wade 19
  percent slower and move 21 percent faster on roads, and the Swamp Beast
  goes about twice as fast in water. (#491)

## Computer players

- A computer player spaces its base by the original's grid, so its
  buildings and builders pile up far less. (#492)
- A computer player cut off by water keeps crossing rather than stopping
  after its first boat, trains warships and fights at sea, sends more
  boats for a large army, and lets a builder ride to a site it cannot walk
  to. It lands more of its army, and it pays for that in losses. (#492,
  #493)
- Units at a computer player's home no longer take orders on targets over
  water and stand stuck for the rest of the game. (#493)

## Multiplayer and the site

- The host of a battle room can set a computer player's team, side and
  colour. (#476)
- The home page says less, and its text no longer covers the Start
  button. (#475)

## For modders

A sight jammer now lays a pale haze on the ground it covers, in both
views, for its owner, its allies and anyone watching the whole map. Those
it jams see only the fog. A jammer with no script and activatewhenbuilt
now comes on when built. (#494)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
