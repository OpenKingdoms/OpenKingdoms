# The Harpy takes units over

Reported from play in issue 177. A Zhon Harpy attacking another
player's units did nothing to them. The engine side is
`src/render/units.c`, with the order arriving through
`src/net/command_exec.c`. The `:NNNNN` marks are reference anchors a
maintainer can use to check a claim again.

## What the data says

`units/zonharp.fbi` carries `cancapture = 1`, its own pool of
`maxmana = 1000` filling at `manarechargerate = 10` a second, and one
weapon named Individual Mind Control. That weapon is
`type = line of sight` with `subtype = mindcontrol`. It costs
`manapershot = 300`, reloads in 1.5 seconds, reaches 300 and flies at
169. Its damage table gives `default = 1` and zero against airship,
dragon, factory, fort, god, lodestone, monarch and naval.

The same subtype is on the Taros mind mage, once as a single shot and
once as an area spell, and on one campaign character.

Sixteen units carry `cantbecaptured = 1`. They are the four dragons,
the four gods, `arawar`, `verflag`, `verharp`, `verman`, `verscout`,
`vertrans`, `vertre` and `zonkrak`.

No file carries a rate, a chance or a cost for capture. The rule is in
the executable.

## What the original does

Capture is a weapon, and an attack order is how a player uses it. A
Line of Sight weapon with `subtype = mindcontrol` gets a behaviour of
its own when the weapon is read (:249801). Its shot leaves like any
straight shot and has a flight of its own, set out below under The
shot's flight. When the shot reaches a unit, the behaviour's hit routine
runs in place of the damage routine (:245361). A mind control hit
therefore never wounds, whether or not it takes the unit. The damage
table is used for one thing, which is deciding what the weapon may be
aimed at (:15160).

The hit routine (:247761-247798) does nothing at all to a unit that

- already belongs to the shot's owner (:247769)
- is a structure (:247772)
- is a commander, which is to say a monarch (:247776)
- is still being built (:247779)
- is aboard a transport (:247782)
- has `cantbecaptured` on its type (:247785)

For any other unit it rolls a number from 0 to 99 and compares it with
a threshold worked out from the victim's veteran rank, which is
`(rank + 16) * 5` held to 99 (:247788-247793). A roll under the
threshold takes the unit.

| Victim's rank | Chance |
|---|---|
| 0 | 80 in 100 |
| 1 | 85 in 100 |
| 2 | 90 in 100 |
| 3 | 95 in 100 |
| 4 and up | 99 in 100 |

A veteran is easier to take than a recruit. That reads backwards and
it is what the original does. The victim's health plays no part. For
an area weapon the threshold is multiplied by the same falloff the
splash gives damage (:245217-245224).

A unit that is taken is handed over by the routine the give units
action and the mission script's Capture command also use (:228891,
called from :155792, :178632 and :247794). It does not relabel the
unit. It makes a new unit of the same type for the new owner on the
same spot (:228975), copies across the health and the build progress
(:228988-228989), the facing and the on or off state, and then
removes the old unit with a cause of death that the death handler
skips the Killed script for (:228998, :227130). So there is no death
animation and no body. Everything else about the unit starts again.
Its kills, its experience and rank, its orders, its stance, its
reload and its own mana are those of a unit just made. If the new
unit cannot be made, the old one stays where it is (:228975).

When choosing a target and again before firing, a mind control weapon
passes over a unit with `cantbecaptured` and a unit still being built
(:15151, :19070, :21157), and like every weapon it passes over a unit
its damage table zeroes (:15160). So a Harpy never spends a shot on a
dragon, a god, a monarch, a ship or a building.

The cost is the weapon's `manapershot` and nothing else. Each shot
takes 300 from the Harpy's own pool whether or not the roll succeeds.
A full pool is three shots, and after that the refill allows one shot
every thirty seconds.

The original has a CAPTURE button and a hotkey for units with
`cancapture` (:150652, :151122, :151767). None of the five in game
gui files that ship has a widget by that name, so the button never
appears, and the capture cursor is loaded (:161432) and never shown.
The attack cursor and the attack order are the whole path.

The original draws its roll from the C library generator, which its
machines do not share. It is not a lockstep game, so it did not need
them to.

## What the engine does now

The unit parser reads `cancapture` into a capability flag and
`cantbecaptured` into the definition, and marks a weapon whose subtype
is mindcontrol. A shot copies that mark when it is fired. It is
simulation state, so it goes into the state hash and into the `PROJ`
record of a save, which grows from 216 bytes to 217 with the new byte
at the end. A save written before this reads the byte back as zero,
which is what those shots were.

Where a shot hits a unit directly, and where a splash reaches one, a
marked shot runs the rule above in place of the damage. The roll is
`World_Rand(100)`, the seeded simulation generator, so every machine
draws the same number on the same tick. A unit that is taken goes
through `Units_Capture`, which lifts the old footprint, spawns the new
unit for the new owner, copies health, facing and build progress, and
removes the old unit with no death script and no body. A seat at its
unit limit takes nothing.

`weapon_can_target_unit` applies the aiming rule, so a Harpy neither
picks nor fires at a unit it could not take.

`TAK_CMD_CAPTURE` had been declared on the wire and ignored. It is now
the attack order for a unit that carries `cancapture`, and refused for
any other. The HUD needed nothing. The attack cursor over an enemy
already sends the attack, in both views, because both views sit on the
same simulation.

`cantbecaptured` joins the definition hash a save checks its units
against, as every field that steers the simulation does. A save made
before this that holds one of those sixteen units or a Harpy is
refused with the message that the unit has changed.

## What is left out

The original refuses to aim any weapon at a unit its damage table
zeroes. Here that rule is applied to mind control weapons only, to
keep this change to the Harpy. Applying it to every weapon is its own
piece of work with its own tests.

The original copies the on or off state to the new unit. The engine
does not. A unit with `activatewhenbuilt` is switched on by the tick
as any new unit is.

A flyer taken in the air comes over landed, as any new unit starts.

A loaded transport can be taken, because the hit routine asks only
whether the victim is itself a rider (:247782). What the original does
with the riders was not established. Here they step out where the
transport stood, still their old owner's, and the transport comes over
empty.

The give units action still changes the owner in place, so a gift
keeps its kills and rank where the original would reset them.

Turn to stone and turn to frozen use the same roll (:247865) and are
still not simulated.

## The shot's flight

Added 2026-10-04. The owner found mind control "went from overpowered
to mind numbingly powerful as it now hits every time and is instant
instead of having a high chance of missing". The roll, the cost and the
speed above were the original's all along, and nothing merged since
this note had changed them. The flight was not.

What the original does with the shot:

- There is no cast time. The launch starts the shot on the next frame
  (:246909). A `nimbus` weapon waits `nimbustime` times 30 frames more
  (:246910-246915, :250013-250015), and no shipped file sets
  `nimbustime`, so the Harpy never waits.
- The shot leaves on a straight line for the target's sweet spot plus
  the target's velocity times four fifths of the flight time
  (:234031-234060, the multiply by 0xCCCC at :234049). The flight time
  is the distance from the caster to the sweet spot over the weapon's
  speed.
- It flies at `weaponvelocity` along that line (:246885-246892). A
  Harpy 200 px up firing at a unit 220 px away crosses the ground at
  about three quarters of 169 px a second.
- Each frame the shot is freed if its caster is dead or dying
  (:247710-247713), after that frame's step. Every step runs the cell
  test (:247736). The shot strikes a unit of another owner holding its
  16 px cell only when its height is within that unit's model span and
  its place is inside the selection quad of the unit's root piece,
  turned with the unit (:236974-237027), or a flyer over the cell whose
  span holds its height (:245426-245437). There is no reach around the
  target, and the quad is about 29 px across for a swordsman.
- It lives its range over its speed across the ground and one frame
  more (:246919-246922), then vanishes with no effect.

So a still recruit is struck by every shot and taken four times in
five, and a veteran of rank 4 or more 99 times in a hundred. Leading by
four fifths, the shot lands where a unit walking steadily across its
line stood a fifth of the flight earlier. For a swordsman at 33 px a
second and a flight of 1.7 seconds that is 11 px behind it, inside its
quad. A faster unit, or one that turns or changes pace while the shot
is in the air, can be missed, and a recruit keeps its side on one roll
in five.

The engine had the roll right and the flight wrong. It struck its
target anywhere within 24 px of its body, aimed where the target stood,
lived one and a half times its aim distance, flew on after its caster
died, and crossed the ground at the full 169 px a second however steep
its line. A mind control shot now follows the original in all five.
The selection quad is read from the root of the unit's 3DO along with
the model span, its corners in the order the original walks them, and
a definition with no model takes its footprint. Every other shot keeps
the reach and aims where its target stands (D-038).

On two castles, a Harpy 220 px from a passive swordsman fires once it
has climbed, 116 ticks in. Its shot used to land 65 ticks later. It now
takes 97, a little over a second and a half, and one at a swordsman
walking across takes 103. The Harpy reloads in 90 ticks, so a second
shot is often in the air when the first takes its target. Four still
swordsmen were taken from four seeds before and after. Four walking
ones were taken after, where none were before.

What read as instant is most likely the Harpies' stack. A Harpy fires
from wherever it is inside its 300 px reach, and the stack sits close.
At 100 px a shot used to land in under half a second. The original
holds a hovering attacker `hoverattackdistance` off its target, 200 px
for the Harpy (:163005-163007), which the engine does not do yet
(D-039).

The quad is a function of the 3DO file, which the data fingerprint
already hashes, and the fingerprint's schema moves to 4 because the
engine now reads more of that file. The definition hash a save checks
leaves it out, as it leaves out the model span, so saves made before
this still load.

## How it is checked

`test_command_pipeline` has a Capture group that needs no game data.
A Harpy ordered onto an enemy turns it to the Harpy's side and pays
for the shot. The capture order on the wire sends a Harpy to attack
and is refused by a unit without `cancapture`. A wounded unit with
kills comes over with its health and without its kills, its
experience or its stance, and answers to its new seat only. A unit
with `cantbecaptured` is never fired on and costs nothing. A monarch
is fired on, paid for, not taken and not wounded. A seat at its unit
limit takes nothing. A loaded transport comes over empty and its
rider steps out. A shot with a splash rolls for each enemy it reaches,
wounds none and leaves alone the one out of reach. The thresholds
match the table above. Two runs
of one battle from one seed change the owner on the same tick and
produce the same hash after every tick.

The first two of those were written first and failed on the engine
as it stood. The capture cases now use a prey two cells a side, as
every shipped unit is, since a shot strikes only the unit holding its
own cell.

For the flight, a shot passes a unit that stepped 20 px off its line
and draws no roll, leads a walker by four fifths of its flight, ends
after its range, and dies with its caster. A unit's quad turns with
it. These failed on the engine before the change. A recruit standing
still is struck by every shot and taken exactly when the seed's first
draw is under 80, which holds before and after. In `test_ui_screens`
a shipped Harpy takes a shipped swordsman, standing or walking across,
from four seeds each.

`test_sim_hash` shows the new mark moves the hash, and the save round
trip in `test_savegame` carries it.
