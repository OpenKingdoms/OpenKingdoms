# Fog, sight and radar

What each player can see and what that decides: the sight and explored
maps, the Line of Sight and Map Revealed options, the fog cell a unit
stands on, what radar adds, how a unit cloaks, and why a side's units
take on only what it sees.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## The two maps

Each player has two maps over the fog grid, whose cells are 32 pixels
square.

The sight map is a count of how many of that player's units cover a
cell right now. A unit's reveal stamps into it and out of it as the
unit moves.

The explored map is one 16 bit word per fog cell, one bit per player,
recording ground that player has ever had in sight. Bits are only ever
set, never cleared.

Both are set up at load (legacy:167179). A unit's sight entry records
its position, its type's eye height and its sight range
(legacy:167249). The original scales a unit's sight radius by the
height of the ground it lights, and the engine departs there, see
D-014.

A unit's fog cell is height compensated. Its column is x over 32 and
its row is z less half the unit's height, over 32, that is
`(z - y/2) / 32` (legacy:208685, legacy:208695,
legacy:250583-250596). Fog reveal, the minimap (legacy:208475) and the
fire gates all use this one mapping, so a test that takes a unit's
flat position instead is wrong wherever the ground slopes.

## Line of Sight off

Battle setup offers Map Revealed and Line of Sight as two checkboxes,
and they are two separate bits in the original, packed next to each
other in the session options byte and unpacked into two bytes of the
input state (legacy:135486-135487). The game options name them
`MappingOn` and `LosOn`.

The mapping bit decides how the explored map starts and whether it
grows. With mapping off every bit is set at load, and the stamp does
not bother setting any more (legacy:167192-167202). With mapping on
the map starts empty and every unit's reveal ORs the player's bit into
the cells it covers (legacy:167404-167409). Map Revealed is the only
setup option that would start the map full, so a checked Map Revealed
is taken to be mapping off.

Line of Sight decides only how the sight map starts. With it on the
sight map starts empty and fills from unit reveals. With it off every
player's sight map is filled solid at load (legacy:167211-167219), so
every cell counts as seen for as long as the match runs.

The reveal stamp never asks what Line of Sight says
(legacy:167323-167436), and the call that re-stamps a unit that has
moved makes the add unconditionally (legacy:167466). So the explored
map keeps growing with Line of Sight off. The option grants full
sight, not a revealed map.

One visibility test serves drawing, picking, projectiles and script
sounds (legacy:206797). With Line of Sight on it reads the viewer's
sight map (legacy:206887-206892), and with it off the viewer's
explored map (legacy:206877-206884). The minimap's marks, under Radar
below, and a positional sound apply the same rule to a single fog
cell. An enemy standing on ground the player walked past long ago
therefore draws, and an enemy on ground the player has never been near
does not.

The fog overlay sorts every cell into three levels
(legacy:130295-130310). A clear explored bit gives opaque black in
both modes. The option off, or the cell seen now, gives no shading.
Anything else gets the grey level. So with Line of Sight off the grey
level never shows and unexplored ground is still black. The minimap's
terrain pass makes the same three cases in palette terms
(legacy:208407-208418). It blacks a cell whose explored bit is clear
whatever the option says, and since the sight map is full with the
option off, everything explored reads as the plain map byte. How the
overlay is drawn is in
[2026-09-04-rendering-visuals.md](2026-09-04-rendering-visuals.md).

Sound follows the same rule, a script's sound through this test and a
weapon's start cue or a shell's impact through the positional play's
own (legacy:221150). With Line of Sight off an enemy on ground the
player has never explored is silent as well as unseen, see
[sound-triggers.md](sound-triggers.md).

A side's target list and the computer player's map evaluation call the
same test (legacy:20511-20545). In its Line of Sight off branch the
test reads the explored map of the local player, whichever player is
asking. The engine does not copy this, since each machine in a match
would read a different map, and with Line of Sight off its simulation
sees the whole map.

## Radar

`sightdistance` and `radardistance` are the only range keys, read side
by side into the unit type (legacy:162920-162923). There is no sonar
and no stealth key. `radardistance` is set by 71 of the 155 base unit
files, mostly 400 to 1000 pixels, with the dragons and gods at 2000.
Sight on the same units is 145 to 500, so radar reaches two to four
times further.

Radar's one reader in play is the pass that marks which units the
local viewer sees on the minimap (legacy:208607-208711). It runs once
a tick, only when the game has more than one player, and starts from
nothing each time (legacy:208607-208710).

1. Every mark and every cloak suppression flag is cleared, and the
   viewer's own units are marked (legacy:208630-208641).
2. Each of the viewer's own units that is alive, not dying, finished
   and has a `radardistance` marks every unit within that radius
   (legacy:208642-208655). A unit is marked only when it belongs to
   another player and is not cloaked (legacy:208580-208590), so radar
   never sees through a cloak.
3. A cloaking unit with an enemy inside its `mincloakdistance` has its
   cloak suppressed (legacy:208656-208676), see Cloaking below.
4. Any unit still unmarked and not cloaked is marked when its fog cell
   is in the viewer's sight, or explored with Line of Sight off
   (legacy:208677-208706).

The radius walk (legacy:220374-220459) uses a spatial hash of its own,
with 10 byte cells and a list head per bucket. A bucket is the 16.16
position shifted right by 23, so buckets are 128 world pixels, 8 map
cells, square. The circle is flat, on x and z only, with no height and
no line of sight (legacy:220374-220455). The walk also follows each
unit's chain of carried units, so units inside a transport are
visited.

The minimap and the full screen radar draw a blip for every unit that
is alive, marked and not carried by a transport
(legacy:208442-208527), in its owner's colour (legacy:208470-208526),
its look sorted by flyer, floater and builder (legacy:208481). With
the Mapping and Line of Sight options both off the minimap draws every
unit (legacy:208462-208466). The mark is a single flag with no blip
state of its own. The main view tests the fog maps alone and never the
mark (legacy:206797), and targeting, the computer players and the rest
of the simulation never read it.

So radar shows other players' units as blips beyond sight and nothing
more. Only the viewer's own radar counts, and an ally's is not shared.
A nanoframe has none, and switching a unit on or off makes no
difference. The minimap's pick reads the units it drew, so a radar
blip can be pointed at (legacy:238386-238401): the pick takes the
nearest blip under four squared radar pixels from the pointer. Range
circles for debugging are drawn at legacy:180559-180609.

The original has no jamming. `radardistancejam` and `sightdistancejam`
are tags of the engine's own for mods, see D-045.

## Cloaking

There is no `cancloak` key. A unit can cloak when its `cloakcost` is
above 0 (legacy:9760, legacy:9773, legacy:208663). The keys
(legacy:162947-162963):

| Key | Meaning |
|---|---|
| `init_cloaked` | The unit starts cloaked |
| `cloakcost` | Mana a frame while cloaked and standing, the FBI value over 30 |
| `cloakcostmoving` | Mana a frame while moving, `cloakcost` when missing |
| `mincloakdistance` | Enemy distance that suppresses the cloak, set to 80 when `cloakcost` is above 0 and the key is 0 (legacy:163619) |

Each frame (legacy:8700-8736), when the unit wants to cloak, is not
suppressed and the frame has passed its cloak lockout, it picks the
moving cost if it moves and the standing cost otherwise. When its own
mana pool covers the cost it spends it and turns the cloak on through
the script's cloak state. When it does not, the lockout is set to the
current frame plus 90. A unit's own pool recharges at its type's
`ManaRechargeRate` once the unit is built (legacy:8708). The cloak
order (legacy:9757, legacy:9770) sets or clears the want and calls the
script's `StartCloaking` or `StopCloaking`.

These break a cloak for a while, counted in frames at 30 a second.

| Cause | Lockout | Anchor |
|---|---|---|
| Firing a weapon | 600, 20 s | legacy:245900 |
| Building or mobile building | 300 | legacy:12259, legacy:12539 |
| Repairing | 300 | legacy:12764 |
| A yard build | 300 | legacy:13135 |
| Reclaim or capture | 300 | legacy:32139, legacy:32374 |
| An enemy inside `mincloakdistance` | 90, and the suppressed flag | legacy:208660-208673 |
| Out of mana | 90 | legacy:8726 |

A cloaked unit draws translucent, in render mode 5 or 6
(legacy:197442-197457), subject to a ghost flag on its type and a
global see-all. The local player's own cloaked units also draw a white
circle at `mincloakdistance` (legacy:180534). Radar and the visibility
pass never mark a cloaked unit, and an attack on a cloaked unit ends
(legacy:11010-11013, legacy:11376-11379). The engine draws a cloaked
unit at half alpha, for its owner and a watcher only, in place of the
translucent mode.

## A side takes on only what it sees

A side's units choose targets from a list of enemies, rebuilt every 7
frames, and an enemy goes into that list only when the side's
visibility test passes (legacy:20536-20540). The same test gates what
the player is shown, from drawing and picking to sound
(legacy:206797, legacy:237320, legacy:223583, legacy:152048), and the
minimap marks units by the same rule. It passes when any
corner of the unit's box stands on a cell the side sees now, with Line
of Sight on (legacy:206887-206892), or on a cell the side has
explored, with it off (legacy:206877-206884). So what a player sees
and what their units can take are one set. The corners matter for a
building: a 7 by 7 keep's corners stand 56 pixels from its centre,
nearly two fog cells.

Ranged units routinely outreach their own sight. A Mage Archer sees
250 and shoots 550 (`data/units/arabow.fbi`), and a trebuchet sees 200
and shoots 2700.

Return fire is the one exception. A unit that is hit answers the
shooter with no sight test at all, provided its selected weapon can
take the shooter and reaches it (legacy:15123). A unit that has a
mover also answers inside nine tenths of its `maneuverleashlength`
plus that reach, which a melee weapon does not add
(legacy:15124-15137). The test at legacy:15125 is the unit's mover, so
a building never takes that branch. The distance there is the longer
side plus a quarter of the shorter (legacy:254574), which reads a
diagonal short by about a tenth. Answering shows the side nothing, so
a unit hit from the dark answers a shooter its player cannot see, and
the shooter stays undrawn.

The computer's monarch drops its build when it is hit, so that its
return fire can answer the shooter (legacy:15087-15100).

A target a unit already holds is not dropped when it steps out of
sight. The original keeps shooting what it holds (legacy:11163-11178).
