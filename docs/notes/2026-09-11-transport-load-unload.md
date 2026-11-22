# Transports

How the original carries units: who may ride, loading one unit or a
boxful, unloading, what a rider still is while aboard, and what the
sidebar shows about a loaded transport. Frames are the original's, 30
to a second.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Who may ride

The FBI keys are `transportsize`, `transportcapacity`,
`transportsizecapacity`, `transportedsize`, `cantbetransported`,
`cantransport` and `transportdistance` (legacy:162870-162879).
`cantbetransported` and `cantransport` are definition flags, and
`transportedsize` defaults to `footprintx` times `footprintz`
(legacy:163196).

Every transport ship has `transportdistance = 300`: ARAWAR, VERTRANS,
VERMAN, VERHARP, VERSCOUT, NPCBOTL and NPCRIXX. The Roc, ZONROC, has
154. ARADRAG and ZONDRAG carry a `canload` key without being
transports, and the original reads no `canload` key at all.

A unit may board only when all of these hold (legacy:233612-233649).

1. Both units are alive, built and not paralyzed.
2. Neither is being loaded or unloaded.
3. Both have the same owner, so nothing boards an ally's transport.
4. The rider cannot fly and is not `cantbetransported`.
5. The transport is `cantransport`.
6. The rider has a mission.
7. The rider's `transportedsize` is no more than the transport's
   `transportsize`, a size gate for each rider.
8. The transport carries fewer than `transportcapacity` units, counted
   by walking its chain of riders (legacy:233578).
9. The `transportedsize` of everything aboard plus the new rider is no
   more than `transportsizecapacity` (legacy:233595).
10. A submersion check passes (legacy:233637).

The links run through the units themselves. The transport holds the
head of its chain of riders, each rider holds the next, and each holds
a pointer back to its transport. A load pushes onto the head
(legacy:234563-234564).

## Loading

A pickup gives the transport a ground pickup order, or a flying pickup
for a flyer, and gives the rider an order to seek the transport
(legacy:181697-181785, legacy:186686-186692). The pickup mission
(legacy:14365-14483) matches the two orders, gates on the squared
distance against `transportdistance` and gathers other eligible units
in range.

- The rider walks to within `transportdistance` less 16 of the
  transport, looks again every 30 frames, and stops when the transport
  tells it to (legacy:10505-10580). It gives up when the transport has
  no pickup left at all (legacy:10578) or can no longer take it
  (legacy:10519).
- The transport checks that the rider still seeks it
  (legacy:14386-14390) and still fits (legacy:14391-14394), closes to
  `transportdistance` of it, and marks it to stop
  (legacy:14404-14411).
- It waits for the rider to stand still and gives up after 10 frames
  of movement (legacy:14449-14453). Then, on the first frame, it plays
  the `load` sound and the mindspin at the rider and the swirl on
  itself (legacy:14461-14463, and legacy:27571-27582 for a flyer). The
  rider stays in view for 15 frames (legacy:14457) and is then
  attached (legacy:14470-14473, legacy:234562-234580) and no longer
  drawn (legacy:198693-198706).

The original's unit chatter has a cannot transport unit line. A flying
transport has air pickup and unload missions of its own
(legacy:27164). The engine departs there, see T-001.

## Loading with a drag

- The Load cursor works only with exactly one transport selected
  (legacy:238106-238132, legacy:186096-186128). With none, or with two
  or more, a drag is an ordinary box select (legacy:243594-243615).
  Over a unit the transport cannot take, the cursor turns back into the
  normal pointer (legacy:186387-186402).
- On release every live unit of the player whose drawn position lies
  in the box is considered, in unit order (legacy:237695-237729,
  legacy:238144-238166). Each one the transport can carry with its
  current cargo gets a pickup (legacy:238682, legacy:233612-233649). No
  room is held for pickups already queued, so a box with more units
  than the hold takes queues every unit that fits on its own.
- The first pickup replaces the transport's orders unless Shift or
  Ctrl is held, and the rest append (legacy:238520,
  legacy:238685-238687, legacy:181670-181671). A unit already on the
  list is skipped (legacy:181726-181735). Every rider's own orders give
  way to seeking the transport (legacy:181770-181782), so they all
  start walking at once.
- The drag plays no voice. Load ends after it unless Shift is held,
  and with Shift it ends when Shift is let go (legacy:242531-242541,
  legacy:243768-243771). A click with Shift keeps it the same way
  (legacy:243644-243646).
- Once the hold is full the riders left fail the capacity check and
  stop where they are (legacy:10519). The transport drops their
  pickups when it reaches them (legacy:14391-14394, legacy:14482).

## Unloading

The unload order sends a transport that cannot fly to the ground
unload mission and a flyer to the air one (legacy:187055-187062). The
ground mission (legacy:14487-14649) sets the cargo down one unit at a
time.

1. The next unit off is the head of the transport's chain
   (legacy:14518), so the last unit aboard is the first off. With the
   chain empty the order is done (legacy:14520).
2. The distance from the transport's centre to the order point is
   compared with the transport's `transportdistance`
   (legacy:14521-14529). Out of reach the transport moves toward the
   point with an arrival radius of `transportdistance` less 34 and
   looks again 15 frames later (legacy:14532, legacy:179465-179480). If
   it cannot move, or the move failed, the order ends and nothing is
   dropped. In reach it goes on from where it is. It never sails onto
   the point or lines up on it.
3. The spot is tested with the cargo's own definition at the point
   (legacy:14551). The test looks at each cell of the footprint for the
   map edge, the cargo's slope limit, its water depth window, blocking
   features and any unit standing there (legacy:218679-218912).
4. With the spot clear the transport plays the `unload` sound and the
   mindspin at the point and the swirl on itself, on the first frame
   only (legacy:14559-14561). It tests the spot again every frame and
   drops the cargo 15 frames later (legacy:14554).
5. With the spot not clear it tests again and lets units that can move
   through (legacy:14569, legacy:218806-218811). If that fails as well,
   because of the ground, deep water, a structure or a feature, it
   reports the target blocked and the order ends with the cargo aboard
   (legacy:14573). If only movers are in the way it waits 10 frames and
   goes back to step 2. After six tries it rests 15 to 44 frames and
   starts again (legacy:14579-14586, legacy:182190-182195). A unit
   parked on the point holds the unload up for as long as it stays
   there, and the transport counts as one of those units, so a point
   under the ship itself never clears and the order waits there for
   good.
6. The drop puts the cargo on exactly the order point and detaches it
   (legacy:14598-14600, legacy:234362-234395). The unit set down is
   sent a short way off so the next can land there
   (legacy:14602-14629). The radius is 16 px times one plus two rolls
   of 0 to 2, so 16 to 80 px. While cargo remains it grows by the
   number still aboard, at most six, times half of a size term that is
   not established. The move goes to a spot between that radius plus
   16 px per footprint cell and three times as far from where the unit
   stands (legacy:13858-13868).
7. Two frames later the next unit starts over at step 1.

The effects fire 15 frames before each drop, and with the spot clear
the drops come 17 frames apart, a little over half a second. With its
`transportdistance` of 300 a ship drops from up to 300 px away, and
closes to 266 px when it has to move. The original has no unload
scatter: the random spots at legacy:24329-24359 belong to the flyer
landing search in [pathing.md](pathing.md).

The engine departs at step 2, see T-002, and at step 3, see T-003. It
also draws the two rolls of step 6 from the unit ids and stands the
footprint width in for the size term.

## Effects and scripts

The effects are `anims/mindspin` (14 frames) and the `TransSwirl`
sequence of `anims/transportfx.gaf` (11 frames), both set up to play
once (legacy:161478-161485). Both files give every picture a duration
of 2 frames, and an effect holds each picture for its duration before
it moves on (legacy:255140-255150, legacy:255772-255794). So the swirl
lasts 22 frames and the mindspin 28. `transportfx.gaf` is a paletted
file and takes the `fx.pcx` palette.

No unit script runs on a pickup or a drop. The only transport script
call is `EndTransport` (legacy:24300), made just before `BeginLanding`
when a flying transport lands (legacy:24298-24302). The landing
missions around it, with their VTOL_StepOut step outs (legacy:24242
onwards), are in [pathing.md](pathing.md). No shipped script defines
`EndTransport`, and none uses the attach or drop opcodes. A Kingdoms
script has no `TransportPickup` or `TransportDrop` pieces, which belong
to Total Annihilation. On the script side the original also names
BECARRIED (legacy:179284).

## What a rider still is

The attach only records the transport on the rider and leaves the
selection (legacy:234562-234589). The rider stays a live unit of its
seat, so the update loop and everything that reads the live flag go
on with it.

- It pays its `mogriumincome` and `mogriumstorage` into the pool
  (legacy:235853-235946).
- Its own reserve fills, its wounds mend (legacy:236276-236288,
  legacy:8706-8709) and its auras work from where the transport is
  (legacy:235853-235876).
- It keeps its sight (legacy:206485-206491).
- It keeps its place in a numbered group, and selecting a group skips
  it (legacy:239065-239070).
- A map script does not hear it die. It hits triggers from aboard
  (legacy:178218-178250) and answers the value ports
  (legacy:178769-178810).
- A mission's conditions count it as alive, all but two.
  `AllUnitsKilled` and `MoveUnitToRadius` ask for a unit that stands on
  the map (legacy:240925-240934, legacy:240665-240686).

A transport's end is its riders' end. The kill walks its chain and
kills each one with no script and no body, its owner counting the loss
and the transport's killer the kill (legacy:227218-227234,
legacy:227293-227327). A monarch aboard goes the same way, and with
Monarch Expendable off the transport counts as the monarch
(legacy:227100-227108). A gift leaves out a transport with riders
(legacy:155789).

An order list's `i` is the attach itself, with no walk and no room
check, whoever owns the transport (legacy:228557-228561,
legacy:234446-234449). A unit name there means the first placed unit
that carries it (legacy:228197-228238). Order lists are in
[2026-09-15-mission-conditions.md](2026-09-15-mission-conditions.md).

## What the sidebar shows

The sidebar refresh fills the `HelpText` label of
`data/guis/<side>ingame.gui` from the unit it describes, the single
selected unit or the unit under the cursor (legacy:152045-152069). When
that unit's definition can transport (legacy:152081) it walks the
chain of riders and counts them (legacy:152083-152086). With at least
one aboard the label gets the translation of
`TRANSPORT_CARRYING_HELPTEXT` followed by the count through the format
`%s %d`, so "Carrying 3" in English (legacy:152087-152089). The comment
on that key in `english/translate/messages.tdf` says the same: the
text goes in the helptext control when a transport unit is selected.

That is all the riders get. There are no pictures in the menu grid, no
names, and the second info panel keeps showing the selection's target.
The only listing of which units are aboard is a debug probe of unit
state, which prints "contains" and one name per rider
(legacy:211562-211572). A player never sees it.

When the label would otherwise be empty it carries the mana readout:
the translation of `CRYSTALBALL_MOGRIUM_MESSAGE` ("Mana"), a line
break, then the current value over the maximum, the current clamped to
the maximum (legacy:152100-152110, format `%s\n%d/%d`). The current
value is the crystal ball's smoothed one, an average over the last 29
frames (legacy:8795). The label sits above the crystal ball at 526,
397, 104 by 34 pixels, so the two lines fit. The engine departs here
and shows the pool without the smoothing.

While the pointer rests on a build picture the label shows the help
string the same routine sets on that button: the unit's name, a line
break, the translation of `UNIT_COST_HELPTEXT` ("cost") and the build
cost, format `%s\n%s %d` (legacy:149796-149810, set at
legacy:149805-149809).
