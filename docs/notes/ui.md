# Interface and input

How the original's interface behaves: the main menu's clips and the cut
scenes, the loading screen, dialogs with scroll arrows, the pointer on
the battlefield, the in-game sidebar and its buttons, Shift and Ctrl on
orders, and the veteran rank on the HUD.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Menu clips and cut scenes

The original plays Bink clips in three places: the four doors of the
main menu, the full screen reels (logo, intro, credits) and the arch on
the loading screen. The clips it plays are the logo, the intro, the two
credits reels and the seventeen pieces under `Movies/Gui`, 39 MB of the
539 MB `Movies` folder. An install spells some names in capitals, such
as `LOGO.BIK` and `CREDITS.BIK`.

A clip is stepped by the decoder's own clock. The draw routine waits on
the decoder until the next frame is due, decodes that one frame, draws
it and moves the frame counter on by one (legacy:35289-35341). Nothing
skips a frame to catch up. The rate is in the clip's header: the door
clips run at 30 fps, the loading arch at 20 fps and the reels at 15
fps.

### The doors

Each door is a button with eight states (legacy:147778). State 2 is
rest. States 4 to 7 play clips 4 to 7, `Movies/Gui/<name>N.bik`: 4 is
the still after a click, 5 the enter clip, 6 the hover clip and 7 the
leave clip.

A resting door is drawn from clip 4's still, not from the sprite
sheet. Observed on the GOG v4.0BB release under DxWnd, 640x480: on the
three doors the resting menu differs from the engine drawing clip 4 at
the widget's corner by 16 to 18 per pixel (the summed RGB difference,
the same as over plain background, which is 16 bit dithering), and
from the engine drawing the sheet by 60 to 65. Clip 5's first frame
and clip 7's last fit as well, since all three are the same picture.
The sheet is a larger render of the same art, 2 to 3 px bigger on each
side. The snort is the exception. Its still is redder than the K it
covers: the gold stroke averages 199,131,10 in the still, 221,185,20
in the page and 213,179,22 on the original's resting screen. So at
rest the K is the page's own, and while it plays the engine adds the
clip's change from its still onto the page (D-059). Nothing on the
resting menu moves. The dragon only snorts when the cursor is over it.

Entering sets the hover flag and, from rest, starts clip 5
(legacy:148064-148070). Each tick the button hands over only once the
clip's frame counter has reached its frame count
(legacy:148038-148054): 5 goes to 6, 7 goes to 2, and 4 goes to 6
while the flag is up or to 2 once it is down. Leaving is honoured only
in state 6, where it clears the flag and starts clip 7
(legacy:148027-148035), so a cursor that leaves during the enter clip
waits for it. State 6 has no hand-over of its own, and the decoder
wraps a clip nobody stops (legacy:35341), so the hover clip loops while
the cursor stays. `machine6.bik` is forty frames of the machine idling
and `girl6.bik` thirty, which only makes sense as a loop.
`knight6.bik` and `snort6.bik` are single frames. A click drops the
door to state 4 (legacy:148002-148011).

### The reels

One clip player runs the logo at startup (legacy:241882), which the
`-skiplogo` switch skips (legacy:252020), the intro on the first Story
click of a session (legacy:140763-140767) and the credits from the
Credits door (legacy:140736-140738). The intro's flag is never
cleared, so it plays once a session. The music is paused around a
reel.

A reel ends on its last frame, or early on a key. The clip player's
message loop ends on a character message or a system key message,
which is any key that types a character, Escape and Enter included,
and Alt or F10 (legacy:34816-34849). A mouse click is dispatched and
ignored, and the mouse events queued during the reel are drained
afterwards (legacy:34793-34797). Skipping the victory reel also skips
the credits that would follow it (legacy:154103-154106).

## The loading screen

The load screen is `data/guis/loadscreen.gui` and nothing else
(legacy:158058). Three layers make the picture, in the order the file
authors them.

| Widget | Rect | Art |
|---|---|---|
| `LoadScreen` (root) | 0, 0, 640x480 | `loadingbg.gaf`, `LoadingBG` frame 0 |
| `Background` | 168, 46, 423x351 | `loadingbw.gaf`, `LoadingBW` frame 0 |
| `AnimatedControl` | 168, 46, 423x351 | `loadingc.gaf`, `LoadingC` frame 0 |

`LoadingBG` is a stone wall with an empty arch cut out of it.
`LoadingBW` is a stained glass window unlit and `LoadingC` the same
window lit. Each GAF carries its own palette in a `.pcx` of the same
stem next to it, so none of them goes through the central palette
table.

The arch's clip, `Movies\Gui\Loadscreen.bik`, is opened at the
`AnimatedControl` widget's top left corner (legacy:158297-158299). The
original passes only the widget's x and y, never a size, so the clip
shows at its own 422x351 and the last column of the authored glass
stays visible beside it. The clip is not played. Each tick its frame is
set to the load percentage's share of the frame count, never below the
first frame and never past the last (legacy:158702-158710). The exact
expression is not established, but its inputs are the percentage and
the count, and the result is clamped to 1..count. When the clip cannot
be opened the still art is what the player sees.

The dialog is authored with placeholder text and with per player
progress rows for network games. The original clears and hides all of
that before the screen is first drawn (legacy:158031). It blanks
`LoadText`, sets `Percent` to `0%`, writes the map name into `LoadMap`,
and hides `PlayerName<N>`, `PlayerPercent<N>` and `PlayerProgress<N>`
for eight slots. Each frame it then writes the current phase into
`LoadText` and the percentage into `Percent` (legacy:158380).

The fourth widget the file authors, ahead of both glass layers, is the
progress bar `MainProgress`. A progress bar, widget type 16, writes a
version, then value, step and max, then a byte flag that is read back
only above version 1 (legacy:328245, legacy:328267). The base record
follows. Every shipped dialog is version 2, so all four fields are
present:

```
16 2 50 1 100 0
```

`loadscreen.gui` and `byupdateconsole.gui` are the only shipped dialogs
that use the type.

`data/anims/loading.gaf` holds a single 640x480 composite of the wall
with the unlit window already in it. No dialog references it.

## Scroll arrows

Twelve of the game's dialogs author scroll arrows, 36 arrows in all:
load game, save game, select game, choose map, the four options
screens, both battle menus, the player dialogue and the map view. In
each of them the list box or slider track an arrow serves is authored
before the arrow and encloses it: the `GameList` list box in load, save
and select game, `PlayerList` and `MapInfo` in two other lists, and the
`SoundVolume` and `ScreenSizeSlider` tracks in the options screens. A
widget authored later draws over one authored earlier, and a click on
an arrow belongs to the arrow. An arrow held down repeats.

## A click on the ground

The terrain draws lifted by half its height (legacy:197689), and units
draw lifted the same way, so the flat world point under the pointer is
not the ground the player sees there. The original finds the cell under
the pointer by walking the terrain for the one that projects there
(legacy:212277). Every order on the ground resolves through that cell:
a move, a patrol, an attack on the ground, an unload, a build, and the
revive, raise and sweep clicks.

## The pointer with no command armed

With no command armed, the original's default cursor offers an order by
what lies under the pointer, read against the local player's selection
and fog.

- A damaged unit at peace with the player, the player's own or an
  ally's, offers Heal when a selected unit of the player's could mend
  it by the original's repair test: a finished unit standing still on
  the ground, and a mender that walks and repairs
  (legacy:186541-186545, legacy:233526-233553). A click sends the
  menders. A unit at full health offers the select hand.
- The player's own frame offers Heal when a selected unit can help
  build it.
- A feature on explored ground offers the sweep cursor when the
  selection holds a unit of the player's that can reclaim
  (legacy:186587-186599). A click sends the sweepers to sweep it and
  the other walkers to the spot, the way a click on a frame splits
  them.
- A body the selection can raise offers raising before sweeping, see
  [features.md](features.md).
- An enemy offers the attack. The sweep cursor is never offered over a
  live building.

Shift changes none of this, so a Shift click on a unit the selection
could mend queues the mend.

## The sidebar

Each side's in-game sidebar comes from its own `.gui`, `araingame.gui`
for Aramon. It authors two pairs of contextual buttons, each pair on
one rect.

| Widget | Rect | Art |
|---|---|---|
| `HEAL` | 529, 223, 29x29 | `RepairButton` |
| `LOAD` | 529, 223, 29x29 | `PickupButton` |
| `CLEAR` | 599, 223, 29x29 | `ClearButton` |
| `UNLOAD` | 599, 223, 29x29 | `DropButton` |

The unit-menu refresh shows at most one of each pair by setting widget
visibility (legacy:150749-150831): `LOAD` and `UNLOAD` by the transport
flag (legacy:150749-150775), `HEAL` (legacy:150776-150792), `CLEAR`
(legacy:150793-150808) and the shared backings
(legacy:150809-150831). A transport gets `LOAD` and `UNLOAD` and a
healer or reclaimer `HEAL` and `CLEAR`, so the two buttons on one rect
never show together. The build menu shows only for a builder
(legacy:150134).

No `.gui` has a widget named Sweep. The broom on the sidebar is
`CLEAR`, whose art is `ClearButton`, entry 7 of `igcommonbuttons.gaf`,
a 29x29 broom. It gives the reclaim order and shows for a unit whose
FBI has `canreclaim`, such as `araking.fbi`, `arabuild.fbi` and
`arapries.fbi`. The sweep takes features and corpses as well as units,
see [features.md](features.md).

The order cursors are entries of `cursors.gaf`, in file order: 0
`CursorAttack`, 1 `CursorDefend`, 5 `Cursorload`, 6 `CursorMove`, 8
`CursorPatrol`, 15 `cursorrepair`, 16 `CursorUnload` and 22
`Cursorreclamate`. The original loads its cursor handles in an order
of its own, which is not the file's.

### With nothing selected

The unit-menu refresh hides every child of `UnitMenu` except
`CrystalBall`, `HelpText`, `PositiveM` and `NegativeM`
(legacy:151133-151166, the list at legacy:151146-151152). The
`UnitMenu` panel itself, the sidebar background with the `ButtonPanel`
art at 512, 128, 128x352, stays up whatever is selected, and so does
the crystal ball.

On the bottom bar, the HUD unit-info refresh (legacy:152219) called
with no unit (legacy:152277-152296) hides `UnitText`, `HealthBar`,
`ManaBar` and `Experience`. It asks for `HealthBack` and `ManaBack`
too, but no side's `.gui` has those names, since the backings are
`Static0`, so the empty backings stay up. `KillCount` is hidden
(legacy:152496-152506), and the refresh for a selection of several
units does the same (legacy:152512). The `InfoPanel` strip art,
`HelpText`, `PositiveM`, `NegativeM`, `BottomBar` and `EndCap` stay.
`ManaBar` is the selected unit's own gauge, not the player's mana.

## Build buttons

The build button handler (legacy:150046-150101) passes a count to the
queue: one for a click, five with Shift and ten million with Ctrl,
negative for the right button. The queue (legacy:181790-181866) is a
list of runs, each one def and a count.

- An add joins the last run when it is the same def and appends a new
  run otherwise.
- Ten million or more is a run without end, drawn as `+++` on the
  button (legacy:149925-149930). Nothing is added behind one, since it
  would never be reached.
- A removal takes from the last run of that def first and works back,
  and the product in training goes last. Reaching a run without end, or
  a count of ten million, takes every one of that def, the one in
  training included. That is how the manual's right click on the unit
  icon cancels a Ctrl click.
- A count is taken down when its unit is done (legacy:9524-9527) or
  when its frame is lost, killed or taken, on the pad
  (legacy:9293-9303), so a lost unit is not trained again. A run
  without end is never taken down.

How a factory trains from its queue, its rally point and Stop are in
[economy.md](economy.md). The sounds of the buttons are in
[sound-triggers.md](sound-triggers.md).

### A builder's build buttons

A right click on the build button of a builder that walks sends the
same queue call as a factory's, with the count for every one
(legacy:150067-150093). For such a builder the call walks its order
list from the head and takes off every build order of that kind
(legacy:39336-39351, legacy:181838-181866). The head is the order in
hand, and the removal unlinks it like any other
(legacy:180806-180826). Moves, other kinds of building and every other
order stay where they were.

When the order in hand goes, the builder stops and takes its next
order, if it has one. Its frame stays on the map as it stands. Nothing
is refunded and nothing is taken off. A builder still on its way has
no frame yet and leaves none.

## Shift orders

The manual's section on orders queues moves, builds, heals and patrols
with Shift. A click with Shift keeps the order armed for the next one,
since the mode is kept while Shift is down (legacy:243648,
legacy:243685). A unit's order lists, and which orders a new one
clears, are in [economy.md](economy.md).

How the original walks more than two patrol points given with Shift is
not established. The manual's example patrols between two points
queued with Shift. The engine takes them as one route, walked in the
order given and back through the point the unit started from, round
and round.

A factory takes Move and Patrol as standing orders for its products,
as the manual describes, and each product carries on with the ones
added with Shift once it reaches the rally.

A click with Ctrl changes the order in hand and keeps the queued ones
behind it, which the manual gives as the way to send a builder
elsewhere without losing its build queue.

While Shift is held the original draws the orders of every unit of the
player's that is selected, under the pointer, followed by the camera or
the one unit selected (legacy:210741-210744, legacy:238325-238346). A
build order whose frame is not up shows as a ghost of the building at
its spot (legacy:179596-179649). When the unit under the pointer, the
one followed or the one unit selected has a build list, every other unit
of the player's shows its queued buildings as well
(legacy:238305-238307, legacy:238344). The manual says as much for the
selected unit: "If you hold the Shift button down, you'll see ghost
images of that summoning unit's queued tasks." The engine has no unit the
camera follows, and how it draws the lines is D-031.

## Veteran rank

A unit's veteran level (legacy:232934) is 0 when its FBI has
`noveteran` (read at legacy:163080, default 0). Otherwise a non-zero
per-unit override gives the cap directly, and otherwise the level is
the unit's experience divided by its type's `experiencepoints` (666
when the file has none), clamped to the cap (legacy:232945-232948).
The cap is 10. The multiplier (legacy:232971) is 1.0 plus 0.1 a level
(legacy:232983), so it tops out at 2.0. Both constants have console
setters (legacy:38324, legacy:38338). The level setter that cheats and
missions use writes the experience as the level times
`experiencepoints` (legacy:232954).

A kill adds one to the killer's kill count and the victim's
`experiencepoints` to its experience, and a kill on the killer's own
side earns nothing (legacy:227303-227328). `araking.fbi` has
`commander=1`, `noveteran=1` and `experiencepoints=151`, and
`commander` defaults to 0 (legacy:163074). What the multiplier does in
a fight, the `manapershot` discount (legacy:250539), the two per-unit
modifiers it scales (legacy:235859) and the veteran projectile model
(legacy:246620), is in
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

The rank shows in two places, and nothing is drawn over the unit in
the world. The first is the HUD's `Experience` widget, which the
manual calls the Experience Shield. In `araingame.gui` it is a type 19
Static at 491, 454, 11x22, with three states, frames 0 to 2 of the
`UnitLevels` animation at 13x22, entry 6 of
`data/anims/guageselements.gaf`. The Taros, Veruna and Zhon in-game
dialogs carry the same widget. The HUD unit-info refresh
(legacy:152219) reads the unit's level (legacy:152440-152450). Level 0
hides the widget, and any other level shows frame level minus 1,
clamped to the three authored states.

The second is the model's gold pieces. A unit script reads
`get VETERAN_LEVEL`, port 32, which returns the raw level
(legacy:223313), and shows or hides them. `Arasword.bos` shows
`Torso5`, `ArmUL5` and `ArmUR5` above level 4 and `Sword10` above
level 9, which fits the range 0 to 10.

The `KillCount` label shows the kill count as text only for a unit of
the local human player with a count above 0 (legacy:152496-152506).
