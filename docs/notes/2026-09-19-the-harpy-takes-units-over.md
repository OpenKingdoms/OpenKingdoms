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
its own when the weapon is read (:249801). Its shot flies like any
flat shot. When the shot reaches a unit, the behaviour's hit routine
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
as it stood.

`test_sim_hash` shows the new mark moves the hash, and the save round
trip in `test_savegame` carries it.
