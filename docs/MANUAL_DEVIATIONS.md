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

## M-001: Route following near obstacles

- Change: With something held in a neighbouring cell, a route point
  counts as passed once the unit is within 16 px of it. The original
  passes it only when the unit stands on it, within a few pixels
  (legacy:191282-191284).
- Why: The original plans on 16 px cells and its route points are the
  exact unit origin for a cell. Our planner works on 32 px cells, so
  requiring the unit to land on the point would have it turn back for
  points it has effectively passed. Away from obstacles the original's
  own 32 px radius and cross track rules apply unchanged
  (legacy:184729-184791).
- Citation: `docs/notes/2026-09-10-unit-steering.md`. The manual is
  silent on route following.

## M-002: Replan timing when a unit is in the way

- Change: A unit pinned against a parked unit (M-003) plans a new route
  once it has made no headway for 10 frames. The original searches
  again on every hard blocked frame (legacy:191290, legacy:191387).
  Behind a unit still on the move it waits two seconds plus a small
  fixed per unit offset first. A route that makes no progress toward
  its target for 2.5 seconds is dropped and planned again. Without a
  hard block the original replans after a randomised delay scaled by
  the unit's speed class (legacy:191300-191360) or at random after 120
  frames (legacy:191375-191385).
- Why: The plan ignores units on the move, so searching again behind
  one returns the same route and the wait saves the work. One search
  after 10 frames without headway finds the same way around a parked
  unit that a search every frame would. A fixed delay keeps the
  lockstep simulation free of an extra random draw per blocked unit,
  and the owner asked that no unit ever sit stuck for good against a
  wall or a crowd. The stall watchdog is the guarantee for routes the
  original would have followed into a pocket.
- Citation: `docs/notes/2026-09-10-unit-steering.md`.

## M-003: Units that hold their cells become planning obstacles

- Change: A ground unit that has held its cells for 10 frames, or for
  as long as a cell takes at three quarters of its top speed if that is
  longer (at most a second), is tagged in the occupancy layer, idle or
  held up in a jam alike, and other units plan around it. The original
  folds any unit into its search grid at 10 frames with a slowed rim,
  closes it outright at 150 frames (legacy:188900-188960,
  legacy:187765-187880), and checks units far from the searcher live,
  letting through one that keeps three quarters of the searcher's top
  speed (legacy:21986-22032, legacy:184497-184510). One rule stands in
  for all of that here.
- Why: The plan is a yes or no answer per cell. Judging the pace by the
  time spent on a cell keeps a unit that walks across it out of the
  plan, and a jam has to be visible to the next search, or a column
  behind a stuck unit plans the same route into it again and again.
- Citation: `docs/notes/2026-09-10-unit-steering.md`,
  `docs/notes/2026-09-10-clearance-grid.md`.

## M-004: Clearance grid instead of per cell placement checks

- Change: The planner keeps a clearance map, per move class, giving the
  largest square footprint that fits at every tile, and a unit plans
  only through cells whose clearance covers its own footprint. The
  original runs its footprint placement check per cell during the
  search (legacy:187701-187930 over legacy:219074-219179).
- Why: The same answer for a shipped unit, computed once per map change
  instead of per search node, and the base the hierarchical search,
  flow fields and local avoidance in the design note would sit on. Where
  the original's per cell check and the clearance map disagree the map
  is the stricter of the two, which never sends a wide unit through a
  gap it cannot fit.
- Citation: `docs/notes/2026-09-10-clearance-grid.md`.

---

## M-005: Melee reach counts to the body

- Change: A melee weapon also reaches a walker whose tiles touch the
  attacker's, whatever the centre distance. Otherwise reach stays the
  original's centre distance to a mobile target (legacy:235291). A
  route toward a unit ends on that unit's own cell even when it is
  parked there.
- Why: Walkers hold their cells here (M-003), and the tiles snap to a
  16 px grid, so two units in contact stand 17 to 47 px apart centre
  to centre. With centre distance a 30 px sword could stand against
  an enemy and never strike, and a route shifted off the parked
  target's cell stopped the attacker one cell short.
- Citation: `src/ui/test_ui_screens.c`,
  `swordsman_strikes_an_enemy_standing_beside_it`.

## A-001: AI base defence recall

- Change: When an enemy hits anything within 1280 px of an AI player's
  start, every combat unit of that AI within 1536 px of the start is
  ordered at the attacker (an attack order when the AI can see it, a
  march to where the shots came from when it cannot) until ten seconds
  pass without a hit. An idle unit at home does the same for an allied
  base, a human teammate's included. Waves further out keep going.
- Why: The original has no recall. Its home units engage what enters
  their engagement radius, so a raid on a lodestone at the edge of the
  base can go unanswered while the army is away. Players reported AIs
  that never reacted. The original's rule that a hit on the monarch holds
  its own construction for 1 to 31 seconds (legacy:15092) is kept
  alongside.
- Citation: The manual describes no AI defence rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Hits on an AI".

## A-002: AI influence maps tilt the target and call the defence

- Change: Each AI keeps a coarse grid, one cell per 256 px, of its own
  and allied combat value and assets, of the enemy combat value and
  assets it can see, and of sacred sites and lodestones. The original's
  target scorer (legacy:15365) is multiplied by a quarter to four times
  by how weak and valuable the enemy is in the candidate's cell. A cell
  where seen enemy strength outweighs the AI's presence and something
  of the AI's stands is treated like a hit on the base: the home units
  are sent at the first seen enemy in it before a shot lands.
- Why: The original scores by distance and chance alone and only reacts
  once a unit is hit. Reading strength and value over the map lets the
  AI go for exposed lodestones instead of a fortified front and meet a
  massing army at its own expansion. Per-unit values keep the original's
  formula (legacy:19803). The grid is the same summed threat the
  original computes on demand within a radius (legacy:20857), binned.
- Citation: The manual describes no AI rule for either. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Target choice".

## A-003: AI builds and attacks by goal planning

- Change: Each AI tick reads an abstract state (mana and its cap,
  income against spend, lodestones, factories, idle builders, army at
  home and away, threat and exposure off the influence maps, free
  sacred sites, a known wave target) and scores five goals: hold a
  lodestone count, defend home, keep an army sized to the threat,
  secure a nearby site, kill the weakest enemy. Actions (build a
  lodestone, a factory, a tower, train a unit, hold, send a wave) carry
  preconditions and effects, and a depth-three search finds the
  cheapest sequence for the top goal. Its first step is what the idle
  builder, factory or army does this tick. A walking builder in a fight
  counts as idle, and the build replaces its chase, as the builder think
  did before the planner. Costs are mana cost scaled by the profile
  weight and the profile limits cap the counts, so the sides keep their
  character. Which unit a factory trains is still the
  original's weighted draw.
- Why: The original walks a fixed order (a mana building only under
  pressure, otherwise whatever the weighted draw returns) and never
  looks at the map. A starved AI now feeds its lodestone before it
  trains, and a threatened one raises a tower or holds its army before
  it expands. The original's cues are kept: a mana building at under 30
  percent mana or a stall, one at a time, and the cost brake while
  starved (legacy:19859).
- Citation: The manual describes no AI build rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Build decision" and the gap
  list.

*(More entries added as deviations land.)*
