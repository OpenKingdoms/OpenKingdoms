# Game flow

This note covers how the original paces its simulation and what one
tick does, game speed and pause, the battle options, how a battle is
won and lost and what the end screens show, the line for a fallen
player, and the original's two random number generators.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## The tick

The original simulates at 30 ticks a second. An hour is 108000 ticks,
0x1A5E0 (legacy:21475), the frame pacing divides 30 by the frame rate
(legacy:243014), and 900 ticks stand for 30 seconds (legacy:240040).
The engine runs at 60, see D-001.

The game state's timing block holds the tick counter, raised at
legacy:242441, the last clock reading, the ticks issued this frame, the
frame's delta, a float accumulator for the fraction, a hysteresis
counter, a status word, the id of the slowest peer and the tick delta
against it. The status bits are, in order, paused (bit 0), held back by
lag (bit 1), severe lag (bit 2) and speed reduced (bit 3). Milliseconds
become ticks as ms times 30 over 1000 (legacy:263924).

### Pacing

Each rendered frame, `GameLoop_CalcSimTicks`
(legacy:242330-242421) works out how many ticks to run.

1. The delta is the clock now less the last reading. With cheats on
   and a player count that is not zero, the delta is replaced by the
   player count. That is a fixed step debug mode, not a timing path.
2. The rate is the current speed level times 0.1. The level is the
   interface speed option, 0 to 20 with 10 as normal, so the rate is
   1.0 by default.
3. Once lag reaches 120 ticks, the rate is multiplied by
   max(1 - min(lag, 3600) / 3480, 0.01). Past 449 the severe bit is
   set. Lag is measured against the slowest peer.
4. The accumulator is the delta times the rate plus the carry. The tick
   count is that rounded, and the carry is what is left over, which
   keeps the simulation on the wall clock on average.
5. While the game is not paused, a count under 6 takes one off the
   hysteresis counter, and once that falls below -100 the game speeds
   up by itself. Otherwise the count is clamped to 5 and the counter
   gains one, and once it rises above 10 the game slows down by itself.
6. While the game is paused the count is 0.

So a frame runs at most 5 ticks plus the carry, and the speed adjusts
itself through the hysteresis.

### What a tick does

Inside the tick loop of `GameLoop_SimulateTicks`
(legacy:242425-242513) the order is: unit positions, then the units
themselves, physics, publishing peer data, each player's AI, network
game state, the simulation step, camera tracking, a callback reset,
heightmap normals, debug markers, the terrain tick and the COB step,
which is the script engine's per-tick entry point. Only on a full tick
does the network sync flush follow.

Once per frame, outside the tick loop, come the AI's batch pass, three
entry points one after another, then the feature pool reset, the expiry
of line of sight entries, features spawned from data and the terrain's
dirty flush.

Within the tick some work runs every 3rd tick, 10 times a second
(legacy:128381), some every 10th, 3 times a second (legacy:128638),
and some every 30th, once a second.

## Game speed and pause

The interface options carry a requested level and an effective level,
both 0 to 20, with 10 as the default. `GameSpeed_SetLevel`
(legacy:131724-131806) clamps the level and holds it at 1 or more in
multiplayer only, game mode 3 (legacy:131753-131756), so a campaign or
a skirmish can reach 0, a full stop. A change writes both levels, shows
"Game Speed Normal" at 10 and otherwise "Game Speed" with the signed
difference from 10, and sends network packet 0x18
(legacy:131724-131804). The + and - keys are legacy:131808 and
legacy:131819. The slowing and speeding by itself come from the
hysteresis above, and the effective level climbs back toward the
requested one at legacy:131830. The reset is legacy:131708.

Speed enters the simulation only as the rate, the level times 0.1. It
changes how many ticks are issued, never what a tick does, so it
cannot break determinism and is safe to offer in multiplayer.

Pause is bit 0 of the timing status, and the pacing gives no ticks while
it is set. Pause stops the simulation only: drawing and input go on.

The single player menu pauses the game by itself, set at legacy:154886
and cleared at legacy:154938, except in game mode 3. The flag word for
game over and shutdown is not the pause flag (legacy:227347).

## Battle options

The skirmish options are the `GameOptions` section, read at
legacy:131611-131663:

| Key | Range or meaning |
|---|---|
| `side` | 0 to 7 |
| `MonarchDeath` | whether the monarch is expendable |
| `MultiTimeOutSecs` | 30 to 150 |
| `RandomStartLocations` | bool |
| `LosOn` | bool |
| `MappingOn` | bool |
| `CanDropPlayer` | bool |
| `AllowCheating` | bool |
| `DisableMapScript` | bool |
| `EnemySide` | side index |
| `CrusadesBalance` | bool |
| `FavoriteUnitsPerPlayer` | 20 to 500 |

The engine's unit cap has another range, see D-002.

`LosOn` and `MappingOn` are two independent booleans, not one three
way choice, and they work on different per-cell maps. With Line of
Sight off, what a player sees is read from the u16 bitmask with a bit
per player, the array a save's `Mapping` section stores. With it on,
it is read from the player's own byte array, which carries its own
width and height. `MappingOn` is the Map Revealed toggle over the
explored state (its label is at legacy:139258). What each does to sight
and the fog is in [fog.md](fog.md).

The options screen is legacy:139199-139290. Its multiplayer checkboxes
are MonarchDeath, StartLocations, Mapping, LineOfSight, CheatCodes,
Watching, GameStatus, MapScript, SlowGame and CrusadesBalance. The
single player labels are "Monarch Expendable", "Random Start
Locations", "Map Revealed", "Line of Sight" and "Power Codes".

`AllowCheating` gates the console, which the interface calls Power
Codes. With cheats on, the frame timer also takes the fixed step mode
above (legacy:242345). Cheat entry is legacy:243004 and the console
commands are legacy:36220-36400.

## How a skirmish ends

### What the verdict reads

Each player record carries two counters, and the verdict reads those
and nothing else. Making a unit adds one to the live count for every
unit, and one to the units built count for every unit whose kind is not
a feature (walls are, by `isfeature`, legacy:163000), at
legacy:226969-226973. Freeing a unit record takes one off the live
count (legacy:227378). Nothing else touches them, so a dying unit
counts until its death sequence ends, and a lodestone counts like any
other unit.

Once a second for each player (legacy:206625-206662) the game asks two
questions, in this order. First, the local player has lost when they
have built something and their live count is zero
(legacy:240018-240028). Then, the local player has won when they have
built something and every slot outside their alliance has a live count
of zero (legacy:239992-240013).

Neither has a grace period. The 30 s wait belongs to the Boneyards
branch alone (legacy:240032-240063, gated at legacy:240040, and
legacy:240067-240077), which is reached from legacy:240094-240100 only
in game mode 3. A debug hook at legacy:240111 forces a loss somewhere
between 5 and 10 minutes in when debug logging is on. The engine lets a
lost skirmish play on between the computers, see D-034.

### Monarch Expendable

The option is kept as `MonarchDeath` among the battle options
(legacy:131653) and as `CommanderDeath` in a save's Summary, and it
defaults to 1 (legacy:159476). It is separate from the mission
objectives. The checkbox shows frame 3 when the value is set and frame
4 when it is clear (legacy:139327), so a set value means the monarch is
not expendable.

With the value set, the death of a unit whose kind carries the
`commander` flag (legacy:163074), or of a transport carrying one
(legacy:227100-227108), removes that player's whole army on the spot
(legacy:227174-227181). The removal (legacy:227541-227580) kills every
unit of the slot with death type 10, which skips the `Killed` script
and frees the record at once (legacy:227241-227260). A campaign mission
skips this and relies on its own defeat conditions (legacy:227177). The
live count is then zero and the check once a second does the rest.

With the value clear, a player stays in the battle while any unit of
theirs remains, structures included. What ends such a game in the
original is its AI, whose attack groups score every enemy unit by
distance and only discount an unseen one (legacy:15365), so a last
lodestone is found and destroyed.

The engine departs in one respect without a deviation entry: a dying
monarch plays its death sequence rather than being freed at once, and
its player counts as eliminated from the moment it dies. The engine's
computer player keeps a reduced form of the scoring above, in which
raiders that stand at an enemy start with nothing in sight take the
nearest enemy unit anywhere.

### Tallies

| Tally | Where | What counts |
|---|---|---|
| Units built | legacy:226971 | every unit made whose kind is not a feature |
| Kills | legacy:227302 | one for the killer's player, when the victim belonged to another player |
| Score | legacy:227305 | the victim kind's `experiencepoints` (legacy:162918, default 666), on the same condition |
| Losses | legacy:227296 | one for the player whose unit was destroyed |
| Time | legacy:206617-206619 | the current tick, stamped once a second on every player who still has units |

Death type 10 credits no kill or score and counts no loss. The screen
prints the time stamp as hh:mm:ss, so a beaten player's time stops
where they fell.

### The screens

The verdict loads a one-label dialog over the in-game desktop,
VictoryText.gui or DefeatText.gui, with the word in the 48 px face,
centred over the play area: the screen less the 128 px sidebar and the
48 px bottom strip (legacy:145740-145744). The battle keeps running
under it. The verdict also starts a countdown of 90 ticks, 3 s at
30 Hz (legacy:206566), and when it runs out the game stops the
simulation and opens the statistics dialog (legacy:206533,
legacy:244079-244086).

The statistics dialog is Defeat.gui, or Victory<prefix>.gui for the
local side's `nameprefix` (legacy:153765-153776). It fills one row per
slot that built something (legacy:153968): the badge from
teamlogos.gaf `<prefix>team` at frame colour plus 2, the player's
name, units built, kills, losses, time and score
(legacy:153975-154004). In a skirmish Proceed's help text is
`SKIRMISH_BATTLE_ROOM` from the translate table (legacy:154043).
Proceed leads back to the battle room and Main Menu to the main menu
(legacy:154080-154131). The buttons carry their own click sounds in
the dialog file, ok.wav and cancel.wav, the help strip shows the
hovered button's text, and the root widget's accelerator string maps
Enter to Proceed and Escape to Main Menu.

No movie plays for a skirmish. `Movies/Victory.bik` runs from Proceed
after the last campaign mission only (legacy:154103).

## Mission objectives

A campaign mission's victory and defeat conditions are read from the
mission file at legacy:239243-239850. The eleven victory and seven
defeat conditions, and how each is read, are in
[2026-09-15-mission-conditions.md](2026-09-15-mission-conditions.md).
`KillUnitType` takes a unit type and a number, `MoveUnitToRadius` a
unit type and three numbers, and `UnitTypePassesX` and
`UnitTypePassesZ` a unit type and a number. Coordinates are read out
of the argument and shifted left 20 bits into 20.12 fixed point. The
literal `ANYTYPE` matches any unit.

Each objective answers six calls, in this order: whether it is
satisfied, a unit killed, a unit built, a capture or a plane passed,
save, and load. Victory needs the whole victory list at once
(legacy:239922). Each objective carries two flags, `Satisfied` and
`Celebrated`, and the second exists only to stop the victory voice line
playing again each time a save is loaded. A save keeps the flags and
not the objectives, see [saves.md](saves.md).

## A fallen player

When the last unit of a player is gone, in a skirmish or a match, the
original adds one line to the message list, picked at random from
english/translate/death.tdf: DEATH1 to DEATH4, "%s has fallen.", "%s
has been vanquished.", "%s is no more." and "%s has been defeated.",
with the player's name in place of the %s. It is a type 4 entry owned
by the fallen player (legacy:206939-207005, legacy:227378-227387). A
campaign mission adds none. The original picks the line with the C
library's rand, which no other machine shares. The cue that goes with
the line is in [sound-triggers.md](sound-triggers.md).

The engine adds the same line when a player's units on the map, the
dying ones counted, drop to none. It picks the line from the battle
tick and the seat, so every machine shows the same one, and it plays a
cue of its own, see D-043.

## Random numbers

The original has two generators, and they are separate streams.

### The simulation's generator

It is a Lehmer generator, Park and Miller's MINSTD, computed by
Schrage's method so the product in between never overflows 32 bits.
Its state is one 32-bit word (legacy:6243). The draw is at
legacy:254475-254486 and the seeding at legacy:254490.

| Constant | Value |
|---|---|
| Multiplier a | 16807 |
| Modulus m | 2147483647, 2 to the 31st less 1 |
| Schrage's q, m over a | 127773 |
| Seeding xor mask | 0x66E29572 |

Seeding sets the state to the seed xored with 0x66E29572 and then ored
with 1, so the state is never zero and its low bit is always set.

A draw for a bound n gives 0 and leaves the state alone when n is less
than 2. Otherwise the state advances one MINSTD step in Schrage form,
the state times 16807 less 2147483647 times the state divided by
127773, and if the result is less than 1 the modulus is added back. The
value returned is the state modulo n.

Four details decide whether a draw matches bit for bit:

1. The multiply and the sum are 32-bit two's complement with
   wraparound.
2. The division by 127773 is signed.
3. The test before adding the modulus back is a signed comparison
   against 1, not a test against zero and not unsigned.
4. The final reduction is an unsigned modulo.

The modulo bias is part of the behaviour. The state modulo n is not
uniform for an n that does not divide the period, the original's
behaviour carries that skew, and an unbiased draw would not match it.

The generator has about 500 call sites in the AI alone, besides weapon
spread, decay timing and idle wander. Anything that draws from it out
of order changes every draw after it.

| Seeded from | Value | Where |
|---|---|---|
| The start of a battle | the high and low halves of QueryPerformanceCounter, not deterministic | legacy:243076 |
| The `-AutoStop` benchmark switch | 1234 (0x4D2), seeded again every tick | legacy:242438 |
| Interface start | 0xD431 | legacy:94347 |
| A network session | nothing: the lobby field named like a seed holds the maximum player count, see [2026-09-04-multiplayer-hapinet.md](2026-09-04-multiplayer-hapinet.md) | legacy:213069 |

### The C library's generator

The C library's rand, seeded from the time at legacy:243078, drives
the pick among a soundclass's variants, the music shuffle, the debug
hook that forces a loss and the line for a fallen player.
