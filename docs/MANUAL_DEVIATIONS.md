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

- Change: Networking is deterministic lockstep over a relay server
  that owns the turn clock, carried on one WebSocket to every client,
  browser and native alike, instead of the original's peer
  authoritative DirectPlay sessions. Only player commands cross the
  wire. The relay holds no simulation and no game state.
- Why: DirectPlay is gone, does not traverse NAT, and cannot run in a
  browser. Lockstep also gives replays and a server that holds no game
  data. A star relay reaches every home network with no port
  forwarding, where the original needed a link between every pair of
  players, which is 28 links at eight.
- Also a non goal: playing against people running the original game on
  GameRanger. The original's compatibility id is a checksum of the
  running executable file, so a different program cannot present a
  matching one, and a mixed game could not be lockstep in any case
  because original peers never send orders and never wait for a turn.
- Citation: Manual section on multiplayer setup (DirectPlay/TCP-IP
  options).
- Status: In progress. Design in `docs/MULTIPLAYER.md`.

## N-002 (planned): The host role moves when the host leaves

- Change: If the host leaves the battle room, the room stays and the
  occupied human seat with the lowest seat number becomes host. The
  same rule applies in game. The room closes only when no human seat
  is left. The server decides and broadcasts it, so no two clients can
  disagree about who is host.
- Why: The original ended the lobby outright, rejecting everyone else
  with a message saying the creator had left, which loses a room full
  of people to one person's dropped connection. In game it handed the
  flag to the human with the highest network id, which two machines
  could resolve differently when a computer player held the highest
  id.
- Citation: Manual section on the multiplayer battle room, which
  describes the host's controls but not what happens when the host
  leaves.

## N-003 (planned): Computer players outlive whoever added them

- Change: A computer player added to an empty seat stays in the game
  when the person who added it leaves.
- Why: In the original a computer player was a network player owned by
  one machine, so it left with its owner. Under lockstep every client
  simulates every seat from the same seed, so ownership of a computer
  seat is not a thing that exists. Removing an army mid battle because
  an unrelated player's connection dropped changes the game for
  everyone still in it.
- Citation: Manual section on adding computer players in a multiplayer
  game.

## N-004 (planned): Pause and game speed are server pacing

- Change: Pause and the speed level are not simulation state. The
  server produces turns faster, slower or not at all, and no client
  applies either to its own simulation. They never enter the state
  hash.
- Why: The original already computed its tick count as elapsed time
  multiplied by the speed level (legacy:242330-242421), so this is the
  same idea moved to the one place that owns time. Keeping speed and
  pause out of the simulation means two clients that disagree about the
  speed setting cannot diverge, only run at different rates, and a
  paused game is a server that stopped closing turns.
- Citation: Manual section I on the Game Speed slider.

## N-005 (planned): Alliance and sharing changes reach everyone

- Change: An alliance offer, an alliance break and the three sharing
  toggles are commands in the turn stream, applied on the same tick by
  every client, so every player learns of them.
- Why: The original told only the affected player. Lockstep cannot
  allow that, because a rule that changes the simulation has to be
  applied identically everywhere. It also removes a real ambiguity in
  the original, where two players could believe different things about
  who was allied with whom.
- Citation: Manual section on the Diplomacy screen.

## N-006 (planned): Game passwords are checked by the server

- Change: A room password is held and compared by the server, in
  constant time, and a wrong password is refused before any room state
  is sent.
- Why: The original compared a 16 bit hash inside the joining player's
  own client (legacy:193511-193597), and created the underlying session
  with no password at all, so the check was advisory. A modified client
  walked straight in. Server side is the only place the check means
  anything.
- Citation: Manual section on hosting a game, which describes the
  password field on the host screen.

## N-007 (planned): Ready clears when the map or an option changes

- Change: When the host changes the map, a rule option or the unit
  cap, every player's Ready is cleared and has to be given again.
- Why: The original already cleared your own Ready whenever you edited
  your own row (legacy:135767-135790), so the idea that a change
  invalidates readiness is the original's. It did not extend it to the
  host's changes, which let a host swap the map under eight ready
  players and start a game nobody agreed to.
- Citation: Manual section on the multiplayer battle room and the Go
  indicator.

## N-008 (planned): Spectators sit outside the eight seats

- Change: A watcher does not occupy one of the eight seats. A room
  holds up to eight players and up to eight watchers. A defeated
  player may stay and watch without holding a seat.
- Why: In the original every participant, human, computer or watcher,
  was a network player and shared the cap of eight (legacy:192826,
  legacy:268685), so a couple of onlookers cost two players their
  places. Our seats are simulation slots and a watcher has none, so the
  limit has nothing to enforce. The host may still refuse watchers, as
  in the original.
- Citation: Manual section on multiplayer setup, which counts up to
  eight players in a game.

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
  flow fields and local avoidance in the design note would sit on.
- Accuracy: Both structures are built from one map of ground the class
  can cross, one answer per 16 pixel tile. The bitmap sweeps the
  footprint over it, centred on the path cell's centre, which is the
  sweep the original runs per cell (legacy:219089-219131). The
  clearance map takes the largest free square of the same map. A test
  asserts the two answer alike for every cell of a shipped map with
  nothing built on it, and the Castle scan reports zero disagreements
  for every shipped move class. This entry used to say the clearance
  map was the stricter of the two, which was true and was a defect:
  the bitmap sampled footprint corners that reached a tile past the
  footprint, so a two tile class was asked for three tiles of ground
  and a monarch was refused a route from ground his own clearance
  called wide enough.
- Known artefact: a path cell is 32 pixels and holds two tiles per
  axis, and a footprint is judged at one placement per cell, so a band
  of walkable ground one cell wide is a cell's own ground at one
  parity against the cell grid and at no cell at all on the other. It
  is named in a test rather than left to be rediscovered. It bounds
  which cells a route may start and stand on and never whether a unit
  already on that ground is given a way off it, because a search that
  starts there may cross it. Enumerating the placements inside a cell
  would remove it and is filed as a follow up.
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
  builder, factory or army does this tick. A walking builder fighting on
  its own account counts as idle and the build replaces its chase, while
  an attack the AI itself ordered stands. Costs are mana cost scaled by
  the profile weight and the profile limits cap the counts, so the sides
  keep their character. Which unit a factory trains is still the
  original's weighted draw.
- Why: The original walks a fixed order (a mana building only under
  pressure, otherwise whatever the weighted draw returns) and never
  looks at the map. A threatened AI now raises a tower or holds its
  army before it expands. The original's cues and gates are kept: a
  mana building at under 30 percent mana or a stall, one at a time
  (legacy:19859), no structure pick while the pool covers less than 70
  percent of what the frames already standing ask for (legacy:17201,
  :235975), and no training under 7/30 of that same measure
  (legacy:17991). A lodestone is planned only for a pad that can take
  it now.
- Citation: The manual describes no AI build rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Build decision" and the gap
  list.

## T-001: Flying transports load and unload by the ground rules

- Change: The Roc and the Ghost Ship pick up and set down units the way
  ships do. They close to `transportdistance` of the rider or the drop
  point and hover there while the hold runs. The original's air missions
  keep flying past the point between units and turn back for the next
  (legacy:27164, legacy:27700-27900).
- Why: That flight pattern needs air movement our flyers do not have
  yet. The reach, the spot test, the effects, the hold and the order the
  units leave in are the original's.
- Citation: The manual describes no rule for it. Behaviour note
  `docs/notes/2026-09-11-transport-load-unload.md`.

## T-002: An unload out of reach closes in before the first unit leaves

- Change: A transport told to unload out of reach moves to
  `transportdistance` less 34 of the point and only then tests the spot.
  The original starts on the first unit once it is inside
  `transportdistance`, which it checks every 15 frames, and finishes the
  move while the hold runs (legacy:14521-14542).
- Why: Doing one after the other keeps the countdown from starting while
  the ship is still under way. The transport stops in the same place and
  the first unit comes off at most about 20 frames later.
- Citation: The manual is silent. Behaviour note
  `docs/notes/2026-09-11-transport-load-unload.md`, "Unloading".

## T-003: The set-down test covers the cells the unit will take

- Change: The drop test centres the cargo's footprint on the point, which
  is where the unit is then put. The original tests the footprint from
  the 16 px cell that holds the point (legacy:14551) and then centres the
  unit on the point (legacy:234362-234395), so the cells it tests are
  shifted by up to half a footprint from the cells the unit then takes.
- Why: A point that passes the test is then one the unit can stand on.
- Citation: The manual is silent. Behaviour note
  `docs/notes/2026-09-11-transport-load-unload.md`, "Unloading".

## R-002: One shadow pass, drawn before the units

- Change: Every unit shadow of a frame is drawn into one mask and the
  mask darkens the ground once, before any unit body. The original
  draws each unit's shadow immediately before that unit, in the order
  the units are drawn.
- Why: One mask means ground under two overlapping pieces, or under
  two units standing close together, darkens once instead of twice,
  which is what a shadow looks like. The cost of the pass also stays
  in one target switch per frame rather than one per unit. What it
  gives up is a shadow falling across a unit that was drawn earlier,
  which the original allows for a unit standing north east of a tall
  building. Shadows there stop at the neighbour's outline instead of
  crossing it.
- Citation: The manual describes shadows only as a Visual Options
  setting (Game Options, Visual). Behaviour anchors legacy:197182-197243.

## R-003: No shadow while a building goes up

- Change: A unit under construction casts no shadow. It gets one the
  moment it is finished.
- Why: The original starts drawing a half built unit at the halfway
  mark and fades it in with the body, and its shadow fades in with it.
  Our shadow mask carries coverage, not per unit opacity, so a faded
  shadow would need its own pass. A building spends a few seconds
  going up and the shadow appears with the finished walls.
- Citation: Manual is silent on construction visuals. Behaviour
  anchors legacy:197230-197266 and legacy:197310-197320.

## R-004: Shadows always drawn at full resolution

- Change: Shadows are rasterised at the same resolution as the scene.
- Why: The original has a shadow scale setting with an automatic mode
  that drops shadows to half or quarter resolution when a frame runs
  long. Modern hardware draws them at full size cheaply: with 64
  monarchs on screen the pass costs about 2.4 ms a frame through
  OpenGL ES, the browser's path, and 5 ms through Direct3D 9. A player
  who wants the frames back turns shadows off in Visual Options.
- Citation: Manual §I.6 lists the Visual settings and warns that
  higher settings cost system resources. Behaviour anchor for the
  scale setting is the video options block at legacy:197182.

## R-005: Model textures inset half a texel

- Change: A model polygon samples its texture from half a texel inside
  the atlas entry rather than from the entry's exact edges.
- Why: Every model texture shares one atlas, and a sample landing on
  the boundary would read the neighbouring entry. Half a texel is the
  smallest inset that cannot, and it shrinks the drawn art by under
  one percent of a 64 texel tile. The engine point samples, as the
  original does, so nothing wider is needed.
- Citation: Manual is silent on texture sampling.

## R-006: Shadow sprites step from one counter

- Change: A walking unit's shadow sprite runs its frames off the
  renderer's frame counter, so every moving unit of a kind shows the
  same shadow frame. A standing unit holds frame zero.
- Why: The original steps each unit's shadow animation from that
  unit's own sprite state, which it keeps beside the unit. We do not
  carry per unit sprite state for shadows, and the difference is a
  blob of a dozen pixels under a walking soldier.
- Citation: Manual is silent. Behaviour anchor legacy:197225-197229.

## D-005: A map pack contributes only its map

- Change: Every `.kmp` in the game folder's `Maps` folder is mounted
  at startup, and a mounted pack only answers for paths under
  `kmap/`. Anything else inside a pack is ignored.
- Why: The original mounts a pack only while its map is being loaded
  (legacy:168765), so a pack carrying, say, a unit definition would
  have changed the game for that map alone. Mounting them all is
  simpler and lets the chooser list them without opening 181
  archives twice, and the `kmap/` guard keeps a downloaded map from
  quietly replacing units, weapons or art. Shipped packs hold
  nothing but their map.
- Citation: The manual describes downloadable maps as maps.
  Behaviour note `docs/notes/2026-09-11-map-sources.md`.

## M-006: Our own thresholds for stall recovery

- Change: A unit with a live move order that has not closed 32 pixels
  of ground on its goal for 240 ticks gets a fresh search, and after
  four of those have not moved it on the order is ended as
  unreachable. The original scales every retry delay by a per def
  speed byte derived from maxvelocity (legacy:162838-162851,
  legacy:184656-184658), so a slow unit waits several times longer
  than a fast one.
- What counts as closing ground: with a route, the way still left to
  walk along it, and with none the straight line to the order point.
  Each measure is judged against the least it has ever been on this order,
  so ground closed for the first time starts the ladder over and
  ground paced over again does not. A unit with a route is judged on
  that route alone, because the straight line falls and rises on the
  way round a bay and says nothing about whether the way round is
  being walked.
- The way left to walk is not allowed to fall faster than the unit
  walked. A search run from the same spot can hand back a shorter way
  round at any time, and a shorter way found while standing still is a
  different plan, not ground closed. A longer one is taken as it
  comes, because the unit really does then have further to go.
- Why not displacement: the ladder used to measure how far the unit
  had moved from a reference point, and moved the reference whenever
  the unit left a 32 pixel circle around it. A unit that paces gets
  nowhere and resets that ladder for ever. Measured on the Athri Cay
  wander scenario: a swordsman ordered 1900 px across the map paced
  776 px in 3420 ticks, closed none of it, reset the ladder four times
  and never reached the rung that ends an order. Covering ground is
  not making progress.
- Cost: the way left to walk is wanted every tick for every unit under
  orders, and a route runs to ninety six waypoints, so all of it past
  the waypoint being walked is summed once per waypoint per plan and
  kept. On the ffa probe with three hundred units, over the engine tick
  with the route search taken out, summing it every tick costs 0.26 ms
  a tick and keeping it costs 0.03.
- Measured after the change, over the soak's twenty one scenarios: the
  longest any unit held a live order without closing ground fell from
  3420 ticks to 1237, and every offender's ladder now climbs all four
  rungs instead of stopping at one. The monarch still walks the whole
  way round the bay in the band fixture, arriving at tick 2257 to
  2950, which is twice the ladder's own length and is what the route
  measure is there to allow.
- Why: The absolute constant in that formula was lost by the tooling at
  legacy:162841-162842, so the durations cannot be read off the
  reference and parity on them cannot be claimed. Flat integers are
  honest until someone times a stuck unit in the retail build against
  two units of known maxvelocity. They are integers in the simulation
  hash, so they are deterministic and safe for lockstep.
- Open: whether a genuinely unreachable goal should end the order at
  all. In the original the 0x1000 and 0x2000 search status bits are
  inert and no branch completes a mission on a failed search, so the
  order appears to persist for ever, but the absence of a terminating
  branch was not proved. We end it, because issue #60 asks that a unit
  never sit stuck for good and ours already completes an order in
  several near miss cases. An owner parity call, not one the reference
  settles.
- Also: where the way is blocked by another unit and the corridor is
  too narrow to pass it, there is no other route to take and the order
  is ended. The original does not command the blocker to move either:
  legacy:191265-191388 re-runs the search on a delay scaled by that
  same speed byte and never touches the unit in the way. Making a
  blocking unit stand aside would be a new behaviour, not parity, and
  is what issue #60's "or another unit" clause still wants.
- Measured: on the reported band fixture the ladder is what turns the
  one case a route cannot serve, a monarch bracketed by two of his own
  on a 48 pixel band, from nine thousand ticks of grinding into an
  order that ends. Every other case in that fixture is served by the
  route search alone and never reaches the first rung, so the ladder
  is a safety net and is meant to be one. The data free case is
  a_unit_that_covers_ground_without_closing_on_its_goal_gives_up in
  test_movement: a unit pacing a walled pocket for 2023 px with the
  way out facing away from its goal, which the old ladder never gave
  up on and this one ends at tick 1592.
- Citation: Issue #60. Manual is silent.

---

## M-007: A diagonal first step is fragile in a packed block

- Change: None. This records a measured consequence of planning on
  terrain alone, so that the next person to meet it does not read it as
  a fresh bug.
- What happens: the route search may make the first step out of a cell
  a diagonal one. A diagonal step may not cut a corner, so the mover
  needs BOTH orthogonal neighbours of that diagonal to be clear, while
  a straight first step needs one cell. In open ground the difference
  costs nothing. Inside a block of parked units it is the difference
  between leaving and not leaving, because the two cells a diagonal
  needs are held by two different units and neither has a reason to
  move first.
- Measured: the sixty unit squad in live_skirmish_units_actually_move.
  One unit begins at 1096,4440 boxed on all four sides, with friends at
  (0,-40), (-40,0), (+40,0) and (0,+40). Given a first waypoint one
  cell straight north it follows the column out: it clears that
  waypoint at tick 90 and has no block flag left by tick 120. Given a
  first waypoint one cell diagonally north east instead, it needs the
  unit to its north and the unit to its east to move together, wedges
  in the corner between them at 1119,4432 with UNIT_ROUTE_BLOCKED_HARD
  and speed down to 0.11, and is still there 900 ticks later having
  covered 47 px, its route thrashing between 4, 0, 12, 16 and 18
  points.
- Why it is not fixed here: the planner plans on terrain and has no
  knowledge of the crowd, which is the design rather than an oversight.
  Issue #60 defers crowd avoidance, flow fields for groups moving to
  one place and local collision avoidance to a later stage, and this is
  exactly that work. The alternative on offer was to restore an older,
  stricter per cell test so the route shape changed by accident, which
  would undo a correctness fix and force the accuracy claim in M-004 to
  be withdrawn in order to paper over a crowd artefact.
- Not a fixture artefact: the arrangement is tighter in real play than
  in the test. Twenty four units ordered to a single point and left to
  settle for 3600 ticks (soak_legitimate_waits) come to rest at a
  nearest neighbour distance of 17 px minimum, 23 px mean and 35 px
  maximum, with all 24 closer than the 40 px the fixture spawns at.
  Selecting a group that has just arrived and sending it somewhere else
  is ordinary play, and that group is packed tighter than anything this
  test builds by hand.
- Citation: Issue #60, which defers the crowd layer. Manual is silent.

---

## D-006: Chat messages expire on the wall clock

- Change: A chat message leaves the message list when it has been on
  screen for (Text Delay + 1) seconds of wall clock, measured off the
  frame timer. The original counts 30 Hz simulation ticks and drops the
  oldest entry when `entryTick + (TextScrollTime + 1) * 30` falls
  behind the current tick (legacy:205907-205910).
- Why: Counting ticks would make the chat list a reader of simulation
  state, and the standing rule is that nothing about chat touches the
  deterministic simulation. Our tick rate is 60 Hz rather than 30 Hz
  (D-001), so the tick count would have needed converting anyway. Wall
  clock gives the same 1 to 21 seconds a player sees.
- Citation: Manual §I Game Options describes Text Delay in seconds.

## D-007: Chat options ship on, at 5 and at 8

- Change: `ChatLevel` (Unit Chat On) ships on, `TextScrollTime` (Text
  Delay) at 5 and `MaxTextLines` (Text Lines) at 8, in `options.cfg`
  under the original's own key names (legacy:131691-131696).
- Why: The original's factory values cannot be read out of the
  behaviour reference, because its Set Defaults goes through a virtual
  call the reference does not resolve. Two of the three settings have a
  value that makes chat look broken rather than off: Text Lines at 0
  stores and draws nothing at all, and Unit Chat On off hides your own
  line, since a local copy is stored as your own message and only a
  received one bypasses that filter (legacy:206003-206004). Picking
  visible values is the safer guess.
- Citation: Manual §I Game Options lists all three sliders and the
  checkbox but gives no factory values.

## D-008: A console command line reports instead of running

- Change: A chat line whose first non space character is `+` is not
  sent and does not run. The console answers with a local notice that
  console commands are not in yet. Every other line is chat, exactly as
  the original has it (legacy:154470).
- Why: The original's `+` interpreter binary searches a table of about
  sixty handlers behind a permission mask, and the same interpreter
  backs the in game key bindings. It is not a chat feature. Swallowing
  the line keeps the command surface closed rather than broadcasting
  `+kill` to the other players as ordinary chat.
- Citation: Manual §I Game Options is silent on console commands.

*(More entries added as deviations land.)*
