OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

## The story is playable

This release is about the campaign. Open the Book of Deeds, pick a
chapter and play it the way the original plays it, from the first
chapter of the Book of Darien to the last of The Iron Plague. Here is
what was missing and is now in.

A mission is three files, not one. Beside each mission's map the game
ships a script and a page of text, and the engine loaded only the map.
The script is what makes a mission a mission. In the first chapter the
hero you are sent to protect, Emen, is nowhere on the map: the script
creates him, sets the town's defenders waiting for you, and calls the
mission lost the moment he dies. Without it the chapter could not be
won. Every mission's script now runs, the 43 of the base game and all
20 of The Iron Plague, and each is held to the same set of commands.

A garrison waits. A unit placed by a mission carries a list of orders,
and the engine had fired the whole list the moment the map loaded, so
every guard on every map charged at the first second. The list is now
worked through one order at a time, waits and all. A garrison told to
wait half an hour or until an enemy comes near does exactly that.

A unit keeps its owner. The order that sets a unit's standing orders,
hold, defend or roam, had been read as a change of owner, which handed
Veruna's three transports in the fourth chapter to the player as the
map came up. They are Veruna's again.

The briefing. A chapter opens paused under the panel the original
shows, the chapter, its title and what the mission asks of you. A click
or a key puts it away and the clock starts.

The clips. A chapter plays its clip before it begins and, where the
campaign has one, after it ends. In a browser that means the campaign's
clips come along with the game folder now, read in place a piece at a
time like the intro and the credits, so nothing is held in memory. If
you keep a copy of your game files in the browser and made it before
this release, pick the folder again to bring the clips in.

The hero is as tough as the mission makes him. Seven missions give
their hero more armour from the script, Emen half again, others two or
three times over, and that was being read and dropped. It now holds, so
the hero of a chapter no longer dies two or three times too fast.

Winning a chapter opens the next, the book remembers where you are, and
the next chapter starts from the book with its own clip. This was
played through in a browser on the live page's own code, from the book,
for the first two chapters.

## Also in this release

A weapon that shakes the ground shakes the view. The Acolyte's
earthquake, the Dragon and the god of Aramon author a shake, and it is
drawn where the shot lands, with the camera left where you put it.

The AI fights in numbered groups. Its fighters gather into attack
groups and raid groups, launch when full, keep their own target, break
off only when outnumbered more than two to one, and drop a badly hurt
member to walk home. A hurt builder makes for home. Its towers go up
towards the threat rather than behind its own keep. Over twelve games
against the previous AI it won seven.

The AI plans. A goal planner picks what to build and a task network
turns an attack into scouting, massing, striking and holding, with the
army weighing its strength against what it can see before it commits.
An expansion is not placed under the enemy's feet.

Two units about to walk into each other each give way, and a route
through a tight gap is planned on the placements a unit can actually
take inside a cell, so a monarch no longer stalls in a doorway.

The music settings and the volume sliders do what they show.

The window title names the screen and no longer carries a frame rate.

In a browser the shadow pass no longer asks the graphics driver the
same question five and a half thousand times a minute.

Saved games from 0.2.0 do not load in 0.3.0. The unit record grew, and
the format refuses a save from an older version rather than guess at
it.
