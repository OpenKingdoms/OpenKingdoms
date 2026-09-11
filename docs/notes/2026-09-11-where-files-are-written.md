# Where the game writes the player's own files

One resolver, `include/tak_paths.h`, answers for everything the game
persists on the player's behalf. Today that is options.cfg. Saved games
go in the same place, under a `saves` subdirectory it creates on first
use.

## The directories

| Platform | Directory |
|---|---|
| Windows | `%APPDATA%\OpenKingdoms\OpenKingdoms\` |
| macOS | `~/Library/Application Support/OpenKingdoms/OpenKingdoms/` |
| Linux | `$XDG_DATA_HOME/OpenKingdoms/OpenKingdoms/`, else `~/.local/share/OpenKingdoms/OpenKingdoms/` |
| Browser | `/libsdl/OpenKingdoms/OpenKingdoms/` in the in-memory filesystem, mirrored to origin private storage under `prefs/` |

The folder name is doubled because SDL builds the path from an
organisation and an application and the project passes OpenKingdoms as
both. It stays that way on purpose. Fixing it would orphan every
options.cfg a player already has, for a cosmetic gain.

## Separators are never assumed

`SDL_GetPrefPath` hands back a backslash path on Windows and a forward
slash path everywhere else. Anything the resolver appends reuses
whichever separator the directory already carries, so a composed path
never mixes the two. Before this landed, an override of
`C:\scratch\ok` produced `C:\scratch\ok/options.cfg`.

A save slug is refused if it carries a separator, a drive colon or a
parent reference. The name the player typed is stored inside the file,
so the slug on disk never has to be anything but a plain file name.

## The browser, where settings never persisted

SDL's Emscripten backend builds the preference directory in the
in-memory filesystem, which is rebuilt from nothing on every page load,
and the build mounts nothing durable. So until now a browser player who
changed an option lost it on reload.

The engine cannot talk to origin private storage itself. That API's
synchronous handles are the worker path, and the build sets
`ASYNCIFY=0`, so C cannot block on a promise on the main thread. The
layering is the one web/shell.html already uses for the archives.

- C reads and writes with ordinary `fopen`, identical code to the
  desktop build.
- The page copies origin private storage into memory before `main()`
  runs, and copies it back out after each write. The engine asks for
  the write back through an `EM_JS` hook, which is C calling out to the
  page and needs no runtime export.

Two details in web/shell.html matter more than they look.

The restore sits **above** the early return that fires when the
packager preloaded the data. The player's settings have nothing to do
with whether the archives were bundled, and the bundled build is the
one a developer runs first, so a restore placed below that return would
appear to do nothing in exactly the build under test.

Settings live at the storage root under `prefs/`, a sibling of the
`game/` directory the archives cache into, never a child of it. The
Forget my game files link removes `game/` recursively, so a sibling
cannot be reached by it. That is a guarantee by construction rather
than by anyone remembering not to break it later.

A private window has no origin private storage. `prefsDir` returns
null, the write hook does nothing, and the session still works with
settings held in memory. That degrade path is a manual check rather
than an automated one, because automation would only confirm the happy
case.
