# Manual Deviations

Running log of intentional departures from the gameplay rules in
`Total_Annihilation_Kingdoms_Manual.pdf`. The default is faithful. A
deviation needs a recorded reason.

Format per entry:
- Change
- Why
- Manual citation (page, section)

---

## D-001: 60 Hz simulation tick (vs TAK's 30 Hz)

- Change: Game simulation runs at 60 Hz fixed instead of 30 Hz.
- Why: Smoother animation on modern displays. Future-proof for
  lockstep multiplayer at higher tick rate. Real-time gameplay is
  unaffected because TAK speeds are stored as units/second (divided
  by tick rate per tick). Doubling tick rate halves per-tick delta
  but applies it twice as often. SLEEP times in milliseconds
  translate identically.
- Citation: Manual is silent on tick rate (player-facing rules
  describe wall-clock seconds via Game Speed slider, §I).

## D-002: Unit limit of 2000 per player

- Change: The per-player unit cap ranges 200 to 2000 with a default
  of 2000 (TAK_UNITS_PER_SIDE_MIN/MAX/DEFAULT in
  `include/tak_battle_config.h`).
- Why: The original's cap of 500 came from 1999 memory and CPU
  limits. Large late-game armies are part of the appeal.
- Citation: The original's setup screen slider ran 20 to 500 (the
  "maxunits" summary field).

## C-001: Frustum culling

- Change: Off-screen units skip vertex transform and submit.
- Why: Performance. Modern viewports are larger. Without culling
  a 16k×16k map with hundreds of deployed units submits work for
  every unit even when 90%+ are off-camera.
- Citation: Manual implies all-on-screen rendering ("you have to
  explore it with your units to reveal the battlefield" §IV.2 Battle
  Map). No gameplay impact. Fog of war and visibility are
  player-facing, and culling is internal.

## C-002: Modern resolution support

- Change: Render at native window resolution, no upper limit. The
  manual specifies 640×480 minimum and warns higher resolutions are
  costly.
- Why: 1999 hardware concerns are moot. Unit visibility is the
  same regardless of resolution.
- Citation: Manual §I.6 ("Make sure the Resolution slider is set
  to 640x480 in the Visual settings of Game Options. Higher
  resolutions require considerably more system resources.").

## R-001: Hardware-accelerated rendering

- Change: The scene is drawn through the GPU (SDL2, textured
  triangles) instead of the original's software 8-bit palettised
  rasteriser, at any window size.
- Why: Modern displays and drivers. No 8-bit modes exist any more.
  The projection, draw order, lighting tables and palette effects
  reproduce the original's output.
- Citation: Manual §I.6 on resolution and video settings.

## N-001 (planned): Deterministic lockstep multiplayer

- Change: Networking will be lockstep over a relay server, with
  browser clients, instead of the original's peer-authoritative
  DirectPlay sessions.
- Why: DirectPlay is gone, does not traverse NAT, and cannot run in
  a browser. Lockstep also gives replays and a server that holds no
  game data.
- Citation: Manual section on multiplayer setup (DirectPlay/TCP-IP
  options).
- Status: Not yet implemented. Design in `docs/MULTIPLAYER.md`.

---

*(More entries added as deviations land.)*
