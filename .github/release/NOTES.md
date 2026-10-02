OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

Battles now play much closer to the original. Shots stop on what they fly
into, swordsmen and archers fight the way they did in 1999, veterans grow
stronger, and factories and Shift queues work as the manual describes.
Multiplayer gains pings, replays, a fairer leaderboard and a note when
someone hosts. A battle can draw at the original's own pixel size, and big
eight player games run faster. Everyone in a multiplayer room has to be on
this version, so refresh the page after the update.

## Shots and targeting

Arrows, bolts, fireballs, lightning and flame stop on the first hill,
wall, tall tree or rock, stretch of sea or enemy unit in their path, as in
the original. An ally's unit or wall stops a shot but takes no harm from
it, and your own walls never stop your own shots. Catapults still lob over
walls, and area spells and dropped bombs still pass over the ground. (#328)

A unit only picks targets its side can see. A unit that is hit still fires
back at a shooter within its reach. No shot or splash harms the unit that
fired it, and a flyer that has taken off holds its fire until it reaches
its cruising height. (#328, #329)

## Melee, archers and veterans

A swordsman closes all the way to its target, even one standing behind
another. Archers and crossbowmen pick their targets at random among the
enemies in reach, and each arrow leaves at the point in its animation
where the original released it. A caster whose target dies mid cast keeps
the mana. The Use Crusades Units option now works. (#334)

Veterans now hit harder and take less damage as they gain experience, as
in the original. Units carry out their standing order, a fight a unit
started for itself looks for a better target now and then, and the
computer sends its armies out on the attack. (#342)

## Orders and factories

The build buttons work as in the original. A click queues one unit, Shift
and click queues five, and Ctrl and click trains that unit without end,
shown as +++ on the button. A right click takes off the last one queued.
Factories keep training through any order, and every unit a factory
finishes walks to the rally point. (#332)

Hold Shift to queue orders, up to sixteen. While Shift is held you see
where the selected units' orders will take them, and the buildings a
builder has queued. A right click on a builder's build button drops its
queued buildings of that kind, and the one in hand too. (#332, #337)

A builder now walks to its site by a proper route, as a move does, instead
of heading straight at it. A building still going up can no longer be
selected, and a click picks a unit only inside its outline. (#335, #332)

Typed + commands work. Press Enter, type a line starting with + and press
Enter again. The power codes, such as +ATM, +NOWISEE and +IWIN, work only
when the room allows cheat codes, and everyone sees the line. Settings
such as +CLOCK, +NOSHAKE and +SCROLLSPEED work in any game. (#349)

## Setting up a game

The map's picture in the skirmish screen shows its start positions. Click
one to claim it, click again to give it back, or drag a player from one to
another. The map list has a search box, with choosers for the number of
players, the map's size and the sort order. (#333)

## Multiplayer

The room list shows each host's ping and your own. When a battle slows
down for a player whose turns arrive late, it says whom it is waiting for.
(#346)

A switch on the front page shows a browser notice when someone hosts a
game, from any open tab of the site, and a click on the notice joins it.
It asks first if you are already in a battle. (#345, #309)

The leaderboard now knows players by the browser they play from, so
nobody can take over another player's record by using their name. It can
be searched by player, map and date, and shows the games being played
right now beside the finished ones. Records kept by name before this
version stay as they were. (#347)

A dropped player can rejoin the match, and a stalled battle says whom it
is waiting on. (#315)

## Replays

Every new battle is recorded as you play it. Choose Replays on the main
menu to watch one again, with pause and speeds from 1x to 8x and a free
camera. A replay only plays on the version and game files it was recorded
with, and says so if they differ. (#352)

## The original's view at any screen size

A battle can draw one game pixel to one screen pixel at your window's
size, with the sidebar, minimap and bottom strip at the original's size
and in its places, so 1280x600 shows what the original showed at
1280x600. The Resolution slider on the Visual options page picks this or
Fit, the old stretched view. New players start on the original's view,
and saved options stay on Fit until you move the slider. (#336)

The menus and dialogs line up better at every size. The Saved games panel
opens over the menu and keeps clicks off it until you close it. The
Forget my game files link now asks before it removes anything, and it no
longer sits where a click meant for Host Game could land on it. (#306,
#350, #353)

## Maps, buildings and big games

A building's slope is now judged across its whole footprint, as in the
original, and water cells take no slope test. A building can stand flush
against a blocking feature on its east or south side, and is refused on
every edge row of the map. Units keep off ground the map marks as
impassable. (#341, #326, #321)

The unit pool holds 8192 units, up from 2000, so eight full sides fit on
one map. Route planning and the computer players' thinking cost much less
in a big game. (#331, #338, #343)

A computer player short of mana holds back what earns it nothing. (#307)

## The 3D view

Buildings can be placed turned by quarter turns in the 3D view, and a
turned building holds together. The flat floors of keeps and castles lie
whole on the ground under their whole extent, and so does the build
preview. Map features no longer load with a random tilt. Dragon breath now
starts at the dragon's head. (#322, #323, #324, #348, #320, #354)

## Also

The game folder picker finds the game when you pick the folder above it,
and a return visit starts at once. (#308, #314)

Two bugs that only showed on 32-bit builds, such as the browser's, are
fixed: a long multiplayer message could be framed wrongly, and the classic
camera preset could start in the wrong place. (#351)

A game saved in an earlier version may not load in this one, since
battles now play differently.

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round.

## For testers

The desktop build takes `--map`, `--seed`, `--los`, `--scout` and
`--fog-dump` to start a fixed skirmish and write out the fog of war.
`--mission` starts a campaign mission straight away and `--scale
original|fit` picks the battle's scale. (#325, #336)
