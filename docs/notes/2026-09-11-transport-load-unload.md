# Transport load and unload (2026-09-11)

The owner's report that started this: "for ships or units that can carry
other units, you can unload them but it's clunky the ship positions itself
juuust right first - this should happen like legacy. Also there is
animation that happens on load/unload - it should behave just like
legacy. When loading units you should be able to load multiple units at
one time by clicking and dragging after selecting load."

Frames below are the original's, 30 a second. Our simulation runs at 60,
so one frame is two of our ticks.

## Unloading

The unload order sends a transport that cannot fly to the ground unload
mission and a flyer to the air one (legacy:187055-187062). The ground
mission (legacy:14487-14649) sets the cargo down one unit at a time.

1. The next unit off is the head of the transport's passenger chain
   (legacy:14518). A load pushes onto that head (legacy:234563-234564),
   so the last unit aboard is the first off. With the chain empty the
   order is done (legacy:14520).
2. The distance from the transport's centre to the order point is
   compared with the transport's `transportdistance`
   (legacy:14521-14529). Out of reach the transport moves toward the
   point with an arrival radius of `transportdistance` less 34 and looks
   again 15 frames later (legacy:14532, :179465-179480). If it cannot
   move, or the move failed, the order ends and nothing is dropped. In
   reach it goes on from where it is. It never sails onto the point or
   lines up on it.
3. The spot is tested with the cargo's own definition at the point
   (legacy:14551). The test looks at each cell of the footprint for the
   map edge, the cargo's slope limit, its water depth window, blocking
   features and any unit standing there (legacy:218679-218912).
4. With the spot clear the transport plays the `unload` sound and the
   mindspin effect at the point and the transport swirl on itself, on the
   first frame only (legacy:14559-14561). It tests the spot again every
   frame and drops the cargo 15 frames later (legacy:14554).
5. With the spot not clear it tests again and lets units that can move
   through (legacy:14569, :218806-218811). If that fails as well, because
   of the ground, deep water, a structure or a feature, it reports the
   target blocked and the order ends with the cargo aboard
   (legacy:14573). If only movers are in the way it waits 10 frames and
   goes back to step 2. After six tries it rests 15 to 44 frames and
   starts again (legacy:14579-14586, :182190-182195). A unit parked on the
   point holds the unload up for as long as it stays there, and the
   transport counts as one of those units, so a point under the ship
   itself never clears and the order waits there for good.
6. The drop puts the cargo on exactly the order point and detaches it
   (legacy:14598-14600, :234362-234395). The unit set down is sent a
   short way off so the next can land there (legacy:14602-14629). The
   radius is 16 px times one plus two rolls of 0 to 2, so 16 to 80 px.
   While cargo remains it grows by the number still aboard, at most six,
   times half of a size term the reference does not name. The move goes
   to a spot between that radius plus 16 px per footprint cell and three
   times as far from where the unit stands (legacy:13858-13868).
7. Two frames later the next unit starts over at step 1.

The effects fire 15 frames before each drop, and with the spot clear the
drops come 17 frames apart, a little over half a second. Every ship has
`transportdistance = 300` (ARAWAR, VERTRANS, VERMAN, VERHARP, VERSCOUT,
NPCBOTL, NPCRIXX), so ships drop from up to 300 px away and close to
266 px when they have to move. The Roc (ZONROC) has 154.

## Loading

A pickup gives the transport a ground pickup order, or a flying pickup for
a flyer, and gives the rider an order to seek the transport
(legacy:181697-181785, :186686-186692).

- The rider walks to within `transportdistance` less 16 of the transport,
  looks again every 30 frames, and stops when the transport tells it to
  (legacy:10505-10580). It gives up when the transport has no pickup left
  at all (legacy:10578) or can no longer take it (legacy:10519).
- The transport checks that the rider still seeks it
  (legacy:14386-14390) and still fits (legacy:14391-14394), closes to
  `transportdistance` of it, and marks it to stop (legacy:14404-14411).
- It waits for the rider to stand still and gives up after 10 frames of
  movement (legacy:14449-14453). Then, on the first frame, it plays the
  `load` sound and the mindspin at the rider and the swirl on itself
  (legacy:14461-14463, and :27571-27582 for a flyer). The rider stays in
  view for 15 frames (legacy:14457) and is then attached
  (legacy:14470-14473, :234562-234580) and no longer drawn
  (legacy:198693-198706).

No unit script runs on a pickup or a drop. The only transport script call
is `EndTransport`, made just before `BeginLanding` when a flying transport
lands (legacy:24298-24302). No shipped script defines it, and none uses
the attach or drop opcodes.

The effects are `anims/mindspin` (14 frames) and the `TransSwirl` sequence
of `anims/transportfx.gaf` (11 frames), both set up to play once
(legacy:161478-161485). Both files give every picture a duration of 2
frames, and an effect holds each picture for its duration before it moves
on (legacy:255140-255150, :255772-255794). So the swirl lasts 22 frames
and the mindspin 28. `transportfx.gaf` is a paletted file and takes the
`fx.pcx` palette.

## Loading with a drag

- The Load cursor works only with exactly one transport selected
  (legacy:238106-238132, :186096-186128). With none, or with two or more,
  a drag is an ordinary box select (legacy:243594-243615).
- On release every live unit of the player whose drawn position lies in
  the box is considered, in unit order (legacy:237695-237729,
  :238144-238166). Each one the transport can carry with its current cargo
  gets a pickup (legacy:238682, :233612-233649). No room is held for
  pickups already queued, so a box with more units than the hold takes
  queues every unit that fits on its own.
- The first pickup replaces the transport's orders unless Shift or Ctrl is
  held, and the rest append (legacy:238520, :238685-238687,
  :181670-181671). A unit already on the list is skipped
  (legacy:181726-181735). Every rider's own orders give way to seeking the
  transport (legacy:181770-181782), so they all start walking at once.
- The drag plays no voice. Load ends after it unless Shift is held, and
  with Shift it ends when Shift is let go (legacy:242531-242541,
  :243768-243771). A click with Shift keeps it the same way
  (legacy:243644-243646).
- Once the hold is full the riders left fail the capacity check and stop
  where they are (legacy:10519). The transport drops their pickups when
  it reaches them (legacy:14391-14394, :14482).

## What we do

`src/render/units.c` follows these missions at 60 Hz.

- `unit_unload_tick` runs the unload. Out of reach it closes to
  `transportdistance` less 34. It tests the spot with `unit_spot_clear`,
  which runs the placement test's ground checks on the cargo's move class
  footprint and then looks for other units. It plays the effects, holds
  30 ticks, sets the unit down with `unit_set_down` and starts on the next
  4 ticks later, so drops come 34 ticks apart. A blocked spot ends the
  order. Movers in the way mean a retry 20 ticks on, and after six tries
  a rest of 30 to 88 ticks.
- A transport keeps a list of pickups. `unit_pickup_tick` works on the
  head of it. The transport closes to `transportdistance`, stops the
  rider, waits up to 20 ticks for it to stand, plays the effects, holds
  30 ticks and attaches it. `unit_board_tick` walks the rider
  (`UNIT_CMD_BOARD`) to within `transportdistance` less 16.
- `Units_CommandLoadInRect` and `InGame_WorldDrag` handle the drag, and
  `Units_CommandLoadSelected` takes a queued flag for Shift.
- The step a unit takes after it is set down comes from deterministic
  noise on the unit ids. The unnamed size term in step 6 is taken as the
  footprint width, a stand-in until the spacing is checked in the game.
- `flight_tick` asks for `EndTransport` before `BeginLanding` when the
  unit is a transport.
- The effects are one-shot engine effects at 4 ticks a picture, and
  `transportfx` is decoded with `fx.pcx`.

Deviations T-001 to T-003 in `docs/MANUAL_DEVIATIONS.md` cover the
deliberate differences. Not done yet are the target blocked and cannot
transport unit chatter (we have no unit chatter), Ctrl as a second append
key, and the Load cursor turning back into the normal pointer over a unit
the transport cannot take (legacy:186387-186402). A unit with `canload`
that is not a transport (ARADRAG, ZONDRAG) keeps the walk-to order it had
before. The original reads no canload key at all.

## Tests

All in `src/ui/test_ui_screens.c`.

- `unload_in_range_does_not_move_the_ship`
- `unload_out_of_range_stops_at_transport_distance`
- `unload_into_deep_water_is_refused`
- `unload_sets_riders_down_one_at_a_time`
- `unload_is_last_in_first_out`
- `unload_onto_a_building_is_refused`
- `unload_plays_the_swirl_and_steps_the_unit_clear`
- `load_holds_half_a_second_before_the_cargo_vanishes`
- `load_plays_swirl_on_the_ship_and_mindspin_on_the_cargo`
- `transport_effects_play_out_once`
- `flying_transport_runs_EndTransport_before_BeginLanding`
- `drag_in_load_mode_boards_every_boxed_rider`
- `drag_load_leaves_riders_that_do_not_fit`
- `shift_drag_in_load_mode_appends_and_keeps_the_mode`
- `drag_in_load_mode_without_a_single_transport_box_selects`
- `click_load_with_shift_queues_behind_the_current_pickup`
