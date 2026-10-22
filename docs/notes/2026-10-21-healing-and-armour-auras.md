# Healing and armour auras

A unit file may carry three sections that the original reads with the
rest of the unit (legacy:163515-163527): `[AdjustArmor]`, `[AdjustAttack]`
and `[AdjustJoy]`. Each holds `Adjustment` (a float, 1 when absent),
`Radius` (px, 0 when absent), `EdgeEffectiveness` (a float, 0 when absent)
and `AffectsEnemy` (legacy:231765-231795). A radius of 0 is no aura. The
shipped units that carry them:

| Unit | Section | Adjustment | Radius | Edge |
|---|---|---|---|---|
| Acolyte, Acolyte of Anu (ARAPRIES, ARAPRIE2) | AdjustJoy | 250 | 200 | 0.6 |
| Priest of Lihr (VERLIHR) | AdjustJoy | 250 | 200 | 0.6 |
| Pillar of Light (VERPILL) | AdjustJoy | 75 | 550 | 0.4 |
| Sacred Fire (ZONFIRE) | AdjustJoy | 45 | 90 | 1 |
| Titan (ARASMITH) | AdjustArmor | 1.4 | 200 | 0.6 |
| Shaman (ZONSHAM) | AdjustArmor and AdjustAttack | 1.2 | 200 | 0.1 |

None of them sets `AffectsEnemy`.

## What the original does

Every frame each unit's attack and armour scale start again from their
base (legacy:235818-235836). Then each live, complete unit placed on the
map scales the units round it (legacy:235838-235890). The search takes
every unit, and every unit carried by one, whose position lies within the
radius of the caster's (legacy:220374-220459). The caster itself is left
out, and so is any unit on the other side, or with `AffectsEnemy` any
unit on its own side or an ally's (legacy:237117-237129).

How strongly the aura reaches a unit `d` px away is
`edge + (1 - edge) * d / radius` (legacy:237131-237139). That is the edge
effectiveness at the centre, rising to the whole at the rim.

- AdjustArmor multiplies the unit's armour scale, and AdjustAttack its
  attack scale, by `(adjustment - 1) * reach + 1`, every frame
  (legacy:237112-237188). A hit is the weapon's damage times the
  shooter's attack scale, divided by the target's armour scale
  (legacy:245324-245343). Two Shamans multiply.
- AdjustJoy runs once every 30 frames (legacy:235816,
  legacy:235862-235882). A first pass counts every live, complete unit it
  reaches. The second gives each `adjustment * reach / count` of build
  work (legacy:237192-237228). That is the repair applicator: build work
  over the unit's buildtime is the share of its whole health it gets
  back, paid by the caster's owner at the unit's buildcost from what the
  treasury gives. Nothing is paid for a unit already whole
  (legacy:39540-39580). The amount asked is paid even when the heal is
  more than the unit is missing, and a whole unit still counts toward the
  split.

The auras do not depend on the unit being active. The Sacred Fire is
`onoffable`, and its Activate and Deactivate only start and stop its
flames (its own script), so it heals whether on or off. The Pillar of
Light has no `onoffable` in its file and so no on and off buttons.
The ACTIVATE and DEACTIVATE orders reach every onoffable unit
(legacy:150430-150436, legacy:151449-151470).

A unit file may also name the aura whose ring the selection shows
(`showrangeattrib`, `Joy`, `armor` or `attack`, legacy:163420-163494). No
shipped unit sets it.

## What we do

The loader reads the three sections into the unit definition. Before
combat on every tick, each unit's attack and armour aura scales start at
1 and every caster scales the units it reaches. A hit multiplies by the
shooter's scale and divides by the target's, after the veteran scaling
and before the console's double and half shot. The scales are worked out
afresh each tick from what the simulation keeps, so they are neither
saved nor hashed. AdjustJoy heals on every 60th tick, through the same
treasury share a repair pays from and the same 1/256 hit point remainder
the free self repair keeps.

Activate and Deactivate now reach any onoffable unit, not gates alone, and
the HUD lights the state it is in.

## Measured

On two castles, beside a swordsman at a fifth of its 2500 hp, 60 px away,
over ten seconds (a beat is the AdjustJoy's once a second):

| Healer | Before | After |
|---|---|---|
| Acolyte | 500 to 602 hp (its own healtime) | full after 1 beat, 519 mana |
| Pillar of Light | 500 to 605 hp | full after 3 beats, 301 mana |
| Sacred Fire | 500 to 602 hp | full after 2 beats, 259 mana |
| Priest of Lihr | 500 to 605 hp | full after 1 beat, 518 mana |

A swordsman is 99 seconds of build work for 285 mana, so an Acolyte's 180
a second at that range is nearly two whole swordsmen of healing and
costs 518 mana.

The same rule makes a monarch dear to heal. A Monarch is 10 seconds of
build work for 100000 mana, so 20 work a second asks 200000 mana, and a
damaged monarch beside a healer takes the whole treasury every second
while it stands there, as in the original. Builders then wait for mana.
A heal draws at the builders' share but never sets it, since the share
counts busy builders alone (legacy:235952-235977).

## Departures

- Who counts as an ally is the engine's rule: the lobby's teams, or an
  alliance struck in the battle by both sides. The original reads the
  caster's owner's own alliance table alone.
- The ring `showrangeattrib` names is not drawn. No shipped unit asks
  for one.
- A heal reaches the fixed point no larger than the health missing, so
  a heal of more than 32767 hit points cannot overflow it.

## Tests

test_line_of_fire: an Acolyte heals one unit in reach by 200 for 20 mana
and splits its work between two, nothing between beats, never an enemy.
A Shaman scales an ally's attack and armour by 1.11 at half its radius, so
a 40 bolt hits for 44 from it and 36 on it. A Sacred Fire takes the on and
off order and heals either way.
