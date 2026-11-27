# Manual Deviations

Running log of intentional departures from the gameplay rules in
`Total_Annihilation_Kingdoms_Manual.pdf`. The default is faithful. A
deviation needs a recorded reason.

Format per entry, in a few lines. Measurements and history go in the
pull request, not here.
- Change: what departs from the original, and the switch that turns it
  off when there is one
- Why
- Citation: legacy anchors, the manual (page, section), behaviour
  notes, source files and tests

---

## D-001: 60 Hz simulation tick (vs TAK's 30 Hz)

- Change: The simulation runs at a fixed 60 Hz instead of 30 Hz, so each
  tick applies half the original's per frame step. A SLEEP of n
  milliseconds lasts 30n/1000 of the original's frames, rounded down and
  at least one, at two ticks a frame, and the thread runs again on the
  last of those ticks.
- Why: Smoother animation on modern displays, with room for lockstep at
  a higher rate. Speeds are units a second, so real time play is
  unchanged.
- Citation: The manual is silent on tick rate. Its rules describe wall
  clock seconds through the Game Speed slider (§I).

## D-002: Unit limit of 2000 per player

- Change: The per-player unit cap ranges 200 to 2000 with a default of
  2000 (TAK_UNITS_PER_SIDE_MIN/MAX/DEFAULT in
  `include/tak_battle_config.h`).
- Why: The original's cap of 500 came from 1999 memory and CPU limits.
  Large late-game armies are part of the appeal.
- Citation: The original's setup screen slider ran 20 to 500 (the
  "maxunits" summary field).

## C-001: Frustum culling

- Change: Units off screen skip vertex transform and submit.
- Why: Performance on larger modern viewports, where a big map would
  otherwise submit work for every unit however few are on camera. Fog of
  war and visibility are player facing and unchanged.
- Citation: Manual §IV.2 Battle Map ("you have to explore it with your
  units to reveal the battlefield") implies all on screen rendering.

## C-002: Modern resolution support

- Change: The game renders at the window's native resolution with no
  upper limit. The default Original scale draws one game pixel to one
  screen pixel at any size from 640x480 up, with the sidebar, minimap
  and bottom strip placed as the original places them. The Resolution
  slider on the Visual page lists the display's sizes plus Fit, which
  stretches the 640x480 battle screen over the window, and in fullscreen
  a size below the desktop's is a real mode change, as in the original.
  In a browser the sizes are one to four screen pixels per game pixel,
  by default the page's devicePixelRatio rounded. A new install starts
  on Original, an options file from before the setting keeps Fit, and on
  Windows a switch from Fit to Original takes effect from the next
  start.
- Why: The 1999 hardware concerns are moot and unit visibility is the
  same at any resolution.
- Citation: Manual §I.6 ("Make sure the Resolution slider is set to
  640x480 in the Visual settings of Game Options. Higher resolutions
  require considerably more system resources.").

## R-001: Hardware-accelerated rendering

- Change: The scene is drawn through the GPU (SDL2, textured triangles)
  instead of the original's software 8-bit palettised rasteriser, at any
  window size.
- Why: Modern displays and drivers have no 8-bit modes any more. The
  projection, draw order, lighting tables and palette effects reproduce
  the original's output.
- Citation: Manual §I.6 on resolution and video settings.

## N-001 (planned): Deterministic lockstep multiplayer

- Change: Networking is deterministic lockstep over a relay server that
  owns the turn clock, one WebSocket to every client, browser and native
  alike, instead of the original's peer authoritative DirectPlay
  sessions. Only player commands cross the wire, and the relay holds no
  simulation and no game state. Playing against the original on
  GameRanger is not a goal.
- Why: DirectPlay is gone, does not traverse NAT and cannot run in a
  browser, and a star relay reaches every home network with no port
  forwarding. A mixed game could not be lockstep, since the original's
  peers never send orders or wait for a turn.
- Citation: Manual section on multiplayer setup (DirectPlay/TCP-IP
  options). Design in `docs/MULTIPLAYER.md`.

## N-002 (planned): The host role moves when the host leaves

- Change: If the host leaves the battle room or the game, the occupied
  human seat with the lowest seat number becomes host, and the room
  closes only when no human seat is left. The server decides and
  broadcasts it, so no two clients disagree about who is host.
- Why: The original ended the lobby when its creator left, losing a room
  full of people to one dropped connection, and in game handed the flag
  to the human with the highest network id, which two machines could
  resolve differently when a computer player held it.
- Citation: Manual section on the multiplayer battle room, which
  describes the host's controls but not what happens when the host
  leaves.

## N-003 (planned): Computer players outlive whoever added them

- Change: A computer player added to an empty seat stays in the game
  when the player who added it leaves, and the host sets its side,
  colour and team, where the original left them to the machine that
  added it.
- Why: In the original a computer player belonged to one machine and
  left with it. Under lockstep every client simulates every seat from
  the same seed, so no machine owns a computer seat, and an unrelated
  dropped connection should not remove an army.
- Citation: Manual section on adding computer players in a multiplayer
  game.

## N-004 (planned): Pause and game speed are server pacing

- Change: Pause and the speed level are not simulation state. The server
  produces turns faster, slower or not at all, no client applies either
  to its own simulation, and neither enters the state hash.
- Why: The original already computed its tick count as elapsed time
  times the speed level (legacy:242330-242421), and this moves that to
  the one place that owns time, so clients that disagree about speed run
  at different rates without diverging.
- Citation: Manual section I on the Game Speed slider.

## N-005 (planned): Alliance and sharing changes reach everyone

- Change: An alliance offer, an alliance break and the three sharing
  toggles are commands in the turn stream, applied on the same tick by
  every client, so every player learns of them.
- Why: The original told only the affected player, but lockstep must
  apply a rule that changes the simulation identically everywhere, and
  it ends the original's ambiguity about who was allied with whom.
- Citation: Manual section on the Diplomacy screen.

## N-006 (planned): Game passwords are checked by the server

- Change: A room password is held and compared by the server, in
  constant time, and a wrong password is refused before any room state
  is sent.
- Why: The original compared a 16 bit hash inside the joining player's
  own client (legacy:193511-193597) and gave the session itself no
  password, so a modified client walked straight in.
- Citation: Manual section on hosting a game, which describes the
  password field on the host screen.

## N-007 (planned): Ready clears when the map or an option changes

- Change: When the host changes the map, a rule option or the unit cap,
  every player's Ready is cleared and has to be given again.
- Why: The original cleared your own Ready when you edited your own row
  (legacy:135767-135790) but not on the host's changes, which let a host
  swap the map under eight ready players.
- Citation: Manual section on the multiplayer battle room and the Go
  indicator.

## N-008 (planned): Spectators sit outside the eight seats

- Change: A watcher does not occupy one of the eight seats. A room holds
  up to eight players and up to eight watchers, a defeated player may
  stay and watch without a seat, and the host may still refuse watchers,
  as in the original.
- Why: In the original every participant, watchers included, was a
  network player under the cap of eight (legacy:192826, legacy:268685),
  so onlookers cost players their places. Our seats are simulation slots
  and a watcher has none.
- Citation: Manual section on multiplayer setup, which counts up to
  eight players in a game.

## N-009: The Boneyards button reaches the server this build came from

- Change: The Select Game screen keeps the original's button and typed
  address, but the button connects to wherever this build came from, the
  page's own origin in a browser, rather than to Boneyards. A desktop
  build has nothing to guess from, so the button says so and the player
  types an address. A game this build cannot join is listed and marked,
  with the reason shown when it is clicked, where the original hid a
  mismatched session.
- Why: Boneyards was Cavedog's own lobby service and it is gone, and the
  closest honest reading of the button is the server the player already
  reached. No host name is written in the source, where it would outlive
  the deployment.
- Citation: Manual section on multiplayer setup, which describes
  connecting through Boneyards or by address.

## N-010: Co-op against the computer, with drop in seats

- Change: A room hosted by this build lets players drop in, and the host
  may fill the empty seats with computer players and start alone. A
  player who joins the running match replays the turn log from turn 0
  and then takes over a computer seat at a turn the relay sets, on the
  same tick on every machine. A player who leaves a seat the computer
  began in, or leaves while a human teammate plays on, hands the army to
  the computer and opens the seat to the next player. Anyone else who
  leaves loses the army as in the original, so a versus game a player
  walks out of ends with the one who stayed recorded as the winner. A
  room of an older host, or one whose host did not ask for drop in,
  keeps its seats shut once the match is under way.
- Why: The original would not start a network game with one human in it
  and fixed every seat once the game began. In lockstep a seat is a
  simulation slot, and who drives it can change at a turn boundary on
  every machine at once.
- Citation: Manual section on multiplayer setup, which asks for a second
  human before a game can start. Issue #292. The army lost on a leave is
  the original's drop (docs/notes/2026-09-04-multiplayer-hapinet.md,
  legacy:194242).

## D-003: Raising the dead always works at full supply

- Change: A resurrection or animation takes one frame's work per frame
  whatever the player's mana, where the original scales each frame's
  work by the player's economy supply fraction, the figure that slows a
  starved build (legacy:13087, legacy:39469).
- Why: The economy has no such per player figure yet, and raising
  charges nothing in the original, so the only effect is that a starved
  player raises at full speed. Revisit when the supply fraction lands.
- Citation: The manual is silent. Behaviour note
  `docs/notes/features.md`.

## D-004: No underwater corpse sink

- Change: A corpse that lands below sea level is placed like any other
  and rots on the normal schedule, where the original sinks it straight
  away (legacy:227440-227450).
- Why: There is no water rendering or water gameplay yet, and bodies in
  water are rare on land maps. The sink can land with water.
- Citation: The manual is silent. Behaviour note
  `docs/notes/features.md`.

## M-001: Route following near obstacles

- Change: With something held in a neighbouring cell, a route point
  counts as passed once the unit is within 16 px of it, where the
  original passes it only when the unit stands on it, within a few
  pixels (legacy:191282-191284). Away from obstacles the original's
  32 px radius and cross track rules apply unchanged
  (legacy:184729-184791).
- Why: The original's route points are exact unit origins on 16 px
  cells. Ours plans on 32 px cells, so landing on the point would turn a
  unit back for points it has effectively passed.
- Citation: `docs/notes/pathing.md`. The manual is silent on route
  following.

## M-002: Replan timing when a unit is in the way

- Change: A unit pinned against a parked unit (M-003) plans a new route
  after 10 frames without headway, and behind a unit still on the move
  it first waits two seconds plus a small fixed per unit offset. A route
  that makes no progress toward its target for 2.5 seconds is dropped
  and planned again. The original searches again on every hard blocked
  frame (legacy:191290, legacy:191387), and without a hard block replans
  after a randomised delay scaled by the unit's speed class
  (legacy:191300-191360) or at random after 120 frames
  (legacy:191375-191385).
- Why: The plan ignores units on the move, so searching again behind one
  returns the same route, and a fixed delay keeps the lockstep
  simulation free of an extra random draw per blocked unit. No unit
  should sit stuck for good against a wall or a crowd, and the stall
  watchdog covers routes the original would have followed into a pocket.
- Citation: `docs/notes/pathing.md`.

## M-003: Units that hold their cells become planning obstacles

- Change: A ground unit that has held its cells for 10 frames, or for as
  long as a cell takes at three quarters of its top speed if that is
  longer (at most a second), is tagged in the occupancy layer, idle or
  jammed alike, and other units plan around it. The original folds any
  unit into its search grid at 10 frames with a slowed rim, closes it at
  150 frames (legacy:188900-188960, legacy:187765-187880), and checks
  far units live, letting through one that keeps three quarters of the
  searcher's top speed (legacy:21986-22032, legacy:184497-184510).
- Why: Our plan is a yes or no answer per cell, so one rule stands in
  for all of that, and a jam has to be visible to the next search or a
  column behind a stuck unit plans into it again and again.
- Citation: `docs/notes/pathing.md`.

## M-004: Clearance grid instead of per cell placement checks

- Change: The planner keeps a clearance map per move class, the largest
  square footprint that fits at every 16 px tile, and plans a unit only
  through cells whose clearance covers its footprint, where the original
  runs its footprint placement check per cell during the search
  (legacy:187701-187930 over legacy:219074-219179). A 32 px cell holds
  four tile aligned placements of a footprint, each swept as the
  original sweeps one (legacy:219089-219131), and is open when any is
  legal. A step between cells must slide the footprint one tile from a
  legal placement to a legal placement, so a route is a chain of
  placements and is laid along them. A unit whose body fits no placement
  of its cell, or can reach none in a line, is walked out to one first,
  as is a unit whose first leg the step would refuse or whose search
  runs out within 64 cells in a pocket, and a tile of a walk out in a
  cell with no legal placement costs half of PATH_PINCH_COST. A body
  crossing a tile corner must also pass the bodies either side of it,
  which is our own rule.
- Why: The same answer for a shipped unit, computed once per map change
  instead of per search node, and the base for the hierarchical search,
  flow fields and local avoidance in the design note. A cell open by one
  placement is not open from every neighbour, so steps are judged
  placement to placement and every point the mover walks to is one the
  footprint fits. The original anchors its sweep on the unit's position
  rounded to the tile grid (legacy:184166-184186), so it has no 32 px
  cell to keep in step with, and its rule for corners is not
  established.
- Citation: `docs/notes/pathing.md`. A test asserts the passability
  bitmap and the clearance map answer alike for every cell of a shipped
  map with nothing built on it.

## M-005: Melee reach counts to the body

- Change: A melee weapon also reaches a walker whose tiles touch the
  attacker's, whatever the centre distance, and otherwise keeps the
  original's centre distance to a mobile target (legacy:235291). A route
  toward a unit ends on that unit's own cell even when it is parked
  there.
- Why: Walkers hold their cells here (M-003) on a 16 px grid, so two
  units in contact stand 17 to 47 px apart centre to centre, and by
  centre distance a 30 px sword could stand against an enemy and never
  strike.
- Citation: `src/ui/test_ui_screens.c`,
  `swordsman_strikes_an_enemy_standing_beside_it`.

## A-001: AI base defence recall

- Change: When an enemy hits anything within 1280 px of an AI player's
  start, every combat unit of that AI within 1536 px of the start goes
  at the attacker, attacking it when the AI can see it and marching to
  where the shots came from when it cannot, until ten seconds pass
  without a hit. An idle unit at home does the same for an allied base,
  a human teammate's included, and waves further out keep going.
- Why: The original has no recall. Its home units engage what enters
  their engagement radius, so a raid at the edge of the base can go
  unanswered while the army is away. Its rule that a hit on the monarch
  holds its own construction for 1 to 31 seconds (legacy:15092) is kept
  alongside.
- Citation: The manual describes no AI defence rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Hits on an AI".

## A-002: AI influence maps tilt the target and call the defence

- Change: Each AI keeps a coarse grid, one cell per 256 px, of its own
  and allied combat value and assets, the enemy's it can see, and sacred
  sites and lodestones. The original's target score (legacy:15365) is
  multiplied by a quarter to four times by how weak and valuable the
  enemy is in the candidate's cell. A cell where seen enemy strength
  outweighs the AI's presence and something of the AI's stands, other
  than an unarmed mover that builds nothing, sends the home units at the
  first seen enemy in it, as a hit on the base would.
- Why: The original scores by distance and chance alone and reacts only
  once a unit is hit. Per unit values keep its formula (legacy:19803),
  and the grid is its summed threat within a radius (legacy:20857),
  binned.
- Citation: The manual describes no AI rule for either. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Target choice".

## A-003: AI builds and attacks by goal planning

- Change: Each AI tick reads an abstract state (mana and its cap, income
  against spend, lodestones, factories, idle builders, army at home and
  away, threat and exposure off the influence maps, free sacred sites, a
  known wave target) and scores five goals: hold a lodestone count,
  defend home, keep an army sized to the threat, secure a nearby site,
  kill the weakest enemy. The goal planner of A-007 finds the cheapest
  sequence of actions for the top goal, costs being mana scaled by the
  profile weight and counts capped by the profile limits, and its first
  step is what the idle builder, factory or army does this tick. A
  walking builder fighting on its own account counts as idle, a
  lodestone is planned only for a pad that can take it now, and a
  factory still trains by the original's weighted draw.
- Why: The original walks a fixed order and never looks at the map. Its
  gates are kept: a mana building under 30 percent mana or in a stall,
  one at a time (legacy:19859), no structure while the pool covers less
  than 70 percent of what standing frames ask for (legacy:17201,
  legacy:235975), and no training under 7/30 of that measure
  (legacy:17991).
- Citation: The manual describes no AI build rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Build decision".

## A-004: AI passes by a site with no route and remembers a failed one

- Change: The AI asks the pathfinder for a way from the builder to a
  site before it orders a build, as the original does (legacy:17327),
  but where the original then builds nothing that pass, ours passes the
  site by and goes on round its fallback ring (A-014), up to twelve
  route checks, skipping any spot within 48 px of one that just failed.
  A seat also remembers up to sixteen sites for a minute each, the site
  of a build its builder's mover gave up on and a sacred pad its builder
  had no way to. Nothing is sited within 128 px of one, and a remembered
  pad is not counted as free.
- Why: A fixed ring offers the same first site every pass, so without
  the memory a builder goes back to the spot it just failed at, and
  without going on round the ring a builder beside a wall never builds.
- Citation: The manual describes no AI placement rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Placement".

## A-005: AI waves go only where the army can walk

- Change: The wave pick takes a candidate only when a walker of the
  seat, one per movement class and up to four classes, stands on ground
  that reaches it, read off a label of the class's connected ground
  (`TAK_PathGroundConnected`). The label is terrain alone, so an enemy
  walled in by its own buildings still counts as reachable and the march
  settles the last few cells. The march leaves behind each walker whose
  ground does not reach the target, and with nothing reachable the seat
  keeps the best of the rest, sends its flyers at it and leaves every
  walker where it is.
- Why: The original asks the pathfinder before it takes a target
  (legacy:15473) and before a group marches (legacy:18385), a search per
  candidate for one group, where we ask once for a seat over every enemy
  alive. It holds one target per group, and its flying group asks for
  itself and is told yes.
- Citation: The manual describes no AI targeting rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Target choice" and
  "Water and transports". Issue #232.

## A-006: An attack goal decomposed into scout, mass, strike and hold

- Change: A tactical layer runs the wave the goal planner calls for, off
  one wave state read every tick from the unit list and the fog. A wave
  waits at home for a third of what the seat fields, at least one and at
  most eight, and at strength every waiting member is sent on the same
  tick. Short of strength, with the target unseen and no member out, the
  member nearest the target goes to look, never a gathering's only
  member and one march at a time. A wave that never reaches strength
  goes anyway every thirty seconds, and a member away from home goes on
  with it.
- Why: A wave sent unit by unit feeds a defended base a unit at a time.
  The original collects spare ground units into a group that launches at
  a threshold and disbands at or below it (legacy:16187 forms the group
  and ratchets its thresholds on what the seat owns, legacy:18250
  launches or disbands it). The launch count is that ratchet in one
  line, and the thirty seconds stand in for its reschedule of a group
  with no target. The scout is ours, where the original lets an idle
  raid group wander to a random map edge (legacy:18632).
- Citation: The manual describes no AI wave rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Attack waves".

## A-007: The AI plans with a goal planner and a task network, and its army weighs strength

- Change: The two planning layers are engines with the AI's behaviour as
  data: goal oriented action planning in `src/game/ai_goap.c`, a best
  first search on cost for the cheapest plan of up to five steps, and a
  hierarchical task network in `src/game/ai_tasknet.c`, taken apart
  depth first with backtracking. The strategy of A-003 is a domain in
  the first (`src/game/ai_plan.c`) and the wave of A-006 a domain in the
  second (`src/game/ai_htn.c`). A seat that cannot reach its whole
  economy goal takes the plan that ends nearest to it. An army strikes
  at strength in numbers only when it is a fifth stronger than what it
  sees round the target, out of patience on less unless it is
  outnumbered two to one, and every ninety seconds whatever waits,
  believing what it saw for a minute after the fog closes. Members in
  the field facing more than twice their strength are ordered home, and
  a gathering held back by strength sends two of its fastest at the
  softest enemy building it can see.
- Why: Issue #60 asks for planning rather than reaction. The original's
  group launches on a randomised fraction of its size and asks nothing
  about what it walks into (legacy:16187, legacy:18250), ejects a member
  under a third to a fifth of its hit points rather than recalling a
  losing group (legacy:18364), and marches raid groups to a random map edge
  (legacy:18632). Its expansion weighs threat (legacy:16503), and the
  strength test applies that to the wave. The long wait stands in for
  its mass attack countdown (legacy:18880-18895).
- Citation: The manual describes no AI rule at this level. Behaviour
  note `docs/notes/2026-09-04-legacy-ai.md`, "Attack waves".

## A-008: The AI does not expand under the enemy's feet

- Change: A sacred site where the seat can see more enemy strength than
  its own, read off the influence maps of A-002, is passed by for the
  next one and not counted as free while that lasts.
- Why: The original counts the enemies near a site and takes it only
  while three times that count is within what the caller allows
  (legacy:16680-16686). Ours weighs strength, which the seat already
  keeps, because a count would call one dragon and one swordsman the
  same threat.
- Citation: The manual describes no AI expansion rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Attack waves", the spare
  builders paragraph.

## A-009: The AI's fighters go out in numbered groups, and its towers face the threat

- Change: A seat's spare fighters, idle at home, in no group and fit to
  fight, gather into numbered groups, up to four a seat, odd from 21 for
  an attack group and odd from 51 for a raid (A-007). A group keeps the
  target the seat held when it went out, so two groups can be at two
  places, and each is asked for itself whether it breaks off (A-007). A
  member whose hit points fall under its maximum over `rand(3) + 3`,
  drawn each time, leaves the group and comes home (legacy:18364), and a
  group down to a third of what it went out with is spent and comes
  home. While a member's distance squared from its group's mean is at
  least the member count times 200000, or 500000 for a raid, the
  furthest comes home (legacy:15845-15927, legacy:18285, legacy:19426,
  legacy:19450). One pass in ten the spare fighters join the nearest
  attack group below the count it went out with (legacy:16083-16120,
  legacy:16335-16346). A walking builder under a quarter of its hit
  points, or an eighth when it carries a weapon it can pay for, either
  doubled when its mana is half gone, drops its build with enemy
  firepower near and makes for a point near home (legacy:17205-17231,
  legacy:17458). A tower goes up between home and what threatens it, at
  most 384 px out and at least 160 px from the seat's other towers.
- Why: The groups are ports of the original's (legacy:16187,
  legacy:18250), except that ours launch on the third of A-006 rather
  than a draw on the group's size, draw the point near home in a square
  rather than within 320 of the centre, send a straggler home rather
  than to the parent group, and reinforce attack groups only, the
  original's support group units being raiders here. The tower rule is
  ours, since nothing in the original calls its query for defences near
  a point (legacy:20985) and its towers go where any structure goes
  (legacy:20311).
- Citation: The manual describes no AI rule at this level. Behaviour
  note `docs/notes/2026-09-04-legacy-ai.md`, "Groups",
  "Unit, factory and builder think" and "Attack waves".

## A-010: A squad on the march keeps in step

- Change: A member of an attack group out in the field (A-009), more
  than 480 px from the target and nearer it than the group's mean by
  more than 256 px, stands until the rest are within 128 px of that, and
  within 480 px every member goes on at the target as the original's do.
  It is the tactic bit `TAK_AI_TACTIC_SQUAD`.
- Why: Issue #60 asks for potential fields so squads keep formation, and
  the original sends a group's members at the target with nothing more
  than ejecting the straggler (A-009).
- Citation: The manual describes no AI rule at this level. Issue #60.
  The math is in `src/game/ai_squad.c`.

## A-011: Under the remastered rules the computer sweeps the rubble its units are stuck at and steps out of fire

- Change: With the remastered battlefield rules on (D-036), an idle
  builder of a computer seat that can sweep clears rubble that blocks
  until swept within 128 px of a unit of the seat whose order's route
  failed or whose stall ladder has begun, the nearest piece to each
  builder and one builder to each piece. The sweep is a goal planner row
  ranked at 80, below a threat at home and a stalled economy and above
  building an army, attacking and expanding. A walking unit of the seat
  whose model is within 32 px of a burning feature's centre first walks
  clear of every fire, trying eight ways at steps of 24 px out to six
  steps, each weighing four for every fire it passes, one a step and one
  for every 24 px given up toward where the unit is bound, and takes the
  lightest unless it is already walking somewhere clear. With the rules
  off neither happens. The tactic bits are `TAK_AI_TACTIC_SWEEP` and
  `TAK_AI_TACTIC_FIRE`.
- Why: The original has neither rubble that blocks nor fire that hurts,
  so it has no behaviour for them. A computer that only routes round
  what blocks is stuck for good where no route is left, and one that
  ignores fire stands in it.
- Citation: The manual describes no rule for either. D-036 makes the
  conditions they answer. The code is `ai_find_rubble`, `ai_try_sweep`
  and `ai_step_out_of_fire` in `src/game/ai.c`, the sweep row in
  `src/game/ai_plan.c` and the footing in `src/game/ai_htn.c`.

## A-012: The computer settles every free pad under cover, holds the ground it wins and builds a varied army

- Change: Three tactic bits. `TAK_AI_TACTIC_EXPAND`: a seat wants a
  lodestone on every free sacred pad it can safely take, up to the
  profile's limit. A builder bound for a pad past the base's radius
  takes the fighters gathering at home as an escort, which stands 160 px
  past the pad from home until the work ends, and a builder out past
  that radius puts its next hall up at home. `TAK_AI_TACTIC_HOLD`: a
  group whose target falls or whose escort ends picks its next target
  from where it stands, presses on when it can see that target and is a
  fifth stronger there or when the long wait of A-007 is up, and
  otherwise holds the ground it won and draws the reinforcements of
  A-009, as what is left of a spent group does on quiet ground away from
  home. A holding group answers a threat within 1536 px of home, a fit
  fighter idle in the field joins a group near it, and a wave still
  nearer home than its target turns back when home is hit.
  `TAK_AI_TACTIC_MIX`: a fighter the profile names no weight for weighs
  the mean of the named fighters in its build list.
- Why: The original sends a spare builder with every spare ground and
  air unit of the pass as escort to the nearest safe sacred site, up to
  the profile's limit (legacy:16503, legacy:16320-16328), but from roles
  that pass over the commander key (legacy:15528-15545, the key read at
  legacy:163074), and ours may send the monarch because Zhon's other
  builders summon before they build. Its group whose target dies picks
  the next from its own position (legacy:18293, legacy:18343), and one
  with no target waits (legacy:18350). It weighs a type nobody named at
  100 (legacy:19792), and the shipped profile (ai/default.txt in the
  patch and Iron Plague) names no weight for the Centaur the patch added
  to Veruna's castle, whose other troops weigh 3 to 8. The strength
  test, the hold, the regroup, the turn back, the answer to a threat,
  the halls at home and the peer weight are ours.
- Citation: The manual describes no AI rule at this level. Behaviour
  note `docs/notes/2026-09-04-legacy-ai.md`, "Roles" and "Attack waves".
  The code is `ai_group_next`, `ai_escort` and `ai_profile_peers` in
  `src/game/ai.c` and the group domain in `src/game/ai_htn.c`.

## A-013: A computer cut off by water carries its army over

- Change: Under `TAK_AI_TACTIC_NAVAL` a seat is cut off when no walker
  of any movement class could walk from home to its wave target, ships
  never asked. A cut off seat wants a transport before more army,
  trained or from a yard it puts up on water near home that reaches the
  target's, where open water 320 px across lies past the pad, and with
  none fit to sail and none to make it strikes as before. Its walkers
  wait for a transport while its flyers strike alone, and board once a
  full hold, or all the seat has, carries the strength a strike needs
  against the arms round the enemy asset the ferry goes for, or a
  launch's worth of at least half that strength once the long wait of
  A-007 is up. The transport holds its fire and lands the wave on ground
  that walks to the target, the nearest past 448 px from it and dearer
  near enemy arms, chosen for the hardest rider aboard because an unload
  ends at the first rider its spot refuses (legacy:14569-14574). A
  failed landing is tried elsewhere twice before the wave goes home, and
  a crossing lost with its transport waits three times as long before
  the next. A seat runs one ferry and one more for each 30 walkers
  waiting at home, three at most (`TAK_AI_TACTIC_CONVOY`), and may keep
  eight groups rather than four. Transports join no group and wait at
  home or a berth between crossings, and one too hurt to sail or
  stranded on a lake is replaced. A walker takes no target that no
  ground it can walk puts in reach, as the original takes no target it
  has no route to (legacy:15461-15473), a melee weapon counting at the
  40 px it lands at.
- Why: Issue #454: on an island start the computer never attacked a
  player on another landmass. The original's skirmish AI never loads a
  transport, never takes a candidate the pathfinder cannot reach
  (legacy:15473) and does not march a group whose leader has no route
  (legacy:18385), so all of this is ours. The target choice still takes
  every enemy unit, seen or not, as the original does
  (legacy:21137-21168).
- Citation: The manual describes no AI rule at this level. Behaviour
  note `docs/notes/2026-09-04-legacy-ai.md`, "Water and transports". The
  code is the ferry section of `src/game/ai.c`, the crossing rows of the
  army domain in `src/game/ai_htn.c` and the ferry goal and its two
  actions in `src/game/ai_plan.c`. A-015 adds warships, A-016 a
  builder's ride and A-019 waders, covered crossings and safer flights.

## A-014: The computer's base keeps the original's spacing, with a fallback for small ground

- Change: A build's site comes from the original's placement
  (`ai_grid_site` in `src/game/ai.c`, `TAK_AI_TACTIC_SPACING`): thirty
  draws round home, or round a builder out past home, each set inside a
  square of the seat's grid so the footprint leaves six cells to the
  next square, nine for a type with a water depth to keep. The grid
  comes from the match's seed and the seat and adds nothing to a save,
  and a sacred pad is refused. Where the draws find nothing, ours goes
  on to a ring round the builder that takes the nearest site with a
  48 px lane to the next building, then the nearest there is, and the
  lane holds within a square too. A tower keeps its anchor from A-009. A
  unit sent home goes to a spot drawn round home, and an idle builder
  walks to one only when another walker of the seat is within 48 px.
- Why: Issue #487: a base built on the first clear site stands wall to
  wall and troops cannot get through. The original draws in a disc of
  560 px, 400 for a monarch's second of a type, 800 for a type that
  walks and 560 for a monarch's (legacy:17300-17307, legacy:17353,
  legacy:17409-17422), on squares of 14 to 23 cells by 14 to 16
  (legacy:20283, legacy:20311), and its idle units roam round home,
  560 px for a builder and 800 for a fighter (legacy:17687-17708). Its
  failed draws build nothing, which stalls a seat on a small map, an
  island or in a full base.
- Citation: The manual describes no AI placement rule. Behaviour note
  `docs/notes/2026-09-04-legacy-ai.md`, "Placement".

## A-015: A computer cut off by water trains the warships it needs and fights at sea

- Change: Under `TAK_AI_TACTIC_FLEET` a cut off seat (A-013) with a boat
  wants two armed ships more than it sees of the enemy's, eight at most,
  and its yards that make only ships train for that and the ferry alone.
  The largest holds are kept for crossings, as many as the walkers at
  home call for, and the other armed ships are a fleet run by a table in
  `src/game/ai_htn.c`: it answers enemy ships off home, goes at the
  nearest enemy ships when two or more and a fifth stronger, shells the
  nearest enemy building in gun range of water when two or more and no
  enemy ships are seen, and otherwise waits at its berths. A seat that
  loses every armed ship leaves the sea for five minutes, and one with
  four walkers ashore on ground that walks to its target trains no more
  warships while they last.
- Why: Issue #454 asks for ships first on a map that is mostly water and
  a return to land play once the sea is lost. The original's skirmish AI
  draws ships by weight like anything else and has no rule for the sea
  (legacy:20033-20041).
- Citation: The manual describes no AI rule at this level.

## A-016: A builder rides a free transport to a pad it cannot reach quickly

- Change: Under `TAK_AI_TACTIC_RIDE` a builder other than the monarch,
  bound for a free sacred pad it cannot walk to or that lies more than
  1536 px off, rides a free transport of the seat, and a ride to a pad
  it could walk to must save 512 px of walking. The transport sets it
  down on the nearest ground that walks to the pad. One ride a seat at a
  time, none to a pad with enemy arms near, each taking a ferry slot,
  and with no free transport the builder walks or passes the pad by.
- Why: Issue #454 names the trick of a Dark Mason on a Ghost Ship, which
  flies at 2 where the mason walks at 1.45 and crosses water. The
  original's AI never loads a transport (A-013).
- Citation: The manual describes no AI rule at this level.

## A-017: A computer's monarch keeps home when its fall ends the game

- Change: With Monarch Expendable off, under `TAK_AI_TACTIC_MONARCH`, a
  computer's monarch keeps to its own ground, within 1280 px of home and
  twice as near home as any enemy still in the game, and always the
  480 px at the heart of home. It builds out to 2048 px on the same
  terms, a pad only where no enemy is seen near it, leaves work past
  that to its other builders, and turns for home past its ground or on a
  chase that leads off it. It is in danger when the enemy it can see
  outweighs its side where it stands, when it is shot by what it cannot
  see or reach, or under half its hit points. In danger it walks home,
  or at the heart of home stays and builds and fights only there, and it
  takes a fight only in its reach, at the heart of home, or on its
  ground where its side is as strong. The rules are a table in
  `src/game/ai_htn.c`. With Monarch Expendable on the monarch plays as
  before.
- Why: A computer's monarch sent out close to enemy territory could be
  caught there and the game lost. The original never makes the monarch
  the builder of an expedition (legacy:15528-15545) and its idle monarch
  roams 400 px of home (legacy:17687-17703), and ours builds further
  because the first lodestones are the economy.
- Citation: The manual describes no AI rule at this level.

## A-018: Zhon's walking producers are paced as castles are

- Change: A computer seat starts a walking producer, a builder that
  walks and trains fighters such as Zhon's Beast Handler, Beast Tamer,
  Beast Lord and Shaman, only at a build efficiency of 0.7 and while no
  other is under construction or on its way, as the builder's structure
  pick holds a castle or a keep (legacy:17201, legacy:17271-17290).
- Why: The original takes a walking producer as a mobile pick that waits
  for nothing, and each carries `mogriumincome = 1`, so the gates on
  what earns nothing never hold one back (legacy:12127). A Beast Lord or
  a Shaman going up asks more mana a second than a seat earns, so an
  unpaced Zhon seat sat on an empty pool and never started the troops
  its walking producers train only at 0.7.
- Citation: The manual describes no AI rule at this level.

## A-019: A computer cut off by water crosses with its army and its fleet

- Change: Under `TAK_AI_TACTIC_LANDING`, on a map with water, a seat
  whose walkers reach its target only by wading, as a Swamp Beast or a
  Mer Warrior does, is cut off (A-013) while the waders are under half
  its walkers by strength, and the rest are ferried. Any other class that
  walks there leaves it not cut off, as before. An armed transport
  crosses holding its ground with its fire on, where it used to hold its
  fire, so it shoots back as it sails, and its move or unload is not
  broken off. The seat's warships of A-015 sail with a wave crossing in a
  ship, after enemy ships off home, keep within 576 px of it and go at
  enemy ships within 768 px of it. A flying transport of a cut off seat,
  carrying a wave or a builder, lands where the enemy arms at the landing
  and the most it flies over on the way cost least.
- Why: Issue #454 asks that a player on an island not be safe from the
  computer. Measured against an island seat that never crosses, Veruna
  and Zhon never crossed at all, since one Mer Warrior or Swamp Beast
  that could wade over left the seat not cut off, and half the crossings that
  did go were lost with their transport, War Galleys to the island's War
  Galleys while they held their fire and Ghost Ships shot down over the
  island on the way to a landing on its far side. The original's
  skirmish AI never loads a transport (A-013).
- Citation: The manual describes no AI rule at this level.

## T-001: Flying transports load and unload by the ground rules

- Change: The Roc and the Ghost Ship pick up and set down units the way
  ships do, closing to `transportdistance` of the rider or the drop
  point and hovering there while the hold runs. The original's air
  missions keep flying past the point between units and turn back for
  the next (legacy:27164, legacy:27700-27900).
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

- Change: The drop test centres the cargo's footprint on the point,
  which is where the unit is then put. The original tests the footprint
  from the 16 px cell that holds the point (legacy:14551) and then
  centres the unit on the point (legacy:234362-234395), so the cells it
  tests are shifted by up to half a footprint from the cells the unit
  then takes.
- Why: A point that passes the test is then one the unit can stand on.
- Citation: The manual is silent. Behaviour note
  `docs/notes/2026-09-11-transport-load-unload.md`, "Unloading".

## R-002: One shadow pass, drawn before the units

- Change: Every unit shadow of a frame, and every shot's, is drawn into
  one mask that darkens the ground once, before any unit body. The
  original draws each unit's shadow just before that unit, and a shot's
  with the shot.
- Why: Ground under two overlapping pieces or two close units darkens
  once, as a shadow does, at one target switch a frame. A shadow no
  longer falls across a unit drawn earlier, as the original's can north
  east of a tall building, nor a shot's across a unit it flies over.
- Citation: The manual describes shadows only as a Visual Options
  setting (Game Options, Visual). Behaviour anchors legacy:197182-197243
  for units and legacy:246719-246746 for shots.

## R-003: No shadow while a building goes up

- Change: A unit under construction casts no shadow until it is
  finished, where the original fades its shadow in from the halfway mark
  to half strength and turns it solid at done.
- Why: Our shadow mask carries coverage, not per unit opacity, so a
  faded shadow would need its own pass, and a building goes up in a few
  seconds.
- Citation: The manual is silent on construction visuals. Behaviour
  anchors legacy:197252-197265 and legacy:197309-197322. The unit itself
  is drawn as in the original, see
  docs/notes/2026-09-04-rendering-visuals.md, "Intangible mass".

## R-004: Shadows always drawn at full resolution

- Change: Shadows are always rasterised at the scene's resolution. The
  original has a shadow scale setting whose automatic mode drops them to
  half or quarter resolution when a frame runs long. A player who wants
  the frames back turns shadows off in Visual Options.
- Why: Modern hardware draws them at full size cheaply.
- Citation: Manual §I.6 lists the Visual settings and warns that higher
  settings cost system resources. The scale setting is in the video
  options block at legacy:197182.

## R-005: Model textures inset half a texel

- Change: A model polygon samples its texture from half a texel inside
  its atlas entry rather than from the entry's exact edges.
- Why: Every model texture shares one atlas, and a sample on the
  boundary would read the neighbouring entry. Half a texel is the
  smallest inset that cannot, and with point sampling, as in the
  original, nothing wider is needed.
- Citation: The manual is silent on texture sampling.

## R-006: Shadow sprites step from one counter

- Change: A walking unit's shadow sprite runs its frames off the
  renderer's frame counter, so every moving unit of a kind shows the
  same shadow frame, and a standing unit holds frame zero.
- Why: The original steps each unit's shadow from that unit's own sprite
  state, which we do not carry for shadows, and the difference is a blob
  of a dozen pixels under a walking soldier.
- Citation: The manual is silent. Behaviour anchor legacy:197225-197229.

## R-007: A shake moves the draw and not the camera

- Change: A shot of a weapon that authors `shakemagnitude` and
  `shakeduration` shakes the view where it lands, in full on screen and
  half off it, a second shake adding its magnitude and averaging the
  lengths. Each frame's step is put on the camera for the draw and taken
  off again, where the original leaves it on, and the steps come from
  the shake's own generator rather than the C library's. A shot's shake
  is kept beside the projectile, out of the hash and the save, so a shot
  in the air across a load lands without it.
- Why: Our camera is in a save and is read between frames by the
  pointer, the minimap and the scroll, none of which should feel a
  shake, and its own generator keeps a shake from moving the
  simulation's numbers.
- Citation: The manual does not describe the effect. The keys, the on
  screen rule, the pile up and the shrinking box are the original's
  (legacy:250066, legacy:244990-244999, legacy:120454-120470,
  legacy:120568-120594). The Acolyte, the Dragon and the god of Aramon
  author one.

## R-009: Build sparkles drawn fewer and added, with a halo

- Change: A build or raise sparkle draws added over a faint copy 1.75
  times its size, and ten in sixteen of a ring's sparkles draw.
- Why: The original shows fewer, softer stars than our full ring. The
  data names only the art, so the share and the halo come from captures.
- Citation: The ring box (legacy:198540-198551),
  docs/notes/2026-09-04-rendering-visuals.md, "Build sparkles". The
  manual is silent.

## D-005: A map pack contributes only its map

- Change: Every `.kmp` in the game folder's `Maps` folder is mounted at
  startup, and a mounted pack only answers for paths under `kmap/`, so
  anything else in a pack is ignored. The original mounts a pack only
  while its map loads (legacy:168765).
- Why: Mounting them all lets the chooser list the maps without opening
  every archive twice, and the `kmap/` guard keeps a downloaded map from
  quietly replacing units, weapons or art. Shipped packs hold nothing
  but their map.
- Citation: The manual describes downloadable maps as maps. Behaviour
  note `docs/notes/files.md`, "Where maps come from".

## M-006: Our own thresholds for stall recovery

- Change: A unit with a live move order that has not closed 32 px of
  ground on its goal for 240 ticks gets a fresh search, and after four
  such rungs the order ends as unreachable. It is kept instead while the
  ground joins the unit to its point and no stopped friend stands at its
  edge, searching once every 240 ticks unless the tick's search budget
  refused it, until 30 kept rungs, two minutes with no ground closed.
  Progress is the way left along the route, or with no route the
  straight line to the point, measured against the least it has been on
  this order, and the way left may not fall faster than the unit walked.
  Ground closed starts the ladder over. A unit whose body comes within
  32 px of a stopped friend, or a ship within its arrival reach of one,
  still ends its order, so a crowd sent to one point packs. A unit
  blocked by another in a corridor too narrow to pass keeps its order on
  a kept rung, and nothing moves the blocker. The original scales every
  retry delay by a per def speed byte derived from maxvelocity
  (legacy:162838-162851, legacy:184656-184658).
- Why: The constant in the original's formula cannot be read at
  legacy:162841-162842, so flat tick counts stand in, deterministic in
  the simulation hash. A unit that paces covers ground and gets nowhere,
  so progress is ground closed. The original seems never to end an order
  on a failed search, its 0x1000 and 0x2000 search status bits being
  inert, though that is not proved, and ours ends one because issue #60
  asks that no unit sit stuck for good. Like the original
  (legacy:191265-191388) it never moves a blocker, and making one stand
  aside would be new behaviour, which issue #60's "or another unit"
  clause still wants.
- Citation: Issue #60. The manual is silent. Test
  `a_unit_that_covers_ground_without_closing_on_its_goal_climbs_the_ladder`
  in test_movement.

## M-007: A diagonal first step is fragile in a packed block

- Change: None. A diagonal step may not cut a corner, so a diagonal
  first step out of a cell needs both orthogonal neighbours clear where
  a straight one needs one. In a block of parked units those two cells
  are held by two units and neither moves first, so the mover wedges in
  the corner with UNIT_ROUTE_BLOCKED_HARD.
- Why: the planner plans on terrain alone by design, and issue #60
  defers the crowd layer. Going back to the older, stricter per cell
  test would undo a correctness fix and the accuracy claim in M-004.
- Citation: issue #60. live_skirmish_units_actually_move shows the
  wedge, and soak_legitimate_waits shows real play packs tighter. The
  manual is silent.

## M-008: A long route is guided by measured ground distance

- Change: A plan from ground a route may stand on, whose straight line
  to the goal is 32 path cells or more, is searched with an estimate
  from measured ground distance to three fixed marks, taken once per map
  and move class. The original estimates by the straight line alone, an
  octile distance of 18 a straight step and 25 a diagonal
  (legacy:189366-189390), over one flat grid (legacy:22438-22530). The
  estimate never overstates what is left, so a route costs the same, but
  among equally cheap routes another may come back.
- Why: a browser tab runs the whole game on one thread, and a straight
  line estimate opens every cell in a growing disc on a long order.
- Citation: `docs/notes/pathing.md` and issue #60. The manual is silent
  on pathfinding.

## M-009: A frame is held while a helper walks to it

- Change: a frame nobody feeds is held while a builder sent to help it
  keeps getting nearer, or walks a planned route the mover's ladder
  (M-006) sees it gaining ground on, and decays after ten seconds of
  neither. A frame never worked on is removed rather than killed, so no
  side loses a unit. The original's frame decays at half the build rate
  on any tick no builder feeds it (legacy:9640-9644,
  legacy:39531-39534, legacy:39470).
- Why: a helper on a long walk, or on a route that leads round an
  obstacle, would otherwise find its frame gone.
- Citation: issue #229 and test_movement. docs/notes/economy.md has when
  a frame goes up. The manual describes no decay rule.

## M-010: Units give way to each other

- Change: a ground unit with a live move order pulls its aim point
  sideways when another unit within four occupancy tiles, parked or not,
  will pass closer than the two footprints plus 20 px at their closest
  approach. The pull covers the missing gap over the ground left before
  the pass, at most 45 degrees off the route, and the pair's relative
  velocity, or the line between them dead ahead, sends each to its own
  side. A pass behind the unit or under a quarter tile ahead is left
  alone, and the steer is off inside the arrival radius and for fliers.
  The route, the ground, the speed law, the step refusal and the blocked
  axis slide are unchanged.
- Why: the original has no avoidance between units, only a refused
  step, a slide at a fifth of top speed and a replan after a block
  counter (legacy:184190-184230, legacy:184244-184262), and a replan per
  jam is costly on a browser's one thread.
- Citation: issue #60 asks for optimal reciprocal collision avoidance
  between units. --perf-probe crowd --perf-ticks 3600 checks its cost.
  The manual is silent.

## M-011: A group sent to one place shares a flow field

- Change: when at least eight of a side's walkers hold a move order for
  the same point, the first route each plans for it is read off one flow
  field, the ground distance from the goal over terrain and what is
  built, with the search's own step rules and costs. It takes nothing
  from the per tick search budget and has no cap. A route dropped for a
  block or a stall is planned by a search.
- Why: the original plans every unit with a search of its own
  (legacy:22488-22535), so a group took several ticks to set out and a
  long march stopped at the search's 8192 cell cap.
- Citation: issue #60 and the cold planner test. The manual describes no
  rule for routing.

## M-012: Ships keep apart by their hulls

- Change: a ship, a unit whose move class needs a minimum water depth,
  keeps its hull off other ships' hulls. The hull is a line from bow to
  stern with half the beam round it, read from the ship's 3DO. Its
  cells, its water and the route planner are unchanged.
  - A step that brings two hulls within 2 px is refused unless it takes
    the ship no closer, a turn that swings a hull 3 px into another is
    held, and ships give way by their hulls. Two ships that close no
    ground for 8 s, one pressing on the other, go by the cells alone, as
    in the original.
  - A ship stopped short of its point by another settles within two of
    its own lengths, or behind one of its own that ended a move there.
  - A yard begins a ship once the last hull is off the pad and sends
    each to a spot it can reach, clear of the yard and other ships and
    96 px off the next ship's hull, one ahead with no hull in the way
    first. It moves its side's idle ships off the pad's water and out of
    the way of ships on the pad or bound for a point within 1280 px of
    it other than its rally point, where ships stay unless on the pad.
    The original looks again after a random 7 to 21 frames and moves
    nothing (legacy:9373-9382), and a hull the yard cannot move leaves
    the pad to its cell test (legacy:9363-9382).
  - An aura between two ships reaches from hull line to hull line, not
    centre to centre (legacy:237131-237139).
- Why: the original keeps units apart by their move class's cells alone
  (legacy:163193-163195, legacy:219329-219340), and a ship's class is a
  square one and a half to three times shorter than its hull, so fleets
  sat bow into stern.
- Citation: docs/notes/pathing.md, "Ship hulls". The manual describes no
  rule for spacing.

## M-013: A build order goes to one of the builders it names

- Change: a placing click with several builders selected gives the
  building, or the summons, to the first of them that can take its site,
  and the others keep what they were doing. A builder given the same
  order before the frame is up joins it there, where the original's
  reports the target blocked (legacy:12096-12116).
- Why: the original gives the order to every builder that can build it
  (legacy:39200-39248), and the latecomers help the first frame up. Here
  more builders are sent with a click on the frame.
- Citation: the walkthrough in manual section III sends a second builder
  with a click on the frame, which still works. docs/notes/economy.md,
  "A summons without end", has the original's rule.

## M-014: Flyers in the air count each other in pairs

- Change: once a frame each airborne flyer counts the airborne flyers
  whose footprints share a cell with its own, up to seven, and its crowd
  score moves by that count. The original holds one flyer per cell of an
  air layer, with the others on lists and the cell passing among them at
  random (legacy:218336-218512). A move also ends on its ring round the
  point as clicked, where the original first snaps the point to its
  footprint's cell lattice, at most 8 px away (legacy:25075-25083).
- Why: the pairs give the original's count for a pair and for a flock
  without an air layer or a random draw per shared cell, and the snap is
  smaller than the ring.
- Citation: docs/notes/pathing.md, "Air traffic". The manual describes
  no rule for it.

## M-015: A builder waits for units on its site in silence

- Change: a builder that finds only units on its site looks again every
  20 ticks, puts the frame up once no unit stands there and ends the
  order after 30 looks, all in silence. The cursor still refuses a
  building over a unit the player can see, and a summons is placed over
  units.
- Why: the original says it is waiting at the sixth look and that the
  target is blocked when it gives up (legacy:12088-12116), chatter the
  engine has no counterpart for yet.
- Citation: docs/notes/economy.md, "Placing a summons". The manual
  describes no rule for this.

## M-016: A new unit picks its spot off the pad once

- Change: a unit a factory finishes with no orders of the factory's to
  carry walks to the nearest clear spot in a ring round the factory, as
  the original sends it, chosen once and passing over a spot another
  unit is walking to. A flyer flies to a random bearing as far out as a
  walker goes before it looks for ground.
- Why: the original searches again when the walk is cut short
  (legacy:13813-13879), and passing over a taken spot gives two units
  their own spots without a second search. The original takes an
  unknown size term off a flyer's range (legacy:13822-13846), and half
  the factory's footprint, which the walker's ring uses, would set a
  flyer down on its own pad.
- Citation: the getbuilt mission that sends the unit off,
  legacy:9694-9711. The manual describes no rule for this.

## D-006: Chat messages expire on the wall clock

- Change: A chat message leaves the list after (Text Delay + 1) seconds
  of wall clock. The original counts 30 Hz ticks and drops the oldest
  entry when `entryTick + (TextScrollTime + 1) * 30` falls behind the
  current tick (legacy:205907-205910).
- Why: chat may not touch the deterministic simulation, and our tick is
  60 Hz (D-001). A player sees the same 1 to 21 seconds.
- Citation: Manual §I Game Options describes Text Delay in seconds.

## D-007: Chat options ship on, at 5 and at 8

- Change: `ChatLevel` (Unit Chat On) ships on, `TextScrollTime` (Text
  Delay) at 5 and `MaxTextLines` (Text Lines) at 8, in `options.cfg`
  under the original's own key names (legacy:131691-131696).
- Why: the original's factory values are not known, and Text Lines at 0
  or Unit Chat On off make chat look broken, the second by hiding your
  own line (legacy:206003-206004), so visible values are the safer
  guess.
- Citation: Manual §I Game Options lists all three sliders and the
  checkbox but gives no factory values.

## D-008: Console commands run on every machine

- Change: A chat line whose first non space character is `+` runs the
  command it names and goes out like any other line, so everyone sees
  `Player: +NOWISEE` (legacy:154470-154500). A command that changes the
  battle runs on every machine on the same tick, where the original
  changed only the typist's.
  - `+IWin` takes the army of every seat at war with the typist, where
    the original killed player 0 or 1. `+IWin` and `+ILose` end the
    battle, a mission whatever its objectives, with the typist's result,
    as in the original. In a skirmish the beaten seats count as gone and
    the shared rule ends it, so one `+ILose` does not stop two others.
  - `+ShareManaLimit`, `+ShareManaPct`, `+Radar` and `+View` act for the
    typist alone.
  - The developer table's `+IWin`, `+ILose`, `+Kill`, `+NoMana`,
    `+Gods`, `+BurnOne`, `+BurnAll` and `+LotsaBlood` need only the
    room's power codes, like the rest, and `+ShootAll`, beside
    `+GiveMana` in the open table, needs none.
  - A refused power code leaves the typist a note, as do `+ShootAll`,
    `+Gods`, `+BurnOne`, `+BurnAll`, `+LotsaBlood`, `+Contour`,
    `+ShowRanges`, `+Logo` and `+NetStats`, which act on nothing here
    yet.
- Why: in lockstep no machine may change the battle alone, so a power
  code is a command like a move order, and the room option that allows
  it is checked where it runs.
- Citation: the command table, legacy:38938-38946. Manual §I Game
  Options is silent on console commands.

## D-009: A save carries the command queue

- Change: none now. A saved game carries the orders still waiting for
  their tick, and the queue's tick counter, in its `CMDQ` section
  (src/game/savegame.c), so a load loses no order given before the save.
  The entry keeps its number because older pull requests cite it.
- Citation: none. The original is single player and hot seat on this
  path, and the manual says nothing about what a save holds.

## D-010: A browser can write a saved game out to a file

- Change: In a browser the page carries a Saved games panel, beside the
  link that forgets the game files, that writes any save in browser
  storage out as an .oksave file and reads one back without overwriting,
  a taken name getting a "(2)". The engine's own save and load dialogs
  are the original's, and a desktop build has no panel.
- Why: a save in a browser is not a file the player owns. Clearing site
  data or an evicted bucket takes it, and it cannot move to another
  machine or browser.
- Citation: Manual §III Saving and Loading describes files in a
  directory, which a desktop has and a browser does not.

## D-011: Only a campaign the translate table names is offered

- Change: the Book of Deeds lists a `camps\*.tdf` file only when the
  translate table names it, so the expansion's `ipalt.tdf`, the chapters
  of `the iron plague.tdf` with another last one, is not offered.
- Why: the retail game's chooser, seen in playtesting, never shows
  `ipalt.tdf`, whatever the chapter art's catch-all frame
  (legacy:144460-144495) suggests. The original adds every file its scan
  finds and a missed lookup hands back the key, so it filters in a
  directory scan that has no anchor, and the table gives the same
  result.
- Citation: legacy:141576-141720 the scan, legacy:267931 the lookup.

## D-012: The exit caption says Desktop rather than Windows

- Change: the main menu's help text for the exit door reads "Exit to
  Desktop" where the original's reads "Exit to Windows". The
  `ExitToWindows` widget name and the in game menu's caption are
  untouched.
- Why: the engine runs on Windows, macOS, Linux and in a browser, and on
  three of the four the old caption names the wrong system. It is a help
  string, not a rule.
- Citation: Manual §II, the main menu, names the door Exit to Windows.

## D-013: Two main menu doors rest a few pixels off their hit rect anchor

- Change: the machine door's sheet is drawn 2 px left and 3 px up of the
  hit rect anchor, and the knight's 3 px right and 1 px up. The girl's
  sits on the anchor.
- Why: the hover clips are drawn where the original draws them
  (legacy:35248, legacy:147846), but where it rests a sheet is not
  known. The offsets fit each sheet to its clips, so a hover does not
  move the door. If the original's doors jump the same way, this is
  where ours differ.
- Citation: issue #212. The manual does not describe the menu at this
  level.

## D-014: Sight reads the eye fresh at every stamp

- Change: a unit's sight grows and shrinks with the ground it falls on,
  as the original's does, but the engine reads the eye at every stamp
  and stamps again when it has moved at all. The original stamps again
  once the eye has moved by more than 5 (legacy:167455-167456).
- Why: a stale eye would have to be kept on the unit and in a save, for
  a few px of height between two stamps.
- Citation: the stamp's height term (legacy:167413-167423) and the
  ground record (legacy:224395-224470). docs/notes/fog.md.

## D-015: The sweep clears a building you own

- Change: with the broom armed, a click on a building of your own,
  finished or not and lodestones included, sends the selected reclaimers
  to take it down, its mana coming back pro rata as a cleared tree's
  does. An ally's or an enemy's building and anything that walks, the
  monarch included (`canmove` in `araking.fbi`), are refused.
- Why: without it a misplaced building is stuck on the map for good.
- Citation: issue #235. The original's CLEAR order reads only the map
  cell, for a resurrectable corpse, an animatable ruin or a reclaimable
  feature (legacy:187131-187199, legacy:127043-127046, `reclaimable` at
  legacy:127349, `resurrectable` at legacy:127369, `animatable` at
  legacy:127371), and gives no order otherwise (legacy:187201-187202,
  legacy:187219-187221). Its reclaim gives up with "Clearing failed"
  unless a reclaimable feature is there (legacy:32284-32286,
  legacy:32278-32281, legacy:32364-32365). The manual has no Sweep rule.

## D-016: A caster checks it can pay for its spell every tick

- Change: a caster that cannot pay for its selected weapon slot walks
  down, one slot at a time and stopping at the primary, to the next it
  can pay for on the tick it comes up short, not only after a shot.
- Why: the original walks down only after spawning a mana shot
  (legacy:245905-245913), so a caster drained some other way keeps a
  slot it cannot fire and stands in front of its target.
- Citation: issue #234. The manual does not describe weapon switching at
  this level of detail.

## D-017: Campaign progress is a line in the settings file

- Change: the furthest chapter a player has reached is a
  `HighWater.<player>.<book>` line in options.cfg, where the original
  writes `highwater.tak`, or `highwaterEXP<book>.tak` for a book other
  than Book of Darien, under `savedgames\<player>\`. Names alike in
  their first 31 characters share a line.
- Why: the port has no per player saved game directory, and in a browser
  the settings file is restored before the first frame and saved games
  only later, while progress has to be there when the book draws.
- Citation: legacy:153802-153926 writes the highwater save,
  legacy:144384-144422 reads it back, legacy:144073 reads the favourite
  campaign beside it.

## D-018: A mission's order lists and map script, and what they leave out

- Change: a placed unit works through its `InitialMission` list one
  order at a time, and a mission's `missions\<stem>.cob` runs on the
  unit script machine with a host of its own, as in the original. Where
  ours differs:
  - A patrol list sends the unit's patrol order to each point in turn,
    going on within 64 pixels or once it turns for the next. The
    original queues the patrol orders on the unit.
  - `w N` is N seconds, as Total Annihilation documented it. The
    original's time unit is inferred.
  - `wa` ends when the watched unit's health drops, where the original
    also wakes on a miss that does no damage.
  - `a TYPE` takes the nearest unit of the type, where the original
    first adds a random part, up to half, to each distance.
  - The list waits behind `a X Z`, an endless attack on the ground. What
    the original does next is not known, and no shipped list goes on.
  - `i NAME` boards at once and the list waits until the unit is set
    down, though a `w` runs out aboard. The original runs the list
    aboard (legacy:182203-182225, legacy:184105-184124).
  - `c`, `s` and `v` are read and dropped. Nothing makes a unit
    unselectable for `s` to undo, and the speed `v` sets has no field.
  - `SetAttribute` sets health, mana and the scales of D-020.
    `ReadValue` and `WriteValue` are not in, and no shipped script of
    the first campaign calls them.
  - A trigger circle uses the original's distance, the longer side plus
    a quarter of the shorter, and a unit's square is its position over
    16.
- Why: the parts left out need something the engine does not have yet.
  `o N` is the unit's standing orders, not a change of owner.
- Citation: legacy:228246-228770 the order list, legacy:8920-8958 the
  wait, legacy:8963-9031 the attack on a type, legacy:233216 the
  standing orders. The map script's engine is made at legacy:177876,
  its command host is legacy:178336-178652, its value ports
  legacy:178722-178822, its created and trigger events
  legacy:178218-178250, its destroyed event legacy:177932 and its
  verdict legacy:178706. legacy:306687-306699 is the order a query takes
  its arguments in, port first.

## D-019: The briefing is put away with a click or one of four keys

- Change: a campaign mission opens paused under `briefing.gui`, as in
  the original, and a mouse press or Enter, Escape, Space or Pause puts
  it away and starts the clock. F2 raises it again, and putting it away
  then leaves the clock as F2 found it. While it is up it takes every
  key and press, so the one that closes it, or one held as it came up,
  gives no order and opens no chat or menu. A battle loaded from a save
  opens under it too.
- Why: how the original dismisses it is not known. The panel has no
  button and says Paused across its top, hence Pause.
- Citation: legacy:153071-153079 opens it, legacy:154523-154602 fills
  the lines and pauses, legacy:154608-154620 sets a line,
  legacy:167978-168000 reads the mission's text.

## D-020: The attack and armour scales are applied by inference

- Change: a unit's attack and armour scales, in percent and 100 unless a
  script sets them with `SetAttribute AttackPercentage` or
  `SetAttribute ArmorPercentage`, make a hit the weapon's figure times
  the attack scale over the victim's armour scale, never under one point
  when there was any. Both are hashed and saved. Veteran levels multiply
  in by 1 + 0.1 a level to level 10, in integer tenths, into the same
  fields as the original's (legacy:232971-232984, legacy:235826-235862).
- Why: the original keeps both on the unit (legacy:178579-178588,
  legacy:173251-173252), but how its hit reads them is not known
  (legacy:245334), so the formula is read from their names.
- Citation: legacy:178530-178600 the SetAttribute command,
  legacy:173251-173252 the placement percentages, legacy:245324-245345
  the hit.

## D-021: Clearing a tree or a rock pays its mana

- Change: a builder clearing a map feature pays its seat the feature's
  `energy` as it goes, pro rata, as D-015's broom pays back a building.
  A tree or a rock is worth a few hundred mana.
- Why: clearing is slow work for a builder that could be building, and
  the payment makes it worth a player's while.
- Citation: #301, the mana audit. The original reads `energy` into the
  feature table (legacy:127334), but its reclaim pays nothing
  (legacy:32366-32400).

## D-022: A building can be placed turned

- Change: in the 3D view R or ] turns an armed building's ghost a
  quarter turn clockwise and Shift+R or [ turns it back. The footprint
  and yardmap turn with it, and with them the site test, the blocked and
  water cells and a builder's approach. The order, the save, the state
  hash and the client's hello carry the facing. A new placement starts
  unturned, a lodestone never turns, and the 2D view places unturned but
  draws turned buildings, so a match can mix clients. A turned
  building's scripts read its unturned orientation, and a factory never
  swings its base toward its own pad.
- Why: a choice for the 3D view and the remaster. The original places
  every building facing south.
- Citation: keys.tdf binds nothing to R, [ or ]. The original reads a
  footprint's centre back from its top left cell (legacy:184168,
  legacy:184216), which a turned footprint does with its sides swapped.

## D-023: A formation move from a front end

- Change: a front end can send one order that walks each unit it names
  to a point of its own, with three options: a heading each unit faces
  and holds on arrival, a group pace at the top speed of the slowest
  still walking, and a queue of up to sixteen such moves, which any
  unqueued order or change of owner clears. A unit of a formation walks
  to within a few pixels of its point, giving way. The points ride as
  offsets from the first unit's, 128 units a command, a big move's
  commands sharing one pace. A save keeps the line, the pace and the
  heading. The classic view sends none of these.
- Why: a choice for the remaster's front end. The original packs a group
  round the one point clicked and never turns a unit to a heading.
- Citation: the original's move order names a single point for the whole
  selection (see D-018 on its per unit order lists).

## D-024: An ally stops a shot and takes nothing from it

- Change: a shot is stopped by a unit, building or wall of any player
  but the shooter's, as in the original. An ally that stops a shot from
  a weapon with no areaofeffect takes no damage, and a splash harms only
  enemies unless the shot was fired at a point on the ground, so no shot
  wounds an ally by accident.
- Why: the original damages what stops a shot, ally or not, and its
  splash spares only the firing unit. Taking its blocking without its
  friendly fire keeps archers from wounding an ally's front line.
- Citation: the cell test's owner check (legacy:245419), the struck
  unit's hit (legacy:245029-245031), the area pass (legacy:245150).
  docs/notes/2026-09-04-combat-mechanics.md.

## D-025: A unit lets go of a target its shots cannot reach

- Change: when three shots in a row stop short of a target a unit picked
  for itself, on the ground, a tree or a rock, the sea, or a unit or wall
  of a player not its enemy, it passes the target over for ten seconds,
  or a minute if it cannot move. It then takes the nearest other enemy
  it can see or stands, and a computer's unit goes looking. A walker
  that can look for a place to shoot from tries that first and lets go
  after six. A hit on an enemy restarts the count, a shot past its aim
  point does not count, and the skip ends when the target hits the unit
  or either moves 32 px, not counting a walker's own moves. A player's
  or a mission script's attack order is never let go, but the
  computer's own are. A unit that cannot move, or holds position off
  patrol, also lets go of a picked target out of reach or inside its
  minrange.
- Why: shots stop on what they fly into, and the original fires into a
  hill without looking, so an archer in a clearing would shoot the trees
  until its target died.
- Citation: the original's fire gate looks only at range, reload, mana
  and facing (legacy:249328-249420), and a unit with no mover has no
  leash (legacy:15125). docs/notes/2026-09-04-combat-mechanics.md. The
  manual is silent.

## D-026: A flyer holds its fire while it climbs

- Change: a flyer that has taken off to attack or to move holds its fire
  until it reaches cruise height, and a hover attacker until it reaches
  its `hoverattackaltitude`. A flyer on the ground does not fire, as in
  the original (legacy:24543), and takes off for a target.
- Why: some flyers fire from a piece at or under their feet, so a shot
  early in the climb burst in the ground beside them. The original's
  stage that turns weapons back on could not be tied to a height, so the
  engine ties it to cruise.
- Citation: BeginFlight turns the weapons off (legacy:24120), the air
  attack on again (legacy:25797), and the hover attack at its second
  stage (legacy:30223-30253). A shot at or under its cell's floor bursts
  there (legacy:245470-245476). The manual is silent.

## D-027: A factory still being built can take a queue

- Change: a front end may queue units in a factory of yours that is
  still going up, to train once it is finished and its yard is open.
  Every other order to an unfinished building, its rally included, is
  refused, and the classic view sends none.
- Why: the remaster lets a frame be selected only to queue units in it.
- Citation: the original cannot select an unfinished building, and its
  queue add never checks (legacy:181790-181866). docs/notes/economy.md,
  "Build queues and rally points".

## D-028: A seat can claim its start position

- Change: the skirmish screen, and the battle room's Map and View Map
  dialogs, draw the start positions on the map's picture. A click takes
  a start or gives one back, and a drag moves a seat from one start to
  another, any seat in a skirmish and in a room for the host, a player
  only their own. A start another seat claimed is refused, and the
  host's click or drag onto it swaps the two and unreadies both. A new
  map frees every start. A claimed start is kept, and the other open
  seats take the starts left over in seat order, trading them as the
  original's deal does under Random Start Locations, so with no claims
  the deal is the original's.
- Why: so players can pick where they start. The original offers fixed
  or random starts only.
- Citation: the original deals starts to the open seats in seat order
  (legacy:195530-195541), so seat 3 behind a closed seat 2 starts on
  StartPos2, not StartPos3, and for random starts trades the first n
  among the n seats, one cycle from three on and a coin flip for two
  (legacy:195522-195576). Our draws come from the session seed rather
  than the host's C runtime.

## D-029: The map list has a search, filters and orders

- Change: the skirmish screen's map list and the room's map chooser take
  a search, every word matching the map's name, where the original wrote
  the heading, and choosers for players, size and order. Players go by
  the map's start positions, sizes by the size key's area, up to 8 x 8,
  12 x 12, 16 x 16 and beyond, and the order by name, players or size,
  either way. A right click steps a chooser back and clears the search.
  Each row shows the start count and, on the skirmish screen, the size.
- Why: hundreds of maps with the Darien Crusades packs do not browse by
  scrolling alone.
- Citation: none, a new control. The original lists every map in name
  order (legacy:167740).

## D-030: Select Game shows ping

- Change: each row of Select Game's game list ends with its host's ping
  to the server, blank until measured, and the status line reads "Your
  ping to the server is N ms." when it has nothing else to say.
- Why: every game goes through one server, so the two numbers say how a
  game will play before joining it.
- Citation: #295, new text on an existing list and status line.

## D-031: Shift shows the selection's orders

- Change: while Shift is held, the classic view draws a line from each
  of your selected units through its orders, with a marker for each
  move, attack target, patrol and its way round, and rally point, and a
  queued building as a ghost at its spot and facing. Other players'
  units show nothing, and a target you cannot see gives nothing away.
- Why: Shift queues orders (manual section IV), and a player cannot tell
  what is lined up without a picture, as Beyond All Reason draws one.
- Citation: no such drawing was found in the original.
  docs/notes/ui.md, "Shift orders".

## D-032: Battles are recorded, and the main menu plays them back

- Change: every skirmish and multiplayer match started fresh is recorded
  into a .okreplay file beside the saved games, keeping the newest 30
  within 48 MB and never pruning a save. A Replays line on the main menu
  opens the original's load dialog over them, and Load plays one back
  through the normal simulation with local orders refused, Space pausing
  and = and - stepping 1x, 2x, 4x and 8x. Restart plays it again and
  Save Game does nothing. A campaign mission and a battle loaded from a
  save are not recorded.
- Why: a lockstep battle is its seed, its configuration and its
  commands, so a replay is small and needs no simulation of its own.
- Citation: issue #294. `src/net/replay.c`, `src/ui/replay_session.c`,
  and docs/notes/2026-09-04-multiplayer-hapinet.md, "Replays". The
  original has no replays.

## D-033: A match under way can be watched, and the watcher sees the whole map

- Change: Select Game lists a match under way with Watch when its host
  allows watching and a watcher's place is free, and the web page's game
  list and leaderboard give a watch link. A watcher replays the match
  from its first turn and follows it live, gives no order, cannot save,
  load or restart, and sees the whole map, with Tab looking through each
  player's eyes in turn. The players see a chat line when someone starts
  or stops watching. A watcher's world never counts when worlds are
  compared, and it cannot delay a turn.
- Why: the original's watcher joined before the start and took one of
  the eight places (N-008). The relay keeps every turn, so a later join
  works, and a watcher has no side's fog of its own.
- Citation: issue #294. `src/net/relay.c`, `src/net/turnclock.c`,
  `src/ui/select_game.c`, `src/ui/ingame.c`, and
  docs/notes/2026-09-04-multiplayer-hapinet.md, "Watching a match".

## D-034: A lost skirmish plays on between the computers

- Change: a skirmish the local player lost plays on while two seats
  still standing are at war, after the Defeat banner's three seconds,
  with the fog lifted and Tab looking through each player's eyes. The
  defeat, its tick and its cue stand, and when one side is left the
  battle stops with no second verdict and the statistics screen opens.
  It never runs in a match or a campaign mission.
- Why: so a beaten player can watch the rest, as in a match. In the
  original a skirmish is over for the player once their side is gone
  (legacy:240018-240028).
- Citation: `InGame_PlayOn` in `src/ui/ingame.c`.

## D-035: The wind keeps its own clock, and a save keeps a fire where it was

- Change: the wind's next change, 3 to 14 seconds on, comes from a
  stream of its own that starts where the original starts its C library
  generator. The wind's speed and turn and the fire's rolls draw from the
  simulation's generator, as the original's do. Every machine runs every
  hit on scenery, and a save keeps a death or a burn at the picture it
  had reached.
- Why: the original's wind timing shares that generator with its drawing
  (legacy:241703-241718), its host alone decides scenery hits
  (legacy:128759-128776), and its save restarts a burning or dying
  feature (legacy:129249-129270), none of which lockstep allows.
- Citation: `wind_step` and `wind_rand` in `src/game/features.c`, the
  FEAT and WRLD sections in `src/game/savegame.c`.

## D-036: Remastered battlefield rules, a room option

- Change: a row of our own on the skirmish screen and in the battle room
  turns on the remastered battlefield rules. They are off by default,
  always on in Stars of Darien, and the game list marks a room that
  plays them. With them on:
  - Rocks, ruins, spires and grass break, at the damage their file gives
    or else 3000, 4000, 2000 and 200, and leave nothing. Lodestones,
    sacred sites, waves and sound emitters never break. The original
    ignores every hit on an indestructible feature (legacy:128756).
  - The twelve `unitsonly` spells reach scenery as any blast does, where
    the original skips it for them (legacy:245240).
  - A unit whose model is within 32 px of a burning feature's centre
    takes 25 every 15 of the original's frames, credited to nobody. The
    original's fire hurts nobody, since `burnweapon = TreeBurn` is never
    defined and its loader reads only a `[BurnWeapon]` section no feature
    has (legacy:127389-127401).
  - A fire spreads. A burning feature throws four sparks, each 125
    percent of the original's spark time after the last, and its flames
    start over until the last. A spark reaches every flammable feature
    within three cells, or the nearest ring that holds one, at most six.
    The wind stretches that downwind by five steps of twice its parts in
    1/4096 of a cell, at most two cells an axis, and a spark with nothing
    within six cells can light one up to eight cells downwind. A feature
    catches at twice its spreadchance, a quarter of that upwind. At most
    four catch from sparks in a frame, a spark past that waits only when
    it could light something, and each spark starts its cells from
    another corner, so the cap favours no side. In the shipped data the
    original's flames die before its spark comes, so its fires never
    spread.
  - The rubble of a wall, a model wall or a building blocks until it is
    swept, which pays its mana (D-021). The original's wall rubble and
    model wall wrecks let units through.
- Why: the rules make the field matter more, and they stay a choice so
  the original's game is the default everywhere but Stars of Darien.
- Citation: `remastered` on the BattleConfig, TAK_ROOMOPT_REMASTERED on
  CREATE_ROOM and START_GAME, the save's CFGB section, the replay header
  and the state hash carry the option. `remaster_breakable_hp`,
  `remaster_leaves_rubble`, `Features_InstanceBlocks`,
  `Features_TickFrame` and `feat_spark_remastered` in
  `src/game/features.c`, `blast_scenery` and `Units_ScorchAt` in
  `src/render/units.c`, and `GUIDialog_AddOptionRow` in
  `src/ui/gui_loader.c`.

## D-037: The minimap's left button looks when nothing of yours is selected

- Change: under the left click interface, the engine's only one, a left
  press on the minimap with none of your units selected centres the view
  there and follows a drag, as the right button does. With your units
  selected both buttons act as the original's.
- Why: the original's left press there only selects a unit of yours at
  the point (legacy:238837-238848, legacy:243640-243644), so the minimap
  would seem dead to a player who has not found the right button.
- Citation: manual p.69 says the left click interface uses the right
  button to find a location within the Mini-Map. `ig_minimap` in
  `src/ui/ingame.c`.

## D-038: Every shot but mind control reaches its target and aims where it stands

- Change: a shot fired at a unit leaves for where the unit stands,
  strikes it within 24 px of its body, bursting on the nearest point of
  its model box so a splash does its whole damage, and crosses the
  ground at its full weaponvelocity however steep its line. A mind
  control shot follows the original instead. It leads its target by four
  fifths of its flight, strikes only a unit in its own 16 px cell whose
  turned selection quad and model span hold it, flies at its
  weaponvelocity along its line, lasts its range at its speed across the
  ground, and ends when its caster is seen dead or dying.
- Why: moving every weapon to the original's reach and lead is a balance
  change to check weapon by weapon. Mind control went first, since its
  slow shot let the reach turn near misses into captures.
- Citation: the lead (legacy:234031-234060, legacy:234049), the cell
  test (legacy:245376-245476), the quad and span test
  (legacy:236974-237027), the line's speed (legacy:246885-246892), the
  life (legacy:246919-246922), the caster check (legacy:247710-247713).
  docs/notes/2026-09-04-combat-mechanics.md, "Mind control". The manual
  is silent.

## D-039: Where a hover attack stops closing in by the common rule

- Change: a flyer with `hoverattack` closes on its target and holds its
  own bearing at its `hoverattackdistance`, as the original's does, with
  four differences.
  - Beyond that distance plus 160 px it steps out of a crowd by the
    common rule, and nearer by the hover attack's own. The original
    switches at a reach test that is not known, so the line sits where
    the attack turns its weapons on.
  - It counts a point reached within 8 px, the walk's radius. The
    original's move stops within 4.
  - Turned onto its target it creeps off its heading at
    (2 brakerate + acceleration) / (2 - acceleration / maxvelocity) a
    frame, the original's settled speed as it backs away, and along it
    at the walk's speed.
  - Against a point on the ground it turns onto that point in range and
    flies straight at its own point at full speed, where the original's
    plain move faces its way (legacy:30328-30338, legacy:190557-190562).
- Why: our walk keeps no velocity of its own, a point it stops short of
  would never wake the attack, and how the original aims at a point on
  the ground is not known.
- Citation: the far test and formation move (legacy:30139,
  legacy:30467-30502, legacy:235678-235762), the step out
  (legacy:30636-30705), the move to its point (legacy:30362-30364), the
  flight (legacy:183961-184057).
  docs/notes/2026-09-04-combat-mechanics.md. The manual is silent.

## D-040: A unit at its leash still fights what it reaches

- Change: a fight a unit took on for itself ends at its
  `maneuverleashlength` from where it began, as in the original, but
  only while the target is out of reach. Walking back, it takes on what
  is inside the leash, as the original does, and anything its weapon
  reaches. A hit answered on the way keeps the first point, where the
  original starts a new fight from where the unit was hit. A computer
  player's builder on patrol draws only among finished buildings, where
  the original draws among everything.
- Why: units should fire at what they can reach and never stand under
  fire doing nothing, and one point per fight keeps the walk back short.
- Citation: the leash (legacy:11020, legacy:11380), the walk back
  (legacy:10438-10443), a new fight on a hit (legacy:15170,
  legacy:181879-181947), the patrol's draw (legacy:10877-10884).
  docs/notes/2026-09-04-combat-mechanics.md. The manual is silent.

## D-041: Ctrl+D asks before a self destruct starts

- Change: Ctrl+D asks in the game's yes or no box before the selection's
  self destruct starts. Enter or Yes starts it, Escape or No does not,
  and calling off a running countdown asks nothing. The clock stops
  under the box in single player and skirmish and runs on in a match.
- Why: one slip of the keys would otherwise end an army.
- Citation: the original starts the countdown at once
  (legacy:122248-122294). The box is the shipped data/guis/yesno.gui,
  the question form of the original's message dialog
  (legacy:146759-146780). The manual is silent.

## D-042: An aim its script never answers fires after two seconds

- Change: a weapon fires once its script sets the aimed port, as in the
  original, except that an AimWeapon still running after 120 ticks, two
  seconds, with the unit still and the target in reach counts as aimed.
  The exception never applies while the last shot's FireWeapon is still
  playing, or while the aim, or a script it called, waits on a piece
  turning or moving, so firing animations never overlap and a turret
  fires only on target. Any other unanswered aim is asked again.
- Why: some casters' scripts answer only an aim begun while walking, and
  ours starts the aim when the unit stops, so spells like Kirenna's Water
  Blast at a point she walked to never fired. The original's approach
  seems to start the aim earlier, and this stands in until ours does.
- Citation: the aimed port (legacy:223392-223395) and the fire gate
  (legacy:249328-249384). VERTOWER and ARASSH in the original turn fully
  onto a target before firing. The manual is silent.

## D-043: A fallen player is marked by a low horn

- Change: when a player's last unit is gone, beside the line that names
  them, the engine plays a low horn over a muffled drum of about a
  second, made from sine waves at run time (src/sound/fallen_cue.c). It
  plays for every player who falls, in a skirmish or a match, on every
  machine, at the sound volume, but not while a battle is being caught
  up to the present.
- Why: the original plays MessageArrived for that line, a file the
  shipped archives lack, so the fall passed in silence.
- Citation: the line and its cue (legacy:206939-207005,
  legacy:227378-227387), docs/notes/gameflow.md, "A fallen player". The
  manual is silent.

## D-044: A finished sweep plays a flash

- Change: when a sweep takes a feature off the map, the Death01 sequence
  of the game's deathmagic sheet plays once where it stood. A cancelled
  sweep, a feature that rots away or one removed any other way plays
  nothing.
- Why: ours removes the feature at once, where the original's lingers a
  moment, so the flash shows the sweep is done.
- Citation: the original lingers as an animating record and clears its
  cell with no effect sprite (legacy:127856-127884, legacy:128007).
  docs/notes/features.md.

## D-045: Radar and sight jamming, tags for mods

- Change: two unit tags the original never reads, `radardistancejam` and
  `sightdistancejam`, radii in world pixels that no shipped unit sets
  (docs/MODDING.md).
  - Inside an enemy radar jammer's radius a unit shows no radar blip.
  - Inside an enemy sight jammer's radius a player's ground falls back
    to explored, but for the nine cells round each of their units, and
    their units cannot see or pick targets there, though targets already
    held or ordered are still fired on. A builder walks up to a jammed
    site to judge it, a site there is not checked for hidden units, and
    the computer players read the jammed fog too. Sight jamming does
    nothing with Line of Sight off and hides no radar blips.
  - A jammer works while finished and alive, with `onoffable` only while
    the Active and Inactive buttons have it on, and `activatewhenbuilt`
    turns it on when built. It never jams its own side or allies, who
    with watchers see a pale haze on its ground.
- Why: mods get Total Annihilation's radar jamming and a sight jamming,
  and the base game, which has neither, is unchanged. Sight jamming is
  simulation state, in the hash and the saves, while radar jamming and
  the haze are drawing only.
- Citation: #319. The nine cells are those the original's stamp always
  lights (legacy:167402). Radar is the original's (legacy:208642-208655,
  docs/notes/fog.md, "Radar"). The manual is silent.

## D-046: M held on a drag box leaves the monarch out

- Change: with M held during a box drag over the battle, the box selects
  as it would but leaves out your monarch, the units Ctrl+M takes, and
  with Shift adds to the selection, in both views. M still arms Move, as
  in the original, and a box drops it. A click with M held that moves
  under 4 px gives that Move to the spot, a drag let go over the HUD
  keeps Move armed, and M while Move is armed from its button disarms
  it. The box takes whichever key the scheme gives Move (D-057).
- Why: a quick way to take an army into a fight and leave the monarch
  behind.
- Citation: the original's Keys.TDF binds M, LOWER_M and UPPER_M, to
  UnitCommand Move. The box rule is ours. The manual is silent.

## D-047: Battle chat is larger, in the sender's colour, on a plate

- Change: battle chat, its edit box and the +Clock line draw at one and
  a quarter times the original on the 480 row canvas, twice at 1080
  rows and at most four times, in quarter steps, from the game's Times
  sheets scaled by edge directed doubling and resampled. A line carries
  the team badge, the name in the bold sheet in the player's colour and
  the message in a paler tint, a colour too dark to read being lifted
  toward white in linear light to a relative luminance of 0.34 for the
  name and 0.64 for the message. It sits on a rounded warm dark plate,
  every glyph with a dark halo, and fades over its last half second.
  Lines wrap at 560 original pixels.
- Why: the original's 8 point white face is tiny on a browser sized
  canvas and vanishes over snow and sand.
- Citation: legacy:205958-206051 and legacy:206008-206015, the block and
  its badge. `chat_ink_reads_on_snow_and_water_for_every_player_colour`.
  The manual is silent on how chat looks.

## D-048: A ranged unit turns on a flyer hitting a friend

- Change: when a flyer hits a ground unit, every ranged unit of that
  side or an ally within 640 px of the flyer, whose weapon in hand can
  reach and hurt it and which is fighting a ground target it picked or
  its computer player sent it to, takes the flyer as its target, a fight
  it picked, so its leash holds. A fight a human player or a mission
  script ordered, and a unit holding fire, are left alone.
- Why: so archers help against flyers attacking the units round them.
- Citation: #503. The original's target search scores by distance,
  damage and turn (legacy:21170-21246), and a hit draws only the unit
  hit (legacy:15187-15215) or a guard's charge (legacy:233765-233768).

## D-049: Where an attack run and a flyer's patrol depart from the original

- Change: a flyer without hoverattack, such as the Dirigible or the
  Gryphon, flies its attack runs as the original does, except that it
  runs at a target from the start without the original's formation
  move, keeps its points its turning circle and 32 px inside the map
  where the original sends it 800 px back in, and stands off another
  flyer and fires. On patrol it takes its next leg within 128 px of its
  point, unless that point is as near, and flies point to point rather
  than round the order point at its weapon's range plus 160 px.
- Why: the engine has no formation move, edge push or air to air mission
  yet, and walking a leg to within 8 px braked every flyer to a stop.
- Citation: the run (legacy:28491-29118), the reach test and formation
  move (legacy:28572-28630), the edge push (legacy:28854-28924), the
  flying target and dropped key handed elsewhere (legacy:28524-28545),
  the patrol leg (legacy:26275).
  docs/notes/2026-09-04-combat-mechanics.md. The manual is silent.

## D-050: W, A, S and D pan, and Attack and Stop sit on B and N

- Change: W, A, S and D pan the camera in both views, as the arrows and
  the screen edge do, except while Ctrl is held. B arms Attack and N
  gives Stop, where the original has A and S. The other order letters,
  M, G, P, L, U, H, C, K and O, are the original's.
- Why: letter panning ranks above the original's two letters, and A and
  S cannot both pan and give orders. B and N are free in both views, sit
  beside WASD and keep their labels on QWERTZ and AZERTY.
- Citation: the original's Keys.TDF binds LOWER_A and UPPER_A to
  UnitCommand Attack and LOWER_S and UPPER_S to Stop, leaves B empty,
  gives N to MoveToNextUnit and binds no letter to the camera. The
  manual is silent. These are the keys of the Standard scheme (D-057).

## R-008: A reel's soundtrack goes through the game's mixer

- Change: a clip's audio is decoded beside its picture and played
  through the game's mixer at the effects volume, about a third of a
  second ahead of the frame on show. With sound off a reel is silent.
- Why: the original's video library plays the sound itself
  (legacy:35289), and ours decodes with FFmpeg into its one mixer, so
  the clip takes the effects volume, the original having no level for it
  either. The lead keeps the ring from crackling at the first late frame.
- Citation: legacy:35289 the clip's clock. The FFmpeg the site and the
  Mac and Linux releases carry has the two Bink audio decoders, and the
  Windows release takes the full library.

## D-053: The classic view's fog is a drifting mist on a hardware renderer

- Change: on a hardware renderer, explored ground out of sight lies under
  a pale drifting mist in the viewing side's `FogColor` instead of black
  at alpha 120. The software renderer keeps the overlay, `FogMist=0` in
  options.cfg turns the mist off, and a beam draws when its shooter or
  either end is in sight.
- Why: the original in hardware mode shows such a mist. Nothing in the
  data shapes it, so ours is fitted to captures and set by eye.
- Citation: `FogColor` (legacy:164740-164817), the overlay
  (legacy:130167-130436). The manual is silent.

## D-054: A storm's falling drop lands its head on its burst

- Change: each drop of a `hailstorm` storm is drawn shifted so its head
  meets its burst, the shift read from the art. Drawing only.
- Why: the original's Fire Storm meteors land a few px off their bursts,
  which reads as a miss.
- Citation: captures of the original's Fire Storm. The manual is silent.

## D-055: Storm puffs from the first cast

- Change: each drop of a `hailstorm` storm leaves 3 or 4 bigsmoke puffs
  from the battle's first cast, and a cast that takes mana always casts.
- Why: the original shows the puffs only once something has died, and on
  one map its storm stopped casting while still taking mana. Both read as
  limits of its particle system.
- Citation: captures of the original. The manual is silent.

## D-056: Lightning is one jagged bolt, not the original's fading strands

- Change: a lightning bolt is one jagged line of its inner colour in a
  soft band, at full length for its measured life.
- Why: the original's `lightning` system in `effects.tdf` grows fine
  strands from the muzzle that fade. One bolt stands in until a rendering
  pass of our own.
- Citation: captures of the original's Mage Tower, Wisp and Death Totem.
  The manual is silent.

## D-057: The battle's keys are a table with schemes

- Change: every key the battle answers is an action in a table a scheme
  fills. Standard is D-050's keys and the default. OTAK is the Keys.TDF
  keys the engine has a command for, with only the arrows panning. Grid
  puts the orders under the left hand. A player's own keys lie over the
  scheme in `keys.cfg`, set from Key Binds on the options' Interface
  page. E, or X in Grid, arms Resurrect for a selection whose sidebar
  could raise, and the 3D camera gives E up meanwhile. Ctrl with Q, E,
  R, F, Z or X no longer moves the 3D camera, and Ctrl+R no longer
  turns a building being placed.
- Why: OTAK keeps the original's keys and Standard keeps D-050's, so
  each player picks. One press reaches one action.
- Citation: the original reads its keys from Keys.TDF, which a player
  can edit, and has no dialog for them. The file leaves E empty, though
  E arms Resurrect in the original. `test_keybinds`. The manual is
  silent.

*(More entries added as deviations land.)*
