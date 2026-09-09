# Security policy

## Reporting a vulnerability

Please report security issues privately through GitHub's
[private vulnerability reporting](https://github.com/OpenKingdoms/OpenKingdoms/security/advisories/new)
rather than opening a public issue.

We'll acknowledge your report and keep you updated on the fix. There is no
bug bounty — this is a volunteer preservation project — but you'll be
credited in the advisory unless you'd rather not be.

## Scope

OpenKingdoms parses a lot of untrusted-ish input: game data files, maps, replays
and network traffic. The things we most want to hear about:

- **Malicious game data.** A crafted `.hpi`, `.3do`, `.gaf`, `.cob` or
  `.tnt` file causing memory corruption. Custom maps get shared between
  players, so parser bugs have a real path to users.
- **Network input.** Anything a remote player or relay can send that
  crashes or corrupts a client — command frames, lobby handshakes, replay
  streams.
- **The browser build.** Sandbox escapes, or anything that reaches a
  player's files beyond the game folder they selected.
- **The relay server**, once released.

## Out of scope

- Cheating in multiplayer. Lockstep gives clients full game state by
  design; map hacks are a known and accepted property of the architecture,
  as they were in the original.
- Crashes from *your own* malformed or modded data files in single player.
  Report those as ordinary bugs — they're worth fixing, they're just not
  security issues.
- Anything requiring an attacker to already have code execution on the
  machine.

## Supported versions

The latest release and `main`. Older releases don't get backported fixes.
