#ifndef TAK_PERF_PROBE_H
#define TAK_PERF_PROBE_H

/* Performance probes (--perf-probe <scenario>).
 *
 * A scenario sets up a fixed battle, runs it for a fixed number of sim
 * ticks and prints one line per 600 ticks plus a closing line. The
 * fields are described in docs/notes/2026-09-11-perf-probes.md.
 * Nothing here runs unless a scenario is selected. */

struct GameWorld;
struct TAK_Platform;

/* "ffa" or "crowd". Returns 0, or -1 for an unknown name. */
int  PerfProbe_Select(const char *scenario);
/* Shorten a run (--perf-ticks). Call before the world is built. */
void PerfProbe_SetTicks(int ticks);
int  PerfProbe_Active(void);
int  PerfProbe_Finished(void);

/* Build the scenario's battle config and begin its map load. */
int  PerfProbe_BeginWorld(struct TAK_Platform *plat);

/* Measure a world somebody else set up, for probes that live in a
 * test. No spawning, no orders. */
void PerfProbe_BeginMeasureOnly(const char *label, int ticks);
void PerfProbe_Stop(void);
/* End a run early. Prints the part window and a done line saying why,
 * so a probe that is cut short still reports what it measured. */
void PerfProbe_Finish(const char *reason);

/* Sim hooks. BeforeTick runs the scenario's own spawns and orders, and
 * is not counted in the tick time reported by AfterTick. */
void PerfProbe_BeforeTick(struct GameWorld *world);
void PerfProbe_AfterTick(struct GameWorld *world, double tick_ms);
/* Ticks consumed by one frame, and the catch-up cap they ran against. */
void PerfProbe_FrameTicks(int ticks, int cap);
void PerfProbe_EndFrame(double frame_ms);

/* Read back for tests. */
int  PerfProbe_Windows(void);
int  PerfProbe_Ticks(void);
int  PerfProbe_LastUnits(void);
int  PerfProbe_LastStall(void);
int  PerfProbe_Spawned(void);

#endif /* TAK_PERF_PROBE_H */
