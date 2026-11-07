# A yard opens before it trains (2026-11-06)

A playtester saw a Skeleton Archer on the platform of an Abyss that could
not be hurt until the Abyss was destroyed (#479).

## What the original does

A factory's build order has steps (legacy:9316-9340). Its first sets the
unit's activation bit, whose rising edge runs the script's Activate
(legacy:236402). The next waits until the script sets INBUILDSTANCE
(legacy:179347-179358). Only then does it ask QueryBuildInfo and make the
product. When the count runs out it clears the bit, which runs Deactivate
(legacy:9323-9328, 236405).

All eight factories in the shipped data script this much the same way.
Activate goes through RequestState to Go, which plays the doors, calls OpenYard
and sets INBUILDSTANCE. Deactivate reaches Stop, which clears INBUILDSTANCE and
calls CloseYard, the Temple in the other order. OpenYard and CloseYard set
YARD_OPEN and try again every 1.5 seconds while the set is refused.

A yardmap `c` cell blocks only while the yard is shut
(legacy:217987-217988). A set of YARD_OPEN is refused while a live unit
holds a cell that would then block (legacy:218984-219059). So a pad is open
ground from before the product goes up until the last unit has left it, the
unit there holds its own cell, and a shot in that cell meets that unit
(legacy:245414-245424).

## What the engine did

It never ran Activate on a factory, so no factory ever opened its yard. The
`c` cells stayed the factory's, and a product made there could not stamp
them, since a cell another unit holds is left to it. The cell test a shot
makes then found the factory, on the cell where the target stood and on the
way to it, so everything aimed at a unit on the pad hit the building. An archer
shot at there for ten seconds, in the test case for this, stayed at 900 of 900
while the Abyss went from 23020 to 22300. When the building died its cells
cleared and the unit could be hit.

Nothing was wedged. The mover lets a unit standing on someone's cells step
out, so one with a move order always walked off. A unit stays on the pad when
it is a frame whose side has run out of mana, or when it has been told to stop
or fights from there, and that is what looked stuck.

## What it does now

A factory with a script activates when it has something to train and goes on
once its script stands ready, and it deactivates when the queue is empty. The
doors now play and a run's first product goes up after them. That is 85 ticks
for the Abyss from a shut yard. From one still shutting it is the rest of the
close as well, about six seconds at most for the Abyss. A unit on the pad holds
its cell and takes what is aimed at it, and the yard stays open under it until
it leaves, as the script keeps trying.

The computer player does not queue. Its order on a yard still opening is refused
and asked again at its next think, as it asks when a pad is held, so it can still
fall back on another unit. The yard stays open two seconds for that.

## Ours

Between two runs of different units with more on the queue the original clears
and sets the bit again, and the engine leaves it set, so the doors do not
cycle. The script ends in the same state.

A unit standing on the yard keeps it from shutting, and a factory told to train
meanwhile would wait for ever, since its script is still in Stop. BUGGER_OFF, which
the scripts raise meanwhile, is only ever read back by a script (legacy:223287),
so in the original the unit stays until someone moves it. Here the side's idle
units on those cells are sent to a spot round the factory, or a ship out of the
yard's way, once something is waiting. Before this a shipyard already sent idle
ships off its pad (M-012). Enemy units and fighting ones are left, and the yard
waits for them.
