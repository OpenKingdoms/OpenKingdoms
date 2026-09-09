# Game files

**OpenTAK does not include the game's content and never will.** It is an
engine. The units, models, textures, animations, sounds, music, maps and
video that make up *Total Annihilation: Kingdoms* remain the property of
their rights holders. You supply them from a copy you own.

This isn't a technicality we're grudgingly observing — it's the reason
projects like this survive. OpenRA, OpenMW, devilutionX and Ship of
Harkinian all work the same way, and all of them are still here. The ones
that bundled content are not.

---

## What you need

A copy of Total Annihilation: Kingdoms. Any of these work:

- **GOG** — sold as part of the *Total Annihilation Commander Pack*.
- **The original CD** — copy the game folder off the disc to your drive.
- **An existing install** — if it's already on your machine, just point at it.

The files OpenTAK actually reads are the `.hpi` archives in the game
folder, plus `.gp3`/`.ufo` if your install has them:

```
Total Annihilation Kingdoms/
├── TAK.hpi
├── Kingdoms.gp3
├── ...
└── Maps/
```

The expansion, *Iron Plague*, is supported if you have it — its archives
are picked up from the same folder.

---

## Pointing OpenTAK at them

### First run

OpenTAK asks. Select your game folder and it remembers the choice.

### Command line

```bash
OpenTAK --data "/path/to/Total Annihilation Kingdoms"
```

### Config file

The path is stored in a plain-text config so you can edit it directly:

| Platform | Location |
|---|---|
| Windows | `%APPDATA%\OpenTAK\config.ini` |
| macOS | `~/Library/Application Support/OpenTAK/config.ini` |
| Linux | `~/.config/opentak/config.ini` |

```ini
[data]
path = /path/to/Total Annihilation Kingdoms
```

### In the browser

The browser build asks on first visit. Chromium-based browsers (Chrome,
Edge) can pick the whole folder; Firefox and Safari take the `.hpi` files
by drag-and-drop.

**Your files are never uploaded.** They're read locally by the page and
cached in your browser's private storage so you don't have to select them
again. Settings → *Forget my game data* clears that cache.

---

## Loose files instead of archives

If you already have the archives extracted — modders often do — point
OpenTAK at the extracted tree instead:

```bash
OpenTAK --data /path/to/extracted --loose
```

Loose files take priority over archive contents when both are present,
which is also how you test a modded file without repacking anything.

---

## Troubleshooting

**"No game data found at &lt;path&gt;"**
The path needs to be the folder *containing* the `.hpi` files, not the
`.hpi` file itself and not the folder above it.

**"Data appears incomplete: missing &lt;archive&gt;"**
Some installs — particularly partial CD copies — are missing archives.
Copy the full game folder across.

**Multiplayer: "Data mismatch"**
Everyone in a lockstep game has to simulate from identical data. OpenTAK
hashes the parsed game data at join and refuses mismatches, because the
alternative is a desync ten minutes in. Usually this means one player has a
mod installed, or a different release of the game. Run
`OpenTAK --data-report` on both machines and compare to find the file that
differs.

---

## For contributors

Tests that need game data are tagged and skipped when it isn't present, so
you can build and run the data-free suite on a clean checkout. CI runs
exactly that subset — GitHub's runners have no game data and can't legally
be given any. Maintainers run the full suite locally before merging.

Never commit game data, extracted or otherwise. `.gitignore` covers the
obvious cases (`*.hpi`, `*.gp3`, `*.ufo`, `data/`), but a stray asset in a
PR is a takedown risk for the whole project, so it's worth a glance at
`git status` before you push.
