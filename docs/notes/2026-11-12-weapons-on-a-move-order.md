# Weapons on a move order (2026-11-12)

The 0.5.8 notes said units on a move order walk past enemies, which the
original's do not. This is what the original does with a move order and
how the engine now follows it.

## The order itself never fights

A plain move order walks to its point and does nothing else. Its handler
looks for a target only when the order was given as a fight move, or
carries a leash of its own, and the second never while the unit holds
position (legacy:10354-10356). A plain move carries neither, so the
unit never stops, never turns aside and never chases. The order does
not take the weapons either. They stay free to look for themselves, as
they do under every order that does not claim them (legacy:181003).

## The weapons look for themselves

Each frame the original walks round each side's units, maxunits / 30 + 1
of them a frame, so a side under that size has every unit looked at
every frame and a full one each unit once a second (legacy:18985-18996).
For a unit on fire at will that is not building, each weapon left free
with nothing to aim at searches on its own (legacy:19247-19285,
legacy:18920-18933). A dropped weapon never does, and a commandfire one
only for a computer player. No shipped weapon sets commandfire, so the
engine does not read it.

The weapon's own search uses the weapon's range key and nothing else,
where an order's search also takes the sight distance
(legacy:21113-21118). A ranged weapon takes only what it already
reaches. A blade takes anything inside its range key, 300 px on the
shipped swords, and strikes it only in contact (legacy:21141-21144).

The weapon keeps what it took while that stays a thing it can take. It
lets go when its target dies, cloaks, turns friendly or is held already
by a paralyzer, and not when the walk takes it out of reach
(legacy:19262-19284). Hold fire drops it (legacy:9820-9846).

The weapon fires whenever its target is in reach, its aim is done and
it is loaded. A unit that turns its body onto what it shoots only fires
with its body facing the target (legacy:249365-249377), and a walker on
its way does not turn for it (legacy:183434). So such a unit fires on
the move only at what lies ahead of it.

## Hit on the way

A unit on a move order hit by an enemy takes no fight of its own. That
is only for a unit standing by or on patrol. With fire on, each free
weapon that reaches the shooter takes it, unless it already holds
something it reaches (legacy:15187-15215).

## Stances

The stance buttons set the original's fire and move orders together
(legacy:233216-233231). Offensive is fire at will and maneuver,
Defensive fire at will and hold position, Passive hold fire and hold
position. So on a move order Offensive and Defensive both fire at what
they pass and Passive does not. The move order part of a stance makes
no difference to a plain move.

## In the engine

`Unit.pass_id` holds what the weapon in hand took, by stable id, while
the unit's order is a move. It is in the save and the state hash. The
walk is the move order's alone, so `target` stays clear and nothing
that reads a unit's target as a fight sees one. The search uses the
engine's nearest pick, or the scored draw for a fireatwillrandom unit,
as the idle search does.

Not done here: a fight move, which looks with the order's own search at
each wait and turns aside to fight (legacy:10359-10504, the MoveFight
key at legacy:151239-151244), and a weapon per slot, since the engine
fires only the weapon in hand.
