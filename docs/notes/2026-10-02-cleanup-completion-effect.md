# Cleanup completion effect

The feature-reclaim work step removed the target without creating a visual
effect. Successful cleanup now creates one detached, one-shot effect at
the feature's centre and terrain height. Its position is captured before
removal because removing a feature compacts the instance array.

This is specific to successful cleanup. Generic feature removal, decay,
cancelled orders and missing targets do not create this effect. Existing
mana refunds and cleanup timing are unchanged. Missing art or a full
effect pool can drop the visual without preventing cleanup.

## Art choice is provisional

The installed retail archive contains `anims/deathmagic.gaf` with two
sequences: `Death01` has 20 frames and `PurpleDeath` has 15. The existing
palette lookup associates this sheet with `fx.pcx`. PurpleDeath already
serves resurrection. A decoded Death01 preview shows a dark reddish
dispersal effect.

Death01 is used provisionally for cleanup. No config entry or original
runtime observation was found that establishes this mapping. Asset names
and appearance alone do not prove original-game parity. The user was asked
for the remembered appearance. Local previews are `build/Death01-preview.png`
and `build/PurpleDeath-preview.png`, produced by `build/inspect-cleanup-art.py`.
These game-derived images are not committed.

The animation uses the existing engine-effect cadence of four simulation
ticks per frame and expires after 80 ticks. Both render views consume the
shared effect pool. Exact original cleanup timing remains unverified.

## Verification

The resource cleanup regression failed before the fix with expected one
completion effect, actual zero. It now checks no early flash, exactly one
at completion, position, terrain height, advancing frames and expiry.
The corpse cleanup test checks the same completion boundary. A separate
test checks cancellation and removal by another system.

Six targeted native Release tests pass, including existing mana payout,
resurrection and cleanup command tests. Native and public WebAssembly
builds succeed. Browser runtime appearance was not separately checked.
Local evidence is in `build/cleanup-red.log`, `build/cleanup-green.log`,
`build/cleanup-checks.log`, `build/cleanup-build.log` and
`build/cleanup-wasm-build.log`. Review is in `build/cleanup-review.txt`.
