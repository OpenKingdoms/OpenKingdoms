# COB-driven damaged building flames

The retail buildings already run DamageFlameControl from their COB scripts.
The missing connection was EMIT_SFX in the VM, which consumed its operands
without forwarding the effect request. GET_UNIT_VALUE port 4 already reads
health percentage through the call-function dispatcher. A heading query in
another host interface is unrelated and has not been changed.

The VM now resolves the script piece through its existing name-based model
mapping and forwards the node and SFX type to an optional host callback.
Missing callbacks and unbound pieces remain harmless. The unit host accepts
0x104, 0x105 and 0x106 for the small, medium and large sprites named by
`gamedata/damageflames/damageflames.tdf`. Other SFX types remain unsupported.
Smoke is a separate effect and is not implemented by this change.

The host computes the attachment's world position and puts a one-shot
animation in the existing effect pool. Both rendering views use that pool
and their existing fog rules. Flames follow their animated attachment until
the animation expires or the owner disappears. Stable IDs protect reused
unit slots. The callback is installed on normal spawn and save restoration.
It reads no gameplay random values and changes no simulation state.
The host rejects emissions from unfinished construction, including a
direct request from a script that has not waited for completion.

The previous hardcoded controller has been removed, including its health
bands, single-node emission scheduler and health-driven sprite resizing.
The original scripts choose when and where to emit and which size to use.
Repair stops new emissions when the script next checks health. Existing
animations finish instead of being resized or extinguished by an engine
health check.

For the installed ARAKEEP script, the intensity budget is distributed
across eight nodes with three levels per node. The maximum budget of 24
is not a count of 24 separate attachment points. At integer health 30 and
29 the budget is zero, at 28 it is one, and at 10 it is sixteen. The script
randomly adds or removes intensity at individual nodes, then emits all
active nodes and sleeps for 499 ms. It first waits for construction to end.
These are observations of the installed data, not new engine rules.

The VM's old 200-instruction allowance split this loop across ticks and
produced 31-tick intervals even at low intensity. The bounded allowance is
now 16384 instructions per thread per tick so normal effect batches can
reach their authored sleep. The 499 ms sleep itself is unchanged and gives
30-tick intervals with the current scheduler. This allowance applies to
all COB threads, so coroutine and corpus tests are part of verification.

Damage flames can occupy at most 128 of the 512 shared effect slots.
Requests beyond that capacity are dropped without changing script execution
or consuming random values. No new emission schedule is introduced to make
room. Under heavy effect load, some requested flames may not be displayed.

Regression coverage:

- Synthetic VM scripts verify callback arguments, case-insensitive piece
  mapping, absent callbacks, invalid pieces and stack alignment.
- A batch longer than 200 instructions must reach SLEEP in one tick.
- The retail barracks script verifies health boundaries, summed per-node
  intensity, simultaneous emissions, 30-tick cadence and repair.
- The save-restore binding and retail construction wait are exercised.
- Pixel comparisons prove visible flames in both classic and 3D views.
- The host's capacity and unsupported types are checked without changing
  the simulation hash or random state. Reused unit slots lose old flames.

No game assets or original script code are included in the repository.
Tests use the owner's installed data. Local logs and screenshots are under
`build/cob-sfx-*` and `build/damage-flames-*`.

The Release executable was rebuilt. VM selftests and 621 corpus smoke
cases across 156 files pass. The four flame tests pass, as do the selected
savegame, savestate, mission-script and simulation-probe suites. UI checks
for COB entry points, loading a saved battle and the first mission pass.
An isolated control build with the old 200-instruction allowance fails the
new batch regression. The production build passes it and the runaway test.
Independent review has no unresolved findings.

The full 3D suite has 30 passes, two failures and one skip. Its pre-existing
building-floor and build-pad failures reproduce with the unchanged repository
HEAD and this retail install.
Emscripten is not installed locally, so browser compilation remains unchecked.
The local OpenRig session has no bound identity, so its queue is unavailable.
