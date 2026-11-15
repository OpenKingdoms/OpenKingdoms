# A flyer's landing loop runs until it is down (2026-11-14)

Seen in play beside the original: a flyer coming in to land did not
look as it does in the original on the way down.

## What the original shows

An Iron Beak with its status reading "Seeking to land" has its tail
down and its wings up and fluttering a little, and it drifts down. That
look holds for the whole descent. The landed pose comes at touchdown
and not before.

## What the script does

The Iron Beak's script keeps two things apart.

- `setSFXoccupy` tells it whether it is in the air: 5 is in the air.
- `BeginLanding` clears the script's own flight flag and starts
  `land`. `land` holds the descent pose, wings up and tail down, for
  as long as the script is told 5. Once it is told anything else, the
  script takes its landed pose.

So `land` is the descent, and it is the engine that ends it, by saying
through `setSFXoccupy` that the unit is down.

## What the engine did

`flyer_land` cleared the unit's `flying` flag and ran `BeginLanding` at
the top of the descent, which is right. But `unit_sfx_occupy_state`
answered from that flag, so `setSFXoccupy` told the script it was on
the ground a full cruise height up. `land` ended almost at once and the
landed pose took over in the air.

## What the engine does now

The occupy state is 5 for as long as the flyer has height. `land` holds
the descent pose all the way down and the landed pose follows at
touchdown. That rests on the observation above. The occupy anchor
(legacy:185079-185112) is where the state is sent, and says nothing of
when a landing flyer stops being 5.

`BeginLanding` runs where it always did, at the top of the descent
(legacy:24302), and a transport hears `EndTransport` before it
(legacy:24298-24302). The `flying` flag itself changes when it did, so
the cells a landing flyer holds and what may shoot at it are as before.

Most flyers' landing loops draw from the simulation's random generator
about once a tick, so the rolls after a landing differ from before. A
count over all 27 flyers with scripts, each landing once, gave 30 draws
in the descent before and 1343 now, over the same 2188 ticks.

Checked beside the original on 2026-11-15 with this in: the Iron Beak
comes down tail down and wings up and takes its landed pose at
touchdown.

## What went wrong on the way

A first fix moved `BeginLanding` to touchdown, on the reading that
`land` was the touchdown's animation. The flight loop then played down
to the ground and the landing started there, which was that reading
drawn, and not the original. The script settled it.

## Tests

`a_landing_flyer_is_in_the_air_until_it_is_down` in `test_movement`
lands a synthetic flyer and checks that `BeginLanding` comes once at
the top of the descent, that every tick of the descent says 5, and that
the flyer down on dry ground says 1.

`flyer_takes_off_flaps_and_lands` in `test_ui_screens` carried the same
misreading in a comment, that `land` is a loop of the unit shifting on
the spot. Its measure, that the wing moves far less on the ground than
in the air, holds as before.
