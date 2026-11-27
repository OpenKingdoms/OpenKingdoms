# Sound

Where the original starts a sound during play, what it plays and the
rules around each play: the two play calls and the mixer, sound
classes, weapons, unit scripts, order acknowledgements, alarms,
interface cues, ambient features and music.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

## Two ways to play

Every sound goes through one of two entry points (legacy:221090,
legacy:221204).

A flat play takes a wav name, a priority, a loop flag and a volume, and
plays at centre pan. Order acknowledgements, the named interface cues,
alarms and script sounds in category 7 go this way, at volume 0x7f and
priority 7 (legacy:221247-221281, legacy:223775).

A positional play takes a wav name, a priority and a world position
(legacy:221131-221200). The priority is whatever category the caller
passes. The play at a position (legacy:221204) looks the name up in
the global sound name table, 32 bytes an entry with its own count, and
hands it to the extended play (legacy:221131), which decides whether
and how loud it sounds before it reaches the mixer.

1. The map cell is the position, 16.16 fixed point, shifted right by
   20 bits. A cell off the map is silent.
2. With Line of Sight on the local player must see the cell now, and
   with it off the cell must be explored (legacy:221160-221176). The
   cell tested is the height compensated fog cell of [fog.md](fog.md).
   Out of bounds is silent.
3. The volume is 0x7f inside the viewport and 0x40 anywhere else, a
   flat two step with no distance falloff at all
   (legacy:221181-221190).
4. The pan is 64 plus 64 times the offset from the viewport centre
   over the viewport width, clamped to 0..127 (legacy:221191-221194).

## The mixer

The mixer (legacy:307979-308930) plays at most eight sounds at once
(legacy:307992), from a table of 32 channel slots. Its play entry
(legacy:308347) takes a sample, a volume, a priority, a pan and a loop
flag. Volume runs 0..127, and pan 0..127 with 64 at the centre.

A new sound that finds the channels busy may only steal one that is
loaded, not looping and of strictly lower priority, taking the lowest
priority and, among equals, the oldest (legacy:308165-308203,
legacy:308347-308360). When nothing can be stolen the sound is
dropped, not queued.

The master volume goes through the Windows `waveOut` volume, from the
interface volume option scaled by 1024. At start the mixer plays
`menubutton.wav` once to warm up (legacy:308075).

## Sound classes

`gamedata/soundclasses/*.tdf` hold the unit classes
(legacy:221344-221483). Each outer section is a class, such as
`ARAKNIGH`, named by the FBI key `soundclass`, or `unit_default` when
the key is absent (legacy:162820). The game never reads
`soundcategory`, which eight variants set to a class that does not
exist. Its inner sections are actions
(`select`, `move`, `attack`, `guard`, `patrol`, `default`), and each
key in one is a wav name. The section key `prioritized`
(legacy:221394) sets what the values mean. With `prioritized = 1` a
key's float value is its weight, and with `prioritized = 0` every
entry weighs 1.0.

A class is found by name (legacy:221545) and plays flat
(legacy:221569) or at a position (legacy:221638). An action is matched
by a case sensitive compare against the inner section names, so the
capitals in an action's name count. An action the class does not have
plays the first section in file order rather than nothing
(legacy:221615). The pick within an action draws from the C runtime's
`rand()`, scales the draw by the total weight over 32768 and walks the
entries subtracting weights (legacy:221705). It never touches the
simulation's generator, see [gameflow.md](gameflow.md).

`soundclasses.tdf` holds the hit classes (`sword`, `arrow`, `cannon`,
`fire`, `hammer`, `fist`, `staff`, `rock`, `lightning`). Their sections
are materials (`default`, `flesh`, `armor`, `wood`, `scale`, `stone`)
listing `sound0..soundN`, picked uniformly. An unknown material plays
the first section (legacy:221657-221665). All nine classes sit in the
one base file, so a reader has to walk every top level section and not
stop at the first.

## Weapons

An FBI weapon block can carry `soundstart`, `soundhit`,
`soundhitclass`, `soundwater` and `soundtrigger` (legacy:250159-250185,
legacy:250030).

On firing, `soundstart` plays positionally at priority 4 on each
projectile emission, and only when `soundtrigger` is set
(legacy:245977). No shipped weapon carries either key, so the fire cues
heard in play come from the attack scripts.

On impact (legacy:244977, legacy:245000-245014) a sound plays at the
impact point at priority 4.

- Landing on water with no unit struck plays `soundwater`, or nothing
  when the weapon has none. Water is a cell whose terrain height is
  below the side's water height.
- With `soundhitclass` the material is the `bodytype` of the unit
  struck (`flesh`, `armor`, `wood`, `scale`, `stone`, read at
  legacy:163138 with default `default`). Bare ground has no material,
  so the first section of the class plays.
- Without a class, `soundhit` plays directly.

The unit struck is whichever unit holds the cell the shot comes down
in (legacy:245399-245435). Only another player's unit counts, a
unit's owner byte read against the one on the shot (legacy:245419),
and a flyer counts only while the shot's height falls inside its model
(legacy:245432). An area of effect shot finds it the same way as a
direct shot, although its damage is the splash, and so does an instant
line of sight shot at the ground, whose ray steps through the same
test (legacy:247590-247594).

## Unit scripts

Scripts play sounds with the play-sound opcode, 0x10072000
(legacy:306851). It resolves the name through the script's name table,
pops one argument, calls the host's play-sound callback and pushes the
result. The callback (legacy:223761-223781) reads the argument as
flags. The low three bits are the category, which is also the mixer
priority, so the two never differ, and bit 5 is the loop flag.

- Category 7 is a flat play at priority 7 and volume 0x7f, honouring
  the loop flag (legacy:223775).
- Categories 0 to 6 play at the unit's position at the category's
  priority, and only where the local player can see the unit.
- Categories 0 and 1 also need the unit to be selected and a channel
  free, and never steal one.

A flat-only variant of the same callback exists
(legacy:178318-178328).

In the shipped scripts 226 of 240 calls use category 4, twelve use
category 1 and two use category 7. `attack1..4` and `FireWeapon` carry
the swing and shot cues (`STSWISH3`, `CANNON1`, `ARCHER3`, `ARROW10`),
`Dying` carries the death cry (`ARAKNIGHDIE2`) behind a one in four
roll in every shipped script, and a few walk scripts stamp feet at
category 1. The game code has no cue of its own for a death, a
finished build or an arrival. Those sounds all come from the scripts.

A unit that dies runs `Killed(severity, corpse, kind)` and then, when
the script exists, `Dying(kind)` (legacy:227241,
legacy:227250-227256). `Dying` is where the fall animation and the cry
live, see [2026-09-09-cob-entry-points.md](2026-09-09-cob-entry-points.md).

A script's `rand(lo, hi)` draws from the simulation generator, a Lehmer
sequence with multiplier 16807 folded by the Schrage method, seeded by
xor with 0x66e29572 and forced odd (legacy:306663-306673,
legacy:254475-254490). [gameflow.md](gameflow.md) has the generator.

## Order acknowledgements

Selecting a unit by a click plays its class's `select` action, unless
Shift is held (legacy:237940-237950, legacy:237949). A box select
voices the last unit it took, again not with Shift
(legacy:237800-237806). An order voices the first selected unit that
accepts it (legacy:238647).

The voice (legacy:221247-221281) takes a latched class when one is
set, otherwise the unit type's own class, and with neither it plays
nothing. Selecting a unit latches its class and clearing the selection
resets the latch (legacy:221285), which is what makes a mixed
selection speak with one voice. With the UnitSounds interface option
on, the order code picks the action.

| Order code | Action |
|---|---|
| 1, 2 | `attack` |
| 5, 6 | `guard` |
| 7 | `patrol` |
| 0x0E | `Move` |
| 0x0F | `select` |
| anything else | `default` |

With the option off every acknowledgement plays `default`. Either way
the play is flat at volume 0x7f and priority 7.

## Alarms

The tail of each unit's attack update raises the alarms
(legacy:15218-15235, legacy:15222-15236). It acts when the unit
belongs to the local player, bit 7 of its mission flags is clear, and
it has a recorded attacker: the attacker's player slot, stamped on the
unit when damage lands (legacy:227214-227216), differs from the unit's
own, or the kind of the hit before was 1 (legacy:233764). The damage
handler runs that update for each hit but two kinds
(legacy:233751-233762), so the timers alone decide how often an alarm
sounds. Both timers read the clock, not the
battle's ticks (legacy:221306, legacy:221333), so a faster game speed
does not sound them more often.

- A commander, a unit whose FBI has `commander = 1`
  (legacy:163074-163075), plays `AlarmMon` flat at priority 7 and
  volume 0x7f, at most once in 15000 ms on one timer for the whole
  game (legacy:221328-221340, legacy:221336), on screen or off.
- Any other unit plays the `underattack_sound` of the side its FBI
  names (legacy:221309, legacy:162574) flat, only while the unit is off
  screen, selected or not, at most once in `underattack_delay` seconds
  on one global timer (legacy:221298-221325, legacy:221319,
  legacy:221324). Off screen means missing from the list of units drawn
  inside the battle viewport, the list a box select reads
  (legacy:237338-237356, legacy:237483).

`underattack_sound` and `underattack_delay` (legacy:4696-4697) are keys
of each `SIDE<N>` section of `gamedata/sidedata.tdf`, not of a unit's
FBI. The side data reader walks those sections (legacy:164562-164580)
and reads `underattack_sound` (legacy:164683) and `underattack_delay`,
30.0 when it is missing (legacy:164709-164710). Every shipped side,
`SIDE0` to `SIDE6`, has `underattack_sound=AlarmAra` and
`underattack_delay=30`, and the archives carry `ALARMARA.WAV` and
`ALARMMON.WAV` under `english/Sounds`. There is no separate sting for
first contact. This pair is the sound of a fight beginning.

The engine checks on each hit by another player. The original reads
the attacker recorded by the hit before (legacy:233764-233768), which
the engine does not keep.

## Interface

Each widget in a `.gui` file names the wav it plays when used
(`menubutton.wav`, `ok.wav`, `cancel.wav`, and on the in-game sidebar
`attack.wav`, `move.wav`, `guard.wav`, `patrol.wav`, `stop.wav`). A
click plays it flat at volume 0x55 and priority 4 (legacy:332867).

Other cues name a wav directly and bypass the sound classes. They play
flat at 0x7f and priority 7 unless the list says otherwise.

- `addbuild` plays when a build order is queued or a build icon is
  clicked, and `subbuild` when the queue shrinks (legacy:39328-39330,
  legacy:150087).
- `oktobuild` plays when a placement click lands, and `notoktobuild`
  when it is refused (legacy:243684-243688, legacy:137366).
- `setfireorders` and `setmoveorders` play for the standing order
  buttons, `immediateorders` for self destruct (legacy:151760) and
  `specialorders` for capture (legacy:151772), all within
  legacy:151700-151772.
- `CreateSquad` plays when a control group is assigned (legacy:122211)
  and `SelectSquad` when one is recalled (legacy:122226), within
  legacy:122211-122241.
- `Load` and `unload` play positionally at priority 4 when a transport
  takes or drops a unit (legacy:234558, legacy:234575), with other
  plays of them at legacy:14559 and legacy:27811.
- `Victory Condition` plays when a battle ends in a win or a loss
  (legacy:240280). It is a plain flat play, not a change of music.
- `MessageArrived` plays when the message list takes a line owned by
  another player, a chat line (legacy:205815) or the line naming a
  fallen player (legacy:206939-207005, legacy:227378-227387), see
  [gameflow.md](gameflow.md).
- `Ally` plays for an alliance change (legacy:195010). This and the
  chat cue are heard in multiplayer only.

The shipped archives carry wavs for `MenuButton`, `AlarmAra` and the
other kingdom alarms, `AlarmMon`, `Load` and `unload`, but none named
`addbuild`, `subbuild`, `oktobuild`, `notoktobuild`, `setfireorders`,
`setmoveorders`, `immediateorders`, `specialorders`, `CreateSquad`,
`SelectSquad`, `Victory Condition` or `MessageArrived`. Those cues
start in the original and load nothing, so they are silent there as
well. The engine plays a cue of its own when a player falls, see
D-043.

## Ambient features

Every ten frames the original walks the visible map cells inside the
viewport and, while a channel is free, gives each feature that has a
`SoundClass` a countdown seeded from `SoundDelay` and `SoundVariance`.
When the countdown runs out the class plays flat at volume 0x40 and
priority 1 (legacy:128619-128712). The shipped noise features use the
`woodland`, `wind`, `waves`, `town`, `spooky`, `seatown` and `jungle`
classes of `ambient.tdf`. The countdown lives in the emitter's own map
cell (legacy:128676-128706), so bodies that fall or rot around it
never reset it.

## Music

The original has no music state machine, only one global playlist
(legacy:308412-308830). Its mode is 0 for off, 1 for sequential and 2
for shuffle, and shuffle is the default. The tracks are a scan of
`Music\*.wav`, or `Track%d.wav` numbering when a flag selects it. The
`Music` folder is loose in the install, outside the HPI archives.

When a track ends the update (legacy:308579) moves to the next.
Sequential wraps round, and shuffle steps `(idx + 1) % count` through
a permutation shuffled once with the C runtime's `rand()`
(legacy:308656, legacy:308706). The post-frame update drives the
playlist (legacy:242266-242282), gated by a sound option, and playback
pauses when the window loses focus (legacy:242255). Winning and losing
play `Victory Condition` and change nothing in the music.
