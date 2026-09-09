# Behaviour note: legacy multiplayer / HAPINET (2026-09-04)

Derived from behavioural analysis of the retail binary. `:NNNNN` citations are
evidence pointers into a private reference that is not distributed — they mark where a claim can be re-checked by whoever holds it,
and nothing here reproduces that file's expression.

Symbol names marked **†** are *tool-assigned and misleading* — the
automatic label does not describe what the routine does. The description beside
it is authoritative. This region is especially bad: the network codec is
labelled as formation code and the game-start handshake is labelled as a
savegame version getter.

## Headline: TAK is NOT lockstep

`docs/THIRDPARTY_DIRECTPLAY.md:91` calls the legacy protocol "deterministic
lockstep". That is **wrong**. Legacy TAK uses the Total Annihilation
**peer-authoritative** model: optimistic immediate local execution, continuous
authoritative state gossip, and adaptive framerate slowdown instead of turn
barriers.

Evidence, in order of strength:

- **Orders execute locally the moment they are issued**, in the same path that
  broadcasts them, with no acknowledgement or turn barrier in between
  (:234433-234472).
- **Absolute unit positions are transmitted**, not recomputed from shared
  input. Message 0x29 is sent at :234410-234427 and applied at :195077-195086.
- **Damage is transmitted** — message 0x2B feeds the damage application path
  directly (:195096-195098).
- A **bit-packed unit-state gossip packet** (type 0x2C) trickles roughly one
  sixth of the owner's units every tick, stamped with the sender's own 32-bit
  frame number (:236664-236723).
- **There is no wait-for-all-peers barrier anywhere.** The pacing function
  computes `lag = localFrame − oldestRemoteFrame` and simply scales the tick
  rate down (:242330-242400).
- **The RNG is never seeded identically across peers.** Seed synchronisation
  exists only in the `-AutoStop` benchmark mode (:254475-254495, :241551-241581,
  :242436-242440).

**Consequence: TAK-RE multiplayer is a lockstep redesign, not a port.** Roughly
15 of the original's 45 message types exist purely to replicate state
(0x0B, 0x24, 0x27, 0x29, 0x2A, 0x2B, 0x2C) and become redundant under lockstep.

## Architecture layers (bottom-up)

```
Game logic
    builds a message as [u8 type][payload], hands it to the broadcast entry
        |
Message routing  (:192820-196150)
    per-slot send  ·  receive pump †  ·  message dispatcher †
        |
netCondenser aggregation   (OPTIONAL — global sync-enabled flag, :199360-201310)
    NetBuffer (1066 B) -> PlayerFrameInfo (11 peers) -> frame assignment
    -> coalesce -> LZW -> XOR -> checksum
        |
HAPINET wrapper  (:268445-269750)
    34 thin wrappers over IDirectPlay4 / IDirectPlayLobby3
        |
DirectPlay (dplayx.dll)
```

† The receive pump and the message dispatcher both carry tool-assigned
names that describe unrelated player-management operations. See the function
table below for the specific labels.

**Two send paths**, selected by a global sync-enabled flag (:4988/:5222):

- **Raw** — one HAPINET packet per message, sent through the broadcast wrapper
  at :193665.
- **Condensed** — per-peer ring buffers, coalesced to at most 1066 bytes,
  LZW-compressed, and flushed once per tick from the simulation loop
  (:242487-242490). Entry point at :199643, flush at :199591.

**Reliability is toggled by bracketing**, not per-message: a guarantee flag is
turned on, the sends are issued, and it is turned off again. Four wrapper
variants do this at :193677, :193702, :193782, :193807.

**Player model**: 10 slots in a fixed-stride table in the game state. Each slot
carries a kind field — 1 = local human, 2 = local AI, 3 = remote — plus the
local slot index and a network-active bit held separately in the game state.

## HAPINET wrappers (:268445-269750) — corrected names

The reference tooling attached several symbols **one function off**. Each body carries
its own `str_HAPINET_*` debug tag, and that tag is authoritative. Every name in
the left column below is tool-assigned and misleading:

| Address | Tool-assigned name † | What it actually is |
|---|---|---|
| :268570 | `HapiNet_SendBroadcast` | **HAPINET_sendpacket** — IDirectPlay4::Send |
| :268624 | `HapiNet_PeekPacket` | **HAPINET_updategameinfo** — SetSessionDesc |
| :268657 | `HapiNet_ProcessPackets` | **HAPINET_createnewgame** — Open with DPOPEN_CREATE |
| :269098 | `HapiNet_CreateGame` | **HAPINET_joingame** — Open with DPOPEN_JOIN |
| :269168 | `HapiNet_JoinGame` | **HAPINET_enumconnections** — callback |
| :269299 | `HapiNet_GetConnectionInfo` | **HAPINET_enumproviders** — callback |
| :269574 | `HapiNet_GetPlayerId` | **HAPINET_enumplayers** |
| :269593 | `HapiNet_SetSessionData` | **HAPINET_createcompoundaddress** |
| :269612 | `HapiNet_GetSessionData` | **HAPINET_enumaddress** |

The remaining wrappers match their names. Defaults: **maxPlayers = 16**,
enumeration timeout **1500 ms** (:268848).

**HAPINET context** — a single global structure holding, in order: the session
name (180 bytes), the player's long name and short name (16 bytes each), the
application GUID (the TAK game-session GUID, :5148), a `DPSESSIONDESC2` (0x50
bytes), the last from-id and to-id, the COM interface pointers, a
`DPLCONNECTION`, and two default values.

## Game-side networking (:192090-196150)

| Address | Name † where misleading | What it does |
|---|---|---|
| :192098-192208 | `Formation_Assign` † | **NetCondenser::Receive** — checksum verify, de-XOR, LZW decompress |
| :192212-192295 | `Formation_Apply` † | **NetCondenser::Send** — LZW, XOR, checksum, simulated loss, send |
| :192869-192933 | `Network_BroadcastPlayerState` | Message 0x1E, a 156-byte PlayerInfo payload (157 bytes on the wire including the type byte) |
| :193183-193332 | `Network_HostGame` | Lobby-launch worker thread, **40 s timeout** |
| :193358-193608 | `Player_HandleNetPacket` † | The host/join entry point — version check, password, session GUID |
| :193727-193778 | `Network_BroadcastPacket` | Fan-out to every remote slot |
| :193978 | — | Allocates **an 8 KB receive buffer**, held in the game state |
| :194112-194135 | `Player_GetEndGameResult` † | Returns the **reject-reason strings**, codes 0-10 |
| :194477-194510 | `Player_InitAllSlots` † | **Drop timeout** — `timeoutSeconds × 30` ticks since the last packet from that slot |
| :194516-195115 | `Player_CountActive` † | **THE MESSAGE DISPATCHER** |
| :195322-195352 | — | Heartbeat, message 2, every 60 ticks |
| :195356-195378 | — | Round-trip-time reply; the result is stored on the sending slot |
| :195459-195704 | `GameSave_GetVersion` † | **The game-start handshake** (see below) |

The dispatcher indexes its handler table by `type − 2`, so valid message types
start at 2 (:194742).

## netCondenser / PlayerFrameInfo (:199360-201310)

| Address | Behaviour |
|---|---|
| :199413 | AddMessage — grows the buffer by `max(len, 0x200)` |
| :199504 | FindPlayerFrameInfo — 11 slots, stride 0x1044 |
| :199591 | Per-tick flush |
| :199659 | SetSendRate — pacing interval is `1000 / clamp(rate, 2, 30)` ms. A negative rate disables condensing entirely; a rate of 0 means 200 ms |
| :200218 | SendFrame — pacing gate. If it has fallen behind, it resynchronises to `now + pacing / 2` rather than trying to catch up |
| :200320 | QueueMessage — auto-flushes at **0x429 bytes** or **0x400 packets** |
| :200401 | NetBuffer::AddPacket — maximum payload **0x42A (1066) bytes** |
| :200665, :200694 | Condensed receive — reordering, gap detection, out-of-order stash |
| :201122-201308 | Out-queue flush / message splitter — spreads a batch across at most **30 frames** to de-jitter, and **drops surplus 0x2C messages past 512 per batch** |

## Wire format

**Condensed packet:**

```
[u8  kind]        3 = raw, 4 = LZW
[u16 checksum]    sum of all bytes from index 3 to the end
[body]            byte i is XORed with i
```

Send is :192212. Compression is skipped when the raw body is under 13 bytes,
when compressing yields no gain, or when a global compression-disable flag is
set. Receive is :192098, and a **bad checksum is a silent drop**. There is a
**built-in simulated packet-loss facility** at :192278-192288.

**Decompressed payload:**

```
[u32 frame_number]   0xFFFFFFFF = unordered, 0xFFFFFFFE = initial
[NetMessage ...]
```

**NetMessage framing** — two shapes:

- Fixed-length types **0x02-0x2B**: `[u8 type][payload]`, where the length
  comes from a static size table (:5253). Each table entry is 4 bytes: a u16
  size and a u8 flags byte, where **bit 0 = allowed in modes 1 and 2** and
  **bit 1 = allowed in mode 3**.
- Variable-length types **0x2C-0x2E**:
  `[u8 type][u16 total_len including the 3-byte header][payload]`.

Valid type range is 2..0x2E.

### Message types (dispatcher :194516)

| Type | Len | Meaning |
|---|---|---|
| 0x02 | 9 | Heartbeat/ping (every 60 ticks) plus RTT reply |
| 0x05 | 299 | CHAT — `[5][text]` |
| 0x07 | 7 | UNIT ORDER — `[7][u16 unitId][u16 targetId][u8 order][u8 flags]` (send at :234450) |
| 0x08 | 9 | Minimap ping |
| 0x09 | 11 | Projectile spawn (:227154) |
| 0x0C | 6 | Map-grid cell owner / building placement |
| 0x0E | 22 | COB script call — function id plus 4 arguments (:195232) |
| 0x0F | 6 | AimWeapon — heading and pitch, each ×256 |
| 0x10 | 4 | FireWeapon |
| 0x12 | 5 | Unit copy-position / transport |
| 0x14 | 1 | **GAME START (GO)** (:195666) |
| 0x18 | 3 | PAUSE / SPEED — `[0x18][0][paused]` or `[0x18][1][level]` (:131795) |
| 0x19 | 6 | PLAYER LEAVE / DEFEAT — `[u32 playerId][u8 reason]` |
| 0x1A | 5 | Player disconnected |
| 0x1C | 51 | START POSITIONS — 10 × `{u32 playerId, u8 startPos}` (:195624) |
| 0x1D | 5 | Start-position ACK |
| 0x1E | 157 | Full PlayerInfo |
| 0x21 | 10 | ALLY / DIPLOMACY — `[u32 from][u32 to][u8 state]` |
| 0x23 | ? | "has modified his executable" anti-cheat warning |
| 0x26 | 3 | READY — sets the ready bit on the sending slot |
| 0x27 | 5 | Frame / tick counter sync |
| 0x28 | 6 | Resource transfer (float) |
| 0x29 | 15 | ABSOLUTE UNIT POSITION — `[u16 id][i32 x][i32 y][i32 z]` |
| 0x2B | ? | Damage — feeds the damage application path |
| 0x2C | var | Bit-packed unit-state gossip (droppable under congestion) |
| 0x2D | var | Map / file transfer chunk (Boneyards only) |
| 0x2E | var | Reject packet |

**Reject reasons** (:194112, strings at :5136-5155): 5 = game full ·
6 = lost connection · 7 = missing unit · 8 = need newer version ·
9 = no watching allowed · 10 = creator left.

### Type 0x2C gossip bitstream (:236664)

A bitstream, not a byte structure:

| Bits | Content |
|---|---|
| 8 | Message type |
| 16 | Length placeholder, patched in afterwards |
| 32 | The sender's own simulation frame number |
| … | Per unit, repeated for approximately `(unitCount + 5) / 6` units taken round-robin: a 16-bit relative id, then a variable-width absolute id (its width is a global derived from the loaded unit-type count), then the unit's own serialised state |
| 16 | `0xFFFF` terminator |

The length is patched back into bytes 1..2 once the stream is closed. The
deserialiser is around :201640-201760.

**This mechanism is what makes the original tolerate non-determinism.** Every
unit's authoritative state is re-asserted by its owner roughly every six ticks,
so divergence is continuously corrected rather than prevented.

## Runtime behaviours

**Adaptive slowdown** (:242330). Lag is the local simulation frame minus the
lowest frame number reported by any remote slot.

- Lag below **120** frames → full speed.
- Otherwise the tick rate is multiplied by `max(0.01, 1 − min(lag, 3600) / 3480)`.
- Lag above **449** sets the SEVERE bit.
- Status bits, in order: bit 0 paused · bit 1 net slowdown · bit 2 severe ·
  bit 3 running below the requested speed.
- Ticks issued = `round(elapsedMs × speedLevel × 0.1 × slowdown + fractionalCarry)`.

**Disconnect handling.** Heartbeat every 60 ticks; the timeout is
`timeoutSeconds × 30` ticks, after which a "Waiting for player" modal appears
(the reject/drop GUI at :194444). On drop, message 0x19 is broadcast and the
slot is freed (:194242).

**Pause and speed.** Level 0-20, 10 = normal, with a **minimum of 1 in
multiplayer** (:131754). Non-hosts are blocked from changing it unless they are
hosting (:131751). The change is broadcast reliably. Tick rate is
`level × 0.1` (:242355).

**Determinism: none.** The simulation PRNG is a Lehmer/MINSTD generator with
multiplier 16807, seeded by XOR with 0x66E29572 then OR 1 — see the save/sound/
game-flow note for the full constant set and exactness requirements
(:254475, :254490). **No seed is ever transmitted.** The lobby field named
`g_byNetGameSeed` is a **misnomer** — it is a max-players field, not a seed.
The host computes the start-position shuffle using the **CRT** `rand()`
(:195542-195576) and broadcasts the *result* (message 0x1C) rather than a seed,
which is the only reason start positions agree at all.

**Start handshake** (:195459):

1. Wait until every remote slot has set its ready bit.
2. Host shuffles and assigns start positions.
3. Host broadcasts message 0x1C to each peer.
4. Wait for both the LOADED flag and the position ACK from every peer (each
   tracked in its own game-state field).
5. Host broadcasts message 0x14 (GO).

**Session enumeration** (:132990). Up to 100 records of 0x188 bytes each,
filtered by build id and by version (0x15). Record layout:

| Offset | Size | Field |
|---|---|---|
| 0x000 | 180 | Session name |
| 0x0B4 | 16 | Host name |
| 0x0C4 | 16 | Map name |
| 0x0D4 | 128 | Description |
| 0x158 | 4 | Build id |
| 0x15C | 12 | Session GUID |
| 0x168 | 4 | Current player count |
| 0x16C | 16 | Application GUID |

Note the 4-byte gap at 0x154 between the description and the build id — the
record is not tightly packed.

**Join validation** (:193511-193597), in order: build id and version → password
hash (stored on the slot's info block) → `DPSESSION_PASSWORDREQUIRED` (0x400).

**Chat.** Message 5. In-game entry at :137153/:137165 (clamped to 0x101 bytes),
directed chat at :194389. On receipt the text is pushed to the on-screen
message system on **channel 8 = chat** (channel 2 = system, 4 = warning) — both
the message-system and the local-echo entry points carry tool-assigned
names referring to features and particles †, which is nonsense; they are the
text HUD.

## TAK-RE implementation plan (lockstep redesign)

**Phase 0** — fix `THIRDPARTY_DIRECTPLAY.md` and `SPEC.md` §4.14 (six wrong
table entries).

**Phase 1 — transport abstraction.** `tak_net_transport.h`,
`transport_enet.c` (native, 2 channels), `transport_ws.c` (browser WebSocket
relay, everything reliable-ordered). About 10 functions: Init, Shutdown,
HostListen, Connect, Send, Broadcast, Poll, Disconnect. **Unreliable delivery
must be a pure optimisation** — correctness may never depend on it.

**Phase 2 — determinism substrate FIRST.** Reimplement the simulation PRNG and
its seed initialiser bit-exactly (constants and exactness rules are in the
save/sound/game-flow note). **Exchange the seed in the start handshake** — this
is a deliberate fix over the original, which never did. Audit every float and
double on simulation paths. Build a replay harness `test_determinism`: record
`(frame, commands[])`, replay it, and compare a per-frame `TAK_SimHash` taken
over unit id / position / hp / orders plus the frame number and the RNG state.

**Phase 3 — command pipeline.** Slim `TAK_GameCommand` (currently a fixed
1044 bytes; move to a variable unit list in an arena buffer). Add an
`execution_delay` of 3-5 frames at 30 Hz. CRC per serialised frame. Add a
`frame_hash` field to the buffer.

**Phase 4 — lockstep scheduler.** `TURN_LENGTH` of 4 ticks (~133 ms). Send the
buffer for `tick + TURN_DELAY`; stall when a peer's buffer is missing; apply
commands sorted by `(peer_id, sequence)`. **Port the original's adaptive
slowdown as the stall UX** (the 120 / 449 frame thresholds) and reuse its
de-jitter spreading when the turn spans more than one tick.

**Phase 5 — session layer.** A 10-slot player table mirroring the legacy slot
semantics (kind: local human / local AI / remote). Port the validation gauntlet
verbatim in *order* — build id → version → password → free slots → spectator
policy — and reuse the reject-reason enum. Port the start handshake, **with the
seed in the GO message**. LAN discovery by UDP broadcast; the browser build
queries the relay instead.

**Phase 6 — desync detection** (the original has none). Every 30 ticks send
`{tick, SimHash}` reliably. On mismatch, freeze and dump the unit tables. **No
auto-recovery** — a silent resync would hide the bug.

**Phase 7 — parity behaviours.** Chat bypasses the lockstep queue. Pause and
speed rules verbatim, including the MP minimum of 1 and the host-only
restriction. Disconnect: heartbeat every 60 ticks, timeout `seconds × 30`,
modal, then drop to AI or eliminate and continue with the reduced peer set.

**Phase 8 (only if needed)** — coalescing, a per-type size table, lz4 (skip the
XOR obfuscation entirely), and a droppable message class (irrelevant under
lockstep).

**New files**: `src/net/{transport_enet, transport_ws, session, lockstep, sync,
test_determinism}.c` plus matching headers. Wire into `src/CMakeLists.txt` next
to `net/commands.c` (line 79) and the test list (line 1104).

## Quick reference — line ranges that matter

| Range | What |
|---|---|
| 268445-269750 | HAPINET wrappers |
| 192098-192295 | netCondenser codec (wire format) |
| 192820-196150 | Session / join / host / chat / senders |
| 194516-195115 | Message dispatcher (type → handler) |
| 195459-195704 | Start handshake and position shuffle |
| 199360-201310 | PlayerFrameInfo / aggregation / pacing |
| 236664-236723 | 0x2C gossip bitstream |
| 242330-242512 | Adaptive slowdown and per-tick flush |
