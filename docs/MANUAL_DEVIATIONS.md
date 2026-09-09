# Manual Deviations

Running log of intentional departures from the gameplay rules in
`Total_Annihilation_Kingdoms_Manual.pdf`. The default per
`PROJECT_PLAN.md` §1.2 is **faithful**; deviations need a recorded
reason.

Format per entry:
- **What we changed**
- **Why**
- **Manual citation** (page, section)
- **Phase that introduced it**

---

## D-001 — 60 Hz simulation tick (vs TAK's 30 Hz)

- **Change**: Game simulation runs at 60 Hz fixed instead of 30 Hz.
- **Why**: Smoother animation on modern displays. Future-proof for
  lockstep multiplayer at higher tick rate. Real-time gameplay is
  unaffected because TAK speeds are stored as units/second (divided
  by tick rate per tick); doubling tick rate halves per-tick delta
  but applies it twice as often. SLEEP times in milliseconds
  translate identically.
- **Citation**: Manual is silent on tick rate (player-facing rules
  describe wall-clock seconds via Game Speed slider, §I).
- **Phase**: D.

## C-001 — Frustum culling

- **Change**: Off-screen units skip vertex transform and submit.
- **Why**: Performance. Modern viewports are larger; without culling
  a 16k×16k map with hundreds of deployed units submits work for
  every unit even when 90%+ are off-camera.
- **Citation**: Manual implies all-on-screen rendering ("you have to
  explore it with your units to reveal the battlefield" §IV.2 Battle
  Map). No gameplay impact — fog of war and visibility are
  player-facing; culling is internal.
- **Phase**: C (M8).

## C-002 — Modern resolution support

- **Change**: Render at native window resolution, no upper limit. The
  manual specifies 640×480 minimum and warns higher resolutions are
  costly.
- **Why**: 1999 hardware concerns are moot; unit visibility is the
  same regardless of resolution.
- **Citation**: Manual §I.6 ("Make sure the Resolution slider is set
  to 640x480 in the Visual settings of Game Options. Higher
  resolutions require considerably more system resources.").
- **Phase**: A.

---

*(More entries added as deviations land.)*
