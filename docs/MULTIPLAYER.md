# Multiplayer design

Multiplayer is in development. The command wire format and the simulation are
in place. The transport, lobby and session management are not. This document
describes the design so contributors can work against it.

---

## Why lockstep, when the original wasn't

The original game did not use lockstep. Its network layer was
peer-authoritative, with peers gossiping state over DirectPlay and the
session adapting its speed to the slowest participant. That approach was
reasonable in 1999 and is a poor fit now. DirectPlay is gone, it doesn't
survive NAT, it can't run in a browser, and peer-authoritative state is
trivially cheatable.

OpenKingdoms uses deterministic lockstep instead. This is one of the few
intentional departures from the original, and it's recorded in
`docs/MANUAL_DEVIATIONS.md` alongside the others.

What lockstep buys us:

- Bandwidth is tiny. Only player commands cross the wire, never unit
  positions. A 1000-unit battle costs the same as an empty map.
- The server is dumb. The relay forwards command frames. It holds no game
  state, runs no simulation, and needs no game data, which keeps hosting
  legally uncomplicated (see [ASSETS.md](ASSETS.md)).
- Hosting is cheap. A small VPS handles many concurrent games.
- Replays come for free. A recorded command stream replays the whole match.
- Browsers work. A WebSocket relay reaches them everywhere.

The cost is that every client must simulate bit-identically. That constraint
drives most of what follows.

---

## The command stream

Commands are already defined in `include/tak_commands.h`
(`TAK_COMMAND_WIRE_VERSION 1`). Every player action, meaning move, attack,
build, patrol, guard, load, unload, stop, aggro and weapon changes, is a
`TAK_GameCommand` carrying the issuing player, the affected unit ids, target
coordinates and an `execute_tick`.

Commands are gathered per tick into a `TAK_CommandBuffer` and serialised
with `TAK_CommandBufferSerialize`. Nothing else is sent.

### Turn scheduling

Clients don't execute a command when they issue it. They schedule it for a
tick far enough ahead that every peer has received it first:

```
issue at tick T  →  execute at tick T + delay
```

The delay is derived from the worst round-trip in the session. A client may
only advance to tick N once it holds every player's command buffer for N.
That is what keeps the simulations identical, and it is also what makes one
player's stall everyone's stall. Mitigations (input delay tuning, a brief
grace period before dropping a peer) come with the transport work.

---

## Determinism rules

These are binding on all simulation code, and CodeRabbit is configured to
flag violations:

- No floating point in simulation state. Positions are 16.16 fixed point,
  and angles use the original's 65536-per-turn convention. Floats are fine
  for rendering, never for anything a peer also computes.
- One seeded RNG, advanced only by the simulation, never by rendering, UI or
  audio.
- No wall-clock time in simulation. The tick counter is the only clock.
- No iteration order dependence on pointer values, hash order or allocation
  addresses.
- No uninitialised reads. A struct field that's garbage on one machine and
  zero on another diverges the match.

A divergence is usually invisible for a minute and then obvious, with units
occupying different positions on different screens. Debugging that after the
fact is grim, so a periodic state checksum (a hash of unit positions,
healths and the RNG cursor) is exchanged and compared. On mismatch the
session halts and reports the tick, rather than letting players continue in
diverged worlds.

---

## Joining a game: the handshake

Lockstep requires identical simulation inputs, so the join handshake checks
that before anything else.

```
JOIN {
    protocol_ver     u16    wire compatibility
    engine_ver       str    build identifier
    schema_hash      u64    our parser/simulation version
    content_hash     u64    the player's game data
    map_fingerprint  u8[32] the selected map
}
```

### The data fingerprint

Players supply their own game files, and those files legitimately differ:
retail releases, patch levels, GOG versus disc, expansion present or absent,
mods installed. Any of those differences changes the simulation and desyncs
the match.

So we hash the parsed simulation inputs rather than the archives
byte-for-byte. The archives can differ in packing, ordering and metadata
while producing identical simulation behaviour, and hashing raw bytes would
reject compatible players for no reason. The hash covers:

- every unit definition (`.fbi`) and its resolved weapon and move-class
  references
- weapon tables (`.tdf`)
- move-class and terrain-type tables
- the selected map's heightmap and feature placement
- simulation constants (tick rate, fixed-point scale)

Inputs are canonicalised before hashing (sorted by name, whitespace
normalised, case folded, since the original's data-file parsing is
case-insensitive) and then fed to a stable 64-bit hash. Two values come out:

- `content_hash` covers the data itself.
- `schema_hash` covers our parser and simulation version, so that an engine
  change which reinterprets identical bytes is also caught. Without it, two
  builds that read the same file differently would both report a matching
  content hash and desync anyway.

The host compares both against its own and refuses mismatched joins, naming
which group differs: units, weapons, or map. "Your data doesn't match" with
no detail is a support burden, so `OpenKingdoms --data-report` dumps
per-file hashes and two players can diff them to find the single file at
fault.

This is not an anti-piracy check and must never be described as one. It
proves two players have the same data, not where they got it. Its job is
desync prevention and nothing more.

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

The fingerprint must not make mods impossible. Matched-hash is the default
and is required for public listings, and a host can opt a private game into
accepting any data, at their own risk. The lobby shows the hash so players
can see at a glance whether they match.

---

## Transport

- Native clients use UDP with a small reliability layer for command frames,
  falling back to TCP where UDP is blocked.
- Browser clients use a WebSocket to the same relay.
- The relay forwards command frames between session members, tracks
  membership and turn completion, and holds nothing else. No simulation, no
  game state, no game data.

Because the relay is this simple, running one for your friends is expected
rather than exotic, and hosting instructions ship with the release.

---

## Hosting your own

This comes with the multiplayer release. The intended shape is a single
small binary, one port, and a config file listing session limits. No
database, no game data, no assets.

---

## Contributing here

This area is wide open. Good entry points:

- Determinism auditing. Find a float, a `rand()`, or a `time()` call
  reachable from simulation code. Each one is a real bug.
- The state-checksum comparison and its desync report.
- The fingerprint implementation described above.
- Replay recording and playback from the command stream.

See [CONTRIBUTING.md](../CONTRIBUTING.md).
