OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

## Changed in 0.1.2

Terrain draws again on macOS and Linux. In 0.1.1 a skirmish loaded with
every ground tile black, the unit portrait blank and the build buttons
grey, because the picture decoder asked the video library for a codec
the release did not carry. Windows was unaffected. The decoder no longer
depends on the video library, and a release now proves it can decode a
picture before it packages anything.

The Linux archive needs nothing but the C runtime. The 0.1.1 one carried
a system SDL that required nineteen desktop libraries, so a minimal or
older distro failed before the game started.

The cut scenes play. The credits and the loading picture looked for their
clips next to the machine that built the game rather than next to your
copy, so no downloaded build had ever shown them.

The main menu doors move smoothly. Each door's clips are opened once and
played at their own rate, where before every crossing of the cursor
decoded a whole clip mid frame and the animation stuttered to catch up.
The hover clip loops as the original's does, and the intro and the logo
play where the original plays them. Skip the logo with `--skip-logo`.

Building sparkles follow the original: each rises or falls at its own
speed from a ring the size of the model, rather than a single rising
circle.

A building you ordered no longer dies before the builder arrives. A frame
was counted abandoned ten seconds after placement unless a builder was
already standing at it, so a builder walking across the map, or busy on
the previous frame, arrived to nothing. That read as nothing building
once mana ran out.

Minimap dots wear the colours the setup screen assigned.

Skirmish setup refuses a lineup with everyone on one team, the way the
multiplayer room does.

Multiplayer games are recorded. The result of every relay match reaches
the leaderboard at openkingdoms.net/leaderboard.html, with every player's
record and a drill down into each game.

## Changed in 0.1.1

The macOS archive now carries a real SDL2. The 0.1.0 one carried Homebrew's
sdl2-compat, which is a shim over SDL3 and went looking for an SDL3 that was
not there, so the game could not start at all. That archive was withdrawn.

The Windows archive carries the Visual C++ runtime. Without it the game would
not start on a machine that had never installed one.

Each archive is now checked at build time for anything it points at outside
itself or the operating system, which is the check that would have caught the
macOS fault before it shipped.

The video clips play. The doors on the main screen move when you point at
them and the cut scenes run, which earlier builds showed still because they
carried no decoder. The clips come from your own copy of the game.

Resting the cursor on the Credits door, the snort in the top left, no longer
takes the game down.

The doors also looked for their clips next to the machine the build was made
on rather than next to your copy of the game, so they never played in a
downloaded build even where a decoder was present.

## You bring the game

These archives hold the engine and the SDL runtime it needs. They hold no game content and never will. You need your own copy of Total Annihilation: Kingdoms, from GOG or from the discs.

The first run looks for it in the usual places. If it cannot find yours, it says so and how to point it at the folder. That is remembered afterwards.

## Downloads

| Platform | File |
|---|---|
| Windows 10 and 11, 64 bit | `openkingdoms-VERSION_HERE-windows-x64.zip` |
| macOS, Apple silicon | `openkingdoms-VERSION_HERE-macos-arm64.tar.gz` |
| Linux, x86_64 | `openkingdoms-VERSION_HERE-linux-x86_64.tar.gz` |

`SHA256SUMS` carries the checksums. `HOW-TO-RUN.txt` inside each archive covers the rest, and `THIRD-PARTY.txt` names what is built in and under which licence.

macOS asks about an unidentified developer, because this build is not signed by Apple. Clear the download flag once with `xattr -dr com.apple.quarantine OpenKingdoms`.

Linux binaries are built on Ubuntu 22.04 and need that glibc or newer.

There is no Intel Mac build. The hosted Intel runners have been retired, so that one is built from source for now, which takes a few minutes and is covered in the README.

## Multiplayer

Deterministic lockstep over one relay. Type the server address on Select Game and it is remembered.

A room holds players whose builds agree on arithmetic, so Windows plays Windows, macOS plays macOS, Linux plays Linux and the browser plays the browser. Each platform's maths library rounds sines its own way, which is enough to pull two machines apart over a match, so the handshake refuses a mix rather than desyncing it. A game you cannot join is still listed, greyed, with the reason. Lifting that is tracked in [docs/notes/2026-09-14-float-determinism.md](https://github.com/OpenKingdoms/OpenKingdoms/blob/main/docs/notes/2026-09-14-float-determinism.md).

Problems go to [issues](https://github.com/OpenKingdoms/OpenKingdoms/issues), with the version line from the main menu.
