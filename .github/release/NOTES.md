OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release adds a mod list with one click installs, brings the Zhon
jungle back to 46 maps, smooths out big battles and draws a unit under
construction the way the original does. Everyone in a room has to be on
this version, so refresh the page after the update.

## Mods in one click

The new [mods page](https://openkingdoms.net/mods.html) lists mods you
can install with one click, each credited to its author. TA:K Enhanced by
DeeKay installs from there, and The New Era by Sage comes with a short
note on installing it by hand. A mod lands next to your game files and
shows up under Mods, above the Play button. If you join a room that plays
a mod you don't have, the game offers to fetch it first.

On the desktop, `--registry` lists the same mods, and `--install-mod` and
`--remove-mod` add or remove one. (#366, #367)

## The Zhon jungle is back

Two of the game's own scenery files have small typing slips. OpenKingdoms
threw both files out, where the original reads straight past them. So the
Zhon jungle plants and ruins were missing from 46 skirmish maps and some
campaign maps, among them Path of Pardu, Ulasem Arena, Thorn Boscage and
Temple of Blood. Over 5,000 pieces of scenery are back, and the book
font's letter spacing now loads as well. (#368)

## Smoother big battles

In an eight player battle on Ulasem Arena the slowest tick went from
107 ms to 28 ms. Every corpse used to throw away the route planner's work
for the whole map, and now only the patch it lies on is redone. Route
data is prepared while the map loads, a computer player no longer checks
thousands of inland spots for a shipyard, and when many units look for a
route at the same moment, some of them wait a tick. (#370)

## Units under construction

A unit being built now shows from the moment its site is placed, as the
original's Intangible Mass. It is a flickering silhouette in the side's
build colours that grows stronger up to half built, when the unit itself
appears beneath it, and fades away as the build finishes. (#365)

## Also

On the first visit after an update, the site could stop with "The engine
stopped: ASM_CONSTS[code] is not a function" until you reloaded. A page
now always gets its script and engine from the same build. (#364)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on an older
or newer version is greyed in the list with the reason. After an update,
refresh the page before you host or join, and have everyone else in the
room do the same. Desktop builds older than this one cannot join games
hosted with it, and the other way round. Replays play only on the version
that recorded them.
