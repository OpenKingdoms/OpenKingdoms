<p align="center">
  <img src="docs/img/banner.jpg" alt="OpenKingdoms" width="100%">
</p>

# OpenKingdoms

A from-scratch, open-source engine for **Total Annihilation: Kingdoms**
(Cavedog, 1999), written in C11. It runs natively on Windows, macOS and
Linux, and in the browser via WebAssembly.

OpenKingdoms is a reimplementation rather than a mod or a patch. The original
game's rules, meaning its economy, unit behaviour, combat maths, animation
system and map format, have been reconstructed and reimplemented so the game
runs on modern machines without DirectDraw, DirectPlay or a 1999 CPU.

> You need your own copy of the game. OpenKingdoms ships the engine only. It
> contains no units, models, textures, sounds, maps or music. Those are still
> owned by their rights holders and we don't distribute them. Point
> OpenKingdoms at a copy of the game you own and it does the rest. See
> [docs/ASSETS.md](docs/ASSETS.md).

<p align="center">
  <a href="https://openkingdoms.net/"><img alt="Play in your browser" src="https://img.shields.io/badge/play-openkingdoms.net-1f6feb?style=for-the-badge"></a>
  <a href="https://github.com/OpenKingdoms/OpenKingdoms/releases/latest"><img alt="Download the latest release" src="https://img.shields.io/github/v/release/OpenKingdoms/OpenKingdoms?style=for-the-badge&label=download&color=2da44e"></a>
</p>

<p align="center">
  <a href="https://github.com/OpenKingdoms/OpenKingdoms/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/OpenKingdoms/OpenKingdoms/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="Licence" src="https://img.shields.io/github/license/OpenKingdoms/OpenKingdoms"></a>
  <a href="https://github.com/OpenKingdoms/OpenKingdoms/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/OpenKingdoms/OpenKingdoms/total"></a>
</p>

---

## Get it

**[Play in your browser](https://openkingdoms.net/)** with nothing to install,
or download a desktop build:

| | |
|---|---|
| **Browser** | **[openkingdoms.net](https://openkingdoms.net/)**, Chrome, Edge, Firefox or Safari |
| **Windows** | [Latest release](https://github.com/OpenKingdoms/OpenKingdoms/releases/latest), `windows-x64.zip` |
| **macOS** | [Latest release](https://github.com/OpenKingdoms/OpenKingdoms/releases/latest), `macos-arm64` for Apple silicon. Intel Macs [build from source](#building-from-source) |
| **Linux** | [Latest release](https://github.com/OpenKingdoms/OpenKingdoms/releases/latest), `linux-x86_64.tar.gz` |
| **Source** | [Build it yourself](#building-from-source), Windows, macOS and Linux |

Every download needs your own copy of the game. The desktop builds find it
for you, and ask once if they cannot. See
[getting your game files in](#getting-your-game-files-in).

---

## Status

| Area | State |
|---|---|
| Skirmish vs AI | Playable. Economy, building, combat, magic, victory conditions |
| Rendering | 3DO models, GAF/TAF sprites, COB animation, team colours, fog of war |
| Pathfinding | Working. Ongoing accuracy work on tight corridors |
| Maps | TNT loading, heightmaps, features |
| Multiplayer | Playable. Deterministic lockstep over a relay, in the browser and on the desktop |
| Campaign / story mode | **Not playable yet** |
| Sound | Effects and music |

Expect rough edges. This is a preservation project under active development
and it isn't a finished product.

---

## Faithful, with improvements

OpenKingdoms aims at behavioural parity with the original game, and that is
the baseline rather than the ceiling. Where the original was limited by 1999
hardware, or by technology that no longer exists, we improve on it. Every
improvement is written down in
[docs/MANUAL_DEVIATIONS.md](docs/MANUAL_DEVIATIONS.md) so nobody mistakes it
for an accident.

These improvements are in already:

- Runs natively on Windows, macOS and Linux, and compiles to WebAssembly for
  the browser. No DirectDraw, DirectPlay, Glide or 8-bit palette modes, no CD
  check, no installer, no registry.
- Any resolution, windowed or fullscreen, through the `--width`, `--height`,
  `--fullscreen` and `--windowed` flags, rendered on the GPU. The original ran
  fixed 8-bit modes.
- The simulation runs at 60 Hz instead of 30. Unit rates from the data files
  are converted so speeds, reload times and build times come out the same, and
  movement and animation are twice as smooth.
- Up to 2000 units per player. The original's setup screen topped out at 500.
- Pathfinding and fog of war are built to hold a frame rate with large armies,
  using a spatial grid for target scans, cached passability per movement class
  and staggered fog updates.
- Developer tooling the original never had: asset validators, a COB script
  inspector and disassembler, a map inspector, render probes, and a test suite
  that runs the animation VM against every script in the game.

These are still to come:

- Replays recorded from the lockstep command stream.
- A data fingerprint at join, so mismatched game files are caught before they
  cause a desync.
- Play in the browser with your own game files and nothing to install.
- Smooth play with 1000 units on screen.
- A `--data` flag, a saved config file and a first-run folder prompt, so a
  downloaded build never needs a rebuild.
- Units that never get stuck, with movement that respects unit footprints and
  the original's finer path grid.
- Drop-in modding, with loose files on disk taking priority over the archives,
  so a modified file needs no repacking.

---

## Play in your browser

The fastest way in, with nothing to install:

1. Open **<https://openkingdoms.net/>**
2. Point it at your Total Annihilation: Kingdoms folder (that brings the
   music along too), or drop the `.hpi` files from it onto the page.
3. Press Start and play.

Your game files never leave your machine. The page reads them locally and,
if you leave the box ticked, keeps a copy in the browser's own storage so
the next visit boots straight in. A "Forget my game files" link at the
bottom of the page clears that copy.

Chrome and Edge can pick the whole folder. Firefox and Safari take the
`.hpi` files themselves, either through the file button or by
drag-and-drop. The archives are around 300 MB, so the first load takes a
moment and the tab needs a machine with a few GB of memory.

The page is rebuilt from `main` on every push, so it always matches the
latest code, rough edges included.

---

## Building from source

Every release is built by CI from a tag, so you only need this to develop
or to build for a platform we do not ship. To play, take a
[release](https://github.com/OpenKingdoms/OpenKingdoms/releases/latest) or
the [browser](https://openkingdoms.net/).

### Windows

Requires [Visual Studio 2022](https://visualstudio.microsoft.com/) (Desktop
C++ workload), [CMake](https://cmake.org/download/) 3.20+, and
[vcpkg](https://vcpkg.io/).

The first configure also builds FFmpeg through vcpkg. That is what plays
the short clips behind the main menu doors and the loading screen. Pass
`-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON` to skip it and get still
sprites in those two places instead.

```powershell
git clone https://github.com/OpenKingdoms/OpenKingdoms.git
cd OpenKingdoms
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
      -DTAK_GAME_DIR="C:\GOG Games\Total Annihilation Kingdoms"
cmake --build build --config Release
.\build\src\Release\tak-re.exe
```

---

### macOS

Builds on both Apple silicon and Intel. Released builds are unsigned, so a
download needs `xattr -dr com.apple.quarantine OpenKingdoms` once.

```bash
brew install cmake sdl2 ninja
git clone https://github.com/OpenKingdoms/OpenKingdoms.git
cd OpenKingdoms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release       -DTAK_GAME_DIR="/path/to/Total Annihilation Kingdoms"
cmake --build build
./build/src/tak-re
```

---

### Linux

Install dependencies:

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build libsdl2-dev

# Fedora
sudo dnf install gcc cmake ninja-build SDL2-devel

# Arch
sudo pacman -S base-devel cmake ninja sdl2
```

Then:

```bash
git clone https://github.com/OpenKingdoms/OpenKingdoms.git
cd OpenKingdoms
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release       -DTAK_GAME_DIR="/path/to/Total Annihilation Kingdoms"
cmake --build build
./build/src/tak-re
```

You don't need Wine or Proton. OpenKingdoms is a native port rather than a
compatibility layer.

---

## Getting your game files in

OpenKingdoms reads the original game's `.hpi` archives directly and never
copies or ships them.

A downloaded build looks for your copy in the usual install locations and
remembers what it finds, so most people start it and play. If it cannot find
one it prints what to do, and you can name the folder in any of these ways:

```
OpenKingdoms --game-dir "C:\GOG Games\Total Annihilation Kingdoms"
export TAK_GAME_DIR="$HOME/Games/Total Annihilation Kingdoms"
```

or put a copy in a folder called `game` beside the binary. In the browser you
point the page at the folder instead, and the files never leave your machine.

A build you compile yourself can also bake a default in with
`-DTAK_GAME_DIR`, which is what the development setup does.

Where to get a copy if you don't have one:

- GOG sells it as part of the *Total Annihilation Commander Pack*.
- Original CDs work fine. Copy the game folder off the disc.

Full details, including loose extracted files for modding, are in
[docs/ASSETS.md](docs/ASSETS.md).

---

## Multiplayer

Lockstep netcode over a lightweight relay. The server passes command frames
between players and holds no game state and no game data, so hosting one for
your friends is cheap and legally uncomplicated.

Choose Multiplayer, type the address of a server and press Enter. The
address is remembered. Anyone on that server can host a game or join one,
and a browser player and a desktop player use the same server.

A room holds any of them. Windows, macOS, Linux and browser players play
together, because the simulation carries its own trigonometry rather than
borrowing each platform's maths library, which used to round a sine its own
way and pull two machines apart over a match. Two gates in CI keep it that
way, and a build old enough to predate them is still refused rather than
desynced, listed greyed with the reason. The measurement and what was done
about it are in
[docs/notes/2026-09-14-float-determinism.md](docs/notes/2026-09-14-float-determinism.md).

See [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md) for the design.

---

## Contributing

Contributions are very welcome, especially from people who know the original
game well. Accuracy reports are as valuable as code. If something behaves
differently from the 1999 game, that's a bug worth filing.

Start with [CONTRIBUTING.md](CONTRIBUTING.md). The main points:

- OpenKingdoms aims at behavioural parity with the original. Changes to game
  behaviour need evidence, such as a note in `docs/notes/`, a manual
  reference, or a reproducible in-game observation. Improvements on the
  original are welcome as long as they get recorded as deviations in
  [docs/MANUAL_DEVIATIONS.md](docs/MANUAL_DEVIATIONS.md).
- Build both the native and the WebAssembly target before opening a PR.
  Emscripten's clang rejects things MSVC accepts.
- Tests live beside the code as `test_*.c`. CI runs the data-free subset and
  maintainers run the full suite.

---

## Contributors

The people whose pull requests have been merged into OpenKingdoms,
in the [all-contributors](https://allcontributors.org) way.

<table>
  <tr>
    <td align="center" valign="top" width="20%">
      <a href="https://github.com/zbennett10"><img src="https://github.com/zbennett10.png?size=100" width="100" alt="Zachary Bennett" style="border-radius: 50%"/><br /><sub><b>Zachary Bennett</b></sub></a><br />
      <sub>engine, project lead</sub>
    </td>
    <td align="center" valign="top" width="20%">
      <a href="https://github.com/DoubyCz"><img src="https://github.com/DoubyCz.png?size=100" width="100" alt="Jiří Doubravský" style="border-radius: 50%"/><br /><sub><b>Jiří Doubravský</b></sub></a><br />
      <sub>menu and Options fixes, Linux testing</sub>
    </td>
  </tr>
</table>

A merged pull request puts you here. See [Contributing](#contributing).

## How this was built

OpenKingdoms is a reimplementation, written from scratch in C. The original
game's behaviour, covering economy, combat, animation, AI and map handling,
is described in our own words in [docs/notes/](docs/notes/), and those notes
are the reference contributors work from. No original game code or content is
included in this repository, and none ever will be.

[docs/PARITY.md](docs/PARITY.md) explains what parity means here and how
claims are evidenced.
[docs/MANUAL_DEVIATIONS.md](docs/MANUAL_DEVIATIONS.md) records the departures
from the original that were made on purpose, and why.
[docs/DATA_FORMATS.md](docs/DATA_FORMATS.md) is the file format reference
(HPI, 3DO, GAF/TAF, COB, TNT, TDF/FBI).

---

## Licence and attribution

OpenKingdoms is licensed under the **GNU General Public License v3.0**. See
[LICENSE](LICENSE).

Third-party components keep their own licences: FFmpeg (LGPL 2.1 or
later, built with only the Bink decoder so the menu doors and cut scenes
play), SDL2 (zlib), miniaudio
(public domain / MIT-0), stb_image (MIT / public domain), miniz (MIT).

*Total Annihilation: Kingdoms* is a trademark of its respective owners.
This project is not affiliated with, endorsed by, or supported by Cavedog
Entertainment, Atari, or any current rights holder. No game assets are
distributed here.
