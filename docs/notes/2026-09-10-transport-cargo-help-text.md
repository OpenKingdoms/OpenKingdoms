# What the sidebar says about a loaded transport (2026-09-10)

Symptom that started this: in a browser skirmish a war galley with
soldiers aboard looked no different from an empty one. In the original
you can see how many units a transport holds (issue #36).

## What the original shows

The sidebar refresh fills the `HelpText` label of
`data/guis/<side>ingame.gui` from the unit it describes, the single
selected unit or the unit under the cursor (legacy:152045-152069). When
that unit's definition can transport (legacy:152081) it walks the
carrier's chain of passengers and counts them (legacy:152083-152086).
With at least one aboard the label gets the translation of
`TRANSPORT_CARRYING_HELPTEXT` followed by the count through the format
`%s %d`, so "Carrying 3" in English (legacy:152087-152089). The comment
on that key in `english/translate/messages.tdf` says the same: the text
goes in the helptext control when a transport unit is selected.

That is all the passengers get. There are no pictures in the menu grid,
no names, and the second info panel keeps showing the selection's target.
The only listing of which units are aboard is the developer's unit state
probe, which prints "contains" and one name per passenger
(legacy:211562-211572). A player never sees it.

When the label would otherwise be empty it carries the mana readout: the
translation of `CRYSTALBALL_MOGRIUM_MESSAGE` ("Mana"), a line break, then
`current/maximum` with current clamped to the maximum
(legacy:152100-152110, format `%s\n%d/%d`). The current value the
original prints is the crystal ball's smoothed one, an average over the
last 29 frames (legacy:8795). The label sits above the crystal ball at
526, 397, 104 by 34 pixels, so the two lines fit.

The same routine sets the help string on a build button as the unit
name, the cost word and the price (legacy:149805-149809), which is what
the label shows while the cursor rests on a build picture.

## What we do

`src/ui/hud.c` fills `HelpText` every frame the same way. The count comes
from `Units_GetSelectedCargoCount`, which counts the units recorded as
carried by the selection. The readout uses the pool value the crystal
ball already reads, without the 29 frame smoothing. `Font_DrawString`
breaks lines on a newline now, so the readout takes its two lines. The
build picture hover text is not done yet.
