# Corpses, the sweep, raising the dead, and rot (2026-09-10)

What the original does when a unit dies, what lies on the ground
afterwards, and what can be done with it. Line anchors are `:NNNNN`
into the legacy reference.

## The data

A unit's FBI names its body in `corpse`, plus `corpseadjustx` and
`corpseadjustz`, whole cells that shift the body's footprint origin
(:163152-163159). Only the big keeps and castles use the adjust keys,
because their wreck is not the same shape as the building.

The body is a feature definition under `features/corpses/`. It carries
`object`, a 3DO model, instead of the `filename` plus `seqname` sprite pair
that map scenery uses. The loader reads `object` first and only reaches the
sprite keys when it is absent (:127098-127136). Alongside the usual
`reclaimable`, `blocking` and `damage` a corpse can carry `decomposetime`,
`resurrectable`, `animatable` and `featuredead` (:127369-127386, :127524).

Two data facts shape everything below. Mobile units' corpses carry
`resurrectable = 1`, `animatable = 1`, `blocking = 0` and a
`decomposetime` of 30 to 60. Buildings' wrecks carry `blocking = 1` and
none of those three keys: a wreck never rots and cannot be raised. No
corpse file carries `energy`, so sweeping a body pays nothing back.

## Death and the corpse type

The moment a unit dies the engine invokes its `Killed(severity, corpsetype)`
script synchronously, running it to its first sleep, and reads the second
argument straight back (:227142, :306199-306207). The script decides what
is left: 1 for the unit's own corpse, 2 for the wreck that corpse decays to
(one step down `featuredead`), 0 for nothing. The value starts at 0, so a
script that never writes it leaves no body, and a unit with no script leaves
none either (:227113). A frame still under construction never leaves a body
(:227160). Only the low nibble travels in the death packet (:227165).
Shipped scripts write the value at the top of `Killed`, before any sleep;
the monarchs write 0.

The body goes down at the destroy step, which runs when the death script
finishes, not when the hit points reach zero (:227267, :227350). It is
placed with its footprint origin at the unit's own origin cell plus the
adjust keys (:227423), keeping the unit's exact position, facing and team
colour (:128220-128232, :227429). Placement clears whatever features already
stand in the footprint, and an indestructible one refuses the placement
(:128173-128185, :128843). A body that lands below sea level sinks instead
(:227440-227450). We have no water yet, so that branch is not built.

## Rot

The feature tick runs once per 30 Hz frame with no gating and counts each
placed instance's `decomposetime` down by one (:128400-128402). It is a
frame count, not seconds: `decomposetime = 60` is two seconds. Sweeping and
raising both refresh the countdown every step they work (:32394, :13143).

When the countdown reaches zero the body loses its reclaimable,
resurrectable and animatable bits and starts sinking by an eighth of a
world unit per frame. After 60 frames the record is removed
(:128403-128414). Every cursor check reads the instance bits as well as the
definition's, so a sinking body takes no orders (:129476-129480,
:129501-129505).

We run at 60 Hz, so a corpse counts `decomposetime × 2` ticks and sinks for
120.

## The sweep click

The sweep order resolves onto the map cell and each selected unit makes its
own choice, in this order (:187127-187198): a unit with `canresurrect` over a
resurrectable body gets the raise order, a unit with `cananimate` over an
animatable feature gets the same order in animate mode, anyone else with
`canreclaim` over a reclaimable feature gets the sweep. The whole branch is
gated on `canreclaim` (:187127).

## Raising

The ground raise mission (:12931-13210) walks to the body, turns to face it,
takes the build pose and works. Every step re-validates the feature and
fails with the repair chatter if it is gone or rotted (:13065).

What comes back is the unit whose name is the feature's name up to its first
underscore, so `arasword_dead` brings back `ARASWORD`; an animation makes the
raiser's `animatetype` instead (:13061-13072).

The work owed is `buildtime × 0.3 ÷ (workertime ÷ 30)` in 16.16 frame units
for a resurrection and `buildtime ÷ (workertime ÷ 30)` for an animation,
with the three constants taken from the original
(:13076-13078). Each frame subtracts the player's economy supply fraction
times 65536, the same figure the construction tick scales by (:13087,
:39469), so a raise at full supply takes `buildtime × 9 ÷ workertime` frames:
Elsin raising a Swordsman takes about three seconds, a Horseman about six.

Nothing is charged. The only cost check consults the AI player's unit
limits and returns true for a human (:14975-14985).

When the work is done the unit is created on the body's spot for the
raiser's player, facing the way the body lay, and the body is removed
(:13162-13185). A resurrected unit comes back at a tenth of its hit points
(at least one) and the raiser is given a repair order on it (:13186-13205).
An animated unit comes back whole.

## What we build

The corpse is placed from `Units_TickEngines` when the dying unit's threads
finish. `Features_AddInstance` clears the footprint and starts the countdown;
`Features_TickDecompose` counts, sinks and removes. A body with `blocking`
resets the route planner's terrain cache when it lands and when it goes.
Model features are drawn through the same static mesh run as projectile
models, at the instance's position and facing, sinking as they rot.

Deviations, recorded in docs/MANUAL_DEVIATIONS.md: the raise takes the
supply fraction as 1 because the economy tracks no such figure, and the
underwater sink is not built because there is no water.
