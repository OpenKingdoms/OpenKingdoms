# Contextual healing and cleanup

With no command armed, the HUD previously suggested healing only on an
unfinished structure. A damaged completed unit showed the selection hand.
A reclaimable map object showed the normal pointer unless it was a body
the selection could resurrect or animate.

The default hover and click paths now share eligibility checks:

- A damaged own unit offers Heal if another selected own unit can repair
  it. A completed unit at full health offers selection instead.
- An unfinished own unit offers resume construction when a selected unit
  can perform that order.
- A reclaimable feature on explored ground offers Cleanup if the selection
  contains an owned mobile reclaimer. An incapable unit or empty selection
  gets no suggestion.
- Resurrection or animation takes precedence over cleanup on eligible
  bodies. Mixed selections still use each executor's capability checks.
- Shift over a selectable own unit shows the selection hand and Shift-click
  toggles selection. An explicitly armed Heal command still supports queues.
- Enemy attack and allied inspection retain their existing behavior.
  Cleanup does not suggest demolishing live buildings.

The predicates use the local player's selection and fog. The existing
resurrection predicate used player one for both and now uses the local
player too. Hover checks do not issue orders. Clicks use the existing
command queue, so the simulation remains the authority when an order runs.

## Original behavior evidence and limits

The [original manual](https://retrogamer.biz/wp-content/uploads/2015/10/Total_Annihilation_Kingdoms_Manual.pdf),
printed pages 70 and 71, describes healing and clearing as unit-dependent
utility actions and demonstrates an explicitly selected Heal command in
a Shift queue. It does not give a complete default-hover decision table.

The repository's `2026-09-10-corpses-and-raising.md` documents the original
sweep priority: resurrection, animation, then reclaiming eligible features.
Existing feature lookup also rejects bodies that have started sinking.
The existing selection path documents Shift selection toggling, which this
change preserves. Repair eligibility is shared with the existing repair
executor rather than approximated using only a builder flag.

The requested automatic Heal and Cleanup suggestions follow the user's
reported expectation. Exact retail parity for automatic healing, mixed
selections and all modifier combinations has not been independently
verified in the original executable. These limits must not be presented
as a confirmed complete recreation of the retail hover resolver.

## Verification

Both new tests failed before the change: healing expected Heal but got the
selection hand, and cleanup expected Cleanup but got the normal pointer.
They exercise the hover result and the actual default-click command path,
including health, capability, ownership, mixed selection, local player two,
Shift toggling, unexplored terrain and removed features.

The existing revive test now verifies that the builder's default click
issues cleanup, cancels that order before testing resurrection, and checks
resurrection priority for mixed selections and local player two.

Local test selection and logs are `build/hover-cases.txt`,
`build/hover-red.log`, `build/hover-green.log`, `build/hover-checks.log`,
`build/hover-build.log` and `build/hover-wasm-build.log`. Independent review
is in `build/hover-review.txt`. OpenRig could not record a queue row because
this session has no bound identity.
