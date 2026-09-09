# Game files

OpenKingdoms does not include the game's content and never will. It is an
engine. The units, models, textures, animations, sounds, music, maps and
video that make up *Total Annihilation: Kingdoms* remain the property of
their rights holders. You supply them from a copy you own.

That is the reason projects like this survive. OpenRA, OpenMW, devilutionX
and Ship of Harkinian all work the same way, and all of them are still here.
The ones that bundled content are gone.

---

## What you need

A copy of Total Annihilation: Kingdoms. Any of these work:

- GOG sells it as part of the *Total Annihilation Commander Pack*.
- The original CD works. Copy the game folder off the disc to your drive.
- An existing install is fine too. If it's already on your machine, point at
  it.

The files OpenKingdoms actually reads are the `.hpi` archives in the game
folder, plus `.gp3`/`.ufo` if your install has them:

```
Total Annihilation Kingdoms/
├── TAK.hpi
├── Kingdoms.gp3
├── ...
└── Maps/
```

The expansion, *Iron Plague*, is supported if you have it. Its archives are
picked up from the same folder.

---

## Pointing OpenKingdoms at them

### In the browser

The hosted page at <https://openkingdoms.github.io/OpenKingdoms/> asks for
your game folder on the first visit. Chrome and Edge can pick the folder
itself. Firefox and Safari take the `.hpi` files, through the file button
or by drag-and-drop. The page reads the archives locally in the browser
and, with the "remember" box ticked, keeps a copy in the browser's private
storage (OPFS) so later visits boot without asking. Nothing is uploaded.
The "Forget my game files" link at the bottom of the page deletes that
copy.

### Native builds today: at build time

The engine reads the archives directly. You tell it where they are when you
configure the build.

```bash
cmake -B build -DTAK_GAME_DIR="/path/to/Total Annihilation Kingdoms"
```

Or set the `TAK_GAME_DIR` environment variable before running CMake. On
Windows a default GOG install (`C:\GOG Games\Total Annihilation Kingdoms`)
is picked up automatically if nothing is set.

Rebuilding to change the path is not how a released binary should work, so
the next milestone changes it.

### Planned: at run time

These land before the first tagged release, so that a downloaded build needs
no rebuild:

- On first run, OpenKingdoms asks for the folder and remembers it.
- On the command line, `--data "/path/to/Total Annihilation Kingdoms"`.
- In a config file, plain text, so you can edit it directly:

  | Platform | Location |
  |---|---|
  | Windows | `%APPDATA%\OpenKingdoms\config.ini` |
  | macOS | `~/Library/Application Support/OpenKingdoms/config.ini` |
  | Linux | `~/.config/openkingdoms/config.ini` |

- In the browser, the hosted build asks on first visit. Chromium browsers
  (Chrome, Edge) can pick the whole folder, and Firefox and Safari take the
  `.hpi` files by drag-and-drop. Files are read locally and cached in the
  browser's private storage, and they are never uploaded.

---

## Loose files instead of archives

If you already have the archives extracted, which modders often do, point
the loose-file directory at the extracted tree:

```bash
cmake -B build -DTAK_GAME_DIR="/path/to/game" -DTAK_DATA_DIR="/path/to/extracted"
```

`TAK_DATA_DIR` supplies files that are not in any archive. The archives are
searched first, so a loose copy of a file that also exists in an archive is
not used. This is how the development tree works against an extracted copy
of the game. A loose-first override mode, so a modified file can be dropped
in without repacking, is on the roadmap.
`scripts/hpi_extract.py` unpacks archives if you need to get started.

---

## Troubleshooting

**"VFS_Init: cannot open game directory"**
`TAK_GAME_DIR` needs to be the folder *containing* the `.hpi` files, not the
`.hpi` file itself and not the folder above it. Check the CMake configure
output. It prints the directory it will use.

**"Data appears incomplete: missing &lt;archive&gt;"**
Some installs are missing archives, particularly partial CD copies. Copy the
full game folder across.

**Multiplayer: "Data mismatch"** *(when multiplayer ships)*
Everyone in a lockstep game has to simulate from identical data.
OpenKingdoms will hash the parsed game data at join and refuse mismatches,
because the alternative is a desync ten minutes in. Usually this means one
player has a mod installed, or a different release of the game. See
[MULTIPLAYER.md](MULTIPLAYER.md).

---

## For contributors

Tests that need game data carry the CTest label `needs-data`. CI excludes
them with `ctest --label-exclude needs-data`, so you can build and run the
data-free suite on a clean checkout. GitHub's runners have no game data and
can't legally be given any. Maintainers run the full suite locally before
merging.

Never commit game data, extracted or otherwise. `.gitignore` covers the
obvious cases (`*.hpi`, `*.gp3`, `*.ufo`, `data/`), and CI scans every
push for asset extensions. A stray asset in a PR is still a takedown risk
for the whole project, so it's worth a glance at `git status` before you
push.
