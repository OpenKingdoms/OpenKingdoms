# Contextual healing and cleanup

With no command armed, the pointer used to offer healing only over an
unfinished building. A damaged unit showed the select hand, and a map
feature that could be swept showed the plain pointer unless it was a
body the selection could raise.

The original's default cursor offers more, and the hover and the click
now follow it:

- A damaged unit at peace with you, your own or an ally's, offers Heal
  when a selected unit of yours could mend it by the original's repair
  test: a finished unit standing still on the ground, and a mender that
  walks and repairs (legacy:186541-186545, legacy:233526-233553). A
  click sends the menders. A unit at full health offers the select hand.
- Your own frame offers Heal when a selected unit can help build it, as
  before.
- A feature on explored ground offers the sweep cursor when the
  selection holds a unit of yours that can reclaim
  (legacy:186587-186599). A click sends the sweepers to sweep it and the
  other walkers to the spot, as a click on a frame splits them.
- Raising a body comes before sweeping it, the original's order
  (docs/notes/2026-09-10-corpses-and-raising.md).
- Shift changes none of this, as in the original, so a Shift click on a
  unit the selection could mend queues the mend.
- Attacking an enemy and looking at an ally are unchanged. The sweep
  cursor is never offered over a live building.

The checks read the local player's selection and fog. The raise check
used to read player one's for both, which gave every other seat in a
match the wrong cursor. The checks only choose a cursor and an order.
The order still goes through the command queue, and the simulation
decides whether it runs, so a match plays the same on every machine.

## Tests

`the_heal_cursor_shows_over_a_damaged_unit_a_walker_could_mend` in
test_orders covers the mender, an ally, an enemy and a building that
builds. `the_revive_cursor_shows_over_a_body_the_selection_can_raise`
checks that a builder over a body it cannot raise shows the sweep
cursor, that its click sweeps while a swordsman beside it walks there,
that a raiser in the selection still shows revive, and that the second
seat sees revive on its own explored ground.
