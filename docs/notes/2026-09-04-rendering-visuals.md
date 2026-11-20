# Rendering and the classic view

How the original draws a battle in its classic view: the order of a
frame, the scale and projection, terrain and water, models and their
sprites, team colour, a unit under construction, the overlays, the fog,
the effects scripts and weapons ask for, and death. The last section
describes the 3D view, which is OpenKingdoms' own.

A `legacy:NNNNN` anchor points into the maintainers' reference of the
original, where a claim can be checked. The engine's deliberate departures
are in [MANUAL_DEVIATIONS.md](../MANUAL_DEVIATIONS.md), cited by ID.

## Frame order

| What is drawn | Anchor |
|---|---|
| The whole frame | legacy:210163 |
| Terrain tiles and the animated water | legacy:225667 |
| Scene objects (features, units and overlays), bucketed by row | legacy:210467 |
| A feature, corpses and the magic death among them | legacy:211089 |
| One unit: its shadow, LOD and blend | legacy:197117 |
| A unit's screen bounds: the projection and the LOD divide | legacy:196843 |
| A unit's raster into its sprite: the water tint and clip, the nanoframe copy | legacy:196958 |
| A model's primitives: projection, culling and order | legacy:197561 |
| The fog overlay, hardware and software paths | legacy:130167, legacy:130522 |
| Any 2D GAF picture | legacy:209631 |
| A health bar and a selection ring | legacy:210837, legacy:210919 |
| The minimap and radar, on the RADAR_PICTURE, MAPPED and FINAL surfaces | legacy:207443-208600 |

A frame draws the terrain tiles, then the scene objects, then the
minimap, then the HUD (legacy:210228-210273).

In the scene pass every feature shorter than 10 world units is drawn
first. Taller features wait in buckets by Z row. Then, row by row, the
row's ground units draw and after them the row's tall features. Air
units draw last, over everything. Health bars and group digits follow,
and the fog overlay is a pass of its own after those, so it lies over
flyers too.

A row's index is world Z over 16, less camera Y over 16, plus 32: the
object's map cell row less the camera's, biased by 32 rows to stay
positive (legacy:210555). Three bucket lists are kept side by side.
There is no depth sort, only the Z rows.

## Scale and projection

One world unit is one screen pixel. Screen X is world X less camera X,
and screen Y is world Z less half of world Y, less camera Y. Nothing in
the path multiplies (legacy:197195, legacy:210761, legacy:225772).

A map cell is 16 world units, 16 px, and a cell's world position is its
index times 16 (legacy:226350). A graphic tile is 32 px, 2 by 2 cells
(legacy:225797). Simulation positions are 16.16 fixed point in that
same pixel space.

Model vertices are 16.16 fixed point as well. The integer part goes
straight to a screen coordinate, truncated to 16 bits, with no scale
factor (legacy:196879-196888, and in the rasteriser
legacy:197683-197689). One model unit is therefore one world pixel, and
a model's scale is exactly 1/65536 of its raw values, not a free
parameter. The shipped data agrees. ARAWALL's model spans 32.0 units on
its 2 by 2 cell footprint, ARANGATE 224.0 on its 14 by 4 and ARAAT 48.0
on its 3 by 3. Each is its footprint X times 16 px to the pixel, so
walls and gates tile only at this scale.

In a unit's sprite a model vertex projects to screen X = X and screen
Y = -Z - Y/2 (legacy:196879-196892). A skew variant also exists, with
screen X = X + Y/4 and screen Y = -Z - Y/4. Either is then divided by
the LOD scale, and the bounds carry 2 px either side. LOD divides only
the sprite raster, by 1, 2 or 4 (legacy:196889-196892), and the blit
magnifies it again.

## Terrain and water

The terrain is drawn flat, with no height displacement of its tiles
(legacy:225667-226016).

The tile map holds one 4-byte record per graphic tile: a 16-bit atlas
page index, then the tile's X and Y within that page as bytes. Tiles
are half the map's cell resolution, so a tile's record is at tile row
times half the map's width in cells, plus tile column. A page table
gives each page its pixel width, pixel height and texture. A tile's UVs
are its X and Y divided by the page's size counted in 32 px tiles. A
tile in column c lands at x = viewport left - (camera X mod 32) + 32c.

To tell land from water (legacy:225818) the four corner cells' heights
are sampled, and a corner's depth is the sea level, a global byte, less
its height. When all four depths are below 1 the tile is land, one quad
in colour 0xFFFFFFFF. Otherwise it is water and draws in two passes:
the terrain tile modulated by 0xFFC0C0C0, the seabed at 75 percent, and
then a second quad in blend mode 2 carrying an animated colour at each
corner.

A corner's animated colour is built in five steps.

1. A depth of 0 or less makes the corner shoreline, with the fixed
   colour 0xFF404040. There is no separate shoreline term. The gradient
   at the shore comes only from interpolating this dark corner against
   the animated ones.
2. Otherwise the amplitude d is the depth times a global water
   amplitude scalar, over the sea level.
3. The phase is an entry of a pseudo-random table of 171 (0xAB)
   entries, indexed by the corner's index mod 171, times a global phase
   rate scalar, times the elapsed seconds. Elapsed seconds are wall
   clock milliseconds times 0.001, not the simulation tick, so the
   shimmer keeps moving while the game is paused or slowed.
4. Two sine terms are each scaled by d: the sine of the corner index
   plus the phase, and the sine of a second 171-entry table's entry at
   the same index plus the phase. The second table serves as a per-index
   phase offset.
5. The two terms are summed, 64 is added, and the low 8 bits of the
   integer are taken, so the value wraps at 256 and is not clamped. The
   result c is a grey level, and the corner's colour is opaque grey,
   alpha 0xFF with R, G and B all c.

Neighbouring tiles share their corners' phases, so the shimmer runs
across tile edges with no seams. The values of the amplitude and phase
rate scalars are not established.

Water needs a global water-enable byte and a renderer that answers yes
to a 3D capability query (legacy:225782). The software path
(legacy:225942) has no water at all, only a plain tile blit that merges
horizontal runs. Nothing cycles the palette.

## Model draw order

The rasteriser (legacy:197561) walks a model's node table backwards,
from the last node to node 0 (legacy:197658-197661 for the start,
legacy:197944-197946 for the step). Inside a node the primitives run
forward (legacy:197793-197799 for the start, legacy:197938-197941 for
the step), and the node's selection primitive is skipped
(legacy:197794-197797), in the vertex pass as well
(legacy:197712-197715). A face draws when its cross product is 0 or
more (legacy:197804-197807). The node table is the 3DO file's
depth-first order with the root first, and a script's show and hide
index it directly (legacy:223432-223463). ARAKING's selection primitive
is primitive 0 of AraGP, a flat ground plate of four vertices.

That order only breaks ties. Each transformed vertex carries a third
value, a base plus its model space Y, where the base is 0x32, or 0x7D
when the unit's definition floats on water (legacy:197697). The polygon
filler interpolates the value across the span and tests it against a
byte buffer kept beside the sprite. An opaque face writes a pixel only
when its key is greater than the one already there
(legacy:265317-265341), and a remapped face when it is greater or equal
(legacy:265360-265372). The underwater tint reads the same buffer
later.

The key reads as a height, but under screen Y = -Z - Y/2 it is a depth
test. Two surfaces landing on one pixel satisfy Y = -2(screen Y + Z),
so the taller sample is the nearer one. That keeps a monarch's head in
front of his torso, his torso in front of the cape hanging behind it,
and a keep's tower solid rather than a shell of back faces. The node
order gets none of these right on its own, and a keep is a single node
of 120 primitives anyway. The buffer holds bytes, so the key is in
whole units and coplanar pieces tie, which leaves them to the node
order.

ARAKING shows both rules. In reverse node order he paints legs,
shoulders, Head, Cape4 to Cape1, the blood emitters, sword, hands and
arms, Torso, Hip and the root. His cape is a chain from Torso through
Cape1, Cape2, Cape3 and Cape4 down his back, with offsets (0, +816773,
+207255), (0, -552211, +206255), (0, -783915, -11717) and (0, -801995,
-18020), and script pieces 17 to 20 map to it by name. His walk turns
Cape1 from 0 to +9 degrees about x, Cape2 from 0 to +24 (and about z
from -3 to 0), Cape3 from -20 to +18 and Cape4 from -30 to +32, a step
about every 66 ms of sleep. A positive x turn swings a hanging piece
backward (see Piece axes in
[2026-09-09-cob-entry-points.md](2026-09-09-cob-entry-points.md)), and
the chain's summed angle stays at about 0 or more at every keyframe, so
the authored cape never crosses the torso plane. At its worst frame the
chain is 0.5 to 0.9 million model units behind the torso's centre.

The engine rasterises on the GPU rather than in the original's 8-bit
software path, see R-001, and samples model textures half a texel in
from their edges, see R-005.

## Unit sprites

### Shadows

A unit casts a shadow (legacy:197182-197196, legacy:197428-197431) when
all of these hold.

- The `DrawShadows` video setting is on.
- The render object's suppress-shadow flag is clear.
- The unit's definition does not carry flag bit 0x2000000. It is the
  same bit that turns on 2x supersampling, so the units that get
  supersampling are exactly the units that cast no shadow. Which FBI
  key sets the bit is not established.
- A global shadow-enable byte is set, which the console toggles
  (legacy:38584).

The shadow sits at screen X = unit X - camera X + 5 px and screen Y =
(unit Z - camera Y) - ground height / 2, where the ground height is the
larger of the bilinear terrain height and the sea level. So a shadow
lies on the ground and not under the unit's own elevation. The bilinear
height (legacy:226234) weights the cells' heights in sixteenths of a
cell.

A unit still being built casts its shadow only while less than half of
its build is left, in blend mode 6 at alpha (0.5 - left) times 255, so
the shadow fades in to half strength over the second half of the build.
With nothing left it draws in blend mode 5, solid
(legacy:197309-197322, and legacy:197252-197265 for a unit with a
shadow sprite). This is the test at legacy:197310, which belongs to the
shadow pass and not to the body.

The `ShadowScale` video setting picks the shadow raster's divisor. 0
picks it by the frame time budget, and 1, 2 or 3 give divisors 1, 2 or
4.

The shadow's silhouette is built from a different set of pieces than
the body: piece selection tests bit 1 of the piece flags against the
pass's selector (legacy:197663). A feature's shadow is its type's
shadow picture. A weapon's shadow is its `shadowgaf` and `shadowart`,
loaded into a second animation slot and blitted on the ground under the
shot (legacy:250152, legacy:246711). The FBI key `noshadow` is read at
legacy:163001 and `shadtrans` at legacy:127343.

The engine departs in how it draws shadows, see R-002, R-003, R-004 and
R-006.

### Underwater tint and clip

The tint and clip (legacy:197018-197036) start from the unit's
elevation and a base of 0x7D when its definition floats on water, 0x32
when it does not. Below sea level the cut level is sea level + base -
elevation, and every rasterised pixel whose key is at or below it,
other than the transparent key, is remapped through BLUE_TABLE
(legacy:256326-256345). A floating unit at or above sea level is
clipped hard instead, and its pixels below level 0x7D become the
transparent key (legacy:256936).

Both work off the key the rasteriser writes into the byte buffer beside
the sprite, interpolated from the third vertex value, the base plus the
vertex's model space Y (legacy:197691-197696). BLUE_TABLE is a 256-byte
table on the game instance, built at legacy:271094. It is the `.blu`
underwater remap.

### Supersampling, stone and shading

2x supersampling (legacy:197621-197657) needs the 2x AA video setting
and definition flag 0x2000000. It applies to the body pass only.
Coordinates are doubled, the raster is prefilled with palette index 9,
and the result is decimated 2 to 1 on the way out (legacy:197057).

Stone and freeze (legacy:197875-197886) force every primitive onto the
side's `stonegaf` or `frozengaf` texture, read with the side data at
legacy:164740-164756.

Flat shading (legacy:197710-197791) takes a normal for each primitive
from the cross product of two edges, used only when the per-polygon
shading video setting is on and bit 2 of the piece flags is set. The
light level is the dot product scaled by a constant, made an integer,
plus the ambient level. With shading off every face takes the fixed
light level 0xF.

## Team colour and palettes

Team colour is a set of pre-baked texture variants, one for each
colour, and not palette indices 16 to 23. A texture object holds a
handle for each colour, and the renderer's set-ambient entry point
picks the variant for a colour index (legacy:255123). For each
primitive (legacy:197904-197911):

| Primitive flags | Texture used |
|---|---|
| Team colour bit set | The variant for the unit's team colour |
| Animated bit set, team colour bit clear | The animated frame |
| Both clear | The primitive's own texture |

The colour index runs from the unit's player index to that player's
record, to the player's side record, to the side record's team colour
index. The unit's player index is stamped from the player record at
legacy:226508, and the player record also carries its side index.
Minimap blips (legacy:208505) and selection markers (legacy:209953,
from the side data table) pick their colour the same way.

The side records (legacy:164740-164817) carry `FogColor` (in BGR
order), `logogaf`, `stonegaf`, `frozengaf`, `buildsparklygaf`,
`resurrectsparklygaf` and `nimbus`. Each side also names a palette in
gamedata/sidedata.tdf, read with its shade tables at
legacy:164721-164729, and a model's textures are drawn in it. Each also
names a `buildpalette`, see Intangible mass.

All eight side palettes hold the same colours. Colour 172 is (39, 19,
0) in the side palette, (24, 66, 16) on an Aramon map and (189, 156,
132) on a Veruna one. A model's untextured faces share the 8-bit unit
buffer with its textures, so they would take the side palette too, but
that is a reading and not established. The Harpoon Ship's harpoon is
mostly such faces.

The game instance holds these lookup tables, built at
legacy:271010-271101, with helpers at legacy:271173, legacy:271203 and
legacy:271247.

| Table | Size | Purpose |
|---|---|---|
| ALPHA_TABLE | 0x10000 | 256 by 256 blend pairs to the nearest palette index (legacy:271115) |
| SHADE_TABLE | 0x2000 | 32 by 256 |
| LIGHT_TABLE | 0x2000 | Not described |
| GRAY_TABLE | 0x100 | Not described |
| BLUE_TABLE | 0x100 | The `.blu` underwater remap |

## Intangible mass

A unit under construction is drawn as an Intangible Mass, the name the
game's own status line gives it: `UNITMISSIONCODE_BEINGBUILT =
Intangible Mass` in english/translate/unitmissions.tdf. The original
draws the unit's silhouette filled with a shimmer of its side's build
colours, fading in over the first half of the build and back out over
the second.

A unit keeps the share of its build still to do, 1.0 when the site is
placed and 0.0 when the work is done (legacy:39496-39500). Every test
below reads that share, so under half left means more than half built.

The model is rasterised into a sprite whenever the unit needs a fresh
one. While the unit is unfinished the raster is also copied into a
second buffer of the same size (legacy:197003-197017).

The body sprite is drawn only while less than half the build is left
(legacy:197474-197476). It goes out at full alpha in the plain blend
mode, the same as a finished unit (legacy:197441-197466). The body does
not fade. It appears whole just past half built.

The second buffer is drawn over the body while any of the build is left
(legacy:197480-197489). Before it is drawn, every pixel the unit covers
is replaced by a palette index from 0x20 to 0x9f, picked per pixel from
a hash of the pixel's column, its row and the game tick
(legacy:198241-198271). One term of the hash slides along the row every
two ticks and the whole index steps every four, so the colours shimmer
and crawl. The buffer becomes a texture through a second palette the
side keeps beside its main one (legacy:197482). That palette is the
`buildpalette` named in gamedata/sidedata.tdf (legacy:164730-164734),
and a unit uses the one of its definition's side (legacy:162574,
legacy:179628).

The shipped sides name four build palettes. Aramon and the three sides
without a commander use arabipal.pcx, and Taros, Veruna and Zhon use
tarbipal.pcx, verbipal.pcx and zonbipal.pcx. Entries 0x20 to 0x9f of
each are one gradient, run up and back down. Aramon's is yellow green,
Taros orange, Veruna cyan and Zhon a pale blue close to white, the
range of colours footage of the original shows.

The mass draws in blend mode 6. Mode 4 draws with no blending, mode 5
keeps the transparent key and mode 6 blends by the alpha each vertex
carries (legacy:366451-366473). The sprite blit puts its alpha argument
into every vertex over white (legacy:196666, legacy:196730), so mode 6
is a plain alpha blend of the texture. The alpha is left times 510
while under half is left and (1 - left) times 510 otherwise, cut to a
whole number (legacy:197483).

| Built | Body | Mass alpha |
|------:|------|-----------:|
| 0% | not drawn | 0 |
| 25% | not drawn | 127 |
| 49% | not drawn | 249 |
| 50% | not drawn | 255 |
| 75% | opaque | 127 |
| 100% | opaque | no mass |

So the mass is solid at half built, and from there the body is already
under it and shows through as the mass fades. A unit's shadow while it
is built is under Shadows.

## Health bars and selection rings

A health bar (legacy:210837-210915) is drawn 10 px below the unit's
projected origin. It needs the `DisplayDamageBars` video setting and a
unit of the local player's, or an active cheat, and it is skipped when
health is below 1. Its index is health times 30 over maximum health,
from 0 to 30. The quad is 32 by 5 px, centred, from 2 px above to 3 px
below the anchor row. Its texture is a 32 by 155 ARGB1555 strip the
original builds itself (legacy:161507-161555), 31 steps of 3 rows each,
where step N lights N pixels from x = 1. Steps below 10 are red
(0xFC00), steps 10 to 19 yellow (0xFFE0) and steps from 20 up green
(0x83E0). The window's V origin is the index times 5.0/155.

A selection ring (legacy:210919-211085) is drawn while its unit is
selected. Its radius comes from the half extents of the model's
bounding box in x and z. It is 24 vertices, 12 line segments, drawn as
a line list (primitive type 2). Its base angle is (unit id + a free
running frame counter) times 0x4000 / 30. That counter is a different
global from the simulation tick, so the ring turns with the frame
counter. Two arrays step by plus or minus 0x2AAA with dash offsets of
minus or plus 0xCCC. The vertices are turned by the unit's own
orientation and projected through the same projector as the body. The
ring's colour follows health (legacy:210961-210969), from red to yellow
below half health and from yellow to green at half and above.

A group number (legacy:210775-210808) is one or two digits of white
text at screen X less 2, 6 or 10 and screen Y plus 14, with a variant
prefixed by `F`.

The drag-select box (legacy:125401-125540) is four quads over a 2 by 2
solid white texture, white with a varying alpha in the top byte, in
blend mode 6. The illegal-build marker is the `nobuild` and `nobuildx`
GAF.

## Fog of war overlay

The overlay has a hardware path (legacy:130167-130436) and a software
one (legacy:130522).

Explored state is one 16-bit word per fog cell in a global mask, one
bit per player slot. Visibility is kept per player: each player record
holds a byte array with one byte per fog cell, and the array's stride.
The fog grid is half the map's cell resolution, map width / 2 by map
height / 2, so one fog cell is 2 map cells, 32 world units, 32 px, the
same grid as the graphic tiles.

The `LosOn` game option gates the overlay. At 0 every explored cell
counts as visible. The state is initialised at legacy:167179-167244.
The explored mask is filled with 0x00 when `MappingOn` is set and with
0xFF when it is not, and the visibility arrays with 0 when `LosOn` is
set and 1 when it is not. What Line of Sight off does to sight is in
[fog.md](fog.md).

The lattice is aligned to cell centres. Take the camera position in fog
cells (camera / 32, rounded down) and the remainder (camera mod 32).
Then move each axis by half a cell: subtract 16 px from the remainder,
or if the remainder was below 16, add 16 to it and step the cell index
back by one. Y also takes a pre-bias of +64. Quad corners land on fog
cell centres, so each quad's four vertices sample the four cells around
it. Edges off the map are pushed out by -32.0.

| Corner state | Colour |
|---|---|
| Not explored | 0xFF000000, opaque black |
| Explored, and `LosOn` off or the cell in sight | 0x00000000, fully transparent |
| Explored but not in sight | 0x78000000, black at alpha 120 |

| Corners of a quad | What is drawn |
|---|---|
| All four visible | Nothing |
| All four unexplored | Blend mode 4, merged with its neighbours along the row into one wide quad |
| All four dim | Blend mode 5, merged along the row the same way |
| Mixed | Blend mode 6, a single 32 by 32 quad carrying the four colours at its vertices, the alpha ramp interpolated by the hardware |

Afterwards the blend mode goes back to 4. The draw goes through the
renderer's textured quad entry point with a 2 by 2 solid black ARGB1555
texture built at legacy:161386-161390, and each vertex carries its
position and a packed ARGB colour.

The overlay shows no grid because of the half resolution cells, the
lattice aligned to their centres, the colours interpolated between
vertices, only three alpha levels, and fully visible quads skipped
entirely.

The minimap's fog is per pixel and flat (legacy:208407-208418). An
unexplored pixel is 0, an explored pixel out of sight is remapped
through a shade table, and a pixel in sight is left as it is.

The engine departs on a hardware renderer, see D-053.

## Build sparkles

Every unit's render record carries a particle ring, made with the
record (legacy:198530-198576). The ring is sized from the model's box
in the unit type, not from the footprint. Its radius is half the box's
diagonal in the x and z plane, in whole pixels, from half extents taken
as max minus min over two (legacy:198540-198551). When bit 0x2000000 of
the unit's flag word is set, the radius is the larger of the two half
extents instead (legacy:198547-198555). Where that bit is set is not
established. The ring's height is the box's top minus its bottom
(legacy:198573-198576).

The ring holds as many particles as its radius has whole pixels
(legacy:198576, the count argument at legacy:201382-201419), except
that a unit whose `bmcode` is 1 holds a quarter of that
(legacy:198557-198560). The quarter applies to the count only, and the
radius the particles stand on stays the full half diagonal. Every
mobile unit ships with `bmcode` 1, so a raiser's ring is as wide as its
model but holds few. A debug speed setting divides the count further
and clamps it to at least 1 (legacy:198561-198571). Adding particles to
a full ring adds nothing: the request is cut to the free slots and
there are none (legacy:201434-201436). Nothing is replaced, and the
flow goes on because each particle ends at its own time.

Each 30 Hz frame in which the builder's construction step did work,
the site's ring is asked for two particles, one with flag 0 and then
one with flag 1 (legacy:12245-12262, again at legacy:12526-12540 and
legacy:13515-13526). The wrapper adds exactly one particle a call and
takes the side's build sparkle animation when none is named
(legacy:198992-199002). A raise asks the raiser's ring for one and the
body's ring for one each frame (legacy:13129-13133). The body's ring is
made from the feature's data, its height a byte in the feature type and
its count a quarter of its radius (legacy:128232-128239). Where the
body ring's radius comes from is not established, and the engine takes
the feature's footprint for it.

Each particle takes two random draws (legacy:201441-201455). The first
is its angle, a 15-bit random number doubled into a 16-bit turn, and
its place on the ring is the sine and cosine of it. The second is its
speed, the random number doubled and quartered into a 16.16 value and
offset by 2.0, so each particle moves between 2.0 and 4.0 pixels a
frame at a speed of its own.

The flag decides the direction (legacy:201355-201374). Flag 0 starts the
particle at the ring's height, the top of the model, moving down, with
a limit of 0. Flag 1 starts it on the ground moving up, with a limit of
the unit's own world height plus the ring's height. That limit adds an
absolute height to a relative one, so on ground 200 px high a riser
climbs 200 px past the top of the model, and at a hilltop site the
risers reach half the screen.

Each frame a particle is checked before it moves: a faller ends once
its height is below 1 and a riser once it is above its limit
(legacy:201340-201351). Otherwise it moves by its speed and steps its
animation. The animation runs at the GAF's own frame delay and loops
while the entry's loop flag is set (legacy:255756-255794), which it is
for every build sparkle in the data, so a particle plays its pictures
round and round until its height ends it. There is no fade and no
lifetime in frames. Every shipped sparkle sheet shows each picture for
two frames.

Particles with a negative z draw before the unit's model and the rest
after it, each at the unit's position plus its own, projected with the
usual half height (legacy:201487-201543).

The engine draws fewer sparkles, added and with a halo, see R-009.

## Script effects

A script asks for an effect with EMIT-SFX, opcode 0x1000f000, which
names a piece and an effect type, and the original hands the piece and
the type to its host (legacy:306486-306490). Three other script opcodes
go to host callbacks at legacy:306807-306842: 0x10082000 with two
popped arguments, which sets a unit value (legacy:306819), 0x10083000
with three and 0x10084000 with one. Which host call each of the other
0x1007x000 and 0x1008x000 opcodes makes is not established.

The host draws an effect only for a unit the local player can see, its
own or one in sight (legacy:223583, legacy:206797).

| Type | What the original draws |
|---|---|
| 0 and 1 | Starts an emitter of another kind (legacy:214913-214921, legacy:216166). No shipped script asks for either |
| 2 to 5 | Wakes and thrust, see Wakes |
| 0x101 | White smoke, the `bigsmoke` animation (legacy:161422-161424), a truecolor TAF of 10 pictures that ships in V3Rocket.hpi |
| 0x102 | Black smoke, `smoke01` in anims/smoke.gaf (legacy:161419-161421) |
| 0x103 | Bubbles, which call a routine that does nothing (legacy:223636-223640, legacy:214990), so nothing is drawn |
| 0x104, 0x105, 0x106 | A small, medium or large flame (legacy:223642-223650) |
| 0x107 | `purpledeath` from deathmagic.gaf, once where the piece stands |
| 0x108 | The `pillaroflight` TAF, once where the piece stands |
| 0x109 | Steam, the `steam` TAF, a puff like the smoke (legacy:161416-161417, legacy:215024-215028) |

0x107 and 0x108 are among the one-shot animations
(legacy:223651-223663, legacy:161490-161496, legacy:33163). Kirenna
plays 0x107 as she wades in or out, and the Pillar of Light plays
0x108. The engine draws nothing for types 0 and 1.

### Damaged building flames

A building's own script decides when it burns. The shipped scripts run
a `DamageFlameControl` and a `SmokeControl` loop that read the unit's
health, choose pieces and sizes, ask for an effect with EMIT-SFX and
sleep.

The flame sizes come from every TDF in gamedata/damageflames. Each
`smallflame`, `mediumflame` or `largeflame` section adds its `gaf` and
`anim` to that size's list (legacy:202524-202640), and a new flame
takes one from its size's list at random (legacy:202459-202461). The
shipped folder holds one file with one section for each size, all in
flames.gaf.

A unit keeps at most 40 flames (legacy:215116) and 10 puffs of smoke
(legacy:214686), and a request past that makes nothing. A puff rises
0x1fdb times 4 a frame and drifts 8 times the wind's parts a frame. It
holds its first picture 8 frames and each later one a random 4 to 7,
and it is gone on reaching a picture drawn at random from the third to
the second last (legacy:201847-201869, legacy:201941-201966). All of
this randomness is the C library's, never the simulation's.

The scripts decide the rest. ARAKEEP waits for its construction to
end, burns nowhere at 29 percent health and above, has one level of
flame at 28 and sixteen at 10, spreads them over eight pieces at up to
three levels a piece, emits every lit piece and sleeps 499 ms. Its
smoke starts below 66 percent and comes more often the lower its
health. The flame loop runs more than 200 instructions before it
sleeps, and the original runs it to its sleep in one go, since a thread
runs until it sleeps, waits or ends (legacy:306252-307002). Thirty-three
units carry a `SmokeControl`, mobile ones such as ARAWAR and VERTRANS
among them.

### Wakes

Types 2 to 5 are wakes and thrust, and type 2 is the wake 97 walkers
leave in water. Each request asks for one point in a list of up to 100
a unit (legacy:214923-214988). A point starts at the piece's first
vertex, or its second for types 4 and 5, put 3 px either way at random
on each axis, and moves half a pixel a frame toward the other vertex
(legacy:201754-201843). It shows six colours in turn, 16 frames each
for types 2 and 4 and 8 for types 3 and 5, and is gone after the sixth
or once it is over ground at or above the sea (legacy:201685-201712).
It is drawn as one pixel of its colour wherever it is on the screen
(legacy:215190-215228). The six colours run from a light teal to a dark
one (legacy:201707, legacy:201746).

## Projectiles

A weapon's look comes from its TDF keys, read at legacy:249668-250439:
`model`, `veteranmodel` and `veteranlevel`, the `weaponart` GAF, the
shadow art, the explosion classes, `smokedelay`, `shakemagnitude` and
`shakeduration`, `soundstart`, `soundhit`, `soundhitclass`,
`soundwater`, `spinroll`, `RockUnit` and `AimTolerance`. Its flags, by
name, are `dropped`, `noradar`, `startsmoke`, `endsmoke`,
`firestarter`, `soundtrigger`, `tracks`, `unitsonly`, `groundbounce`,
`waterweapon`, `toairweapon`, `noairweapon`, `smoketrail`,
`commandfire`, `nimbus` (a magic glow) and `dontleadtargets`. Every
weapon block sits inline in its unit's FBI. What the other weapon keys
do is in [2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

A shot's art is settled once, when the weapon is read. `model` gives a
3DO, otherwise `weaponart` gives a GAF sequence, otherwise the
lightning and flame Line of Sight subtypes get the held beam
(legacy:250074, legacy:250088, legacy:249761). 28 shipped weapons carry
a model and 39 a weaponart. One names its model with the extension,
`model = zonrock.3do`.

At spawn (legacy:249423-249503) a shot takes its player's team colour
index. Shots draw through the unit renderer, and a model shot draws at
its own heading, pitch and roll in that team colour (legacy:246780,
legacy:249446). Recoil invokes the script's `RockUnit` with minus the
cosine of the heading difference times the amount and minus its sine
times the amount (legacy:249494). Aim scatter is a uniform draw over
plus or minus the accuracy from the original's 15-bit random
generator, whose range is 0x8000 (legacy:249399).

Heading and pitch follow the velocity (legacy:250646), unless the
weapon carries `spinpitch`, `spinheading` or `spinroll`, which are
added each step instead (legacy:250016-250020, legacy:246661).

A `type = Ballistic` shot arcs. Its launch pitch solves
`tan(theta) = k -/+ sqrt(k^2 - 2*rise*k/run - 1)` with
`k = v^2 / (g * gravityadjustment * run)`, taking the low arc unless
the weapon has `lobpreferred` (legacy:246535, legacy:246477). Gravity
is 0x1fdb in 16.16 a frame (legacy:224904). Every shipped siege range
sits just inside the reach, v squared over g, that its own
`gravityadjustment` allows, ARATRE's 2700 against 2811 and VERMORT's
1450 against 1590, which pins both that constant and `weaponvelocity`
as world pixels a second. A dropped shot (`subtype = Dropped`) is its
own behaviour, which takes its run across the ground from its fall
time (legacy:249743), see "Dropped shots" in
[2026-09-04-combat-mechanics.md](2026-09-04-combat-mechanics.md).

An impact plays the weapon's `explosionclass` from
gamedata/explosions/explosions.tdf, one variant picked per hit
(legacy:250135, legacy:245025). `explosionclass` fills the weapon's
first explosion slot, and `waterexplosionclass` and
`lavaexplosionclass` its second (legacy:250135-250150), which plays
when the shot bursts on water or lava (legacy:250146). The engine
departs in how `shakemagnitude` and `shakeduration` shake the view, see
R-007.

A shot fired by a unit ranked at or past the weapon's `veteranlevel`
uses its `veteranmodel` in place of `model` (legacy:250079,
legacy:246620). The swap is on the model only, never on `weaponart`,
and it is made at every spawn, so a unit that ranks up in battle
changes shot on its next one. `veteranlevel` defaults to 10, the rank
cap, so a weapon that names no `veteranmodel` never swaps. Five shipped
weapons author the pair: VERKNIGH, ARABUILD, ARASPY, ZONLORD and
ZONTER.

### Flames, rings and storms

Three families of spell draw from fields of their own.

A flame is a Line of Sight weapon with `subtype = fire`, and the data
also names `bluefire` and `dieselflame`. Its block carries only
`emittime` and `range`, no art and no colours. The original's flame
reads `emittime` and nothing else and emits one particle a frame from
the muzzle toward the strike point, its velocity jittered a tenth
either way. Every dragon, the Taros knight, mage and spout and the Zhon
drake breathe this way, eight units in all. The six that fly hold their
`QueryWeapon` piece, the head, 158 to 219 px up. TARSPOUT's sits 7 px
up, and TARMAGE's `QueryWeapon` names its staff piece, whose origin is
authored at the mage's feet. The engine starts a flame no lower than
12 px over the ground, which lifts those two.

A Remote Effect spell with `radiusart0` to `radiusart2` lays rings:
`ringcount` rings, one every `ringdelay` seconds after `builduptime`,
each made of `spritecount` copies of that ring's radiusart sprite
spreading out to `areaofeffect` over `ringduration`. Earthen Wave,
Earthquake, Ring and Wave of Fire, Tsunami, Water Blast, Wind Wave,
Shockring, Death Aura and Area Mind Control are all this one mechanism
with their own art: `ring_fx_red`, `ring_fx_white`, `ring_fx_blue`,
`ring_fx_green`, `ring_fx_purple` and `TsunamiExplode`.

A Remote Effect spell with `subtype = hailstorm` rains its `weaponart`
at `particlespersecond` for `duration` seconds over `areaofeffect`,
each drop bursting with the weapon's `explosionclass`. Hail Shower,
Fire Storm and Ice Storm are this one, with `iceballspin` or `meteor`.

The engine departs in how it draws storms and lightning, see D-054,
D-055 and D-056.

## Effect art

The original keeps global GAF handles (legacy:161407-161505) for
`bigsmoke`, `smoke` and `smoke01`, `flame`, `radiated`, `radiance02`,
`radiance03`, `radiance04`, `nobuild`, `nobuildx`, `mindspin`,
`transportfx`, `transswirl`, `deathmagic`, `purpledeath`,
`pillaroflight`, `BlueFire`, `DieselFlame` and `steam`. The same table
holds the health bar strip, the 2 by 2 black and white textures and the
`cursors.gaf` frames: attack, airstrike, toofar, capture, defend,
repair, patrol, pickup, teleport, revive, reclamate, load, unload,
move, select, findsite, red, grn, normal, hourglass and pathicon.

Whether the TDFs read at legacy:161560-161600 are the explosion
classes, from `gamedata\effects\*.tdf`, is not settled, since the
classes weapons name ship in gamedata/explosions/explosions.tdf.

## Death and corpses

When a unit dies (legacy:227088-227181) its severity is the overkill
percentage, minus the health times 100 over `maxdamage` since health is
negative at death, plus the unit's cached previous health percentage,
halved and clamped to 1 to 100. The script's `Killed(severity,
corpsetype)` runs unless the cause of death forces a corpse type
directly. Cause 9 forces corpse 1, and causes 0xE, 0xF, 5, 6, 7, 0xA,
0xB, 0 and 8 have variants of their own. A unit still under
construction forces corpse 0, so a nanoframe leaves no corpse
(legacy:197149). The result is packed into a byte, the cause in the
high nibble and the corpse type in the low one, sent as an 11-byte
packet, broadcast and executed locally. Death scripts branch on the
severity, see
[2026-09-09-cob-entry-points.md](2026-09-09-cob-entry-points.md).

Next (legacy:227185-227263), when the severity is above 0 and the
script has a `Dying` entry, `Dying` runs. Otherwise the unit is
destroyed at once (legacy:227267). Destruction first adds a line of
sight entry at the place of death for the player's own units, lasting
60 of the original's ticks, two seconds at its 30 Hz.

The corpse (legacy:227395-227456) is the feature named in the
definition's main corpse field, with a heap corpse field and an
alternate corpse field beside it. The decay chain is walked one level
at a time through each feature type's next stage. A corpse placed
above sea level takes a burn animation from its feature type. One
below sea level sinks instead, at 0xFFFFD334 in 16.16, about -0.175
world units an original tick, unless its type carries flag bit
0x1000000, whose FBI key is not established. The engine departs here,
see D-004. Where a corpse is placed, and its rot, the sweep and
raising, are in [features.md](features.md).

A corpse is drawn as a full 3D model through the unit renderer. The
feature draw (legacy:211200-211226) copies the corpse's record into a
scratch render object and draws it like a unit.

The magic death (legacy:211228-211323), driven by a field on the corpse
record, renders the frame to a temporary surface named `magic_death`,
forces every pixel that is not the key to 0xFF, and draws the result as
a quad coloured white with the fade as its alpha, in blend mode 6 and
then 8. It shows as a white silhouette added over the scene, fading
out.

A static feature (legacy:211147-211188) has a term of minus the sum of
its 4 corner heights over 8 in its screen Y. Its shadow art draws only
with `DrawShadows` on, and its body art always draws. A flag bit
chooses animated playback or a fixed frame 0. Each feature type
carries a height, its next decay stage, a decay animation, flags, a
footprint in X and Z, body art, shadow art and animation state.

## Options the draw reads

Two option blocks hang off the input and options state. Only the
settings' names and meanings matter here.

| Video setting | Meaning | Read at |
|---|---|---|
| InGameScreenW, InGameScreenH | The in-game screen's width and height, in that order | |
| DoShading | The master lighting switch | |
| DisplayDamageBars | Health bars over units | legacy:210837 |
| 2x AA | The 2x supersampled body pass | legacy:197621 |
| DrawShadows | Unit, feature and weapon shadows | legacy:197182, legacy:197428 |
| Per-polygon shading | The flat shading path, apart from DoShading | legacy:197710 |
| D3D present | The hardware present path | |
| Shadow LOD source | Which LOD level the shadow silhouette comes from | |
| ShadowScale | The shadow raster divisor, see Shadows | |

| Game setting | Meaning | Read at |
|---|---|---|
| LosOn | Line of sight on, see Fog of war overlay | legacy:167179, legacy:130167 |
| MappingOn | The map starts unexplored, see Fog of war overlay | legacy:167179 |
| CanDropPlayer | A multiplayer host may drop a player | |
| DisableMapScript | Suppresses the mission script | |

The original keeps as globals the map's size in cells, the world's
size, the viewport's edges and size, the camera position, the sea
level, the explored mask, the graphic tile map, the map cell grid, the
feature types, the corpse instances, the scene's Z row buckets, the
frame and tick counters, the units and the player records. Each map
cell carries the 16-bit id of the unit on it, its terrain height, a
feature id, a corpse id and two flag bytes.

## The 3D view

This section describes OpenKingdoms and not the original. V switches a
battle between the classic view and the 3D view, which draws the same
world under a free camera. Entering it puts "3D view, experimental.
Press V for the classic view." on the message line for six seconds of
wall time. Where no OpenGL driver is available, V prints a line and
does nothing.

The classic bindings all still work. The 3D view adds keys the original
leaves unbound in its keys file, where LOWER_Q, LOWER_E, LOWER_R,
LOWER_F, LOWER_V, LOWER_X and LOWER_Z are empty and Home is not bound.

| Key | Does |
|---|---|
| V | Switches between the classic view and the 3D view |
| Q and E | Orbit the camera left and right around its target |
| R and F | Tilt the camera up and down |
| Z and X | Zoom in and out |
| Mouse wheel | Zoom |
| Middle button drag | Orbit and tilt |
| Home | Back to the classic angle and height over the same spot |
| WASD, arrows, screen edges | Scroll, in the camera's own directions |

While a building's ghost is armed, R turns it instead, see D-022.

The camera stays over the map, at a tilt between 12 and 89 degrees, a
distance between 48 and 6000 px and at least 20 px above the ground
under the eye. Scrolling in 3D moves the classic camera with it, so the
minimap's rectangle and the return to the classic view land on the same
ground, and a minimap jump moves the 3D camera. A left click becomes a
ray, the ray meets the terrain, and that ground point takes the same
order path as a click in the classic view, so selecting, moving and
attacking work. A drag box takes its corners on the ground, so it is
approximate. Going to the 3D view and back leaves the classic view as
it was, over the same ground with the same selection.

To match the classic view it draws these.

- The terrain as a mesh at the height map's resolution, one vertex per
  16 px cell, each 32 px block sampling the same square of the same map
  chunk the classic view samples.
- The water as a translucent plane at the map's sea level.
- One directional light, on the terrain by its slope and on models by
  their face normals, with ground and models more than 2500 px from the
  eye fading a little toward the sky colour.
- Every unit and building as its shipped 3DO model, posed by the same
  COB piece state as in the classic view, in team colour from the same
  per-colour textures and side palette.
- Features with a model, such as corpses, rubble and some rocks, as
  models. Sprite features such as trees, most rocks and plants stand up
  as billboards facing the camera, a little taller than their sprite
  since it was painted for the classic tilt. Mana pads lie flat on the
  ground.
- Fog by the classic view's tests: unexplored ground black, ground out
  of sight dimmed and a unit the local player cannot see not drawn.
- The selection as a ring on the ground under each selected unit, green
  for the local player's and red for anyone else's.
- The shots and effects the classic view draws, behind the same
  visibility gate. A model shot is its model at its heading, pitch and
  roll. A sprite shot or an impact sprite is a billboard, its frame
  chosen as the classic view chooses it. A beam is a ribbon from muzzle
  to target in three widths, the outer ones fainter, jittered afresh
  each frame as the classic rays flicker. A shot with no art is the
  classic view's bright dot.
- A wake point as a speck 1.5 px across facing the camera, where the
  classic view draws one pixel.
- A unit under construction with its body by the classic rule and its
  mass as the model drawn flat in the mean of its side's 128 build
  colours at the classic alpha, after a pass that writes only depth so
  that only the front surface blends.
- The whole HUD over the frame, unchanged.

| Switch | Does |
|---|---|
| `--renderer <name>` | The SDL render driver, opengl by default |
| `--view3d` | Opens every battle in the 3D view |
| `--cam3d x,z,yaw,pitch,dist` | Where the 3D camera starts |
| `--reveal` | Plays a `--perf-probe` scenario with the map revealed |
| `--screenshot <bmp>` | Saves the frame at `--screenshot-tick` and quits |

`--skip-logo --perf-probe build8 --reveal --view3d` plays a human seat
and seven computer players on Ladron's Tarn with the map revealed, in
the 3D view.
