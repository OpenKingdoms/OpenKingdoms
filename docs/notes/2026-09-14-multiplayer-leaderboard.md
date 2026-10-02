# The multiplayer leaderboard

openkingdoms.net/leaderboard.html shows every multiplayer battle played
through the relay: who has the most wins, their cumulative score, units
built and lost, and for each player the games they were in with the place
they took. This note records the decisions and what they do and do not
promise.

## What is recorded, and when

A battle ends when the verdict fires in the simulation, the same rule the
end screen uses (docs/notes/2026-09-10-end-of-battle.md). At that tick each
client in the match sends the relay one MATCH_RESULT message: the match id
the relay named in START_GAME, the tick the verdict fired on, and for every
seat in the battle the end screen's own columns off the player record,
units built, kills, losses, score and the last tick with units, plus
whether the seat still stood and whether its army was removed at once.

In lockstep every client holds the same numbers, so the relay takes the
first report from a seated player as the game's record and compares each
later one against it. A report that agrees raises the game's report count.
A report that differs marks the game disputed and changes nothing. A
watcher's report is neither taken nor held against anyone. A second report
from the same seat, a report naming a different match, one carrying a tally
set the relay does not know, and one whose verdict tick lies beyond the
turns the relay has actually delivered to the room are refused.

The relay joins the report to what it already knows about the room: the
map and its fingerprint, the rule options, the unit cap, each seat's name,
side, colour, team and kind, and the wall clock time the match started
and ended. Computer seats are recorded inside the game so the game view
shows the whole end screen, and never appear on the board.

The tally set is versioned in the message and in every record, so a
column added later reads back beside the old ones.

## Place and result

Everyone still standing when the verdict fires shares first place, which
is what allied winners are. Everyone else ranks by the tick they fell,
later is better, and ties share a place. A seat is standing when it had
units and had not resigned. Result is won for a standing seat and lost for
the rest, and a battle where nobody stood is a loss for everyone in it.

## Leavers

The relay, not the reports, decides what happened to a seat whose player
left. The turn clock remembers the turn on which each seat's player
resigned, was removed by the host, or dropped and ran out the reconnect
countdown, and forgets it again when a dropped player comes back and
reclaims the army. When a report arrives, any seat whose leaving turn is
at or before the verdict tick is recorded not standing, with its last
tick alive set to the first tick of the turn it left, whatever the report
said about it. Its other tallies are kept as reported. A player who left
after the verdict is recorded as the report has them. A battle every human
leaves before any verdict fires is not recorded at all, because nobody is
left to report it.

## Who a player is

A player is the device they play on. Every client says hello with a device
token, sixteen random bytes made once and kept with its settings, the same
token a rejoin is recognised by. When a match goes the relay notes the
token of whoever sat in each seat, and the record keys each human seat on
a player id made from that token: the first eight bytes of SHA-256 over a
fixed label and the token. The token is never shown. The id is public, and
nobody can work back from it to a token that makes it, so a player's record
cannot be played onto by typing their name.

The board shows each player by the name they last played under, and a
game shows each seat by the name typed for it then, with the player's
present name beside it when that differs. Two players who go by one name
are two rows, and the page adds the end of each one's id to tell them
apart.

A client from before tokens says hello with sixteen zeros. It plays as it
always has, with the same bytes on the wire, and its seat is recorded in
the game under its typed name, but the seat counts for nobody, because a
typed name is anybody's.

The rejoin work made the token, so no player loses it by updating. A
player who clears the browser's storage, or plays on a second machine, is
a second player. That is the cost of having no accounts.

### Records from before devices

Records written before this keyed a player by the name, trimmed and
compared without case. They are kept exactly as written, and a seat in the
file says which kind of id it holds. Those rows are frozen. They stay on
the board and on their own pages as they were, and no device ever takes
one over, because nothing a device can show proves it was the person who
typed that name. A player who played before devices keeps an old row
beside their new one. The live relay had recorded no games when this
changed, so no old row exists there.

One device can sit in two seats of one game, from two tabs of one browser,
which share a token. A player cannot both win and lose one game, so that
game counts for neither seat. It is still shown in full.

## One table per mod set

A modded game is a different game, so it is not ranked beside vanilla
(#287). Every record is filed under the host's mod set, the name its
greeting gave and the data fingerprint the room was made with, which
every seat matched to get in. Each pair is a table of its own. Its id is
the mod's name folded to lower case letters, digits and dashes, a dash,
and the fingerprint in sixteen hex digits, for example
`vanilla-3f9a1c0e7a11d2b4` or `tak-enhanced-e4a1000000000014`. The name is
compared without case, so two hosts who type one mod's name differently
still share its table when their data is the same, and two installs that
both call themselves Vanilla on different releases keep apart, since they
would not have been let into one room either.

A host from before protocol 4 names no mod set, and its games go on a
table called `unnamed-` and the fingerprint. Games recorded before tables
carry neither, so nothing says which data they ran on. They stay on a
table of their own, Earlier games, rather than being guessed onto
vanilla's.

The page asks `/api/tables` which tables there are, vanilla first and
then by how many games each holds, and opens on the vanilla table with the
most games. A picker above the table switches between them, and the
address keeps the choice, so a table can be linked. The table, both games
lists and a player's page take `table`, and without it they count every
game, exactly as before tables, so a page from before this reads a newer
relay as it always did.

## Disputed games

A disputed game stays in the ledger and is shown in full, flagged, on its
own page and in the games lists. It is left out of every sum: the table
and a player's row count only undisputed games, and the table says how
many games were left out so the omission is visible. The first report
stands as the record because there is no third witness, so a player who
reports first and lies can take one game off the board for everyone in
it, but cannot put a win on it.

## Where it is stored

The relay keeps one append only file, given by `--store PATH`. It starts
with a magic and a version, then records back to back, each a tag, a
length, a payload and a checksum over all three, written with the same
bounded writer the protocol uses. Tag 1 is a finished match, tag 2 a
confirmation or dispute of one, tag 3 a finished match with seats keyed by
device, which is tag 1 with one more byte a seat saying which kind of id
it holds. Tag 4 is never written: a build that never shipped put name
claims there, and a reader steps over it by length like any tag it does
not know. A match with no device seat is still written as tag 1, so a
file from before devices is rewritten byte for byte as it was. Tag 5 is a
finished match filed under a mod set: tag 3, then the mod's name and
version and the data fingerprint. A match with no mod set keeps tag 1 or
3, so a file from before tables is rewritten byte for byte as well.

A relay from before devices skips tag 3 by its length and counts those
as records it could not read. It would then number new matches from
its own count, over ids the skipped records hold. So a relay is not rolled
back past this change onto the same file. Keep a copy of the file first.
The same holds for tag 5 and a relay from before tables.

The file is read whole into memory at start and every question the site
asks is answered from memory.

The checksum is CRC32 over tag, length and payload. A checksum rather than
a sync mark, because it catches a wrong byte anywhere in a record, length
and payload alike, in one check, and it is what lets the reader recover: a
record that does not check is stepped over a byte at a time until one
does, so one bad byte costs one record and not the rest of the file. A
record whose length is beyond any record the relay writes is stepped over
the same way. Whatever was stepped over is dropped from the file by
writing every good record to a file beside the old one and renaming it
into place, so the original is never cut short. A record with a tag this
build does not know is skipped by its length. An empty file is a new
ledger. A file that is not a ledger is refused and left exactly as it is,
and the relay then keeps results in memory and says so loudly rather than
stop serving games.

This was chosen over SQLite because it needs no third party code, the
relay stays one static binary with no allocation in it, and the whole
store is tested with nothing but a scratch file. The memory cap is 8192
matches, about five megabytes, which is years of a playtest community.
SQLite is the upgrade path if the ledger ever outgrows that, and the
record format is the schema it would import.

The file has to live on persistent storage. On a host that discards the
machine's disk between runs that means a mounted volume and a `--store`
path on it. Without `--store` the relay keeps results in memory until it
restarts and says so at start.

## How the page gets the data

The relay answers plain HTTP on the same port the WebSocket uses. A
connection whose first bytes are a GET rather than an upgrade gets one
JSON response and a close. The routes are read only: the table, one
player with their games newest first, recent games, one game in full, the
maps games were played on, the tables there are, and a health line with a
version stamp. The
table can be searched by the name a player goes by now, and both games
lists by a player's name as typed then or now, by a piece of the map's
name, and by the span of dates the game ended in. A date on the page is
the reader's own day, sent as the unix milliseconds it starts and ends
at. Every answer carries
`Access-Control-Allow-Origin: *`, because the page and the relay are on
different hosts and the data is public. Every list is paged,
with caps sized so the largest page fits the relay's one response
buffer, and the page asks for more as the reader wants it rather than
for everything at once.

The page is web/leaderboard.html, plain HTML, CSS and JavaScript in the
game page's own colours and faces, copied to the site by the web
workflow. It finds the relay the way the game page does, from relay.txt
beside it, and turns the wss address into https. It draws from nothing
when the ledger is empty and says so when the relay does not answer.

The page's working out, routes and their filters, the query sent, names
that need telling apart and the live games, sits in web/leaderboard.js
with no page in it, so web/test_leaderboard.js runs it in node.

Above the table and the games list the page shows the games not yet
over, from `/api/rooms`: the open ones with a Join link, the same ?join=
link the game page takes, and the ones being played with how long they
have run.

The front page links the leaderboard from its fine print and from the
games panel. Over a running game the page's own link strip, beside Saved
games, opens it in a new tab so the battle keeps its tab. When a seated
player's game sends the verdict to the relay it asks the page to offer a link to the
player's own page, worked out from the same token, on the strip over the
end screen, because the end screen is the original's own dialog. A
desktop build has no page and offers nothing.

The page updates while open by asking `/api/health` every ten seconds and
redrawing the current view when the version stamp moved or the relay has
just come back. The games not yet over change without the stamp moving,
so their card is asked for on its own in the same poll. One request is in flight at a time, and a draw that
finishes after a newer one was started is thrown away. Polling was
chosen over a push because the relay answers a request and closes, holds
no HTTP connection open, and a game ends a few times an hour at most, so
one small request every ten seconds is the whole cost and no new
machinery is needed on the relay.

## Limits

- The relay is the only witness. A client that reports first and lies
  gets its numbers recorded until an honest client's report marks the
  game disputed, and a disputed game counts for nobody. The relay checks
  what it can on its own: the match, the tally set, and the verdict tick
  against the turns delivered.
- The relay's match id restarts with the process. The ledger's own id
  does not, and that is the one the site uses.
- Timestamps are the relay machine's clock.
- The board holds the first 8192 distinct players and 8192 matches. Past
  that the relay refuses to record and counts what it refused.
- A table is the mod set's name and its fingerprint. A mod that changes
  only art or sound leaves the fingerprint at vanilla's, so its players
  can join vanilla games, but a game it hosts is filed under its own
  name. Hosting with Vanilla chosen puts the game on vanilla's table.
- A player is a device. Clearing the browser's storage or moving to
  another machine starts a new player, and there is no way to merge two,
  or to join an old name's row to a device's.

## What an answer costs

The relay answers HTTP on the thread that relays turns, so a slow answer
is a pause in every game being played. Everything an answer needs beyond
the rows it writes, the table, each player's present name, the maps
played and the list of tables, is worked out in one pass the first time a question is asked
after the ledger changed, and reused until it changes again. A page of
games then costs its 25 games, and the map list its rows. One table's
rows are worked out the first time that table is asked for after a change
and kept until the ledger or the table asked for changes. The lists with
a filter walk the matches once, which is what main's table did on every
request. test_http_api times every route on a full ledger against that
table and counts how often the index is built.
