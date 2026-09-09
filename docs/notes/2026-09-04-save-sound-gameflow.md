# Behaviour note: save/load, sound events, game flow, PRNG (2026-09-04)

Derived from behavioural analysis of the retail binary. `:NNNNN` citations are
evidence pointers into a private reference that is not distributed — they mark where a claim can be re-checked by whoever holds it,
and nothing here reproduces that file's expression.

Symbol names marked *(tool-assigned, misleading)* are automatic labels
that do **not** describe what the routine does; the description beside them
does. This region is unusually badly labelled — save serialisers appear as
render functions, network code appears as save code.

## Critical correction

The whole `GameSave_*` symbol cluster (:195119-199360) contains **zero**
savegame code *(tool-assigned, misleading)*. It is three unrelated
things:

1. Network packet marshalling — :195119-195460
2. Player colour-slot allocation — :196155-196395
3. `Kingdoms.ini` settings persistence — :199006-199360

The **real** savegame system is the HapiBank key/value bank. Saves carry the
magic string `"Kingdoms Saved Game v1"` and are written to `savegame\%s.sav`
(format string at :1524).

## A. Save / load (the real system)

### Top level

| Operation | Entry point |
|---|---|
| Master save | `SaveGame_WriteSummary(save, description, gameId)` :164912-165007 |
| Master load | `SaveGame_LoadSummary` :164888 |
| Summary-only open (for the save-browser list; filters to the `Summary` section) | `SaveGame_OpenSummary` :164823 |
| Full open | :164862 *(the reference tooling labels it as a palette loader — misleading)* |
| Close | :164850 |

### HapiBank container

A HAPI key/value archive. The operations, with citations:

| Operation | Citation |
|---|---|
| Init | :253054, :253130 |
| Free | :253063 |
| OpenArchive | :253144-253406 |
| WriteEntry | :253407 |
| LoadAccount | :253796 |
| FindOrAddSection | :253908 |
| SetInt / SetInt64 / SetString | :253952 / :253977 / :254002 |
| GetInt / GetFloat / GetString | :254029 / :254048 / :254067 |
| KeyExists | :254086 |
| Key-table operations | :254097-254238 — note that get-key-by-index is a **positional blob stream**, not a lookup |
| GetSectionCount | :254264 — returns the **byte length of the current blob key** and is used as a size guard, despite the name |
| CompressLZW / Decompress | :272156 / :273081 |
| AuditFile | :254379 |

### `[Summary]` section

The summary is what the save browser reads without opening the rest of the
file. Keys, in write order:

| Key | Value | Condition |
|---|---|---|
| `maxunits` | The FavoriteUnitsPerPlayer option, 20..500 | Always. Also the **only** back-compatibility probe on load — older banks are detected by key absence |
| `Campaign` | Campaign id | Always |
| `Mission` | Mission id | Always |
| *(player dialog button)* | UI selection state | Always |
| *(side name)* | Player's side | Always |
| `Players` | Player count | Always |
| `Gametype` | 1 = campaign, 2 = skirmish/MP, 3 = in-battle | Always |
| `CommanderDeath` | The battle option the options screen calls **MonarchDeath** | Gametype == 2 |
| `Location` | The **RandomStartLocations** battle option | Gametype == 2 |
| `Mapping` | The **MappingOn** battle option | Gametype == 2 |
| `LineOfSight` | The **LosOn** battle option | Gametype == 2 |
| `BetweenMissions` | 1 | Only when saving between missions |
| `Victory` | Bit 2 of the game-state mission-result flags | Only when saving between missions |
| `Description` | Save description string | Always |
| `Game ID` | Game id | Always |
| `Game Time` | The elapsed game-time counter | Always |
| `Radar Image` | Blob of `mapW × mapH × 2` bytes — the radar thumbnail | Gametype == 3 only (:164966-164975) |

Two conditional sections follow: `[UnitBalance]` with `V3Units=1` (:164986),
and `[CreonUnits]` with `CreonUnits=1` when any player is side 7 (:164995).

### Per-subsystem serialisers

All of these are *(tool-assigned, misleading)* — they are labelled as
render functions.

| Section | Save | Load |
|---|---|---|
| Units | :227938-228193 | :227585-227619 |
| PlayerFeatures | :226100-226132 | :226136-226177 |
| Mapping (fog) | :226181-226193 | :226197-226216 |
| Players | :206939-207010 | :207011-207060 |
| Camera | ~:120860-120960 | keys `PresetX%d` / `PresetZ%d` / `PresetUsed%d` / `X Position` / `Z Position` |
| MapScript | :177901 | :177916 |
| Victory/Defeat conditions | :239858-239886 (the objective interface's Save entry) | :239890-239918 (its Load entry) |

### `Units` section

Header keys: `Version` = **0x2C (44)** and `Number of Units` = N.

The writer walks the global unit pool — a contiguous fixed-stride array
delimited by two game-state pointers — and skips any unit that does not carry
the persist flag.

Each surviving unit becomes a **fixed 235-byte (0xEB) record**. Fields, in
order:

| Field | Notes |
|---|---|
| Unit type name | 32 bytes, copied from the type definition |
| Unit id | u16 |
| Is-commander flag | u8 |
| Build percentage | u16 |
| World position x, y, z | 20.12 fixed point |
| Fourth position-related value | Stored alongside the three above |
| Heading | Two separate stored heading values |
| Health | Two stored values (current and its companion field) |
| Target unit id | Validity-checked against the live pool on save |
| Attacker id | 0xFF is the "none" sentinel |
| Weapon states | 8 of them (:228106-228111) |
| Miscellaneous counters | 9 of them |
| Repacked bitfield | See below (:228126-228137) |
| Build-queue / cargo entries | 7 × u16, preceded by a count byte |
| Piece-transform records | 3 records of 16 bytes each |

**Bitfield repack** (:228126-228137): the unit's persistent flag word is not
stored raw. Bits 0..9 are shifted left by 4; bit 0xB is stored; bits 0xC..0x15
are stored; a nibble from a second flag field is stored; the low nibble of a
third field is stored; then the top nibble. **Transient render flags are
deliberately dropped** — they must not survive a save/load round trip.

**Orders**: two linked lists are walked — the unit's current order queue and
its patrol queue. Nodes are keyed `u%04xm%04x` (:228024-228034). The unit's
capture state is saved separately (:228037).

**COB VM state IS saved.** Each unit gets a `Script%i` key whose value is the
COB engine's serialised thread state (:228018-228020, via an entry point the
the reference tooling labels as a thread-killer — misleading; it is the serialiser). That
covers thread program counters, stacks, locals, signals and sleeps. Without
this a reload resumes every script from the top.

### Other sections

- **Mapping** — key `Data`. Blob is the per-cell player-visibility bitmask
  (one u16 per cell), length `mapW × mapH × 2` bytes, i.e.
  `(mapW × mapH × 2) / 4` dwords. **Load refuses on a size mismatch.**
- **PlayerFeatures** — key `Plotmap`, `(mapW × mapH) / 2` bytes, packing two
  nibbles per byte: the damage and burn values from the per-cell feature
  record array.
- **Players** — side, colour, ally mask, defeat flag, and the economy. **Mana
  lives here**, not in the units section.

### Load order (:164888)

Summary → Players → Camera → network fixups → PlayerFeatures → Mapping →
Units → objective `Satisfied`/`Celebrated` restore.

- Back-compatibility is by key absence; `maxunits` is the only probe.
- Units checks `Version == 0x2C`, then accepts a record length of **0xEB
  (current)** or **0xE9 (233 bytes, legacy)** and skips anything else, then
  instantiates at :227623.
- **Objective definitions are re-parsed from the mission TDF** — the bank only
  restores the fired flags. The `Celebrated` flag is what prevents the victory
  voice line replaying every time a save is reloaded.

## B. Sound events

### Soundclass TDF loader

`SoundClass_LoadAll` :221344-221483 *(tool-assigned, misleading: labelled
as sound shutdown)* reads `gamedata\soundclasses\*.tdf`.

TDF structure: **outer sections are classes** (e.g. `ARAKNIGH`), **inner
sections are actions**, and **keys are wav names**. The parsed model is a class
(name, subclass count, subclasses), a subclass (sound count, total weight,
action name, weight array, entry array), and an entry (pending name, resolved
sound index).

The section-level boolean key `prioritized` (:221394) changes what the keys
mean:

- `prioritized = 0` → every entry gets weight 1.0, uniform pick.
- `prioritized = 1` → **the key's float value IS the weight**.

Playback:

- Lookup by name — :221545.
- **Weighted variant pick** — :221705. A CRT `rand()` draw is scaled by
  `totalWeight / 32768` and the entry list is walked subtracting weights.
  **This uses the CRT generator, not the simulation PRNG** — see the PRNG
  section for why that matters.
- 2D play — :221569. Action lookup is a linear string compare over the action
  list, and an **unknown action falls back to subclass 0** (:221615) rather
  than going silent.
- 3D play — :221638.

### Order-acknowledgement sounds

`Unit_PlayOrderAck` :221247-221281 *(tool-assigned, misleading: labelled
as a volume setter)*.

1. Pick the sound class: if the global "active sound class" latch is set
   (non-negative) it wins; otherwise use the unit type's own `soundclass`
   index. If neither yields a non-negative class, play nothing.
2. Only proceed if the **UnitSounds** interface option is enabled.
3. Map the order code to an action name:

| Order code | Action name |
|---|---|
| 1, 2 | `attack` |
| 5, 6 | `guard` |
| 7 | `patrol` |
| 0x0E | `Move` |
| 0x0F | `select` |
| anything else | `default` |

The action names are matched by string compare against the TDF's inner section
names, so the capitalisation above is load-bearing.

Callers: :237806, :237949, :238647.

The latch itself is maintained by `Unit_SetActiveSoundClass` :221285
*(tool-assigned, misleading: labelled as a fade-out)*. Selecting a unit
latches that unit's class; clearing the selection resets the latch to −1. This
is what makes a mixed selection speak with one voice.

### Engine-side bare-WAV sounds

These bypass the soundclass system entirely and name a wav directly:

`SelectSquad` :122226+ · `CreateSquad` :122211 · `addbuild` :150087 ·
`oktobuild` / `notoktobuild` :243684 / :243688 ·
`immediateorders` / `specialorders` :151760 / :151772 ·
`unload` / `Load` :14559, :27811, :234558, :234575 ·
`AlarmMon` :221336 (monarch attacked; **rate-limited to one per 15000 ms**) ·
the per-unit `underattack_sound` FBI key with its `underattack_delay`
companion (:221319, keys at :4696-4697) · `"Victory Condition"` :240280 ·
`MessageArrived` :205815 · `Ally` :195010 · a `menubutton.wav` warm-up play at
:308075.

**Unit death, build-complete and arrival have no engine hooks at all** — those
sounds come from COB `play-sound` calls in the unit scripts.

### Miles mixer region (:307979-308930)

32 channel slots. The play entry point (:308347) takes a sample, volume,
priority, pan and a loop flag.

- **Voice stealing** via the free-channel search at :308165. A channel is
  stealable only if it is loaded, not looping, and its priority is **strictly
  lower** than the requested one. Among candidates it takes the lowest
  priority, then the oldest serial. If nothing is stealable the call **fails
  silently** — it does not queue.
- Volume range 0..127. Pan range 0..127 with 0x40 (64) as centre.
- Master volume goes through the Win32 `waveOut` volume API, from the interface
  volume option scaled by 1024 (a shift of 10).

### Music (:308412-308830) — there is no state machine

One global playlist, and that is all.

- Mode: 0 = off, 1 = sequential, 2 = shuffle (**shuffle is the default**).
- Track source: a scan of `Music\*.wav`, or `Track%d.wav` numbering when a flag
  selects it.
- Update :308579 — on stream end, advance. Mode 1 wraps sequentially; mode 2
  advances `(idx + 1) % count` through a permutation that was shuffled once
  with CRT `rand()` (:308656/:308706).
- Driven from the post-frame update :242266-242282, gated by a sound option;
  playback pauses on window focus loss :242255.
- **Win and lose are a plain 2D play of `"Victory Condition"`** — not a music
  transition.

### COB `play-sound` chain

COB opcode **0x10072000** (:306851) resolves the sound name through the script
engine's name table, pops one argument, calls the host's play-sound callback
and pushes the return value.

The host callback :223761-223781 *(tool-assigned, misleading: labelled as
sprite data free)* behaves as follows, given the flags word passed from the
script:

1. **Category = the low 3 bits of the flags word.** The same value doubles as
   the Miles priority, so category and priority are never independent.
2. **Loop flag = bit 5 of the flags word.**
3. Categories **0..6** are **LOS-gated**: the local player must be able to see
   the emitting unit.
4. Categories **0 and 1** carry an extra gate: they play **only if the unit is
   currently selected AND a free channel exists**. They never steal a channel.
5. Category **7** is the global/UI case — a 2D play at priority 7, full volume
   (0x7F = 127), honouring the loop flag.
6. Every other category is a 3D play at the unit's world position, at that
   category's priority.

A 2D-only variant of the same host exists at :178318-178328.

The chain from there: 3D-play-at-position :221204 → name-to-index lookup
against the global sound name table (32 bytes per entry, with its own count) →
extended 3D play :221131 → the Miles play entry point.

### Positional model (:221131-221200)

1. The map cell is the world position shifted right 20 bits (positions are
   20.12 fixed point). Off-map is silent.
2. Audibility depends on the LosOn option. With **LosOn == 0**, the local
   player's bit must be set in the per-cell visibility bitmask. With
   **LosOn != 0**, the per-player explored array must have a non-zero byte for
   that cell. Out of bounds is silent.
3. **Volume is 0x7F (127) inside the viewport and 0x40 (64) outside it — a flat
   2:1 duck with NO distance falloff.** This is not an approximation of
   attenuation; there is no attenuation.
4. Pan is linear across the viewport width, 0x40 (64) at centre, clamped to
   0..0x7F.

## C. Game flow

### Timing — 30 Hz, confirmed three ways

`0x1a5e0` = 108000 = 3600 × 30 (:21475); a `30/fps` expression at :243014;
900 ticks treated as 30 seconds at :240040.

The game-state timing block holds: the tick counter (incremented at :242441),
the last clock reading, the ticks issued this frame, the frame delta, a
**float fractional accumulator**, a hysteresis counter, a status bitfield, the
id of the laggiest peer and the tick delta against it.

Status bits, in order: bit 0 paused · bit 1 lag-throttled · bit 2 severe lag ·
bit 3 speed reduced.

`Timer_GetTicks` :263924 *(tool-assigned, misleading: labelled as a debug
assert)* converts milliseconds to ticks as `ms × baseRate(30) / 1000`.

### Sim-tick pacing (`GameLoop_CalcSimTicks` :242330-242421)

Per render frame, in order:

1. `delta` = now − last clock. **Debug substitution:** when cheats are enabled
   and the player count is non-zero, `delta` is replaced by the player count —
   a fixed-step debug mode, not a timing path.
2. `rate` = the current speed level × 0.1. The level is the interface speed
   option, range 0..20, with 10 as normal — so rate 1.0 at default.
3. **Lag throttle**: once lag reaches **120 ticks**, `rate` is multiplied by
   `max(1 − min(lag, 3600) / 3480, 0.01)`. Beyond **449** the severe bit is
   set. Lag is measured against the slowest peer.
4. `acc = delta × rate + carry`; `ticks = round(acc)`; `carry = acc − ticks`.
   The fractional carry is what keeps the simulation on wall-clock average.
5. While not paused: if `ticks < 6` the hysteresis counter decrements, and once
   it falls below **−100** the game speeds up automatically; otherwise `ticks`
   is **clamped to 5** and the hysteresis counter increments, and once it rises
   above **10** the game slows down automatically.
6. While paused, `ticks` is 0.

**Net effect: at most 5 simulation ticks per render frame, plus a fractional
carry, plus hysteresis-driven automatic speed adjustment.**

### Per-tick work (`GameLoop_SimulateTicks` :242425-242513)

Inside the tick loop, in order: unit position update, unit update, physics,
peer-data publish *(tool-assigned network name)*, **per-player AI**
*(tool-assigned, misleading: labelled as feature height)*, network game
state processing, simulation step, camera tracking, callback reset, heightmap
normal recalculation, debug markers, terrain tick, **the COB VM step** (the
script engine's per-tick entry point), and — only on a "full" tick — the
network sync flush.

Once per **frame**, outside the tick loop: the AI batch pass — **three**
consecutive entry points *(tool-assigned auto-numbered method names in
the `BYMaia` cluster, meaningless as labels)* — then feature pool reset, LOS
entry expiry, feature spawn from data, and the terrain dirty flush.

Sub-rates inside the tick: every 3rd tick = 10 Hz (:128381), every 10th =
3 Hz (:128638), every 30th = 1 Hz.

### Victory / defeat conditions

Parsed from the mission TDF by `MissionObjectives_LoadFromTDF` :239243-239850
*(tool-assigned, misleading: labelled as a timer reset)*.

**Victory conditions**: `KillEnemyCommander`, `DestroyAllUnits`,
`KillAllMobileUnits`, `BuildUnitType(s)`, `CaptureUnitType(s)`,
`KillAllOfType(s)`, `KillUnitType` (argument format `"%s %i"`),
`MoveUnitToRadius` (`"%s %i %i %i"`), `UnitTypePassesX` / `UnitTypePassesZ`
(`"%s %i"`), `VictoryTimerRunsOut`.

**Defeat conditions**: `CommanderKilled`, `AllUnitsKilled`, `UnitTypeKilled`,
`AllUnitsKilledOfType`, `AnyUnitPassesX` / `AnyUnitPassesZ`,
`DeathTimerRunsOut`.

Coordinates are scanned out of the argument string and then shifted left 20
bits into 20.12 fixed point. The literal `ANYTYPE` matches any unit.

Each objective is an object with **six entry points**, in this order:
`IsSatisfied` (victory is an AND over the whole victory list, :239922),
`onUnitKilled`, `onUnitBuilt`, `onCaptured` / passes-plane, `Save`, `Load`.
Each carries **two flags**: `Satisfied` and `Celebrated` — the second exists
purely to stop the victory voice line replaying after a reload. Save sections
are named `VictoryCondition_*` and `DefeatCondition_*`.

The skirmish monarch toggle is **separate** from the objective system: it is
the MonarchDeath battle option (:131653), stored in the summary as
`CommanderDeath`.

The "everyone else is dead" check is :240032-240063 — it has a **900-tick (30 s)
grace period** and scans 10 player slots. The local-defeat check is :240018.
There is a debug anti-stall hook at :240111 that forces a loss between 5 and 10
minutes when debug logging is on.

### Game speed and pause

The interface options carry a configured level and a current level, both 0..20,
default 10. `GameSpeed_SetLevel` :131724-131806 clamps the level and enforces a
**minimum of 1 while in battle**. The OSD prints "Game Speed Normal" at 10 and
`"%s %c%d"` with a sign otherwise. The `+` and `-` keys are :131808 / :131819;
automatic slow/speed comes from the hysteresis path above; reset is :131708.

**Speed enters the simulation only as `rate = level × 0.1`** — it changes how
many ticks are issued, never what a tick does. That makes it deterministic-safe
and therefore safe to expose in multiplayer.

Pause is bit 0 of the timing status bits; the pacing function forces `ticks = 0`
while it is set. Render and input keep running.

### Skirmish options (`GameOptions` section, reader :131611-131663)

| Key | Range / meaning |
|---|---|
| `side` | 0..7 |
| `MonarchDeath` | Monarch is expendable / not |
| `MultiTimeOutSecs` | 30..150 |
| `RandomStartLocations` | bool |
| **`LosOn`** | bool |
| **`MappingOn`** | bool |
| `CanDropPlayer` | bool |
| `AllowCheating` | bool |
| `DisableMapScript` | bool |
| `EnemySide` | side index |
| `CrusadesBalance` | bool |
| `FavoriteUnitsPerPlayer` | 20..500 |

**`LosOn` and `MappingOn` are two independent booleans, NOT a three-way enum.**
This matters because they select different data structures:

- `LosOn == 0` → circular/bitmask LOS through the per-cell u16 visibility mask
  (this is the array the `Mapping` save section stores).
- `LosOn == 1` → true LOS through the per-player explored byte array, which
  carries its own width and height.
- `MappingOn` is the "Map Revealed" toggle over the explored state (UI label at
  :139258).

Options screen :139199-139290. The MP checkbox set is MonarchDeath,
StartLocations, Mapping, LineOfSight, CheatCodes, Watching, GameStatus,
MapScript, SlowGame, CrusadesBalance. The SP labels are "Monarch Expendable",
"Random Start Locations", "Map Revealed", "Line of Sight", "Power Codes".

Cheats: `AllowCheating` gates the console (labelled "Power Codes" in the UI);
the cheats-enabled global also hijacks the frame timer into fixed-step mode
(:242345); cheat entry is at :243004; console commands are :36220-36400.

### PRNG — the determinism fact

**There are two generators, and they must never be confused.**

#### Simulation PRNG

A **Lehmer / Park-Miller MINSTD** generator, computed with **Schrage's
method** so the intermediate product never overflows 32 bits. Its state is a
single 32-bit global word *(the reference tooling names it after an unrelated
renderer-lost-device flag — misleading)* :6243. The generator itself is at
:254475-254486 and the seed initialiser at :254490.

Constants:

| Constant | Value |
|---|---|
| Multiplier (a) | **16807** |
| Modulus (m) | **2147483647** (2³¹ − 1) |
| Schrage folding constant (q = m / a) | **127773** |
| Seed-init XOR mask | **0x66E29572** |

Behaviour, precisely:

- **Seeding**: the new state is the supplied seed **XORed with 0x66E29572, then
  ORed with 1** — so the state can never be zero, and the low bit is always
  set.
- **Draw for a bound `n`**: if `n` is less than 2 the result is **0** and the
  state is not advanced. Otherwise the state advances one MINSTD step in
  Schrage form — the state times 16807, minus 2147483647 times the state
  divided by 127773 — and if the resulting state is less than 1 the modulus is
  added back. The returned value is the state **modulo n**.

Bit-exactness requirements — all four of these are load-bearing:

1. The multiply and the accumulation are **32-bit two's-complement with
   wraparound**.
2. The division by 127773 is a **signed** division.
3. The "add the modulus back" test is a **signed** comparison against 1 (not a
   test against zero, and not unsigned).
4. The final reduction is an **unsigned** modulo.

**The modulo bias is part of the specification.** `state % n` is not uniform
for values of `n` that do not divide the period, and the legacy game's
behaviour depends on that skew. A "corrected" unbiased draw will diverge.
Reproduce the bias.

This generator has roughly **500 call sites in the AI alone**, plus weapon
spread, decay timing and idle wander — anything that consumes it out of order
desynchronises everything after it.

**Seeding sources**:

| Source | Value | Citation |
|---|---|---|
| Battle start | QueryPerformanceCounter high + low — **non-deterministic** | :243076 |
| `-AutoStop` benchmark mode | 1234 (0x4D2), and **reseeded every tick** | :242438 and nearby |
| UI initialisation | 0xD431 | :94347 |
| Network session | The lobby session seed field | :213069 |

#### Presentation PRNG

The CRT `rand()`, seeded from `time()` at :243078. It drives the soundclass
variant pick, the music shuffle and the debug anti-stall hook.

**Never use `rand()` in simulation code, and never let non-simulation code
touch the simulation seed.** These are separate streams in the original and
must stay separate.

## Gap list vs current TAK-RE

**Sound**

1. `GameSound_UnitAction` / `WeaponHit` / `PlayUI` have **zero callers** — wire
   them up.
2. No order-acknowledgement dispatcher (the order-code table above) and no
   selected-unit voice latch.
3. **No COB play-sound host** — this is the single biggest missing piece of
   gameplay audio: death, build-complete, arrival and weapon sounds all come
   through it.
4. Missing the LOS gate, the selection gate and the never-steal rule.
5. Attenuation must be the binary 0x7F / 0x40 duck, not a continuous falloff.
6. No `underattack_sound` handling and no monarch alarm.
7. Verify `prioritized` handling and the fallback-to-subclass-0 rule in
   `soundclass.c`.
8. Browser music is a WASM-backend problem only — the `Music/` directory is
   loose, outside the HPI archives.
9. **Do not build a music state machine** — the original has none, and adding
   one is a parity deviation.

**Save / load**

10. Nothing exists yet.
11. Build the HapiBank container first, including LZW.
12. The 235-byte (0xEB) unit record at `Version` 0x2C.
13. COB VM serialisation is required — and is needed again for MP rejoin.
14. The `Celebrated` flag matters; skipping it double-plays victory lines.

**Game flow**

15. Verify our game loop matches accumulator + 5-tick cap + hysteresis.
16. Port the lag throttle for MP.
17. Port MINSTD exactly (including the bias) and confine `rand()` to
    presentation.
18. The battle config must carry all 12 `GameOptions` keys, with LosOn and
    MappingOn as two booleans backed by two separate arrays.
19. The victory/defeat condition system is a large gap — 11 victory types, 7
    defeat types, a six-entry-point objective interface.
20. Pause must stop the simulation only.

## Open questions

- The per-field byte widths inside the 235-byte unit record are not pinned down
  by this analysis — the field **order** and the total size are solid, but a
  first implementation should re-derive the widths at :227938-228193 and
  confirm the record sums to 0xEB.
- The `Mapping` blob length is recorded as `mapW × mapH × 2` bytes written as
  dwords; confirm the loader's size guard uses the byte count, not the dword
  count, before relying on the mismatch rejection.
