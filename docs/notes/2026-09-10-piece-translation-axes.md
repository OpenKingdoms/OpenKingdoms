# Script piece translations sit in a flipped model frame (2026-09-10)

Symptom: the Aramon gate's doors slide toward each other when it opens
(issue #33). The script moves Door1 to x +50 and Door2 to x -50
(arangate.cob, `open`), and in the 3DO Door1's vertices lie on the
negative x side of the gateway, so taken literally the two halves cross
in the middle. The original slides them outward into the walls.

## What the original does

1. At load it negates x and z of every vertex and every piece offset in
   the 3DO tree (legacy:270790-270815). That is half a turn about y, not
   a mirror.
2. A script MOVE is stored raw on the piece (legacy:223426-223438) and
   added raw to that flipped offset when the piece matrix is built
   (legacy:198927-198930, legacy:364077-364100).
3. The piece matrix is parent x T(offset + pos) x R, with R applied as
   z, then x, then y, and the y angle negated (legacy:364101-364160).
   The unit's own angles enter the same way at the root
   (legacy:198681-198683).
4. Only the projection negates z (legacy:197681).

## What that means for us

Our mesh keeps the authored 3DO frame, so in that frame a script
translation (px, py, pz) has to land at (-px, py, -pz). The x mirror the
draw path already applies matches the original's zero heading view of
the model, so vertices and offsets need no change. The 2026-09-04 note's
claim that MOVE signs were fine held only for pieces that move on y.

Rotation order: conjugating the original's order into the authored frame
gives Ry(-y) Rx(-x) Rz(-z). Ours composed Rz Ry Rx. Single axis turns are
the same either way, which is why the 2026-09-04 oracle table could not
see the difference. A piece turned on two axes now composes as the
original does.

## Verified

On ARANGATE, with the fix each door origin moves 50 px to its own side
of the gateway and the pair end 100 px apart, then return to the centre
on close. A quarter turn on z and y of one door lifts its vertex mean by
the 23 px it sits from the origin, which the old order left level.
Test: own_unit_walks_through_its_gate_and_gate_opens in
src/ui/test_ui_screens.c.
