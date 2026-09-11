# Sound triggers

Where the original starts a sound during play, what it plays, and the
rules around each play. Line anchors are `:NNNNN` in the legacy
reference. The engine side of this note is `src/sound/game_sound.c`,
`src/render/units.c` (weapons, scripts, alarms) and `src/ui/ingame.c`
and `src/ui/hud.c` (acknowledgements and interface cues).

## Two ways to play

Every sound goes through one of two entry points (:221090, :221204).

A flat play takes a wav name, a priority, a loop flag and a volume, and
plays at centre pan. Order acknowledgements, interface cues, alarms
and script sounds in category 7 all go this way, at volume 0x7f and
priority 7 (:221247-221281, :223775).

A positional play takes a wav name, a priority and a world position
(:221131-221200). It is silent when the cell is off the map or when the
local player cannot see it: with line of sight on, the cell has to be
lit, otherwise it has to be explored (:221160-221176). Volume is a flat
two-step, 0x7f inside the viewport and 0x40 anywhere else, with no
distance falloff (:221181-221190). Pan is 64 plus 64 times the offset
from the viewport centre over the viewport width, clamped to 0..127
(:221191-221194). The priority is whatever category the caller passes.

The mixer keeps eight channels (:307992). A new sound that finds them
all busy may only steal a channel of strictly lower priority, taking
the lowest and, among equals, the oldest, or it is dropped
(:308165-308203, :308347-308360).

## Sound classes

`gamedata/soundclasses/*.tdf` hold one class per unit, named by the
FBI key `soundcategory` (the FBI also carries `soundclass` with the
same value). Sections inside a class are actions (`select`, `move`,
`attack`, `guard`, `patrol`, `default`) and each entry is a wav name
with a weight when `prioritized = 1`, or equal weights otherwise
(:221394). Playing an action picks by weight with the C runtime
generator, not the simulation one (:221705). An action the class does
not have plays the first section in file order (:221615).

`soundclasses.tdf` holds the hit classes (`sword`, `arrow`, `cannon`,
`fire`, `hammer`, `fist`, `staff`, `rock`, `lightning`). Their sections
are materials (`default`, `flesh`, `armor`, `wood`, `scale`, `stone`)
listing `sound0..soundN`, picked uniformly. An unknown material plays
the first section (:221657-221665). Nine classes sit in the base
file, so a loader has to walk every top level section, not stop at
the first.

## Weapons

An FBI weapon block can carry `soundstart`, `soundhit`, `soundhitclass`,
`soundwater` and `soundtrigger` (:250159-250185, :250030).

Fire: `soundstart` plays positionally at priority 4 on each projectile
emission, and only when `soundtrigger` is set (:245977). No shipped
weapon carries either key, so the fire cues you hear come from the
attack scripts (below).

Impact (:244977, :245000-245014), priority 4 at the impact point:

- Landing on water with no unit struck plays `soundwater`, or nothing
  when the weapon has none. Water is a cell whose terrain height is
  below the side's water height.
- With `soundhitclass`, the material is the `bodytype` of the unit
  struck (`flesh`, `armor`, `wood`, `scale`, `stone`, read at :163138
  with default `default`), and bare ground uses no material, so the
  first section of the class plays.
- Without a class, `soundhit` plays directly.

The unit struck is whichever unit holds the cell the shot comes down
in (:245399-245435). An area-of-effect shot finds it the same way as
a direct shot, although its damage is the splash.

## Unit scripts

The scripts play sounds with the play-sound opcode, whose argument is
the category in the low three bits (:306851, :223761-223781). In the
shipped scripts 226 of 240 calls use category 4, twelve use category 1
and two use category 7. Category 7 is a flat play. Categories 0 to 6
are positional at the unit, at priority equal to the category, and
play only where the local player can see the unit. Categories 0 and 1
also require the unit to be selected and a free channel, never
stealing one.

Where the calls sit: `attack1..4` and `FireWeapon` carry the swing and
shot cues (`STSWISH3`, `CANNON1`, `ARCHER3`, `ARROW10`), `Dying`
carries the death cry (`ARAKNIGHDIE2`) behind a one in four roll in
every shipped script, and a few walk scripts stamp feet at category 1.

A unit that dies runs `Killed(severity, corpse, kind)` and then, when
the script exists, `Dying(kind)` (:227241, :227250-227256). `Dying` is
where the fall animation and the cry live. The engine keeps the unit
until both finish.

The script `rand(lo, hi)` draws from the simulation generator, a
Lehmer sequence with multiplier 16807 folded by the Schrage method,
seeded by xor with 0x66e29572 and forced odd (:306663-306673,
:254475-254490).

## Acknowledgements

Selecting a unit by click plays its class `select` action, unless
shift is held (:237940-237950). A box select voices the last unit it
took, again not with shift (:237800-237806). An order voices the first
selected unit that accepts it (:238647) with `attack`, `guard`,
`patrol`, `Move` or `select` by order kind and `default` for the rest
(:221247-221281). With the unit sounds option off every one of these
plays `default`.

## Alarms

When a unit of the local player is damaged by another player and is
not selected, the side's `underattack_sound` from `sidedata.tdf`
(`AlarmAra` and so on) plays flat, then not again for
`underattack_delay` seconds (thirty in the shipped data)
(:15218-15235, :221298-221325). A commander (FBI `commander = 1`)
plays `AlarmMon` instead, selected or not, fifteen seconds apart
(:221328-221340).

## Interface

Each widget in a `.gui` file names the wav it plays when used
(`menubutton.wav`, `ok.wav`, `cancel.wav`, and on the in-game sidebar
`attack.wav`, `move.wav`, `guard.wav`, `patrol.wav`, `stop.wav`). A
click plays it flat at volume 0x55, priority 4 (:332867).

Flat cues by event, all at 0x7f and priority 7:

- `addbuild` when a build order is queued or a build icon is clicked,
  `subbuild` when the queue shrinks (:39328-39330, :150087).
- `oktobuild` when a placement click lands, `notoktobuild` when it is
  refused (:243684-243688, :137366).
- `setfireorders` and `setmoveorders` for the standing order buttons,
  `immediateorders` for self destruct, `specialorders` for capture
  (:151700-151772).
- `CreateSquad` when a control group is assigned, `SelectSquad` when
  one is recalled (:122211-122241).
- `Load` and `unload`, positional at priority 4, when a transport takes
  or drops a unit (:234558, :234575).
- `Victory Condition` when the match ends (:240280).
- `MessageArrived` for a chat line from another player and `Ally` for
  an alliance change, multiplayer only (:205815, :195010).

The shipped archives carry wavs for `MenuButton`, `AlarmAra` and the
other kingdom alarms, `AlarmMon`, `Load` and `unload`, but none named
`addbuild`, `subbuild`, `oktobuild`, `notoktobuild`, `setfireorders`,
`setmoveorders`, `immediateorders`, `specialorders`, `CreateSquad`,
`SelectSquad` or `Victory Condition`. Those cues start in the original
and load nothing, so they are silent there as well. The engine keeps
the triggers so a modded install that adds the files hears them.

## Ambient features

Every ten frames the original walks the visible map cells inside the
viewport and, while a channel is free, gives each feature that has a
`SoundClass` a countdown seeded from `SoundDelay` and `SoundVariance`.
When it runs out the class plays flat at volume 0x40, priority 1
(:128619-128712). The shipped noise features are the `woodland`,
`wind`, `waves`, `town`, `spooky`, `seatown` and `jungle` classes in
`ambient.tdf`. The countdown lives in the emitter's own map cell
(:128676-128706), so bodies that fall or rot around it never reset
it. The engine keys it the same way, by the emitter's cell and
definition.
