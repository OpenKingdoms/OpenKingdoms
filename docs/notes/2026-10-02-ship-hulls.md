# Ships keep apart by their hulls (2026-10-02)

The owner asked for ships to stop sailing into each other. In the
original a ship holds the cells of its move class and nothing else
(legacy:163193-163195), and the move step refuses only a cell another
unit holds (legacy:219329-219340). The classes are squares, 32 px for
WATER2 up to 80 px for WATER5, under hulls that are mostly long and
thin. Two galleys could sit bow into stern 64 px apart with 86 px of
each hull inside the other.

This is a deliberate departure from the original, recorded as M-012 in
docs/MANUAL_DEVIATIONS.md. It changes outcomes, so it is engine build 20.

## The measurement

Every unit whose move class needs water under it, from the shipped
units and models with Iron Plague's archives over the base game. The
hull is the drawn extent of the 3DO, the selection mesh left out, in
whole px rounded out. Length runs bow to stern and beam is twice the
widest reach to either side, oars and sails included, since those are
what show when two ships touch. The cell is the side of the move
class's square.

| Unit | Name | Class | Cell | Length | Beam | Length over cell |
|---|---|---|---|---|---|---|
| ARATRANS | Ark | WATER5 | 80 | 171 | 108 | 2.1 |
| ARAWAR | War Galley | WATER4 | 64 | 150 | 50 | 2.3 |
| CREIRON | Iron Clad | WATER3 | 48 | 146 | 30 | 3.0 |
| CRESTER | Stern Wheeler | WATER3 | 48 | 129 | 32 | 2.7 |
| CRESUBM | Submersible | WATER2 | 32 | 99 | 54 | 3.1 |
| MONELEM | Sea Serpent | WATER4 | 64 | 95 | 78 | 1.5 |
| MONPIRAN | Piranha | WATER2 | 32 | 53 | 20 | 1.7 |
| NPCBOTL | Sea Shadow | WATER3 | 48 | 127 | 24 | 2.6 |
| NPCRIXX | Solan Rixx Flagship | WATER4 | 64 | 150 | 62 | 2.3 |
| TARCSHIP | Cult of Lokken Ship | WATER3 | 48 | 134 | 102 | 2.8 |
| VERFLAG | Flagship | WATER4 | 64 | 150 | 62 | 2.3 |
| VERHARP | Harpoon Ship | WATER4 | 64 | 137 | 108 | 2.1 |
| VERMAN | Man of War | WATER4 | 64 | 176 | 68 | 2.8 |
| VERSCOUT | Skiff | WATER3 | 48 | 113 | 82 | 2.4 |
| VERTRANS | Transport Ship | WATER4 | 64 | 109 | 108 | 1.7 |
| VERTRE | Trebuchet Ship | WATER5 | 80 | 145 | 158 | 1.8 |
| ZONBAR | Giant Barracuda | WATER3 | 48 | 133 | 82 | 2.8 |
| ZONKRAK | Kraken | WATER4 | 64 | 135 | 78 | 2.1 |

Every hull is longer than its cell, by 21 px for the Piranha up to 112
px for the Man of War, one and a half to three times the cell. Beams
run from half the cell (Sea Shadow) to twice it (Cult of Lokken Ship,
Trebuchet Ship), where oars or a yard reach out amidships. The hover
units (the gods, the Lich, the Mer Warrior, Kirenna, the Priest of
Lihr, Rictus, the Swamp Beast) and the ghost ship, which flies, are not
in the table.

## The rule

A ship's hull is a capsule: a line along its heading with half the beam
round it, reaching the bow and the stern. One radius would not do. A
circle round a galley's length keeps two galleys 150 px apart abeam
when 50 px would, and a circle round its beam lets them overlap bow to
stern. The capsule follows the length and the beam at any heading and
costs one closest distance between two segments per pair.

The numbers come from the def's 3DO the first time they are asked for,
in whole px, the same file on every machine and in the browser, the
way the model's height for shots already is. A unit is a ship when its
move class wants a minimum water depth. Ground units, hover units and
flyers have no hull and move exactly as before.

What a ship does with it:

- A step that would bring its hull within 2 px of another ship's is
  refused, the way a step onto another unit's cell is, unless the step
  takes it no closer than it already stands. Ships that are pressed
  together can always part.
- A turn that would swing its hull more than 3 px into another's is
  held.
- A ship stopped by another within two of its own lengths of its order
  point settles there and the fleet packs. A ship stopped by one of its
  own that already ended a move to the same point settles too, so a
  fleet sailing in a line packs behind its head instead of grinding.
  A ship still over its yard or still overlapping another goes on.
- Two ships closing on each other give way by their hulls measured
  across the line they would pass on, rather than by their cells.
- A shipyard begins a ship only once the last one's hull is off the
  pad, and a ship it launches sails until its hull is clear of the yard
  and on to water no other hull holds.

The route planner is not changed. It still plans in the class's cells,
so narrow water a fleet could cross before is still open to it.

## Before and after

Data free tests in test_movement, with the measured hulls of the War
Galley, Iron Clad, Trebuchet Ship, Skiff and Man of War on synthetic
defs:

| Case | Before | After |
|---|---|---|
| Ten ships sent onto one point, worst overlap at rest | 75.0 px | 0.0 px |
| The same, worst overlap while sailing | 90.6 px | 2.7 px |
| A yard builds three ships, worst overlap | 59.0 px | 0.0 px |
| Eight ships through a strait 192 px wide, worst overlap | 72.6 px | 3.0 px |
| The same, ticks until the last is through | 1323 | 1572 |

A walker and horsemen crowd lands on the same pixels as before (the
test pins the layout), and the needs data test sends a fleet with its
real scripts onto one point of open sea on Athri Cay.

## The jam at a corner (#458)

A Sea Fort on tight water stopped for good after a few ships. Measured
on the shipped maps with the archives: the six tightest free sites of
each map with water, a fort alone, twelve ships off a mixed queue, no
rally point, mana to spare, up to 36000 ticks. Riparian Conflict has one
free site and Iapur Narrows none, which makes 37 sites on seven maps.

| | Before | After |
|---|---|---|
| Sites that launched all twelve | 23 | 32 |
| Longest wait on the pad at the median site, ticks | 1479 | 580 |
| Longest wait at the worst site that launched them all, ticks | 1885 | 2697 |
| All the waiting on pads, ticks | 478622 | 268132 |
| Sites with a pair more than 3.5 px on each other at some time | 0 | 25 |
| Pairs more than 3.5 px on each other when a run ended | 0 | 0 |

The same with 24 Men of War, 12 Trebuchet Ships or 24 of the mix at five
of the sites finished 3 of 15 runs before and finishes 14 of 15 now.

Traced, the sites that stopped had three causes:

- Two ships under way pressed bow to bow, each with its order across
  the other. A step that comes closer is refused, a turn is held and a
  ship cannot reverse, so they stood until the order gave up, and the
  yard then sent them off again from the same pose.
- A ship sent off the pad was stopped within two lengths of its point
  by an idle ship, settled at once, and was sent off again 28 ticks
  later, for ever.
- A launch went for a point in water its class could not stand on or
  reach, or with shallows on the straight way to it. A ship with no
  route runs straight at its point, so it grounded in the way of the
  next ship.

The overlap is the price, and it stays in the jams: ships that have
stalled pass by the cells alone, and part again. The worst of it is
88 px.

## What still stops

Five of the 37 sites still stop, and #458 stays open for them. Before
counts the ships launched without these rules.

| Site | Before | After |
|---|---|---|
| Ladron's Tarn (576,4032) | 12 | 1 |
| Sea Dragon Spine (2816,2304) | 9 | 3 |
| Riparian Conflict (2816,2240) | 4 | 1 |
| Meredoc Keys (2880,6336) | 2 | 2 |
| Lake Lokken (1280,2816) | 1 | 1 |

The first three launch fewer than they did. A yard's script now opens
it before it trains, so a ship on the pad no longer stands on a built
cell. A route from there can die in a few cells where the class has no
way out of the pocket, and the test for water ahead now counts the
fort's closed cells as in the way. With no spot left the ship goes at
most 48 px past the pad and stops, its cells on the pad's. Without these
rules it was sent to a spot anyway and sailed straight at it.

Two changes tried alone did not cure this, each ending at 32 or fewer of
the 37. Judging a ship on its yard by where it stands and not by the
cell moved Sea Dragon Spine to 11 and Ladron's Tarn to 3. Taking any spot
as a last resort moved them the same way and stopped Meredoc Keys
(192,5056) at 10.
