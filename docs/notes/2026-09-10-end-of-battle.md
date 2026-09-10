# How a skirmish ends

Line numbers are anchors into the legacy reference, as in every note here.

## What the verdict reads

Each player record carries two counters that the verdict reads and
nothing else. Unit creation adds one to the live count for every unit
and one to the units-built count for every unit whose kind is not a
feature (walls are, `isfeature` :163000) at :226969-226973. Freeing a
unit record takes one off the live count at :227378. Nothing else
touches them, so a dying unit still counts until its death sequence
ends, and a lodestone counts like any other unit.

Once a second per player (:206625-206662) the game asks, in this order:

- Defeat, :240045-240051: the local player built something and the
  live count is zero.
- Victory, :240018-240033: the local player built something and every
  slot outside their alliance has a live count of zero.

Neither has a grace period. The 30 s wait some notes mention belongs to
the Boneyards branch (:240053-240063 and :240065-240072, reached from
:240094-240100 only when the game mode is 3).

## Monarch Expendable

The option is the CommanderDeath session value, stored as `MonarchDeath`
and defaulted to 1 (:159476). The checkbox shows frame 3 when the value
is set and frame 4 when clear (:139327), so a set value means the
monarch is not expendable.

With the value set, the death of a unit whose kind carries the
`commander` flag (:163074), or of a transport carrying one
(:227100-227108), removes the owner's whole army on the spot
(:227174-227181). The removal (:227541-227580) kills every unit of the
slot with death type 10, which skips the Killed script and frees the
record at once (:227241-227260). Campaign missions skip this and rely
on their own defeat conditions (:227177). The live count is then zero
and the per-second check does the rest.

With the value clear a player stays in the battle while any unit of
theirs remains. There is no exception for structures. What ends such a
game in the original is its AI, whose attack groups score every enemy
unit by distance and only discount an unseen one (:15365), so a last
lodestone is found and destroyed.

## Tallies on the player record

- Units built, :226971: creations of non-feature kinds.
- Kills, :227302, and score, :227305: the killer's player gains one
  kill and the victim kind's `experiencepoints` (:162918, default 666)
  when the victim was another player's unit. Death type 10 credits
  nothing.
- Losses, :227296: the owner of a destroyed unit. Death type 10 counts
  nothing.
- Time, :206617-206619: once a second every player who still has units
  gets the current tick stamped on their record, and the screen prints
  that stamp as hh:mm:ss. A beaten player's time stops where they fell.

## The screens

The verdict loads a one-label dialog over the in-game desktop:
VictoryText.gui or DefeatText.gui, the word in the 48 px face, centred
over the play area (the screen less the 128 px sidebar and the 48 px
bottom strip, :145740-145744). The battle keeps running under it. The
verdict also starts a 90 tick countdown (:206566 at 30 Hz, 3 s) and when
it runs out the game stops the simulation and opens the statistics
dialog (:206533, :244079-244086).

The statistics dialog is Defeat.gui, or Victory<prefix>.gui for the
local side's `nameprefix` (:153765-153776). It fills one row per slot
that built something (:153968): the badge from teamlogos.gaf
`<prefix>team` at frame colour plus 2, the player's name, units built,
kills, losses, time and score (:153975-154004). Proceed's help text is
`SKIRMISH_BATTLE_ROOM` from the translate table in a skirmish
(:154043). The button leads back to the battle room and Main Menu to
the main menu (:154080-154131). The buttons carry their own click
sounds in the dialog file (ok.wav and cancel.wav), the help strip shows
the hovered button's text, and the root widget's accelerator string
maps Enter to Proceed and Escape to Main Menu.

No movie plays for a skirmish. `Movies/Victory.bik` runs from Proceed
after the last campaign mission only (:154103).

## What OpenKingdoms does

The rule above is in `src/ui/ingame.c` and the elimination in
`src/render/units.c`. One deliberate difference: a dying monarch keeps
its death sequence instead of being freed at once, so the explosion
plays out. The player is flagged eliminated the moment the monarch
dies, which is what the count being zero means in the original.

The AI's raiders, once they stand at an enemy start position with
nothing in sight, take the nearest enemy unit anywhere as their target
(`src/game/ai.c`), the original's no-visibility scoring reduced to the
case that stalled games.
