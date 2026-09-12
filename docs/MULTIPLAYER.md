# Multiplayer design

Multiplayer is being built now. This document is the approved design. It
also corrects an earlier version of itself that described something else,
because contributors were working from claims that were not true.

---

## Correcting the record

The earlier draft of this page said three things that do not hold, and they
are worth naming rather than quietly deleting.

- It said positions are 16.16 fixed point and angles use the 65536 per turn
  convention. They do not yet. The simulation still keeps heading, sub pixel
  position, speed, mana and several accumulators as floats, and it calls
  `sqrtf`, `atan2f`, `sinf`, `cosf` and `floorf` on movement, facing,
  projectile and animation host paths. The fixed point conversion is real
  planned work, roughly 60 to 80 sites, and it is scheduled late on purpose.
  Until it lands, two native machines can disagree with each other, because
  the C runtime picks fused multiply add variants per processor.
- It said every player action is already a command. Not one is. The command
  layer in `src/net/commands.c` is 143 lines and has no caller anywhere
  outside its own test. The user interface calls `Units_Command*` directly
  and applies orders in the middle of a frame.
- It named UDP with a reliability layer as the native transport. Version one
  uses one WebSocket for every client, browser and native alike. An
  unreliable path can be added later behind the transport interface if
  measurement shows it is worth building.

The design below is what we are building.

---

## The shape of it

Deterministic lockstep, with the turn clock owned by a small relay server,
carried over one WebSocket on port 443 to every client. One protocol, one
server binary, one client, for Windows, macOS, Linux and the browser.

Up to eight seats per game, humans and computer players together, exactly as
the original counted them. Spectators sit outside the eight seats rather than
eating one.

Only player commands cross the wire, so bandwidth does not depend on army
size. A thousand unit battle costs the same as an empty map. Every client
simulates every player and every computer opponent from one seed.

### Why a relay and not the alternatives

- A server that ran the simulation would have to hold game data, which
  [ASSETS.md](ASSETS.md) forbids and which makes hosting awkward. It also
  costs far more bandwidth.
- Peer gossip, the way the original worked, is cheatable, needs every pair of
  players to reach each other, which is 28 links at eight players, and cannot
  run in a browser at all.
- A WebRTC mesh gives datagram delivery but it is those same 28 fragile
  links, it needs a signalling server and a relay for a large share of home
  networks, and its browser binding has no C interface.
- A star relay over a secure WebSocket reaches every network with no port
  forwarding, no UPnP and no NAT traversal, which is the whole point. In
  lockstep every command has to arrive anyway, so an unreliable transport
  buys little.

---

## The turn clock

This is the part that differs from classic lockstep, and it is why one
player's jitter does not stall the other seven.

The server closes a turn every 3 ticks, which is 50 milliseconds at 60 Hz and
20 turns a second, and broadcasts the bundle. It never waits for a client's
input. A command that arrives late lands in the next open turn.

Clients never simulate past the bundles they hold, so there is no fixed
global input delay to tune. Each client runs a small adaptive buffer of one
to three turns. A player 30 milliseconds from the server feels roughly 135 to
185 milliseconds of command latency, which sits inside the band that plays
well. The original applied orders the instant you clicked, so that delay has
to be covered by feedback rather than by the simulation. The order
acknowledgement voice, the cursor response and the waypoint marker all fire
on click, and only the unit's movement waits for its turn.

Game speed and pause are server pacing, not simulation state. The server
produces turns faster, slower or not at all. That is what the original did,
where the tick count was elapsed time multiplied by the speed level, and it
keeps speed and pause out of the state hash entirely.

A client that falls behind is handled by a governor rather than a stall. The
server watches each client's acknowledged turn and slows turn production when
one falls behind, which is the original's continuous throttle rather than a
hard wait. The original's "Slowing down to wait for" overlay stays.

The local half of that checksum is in the tree. `Units_DebugStateHash` and
`TAK_AI_DebugStateHash` are FNV-1a over the integer unit and AI state and
the AI's random cursor, in a fixed order, sampled every 60 ticks by
`test_movement`. They also take the bit patterns of the mover's heading,
speed and subpixel remainder, which are still floats. That makes the value
answer "did these two runs of this build diverge" and not yet "do these two
machines agree". Making it answer the second question needs the fixed-point
mover, which is the work this section asks for above.

---

## The relay core

The server is one small C binary built from a relay core that also links into
the engine's tests. The core is a pure state machine over a transport
interface with no sockets in it, which is what lets the eight client loopback
test run the real room and turn logic in one process, and what would later
let a native client host a game on a local network.

Its responsibilities are the room directory, authoritative room state, the
turn clock, turn logs for reconnect and replays, membership, spectators, chat
relay, rate limits and moderation. It never simulates, never holds game
state, and never reads game data.

One caveat on that last point, so the document stays honest. An operator may
configure a private instance to serve their own game files to an invited
roster, which is a separate process behind its own lock and off by default.
The public instance has no such configuration and shares nothing.

---

## The protocol

Binary WebSocket frames, one logical message per frame, little endian, every
field length checked, with a 64 KB cap the server enforces. Every message
starts with a type byte and a length, so a client that does not know a type
can skip it.

### Handshake

- HELLO from the client carries the protocol version, the engine build id,
  the determinism class, the schema hash, the content hash and per group
  hashes for units, weapons, features, scripts and computer opponent data, an
  expansion flag, a 128 bit device token, the display name, the client kind
  and a server access key when one is needed.
- WELCOME from the server carries the session id, the server name and flags,
  the message of the day, the protocol range it supports and the newest
  client build it knows about, which drives the update notice.
- REJECT carries a reason code. The codes reuse the original's vocabulary and
  add data mismatch, engine version, server password, banned and rate
  limited.
- PING and PONG go every 2 seconds, the original's heartbeat cadence,
  carrying timestamps. The round trip fills the battle room's Ping column.
  These are application messages because a browser page cannot send a
  WebSocket ping frame.

### Lobby

- LIST_ROOMS and ROOM_LIST, with the server pushing updates while a client
  sits on the Select Game screen.
- CREATE_ROOM, JOIN_ROOM by id or by a six character code with an optional
  password and a watcher flag, and LEAVE_ROOM.
- ROOM_EDIT changes one field. The server checks it against the editor's
  rights and then applies it.
- ROOM_STATE is a full snapshot with a revision number. Eight slots at about
  32 bytes each is small enough that snapshots beat deltas and remove a whole
  class of drift bugs.
- CHAT with a scope of room, everyone, team or one player.
- START from the host, which the server gates on the original's rules.

### Match start

- START_GAME carries the match id, the seed, the whole battle
  configuration including the map and its fingerprint, every rule toggle, the
  unit cap and all eight slots, the seat mapping, the turn length in ticks,
  and the content and schema hashes everyone agreed on. Start positions come
  from the seed inside the simulation rather than from the host.
- LOAD_PROGRESS from each client is relayed as LOAD_STATE, which feeds the
  load screen's seven remote rows.
- LOADED carries a hash of the freshly built world. Comparing those before
  the first tick catches a data mismatch while it is still an error message
  rather than a desync ten minutes in.
- GO starts turn zero.

### In game

- CMD from a client carries a sequence number and up to 64 commands. The
  client does not send its player id or an execution tick. The server stamps
  the seat, which is also the rule that stops order forging, and the turn
  decides the tick.
- TURN from the server carries the turn number and that turn's commands
  tagged by seat. Empty turns are a few bytes and consecutive empty turns
  collapse into a range.
- ACK from a client carries the last turn simulated, plus a state hash every
  60 ticks. The field is 64 bits wide. The simulation hash in
  include/tak_sim_hash.h is 32 bits today, so a client zero extends it and a
  wider hash later needs no change to the protocol. The server compares
  hashes from different clients without holding any simulation of its own.
- PACE carries the speed level, the paused flag, the reason and the lagging
  seat. Pacing is timing only and never enters the hash.
- PLAYER_STATUS carries connected, lagging, lost, catching up or dropped,
  with the countdown when one is running.
- The server injects system commands into the turn stream so every simulation
  applies them on the same tick. Those are a player leaving with its
  disposition, a returning player reclaiming their army from the computer,
  and the end of match marker.

Chat rides its own channel rather than the turn stream, so it is never
delayed by a turn and cannot affect the hash. The server stamps each line
with the turn number and writes it into the server side replay. The
original's rules stay: 256 characters, watcher chat reaching only watchers,
direct messages, and the 30 line ring.

### Versioning

The protocol version is negotiated in HELLO and the server supports a range.
Simulation compatibility is a separate thing carried per room as the host's
engine build id, determinism class and content hash. Rooms you cannot join
are listed and greyed with the reason rather than hidden, which is the one
thing the original got wrong here. It dropped mismatched sessions from the
list without a word, leaving players with no idea why a friend's game was
invisible.

---

## Determinism

Every client must simulate bit identically. These rules are binding on
simulation code.

- No floating point in simulation state. This is the goal and not yet the
  state of the tree. Positions, angles, speeds and accumulators become fixed
  point during the conversion work, with 16.16 for position and 65536 per
  turn for angles. Floats stay fine for rendering.
- One seeded generator, advanced only by the simulation, never by rendering,
  the interface or audio.
- No wall clock in the simulation. The tick counter is the only clock.
- No dependence on pointer values, hash order or allocation addresses. Unit
  definition order comes from a case folded sort of the unit name rather than
  from whatever order the filesystem hands back.
- No uninitialised reads. A field that is garbage on one machine and zero on
  another diverges the match.

Because the browser build is already float safe, and two native machines are
not yet safe with respect to each other, the join handshake carries a
determinism class and only clients of the same class share a room. The
browser class ships first and serves Windows, macOS and Linux players
immediately. Native clients join the same rooms once the fixed point work
passes a golden hash check on every build target.

### Catching a desync

Every client hashes at ticks divisible by 60 and keeps a 60 tick ring of per
subsystem hashes. The server compares them. On a mismatch it names the tick
and the disagreeing seats. With three or more simulations in the room,
spectators included, the majority continues and the outlier is put into catch
up, which resyncs it by replaying the turn log from the start. With only two,
the match halts with a report rather than letting two diverged worlds play
on. Each client writes a desync bundle locally holding the replay, its hash
trace and the per subsystem breakdown at the mismatch tick, which is a
complete reproducer.

### The data fingerprint

Players supply their own game files, and those files legitimately differ by
release, patch level, expansion and mods. Any of those differences changes
the simulation.

So we hash the parsed simulation inputs rather than the archives byte for
byte. Archives can differ in packing and ordering while producing identical
behaviour, and hashing raw bytes would reject compatible players for no
reason. Inputs are canonicalised first, sorted by name and case folded,
because the original's data parsing ignores case. Two values come out.
`content_hash` covers the data. `schema_hash` covers our parser and
simulation version, so an engine change that reads identical bytes
differently is caught too.

The map is separate and has a fingerprint of its own, described next.

A mismatch names the group that differs rather than saying only that
something is wrong, and `--data-report` dumps per file hashes so two players
can find the single file at fault.

This is not a piracy check and must never be described as one. It proves two
players have the same data, not where they got it.

### The map fingerprint

The map gets a fingerprint of its own, because players collect maps from
everywhere and two maps with the same name are not the same map. It is a
SHA-256 over the content that decides how a battle plays, and it is
implemented in `include/tak_map_fingerprint.h` and
`src/game/map_fingerprint.c`. The engine hashes, in this order:

- the `.tnt`, byte for byte, which is the terrain, the heights and the
  features
- the `.ota` in canonical form, without `missionname` and
  `missiondescription`, which are labels a player can retype without
  changing the battle
- the `.crt` byte for byte, when the map has one
- the map's `.tdf` in canonical form, when the map has one

The `.txt` is left out: it is the blurb shown beside the map.

Canonical form of a TDF text, which the map editor has to reproduce if a
save is not to change the fingerprint:

- Blank lines and `//` comment lines are dropped.
- Section and key names are lower cased. Values are trimmed of leading
  and trailing blanks and otherwise kept as authored, since a value is
  content.
- Sections keep the order the file gives them, because the engine spawns
  units and reads start positions in that order.
- Keys inside a section are sorted by name, and two keys of the same name
  keep their file order, because the parser takes the first of them.
- Each section is written as `[name]`, `{`, its keys as `key=value`, then
  its subsections, then `}`, one per line.

The hashed stream is `OKMAP1`, then one line per part naming it and its
byte count, then the bytes, with `-` for a part the map does not have.
Framing the parts this way keeps a map with a long `.tnt` and no `.crt`
from colliding with one that splits the same bytes differently.

Two players whose fingerprints match have the same map whether one holds
it in `maps.hpi`, another in a `.kmp` map pack and a third as loose
files. `map_inspect --fingerprint` prints one line per installed map, so
two players can diff their lists and see which map differs.

The fingerprint has to be identical on every build, so
`scripts/fingerprint-wasm-check.sh` compiles the same code with
Emscripten and runs its tests under node against the same golden values
the native suite uses.

The relay never computes a fingerprint. It carries the 32 bytes the host's
client computed, in the room state and in START_GAME, and a room cannot
start until it has one.

---

## Drops, stalls and reconnect

Transient jitter costs nothing, because the turn clock does not wait.

A client whose simulation falls behind triggers the governor. The server
slows turn production when a client is more than half a second behind for
more than two seconds. The original waited four seconds, which made sense
when a lagging peer only meant stale copies of its units. Here it means that
player issuing orders against a stale view, so we react sooner.

Silence for five seconds means a lost connection. The server pauses the match
and opens the original's reject dialog with its countdown, which the room
option bounds to the original's 30 to 150 seconds. The countdown is server
owned, so no two clients can disagree about the outcome. The host may reject
early. If the player returns inside the window, the match resumes at once and
they catch up from the turn log.

On a timeout or an early reject the server injects the player leaving into
the turn stream and the match resumes. A room option decides whether the
computer takes the army over or the army is removed, which is what the
original did.

A dropped player can rejoin later. Select Game offers Rejoin for their device
token, and they fast forward the whole turn log with no rendering behind the
load screen's progress bar, without pausing anyone else, then reclaim their
seat from the computer.

---

## What we are not doing

Playing against people running the original game on GameRanger is a non goal,
recorded in [MANUAL_DEVIATIONS.md](MANUAL_DEVIATIONS.md) under N-001. Four
things stand in the way and none of them move. GameRanger is closed. The
original's compatibility id is a checksum of the running executable file, so
our engine cannot present a matching one without faking a checksum of a file
it is not. Reaching the session at all would mean a complete DirectPlay peer
implementation, which a browser cannot open sockets for anyway. And a mixed
game could not be lockstep, because original peers never send orders and
never wait for a turn.

The way to reach that community is to be easier to join than they are. They
already know these screens, and a link puts someone in a battle room with no
install and no lobby program.

Lockstep also cannot stop a map hack. Every client holds the whole world, so
a modified client can reveal it. Ownership and command validation are
deterministic and enforced everywhere, which stops order forging, and the
state hash catches simulation tampering. That is the honest boundary.

---

## Hosting your own

One small binary, one port, and a config file. No database and no game data.
A generic Dockerfile, a compose file and an example config ship with the
release. Anything about a particular deployment, its domain or its keys stays
out of this repository.

---

## Contributing here

Good entry points, roughly in the order they unblock other work:

- Determinism auditing. Find a float, a `rand()` or a `time()` call reachable
  from simulation code. Each one is a real bug.
- Taking the piece hierarchy out of the renderer so a headless target can
  step the simulation with no window.
- The state hash and its per subsystem breakdown.
- Replay recording and playback from the turn log.
- Turning a player action into a command. There are a lot of them and each
  one is a small, self contained change.

See [CONTRIBUTING.md](../CONTRIBUTING.md).
