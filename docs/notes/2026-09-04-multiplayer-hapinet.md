# Multiplayer and the net layer

How the original plays a network game, from its DirectPlay wrapper up
to the messages, the pacing and the start handshake. The second half
is the reference for OpenKingdoms' own multiplayer systems that
[MULTIPLAYER.md](../MULTIPLAYER.md) points at: which builds may share a
room, the room, watching a match, the leaderboard and replays.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate departures
are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md), cited by ID.

## The original's model

The original does not play in lockstep. It keeps Total Annihilation's
peer authoritative model: each machine runs its own orders at once,
every peer keeps telling the others the state of its own units, and a
lagging peer slows the game down rather than holding it at a turn
barrier.

- Orders run locally the moment they are given, in the same path that
  broadcasts them, with no acknowledgement or turn barrier between
  (legacy:234433-234472).
- Absolute unit positions are sent, not worked out again from shared
  input. Message 0x29 is sent at legacy:234410-234427 and applied at
  legacy:195077-195086.
- Damage is sent. Message 0x2B feeds the damage path directly
  (legacy:195096-195098).
- Message 0x2C, a bit packed gossip of unit state, carries about a
  sixth of the sender's own units every tick, stamped with the sender's
  32 bit frame number (legacy:236664-236723).
- Nothing waits for every peer. The pacing takes the lag as the local
  frame minus the oldest frame any peer has reported and scales the
  tick rate down (legacy:242330-242400).
- The peers never share a random seed. Seeds are matched only in the
  `-AutoStop` benchmark mode (legacy:254475-254495, 241551-241581,
  legacy:242436-242440).

About 15 of the original's 45 message types exist only to copy state
between peers, among them 0x0B, 0x24, 0x27, 0x29, 0x2A, 0x2B and 0x2C.
The original has no desync detection. Each unit's state is asserted
again by its owner about every six ticks, so a divergence is corrected
as it happens rather than prevented. The engine departs here and plays
in lockstep over a relay, see N-001.

## Layers

From the bottom up:

1. DirectPlay itself, dplayx.dll.
2. HAPINET, 34 thin wrappers over IDirectPlay4 and IDirectPlayLobby3
   (legacy:268445-269750).
3. The netCondenser, an optional layer that aggregates messages
   (legacy:199360-201310). It takes a NetBuffer of up to 1066 bytes
   into the PlayerFrameInfo of one of 11 peers, assigns it a frame,
   coalesces, compresses with LZW, XORs and checksums it.
4. Message routing (legacy:192820-196150), with a send per slot, a
   receive pump and the message dispatcher.
5. The game logic, which builds a message as a type byte and a payload
   and hands it to the broadcast entry.

A global sync enabled flag (legacy:4988, 5222) picks one of two send
paths. On the raw path each message is one HAPINET packet, sent through
the broadcast wrapper at legacy:193665. On the condensed path messages
go into a ring buffer per peer, are coalesced to at most 1066 bytes,
compressed with LZW and flushed once per tick from the simulation loop
(legacy:242487-242490). That path is entered at legacy:199643 and
flushed at legacy:199591.

Reliability is switched by bracketing, not per message. A guarantee
flag goes on, the sends go out, and it goes off again. Four wrapper
variants do this (legacy:193677, 193702, 193782, 193807).

The player table holds 10 slots of a fixed stride in the game state.
Each slot has a kind, 1 for a local human, 2 for a local AI and 3 for a
remote player. The local slot index and a network active bit are kept
in the game state apart from the table.

## The HAPINET wrappers

| Anchor | Routine |
|---|---|
| legacy:268570 | HAPINET_sendpacket, IDirectPlay4::Send |
| legacy:268624 | HAPINET_updategameinfo, SetSessionDesc |
| legacy:268657 | HAPINET_createnewgame, Open with DPOPEN_CREATE |
| legacy:269098 | HAPINET_joingame, Open with DPOPEN_JOIN |
| legacy:269168 | HAPINET_enumconnections, a callback |
| legacy:269299 | HAPINET_enumproviders, a callback |
| legacy:269574 | HAPINET_enumplayers |
| legacy:269593 | HAPINET_createcompoundaddress |
| legacy:269612 | HAPINET_enumaddress |

A session allows 16 players by default, and enumerating sessions times
out after 1500 ms (legacy:268848).

The HAPINET context is one global structure holding, in order, the
session name (180 bytes), the player's long and short names (16 bytes
each), the application GUID, which is the game's session GUID
(legacy:5148), a DPSESSIONDESC2 (0x50 bytes), the last from and to ids,
the COM interface pointers, a DPLCONNECTION and two default values.

## Game side routines

The game side of the network spans legacy:192090-196150.

| Anchor | What it does |
|---|---|
| legacy:192098-192208 | netCondenser receive: checks the checksum, undoes the XOR, decompresses |
| legacy:192212-192295 | netCondenser send: LZW, XOR, checksum, the simulated loss, then the send |
| legacy:192869-192933 | Broadcasts message 0x1E, a 156 byte PlayerInfo payload, 157 bytes on the wire with its type byte |
| legacy:193183-193332 | The lobby launch worker thread, with a 40 s timeout |
| legacy:193358-193608 | The host and join entry: version check, password, session GUID |
| legacy:193727-193778 | Fans a message out to every remote slot |
| legacy:193978 | Allocates an 8 KB receive buffer, held in the game state |
| legacy:194112-194135 | Returns the reject reason texts, codes 0 to 10 |
| legacy:194477-194510 | The drop timeout, the timeout in seconds times 30 ticks since the last packet from that slot |
| legacy:194516-195115 | The message dispatcher |
| legacy:195322-195352 | The heartbeat, message 2, every 60 ticks |
| legacy:195356-195378 | The round trip reply, its result stored on the sending slot |
| legacy:195459-195704 | The game start handshake and the start position shuffle |

The dispatcher indexes its handler table by the type minus 2, so the
valid message types start at 2 (legacy:194742).

## The netCondenser

| Anchor | Behaviour |
|---|---|
| legacy:199413 | Adding a message grows the buffer by its length or 0x200, whichever is larger |
| legacy:199504 | Finding a peer's PlayerFrameInfo, 11 slots of stride 0x1044 |
| legacy:199591 | The flush once per tick |
| legacy:199659 | Setting the send rate. The pacing interval is 1000 ms over the rate held between 2 and 30. A negative rate turns condensing off, and a rate of 0 means 200 ms. |
| legacy:200218 | Sending a frame is the pacing gate. Fallen behind, it sets its next send to now plus half the interval rather than catching up. |
| legacy:200320 | Queueing a message flushes on its own at 0x429 bytes or 0x400 packets |
| legacy:200401 | A NetBuffer packet carries at most 0x42A (1066) bytes of payload |
| legacy:200665, 200694 | The condensed receive: reordering, gap detection and a stash for packets out of order |
| legacy:201122-201308 | The out queue flush and message splitter. It spreads a batch across at most 30 frames to smooth jitter, and drops 0x2C messages past 512 in one batch. |

## Wire format

A condensed packet:

```
[u8  kind]        3 = raw, 4 = LZW
[u16 checksum]    sum of all bytes from index 3 to the end
[body]            byte i is XORed with i
```

The codec spans legacy:192098-192295, with the send at legacy:192212
and the receive at legacy:192098. Compression is skipped when the raw
body is under 13 bytes, when compressing gains nothing, or when a
global flag turns it off. A packet with a bad checksum is dropped
without a word. A simulated packet loss is built in
(legacy:192278-192288).

Decompressed, the payload is a frame number and then the messages:

```
[u32 frame_number]   0xFFFFFFFF = unordered, 0xFFFFFFFE = initial
[NetMessage ...]
```

A message comes in one of two shapes. Types 0x02 to 0x2B have a fixed
length, `[u8 type][payload]`, taken from a static size table
(legacy:5253). Each entry of that table is 4 bytes, a u16 size and a u8
flags byte, where bit 0 allows the message in modes 1 and 2 and bit 1
allows it in mode 3. Types 0x2C to 0x2E carry their length,
`[u8 type][u16 total length including the 3 byte header][payload]`.
The valid types run from 2 to 0x2E.

### Message types

The dispatcher is at legacy:194516. A length counts the type byte.

| Type | Length | Meaning |
|---|---|---|
| 0x02 | 9 | Heartbeat and ping, every 60 ticks, and the round trip reply |
| 0x05 | 299 | Chat, `[5][text]` |
| 0x07 | 7 | Unit order, `[7][u16 unitId][u16 targetId][u8 order][u8 flags]`, sent at legacy:234450 |
| 0x08 | 9 | Minimap ping |
| 0x09 | 11 | Projectile spawn (legacy:227154) |
| 0x0C | 6 | Map grid cell owner or building placement |
| 0x0E | 22 | COB script call, a function id and 4 arguments (legacy:195232) |
| 0x0F | 6 | AimWeapon, heading and pitch, each times 256 |
| 0x10 | 4 | FireWeapon |
| 0x12 | 5 | Unit copy position or transport |
| 0x14 | 1 | Game start, GO (legacy:195666) |
| 0x18 | 3 | Pause or speed, `[0x18][0][paused]` or `[0x18][1][level]` (legacy:131795) |
| 0x19 | 6 | Player leave or defeat, `[u32 playerId][u8 reason]` |
| 0x1A | 5 | Player disconnected |
| 0x1C | 51 | Start positions, 10 entries of `{u32 playerId, u8 startPos}` (legacy:195624) |
| 0x1D | 5 | Start position acknowledgement |
| 0x1E | 157 | Full PlayerInfo |
| 0x21 | 10 | Alliance and diplomacy, `[u32 from][u32 to][u8 state]` |
| 0x23 | not known | Anti-cheat warning that a player's game program was modified |
| 0x26 | 3 | Ready, sets the ready bit on the sending slot |
| 0x27 | 5 | Frame or tick counter sync |
| 0x28 | 6 | Resource transfer, a float |
| 0x29 | 15 | Absolute unit position, `[u16 id][i32 x][i32 y][i32 z]` |
| 0x2B | not known | Damage, fed to the damage path |
| 0x2C | varies | Bit packed unit state gossip, dropped under congestion |
| 0x2D | varies | Map or file transfer chunk, Boneyards only |
| 0x2E | varies | Reject packet |

The reject reasons (legacy:194112, their texts at legacy:5136-5155) are
5 game full, 6 lost connection, 7 missing unit, 8 need newer version,
9 no watching allowed and 10 creator left.

### The unit state gossip

Message 0x2C (legacy:236664) is a bitstream, not a byte structure:

| Bits | Content |
|---|---|
| 8 | Message type |
| 16 | Length, a placeholder patched in afterwards |
| 32 | The sender's own simulation frame number |
| varies | For about (unit count + 5) / 6 units taken round robin, each a 16 bit relative id, an absolute id whose width is a global set from the number of unit types loaded, then the unit's own serialised state |
| 16 | 0xFFFF terminator |

The length is patched back into bytes 1 and 2 once the stream closes.
The stream is read back around legacy:201640-201760.

## Pacing, drops and speed

The tick rate follows the slowest peer (legacy:242330). The lag is the
local simulation frame minus the lowest frame number any remote slot
has reported. Below 120 frames of lag the game runs at full speed. Past
that the tick rate is scaled by 1 minus the lag over 3480, with the lag
capped at 3600 and the factor kept at 0.01 or more. A lag above 449
sets the severe bit. The status bits, from bit 0, are paused, net
slowdown, severe, and running below the requested speed. The ticks
issued are the elapsed milliseconds times the speed level times 0.1
times the slowdown, plus the fraction carried over, rounded.
The slowdown and the flush once per tick span legacy:242330-242512.

The heartbeat goes every 60 ticks. After the timeout in seconds times
30 ticks with nothing from a slot, a "Waiting for player" dialog
appears, the reject and drop GUI at legacy:194444. When the slot is
dropped, message 0x19 is broadcast and the slot is freed
(legacy:194242).

The speed level runs from 0 to 20, where 10 is normal, and in
multiplayer it is at least 1 (legacy:131754). Only the host may change
it (legacy:131751), and the change is broadcast reliably. The tick rate
is the level times 0.1 (legacy:242355). The engine makes pause and
speed the server's pacing instead, see N-004.

## Random numbers and the start

The simulation's generator is a Lehmer (MINSTD) generator with
multiplier 16807, seeded by XOR with 0x66E29572 and then OR 1
(legacy:254475, 254490). Its full constants and how exactly they must
be matched are in [gameflow.md](gameflow.md), under "Random numbers".
No seed is ever sent, so the peers' generators differ. A lobby field
that could be taken for a seed holds the maximum player count.

The host shuffles the start positions with the C runtime's rand()
(legacy:195542-195576) and broadcasts the result in message 0x1C, not a
seed, which is the only reason the start positions agree. A seat may
claim its start in the engine, see D-028.

The start handshake (legacy:195459):

1. Wait until every remote slot has set its ready bit.
2. The host shuffles and assigns the start positions.
3. The host sends message 0x1C to each peer.
4. Wait for both the loaded flag and the position acknowledgement from
   every peer, each kept in its own game state field.
5. The host broadcasts message 0x14, GO.

## Sessions and joining

Session enumeration (legacy:132990) reads up to 100 records of 0x188
bytes each and keeps those whose build id and version (0x15) match. A
record:

| Offset | Size | Field |
|---|---|---|
| 0x000 | 180 | Session name |
| 0x0B4 | 16 | Host name |
| 0x0C4 | 16 | Map name |
| 0x0D4 | 128 | Description |
| 0x158 | 4 | Build id |
| 0x15C | 12 | Session GUID |
| 0x168 | 4 | Current player count |
| 0x16C | 16 | Application GUID |

A 4 byte gap at 0x154 lies between the description and the build id,
so the record is not tightly packed. The engine lists a session it
cannot join, greyed with the reason, see N-009.

A join (legacy:193511-193597) checks the build id and version first,
then the password hash kept on the slot's info block, then
DPSESSION_PASSWORDREQUIRED (0x400), and after those whether a slot is
free and whether watching is allowed. The engine checks a password on
the server, see N-006, and seats watchers outside the eight places,
see N-008.

Chat is message 5. A line typed in battle is taken at
legacy:137153, 137165 and cut to 0x101 bytes, and a line to chosen
players goes through legacy:194389. A received line goes to the
on-screen text messages on channel 8, the chat channel, where channel 2
is the system and channel 4 a warning.

## Which builds may share a room

This section describes OpenKingdoms and not the original, whose own
rule is the build id and version check under "Sessions and joining".

Lockstep sends orders, not state. Every machine in a match runs the
same simulation over the same orders and has to reach the same answer
bit for bit, so any difference in arithmetic is a desync some minutes
later.

IEEE 754 pins addition, subtraction, multiplication, division and the
square root to one correctly rounded answer, and floorf is exact, so
sqrtf and floorf agree on every platform and stay in the simulation. It
pins nothing for sinf, cosf, tanf, atanf or atan2f. Each platform's
libm rounds those its own way, a last place or two apart, and that
difference survives into a different cell after enough ticks. A
workload that happens to agree on two platforms is no evidence that the
platforms agree.

So simulation code never calls the platform's libm for those. It calls
the engine's own tak_sinf, tak_cosf, tak_tanf, tak_atanf and tak_atan2f
from include/tak_trig.h, built from pinned double constants and the
four pinned operations alone. They give the correctly rounded float for
every angle the game produces, on every platform by construction.
cmake/libm_guard.cmake, run as the test_libm_guard ctest on every
platform including the browser, enforces this as a grep with no
judgement in it over the files that write simulation state, their
drawing calls included, so no hand kept list of the calls that matter
can let a desync in. Two data free gates hold the rule in CI on
Windows, macOS, Linux and the browser: test_trig pins the arithmetic,
and test_sim_probe pins the simulation hash a synthetic battle reaches.

Headings, speeds and subpixel positions are still floats. That is
sound for lockstep, because every operation under them is pinned and
the simulation hash reads them by bit pattern. Converting them to fixed
point is a matter of precision and of the original's behaviour, not of
platforms disagreeing.

tools/float_probe.c hashes the bit pattern of four fixed workloads
through the platform's own libm, so two platforms can be compared by
hand: trig, a whole turn of headings through sinf and cosf, dist, the
distance between two units as sqrtf over a sum of squares, aim, the
angle from one unit to another through atan2f, and walk, a position
built up one tick at a time from sinf and cosf. Built with each
platform's release optimisation, it gave three answers on three
platforms when the engine took on its own trigonometry:

| workload | Windows, MSVC x64 | Linux, gcc and glibc | browser, Emscripten |
|---|---|---|---|
| trig | `87e65ad4` | `1224a512` | `d8532442` |
| dist | `2bbe6af1` | `2bbe6af1` | `2bbe6af1` |
| aim | `16b6de04` | `a421a6c1` | `fd3f792f` |
| walk | `c3c5dc73` | `bba8a81d` | `c3c5dc73` |

A determinism class names one float environment, and a room admits
only clients of its class. There is one class, TAK_CLASS_OWN_TRIG, so
Windows, macOS, Linux and browser players share rooms. The retired
classes TAK_CLASS_WINDOWS, TAK_CLASS_MACOS, TAK_CLASS_LINUX and
TAK_CLASS_BROWSER keep their numbers, so a build from before the engine
carried its own trigonometry, which really would desync, never lands in
a room with a later one. It is refused with
TAK_REJECT_DETERMINISM_CLASS, and the lobby lists such a room greyed
with the reason rather than hiding it. The protocol version, the engine
build id and the data and map fingerprints complete the rule, in
[MULTIPLAYER.md](../MULTIPLAYER.md).

## The room

This section describes OpenKingdoms' battle room on the relay, not the
original's, though it keeps the original's rules where an anchor is
given.

The room draws everything from the server's snapshot and nothing from
local guesses, which is the original's rule (legacy:136219-136222). The
Go box is frame 4 when the seat is ready and frame 3 when it is not.
The rule boxes draw the room's options word, the unit limit draws the
cap and the map name draws the room's map. The colour and team cells
have no art of their own in the GUI, so the badge is painted from the
side's team logo sheet with the team as a number, as the skirmish
screen does. A refusal from the server shows on the help strip.

The host may change the rules, the unit limit and the map, each as one
edit sent to the server. A guest pressing the same box sends nothing
and is told whose it is (legacy:136832). The map chooser is
choosemap.gui for the host and viewmap.gui for a guest
(legacy:136832-136851). OK on the host's chooser sends the map's name
and the fingerprint this install computed for it, so the room's map is
one particular file and not a name two installs might read differently.

Text input is on for the whole of the room, so a player just types.
Enter sends the line to the room, and lines from the server go under
the ones before, newest at the bottom, as the original keeps the list
(legacy:136856).

Typing a name in the lobby reconnects, because the name travels in the
greeting. A socket's callbacks act only while that socket is still the
game's link, so the old socket closing cannot mark the new link closed.
Enter, Escape and Backspace are latched when the key goes down, and the
screens read the latch as well as the held state, so a press that goes
down and up between two frames still counts.

The relay pings every client every two seconds, the cadence of the
original's heartbeat. A welcomed client heard from not at all for
TAK_RELAY_SILENT_MS, four heartbeats, is dropped whatever its socket
says, and a room it was alone in goes with it. Any frame that reaches
the relay's dispatcher counts as being heard, so an idle client that
answers pings is never dropped. The lobby is told when a room appears,
starts or goes, so a dead game leaves every screen on its own.

## Watching a match

This section describes OpenKingdoms and not the original. In the
original a watcher joined before the game started and took one of the
eight places. Joining a match under way works here only because the
relay keeps every turn of it, see D-033 and N-008.

### Getting in

Select Game lists every game on the server. A game under way shows
Watch on its row when its host allows watching, a watcher's place is
free and this client could build its world. Watch watches it. Join on a
running game watches it too, since a game under way takes no more
players, unless it has a computer's seat to drop in to (N-010). The web
page's list of games and the leaderboard show a Watch button for the
same games, which opens the page with `?watch=CODE`, and the engine
takes `--watch CODE` the way it takes `--join CODE`.

The relay adds the watcher to the room and to the match as a
simulation with no seat, and sends it the room state, START_GAME naming
no seat, GO and then the turns. Select Game builds the world from
START_GAME and goes to the loading screen. It does not open the battle
room, which is for a game that has not started. A player rejoining
their own match goes the same way, and for them the room state can
arrive after START_GAME.

### Catching up

A twenty minute match is 24000 turns, and a browser page holds 256 KB
of messages between two frames, so the whole log at once does not fit.
From protocol 5 the relay sends a catch up a window at a time, at most
128 turns and about 96 KB past the last turn the client acknowledged.
An empty run is cut to fit the window. While the stream has not reached
the latest turn, the live turns wait in the log and reach the client
from there, so it sees every turn once and in order. A page still
loading its world acknowledges nothing, so it holds the first window
and no more until the world is built. A client of protocol 3 or older
is sent the whole log at once. From protocol 10 a watcher, like a
returning player, starts from a snapshot of a player's world and the
turns after it when a world can give one, as
[MULTIPLAYER.md](../MULTIPLAYER.md) describes.

Once built, the battle runs the turns it holds as fast as a frame's
budget allows and says "Catching up with the game..." until it is less
than a second behind, as a rejoin does.

### What a watcher cannot do

A watcher never changes the players' game.

- It gives no order. The match refuses to send one, and the relay
  refuses a CMD from a simulation with no seat.
- Its world never counts when the worlds are compared. The players'
  hashes are judged among themselves. A watcher's waits for their
  agreement and is checked against it, and a watcher that differs is
  resynced alone. So a watcher can neither outvote a player nor halt a
  match.
- Its arrival moves no turn. A player coming back restarts the turn
  timer, which is right after a stall, but nobody waits for a watcher,
  so its arrival leaves the timer alone.
- Falling behind, it does not slow the turns, since the governor reads
  seated players only.
- It cannot pause or change the speed. The relay refuses both.

### Looking

A watcher holds no seat and has no fog of its own, so it sees the whole
map. Tab looks through one player's eyes, with their fog and their
sidebar, then the next player's, and after the last back to the whole
map. The line at the top of the play area says which. Which view shows
is presentation only, like the seat a player plays, since every machine
keeps every seat's fog. The camera is the watcher's own, and the 3D
view works.

A watcher's F1 menu does not save, load or restart. A watcher wins and
loses nothing, so no Victory or Defeat shows, and its report of the
verdict is not taken for the leaderboard. A watcher's machine records
the battle as a replay like anyone else's.

### A beaten player

A player whose army is gone while the others fight on stays to watch.
With Monarch Expendable off that is the moment the monarch falls, as in
a skirmish. The Defeat banner shows for three seconds, then the whole
map shows with no fog, and Tab walks the players' views as above. The
beaten client gives its seat up to watch, so from then on it gives no
order, its world is checked like a watcher's and never counts, and its
report of the verdict is not taken. The relay records a beaten player
who leaves early at the tick the battle beat them, not the tick they
left, so leaving never lifts their place. A skirmish lets its beaten
player watch the same way while two computers still fight, see D-034.

### Who is watching

From protocol 5 the room state names the watchers after the starts, so
the players see a line in the chat block when someone begins watching
and when they stop. A client of protocol 2 or 3 is written the room
state it always was.

### Leaving

Leaving the battle tells the relay the watcher left and disconnects,
and the players see it go at once. A watcher who left on purpose is not
handed the match back the next time its device says hello. A watcher
whose tab closed is, and catches up again as above. When a match has
used every simulation slot, a new watcher takes the slot of one who
left.

### The room list

A game under way is listed as closed to players, unless a client of
protocol 6 or later could drop in to a computer's seat in it. From
protocol 5 a client whose game data differs from the game's is told so
instead, so its list offers no Watch that could not work. An older
client reads the list as it always did.

## The leaderboard

This section describes OpenKingdoms and not the original.
openkingdoms.net/leaderboard.html shows every multiplayer battle played
through the relay: who has the most wins, their cumulative score, units
built and lost, and for each player the games they were in with the
place they took.

### What is recorded

A battle ends when the verdict fires in the simulation, by the rule
the end screen uses ([gameflow.md](gameflow.md), "How a skirmish
ends"). At that tick each client in the match sends the relay one
MATCH_RESULT: the match id the relay named in START_GAME, the tick the
verdict fired on, and for every seat the end screen's own columns off
the player record, units built, kills, losses, score and the last tick
with units, with whether the seat still stood and whether its army was
removed at once.

In lockstep every client holds the same numbers, so the relay takes the
first report from a seated player as the game's record and compares
each later one with it. A report that agrees raises the game's report
count. A report that differs marks the game disputed and changes
nothing. A report from a simulation with no seat, a watcher's or a
beaten player's, is neither taken nor held against anyone. The relay
refuses a second report from the same seat, a report naming another
match, one carrying a tally set it does not know, and one whose verdict
tick lies beyond the turns it has delivered to the room.

The relay joins the report to what it knows about the room: the map and
its fingerprint, the rule options, the unit cap, each seat's name,
side, colour, team and kind, and the wall clock times the match started
and ended, by the relay machine's clock. Computer seats are recorded
inside the game, so the game's page shows the whole end screen, and
never appear on the board. The tally set is versioned in the message
and in every record, so a column added later reads back beside the old
ones.

### Place and result

Everyone still standing when the verdict fires shares first place,
which is what allied winners are. Everyone else ranks by the tick they
fell, later being better, and ties share a place. A seat is standing
when it had units and had not resigned. The result is won for a
standing seat and lost for the rest, and a battle where nobody stood is
a loss for everyone in it.

### Leavers

The relay, not the reports, decides what happened to a seat whose
player left. The turn clock remembers the turn on which each seat's
player resigned, was removed by the host, or dropped and ran out the
reconnect countdown, and forgets it again when a dropped player comes
back and reclaims the army. When a report arrives, any seat whose
leaving turn is at or before the verdict tick is recorded not standing.
Its last tick alive is the first tick of the turn it left, or the tick
the battle beat it when that came earlier, whatever the report said.
Its other tallies are kept as reported. A player who left after the
verdict is recorded as the report has them. A battle every human leaves
before any verdict fires is not recorded at all, because nobody is left
to report it.

### Who a player is

A player is the device they play on. Every client says hello with a
device token, sixteen random bytes made once and kept with its
settings, the same token a rejoin is recognised by. When a match goes,
the relay notes the token of whoever sat in each seat, and the record
keys each human seat on a player id made from it: the first eight bytes
of SHA-256 over a fixed label and the token. The token is never shown.
The id is public, and nobody can work back from it to a token that
makes it, so a player's record cannot be played onto by typing their
name. Clearing the browser's storage or playing on a second machine
makes a second player, and two players cannot be merged. That is the
cost of having no accounts.

The board shows each player by the name they last played under, and a
game shows each seat by the name typed for it then, with the player's
present name beside it when that differs. Two players who go by one
name are two rows, and the page adds the end of each one's id to tell
them apart.

A client from before tokens says hello with sixteen zeros. It plays as
it always has, with the same bytes on the wire, and its seat is
recorded in the game under its typed name, but the seat counts for
nobody, because a typed name is anybody's.

Records from before devices keyed a player by the name, trimmed and
compared without case. They are kept exactly as written, and a seat in
the file says which kind of id it holds. Those rows are frozen. They
stay on the board and on their own pages, and no device ever takes one
over, because nothing a device can show proves it was the person who
typed that name. A player from before devices keeps the old row beside
the new one, with no way to join the two.

One device can sit in two seats of one game, from two tabs of one
browser, which share a token. A player cannot both win and lose one
game, so that game counts for neither seat. It is still shown in full.

### One table per mod set

A modded game is a different game, so it is not ranked beside vanilla.
Every record is filed under the host's mod set, the name its greeting
gave and the data fingerprint the room was made with, which every seat
matched to get in. Each pair is a table of its own. Its id is the mod's
name folded to lower case letters, digits and dashes, a dash, and the
fingerprint in sixteen hex digits, for example
`vanilla-3f9a1c0e7a11d2b4` or `tak-enhanced-e4a1000000000014`. The name
is compared without case, so two hosts who type one mod's name
differently still share its table when their data is the same, and two
installs that both call themselves Vanilla on different releases keep
apart, since they could not share a room either.

A mod that changes only art or sound leaves the fingerprint at
vanilla's, so its players can join vanilla games, but a game it hosts
is filed under its own name. Hosting with Vanilla chosen puts the game
on vanilla's table. A host from before protocol 4 names no mod set, and
its games go on a table called `unnamed-` and the fingerprint. Games
recorded before tables carry neither, so nothing says which data they
ran on. They stay on a table of their own, Earlier games, rather than
being guessed onto vanilla's.

The page asks `/api/tables` which tables there are, vanilla first and
then by how many games each holds, and opens on the vanilla table with
the most games. A picker above the table switches between them, and the
address keeps the choice, so a table can be linked. The table, both
games lists and a player's page take `table`, and without it they count
every game.

### Disputed games

A disputed game stays in the ledger and is shown in full, flagged, on
its own page and in the games lists. It is left out of every sum: the
table and a player's row count only undisputed games, and the table
says how many games were left out so the omission is visible. The relay
is the only witness and the first report stands as the record, so a
player who reports first and lies can take one game off the board for
everyone in it, but cannot put a win on it.

### Where it is stored

The relay keeps one append only file, given by `--store PATH`. It
starts with a magic and a version, then records back to back, each a
tag, a length, a payload and a CRC32 over all three, written with the
same bounded writer the protocol uses.

| Tag | Record |
|---|---|
| 1 | A finished match |
| 2 | A confirmation or dispute of a match |
| 3 | A finished match with seats keyed by device, which is tag 1 with one more byte a seat saying which kind of id it holds |
| 4 | Never written. A build that never shipped put name claims there, and a reader steps over it by length. |
| 5 | A finished match filed under a mod set, which is tag 3 and then the mod's name and version and the data fingerprint |

A match with no device seat is still written as tag 1, and one with no
mod set as tag 1 or 3, so a file from before devices or before tables
is rewritten byte for byte as it was. A relay from before devices skips
tag 3 by its length, counts those as records it could not read, and
then numbers new matches from its own count over ids the skipped
records hold. So a relay from before devices is never run on a file
that holds tag 3, nor a relay from before tables on a file that holds
tag 5. Keep a copy of the file before rolling a relay back.

The file is read whole into memory at start, and every question the
site asks is answered from memory. The ledger holds 8192 matches,
about five megabytes, and the first 8192 distinct players. Past that
the relay refuses to record and counts what it refused. The ledger
numbers matches with its own id, which the site uses. The relay's match
id restarts with the process.

A record that does not check is stepped over a byte at a time until
one does, so one bad byte costs one record and not the rest of the
file. A record whose length is beyond any record the relay writes is
stepped over the same way. Whatever was stepped over is dropped by
writing every good record to a file beside the old one and renaming it
into place, so the original is never cut short. A record with a tag
this build does not know is skipped by its length. An empty file is a
new ledger. A file that is not a ledger is refused and left exactly as
it is, and the relay then keeps results in memory and says so loudly
rather than stop serving games. A checksum rather than a sync mark
catches a wrong byte anywhere in a record in one check, and is what
lets the reader recover.

A flat file rather than SQLite keeps the relay one static binary with
no third party code and no allocation. The file has to live on
persistent storage, which on a host that discards the machine's disk
between runs means a mounted volume and a `--store` path on it. Without
`--store` the relay keeps results in memory until it restarts and says
so at start.

### How the page gets the data

The relay answers plain HTTP on the port the WebSocket uses. A
connection whose first bytes are a GET rather than an upgrade gets one
JSON response and a close. The routes are read only: the table, one
player with their games newest first, recent games, one game in full,
the maps games were played on, the tables there are, and a health line
with a version stamp. Their paths are listed in
[MULTIPLAYER.md](../MULTIPLAYER.md), "Hosting your own". The table can
be searched by the name a player goes by now, and both games lists by a
player's name as typed then or now, by a piece of the map's name, and
by the span of dates the game ended in. A date on the page is the
reader's own day, sent as the unix milliseconds it starts and ends at.
Every answer carries `Access-Control-Allow-Origin: *`, because the page
and the relay are on different hosts and the data is public. Every list
is paged, with caps sized so the largest page fits the relay's one
response buffer, and a page of games holds 25.

The relay answers HTTP on the thread that relays turns, so a slow
answer pauses every game being played. What the answers share, the
table, each player's present name, the maps played and the list of
tables, is worked out in one pass the first time a question is asked
after the ledger changed, and kept until it changes again. One table's
rows are kept the same way, until the ledger or the table asked for
changes. A list with a filter walks the matches once.

The page is web/leaderboard.html, plain HTML, CSS and JavaScript in the
game page's own colours and faces, copied to the site by the web
workflow, with its working out in web/leaderboard.js. It finds the
relay from relay.txt beside it, as the game page does, and turns the
wss address into https. It draws from nothing when the ledger is empty
and says so when the relay does not answer. Above the table and the
games list it shows the games not yet over, from `/api/rooms`: the open
ones with a Join link, the same `?join=` link the game page takes, and
the ones being played with how long they have run.

The front page links the leaderboard from its fine print and from the
games panel. Over a running game the page's own link strip, beside
Saved games, opens it in a new tab so the battle keeps its tab. When a
seated player's game sends the verdict to the relay, it asks the page
to offer a link to the player's own page, worked out from the same
token, on the strip over the end screen, because the end screen is the
original's own dialog. A desktop build has no page and offers nothing.

The page updates while open by asking `/api/health` every ten seconds
and redrawing the current view when the version stamp moved or the
relay has just come back. The games not yet over change without the
stamp moving, so their card is asked for on its own in the same poll.
One request is in flight at a time, and a draw that finishes after a
newer one was started is thrown away. It polls rather than being
pushed to, because the relay answers a request and closes and holds no
HTTP connection open.

## Replays

This section describes OpenKingdoms. The original has no replays, see
D-032.

### What a replay is

A lockstep battle is a pure function of three things: the battle
configuration with its seed, the engine and data that run it, and the
commands its queue applies. A replay records the first, checks the
second, and records the third as the queue applies it. Nothing else
goes in. The computer players give no commands through the queue,
because every machine runs them from the seed, and so does playback.

The recorder sees every command the moment before it is applied and in
the order it is applied, whether it came from this machine or from the
relay. A command carries the tick it ran on and the seat it belongs to,
so a match and a skirmish are recorded alike.

### When a battle is recorded

A skirmish or a match is recorded when the battle screen opens on a
world at tick zero. A campaign mission is not, because its script and
placements are not in the header. A battle loaded from a save is not,
because it starts part way through and a replay starts from the seed.
The `RecordReplays` setting turns recording off when it is 0. A
recording of under one second of battle is deleted on close.

The file goes in the saved game directory as
`YYYYMMDD-HHMMSS.okreplay`, from the time the battle started in UTC. In
a browser that directory is the one the page already copies to origin
private storage, so a replay is kept the way a save is kept, and the
page's Saved games panel writes one out to a file and reads one back.

While a battle records, the file is flushed every 10 checkpoints, ten
seconds, and in a browser the page is asked to copy it to storage at
the same time. A tab that closes loses ten seconds of battle at most.

### How much is kept

The directory keeps the newest 30 replays within 48 MB
(`TAK_REPLAY_KEEP`, `TAK_REPLAY_BUDGET_BYTES`). A new recording prunes
when it opens and again when it closes, oldest first by the time in its
header, and a replay whose header will not read counts as the oldest.
Once one replay does not fit, every older one goes with it. Only
`.okreplay` files are pruned, never a save. The page keeps browser
storage to the same two numbers by the files' modification times, after
every copy out and before it reads the saved games in, and it removes a
pruned file from the running game's filesystem too, so nothing copies
it back. A replay in storage past the 16 MB guard is pruned and never
read in.

### The file

All numbers are little endian. The header is 576 bytes:

| Bytes | Field |
|---|---|
| 8 | `OKREPLAY` |
| 2, 2 | Format version (1), header size |
| 4 | `TAK_ENGINE_BUILD_ID` |
| 8, 8, 40 | Data schema, content and the five group hashes |
| 32 | Map fingerprint |
| 1, 1, 1, 1 | Flags, the recording seat, a match's turn ticks, spare |
| 8 | When it was recorded, UTC seconds |
| 96, 32 | Map name and the map's kingdom |
| 4, 4, 8 | Seed, units per side, the eight rule toggles |
| 8 x 38 | Each seat's kind, side, team, colour, difficulty, start and name |
| 4, 4 | Last tick and command count, written on close |
| 4 | FNV-1a of the 572 bytes before it |

The flags say whether it was a match, whether the map fingerprint is
present, whether the recording closed, and whether the battle read its
models from the archives alone, as a match does and as any battle does
when the game folder has no objects3d models. A battle that read the
game folder's objects3d instead has that folder's model hash folded
into its content hash by XOR, so its replay plays only with the same
models.

After the header come records. Each is a tag byte and the ticks since
the previous record as an unsigned LEB128 of at most five bytes, then:

- for a command, the seat, the wire length as a LEB128, and the command
  in the wire format the relay carries
- for a checkpoint, the simulation hash after that tick's orders ran,
  and the FNV-1a of every record byte before this record
- for the end, the FNV-1a of every record byte before it

A checkpoint is written every 60 ticks, the cadence a match already
hashes at, so a match computes one hash for both. No record may name a
tick more than 60 past the last checkpoint, and none past eight hours
at 60 Hz (`TAK_REPLAY_MAX_TICKS`). The writer stops taking commands at
16 MB, at eight hours, or at a tick no checkpoint reaches, and the file
then ends at the last tick it can vouch for. A busy hour of a match is
a few hundred kilobytes, so the 16 MB guard is not a limit a battle
meets.

### Reading one back

The reader first refuses a file larger than 16 MB, which costs one
seek, and a header whose fields fall outside what the engine has: a
seat's kind, side, team, colour, difficulty and start, the units per
side, the rule toggles, a match's turn length, the flags, a local seat
that is empty, a battle with no one in it, a last tick past eight
hours, and a map name that is not a plain name. The writer holds a
header to the same limits before it records anything. A player name
reaches the writer as the room or the settings file carried it, so the
writer first turns any control byte in a name into a space and each
rule toggle into 0 or 1. A battle whose setup still falls outside the
limits is not recorded, so a replay that was recorded always opens.

Then the reader walks the whole stream once before any world is built,
a window of 8 KB at a time, and refuses a file that is damaged anywhere
or that breaks the tick rules above. The header's checksum covers the
header, and every record is under the stream checksum that the next
checkpoint or the end record carries, so one wrong byte anywhere is
caught. A closed recording must reach its end record with the tick and
count its header names and nothing after it, so a closed file that was
cut is refused. A recording that never closed, which is what a browser
tab shut mid battle leaves, plays up to its last checkpoint and drops
what came after.

Then the build and the data are checked. Determinism needs the same
engine and the same data, so a different `TAK_ENGINE_BUILD_ID`, a
different data schema or content hash, or a different copy of the map
is refused with a sentence saying which. A differing data group is
named, the way the lobby names one. Neither the writer nor the reader
holds more than its own fixed size, however long the battle.

The Replays list reads each file's header and nothing more, at most 64
files, so opening it costs a few hundred bytes a replay. A damaged
stream shows in the list and is refused when chosen. A recording that
never closed shows no length until then.

### Playback

The world is built from the header the way a match builds it from
START_GAME, and the local seat is the recording player's, for their fog
and their sidebar. Before each tick the commands recorded for it go
into the queue, and every local order is refused at the one place
orders leave the interface. At each checkpoint the simulation hash is
compared with the recorded one. The first that differs is reported on
the screen as the time the battle drifted from the recording, and in
the log with both hashes.

Space pauses and resumes. The = and - keys step through 1x, 2x, 4x and
8x, which scale the tick rate and never what a tick does. The camera
and the selection are free.
