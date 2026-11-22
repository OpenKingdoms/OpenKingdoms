OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release is mostly parity work from Artificer-111. The classic view
draws closer to the original, with lit models, textures on the original's
clock, mist in the fog, shadows under walls and statues, and models drawn
against their own depth. Unit scripts keep the original's timing, missions
run their patrols and standing orders as written, and the cursor, the info
panel and the build keys behave as they do in the original. All players
need 0.6.2: browser players just reload the page.

## The classic view

- Model faces are lit through the original's shade ramp, animated
  textures run on its frame clock, the fog carries a drifting mist, and
  effects and selection rings look as they do in the original. (#522)
- On the GL renderers, desktop and browser, each model is drawn against
  its own depth, so the Veruna Citadel's gate and the Man of War's sails
  show as in the original. (#528)
- On GL, model faces are shaded on the graphics card from one copy of
  each texture atlas, so a battle takes far less video memory. (#530)
- Walls, stone and ice statues and corpses cast their shadows. (#529)

## Units and missions

- A unit script's sleep lasts the original's frames, so animations and
  reloads keep the original's timing. Musketeers fire every 7 seconds
  and Grenadiers about a quarter more often than in 0.6.1. (#520)
- A unit whose script runs its own walk is left to it, so walk cycles no
  longer jitter or restart. (#523)
- Nine units that were silent, among them the Darkest Priest and the
  Buck, now answer when selected or given an order, since a unit voices
  from its sound class as in the original. (#521)
- Missions run patrols of several points, attacks on a place and
  standing orders as their files write them, and placed units face the
  way their mission sets. (#519)

## Controls and display

- The cursor shows the order the next click gives, over the field, the
  minimap and the Tab map. (#524)
- The info panel describes the unit or feature under the pointer, and
  keeps its empty bars up. (#525)
- With a building armed, a Shift drag lays a line of it, and the build
  ghost follows the pointer freely. A unit holding a summons without end
  refuses Shift orders, as in the original. (#526)
- Ctrl+Shift puts an order in front of the queue. (#527)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
