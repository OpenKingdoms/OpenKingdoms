## What this changes

<!-- One or two sentences. The diff shows what, so say why. -->

## Parity evidence

<!-- REQUIRED if this changes game behaviour. Delete this section if it
     doesn't (build fixes, refactors, docs, tooling).

     OpenKingdoms targets behavioural parity with the 1999 game: when we and
     the original disagree by accident, we're wrong. So say what the original
     does and how you know. See CONTRIBUTING.md. -->

- Original behaviour:
- Our behaviour before this change:
- Evidence: <!-- docs/notes/ citation, manual reference, reproducible
                 in-game observation, or a data-file fact -->

<!-- If you are departing from the original on purpose, say so and say why.
     Deviations are sometimes right, but they get recorded rather than
     slipped in. Add an entry to docs/MANUAL_DEVIATIONS.md. -->

## Testing

- [ ] Native build passes
- [ ] WebAssembly build passes (emcc rejects what MSVC accepts)
- [ ] `ctest` passes, and `ctest -L golden` if you have the game files

How the change was shown to work:

<!-- A repro, a measurement, or an existing test that now catches it. It
     does not have to stay in the tree. A new test only for the areas
     CONTRIBUTING.md lists. Screenshots welcome for visual changes. -->

## Checklist

- [ ] No game data committed (no `.hpi`, assets, extracted files, maps)
- [ ] No original game code, decompiled or disassembled, in the diff or in
      commit messages
- [ ] No new markdown file, notes edited in place (see CONTRIBUTING.md)
- [ ] Comments are brief, one to three lines stating a constraint or a reference
- [ ] Matches the surrounding code style
