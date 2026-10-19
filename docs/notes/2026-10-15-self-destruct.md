# Self destruct, and the line for a fallen player (2026-10-15)

A player on the site found that Ctrl+D did nothing to a lodestone, and
that nothing in the message list says when a player is beaten. The
engine had no self destruct at all and no such line. This note records
what the original does for both and what the engine now does.

## Ctrl+D

Keys.TDF binds both CTRL_D and CTRLSHIFT_D to SelfDestruct. The
original looks at every selected unit of the local player. When any of
them already counts down, those stop and nothing else happens.
Otherwise every one of them starts, and the message list says "Leaving
Your Command", the unitmissions table's text for SELF DESTRUCT ENGAGED
(legacy:122248-122294). Nothing on the way asks what kind of unit it
is, so a lodestone, a castle or a wall goes the same way as a soldier.

The count is the unit file's `selfdestructcountdown` in seconds, 2 when
the file has none, and then 0 to 14 frames more at random
(legacy:163089-163095, legacy:9056-9088). No file in the base game,
Iron Plague or the patches has the line, so every unit counts two
seconds and a little. With a countdown of 0 the unit goes at once.

The order sits on a list of its own beside the unit's other orders, and
giving it clears none of them, so a unit goes on with what it was doing
while it counts (legacy:180727-180741, legacy:181669-181683). Only
Ctrl+D again, or the unit's death, ends the count.

The end is a hit of 30000 the unit deals itself with death type 5
(legacy:9086). That type runs no Killed script, so the unit leaves no
body and throws no pieces, and its Dying script runs with 5
(legacy:227124-227127, legacy:227250-227256). A lodestone's Dying fades
it out white. The owner counts the loss and nobody the kill
(legacy:227331-227338). A unit file can name a `[SELFDESTRUCTAS]`
weapon to burst then, and none in the shipped data does.

In the engine the command is TAK_CMD_SELF_DESTRUCT with the selection,
and every machine decides start or stop on the tick it runs. The count
is `Unit.selfd_ticks`, in the state hash and in the unit record from
version 14. The few random frames come from the simulation's generator
when the count starts, where the original draws them when the seconds
run out. Engine build 43.

A start asks first in the engine, in the shipped yes or no box, which
the original never does. Calling a countdown off asks nothing. That is
our own addition, recorded as D-041 in docs/MANUAL_DEVIATIONS.md.

## A fallen player

When the last unit of a player is gone, in a skirmish or a match, the
original adds one line to the message list, picked at random from
english/translate/death.tdf: DEATH1 to DEATH4, "%s has fallen.", "%s
has been vanquished.", "%s is no more." and "%s has been defeated.",
with the player's name in place of the %s. It is a type 4 entry owned by
the fallen player (legacy:206939-207005, legacy:227378-227387). A
campaign mission adds none.

The engine adds the same line when a player's units on the map, the
dying ones counted, drop to none. The original picks the line with the
C library's rand, which no other machine shares. The engine picks it
from the battle tick and the seat, so every machine shows the same
line. The original also plays MessageArrived for a line about another
player. The shipped archives carry no MessageArrived.wav, so with the
original's files that cue plays nothing. The engine plays a cue of its
own instead (D-043), every time a player falls, the computer included,
on every machine. A battle being caught up to the present skips it.
