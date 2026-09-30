# Watching a match (2026-09-30)

Issue #294 asked for two things: watching a match live, and replays. The
replays are in docs/notes/2026-09-30-replays.md. This note covers
watching. None of it is the original's: its watchers joined before a
game started and sat in one of the eight places. Joining a match under
way works here because the relay keeps every turn of it.

## Getting in

Select Game lists every game on the server. A game under way shows Watch
on its row when its host allows watching, a watcher's place is free and
this client could build its world. A click on Watch watches it, and so
does Join on a running game, since a running game takes no players. The
web page's list of games and the leaderboard's show a Watch button for
the same games, which opens the page with `?watch=CODE`, and the engine
takes `--watch CODE` the way it takes `--join CODE`.

The relay adds the watcher to the room and to the match as a simulation
with no seat, and sends it the room state, START_GAME with no seat named,
GO for turn zero and then the turns. Select Game builds the world from
START_GAME and goes to the loading screen. It does not open the battle
room, which is for a game that has not started. The same holds for a
player rejoining their own match, where the room state can arrive after
START_GAME.

## Catching up

A match of twenty minutes is 24000 turns. A client holds 256 turns, and
a browser page holds 256 KB of messages between two frames, so the whole
log at once does not fit. From protocol 4 the relay sends a catch up a
window at a time: at most 128 turns, and about 96 KB of them, past the
last turn the client acknowledged. An empty run is cut to fit the
window. While the stream has not reached the latest turn, the live turns
wait in the log and reach the client from there, so it sees every turn
once and in order. A page still loading its world acknowledges nothing,
so it holds the first window and no more until it is built. A client of
protocol 3 or older is sent the whole log at once, as before.

Once built, the battle runs the turns it holds as fast as a frame's
budget allows and says "Catching up with the game..." until it is less
than a second behind, which is what a rejoin already did.

## What a watcher cannot do

A watcher never changes the players' game.

- It gives no order. The match refuses to send one, and the relay
  refuses a CMD from a simulation with no seat.
- Its world never counts when the worlds are compared. The players'
  hashes are judged among themselves. A watcher's hash waits for their
  agreement and is checked against it, and a watcher that differs is
  sent back to turn zero, alone. Before this a watcher counted towards
  the majority, so two watchers reporting one wrong hash could tie two
  players into a halt.
- Its arrival moves no turn. A player coming back restarts the turn
  timer, which is right after a stall. A watcher is waited for by
  nobody, so its arrival no longer touches the timer.
- A watcher falling behind does not slow the turns, which was already
  true: the governor reads seated players only.
- It cannot pause or change the speed. The relay refuses both.

`test_relay_loopback` plays one match twice from one seed on a steady
network, with and without watchers, one of them in a world that goes
wrong, and the players receive the same turn bytes and agree the same
hashes both times.

## Looking

A watcher holds no seat and has no fog of its own, so it sees the whole
map. Tab looks through one player's eyes, with their fog and their
sidebar, then the next player's, and after the last back to the whole
map. The line at the top of the play area says which. Which view is
shown is presentation only, like the seat a player plays: every machine
keeps every seat's fog. The camera is the player's own as always, and
the 3D view works.

A watcher's F1 menu does not save, load or restart. It wins and loses
nothing, so no Victory or Defeat is shown, and its report of the verdict
is not taken for the leaderboard. A watcher's machine records the battle
as a replay like anyone else's.

## Who is watching

From protocol 4 the room state names the watchers after the starts, so
the players see a line in the chat block when someone begins watching
and when they stop. A client of protocol 2 or 3 is written the room
state it always was.

## Leaving

Leaving the battle tells the relay the watcher left, and disconnects.
The players see it go at once. A watcher who left on purpose is not
handed the match back the next time its device says hello, where a
watcher whose tab closed is, and watches again from the start of the
log. When a match has used every simulation slot, a new watcher takes
the slot of one who left.

## The room list

A game under way is listed as closed to players. From protocol 4 a
client whose game data differs from the game's is told so instead, so
its list offers no Watch that could not work. An older client reads the
list as it always did.
