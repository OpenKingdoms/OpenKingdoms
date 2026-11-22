OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release fixes what playtesting found in 0.6.0. Battle chat is larger
and easier to read, and it now reaches the other players in a match. Units
fire all their weapons, flyers make attack runs, Sea Forts on tight water
launch their ships, and computer players keep their factories busy. All
players need 0.6.1: browser players just reload the page.

## Chat

- Battle chat is larger and scales with the screen. A line shows the
  sender's team badge, their name in their player colour and the message
  in a paler tint, on a soft dark plate. (#504)
- Battle chat in a multiplayer match now reaches every player in it, and a
  watcher's line reaches only the other watchers. (#504)

## In battle

- A unit that does not switch weapons fires every weapon, each at a target
  of its own, as in the original. That covers the gods, the Ghost of
  Garacaius, towers with two bows, the Rolling Tower, the Dirigible, the
  Aerial Juggernaut and the Iron Clad. (#505)
- A unit walking to a target out of its reach answers what shoots it.
  (#505)
- Routes go over a hill a unit can climb and round bad ground, as in the
  original. (#505)
- Flyers without a hover attack make attack runs past their target, as in
  the original: the Dirigible, Gryphon, Fallen Angel, Aerial Juggernaut
  and the Wrath of Tammuz. A Dirigible ordered at a keep now does real
  damage. (#506)
- Dropped stones and eggs fall onto their target in under two seconds
  instead of crawling along the ground. (#506)
- Musketeers fire one shot at a time, paced by their scripts as in the
  original. The Grenadier and the Trebuchet ship are paced the same way.
  (#506)
- Parrots and other flyers keep their speed on patrol. (#506)
- The Rictus rides the sea surface. (#508)
- A tower fires only once its turret has turned onto the target, as in
  the original. (#512)
- A landing flyer keeps its wings up and its tail down until it is down.
  (#513)

## Movement

- A unit's step tests its whole body, as in the original, so a catapult
  stops at the foot of a ramp it does not fit on instead of hanging over
  the cliff. (#515)
- A unit wedged at a thin ridge or in a pocket is routed out along ground
  it can walk. (#511)
- Units sent through a gap one body wide keep trying and find their way
  through or round, instead of giving up at the wall. (#515)
- Route checks are faster on maps with many features. (#516)

## Ships

- A Sea Fort on tight water launches its whole queue at far more sites,
  and ships caught bow to bow get past each other. (#495)
- A ship mending another holds its heading instead of swinging round at
  once, and a builder on foot turns onto its work at its own rate, as in
  the original. (#495)
- A Repair Skiff at a ship's bow mends it. (#495)

## Computer players

- A computer's castles keep training cheap troops when its mana runs
  short, so its factories no longer stand idle. (#509)
- A computer's units give up a chase at their leash and walk back, so a
  monarch can no longer lure them into her base. (#509)
- Zhon's computer keeps training its army instead of spending everything
  on beast producers. (#509)

## Campaign

- A mission's players take the colours the mission gives them. (#507)
- Animals on a neutral side are left alone unless you order an attack or
  they bite. (#507)
- A mission's unit list holds, so Black Peace no longer offers the dragon,
  and its wolves no longer block the win. (#507)
- Kill counts in a mission's goals count down, and a goal once met stays
  met. (#507)

## Keys

- The original's keys work in battle: M Move, B Attack, G Guard, P
  Patrol, L Load, U Unload, H Heal, C Clear, N Stop and O for the gate.
  W, A, S and D still pan, which is why Attack and Stop sit on B and N.
  (#514)
- Ctrl+F1 to F3 pick a weapon. F5 to F8 jump to a camera bookmark and
  Ctrl+F5 to F8 store one. Pause pauses a skirmish or a mission, F2 shows
  a mission's briefing again, F12 clears the chat, and Ctrl+S and Ctrl+L
  open save and load. (#514)

## Controls and display

- A plain click on a build button queues one again after a Shift-click.
  (#508)
- Hovering a build button shows the unit's cost. The rank shield shows
  again in the bottom panel. (#508)
- A click on a radar blip on the minimap attacks it. (#508)
- A unit's queue holds 32 orders, so patrol routes can be longer. (#508)
- The Man of War's sails show in front of its rigging, and the harpoon
  looks like wood again. (#506, #508)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
