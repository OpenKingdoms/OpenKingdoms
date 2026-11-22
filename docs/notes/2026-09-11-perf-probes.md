# Performance probes

How to run OpenKingdoms' performance probes, the scenarios they play,
the lines they print, the spike lines, the limits
`scripts/web-perf.js` checks and how to read the numbers. All of it is
the engine's own tooling and not the original's behaviour, so this
note cites no legacy anchors and no deviations.

## Running one

A probe builds its own battle and goes straight to loading, so a run is
the same every time. It turns vsync off, because the frame figure
should be the work a frame costs and not the wait for the display, and
the engine quits when the scenario ends. Natively it runs with the
window and the renderer, from these switches:

- `--perf-probe <scenario>`, one of ffa, crowd, build8, build1 and big8
- `--perf-ticks <n>`, which shortens the run to n sim ticks
- `--perf-scale <k>`, which repeats the map k by k times, 1 or 2
- `--reveal`, which plays with the map revealed and line of sight off,
  for captures

For example `--perf-probe crowd --perf-ticks 3600` plays one sim
minute of crowd.

In a browser the flags arrive through the page's query string, as in
`?args=--perf-probe%20ffa`. `scripts/web-perf.js` drives that, as in
`node scripts/web-perf.js <url> <game dir> ffa crowd`. It feeds the
archives from an install the way `scripts/web-smoke.js` does, reads the
probe lines off the console, and fails when a line breaks one of the
limits below. `WEB_SMOKE_OUT` picks its output folder. Only one browser
probe runs at a time.

In the suite, `test_ui_screens` runs `perf_probe_duel`,
`perf_probe_ffa`, `perf_probe_crowd` and `perf_probe_shadows` when
`TAK_PERF_PROBE` is set. The duel draws every frame. The ffa and crowd
cases run the simulation alone, because a Debug build with an army on
the map draws far too slowly, and frame figures come from the browser
run anyway. `perf_probe_shadows` draws 64 units for 60 frames, with
shadows off and on, and times them.

## Scenarios

A sim minute is 3600 ticks. Every scenario plays with the monarch
expendable, so an early death does not end the run.

`ffa` is four computer seats on Lake Lokken, the largest map with four
starts, with nobody human. The seats are Aramon, Taros, Veruna and
Zhon, one per team, all at difficulty 2. Every sim minute each seat is
topped up to 75 mobile units, spawned in a ring around its start on
ground its move class can stand on. It runs 12 sim minutes, 43200
ticks.

`crowd` is 300 walkers of mixed move classes on Tarosian Plain, in two
blobs of 150 on the third and fifth starts, ordered through each other
and swapped again every sim minute. Both seats are human and nobody
drives them, so no computer player interferes and the walkers are the
whole measurement. It runs 5 sim minutes, 18000 ticks.

`duel`, in the suite only, is two computer players on Two Castles,
printing the same fields as the others.

`build8` and `build1` ask how much a computer player builds for itself.
Neither spawns anything, so every unit on the map was built by a seat.
Both run on Ladron's Tarn at difficulty 2 for 10 sim minutes, 36000
ticks. `build8` fills all eight starts and `build1` leaves six closed,
so the two runs compare directly. Seat one is a human who never acts in
both, because a skirmish with no human standing is over on the first
tick, and that rule belongs to the end screen and not to the probe. The
soak `eight_ai_seats_each_build_an_army`, run with `TAK_SOAK=1`, plays
build8 for 6 sim minutes, and `TAK_PROBE_TICKS` lengthens it.

### Eight seats on a bigger map

`big8` plays Ulasem Arena, the largest map with eight starts, with
eight computer seats on their own teams and nobody human, for 10 sim
minutes. `--perf-scale 2` repeats the map twice each way and moves each
start into its own copy, so the same eight seats spread over four times
the ground. Twice a side is the largest map the engine is built for.
`TAK_PERF_MAP` and `TAK_PERF_SEATS` play the same scenario on another
map with fewer seats, from 2 to 8. Each window of big8 prints the
census of what each seat has built, and splits the combat tick, the
mover and the computer player's think into their parts, as described
under "The line".

## The line

A probe prints one line for each window of 600 sim ticks, and a done
line when the run ends. Every figure is a difference between two
readings, so a window figure is what that window cost. A window line
reads `perf-probe <scenario> wNN key=value ...` with these keys:

| Field | Meaning |
|---|---|
| `tick` | ticks run so far, `ticks` how many are in this window |
| `sim` | ms spent in the sim step over the window |
| `worst` | the worst single tick in the window, in ms |
| `ai`, `eng`, `eco`, `fog` | the four sim systems, in ms |
| `ai_worst` | the worst single tick of AI work |
| `path` | ms inside route searches, `path_worst` the worst tick |
| `cmb`, `prj`, `cob`, `misc` | the engine split inside `eng` |
| `plans` | route searches units asked for |
| `work` | nodes the searches expanded |
| `rebuilds`, `rebuild` | passability and clearance cache builds, and their ms |
| `parked` | footprints that gained or lost the parked tag |
| `frames`, `p50`, `p95`, `p99` | frames drawn and their ms percentiles |
| `capped` | frames that hit the four-tick catch-up cap |
| `units` | live units, `heap` the live bytes in KB |
| `pmem` | KB held by the planner's per-layer caches |
| `stall`, `stall_max` | the stall census at the last sample and its worst |
| `frame_max` | the longest frame in ms, the one hitch a percentile hides |
| `over33`, `over85` | frames over 33 ms, and over 85 ms, which outlasts the browser's audio period and is heard as a glitch |
| `flows`, `flow` | group flow fields swept, and their ms |
| `rss` | KB the process holds, 0 when that is not known |
| `map` | the map's size in tiles, wide by high |
| `orders` | hostile orders each AI seat has issued, ffa only |

The done line, `perf-probe <scenario> done ...`, carries the whole
run's frames, p50, p95, p99, capped, worst, path_worst, ai_worst,
stall_max, frame_max, over33 and over85, and `end`, which is `complete`
or says why the run was cut short. A run cut short still prints its
part window and a done line.

The build scenarios and big8 add a `census` line for each seat and one
`slots` line to every window:

| Field | Meaning |
|---|---|
| `ever` | slots this seat has ever used, `alive` standing and finished |
| `uc` | frames still under construction, `busy` builders with an order |
| `mob`, `cmb`, `str` | mobile, mobile and armed, and structures |
| `bld` | builders, `fac` of which the AI counts as production structures |
| `mana` | standing mana income buildings |
| `built`, `lost`, `kills` | the battle statistics for the seat |
| `pool`, `spend` | mana held against its cap, and what it is spending |
| `slots` | slots consumed of the whole pool, with `spawn_fails` |

The `fac` column asks the computer player's own classifier. Nearly
every structure carries some mogrium storage, a keep stores 200, so a
count by storage would take keeps and castles for mana buildings and
find no production structure anywhere.

big8, and any run with `TAK_PERF_SPIKE` set, adds three lines to every
window: `cmb` with the combat tick's upkeep, acquire, decide, act and
tail, `walk` with the mover's calls, plan, route, aim and step, and
`ai` with the computer player's maps, threat, plan, wave, break,
builders and fighters, all in ms.

`TAK_PERF_HASH=1` adds a `hash` line before every window, whose `fold`
is the state hash of every tick so far folded together, so two builds
can be held to the same battle. `TAK_PERF_NO_GRID` answers every
question the spatial grid would answer by a scan of every unit, and
with `TAK_PERF_HASH` shows whether the two pick the same.

The stall census samples every 60 ticks. It counts a unit that holds a
movement order, has stayed within 32 px of one spot for 30 s, is more
than 96 px from its goal, is not within weapon range of its target and
is not waiting on a plan. It is "never stuck for good" turned into a
number, and it must read zero at every sample.

## Spikes

A window's `worst` says a tick was slow, not what made it slow.
`TAK_PERF_SPIKE=<ms>` prints a `spike` line for every tick at least that
long. It holds the tick's own share of each window figure, then the
combat split, the mover split as `wplan`, `wroute`, `waim` and `wstep`,
the computer player's parts, and the live units. `whole` counts from
before the tick's orders, `sim` from where the window figures start.

`TAK_SPIKE_PROBE=<minutes> test_ui_screens sim_spike_probe` plays big8
flat out with the map revealed, the way the remaster's soak runs a
battle, and prints the same lines. `TAK_PERF_MAP` and `TAK_PERF_SEATS`
work there too. Like the AI duel, the case is registered only when the
variable is set. For example
`TAK_SPIKE_PROBE=15 TAK_PERF_SPIKE=10 test_ui_screens sim_spike_probe`
prints every tick over 10 ms of a 15 minute battle.

## The limits web-perf.js checks

The limits are for the browser run, in Edge with the public build. A
run fails when:

- the whole frame is over 16.7 ms at the 95th percentile or over 25 ms
  at the 99th, over the run
- more than 0.5 percent of frames hit the four-tick catch-up cap
- pathing averages over 1 ms a tick in any window, or takes over 3 ms
  in any one tick
- the computer players together take over 3 ms in any tick
- the planner caches hold over 16 MB
- the stall census reads above zero at any sample
- ffa falls below 300 live units in its last 10 sim minutes, checked
  on a full length run
- the run does not end complete

## Reading the numbers

Every window figure is per 600 ticks, so a shortened run compares with
a full one directly. Only the done line speaks for the whole run.

In a browser the page is not cross-origin isolated, so its clock is
coarse: about 0.1 ms in Edge by one account and a whole millisecond by
another, which is not settled. Either way a sub-millisecond tick can
read as zero, the figures for each system are sums over a window, and
a worst-tick figure carries the clock's step.

A Debug build is between ten and thirty times slower than a Release
one, so suite numbers only ever compare with other suite numbers. The
ffa and crowd suite probes run the simulation without drawing, so
their frame columns are zero by design.
