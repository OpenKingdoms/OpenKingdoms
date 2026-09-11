# Performance probes and the baseline they set (2026-09-11)

Movement and AI work is about to change the hottest parts of the tick,
so the engine needs a way to say what a tick costs before and after.
This note describes the probes, what each field means, and the numbers
main produced on the owner's machine. Every later change is compared
with these.

## Running one

Native, with the window and the renderer:

```
tak-re --perf-probe ffa
tak-re --perf-probe crowd
tak-re --perf-probe crowd --perf-ticks 3600
```

The flag builds its own battle and goes straight to loading, so a run is
the same every time. It turns vsync off, because the frame figure should
be the work a frame costs and not the wait for the display. The engine
quits when the scenario ends.

In the browser the flag arrives through the page's query string, which
is how `scripts/web-perf.js` drives it:

```
https://openkingdoms.net/?args=--perf-probe%20ffa
node scripts/web-perf.js http://localhost:8097/tak-re.html "<game dir>" ffa crowd
```

`web-perf.js` feeds the archives the way `web-smoke.js` does, reads the
probe lines off the console, and fails when a line breaks one of the
limits listed below. `WEB_SMOKE_OUT` picks the output folder, and only
one browser probe runs at a time.

In the suite, `test_ui_screens perf_probe_duel`, `perf_probe_ffa` and
`perf_probe_crowd` run the same code. The duel draws every frame as it
always has. The other two run the simulation alone, because a Debug
build with an army on the map draws far too slowly to reach a window,
and frame figures come from the browser run anyway.

## The scenarios

`ffa` is four AI seats on Lake Lokken, the largest map with four starts.
Every sim minute each seat is topped up to 75 mobile units, spawned in a
ring around its start on ground its move class can stand on. It runs for
12 sim minutes. The seats are Aramon, Taros, Veruna and Zhon, one per
team, all at difficulty 2, with the monarch expendable so an early death
does not end the run.

`crowd` is 300 walkers of mixed move classes on Tarosian Plain, in two
blobs of 150 on the third and fifth starts, ordered through each other
and swapped again every sim minute. Both seats are human, so no AI
interferes and the walkers are the whole measurement. It runs for 5 sim
minutes.

`duel` is the existing two-AI probe on Two Castles, kept for comparison
with earlier numbers, now printing the same fields as the other two.

## The line

One line per 600 sim ticks, then a done line with the whole run's frame
figures. Everything is a difference between two readings, so a window
figure is what that window cost.

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
| `orders` | hostile orders each AI seat has issued, ffa only |

The stall census samples every 60 ticks. It counts a unit that holds a
movement order, has stayed within 32 px of one spot for 30 s, is more
than 96 px from its goal, is not within weapon range of its target and
is not waiting on a plan. That is the "never stuck for good" line of
issue #60 turned into a number, and it must read zero at every sample.

## The limits web-perf.js checks

These come from the issue #60 acceptance checks. The browser figures are
measured in Edge with the public build.

- whole frame at most 16.7 ms at the 95th percentile and 25 ms at the 99th
- at most 0.5 percent of frames at the four-tick catch-up cap
- pathing at most 1 ms per tick averaged over any window, and 3 ms in any tick
- AI for all players at most 3 ms in any tick
- planner caches at most 16 MB
- the stall census zero at every sample
- ffa keeps at least 300 units alive through its last 10 sim minutes

Main breaks several of these, which is the point of writing them down.

## Reading the numbers

Two things about the measurement itself. Edge's clock is coarse without
cross-origin isolation, about 0.1 ms, so per-system figures are window
sums and a worst-tick figure carries that resolution. And a Debug build
is between ten and thirty times slower than a Release one, so suite
numbers only ever compare with other suite numbers.

## Baseline: main at 03463ae

Native figures are a Release build on the owner's machine, running the
engine itself with the window and the renderer. Browser figures are
Edge on the same machine with the public build. Both baseline runs were
shortened to keep the wall clock sensible: 7200 ticks natively and 3600
in the browser, against the 43200 and 18000 the scenarios run in full.
Every figure below except the whole-run frame percentiles is per 600
ticks, so it compares with a full run directly.

Native engine, ffa:

| Figure | Value |
|---|---|
| frames | 2433, p50 58.1 ms, p95 126.0 ms, p99 188.0 ms |
| frames at the catch-up cap | 1110 of 2433, 45.6 percent |
| worst single tick | 1113.47 ms |
| worst pathing tick | 943.75 ms |
| worst AI tick | 168.54 ms |
| pathing per window | 8524 ms falling to 3750 ms, so 14.2 down to 6.3 ms per tick |
| cache rebuilds | 14 builds costing 4009 ms in the first window |
| stall census | up to 5 units |
| planner caches | 1406 KB |
| live units | 280 to 302 |

Native engine, crowd:

| Figure | Value |
|---|---|
| frames | 5483, p50 14.4 ms, p95 94.3 ms, p99 148.0 ms |
| frames at the catch-up cap | 497 of 5483, 9.1 percent |
| worst single tick | 548.47 ms |
| worst pathing tick | 109.36 ms |
| pathing per window | 1365 to 5408 ms, so 2.3 to 9.0 ms per tick |
| stall census | up to 14 walkers |
| planner caches | 1125 KB |

Browser, ffa then crowd:

| Figure | ffa | crowd |
|---|---|---|
| frames | 2290 | 2467 |
| p50 | 23.0 ms | 16.1 ms |
| p95 | 54.1 ms | 116.0 ms |
| p99 | 79.1 ms | 164.0 ms |
| at the catch-up cap | 71 frames, 3.1 percent | 259 frames, 10.5 percent |
| worst single tick | 455 ms | 643 ms |
| worst pathing tick | 427 ms | 93 ms |
| worst AI tick | 27 ms | 1 ms |
| pathing, worst window | 12.0 ms per tick | 7.6 ms per tick |
| stall census | up to 1 | up to 8 |
| planner caches | 1406 KB | 1125 KB |

So main breaks eight of the browser limits in ffa and seven in crowd.
Route search is the whole story: it is between six and fourteen times
its budget per tick, one search in the first window costs the better
part of a second, and cache rebuilds cost four seconds in that window
because the occupancy version moves on every structure imprint retry.
The stall census is not zero in either scenario, so units do get stuck.
Those are PRs 3, 4, 13, 14 and 16 of the plan, in that order.

Two more notes on reading these. In the browser every figure arrives as
a whole millisecond, because the page is not cross-origin isolated and
its clock is clamped, so a sub-millisecond tick reads as zero. And the
in-suite probes run the simulation without drawing, so their frame
columns are zero by design.

For the record, the in-suite Release numbers on the same machine, over
1200 ticks: ffa 9443 and 7948 ms of simulation per window with 8249 and
6803 ms of it in route search, crowd 9908 and 9485 ms per window with
5258 and 5498 ms in route search, and duel 20 windows with a worst tick
of 296.96 ms and a stall census of 2.
