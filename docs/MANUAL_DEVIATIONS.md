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

## D-003: Raising the dead always works at full supply

- Change: A resurrection or animation takes one frame's work per frame
  regardless of the player's mana. The original scales each frame's
  work by the player's economy supply fraction, the same figure that
  slows a starved build (legacy:13087, legacy:39469).
- Why: The economy has no such per-player figure yet. Raising charges
  nothing in the original, so the only effect is that a starved player
  raises at full speed. Revisit when the supply fraction lands.
- Citation: Manual is silent. Behaviour note
  `docs/notes/2026-09-10-corpses-and-raising.md`.

## D-004: No underwater corpse sink

- Change: A corpse that lands below sea level is placed like any other
  and rots on the normal schedule. The original sinks it straight away
  (legacy:227440-227450).
- Why: There is no water rendering or water gameplay yet. Bodies in
  water are rare on land maps and the branch can land with water.
- Citation: Manual is silent. Behaviour note
  `docs/notes/2026-09-10-corpses-and-raising.md`.

---

## A-001: AI base defence recall

- Change: When an enemy hits anything within 1280 px of an AI player's
  start, every combat unit of that AI within 1536 px of the start is
  ordered at the attacker (an attack order when the AI can see it, a
  march to where the shots came from when it cannot) until ten seconds
  pass without a hit. An idle unit at home does the same for an allied
  base. Waves further out keep going.
- Why: The original has no recall. Its home units engage what enters
  their engagement radius, so a raid on a lodestone at the edge of the
  base can go unanswered while the army is away. Players reported AIs
  that never reacted. The original's rule that a hit builder freezes new
  construction (legacy:15092) is kept alongside.
- Citation: The manual describes no AI defence rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Hits on an AI".

*(More entries added as deviations land.)*
