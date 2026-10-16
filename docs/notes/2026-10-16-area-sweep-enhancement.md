# Area sweep and area resurrection (2026-10-16)

## What was added

Armed CLEAR (the sweep/cleanup button) now supports drag-select. When CLEAR
is armed and the player draws a rectangle, every sweepable feature in the
box gets a queued TAK_CMD_RECLAIM_FEATURE order. The first order replaces
the queue, subsequent ones queue behind it. Each selected unit resolves
per feature: a raiser raises, a reclaimer sweeps (legacy:187142-187198).

## The canreclaim gate

The original gates the whole sweep branch on canreclaim (legacy:187127).
A unit with canresurrect but not canreclaim, such as Elsin, cannot use the
CLEAR button at all. In the original Elsin resurrects via default
right-click on individual corpses, not through the sweep button.

## Possible future enhancement

Allow area resurrection without arming CLEAR. A right-click drag (or a
dedicated resurrect button) that emits TAK_CMD_RESURRECT_FEATURE per corpse
in the rectangle would let monarchs resurrect groups without clicking each
corpse individually. This would be a deliberate deviation from the original
and should be recorded in MANUAL_DEVIATIONS.md if implemented.
