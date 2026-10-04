# A summons without end (2026-10-04)

Zhon makes its army with builders that walk. A Zhon player in the
original holds Ctrl on a builder's button for a unit, clicks one spot,
and the builder summons that unit there over and over until told to
stop. The owner found that Ctrl did nothing here, so building an army
meant placing every unit by hand. This note records what the original
does and what the engine now does.

## The button

A left click on a walking builder's button arms placement, as it does
for any building (legacy:150058-150087). With Ctrl held, and a product
whose `bmcode` is 1, the click also sets a repeat flag. Without Ctrl, or
for any other product, the flag is cleared (legacy:150077-150084). Shift
does nothing at the button.

The loader keeps `bmcode` at 1 only for a def with a `turnrate` and a
`brakerate`, and turns any other value to 0 (legacy:162855, 162865,
163608-163617). So the repeat is for units, the Zhon harpy and every
other flyer among them, and never for a building. An older note here
took `bmcode` 1 for walls, which it is not.

## The click

The order the click gives counts ten million with the repeat flag set,
and one without it (legacy:39237). Shift and Ctrl held at the click
normally queue the order or change the one in hand. With the repeat
flag set, Ctrl drops both, so a player who keeps Ctrl down through both
clicks replaces what the builder was doing (legacy:39177-39180). Shift
alone still queues it. A building queued with Shift is never given to a
builder that holds an order of ten million, since it would never be
reached (legacy:39208-39219). The placement ends after this one click
even with Shift held (legacy:242531-242540).

## The summons

The build order tests its site once the builder is in reach. The first
test counts every unit on the site, and a second one lets units that
can move through. When only the second passes, the builder looks again
10 frames later. It says it is waiting at the sixth look and gives up
after 30, about 10 seconds. When even the second fails, something that
will not move holds the site and the order ends at once
(legacy:12088-12124).

When the unit is finished, an order of any count but ten million ends
there. One of ten million instead sends the new unit off the spot and
starts again a frame later on the same spot (legacy:12272-12315). The
unit goes through the same step aside a transport's cargo takes once
it is set down (legacy:14601-14629). Its radius is 16 px times one plus
three rolls of 0 to 2, and four more for a builder that flies, so 16 to
112 px, or 80 to 176 px. The unit goes to a spot between that radius
plus 16 px for each cell of its footprint and three times as far
(legacy:13858-13868). The order never ends by itself. A right click on
the button takes every build order of that def (legacy:150087-150093,
181838-181866), and Stop or any new order without Shift ends it too.

The button reads `+++` while the builder holds such an order
(legacy:149922-149929).

## The engine

A build command carries an endless bit, `TAK_CMD_ARG_ENDLESS`, which the
executor honours only for a walking builder and a def
`Units_DefCanRepeat` names. The builder keeps the summons on the order
in hand, and a Shift click keeps it on the queued order, so both are in
the state hash and the save. When a unit is finished the builder steps
it aside, waits two ticks, the original's frame, and starts the next on
the same spot. A spot that units hold is looked at again every 20
ticks until 30 looks have gone by. Ground, a building or a feature on
it, or no room for one more unit, ends the order. The original's
chatter has no counterpart here yet, so the waiting and the giving up
are silent.

The classic sidebar reads Ctrl at the build button. The remaster's
front end arms the same placement through the embedding API, and its
own placing click sends the same command.

Each build command still goes to the first builder of the selection
that takes its site, where the original gives it to every builder
selected. That is M-013.
