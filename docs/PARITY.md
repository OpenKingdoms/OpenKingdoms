# Parity

OpenTAK's goal is not "a good RTS in the style of Kingdoms". It is *this*
game — the 1999 release — running on modern machines and behaving the way
it did. That single idea decides most arguments before they start.

## The rule

**The original game is the specification.** When OpenTAK and the original
disagree, OpenTAK is wrong. That holds even when the original's behaviour
is odd, unbalanced, or clearly a bug that players learned to live with.
People who still play this game play it *with* those quirks, and a
reimplementation that quietly fixes them isn't the game they remember.

## Deviations

Sometimes we deviate anyway, on purpose. Each one is recorded in
[MANUAL_DEVIATIONS.md](MANUAL_DEVIATIONS.md) with the reason. The current
list is short and mostly structural:

- The simulation runs at 60 Hz where the original ran at 30. Unit rates in
  the data files are converted so speeds, reload times and build times
  come out identical.
- Multiplayer uses deterministic lockstep rather than the original's
  peer-authoritative networking, which depended on DirectPlay and could not
  run in a browser. See [MULTIPLAYER.md](MULTIPLAYER.md).
- Rendering is hardware-accelerated rather than software 8-bit, but the
  projection, draw order, lighting tables and palette effects reproduce the
  original's output.

A deviation that isn't written down is a bug.

## Evidence

Because the original is the spec, changes to game behaviour need to show
what the original does. In roughly descending order of strength:

1. **A behaviour note** in [`notes/`](notes/). These are our own written
   descriptions of how each subsystem behaves — targeting, the economy,
   the animation VM, the AI's build scoring, fog, pathing and so on — with
   the numbers. Citations of the form `:NNNNN` inside them are internal
   reference anchors used by maintainers; contributors can ignore them.
2. **The game manual**, for documented rules.
3. **A reproducible observation** in the original game — what you did,
   what happened, ideally with a screenshot or video.
4. **A data-file fact** — an `.fbi` or `.tdf` field and its observed
   effect.

"It felt better this way" is not on the list.

## Reporting a deviation

If you know the original well and OpenTAK does something differently,
please [file a parity deviation](https://github.com/zbennett10/open-tak/issues/new?template=parity_deviation.yml).
The template asks for the original's behaviour, ours, and your evidence.
These reports are among the most valuable contributions the project gets,
and they need no code.

## Validation

Every behaviour claim wants a way to check it stays true:

- **Automated tests** (`src/**/test_*.c`) for anything that can be asserted
  without a human — parsers, the animation VM against a corpus of scripts,
  pathfinding on generated maps, economy arithmetic, command serialisation.
- **Render probes** that draw a fixed scene to an image for side-by-side
  comparison with the original.
- **Manual smoke items** in [MANUAL_SMOKE_TESTS.md](MANUAL_SMOKE_TESTS.md)
  for the things only a person can judge.

A regression that has been fixed once gets a test so it can't come back
quietly.
