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

## Saved games in the browser

`Paths_SaveDir` is a subdirectory, `saves/` under the preference
directory. Both halves of the mirror used to be flat: the restore
copied only entries whose kind was `file`, and the write back skipped
anything `FS.isFile` said no to. A saved game written through
`Paths_SaveFile` landed in memory, never reached storage, and died with
the tab without an error anywhere. Both halves now walk one level of
subdirectory, so the desktop layout and the browser layout stay the
same shape.

The restore no longer runs at boot. Settings are a few hundred bytes
and the game reads them on the way up, so the boot still waits for
those. Saved games are neither, and a player with twenty of them was
paying for all twenty before the main menu drew, whether or not they
were going to load one. They come in when a save or load dialog opens.

`Paths_BeginSaveSync` and `Paths_SavesPending` are how the dialog asks
and waits. Both are no-ops on a desktop, where the directory is already
the directory. In a browser the first calls `Module.restoreSaves` and
the second reads the flag it sets, and the dialog draws an empty list
with its help line saying so until the flag clears. The order matters:
the load dialog decides whether the directory is empty, and deciding
that before the files arrive is how a player with saves is told they
have none, in a message that closes the dialog on the first press.

There is no way around the wait. Origin private storage can only be
read a promise at a time on the main thread, and reading a file from it
inside a synchronous `fopen` would need Asyncify, which this build
turns off on purpose. So the dialog waits rather than the boot.

Three things follow from saves being large where options.cfg is not.

The write back only copies a file whose size or modification time has
moved. It used to rewrite every file in the preference directory on
every notify, which costs nothing for one small settings file and would
cost a directory of saves on every save press.

A deleted save is removed from storage as well as from memory. Without
that it would come back on the next reload, which is worse than a
delete that failed outright.

The page asks for persistence with `navigator.storage.persist()`. Until
an origin is granted it, its storage is best effort and can be evicted
under disk pressure. Losing a settings file is cheap and losing a
campaign is not, so a refusal is shown to the player rather than logged
and forgotten.

Shown where they can see it. The picker's status line is behind the
canvas once the game is up, so anything said there during a battle is
said to nobody. A strip over the canvas carries these now, and a
warning stays on it until it is dismissed.

## Getting a saved game out, and back in

A save in browser storage is not a file the player owns. Clearing site
data takes it, and it cannot be carried to another machine or another
browser. The Saved games panel, beside the forget link, writes one out
as an `.oksave` file and reads one back.

An import never overwrites. The file already in storage is somebody's
game and two saves are cheaper than the wrong one gone, so a name that
is taken gets a `(2)`. An imported file is written to storage and to
the in-memory filesystem both, so the load dialog finds it without a
reload.

`scripts/saves-browser-smoke.js` is the run that proves all of this. It
needs a browser, a served build and the player's own game files, so it
is not in CI.

## What the storage limit means for a player with many saves

`navigator.storage.estimate()` is the only figure worth quoting, and
the page now reads it. The published per browser quotas are folklore
until that call answers on the machine in front of the player, so the
page prints what it gets and warns below 32 MB of headroom rather than
asserting a number.

The order of magnitude on our side is what decides whether this is ever
a problem. The bulk of a save is the fog layers, one per player at one
byte per 32 pixel cell, which deflate flattens hard because they are
mostly uniform, plus the unit records. A save on the order of a few
hundred kilobytes to low single digit megabytes is the target. No
browser's quota threatens a few dozen of those. Eviction is the real
risk, not size, which is why the persistence grant matters more than
the cap.
