OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

## Changed in 0.1.5

The 3D view draws your own models. Put a glTF 2.0 model from Blender in
a models3d folder inside your game folder, named after a unit's object
name, aralode.glb for the Aramon lodestone, and the 3D view draws it in
place of the shipped one. This works in a browser too: pick your game
folder on the page, or drop a .glb on it at any time. The engine reads
what Blender writes: base colour, normal maps, roughness and metal,
emission, transparency, both faces where a material asks, and the
second UV map when the material is laid by it. A material named
teamcolor takes the owning player's colour, and one named for its glow
breathes. Glass and polished metal catch the light along their edges. A
model's pictures are decoded once and shared by every team colour. A
custom property named tak_scale on any object sets the size, so a model
authored in metres need not be rescaled. docs/CUSTOM_MODELS.md has the
whole of it.

Custom pieces move. An object in the model named like a piece of the
shipped model turns, moves, hides and shows as the unit's own script
drives that piece, the script that has always rowed the oars and blinked
the lodestone's light. A model with objects named aralode and
aralode_off blinks as the original does, and a Veruna ship with Oar1 to
Oar6 rows. Nothing has to be animated in Blender.

Standing stones stand up. The stones around a mana site, and most trees,
rocks and ruins, are flat pictures in the original, and in the 3D view a
picture can only lie on the ground. A model named after the picture's
sequence, verhenge01.glb for the first Veruna standing stone, stands
where the picture lay.

Replacement models for the classic game. A loose .3do under objects3d
in your game folder stands in for the shipped one, in both views, which
is how a model mod for the original arrives. The twenty rebuilt
buildings from TA:K Enhanced go straight in, and the browser page
carries them along with your archives.

The build preview in the 3D view is the building itself, standing at
the site the pointer picks, at the camera's angle, tinted green where
the site will take it and red where it will not. It was a picture at the
classic angle pasted over the scene.

The main menu and Options are put right. Buttons showed their pushed in
face whenever the pointer was over them, and now only while held. Each
Options tab lands inside its frame, the selected tab stays pushed in,
the tab buttons keep the size of their art, the help text sits in the
strip the dialog has for it, the version line stays on, and Exit works,
where before it did nothing. The menu doors keep their size when a hover
starts their clip, and open again for a cursor that came back during
the leaving clip. The exit door's caption reads Exit to Desktop, since
the game runs on more than Windows now, and it is recorded as a deviation.
These are the work of Jiří Doubravský.

## Changed in 0.1.4

There is a 3D view. Press V in any battle and the same world is drawn
in 3D under a camera you can orbit, tilt and zoom, with the terrain
built from the map's own heights and textures, water at the water line,
and every unit and building as its own model moving as the classic view
moves it. Shots in flight, the bursts where they land, beams and spell
effects are drawn in it too. Press V again and the classic view is back
with nothing lost. It is experimental and it says so when it opens.
Nothing in the simulation changed for this, and a player in the 3D view
can play a match against a player in the classic one.

The 3D view works in a browser tab. Only its first frame ever reached
the screen there, and once frames moved the whole play area was washed
blue, because the scene was drawn with no depth buffer and the water
plane covered everything. Building works in it as well: the placement
ghost stands where the pointer is, so a lodestone can be put down.

Spells look like themselves. A dragon's breath and every other flame
came out as a lightning bolt, and the great area spells, Elsin's
Earthen Wave among them, came out as a disc flying at the target. Each
is drawn from its own weapon data now, the way the original draws it.
Flames stream from the muzzle. Earthen Wave, Earthquake, Ring of Fire,
Fire Wave, Tsunami, Water Blast, Wind Wave, Shockring, Death Aura and
Area Mind Control spread their rings in their own colours. Hail Shower,
Fire Storm and Ice Storm rain over their area and burst where they
land. This holds in both views.

Computer players move on to their next factory. A seat priced only the
first production building on its list, so once it had built as many of
those as its profile allowed it never built another kind. Taros never
reached the Dungeon, which is why its fire demons were never seen.

Scrollbars scroll by their arrows. Every scrollbar in the game moved by
dragging its ball and not by the arrows at its ends, because the arrows
sat inside the list they scrolled and the list took every click.

The saved games bar in the browser sits on a dark plate, so it reads
over the Book of Deeds rather than vanishing into its gold.

## Changed in 0.1.3

Every build can now play every other build. Until this release a Windows
player, a Mac player, a Linux player and a browser player were each kept
in a separate room, because each platform's maths library rounded a sine
its own way and the difference pulled two machines apart over a match.
The simulation carries its own trigonometry now, identical on every
platform by construction, and a test plays out a battle and compares the
result across all four so it stays that way.

Story mode is a campaign rather than a mission launcher. The Book of
Deeds lists the campaigns it finds, so the Iron Plague is reachable for
the first time, chapters carry their written titles and artwork, and
finishing a mission returns you to the book with the next chapter open.

Campaign missions can be won. Victory and defeat conditions were treated
as one list that all had to hold at once, and losing your whole army was
in it, so twenty nine of the shipped missions could not be completed at
all. They are two lists now, as the original has them, and defeat fires
properly. Twenty three further missions are won by their mission script,
which is the next piece of work.

Computer players build armies. A seat refused to start anything while
any of its units was under construction, and refused a second production
building while it owned one, so the first castle it began was often the
last thing it built. Its army target also stopped growing under fog. In a
measured match a computer player went from twelve units built to forty
two.

Shells land where they should. A lobbed shot left the middle of the unit
rather than the barrel, and detonated when it reached its target rather
than when it met the ground, so a cannoneer on a hill would shoot
through a ridge.

An order now lands where you clicked. The ground draws lifted by half
its height and units draw the same way, but an order carried the flat
reading of the pointer, so a unit walked to a spot up to 45 pixels above
the click and missed further the higher the ground. Walking up the
screen it overshot and walking down it stopped short. Selection boxes
had the same fault and could miss a unit they were drawn over. This was
never right on any platform rather than something that broke recently.

With Line of Sight off, ground you have explored stays explored and
ground you have not is black, which is what the original does. The
option grants sight. It never revealed the map.

## Changed in 0.1.2

Windows, macOS, Linux and browser players share rooms. Before this each
platform played only its own, because each one's maths library rounded a
sine its own way and two machines would drift apart over a match. The
simulation now does its own trigonometry and reaches the same answer
everywhere.

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

A Windows, macOS, Linux or browser player can all sit in one room. Each platform's maths library used to round a sine its own way, which is enough to pull two machines apart over a match, so the handshake used to refuse the mix. The simulation now carries its own trigonometry and every build reaches the same answer, checked on all four platforms before a release is built. A build from before that change is still refused, listed greyed with the reason, because it really would desync. The details are in [docs/notes/2026-09-14-float-determinism.md](https://github.com/OpenKingdoms/OpenKingdoms/blob/main/docs/notes/2026-09-14-float-determinism.md).

Problems go to [issues](https://github.com/OpenKingdoms/OpenKingdoms/issues), with the version line from the main menu.
