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
- [ ] `ctest` passes
- [ ] Added a test that would have failed before this change

<!-- How did you verify this by hand? Screenshots welcome for visual changes. -->

## Checklist

- [ ] No game data committed (no `.hpi`, assets, extracted files, maps)
- [ ] No original game code, decompiled or disassembled, in the diff or in
      commit messages
- [ ] Comments are brief, a line or two stating a constraint or a reference
- [ ] Matches the surrounding code style
