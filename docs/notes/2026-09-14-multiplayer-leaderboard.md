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

A player is the name they typed on the Select Game screen, trimmed and
compared without case. Zach, zach and " ZACH " are one player. The relay
refuses a name that is nothing but blanks. The leaderboard keys every
record on a hash of the trimmed name and shows the spelling most recently
used.

The trade off is plain. Two people who type the same name share one
record, and anyone can type another player's name and play as them. That
is accepted for a playtest community. A check on the name, a password or
a token, can be added later without changing the stored records, because
the records hold the name and its hash and nothing else about identity.

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
confirmation or dispute of one. The file is read whole into memory at
start and every question the site asks is answered from memory.

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
player with their games newest first, recent games, one game in full, and
a health line with a version stamp. Every answer carries
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

The page updates while open by asking `/api/health` every ten seconds and
redrawing the current view when the version stamp moved or the relay has
just come back. One request is in flight at a time, and a draw that
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
- The board holds the first 8192 distinct names and 8192 matches. Past
  that the relay refuses to record and counts what it refused.
