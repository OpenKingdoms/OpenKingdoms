# Combat and weapons

This note covers how the original's units fight: what a unit carries,
veterancy, the weapon keys, aiming and firing, how a shot flies and
what it strikes, damage, how units and weapons take targets, orders
and stances, the attacks of flyers, mind control, healing and auras,
self destruct, and patrol and guard.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate
departures are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md),
cited by ID.

The original's simulation runs at 30 frames a second, and a frame in
this note is one of those. The FBI keys that give a rate a second are
divided by 30 for a frame. A world unit is a pixel.

## What a unit carries

Each unit keeps its current order and three weapon slots, each slot
with its own aim, reload and flags. It keeps its position as X, Y and
Z in 16.16 fixed point, and its heading. It points to the transport
that carries it and heads a list of the units it carries, each of
which points to the next. It points to its unit type and to its
player's record.

It keeps the frame before which it may not cloak again, and a mana
pool of its own that cloaking spends from instead of the player's
([fog.md](fog.md) has cloaking). It keeps its experience, a veteran
override that forces the top rank when set, its owner index, which is
its player and colour index and also gives the team colour, and its
kill count.

Its build fraction is what is still to build, 1.0 when it is placed
and 0.0 when it is complete. Its hit points carry an 8 bit fraction,
so heals of under one hit point add up. A cache holds its current and
previous health percentage and is refreshed every 30 frames. Its state
bits are activated, cloaked and actively building. Its flags are
visible to the local player, wants to cloak, cloak suppressed, hit
points changed, under construction and alive, and the top two bits of
the flag word hold the active weapon slot.

"Use Crusades Units" in the battle menu loads each unit from
`unitscb/` where the Crusades set has one (legacy:162511-162515). The
set ships in V3Rocket.hpi and gives the swordsman 3000 hit points in
place of 2500, among other changes.

## Veterancy

A unit type with the `noveteran` flag (legacy:163080) stays at level
zero. Otherwise a unit whose veteran override is set is at the top
rank, and any other unit's level is its experience divided by its
type's `experiencepoints`, which defaults to 666 (legacy:162918), held
to the rank cap of 10 (legacy:232934).

The veteran multiplier is 1 plus 0.1 for each level, or the override
as it stands when that is set (legacy:232971-232984). The rank cap and
the step of 0.1 are globals with console setters (legacy:38338,
legacy:38324). The rank shown to the player is the multiplier cut to a
whole number, and at least 1 (legacy:179332). Port 32 of the unit
script returns the raw level (legacy:223313), not the multiplier and
not the shown rank.

A kill is credited as the victim dies (legacy:227303-227328). The
killer's kill count goes up by one and its experience by the victim
type's `experiencepoints`. Two counters on the killer's player record
go up, and a loss counter on the victim's player. Nothing is credited
when the victim's owner index is the killer's own (legacy:227322), so
there is no credit for a unit of one's own and no team credit. The
mana bounty a kill pays, `mogriumbounty`, is in
[economy.md](economy.md).

A veteran hits harder and takes less. Its attack scale and its armour
scale are each multiplied by its veteran multiplier
(legacy:235826-235862, both at legacy:235859), so a level 5 swordsman
strikes for 150 percent and takes two thirds of each blow. The two
scales sit at +0xe0 and +0xe4 in the unit record. How they enter a
hit is under Damage and splash, and the auras that scale them again
are under Healing and auras.

A weapon's `ManaPerShot` is discounted by the veteran multiplier
(legacy:250539). A shot takes the weapon's `veteranmodel` in place of
its `model` when the weapon's `veteranlevel` is at or below the unit's
level (legacy:246620, legacy:246834, legacy:246902).
[2026-09-04-rendering-visuals.md](2026-09-04-rendering-visuals.md)
has the shipped weapons that carry the pair.

## Weapon keys

The original's weapons have none of Total Annihilation's `tolerance`,
`pitchtolerance`, `targetmoveerror`, `accuracy`, `energypershot` and
`metalpershot`. They have `AimTolerance`, `ManaPerShot` and
`dontleadtargets` instead.

The weapon record, by TDF key (legacy:249668-250434):

| Key | Meaning |
|---|---|
| `type`, `subtype` | The weapon's behaviour, below |
| damage table | A figure per damage category (legacy:250486), and `default` for the rest |
| `areaofeffect` | Size of the splash, a burst reaches half of it |
| `edgeeffectiveness` | Share of the damage at the splash's edge |
| `range` | Maximum range in px, at least 2 |
| `minrange` | Minimum range |
| `reloadtime` | Seconds in the file, kept in frames |
| `switchreloadtime` | Reload applied on switching to this slot |
| `turnrate` | Turret turn rate |
| `burst`, `burstrate` | Shots in a burst, and the gap between them |
| `sprayangle` | Spread of a burst |
| `randomdecay` | Read, meaning not established |
| `holdtime`, `weapontimer` | Kept on the record, set by no shipped file |
| `AimTolerance` | The aim gate, under Aiming and firing |
| `hoverattackdistance`, `hoverattackaltitude`, `airtoair` | Air attack geometry |
| `weaponvelocity` | Speed, with its count of substeps |
| `ManaPerShot` | Mana for each shot |
| damage type | Low nibble of the flag word, 1 normal, 2 fire, 3 explosion, 4 paralyzer (legacy:250385) |

The weapon's `type` picks its behaviour (legacy:249725-249983):
Melee, Ballistic, Ballistic with `subtype = Dropped`
(legacy:249737-249746), Line of Sight, Guided and Wandering
(legacy:249968-249979). The Line of Sight subtypes lightning, fire,
bluefire, dieselflame, mindcontrol (legacy:249801), turntostone and
turntofrozen have behaviours of their own. A weapon is melee when its
`type` is Melee and only then (legacy:249726). The goblin's Dead Guy
Bone, with a reach of 50, is one.

`weaponvelocity` is in px a second, the same unit as `range`. The
record keeps it in 1/65536 px a frame, the file's figure times
65536/30, with no other scaling (legacy:249987-249989). Each frame's
move is cut into as many substeps as it takes to keep each at 16 px or
less (legacy:250425-250433, legacy:250426-250433). arabow.fbi WEAPON1
has a range of 550 and a weaponvelocity of 530, about 1.04 s of flight
at full range. araking.fbi WEAPON1, a lightning, has a range of 250
and a weaponvelocity of 500. Shipped velocities run from 10 to 5000.

A Line of Sight weapon's `hweffect` is drawn for `emittime` frames,
read at legacy:247444 with a default of 30. araking's lightning has an
emittime of 15, half a second, and shipped values run from 10 to 55.
The weapon carries `innercolor`, `middlecolor` and `outercolor` RGB
triples. araking.fbi WEAPON1 sets them to white, 200 230 255 and
180 200 255, with `hweffect = lightning`.

The record also holds `soundstart` (legacy:250159-250166). It plays
at the shooter at each shot of a burst, at priority 4, when the
weapon's `soundtrigger` flag is set (legacy:245977-245979). No shipped
FBI sets `soundstart` or `soundtrigger`, so the sound of a shot comes
from the play-sound of its FireWeapon script (opcode 0x10072000).
[sound-triggers.md](sound-triggers.md) has the sound keys.

A weapon's `shadowgaf` and `shadowart` are read into a second
animation slot and drawn on the ground under the shot
(legacy:250152, legacy:246711). `smoketrail`, `smokedelay`,
`startsmoke`, `endsmoke` and `nimbus` add smoke puffs and a glow. An
explosion class may carry particle emitters in effects.tdf, a
`lightmap` tier and a screen shake by `shakemagnitude` and
`shakeduration`. A shot that bursts on water or lava plays the
weapon's `waterexplosionclass` or `lavaexplosionclass`
(legacy:250146).

## Aiming and firing

Each frame, for each weapon slot (legacy:245872-245923), the original
honours the weapon switching mode, counts the reload down, updates the
aim and the target (legacy:249255), checks that the aim has settled
(legacy:249328) and fires (legacy:249388). When the script gives that
slot's shot signal, the shot is made (legacy:249423), the unit's cloak
is held off for 600 frames ([fog.md](fog.md)), and a unit whose mana
cannot cover the shot drops to a cheaper weapon slot (legacy:233957).

With mana in hand and the reload at 0, the aim angles are worked out
(legacy:249211). The aim has settled when the heading error is within
`AimTolerance` and the pitch error within half of it
(legacy:249328-249384). A unit that turns its body onto what it shoots
then meets one more gate once its script's AimWeapon has answered. Its
body must face the target within the larger of `AimTolerance` and
0x200 (legacy:249365-249377). Only then does it fire. A 3 bit retry
count lives in the slot's flags. When the aim fails, AimWeapon is
asked again and the count is set back to 7 (legacy:249308). The fire
gate always waits for the aim (legacy:249328-249338).

There is no line of fire check. The fire gate looks at range, reload,
mana and facing and nothing else (legacy:249328-249420), so a unit
keeps shooting into a hill, and units neither hold fire nor move to
find a clear line. The engine departs here for a target a unit took on
for itself, which it lets go when its shots keep stopping short of it,
see D-025.

### Where a shot aims

A shot aims at its target's SweetSpot piece, the piece the unit's
script names for the purpose, or piece 0 when it names none
(legacy:186005-186013, legacy:234031-234042). 162 of the 217 shipped
scripts name one, and the rest are walls, lodestones and the like.

A shot at a moving unit leads it by the target's velocity times four
fifths of the flight time, where the flight time is the distance to
the sweet spot over the weapon's speed (legacy:234031-234060,
legacy:234043-234055, the multiply by 0xCCCC at legacy:234049). A
weapon with `dontleadtargets` aims at a frozen target position instead
of the live one (legacy:249211-249250). The engine leads only mind
control shots, see D-038.

A shot leaves with the unit's heading plus the slot's aim heading, the
slot's pitch and the weapon's speed (legacy:246602-246637).

### The script's signal

A weapon's FireWeapon script starts the attack animation and charges
the reload, and the shot or blow leaves when the script sets port 23
(legacy:223397-223400, legacy:245900-245904). The Aramon archer's
attack sets it after 1.32 s of sleeps, the swordsman's after 0.51 to
0.65 s. Port 21 calls the shot off.

A target lost during the draw takes the shot with it. The script hears
TargetCleared (legacy:233924-233940) and skips its release, the reload
stays spent, and a late port 23 releases nothing. The shot leaves at
the signal wherever the target stands, and a blow strikes only a
target still in reach (legacy:246456-246468). A spell's mana is
checked when the draw starts and paid when the shot leaves
(legacy:249341-249342, legacy:249460-249461), so a caster whose target
dies during the cast keeps it.

A shot at a point on the ground is drawn and let go the same way,
since the weapon holds a unit or a point alike and releases either on
port 23 (legacy:245894-245906, legacy:233903-233911). The Mage
Archer's arrow at the ground leaves about 2.1 s into its draw and the
Mage Builder's hammer about 0.77 s, as at a unit. The shot goes where
the order points when it leaves, and an order of another kind calls it
off.

### Reload

The reload is `reloadtime` plus a jitter drawn evenly between minus
and plus an accuracy term, from the original's 15 bit generator, whose
draws run to 0x8000 (legacy:249388). What sets the size of that term
is not established.

A script hears its longest reload once, through SetMaxReloadTime, in
milliseconds, the frames times 1000 over 30. It keeps twice that as
how long a pose holds after an aim (legacy:245659-245687). A script
that is never told keeps its own default of 5 seconds.

Musketeers, grenadiers and trebuchets hold AimWeapon while FireWeapon
plays the shot and the reload together, and the fire gate waits for
the aim. So a Musketeer fires every 7.06 s, the length of its script's
shot and reload, where its file says 6.2. The engine lets an aim that
never answers fire after two seconds, see D-042.

How `burst`, `burstrate` and `sprayangle` act inside each type's
launch (legacy:246400-249100) is not established.

### Which weapons fire

A unit without `weaponswitching` fires every weapon. The original
starts such a unit in the mode that lets all three slots fire, and one
with it in the slot `defaultweapon` names (legacy:226586-226598). A
weapon may take a target only in a slot the mode allows
(legacy:233882-233896). So the gods, the two bow towers, the Rolling
Tower, the Dirigible, the Aerial Juggernaut and the Iron Clad fire all
their weapons, each at a target of its own. Every shipped caster and
monarch, the Fallen Angel among them, carries `weaponswitching` and
fires only the weapon picked.

## How a shot flies

Every flying shot splits its speed into steps of 16 px or less
(legacy:250425-250433) and runs one test after each step, on the map
cell it has reached (legacy:245377-245476). The order is fixed.

1. Off the map, the shot is gone with no burst (legacy:245393-245396).
2. The unit standing in the cell stops it when that unit's owner is
   not the shot's owner (legacy:245419) and the shot lies inside the
   unit's body, which the paragraph after this list sets out. The
   shooter's own units, buildings and walls never stop its shots. An
   ally's do.
3. A flyer over the cell stops it when the shot's height is inside the
   flyer's model (legacy:245426-245437).
4. A weapon with `unitsonly` stops looking here
   (legacy:245440-245443).
5. A feature in the cell stops it when the shot is no higher than the
   cell's floor plus the feature's height (legacy:245444-245461). The
   floor is the lowest of the cell's four corners
   (legacy:224590-224622).
6. A shot above the ground and under the sea bursts on the water,
   unless the weapon is a `waterweapon` or the map sets
   `nosealeveltrigger` (legacy:245463-245468).
7. At or under the floor it bursts, or bounces with a quarter of its
   vertical speed for `groundbounce` (legacy:245470-245476).

A shot is inside a unit's body when its height lies between the unit's
base and the top of its model (legacy:236955-236958) and its place
lies inside the selection quad of the model's root piece, the
primitive the 3DO header names at +0xC, turned with the unit
(legacy:236974-237027, legacy:237007-237027). A unit whose root has no
such primitive is never struck in its cell. The quad is about 29 px
across for a swordsman. A shot bursts inside the quad of the unit it
strikes, so its target takes a splash weapon's whole hit, the full 817
of a Rictus fireball on a swordsman.

A feature's height is cut to a byte (legacy:127053). So a feature
authored 300 tall, as the largest trees and rocks are, stops shots at
44 over its floor, and one authored 200 at 200.

The ballistic shell runs the test on every step of its arc
(legacy:246642-246690). A straight shot, which is a Line of Sight
weapon with no special subtype or a Guided one, leaves the QueryWeapon
piece on a line to its aim point and keeps that line
(legacy:246851-246925, legacy:246927-247010). Lightning follows the
same straight line (legacy:247147-247160). A flame walks its whole
range through the test on its first frame to find where it stops
(legacy:247535-247575). The mind control, stone and frost rays are
straight shots.

Remote Effect spells and Wandering shots run no test on the way
(legacy:249842-249950, legacy:249099-249160). So the 17 Remote Effect
spells, Earthquake, Hail Shower, Firestorm, Ring of Fire, Tsunami and
the Wind and Fire Waves among them, land behind ridges and walls. So
does the Mind Mage's Individual Mind Control, which is `unitsonly`.
The Harpy's is a straight shot without it and stops on them. Wandering
shots pass over ridges and walls, bursting as they go.

A shot whose target dies keeps flying, and nothing in the original
ends it early. A shot lives its distance over its speed when
`weapontimer` is 0 (legacy:245980-245986). How a ballistic shot picks
its arc, the gravity it falls under and the art a shot shows are in
[2026-09-04-rendering-visuals.md](2026-09-04-rendering-visuals.md).

### Dropped shots

A Ballistic weapon with `subtype = Dropped` is a behaviour of its own
(legacy:249737-249746, legacy:249743), a shell that falls from its
carrier (legacy:246794). It leaves level and nose down, and its speed
across the ground is the run to its aim over the time its fall takes,
so it lands on its aim from wherever it was dropped
(legacy:246799-246829). Its weaponvelocity plays no part. That is 15
on the Dirigible and 10 on the Iron Beak, and the shipped dropped
weapons run from 10 to 45. A Dirigible's stone dropped from 200 px up
lands in just under two seconds.

## Damage and splash

A hit is the weapon's figure for the victim's damage category, times
the shooter's attack scale, divided by the victim's armour scale
(legacy:245324-245343). Each frame the two scales start again from
their base (legacy:235818-235836) and take the veteran multiplier and
any aura that reaches the unit, as under Veterancy and Healing and
auras.

A shot with an `areaofeffect` of 17 or more bursts where it stopped.
One under 17 hits the unit that stopped it, target or not, and nothing
else (legacy:245029-245031). An ally that stops a shot takes it like
anyone else. The engine departs here, see D-024. A shot that ends on a
feature, the sea or the ground does the harm of its burst and nothing
else.

A burst reaches half the `areaofeffect` (legacy:245089). Each unit is
measured from the burst to the nearest point of its model's box, the
box set unturned on the unit where it stands, in whole pixels
(legacy:245164-245209). A unit at 0 takes the whole hit, and one
further out but inside the radius takes
`edge + (1 - edge) * (d / r - 1)^2` of it, where edge is the weapon's
`edgeeffectiveness` (legacy:245213-245217). A catapult's ball, 1250
with an edge of 0.1 over an areaofeffect of 100, gives 845 at 10 px,
406 at 25, 125 at 49 and nothing from 50.

The blast (legacy:244963-245050) plays the weapon's hit sound, or its
water sound over the sea, and shakes the view when the weapon shakes.
Every unit whose model is within half the areaofeffect, but the unit
that fired, takes its share by the falloff. Then the scenery within
that reach takes the weapon's damage, unless the weapon is `unitsonly`
(legacy:245236-245302). Where the map lets shots through the sea, a
burst over the sea bed does nothing (legacy:244975-244984).

## Finding targets

Each frame the original walks round each side's units,
`maxunits / 30 + 1` of them a frame, so a side under that size has
every unit looked at every frame and a full one each unit once a second
(legacy:18985-18996). For a unit on fire at will that is not building,
each weapon left free with nothing to aim at searches on its own
(legacy:19247-19285, legacy:18920-18933). A dropped weapon never does,
and a `commandfire` one only for a computer player. No shipped weapon
sets `commandfire`.

A weapon's own search uses its `range` key and nothing else. A search
made for an order, an idle unit's among them, reaches the larger of
the unit's sight and its weapon's raw range (legacy:21113-21118). A
ranged weapon takes only what it already reaches. A blade takes
anything inside its range key and strikes it only in contact
(legacy:21141-21144). Patch 3 (V3Rocket.hpi) set every melee weapon's
range to 300, so a swordsman with a sight of 135 takes on anything
within 300, while its reach in a fight is unchanged.

The search takes nothing the weapon cannot hurt
(legacy:21165-21167). Every weapon passes over a unit its damage table
zeroes (legacy:15160), so a wolf, whose table gives a lodestone 0,
never takes one. A blade takes only a unit on the ground
(legacy:21164). Its reach test refuses one in the air
(legacy:246390-246403), and its attack ends when the target is in the
air (legacy:11383). Other weapons take a flyer unless their file says
`noairweapon`.

Units with `fireatwillrandom = 1` in their unit file (legacy:163097),
the archers, crossbowmen and a few others, draw their target at random
among the enemies in reach (legacy:21060-21251). Up to fifty
candidates are drawn with the simulation's generator. Each scores
`rand(m) / 2 + rand(m)`, where m is the largest int over the damage it
would take, plus a turn term for a unit that turns slower than 1000 a
frame. The score is ten times worse for one out of reach or
unfinished, and the least wins. The key changes only how the search
scores what it finds (legacy:21170). Any other unit takes the nearest.

A weapon keeps what it took while that stays a thing it can take. It
lets go when its target dies, cloaks, turns friendly or is held
already by a paralyzer, and not when the walk takes it out of reach
(legacy:19262-19284). Hold fire drops it (legacy:9820-9846).

The weapon fires whenever its target is in reach, its aim is done and
it is loaded. A unit that turns its body onto what it shoots fires
only with its body facing the target (legacy:249365-249377), and a
walker on its way does not turn for it (legacy:183434). So such a unit
fires on the move only at what lies ahead of it.

A unit on a move order hit by an enemy takes no fight of its own. That
is only for a unit standing by or on patrol. With fire on, each free
weapon that reaches the shooter takes it, unless it already holds
something it reaches (legacy:15187-15215). The same holds in a fight.
An attack puts its target on the weapon only while the target is in
reach (legacy:11137-11141). So a unit walking to a target out of reach
that is hit takes the shooter with its weapon if it reaches it, and
fires at it as it walks when its body allows. The attack goes on, and
the weapon goes back to the attack's target once that is in reach.

An order puts its target on the first weapon only (legacy:11141). The
others look for themselves as above, under any order, keep what they
took, answer a hit as a free weapon does, and fire when what they hold
is in reach. A structure's attack is the exception. It puts its
target on every weapon (legacy:9141-9158), so both bows of a tower
shoot what the tower was told to shoot.

The computer player's groups go by move orders and take their targets
by search (legacy:18487-18530).

## Orders and stances

A plain move order walks to its point and does nothing else. Its
handler looks for a target only when the order was given as a fight
move, or carries a leash of its own, and the second never while the
unit holds position (legacy:10354-10356). A plain move carries
neither, so the unit never stops, turns aside or chases. The order
does not take the weapons, which stay free to look for themselves, as
under every order that does not claim them (legacy:181003).

A fight move looks with the order's own search at each wait and turns
aside to fight (legacy:10359-10504, the MoveFight key at
legacy:151239-151244). The engine has no fight move.

The stance buttons set the fire and move orders together
(legacy:233216-233231). Offensive is fire at will and maneuver,
Defensive fire at will and hold position, Passive hold fire and hold
position. So on a move order Offensive and Defensive both fire at what
they pass and Passive does not. The move order part of a stance makes
no difference to a plain move.

`standingunitorder` in the unit file is the stance a unit starts with
(legacy:162926-162947). 1 is hold position, which the archers carry,
and 2 is maneuver, which the melee units carry. A mission's `o N` uses
the same numbers, 0 passive, 1 defensive and 2 offensive. A unit whose
file gives no `standingunitorder` starts offensive and moves by
`standingmoveorder`, which is 2, roam, when the file does not say
either (legacy:162930-162934). Roam has no leash (legacy:13733-13737),
so such a unit answers a shooter and takes a new target at any
distance. What the other values of `standingmoveorder` mean is not
established.

An idle unit on maneuver that finds a target gets two missions, a move
back to where it stands and the attack on top of it. The attack
carries `maneuverleashlength` and that point (legacy:181879-181947,
called from the standby at legacy:13621-13633). Both attack missions
end once the unit stands the leash or more from the point
(legacy:11020 for a ranged weapon, legacy:11380 for a blade), and the
move back then walks it there. On the way it takes on only what stands
inside the leash round that point, and the fight it takes keeps the
same point (legacy:10438-10443). A unit that roams gets the attack
with no leash and no move back. One holding position gets neither, and
only its weapons fire, at what they reach. A leash of 0 is no limit.
The engine departs at the leash, see D-040.

## Closing in and looking again

The attack mission asks the melee behaviour on every step whether the
target is in reach, and walks on while it is not
(legacy:246390-246428). An attack whose route gives out keeps closing.

### How a shot went

When a shot ends, the original adds up the harm it did to other
players' units and to its own player's (legacy:245317, legacy:245370,
legacy:15038-15060). More than twice as much to enemies sets one flag
on the shooter, 0x1000000 (legacy:15046-15048). Anything else sets a
wasted flag for that weapon, one of 0x200000, 0x400000 or 0x800000
(legacy:15050-15059). A mind control hit on a unit it may take counts
as harm whether the roll takes it or not (legacy:247761-247798).
Firing raises 0x10000, 0x20000 or 0x40000 on the shooter by weapon
kind (legacy:249395-249414).

### A ranged unit's level

The ranged attack handler keeps a level from 0 to 5 in its mission
(legacy:11103-11131). A good shot may bring it down: on a draw of 2 it
loses up to its own value. A wasted shot with the target in reach
takes up to four draws to raise it, of 2, 3, 3 and 2, never past 4.
The level then says what the unit does (legacy:11198-11311).

| Level | What it does |
|---|---|
| 0, 1 | Walks only if the target is out of reach |
| 2 | Walks to within a range of about two thirds of its reach |
| 3 | Walks to within about an eighth of its reach |
| 4 | Goes to a point round the target, a sixth of a turn either side of where it stands, inside its reach, and the level becomes 5 |
| 5 | Waits for that walk to end, and the level becomes 4 |

The ranges are drawn, and the table gives the middle of each draw.
Levels 2 and 3 climb when the unit already stands that close, on a
draw of 3 that is not 0. A shot that ends during a walk is not judged
until the walk is over (legacy:11214), and a new order starts the
level over (legacy:11091-11093). The weapon stays on its target
through all of it and fires when the walk happens to face it. A unit
holding position with a target it took on for itself does not run
this handler, so it does not walk. A plain miss counts as wasted. The
engine counts only a shot that stops short and lets a walker give up a
target it took on for itself after six of them, see D-025.

### Looking again

The two attack handlers each have their own rule for looking at other
targets, and only while the target is out of reach. A ranged unit with
its target in reach and its shots landing does not look at all.

A ranged unit with its target out of reach leaves level 0 on a draw of
2 (legacy:11109-11113). At each wasted shot or wait it searches unless
a draw of its level plus one comes up 0 (legacy:11189-11195).

The blade's handler waits between its checks. With the target more
than a footprint and a half away the wait is
`rand(5) + rand(5) + 4` frames, 4 to 12 (legacy:11485-11509, the
dispatcher at legacy:182148-182175). The wait with the target nearer
is not established. At the end of each wait a unit that is not holding
position (legacy:11517), is on fire at will (legacy:182080) and is in
an attack it took on for itself, on maneuver or as return fire, runs
the standard target search again (legacy:182079-182083, then
legacy:21060) on half its checks. It draws rand(2) before the search
and searches only on 0 (legacy:11517). Only when the search finds a
different target inside its leash (legacy:11520) does it draw
rand(10), and it takes that target only on 0 (legacy:11519). So about
1 check in 20 takes what the search finds. A random picker among three
targets switches on about 1 check in 30, and one that takes the
nearest keeps its target unless another is nearer. The search runs
only when a check on the weapon's range passes (legacy:11516,
legacy:11408). The handler returns at once for a unit with no mover or
with `canfly` (legacy:11351-11355), so towers and flyers never look
again this way, even the dragons that are born on maneuver.

An attack order is never searched again.

## Flyers in a fight

A flyer on the ground does not fire. The standby turns a flyer's
weapons off (legacy:24543), and every flyer attack takes off first
(legacy:25786).

A flyer's attack on something on the ground takes one of two
missions, chosen on `hoverattack` alone (legacy:28524-28545). One with
`hoverattack` holds a point round its target. One without it attacks
in runs (legacy:28491-29118). A target that can fly, or a weapon in
hand with the `dropped` key, sends both elsewhere (legacy:28524-28545,
legacy:30040-30052). The `dropped` key is a key of its own, not
`subtype = Dropped` (legacy:250054-250055, tested at legacy:30051),
and no shipped weapon sets it.

Every VTOL mission sends a flyer near the map's edge 800 px back in
(legacy:30054-30117). The engine keeps the points a flyer makes for
inside the map instead. A target that fails a reach test on the unit's
path is handed to a formation move, which steps out of crowds by the
common rule (legacy:30139, legacy:30467-30502,
legacy:235678-235762).

### Hover attack

A unit reads `hoverattack` into a flag, and `hoverattackdistance` and
`hoverattackaltitude`, the last defaulting to `cruisealt`
(legacy:163005-163011). A weapon reads its own `hoverattackdistance`
and `hoverattackaltitude`, both 0 when unset (legacy:250022-250024).
The attack takes the value of the weapon in hand and falls back to the
unit's when that is 0 (legacy:30150-30163). No shipped weapon sets
either key. In the base game the unit keys are on aradrag, tardrag,
verdrag, zondrag, zondrake, zonharp, zonhunt, zonflies, tarbeak,
tarknigh, tarpries, tarprie2 and tarship, and the Harpy holds 200 px
off its target (legacy:163005-163007). The Iron Beak, the dragons and
the Harpy hover.

The attack is a mission of six stages, run once a frame. The mission
runner wakes a stage when its delay is up or when one of the events it
waits on comes in (legacy:182119-182192).

- Stage 0 runs BeginFlight, counts no step outs yet and takes the
  target's position (legacy:30164-30184).
- Stage 1 turns the weapons off. Within the hover distance plus 160 px
  of the target it goes straight on to stage 2. Otherwise it flies at
  the target, to within the hover distance plus 144, and looks again
  in 15 frames or when that move ends (legacy:30185-30222).
- Stage 2 sets the weapon in hand on the target, or on the ground
  point (legacy:30223-30253). From here the weapons fire on their own
  as the flyer moves.
- Stage 3 picks the flyer's point. The bearing is the one from the
  target to the flyer, one time in a hundred turned by rand(2048) of a
  65536 turn either way. The distance is the flyer's own while that is
  within 16 px of the hover distance, one time in a hundred redrawn
  all the same, else the hover distance plus rand(8) - 3. The flyer
  flies to that point off the target, at its hover height, and stage 4
  waits 15 + rand(15) frames (legacy:30254-30378).
- Stage 4 wakes when the delay is up, when the flyer fires, when a
  shot of its lands without hurting the other side, or when it reaches
  its point. With a crowd score under 5 it goes back to stage 3 a
  frame later, except that after such a shot, one time in two, with a
  unit target that stands still, it goes to stage 5
  (legacy:30616-30635).
- Stage 5 draws a bearing at random and flies to the hover distance
  plus rand(8) - 3 along it, and stage 3 runs again once it gets there
  (legacy:30381-30449).

A flyer holding its point reaches it again each frame it is picked, so
it picks again every other frame, and the one in a hundred turns walk
it slowly round the target. A flyer whose shot misses a target that
stands still, is stopped short or does it no harm flies round it to a
bearing drawn at random one time in two.

At stage 4 a flyer whose crowd score is 5 or more counts a step out.
One that took its own target may, one time in two, turn on another
enemy and start over. Otherwise it steps out
`(rand(10) + rand(10) + rand(10) + footprint) * 16` px, 32 to 464 px
for a Harpy, against 32 to 256 for the other missions' step out. The
attack then ends if `rand(5) + 1 + rand(5)` is under the count, else it
goes back to stage 1 a frame later (legacy:30636-30705). The first
step out never ends it, and the chance grows to 15 in 25 by the sixth.

A move ending raises 0x100 on the mission (legacy:191274). Stage 3
waits on 0xe70768, and stage 5 and stage 1 on 0x768, which takes in
the move's end and leaves out shots. So a shot that hurts the enemy
never wakes stage 4, and one that hits the ground, is stopped by the
shooter's own side or does no harm does. The shot events are under
How a shot went.

The move to the flyer's point names the target only for the flyer to
face, keeps the point where it was set (legacy:190458-190478,
legacy:190495-190496) and counts it reached within 4 px. Within 320 px
of its point the flyer turns onto its target at its turnrate, and
further out it faces the way it flies (legacy:241346-241351). A flyer
turned onto its target that backs away settles at a creep of
`(2 brakerate + acceleration) / (2 - acceleration / maxvelocity)` px a
frame (legacy:183961-184057), 0.42 px a frame for a Sky Knight, so a
walker that comes at it passes beneath it rather than pushing it back.
The weapon in hand fires whenever it is ready, its aim is done, the
target is in reach and the flyer's body faces it as the body gate asks
(legacy:249365-249377), on the move as well as at its point. The
flyer climbs to its `hoverattackaltitude` while it holds points.

Harpies holding 200 px off a target are clear of a Water Ball cast on
it, which reaches 50 px, but a Water Blast reaches 250 px and still
takes the whole ring of them. The engine departs from the hover attack
where it stops closing in by the common rule, see D-039, and holds a
flyer's fire while it climbs, see D-026.

### Attack runs

The run goes round these stages.

- It takes off, turns its weapons off and sets them on the target or
  the ground point, after which they fire on their own whenever they
  reach (legacy:28642-28684).
- It flies at the target's point. The move ends within half its
  `attackrunlength`, or half its weapon's range when it has none, and
  every 15 frames the flyer makes again for where the target is now,
  or steps out `(rand(10) + rand(10) + rand(10) + footprintx) * 16` px
  when its crowd score is 5 or more (legacy:28685-28716,
  legacy:29039-29117).
- It flies on past the target to a point the run length and
  128 + rand(128) px beyond it, or three times the weapon's range when
  it has no run length, until within 128 + rand(128) px of it. On its
  first pass, a flyer still further than half its range from the
  target picks that point again 15 frames later (legacy:28717-28790).
- It turns a quarter turn to one side at random and flies the weapon's
  range that way, or rand(128) plus half the run length, until within
  64 + rand(128) px (legacy:28791-28853).
- It comes round at the target again (legacy:28854-28924).

The armed flyers without `hoverattack` are the Dirigible, the Gryphon,
the Fallen Angel, the Aerial Juggernaut and the Wrath of Tammuz. The
Dirigible, the Gryphon and the Wrath of Tammuz, whose Hurricane
reaches 500 px, have no run length and fly runs three times their
range long. The Fallen Angel and the Aerial Juggernaut carry run
lengths of 400 and 200. The flyer mover keeps its speed and swings
round, and a Dirigible turns 100 a frame. The engine departs from the
run in places, see D-049.

## Wandering shots

Four weapons in the shipped data are `type = Wandering`: the Weather
Witch's Tornado (TARWITCH), the Fire Vortex (TARGOD), the Water Vortex
(VERGOD) and the Hurricane (ZONGOD).

A Wandering weapon has its own behaviour (legacy:249968-249979). It
reads `wanderstartart`, `wanderloopart` and `wanderendart`, three
animations from the anims folder, with the loop art set to repeat and
the other two to play once. It reads `duration` and `variationtime`
in seconds and keeps them in frames, times 30, and `maxvariation` as a
whole number (legacy:249000-249044). It keeps `weaponvelocity` and its
substeps as every weapon does (legacy:249987-249989,
legacy:250426-250433). `builduptime` and `decaytime` are in the files
but nothing reads them.

The shipped numbers, as V3Rocket.hpi gives them over the base game:

| Weapon | Unit | weaponvelocity | duration | variationtime | maxvariation | areaofeffect | damage |
|---|---|---|---|---|---|---|---|
| Tornado | TARWITCH | 45 | 9 | 2 | 5 | 30 | 50 |
| Fire Vortex | TARGOD | 80 | 8 | 2 | 8 | 180 | 12500 |
| Water Vortex | VERGOD | 45 | 9 | 1.5 | 4 | 180 | 15000 |
| Hurricane | ZONGOD | 45 | 6 | 0.5 | 2 | 240 | 7000 |

The base game's gods carry smaller vortexes, 45 px a second with an
areaofeffect of 30. Every one of the twelve animations holds each
picture for two frames.

When the weapon fires, the shot seeds a stream of its own from the
frame number, the frame's high byte in the top of the seed and its low
byte in the second (legacy:249049-249055). It takes the way from its
caster to the point it was aimed at as a unit vector and sets out
32 px along it, on the ground there (legacy:249059-249094). Its
velocity is weaponvelocity along that way. Two spreads are kept for
its turns, `maxvariation` times the south part of the way for the east
velocity, and times the east part for the south one. So a shot cast
due east drifts only north and south, and one cast on a diagonal both
ways. It starts on the next frame, or later by `nimbustime` when the
weapon has `nimbus` and the caster's side has nimbus art. No shipped
weapon has a nimbustime. When a `nimbus` weapon fires, the side's
nimbus art plays on the caster.

Each frame the shot does one of four things (legacy:249099-249174).

Before its first frame it waits on its caster, and if the caster is
dead or dying by then the shot is gone with it (legacy:249103-249111).

On its first frame it shows its start art. While that plays it stays
where it is and hurts nothing. A picture holds the frames its file
gives it, then the next follows, and the start and end art end after
their last picture (legacy:255756-255795).

When the start art ends its run begins (legacy:248966-248981). The
loop art shows, the run's length and the frames to its first turn
count from that frame, and it turns. A turn is the velocity it set out
with plus a draw from each spread, centred, east first. The draw is
the stream's next number scaled to twice the spread, less the spread,
in px a frame (legacy:248972-248980). The stream is the minimal
standard generator, kept unsigned (legacy:248985-248996).

On each frame of the run it moves by its velocity once for each
substep, which with more than one substep moves it by their square,
and no shipped weapon has more than one (legacy:249141-249147). When
the frames to its turn run out it turns again, and the next turn is
`variationtime` later (legacy:249148-249156). Then it bursts where it
is, the same blast a shot makes when it lands, called with no unit
struck and without its explosion art (legacy:249158, the blast at
legacy:244963-245050). None of the four is `unitsonly`, so the scenery
in reach takes the damage too (legacy:245236-245302). So the Tornado
deals up to 50 on each of 270 frames to whatever it passes, and fells
the trees in its way.

When the run's frames are spent it shows its end art and drifts on
with its last velocity, hurting nothing, until that art ends, and is
gone. Nothing else ends it. It never meets the ground, a ridge or a
unit on its way, and it runs on after its caster falls.

Under the remastered battlefield rules a burst also breaks rocks and
whatever else a blast breaks there, see D-036.

## Mind control

Capture is a weapon, and an attack order is how a player uses it.

The Zhon Harpy's file, `units/zonharp.fbi`, carries `cancapture = 1`,
its own pool of `maxmana = 1000` filling at `manarechargerate = 10` a
second, and one weapon named Individual Mind Control. That weapon is
`type = line of sight` with `subtype = mindcontrol`. It costs
`manapershot = 300`, reloads in 1.5 seconds, reaches 300 and flies
at 169. Its damage table gives `default = 1` and zero against airship,
dragon, factory, fort, god, lodestone, monarch and naval. The same
subtype is on the Taros mind mage, once as a single shot and once as
an area spell, and on one campaign character.

Sixteen units carry `cantbecaptured = 1`. They are the four dragons,
the four gods, `arawar`, `verflag`, `verharp`, `verman`, `verscout`,
`vertrans`, `vertre` and `zonkrak`. No file carries a rate, a chance
or a cost for capture.

A mindcontrol weapon gets a behaviour of its own when it is read
(legacy:249801). When its shot reaches a unit, the behaviour's hit
routine runs in place of the damage routine (legacy:245361), so a mind
control hit never wounds, whether or not it takes the unit. The damage
table is used only to decide what the weapon may be aimed at
(legacy:15160).

The hit routine (legacy:247761-247798) does nothing at all to a unit
that

- already belongs to the shot's owner (legacy:247769)
- is a structure (legacy:247772)
- is a commander, which is to say a monarch (legacy:247776)
- is still being built (legacy:247779)
- is aboard a transport (legacy:247782)
- has `cantbecaptured` on its type (legacy:247785)

For any other unit it rolls a number from 0 to 99 and compares it with
a threshold from the victim's veteran rank, `(rank + 16) * 5` held to
99 (legacy:247788-247793). A roll under the threshold takes the unit.

| Victim's rank | Chance |
|---|---|
| 0 | 80 in 100 |
| 1 | 85 in 100 |
| 2 | 90 in 100 |
| 3 | 95 in 100 |
| 4 and up | 99 in 100 |

A veteran is easier to take than a recruit. The victim's health plays
no part. For an area weapon the threshold is multiplied by the same
falloff the splash gives damage (legacy:245217-245224). Turn to stone
and turn to frozen use the same roll (legacy:247865). The engine does
not simulate those two.

A unit that is taken is handed over by the routine the give units
action and the mission script's Capture command also use
(legacy:228891, called from legacy:155792, legacy:178632 and
legacy:247794). It does not relabel the unit. It makes a new unit of
the same type for the new owner on the same spot (legacy:228975),
copies across the health and the build progress
(legacy:228988-228989), the facing and the on or off state, and then
removes the old unit with a cause of death that the death handler
skips the Killed script for (legacy:228998, legacy:227130). So there
is no death animation and no body. Everything else about the unit
starts again. Its kills, its experience and rank, its orders, its
stance, its reload and its own mana are those of a unit just made. If
the new unit cannot be made, the old one stays where it is
(legacy:228975).

The old unit goes through the ordinary kill (legacy:228985), which
kills whatever it carries (legacy:227218-227234). A loaded transport
can be taken, since the hit routine asks only whether the victim is
itself a rider (legacy:247782), so its riders die and it comes over
empty. The engine departs here in four ways: it does not copy the on
or off state, a flyer taken in the air comes over landed, the riders
of a taken transport step out, and its give units action changes the
owner in place, so a gift keeps its kills and rank.

When choosing a target and again before firing, a mind control weapon
passes over a unit with `cantbecaptured` and a unit still being built
(legacy:15151, legacy:19070, legacy:21157), and like every weapon it
passes over a unit its damage table zeroes (legacy:15160). So a Harpy
never spends a shot on a dragon, a god, a monarch, a ship or a
building.

The cost is the weapon's `manapershot` and nothing else. Each shot
takes 300 from the Harpy's own pool whether or not the roll succeeds.
A full pool is three shots, and after that the refill allows one shot
every thirty seconds.

The original has a CAPTURE button and a hotkey for units with
`cancapture` (legacy:150652, legacy:151122, legacy:151767). None of
the five in game gui files that ship has a widget by that name, so the
button never appears, and the capture cursor is loaded
(legacy:161432) and never shown. The attack cursor and the attack
order are the whole path.

The original draws its roll from the C library generator, which its
machines do not share, since it is not a lockstep game. The engine
draws it from its seeded simulation generator, so every machine
agrees.

### The shot's flight

There is no cast time. The launch starts the shot on the next frame
(legacy:246909). A `nimbus` weapon waits `nimbustime` times 30 frames
more (legacy:246910-246915, legacy:250013-250015), and no shipped file
sets `nimbustime`, so the Harpy never waits.

- The shot leaves on a straight line for the target's sweet spot plus
  the target's velocity times four fifths of the flight time
  (legacy:234031-234060, the multiply by 0xCCCC at legacy:234049). The
  flight time is the distance from the caster to the sweet spot over
  the weapon's speed.
- It flies at `weaponvelocity` along that line
  (legacy:246885-246892). A Harpy 200 px up firing at a unit 220 px
  away crosses the ground at about three quarters of 169 px a second.
- Each frame the shot is freed if its caster is dead or dying
  (legacy:247710-247713), after that frame's step. Every step runs the
  cell test (legacy:247736). The shot strikes a unit of another owner
  holding its 16 px cell only when its height is within that unit's
  model span and its place is inside the selection quad of the unit's
  root piece, turned with the unit (legacy:236974-237027), or a flyer
  over the cell whose span holds its height (legacy:245426-245437).
  There is no reach around the target.
- It lives its range over its speed across the ground and one frame
  more (legacy:246919-246922), then vanishes with no effect.

So a still recruit is struck by every shot and taken four times in
five, and a veteran of rank 4 or more 99 times in a hundred. Leading
by four fifths, the shot lands where a unit walking steadily across
its line stood a fifth of the flight earlier. For a swordsman at 33 px
a second and a flight of 1.7 seconds that is 11 px behind it, inside
its quad. A faster unit, or one that turns or changes pace while the
shot is in the air, can be missed, and a recruit keeps its side on one
roll in five. The engine flies mind control shots this way and every
other shot as D-038 sets out.

## Healing and auras

A unit with a non-zero `healtime` that is alive, built, not paralyzed
and below its full hit points heals itself every 8 frames with no mana
charged, so it costs nothing (legacy:236280-236287). `healtime` is
build work a second, not hit points a second. The heal divides it by
the unit's `buildtime` and multiplies by its `maxdamage`, so a unit
mends `healtime / buildtime` of its maximum health a second, and one
left alone mends fully in `buildtime / healtime` seconds. That ratio
is a round designed number across the shipped files. All five
monarchs come to 240 seconds, and so does an ordinary swordsman. A
castle takes 480, a wall 156, a dragon 900.

A builder's repair gives `workertime` over 30 a frame against the
target's `buildtime`, with mana charged, at legacy:32674 and
legacy:13512, so it costs mana and is held back by the treasury's
supply fraction (Repair and The treasury in
[economy.md](economy.md)). It does not read `healtime`.

There are no heal weapons. The special weapon types are mindcontrol,
turntostone, lightning, fire and paralyzer.

The health percentage cache refreshes every 30 frames
(legacy:236256). Port 4 of the unit script returns the live value, hit
points times 100 over `maxdamage` (legacy:223212).

### Auras

A unit file may carry three sections that the original reads with the
rest of the unit (legacy:163515-163527): `[AdjustArmor]`,
`[AdjustAttack]` and `[AdjustJoy]`. Each holds `Adjustment` (a float,
1 when absent), `Radius` (px, 0 when absent), `EdgeEffectiveness` (a
float, 0 when absent) and `AffectsEnemy` (legacy:231765-231795). A
radius of 0 is no aura. The shipped units that carry them:

| Unit | Section | Adjustment | Radius | Edge |
|---|---|---|---|---|
| Acolyte, Acolyte of Anu (ARAPRIES, ARAPRIE2) | AdjustJoy | 250 | 200 | 0.6 |
| Priest of Lihr (VERLIHR) | AdjustJoy | 250 | 200 | 0.6 |
| Pillar of Light (VERPILL) | AdjustJoy | 75 | 550 | 0.4 |
| Sacred Fire (ZONFIRE) | AdjustJoy | 45 | 90 | 1 |
| Titan (ARASMITH) | AdjustArmor | 1.4 | 200 | 0.6 |
| Shaman (ZONSHAM) | AdjustArmor and AdjustAttack | 1.2 | 200 | 0.1 |

None of them sets `AffectsEnemy`.

Every frame each unit's attack and armour scales start again from
their base (legacy:235818-235836). Then each live, complete unit
placed on the map scales the units round it (legacy:235838-235890).
The search takes every unit, and every unit carried by one, whose
position lies within the radius of the caster's
(legacy:220374-220459). The caster itself is left out, and so is any
unit on the other side, or with `AffectsEnemy` any unit on its own
side or an ally's (legacy:237117-237129). Who is an ally comes from
the caster's player's own alliance table. The engine uses its own rule
for allies here.

How strongly an aura reaches a unit `d` px away is
`edge + (1 - edge) * d / radius` (legacy:237131-237139), the edge
effectiveness at the centre rising to the whole at the rim.

AdjustArmor multiplies the unit's armour scale, and AdjustAttack its
attack scale, by `(adjustment - 1) * reach + 1`, every frame
(legacy:237112-237188). Two Shamans multiply. A Shaman scales by 1.11
at half its radius, so a bolt of 40 hits for 44 from a unit it scales
there and for 36 on one.

AdjustJoy runs once every 30 frames (legacy:235816,
legacy:235862-235882). A first pass counts every live, complete unit
it reaches. The second gives each `adjustment * reach / count` of
build work (legacy:237192-237228). That is the repair applicator.
Build work over the unit's buildtime is the share of its whole health
it gets back, paid by the caster's player at the unit's buildcost from
what the treasury gives. Nothing is paid for a unit already whole
(legacy:39540-39580). The amount asked is paid even when the heal is
more than the unit is missing, and a whole unit still counts toward
the split.

A swordsman is 99 seconds of build work for 285 mana, so an Acolyte
60 px away gives it 180 work a second, nearly two whole swordsmen of
healing, and asks 518 mana for it. A Monarch is 10 seconds of build
work for 100000 mana, so 20 work a second asks 200000 mana, and a
damaged monarch beside a healer takes the whole treasury every second
while it stands there. Builders then wait for mana. A heal draws at
the builders' share but never sets it, since the share counts busy
builders alone (legacy:235952-235977).

The auras do not depend on the unit being active. The Sacred Fire is
`onoffable`, and its Activate and Deactivate only start and stop its
flames, so it heals whether on or off. The Pillar of Light has no
`onoffable` in its file and so no on and off buttons. The ACTIVATE and
DEACTIVATE orders reach every onoffable unit (legacy:150430-150436,
legacy:151449-151470).

A unit file may also name the aura whose ring the selection shows,
`showrangeattrib` with `Joy`, `armor` or `attack`
(legacy:163420-163494). No shipped unit sets it.

## Self destruct

Keys.TDF binds both CTRL_D and CTRLSHIFT_D to SelfDestruct. The
original looks at every selected unit of the local player. When any of
them already counts down, those stop and nothing else happens.
Otherwise every one of them starts, and the message list says "Leaving
Your Command", the unitmissions table's text for SELF DESTRUCT ENGAGED
(legacy:122248-122294). Nothing on the way asks what kind of unit it
is, so a lodestone, a castle or a wall goes the same way as a soldier.
The engine asks first, see D-041.

The count is the unit file's `selfdestructcountdown` in seconds, 2
when the file has none, and then 0 to 14 frames more at random, drawn
when the seconds run out (legacy:163089-163095, legacy:9056-9088). No
file in the base game, Iron Plague or the patches has the key, so
every unit counts two seconds and a little. With a countdown of 0 the
unit goes at once. The engine draws the extra frames when the count
starts.

The order sits on a list of its own beside the unit's other orders,
and giving it clears none of them, so a unit goes on with what it was
doing while it counts (legacy:180727-180741, legacy:181669-181683).
Only Ctrl+D again, or the unit's death, ends the count.

The end is a hit of 30000 the unit deals itself with death type 5
(legacy:9086). That type runs no Killed script, so the unit leaves no
body and throws no pieces, and its Dying script runs with 5
(legacy:227124-227127, legacy:227250-227256). A lodestone's Dying
fades it out white. Its player counts the loss and nobody the kill
(legacy:227331-227338). A unit file can name a `[SELFDESTRUCTAS]`
weapon to burst then, and none in the shipped data does.

The line the message list gives a fallen player is in
[gameflow.md](gameflow.md).

## Patrol and guard

A unit type may carry `canguard` and `canpatrol` (legacy:163033). Its
standing orders are read into six bits of the type
(legacy:162926-162946).

A patrol is a standing order, order type 9 with a repeat count of -1,
driven by the waypoint driver at legacy:11565. It fights on the way
and then takes up its route again. A patrol a mission gives its units
is a patrol too, over all its waypoints. A leg to a point the unit
cannot stand on ends where its route ends, and the patrol takes its
next leg (legacy:11565). On a waypoint it cannot reach the driver sets
an unreachable flag. A draw of 1 in 3 then ends the order outright,
and the flag clears again with a chance of 1 in 15 on each attempt.

Between its legs a patrol looks with the standby search and takes what
it finds by the same two missions as an idle unit on maneuver,
whatever the stance, so a unit holding position on patrol closes in
too. Its point is where it left its route (legacy:10843-10851). A
passive unit does not search. With nothing to fight, a builder on
patrol looks inside its sight for a damaged ally standing still on the
ground, or a frame of its own side, takes one of them at random and
mends or helps build it, then goes on to the point it was making for
(legacy:10861-10930, legacy:14708-14722, legacy:233526-233571). A
computer player's builder takes only a finished building.

The wander step (legacy:13799-13884) takes a random heading at a
radius of footprintX times 16 plus a radius the order carries, round
the order's anchor. It gives up by comparing 3 times rand(4) with a
retry count in the order record that goes up on every attempt.

A flyer on patrol flies legs that end within 128 px of the point each
makes for (legacy:26275), so it never brakes onto a point. Its legs go
round the order point at its weapon's range plus 160 px. The engine
flies a flyer's patrol point to point, see D-049.

Guard (legacy:9604-9700) mirrors the guarded unit's order chain. Move
orders are given again as order type 2 and patrol as type 9, with the
repeat set to -1 when the guarded unit's order repeats. With nothing
to mirror the guard falls back to a plain follow. Area guard
(legacy:12837-12927) is a box along the map's axes held in the order
record, scanned on a 16 px snap, as order type 0xc.
