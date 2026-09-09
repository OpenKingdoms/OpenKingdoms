# Multiplayer design

**Status: in development.** The command wire format and the simulation are
in place; the transport, lobby and session management are not. This document
describes the design so contributors can work against it.

---

## Why lockstep, when the original wasn't

The original game did not use lockstep. Its network layer was
peer-authoritative — peers gossiped state and the session adapted its speed
to the slowest participant, over DirectPlay. That approach was reasonable in
1999 and is a poor fit now: DirectPlay is gone, it doesn't survive NAT, it
can't run in a browser, and peer-authoritative state is trivially cheatable.

OpenKingdoms uses **deterministic lockstep** instead. This is a deliberate,
documented deviation from the original — one of the few — and it's recorded
in `docs/MANUAL_DEVIATIONS.md` alongside the others.

What lockstep buys us:

- **Tiny bandwidth.** Only player commands cross the wire, never unit
  positions. A 1000-unit battle costs the same as an empty map.
- **A dumb server.** The relay forwards command frames. It holds no game
  state, runs no simulation, and — importantly — **needs no game data**,
  which keeps hosting legally uncomplicated (see [ASSETS.md](ASSETS.md)).
- **Cheap hosting.** A small VPS handles many concurrent games.
- **Replays for free.** A recorded command stream replays the whole match.
- **Browser support.** A WebSocket relay works everywhere.

What it costs: every client must simulate **bit-identically**. That
constraint drives most of what follows.

---

## The command stream

Commands are already defined in `include/tak_commands.h`
(`TAK_COMMAND_WIRE_VERSION 1`). Every player action — move, attack, build,
patrol, guard, load, unload, stop, aggro and weapon changes — is a
`TAK_GameCommand` carrying the issuing player, the affected unit ids, target
coordinates and, crucially, an `execute_tick`.

Commands are gathered per tick into a `TAK_CommandBuffer` and serialised
with `TAK_CommandBufferSerialize`. Nothing else is sent.

### Turn scheduling

Clients don't execute a command when they issue it. They schedule it for a
tick far enough ahead that every peer has received it first:

```
issue at tick T  →  execute at tick T + delay
```

The delay is derived from the worst round-trip in the session. A client may
only advance to tick N once it holds every player's command buffer for N —
which is what keeps the simulations identical, and also what makes one
player's stall everyone's stall. Mitigations (input delay tuning, a brief
grace period before dropping a peer) come with the transport work.

---

## Determinism rules

These are binding on all simulation code, and CodeRabbit is configured to
flag violations:

- **No floating point in simulation state.** Positions are 16.16 fixed
  point; angles use the original's 65536-per-turn convention. Floats are
  fine for rendering, never for anything a peer also computes.
- **One seeded RNG**, advanced only by the simulation, never by rendering,
  UI or audio.
- **No wall-clock time** in simulation. The tick counter is the only clock.
- **No iteration order dependence** on pointer values, hash order or
  allocation addresses.
- **No uninitialised reads.** A struct field that's garbage on one machine
  and zero on another diverges the match.

A divergence is usually invisible for a minute and then obvious: units
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
    map_hash         u64    the selected map
}
```

### The data fingerprint

Players supply their own game files, and those files legitimately differ:
retail releases, patch levels, GOG versus disc, expansion present or absent,
mods installed. Any of those differences changes the simulation and desyncs
the match.

So we hash the **parsed simulation inputs** rather than the archives
byte-for-byte — the archives can differ in packing, ordering and metadata
while producing identical simulation behaviour, and hashing raw bytes would
reject compatible players for no reason. Hashed:

- every unit definition (`.fbi`) and its resolved weapon and move-class
  references
- weapon tables (`.tdf`)
- move-class and terrain-type tables
- the selected map's heightmap and feature placement
- simulation constants (tick rate, fixed-point scale)

Inputs are canonicalised before hashing — sorted by name, whitespace
normalised, case folded, since the original's data-file parsing is
case-insensitive — then fed to a stable 64-bit hash. Two values come out:

- **`content_hash`** over the data itself.
- **`schema_hash`** over our parser and simulation version, so that an
  engine change which reinterprets identical bytes is also caught. Without
  it, two builds that read the same file differently would both report a
  matching content hash and desync anyway.

The host compares both against its own and refuses mismatched joins,
naming *which* group differs — units, weapons, or map. "Your data doesn't
match" with no detail is a support burden; `OpenKingdoms --data-report` dumps
per-file hashes so two players can diff and find the single file at fault.

**This is not an anti-piracy check and must never be described as one.** It
proves two players have the *same* data, not where they got it. Its job is
desync prevention, nothing more.

**Modding:** the fingerprint must not make mods impossible. Matched-hash is
the default and is required for public listings; a host can opt a private
game into accepting any data, at their own risk. The lobby shows the hash so
players can see at a glance whether they match.

---

## Transport

- **Native clients:** UDP with a small reliability layer for command
  frames, falling back to TCP where UDP is blocked.
- **Browser clients:** WebSocket to the same relay.
- **The relay:** forwards command frames between session members, tracks
  membership and turn completion, and holds nothing else. No simulation, no
  game state, no game data.

Because the relay is this simple, running one for your friends is expected
rather than exotic, and hosting instructions ship with the release.

---

## Hosting your own

Coming with the multiplayer release. The intended shape: a single small
binary, one port, a config file listing session limits. No database, no
game data, no assets.

---

## Contributing here

This area is wide open. Good entry points:

- Determinism auditing — find a float, a `rand()`, or a `time()` call
  reachable from simulation code. Each one is a real bug.
- The state-checksum comparison and its desync report.
- The fingerprint implementation described above.
- Replay recording and playback from the command stream.

See [CONTRIBUTING.md](../CONTRIBUTING.md).
