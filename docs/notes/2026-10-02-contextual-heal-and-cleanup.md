# Contextual healing and cleanup

With no command armed, the pointer used to offer healing only over an
unfinished building. A damaged unit showed the select hand, and a map
feature that could be swept showed the plain pointer unless it was a
body the selection could raise.

The hover and the click now share one set of checks:

- A damaged unit of yours offers Heal when another selected unit of
  yours can repair it, and a click sends the menders. A unit at full
  health offers the select hand. This one is our own addition, D-044.
- Your own frame offers Heal when a selected unit can help build it, as
  before.
- A feature on explored ground that the selection can sweep offers the
  sweep cursor, and a click sweeps it, when the selection holds a mobile
  unit of yours that can reclaim. The original's default cursor does the
  same (legacy:186587-186599).
- Raising a body comes before sweeping it, the original's order
  (docs/notes/2026-09-10-corpses-and-raising.md).
- Shift over your own unit shows the select hand, and a Shift click
  still adds it to the selection or takes it out.
- Attacking an enemy and looking at an ally are unchanged. The sweep
  cursor is never offered over a live building.

The checks read the local player's selection and fog. The raise check
used to read player one's for both, which gave every other seat in a
match the wrong cursor. The checks only choose a cursor and an order.
The order still goes through the command queue, and the simulation
decides whether it runs, so a match plays the same on every machine.

The repair check is the one the repair order makes: a frame the unit
can help build, or a unit at peace with it when it can repair.

## Tests

`the_revive_cursor_shows_over_a_body_the_selection_can_raise` now also
checks that a builder over a body it cannot raise shows the sweep cursor
and its click sweeps, that a raiser in the selection still shows revive,
and that the second seat sees revive on its own explored ground.
