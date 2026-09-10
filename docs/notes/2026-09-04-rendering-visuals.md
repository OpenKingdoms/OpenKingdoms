# Behaviour note: legacy rendering / visuals (2026-09-04)

Derived from behavioural analysis of the retail binary. `:NNNNN` citations are
evidence pointers into a private reference that is not distributed. They mark where a claim can be re-checked by whoever holds it,
and nothing here reproduces that file's expression.

In the render and world code the reference tooling's symbol names are almost entirely
wrong. Every name in this note is **our** descriptive label, assigned from what
the routine actually does, not a symbol recovered from the binary. Legacy
in-memory struct offsets have been replaced by field names throughout. We read
our own structures, so the offsets carry no implementation value. Where a field
has no recoverable name, it is described by role and any bit value that is the
only handle on it is kept and flagged.

Call graph (our names):

| Line | What it does |
|---|---|
| :210163 | Frame_DrawAll (top-level frame draw) |
| :225667 | Terrain_DrawTiles (tiles + animated water) |
| :210467 | Scene_DrawObjects (features/units/overlays, row-bucketed) |
| :211089 | Feature_Draw (incl. corpses + magic-death) |
| :197117 | Unit_Render (per-unit dispatcher: shadow, LOD, blend) |
| :196843 | Unit_ComputeScreenBounds (projection + LOD divide) |
| :196958 | Unit_RasterToSprite (water tint/clip, nanoframe copy) |
| :197561 | Model_RasterizePrims (known-good projector/cull/prim-order) |
| :130167 | Fog_DrawOverlay (hardware path, software twin :130522) |
| :209631 | Sprite_Blit (universal 2D GAF blitter) |
| :210837 | HealthBar_Draw · :210919 SelectionRing_Draw |
| :207443-208600 | minimap/radar (RADAR_PICTURE / MAPPED / FINAL surfaces) |

Frame order (:210228-210273): terrain tiles → scene objects → minimap → HUD.

Within the scene-object pass, every feature shorter than 10 world units is
drawn first. Taller features are deferred and bucketed by Z-row. Then, row by
row, that row's ground units draw, followed by that row's tall features. Air
units draw last, over everything. Health bars and group digits follow, and the
fog overlay is a separate pass after those.

Row index = (world Z ÷ 16) − (camera Y ÷ 16) + 32, which is the object's
map-cell row minus the camera's, biased by 32 rows to keep the index positive
(:210555). Three parallel bucket lists are maintained side by side.
There is no depth sort, only Z-row bucketing.

## TA_SCALE resolved: 1 world unit = 1 screen pixel

- screen X = world X − camera X, and screen Y = (world Z − world Y ÷ 2) −
  camera Y. No multiply anywhere in the path (:197195, :210761, :225772).
- Model vertices are 16.16 fixed-point. The integer part goes straight to a
  screen coordinate, truncated to 16 bits, with no scale factor
  (:196879-196888, and the rasteriser itself at :197683-197689).
- **1 map cell = 16 world units = 16 px** (world position = cell index × 16,
  :226350). **1 graphic tile = 32 px = 2×2 cells** (:225797). Simulation
  positions are 16.16 fixed-point in that same pixel space.
- LOD divides only the sprite raster (by 1, 2 or 4, :196889-196892) and then
  magnifies again on blit.
- **One model unit is therefore one world pixel: the scale is exactly
  1/65536, not a free parameter.** The shipped data confirms it. ARAWALL's
  model spans 32.0 units on its 2×2-cell footprint, ARANGATE 224.0 on its
  14×4, ARAAT 48.0 on its 3×3. Every one is footprintX × 16 px to the pixel,
  so walls and gates tile only at this scale. Our old hand-tuned 0.000021
  drew every model 37% oversized.

## Unit draw order is a per-pixel height buffer, not pure painter's

The rasteriser walks the render object's node table **backwards**, from the
last node to node 0 (:197658-197661 init, :197944-197946 step), with prims
forward inside a node and the node's selection primitive skipped
(:197794-197797). Backface rule: cross ≥ 0 draws (:197804-197807).

That order alone does not decide what is visible. Each transformed vertex
carries a **third component: a base plus the model-space Y**, base 0x32, or
0x7D when the definition floats on water (:197697). The polygon filler
interpolates it across the span and depth-tests it against a parallel byte
buffer, writing the pixel only when the incoming key is **greater** than the
key already there (:265317-265341 opaque path, :265360-265372 remapped path,
which uses ≥). The same buffer is what the underwater tint reads later.

The key reads as "height" but under sy = −z − (y ÷ 2) it is a depth test:
two surfaces landing on one pixel satisfy y = −2(sy + z), so the taller
sample is the nearer one. That is what keeps a monarch's head in front of his
torso, a torso in front of the cape hanging behind it, and a keep's tower
solid instead of a see-through shell of back faces. The node order gets none of
those right on its own, and a keep is a single 120-prim node anyway.

Our renderer has no per-pixel buffer, so it resolves the same comparison per
triangle: order by the triangle's tallest vertex key, break ties with the
reverse-node authored order. Quantising the key to whole units, as the byte
buffer does, keeps coplanar pieces tied so the authored order still decides
them.

## Water (Terrain_DrawTiles :225667-226016)

**Tile map.** One 4-byte record per graphic tile: a 16-bit atlas page index,
then the tile's X and Y within that page as bytes. Tiles are half the map's
cell resolution, so the record index is
`tileRow × (mapWidthInCells ÷ 2) + tileColumn`.

**Atlas pages.** A page table gives each page its pixel width, pixel height and
texture handle. UVs come from the tile's X and Y divided by the page size
measured in 32-pixel tiles (page width ÷ 32, likewise height).

**Screen placement.** `x = viewportLeft − (cameraX mod 32) + column × 32`.
Terrain is drawn **flat**, with no height displacement of tiles.

**Land or water (:225818).** Sample the four corner cells' terrain heights.
Depth at a corner = sea level (a global byte) − that corner's height. If all
four depths are below 1 the tile is land: one quad, colour 0xFFFFFFFF.

Otherwise the tile is water and draws in two passes:

1. The terrain tile, modulated by **0xFFC0C0C0**, the seabed at 75%.
2. A second quad in **blend mode 2**, carrying four animated per-corner
   colours.

Per corner, the animated colour is built like this:

1. Depth ≤ 0 → the corner is shoreline and takes the fixed colour
   **0xFF404040**. There is no separate shoreline term. The gradient at the
   shore is produced purely by interpolating this fixed dark corner against the
   animated ones.
2. Otherwise amplitude `d` = depth × (a global water-amplitude scalar) ÷ sea
   level. That scalar's value has not been read out of the binary's data
   section yet. See Open items.
3. Phase = (a 171-entry (0xAB) pseudo-random table, indexed by the corner index
   modulo 171) × (a global phase-rate scalar, also unread) × elapsed seconds.
   Elapsed seconds is **wall-clock milliseconds × 0.001, not the simulation
   tick**. The shimmer keeps moving when the game is paused or slowed.
4. Two sine terms, each scaled by `d`: sine of (corner index + phase), and sine
   of (a second 171-entry table's entry at the same index + phase). The
   reference tooling labels that second table a sin/cos lookup. Here it is used
   as a per-index phase offset.
5. Sum the two terms, add 64, convert to integer and take the low 8 bits. It
   **wraps at 256, it does not clamp**. The result is a grey level `c`, and the
   corner colour is opaque grey: alpha 0xFF, R = G = B = `c`.

Corner phases are shared between neighbouring tiles, so the shimmer is
continuous across tile boundaries with no seams.

The gate is a global water-enable byte, plus the renderer answering yes to a 3D
capability query (:225782). The software fallback (:225942) has **no water at
all**, just a plain tile blit with horizontal run merging. There is no palette
cycling.

## Fog of war overlay (:130167-130436 hardware, :130522 software): THE grid-artifact fix

**State.** Explored state is one 16-bit word per fog cell in a global mask, one
bit per player slot. Visibility is per player: each player record holds a
pointer to a byte array with one byte per fog cell, plus that array's stride.
**The fog grid is half the map's cell resolution, mapWidth/2 × mapHeight/2, so
one fog cell = 2 map cells = 32 world units = 32 px**, the same grid as the
graphic tiles.

The `LosOn` game option gates it: when it is 0, every explored cell counts as
visible. Initialisation happens in LOS_UpdateAll (:167179-167244). The
explored mask is filled with 0x00 when `MappingOn` is set and 0xFF when it is
not. The per-player visibility arrays are filled with 0 when `LosOn` is set and
1 when it is not.

**Lattice alignment (the crux).** Take the camera position in fog cells (floor
of camera ÷ 32) together with the remainder (camera mod 32). Then bias each
axis by half a cell: subtract 16 px from the remainder, and if the remainder
was already below 16, add 16 to it instead and step the cell index back by one.
Y additionally takes a +64 pre-bias. The effect is that **quad corners land on
fog-cell centres, so each quad's four vertices sample the four surrounding
cells.** Off-map edges are pushed out by −32.0.

Per-corner colours:

| Corner state | Colour |
|---|---|
| Not explored | 0xFF000000, opaque black |
| Explored, and either `LosOn` is off or the cell is currently visible | 0x00000000, fully transparent |
| Explored but not currently visible | **0x78000000**, black at alpha 120 |

Emission rules, per quad:

| Corners | Emitted |
|---|---|
| All four visible | **Nothing is drawn** |
| All four unexplored | Blend mode 4, merged horizontally with its neighbours into one wide quad |
| All four dim | Blend mode 5, same horizontal run merging |
| Mixed | **Blend mode 6, a single 32×32 quad carrying the four per-vertex colours**. The hardware Gouraud-interpolates the alpha ramp |

Afterwards blend mode is restored to 4. The draw goes through the renderer's
textured-quad entry point with a 2×2 solid-black ARGB1555 texture built at
:161386-1390. Each vertex carries its position and a packed ARGB colour.

The legacy has no grid artifacts because of half-resolution cells, a
centre-aligned lattice, per-vertex Gouraud interpolation, only three alpha
levels, and fully-visible quads skipped entirely.

Minimap fog, by contrast, **is** per-pixel and flat (:208407-208418):
unexplored → 0, explored but not visible → the pixel remapped through a shade
lookup table, visible → the pixel unchanged. The surfaces involved are named
RADAR_PICTURE, MAPPED and FINAL.

## Unit sprite pipeline

**Projection (:196879-196892).** Body: screen X = X, screen Y = −Z − (Y ÷ 2)
(known-good). A skew variant exists: screen X = X + (Y ÷ 4), screen Y =
−Z − (Y ÷ 4). Either is then divided by the LOD scale. Bounds carry ±2 px.

**Shadows** (:197182-197196, :197428-197431). Gates, all required:

- the `DrawShadows` video setting is on
- the render object's suppress-shadow flag is clear
- the unit definition does **not** carry flag bit 0x2000000. The name is
  unrecovered and the bit value is the only handle we have on it. It is the
  *same* bit that enables the 2× supersample path below, so the units that get
  AA are exactly the units that get no shadow
- build-time-remaining is 0, i.e. the unit is finished
- a global shadow-enable byte is set, toggled from the console (:38584)

Anchor: shadow screen X = unit X − camera X **+ 5 px**. Shadow screen Y =
(unit Z − camera Y) − (groundHeight ÷ 2), where groundHeight is the larger of
the bilinear terrain height and the sea level, so **the shadow sits on the
ground, not under the unit's own elevation.** Bilinear terrain height is
computed at :226234 from the per-cell heights with 1/16 sub-cell weights.

The `ShadowScale` video setting selects the raster divisor: 0 = auto by
frame-time budget, otherwise 1, 2 or 3 giving divisors 1, 2 and 4.

The shadow silhouette is built from a **different subset of pieces** than the
body: piece selection tests bit 1 of the piece flags against the pass's version
selector (:197663). Feature shadows use the feature type's shadow image. Weapon
shadows use the weapon's `shadowgaf`/`shadowart`. FBI `noshadow` is parsed at
:163001, `shadtrans` at :127343.

**Underwater tint / clip** (:197018-197036). Take the unit's elevation. The
base offset is 0x7D when the definition carries the floats-on-water flag and
0x32 when it does not. If elevation is below sea level, the cut level is
`seaLevel + base − elevation`, and every rasterized pixel whose height key is
at or below that level, and which is not the transparency key, is remapped
through BLUE_TABLE (:256326-256345). A floating unit at or above sea level
instead gets a **hard clip**: pixels below level 0x7D are set to the
transparency key (:256936).

This works because the rasterizer writes a per-pixel height key into a parallel
byte buffer alongside the sprite. The key is interpolated from a third vertex
component carrying the model base plus world Y (:197691-197696). BLUE_TABLE is
a 256-byte table on the game instance, built at :271094. It is the `.blu`
underwater remap.

**Nanoframe.** The unit's build-time-remaining runs from 1.0 when placed to 0.0
when complete. **At 0.5 or above the unit is not drawn at all**. Below 0.5 it
draws in blend mode 6 with the alpha ramping 0 → 255. At exactly 0.0 it
switches to blend mode 5, opaque (:197310-197322). A unit killed while still a
nanoframe leaves no corpse (:197149).

**2× supersample AA** (:197621-197657). Gated by the 2×AA video setting and by
the same definition flag 0x2000000 that suppresses shadows. Body pass only:
coordinates are doubled, the raster is prefilled with palette index 9, and the
result is decimated 2:1 on the way out (:197057).

**Stone / freeze recolour** (:197875-197886). Every primitive is forced onto
the side's `stonegaf` or `frozengaf` texture (sidedata parsed :164740-164756).

**Flat shading** (:197710-197791). A per-primitive normal from the cross
product of two edges, used only when the per-polygon shading video setting is
on **and** bit 2 of the piece flags is set. Light level = the dot product
scaled by a constant, converted to integer, plus the ambient level.
**When shading is off, every face uses a fixed light level of 0xF.**

## Team colour: pre-baked per-colour texture variants (NOT palette indices 16..23)

A texture object holds an array of per-colour texture handles, and the
renderer's set-ambient entry point (:255123) selects the variant for a given
colour index. Selection per primitive (:197904-197911):

| Primitive flags | Texture used |
|---|---|
| Team-colour bit set | The per-colour variant for the owner's team colour |
| Animated bit set, team-colour bit clear | The animated frame |
| Both clear | The primitive's plain texture |

The team colour index is resolved by walking: the unit's owner index → that
player's record → the player's side record → the side record's team-colour
index. The unit's owner index is stamped from the player record at :226508, and
the player record also carries its side index.

The same per-colour selection is used for minimap blips (:208505) and for
selection markers (:209953, out of the side-data table).

Side-data records (:164740-164817) carry, by name: `FogColor` (**BGR order**),
`logogaf`, `stonegaf`, `frozengaf`, `buildsparklygaf`, `resurrectsparklygaf`,
`nimbus`.

## Health bars & selection rings

**Health bar** (:210837-210915), drawn 10 px below the unit's projected origin.
Gated on the `DisplayDamageBars` video setting and on the unit being the local
player's (or a cheat being active). It is skipped when health is below 1.

Index = health × 30 ÷ max health, giving 0..30. The quad is 32×5 px, centred,
spanning 2 px above to 3 px below the anchor row. The texture is a
procedurally-built 32×155 ARGB1555 strip (:161507-161555): 31 steps of 3 rows
each, where step N lights N pixels starting at x = 1. Colours: red 0xFC00 for
N below 10, yellow 0xFFE0 for 10..19, green 0x83E0 for N of 20 and above. The
UV window's V origin is index × (5.0 ÷ 155).

**Selection ring** (:210919-211085), drawn when the unit is selected. Radius
comes from the model's XZ bounding-box half-extents. **24 vertices = 12 line
segments** (primitive type 2, line list). Base angle = (unit id + a
free-running frame counter) × 0x4000 ÷ 30. That counter is a different global
from the simulation tick, so **the ring rotates with the frame counter**. Two
arrays step by ±0x2AAA with ∓0xCCC dash offsets. Vertices are rotated by the
unit's own orientation and projected through the same projector as the body.

Ring colour is a health gradient (:210961-210969): below half health it ramps
red → yellow, at or above half health yellow → green.

**Group number** (:210775-210808): one or two digits of white text at
(screenX − 2 / −6 / −10, screenY + 14), with an `F`-prefixed variant.

**Drag-select box** (:125401-125540): four quads over a 2×2 solid-white
texture, coloured white with a varying alpha in the top byte, blend mode 6.
The illegal-build marker uses the `nobuild`/`nobuildx` GAF.

## Corpses & death

**Unit_Kill (:227088-227181).** Severity = the overkill percentage
(−health × 100 ÷ `maxdamage`, since health is negative at death) plus the
unit's cached previous health percentage, halved, then clamped to 1..100. The
COB `Killed(severity, &corpsetype)` script runs unless the death cause forces a
corpse type directly. Cause 9 forces corpse 1, and causes 0xE, 0xF, 5, 6, 7,
0xA, 0xB, 0 and 8 have their own variants. A unit still under construction
forces corpse 0.

The result is packed into a byte as cause in the high nibble and corpse type in
the low nibble, sent as an 11-byte packet, broadcast, and executed locally.

**Unit_ApplyDeath (:227185-227263).** If severity is above 0 and the script has
a `Dying` entry, `Dying` runs. Otherwise the unit is destroyed immediately
(:227267). Destruction first adds an LOS entry with a **60-tick lifetime at the
death site** for the player's own units.

**Corpse spawn (:227395-227456).** The feature id comes from the unit
definition's primary corpse field, with separate heap-corpse and alternate
corpse fields available. The decay chain is walked one level at a time through
each feature type's next-stage field. The cell is the definition's corpse cell
offsets plus the unit's own cell position. Above sea level the corpse is placed
in the map grid carrying the unit's position, **rotation and team colour**,
plus a burn animation from the feature type. Below sea level it sinks instead,
at a rate of 0xFFFFD334 in 16.16 (≈ −0.175 world units per tick), unless the
type carries flag bit 0x1000000 (name unrecovered, and the bit value is the
only handle on it).

Corpses are rendered as full 3D models through the unit renderer.
Feature_Draw (:211200-211226) copies the corpse instance record into a scratch
render object and draws it like a unit.

**Magic-death dissolve** (:211228-211323), driven by a field on the corpse
record: the frame is rendered to a temporary surface named `magic_death`, every
non-key pixel is forced to 0xFF, and the result is drawn as a quad coloured
`(fade << 24) | 0xFFFFFF` in blend mode 6 then 8, an additive white silhouette
fading out.

**Static feature draw** (:211147-211188): screen Y includes a
−(sum of the 4 corner heights ÷ 8) term. The shadow art is gated on
`DrawShadows`. The body art always draws. A flag bit selects animated playback
versus a fixed frame 0.

Each feature type carries: height, next decay stage, decay animation, flags,
footprint X and Z, body art, shadow art, and animation state.

## SFX / particles / EMIT

COB dispatch (:306807-306842) routes three opcodes to host callbacks:
0x10082000 to a callback taking 2 popped arguments, 0x10083000 to one taking 3,
and 0x10084000 to one taking 1.

**DISCREPANCY.** This dig read 0x10082000 as `EMIT_SFX(sfxType, piece)`. Our
VM (cited at :306486 / :306819) has `EMIT_SFX` = 0x1000f000 and 0x10082000 =
`SET_UNIT_VALUE`. The sound dig read 0x10082000 as `EXPLODE`. The VM is
corpus-validated and behaviourally verified (lodestone activation), so trust
the VM. Re-verify the 0x1007x000 / 0x1008x000 host rows in a focused pass
before building the SFX host.

The engine object's vtable is data-only in the reference, so the **concrete
EmitSfx body is not recoverable**. It needs a runtime trace or effects-TDF
schema work.

**Effect asset table** (Visuals_Init :161407-161505) holds global GAF handles
for: `bigsmoke`, `smoke`/`smoke01`, `flame`, `radiated`, `radiance02`,
`radiance03`, `radiance04`, `nobuild`, `nobuildx`, `mindspin`, `transportfx`,
`transswirl`, `deathmagic`, `purpledeath`, `pillaroflight`, `BlueFire`,
`DieselFlame`, `steam`. It also holds the health-bar strip, the 2×2 black and
2×2 white textures, and the `cursors.gaf` frames: attack, airstrike, toofar,
capture, defend, repair, patrol, pickup, teleport, revive, reclamate, load,
unload, move, select, findsite, red, grn, normal, hourglass, pathicon.

Explosion classes come from `gamedata\effects\*.tdf` (:161560-161600). Weapon
binding (:250135-250150): `explosionclass` binds to the weapon's primary
explosion slot, `waterexplosionclass` and `lavaexplosionclass` to its secondary.

## Projectiles

Weapon visual fields, by TDF key (Weapon_LoadFromTDF :249668-250439): model ·
`veteranmodel` + `veteranlevel` · `weaponart` GAF · shadow art ·
explosion classes · `smokedelay` · `shakemagnitude` + `shakeduration` ·
`soundstart`, `soundhit`, `soundhitclass`, `soundwater` · `spinroll` ·
`RockUnit` · `AimTolerance`.

Weapon flags, by name: dropped, `noradar`, `startsmoke`, `endsmoke`,
`firestarter`, `soundtrigger`, `tracks`, `unitsonly`, `groundbounce`,
`waterweapon`, `toairweapon`, `noairweapon`, **`smoketrail`**, `commandfire`,
**`nimbus`** (magic glow), `dontleadtargets`.

Spawn (:249423-249503): the projectile inherits the owner's team colour index.
Recoil is applied by invoking the script's `RockUnit` entry with
−cos(Δheading) × amount and −sin(Δheading) × amount (:249494). Aim scatter is a
uniform draw on ±accuracy from the 15-bit engine RNG, range 0x8000 (:249399). Projectiles are
drawn through the normal unit renderer, with the `spinroll` roll term applied.
Heading and pitch come from the velocity vector (Weapon_SetAngles :250646).

## Palette LUTs (on the game instance)

| Table | Size | Purpose |
|---|---|---|
| ALPHA_TABLE | 0x10000 | 256×256 blend pair → nearest palette index (:271115) |
| SHADE_TABLE | 0x2000 | 32×256 |
| LIGHT_TABLE | 0x2000 | - |
| GRAY_TABLE | 0x100 | - |
| **BLUE_TABLE** | 0x100 | **the `.blu` underwater remap** |

Builders at :271010-271101. Helpers at :271173, :271203, :271247.

## Config

Two option blocks hang off the input/options state object. We read our own
config, so only the setting names and meanings matter. The legacy field
positions are not reproduced here.

**Video options block:**

| Setting | Meaning | Consumed at |
|---|---|---|
| InGameScreenW / InGameScreenH | In-game screen width and height, in that order | - |
| DoShading | Master lighting toggle | - |
| DisplayDamageBars | Draws the health bar over units | :210837 |
| 2×AA | Enables the 2× supersampled body pass | :197621 |
| **DrawShadows** | Enables unit, feature and weapon shadows | :197182, :197428 |
| Per-polygon shading | The flat-shading path specifically (distinct from `DoShading`); when off, all faces use light level 0xF | :197710 |
| D3D present | Hardware present path | - |
| Shadow LOD source | Which LOD level the shadow silhouette is taken from | - |
| **ShadowScale** | Shadow raster divisor: 0 = auto by frame-time budget, else 1 / 2 / 3 → divisors 1 / 2 / 4 | - |

**Game options block:**

| Setting | Meaning | Consumed at |
|---|---|---|
| LosOn | Line-of-sight on; when 0 every explored fog cell counts as visible | :167179, :130167 |
| MappingOn | Map starts unexplored; when off the explored mask starts filled | :167179 |
| CanDropPlayer | Multiplayer host may drop a player | - |
| DisableMapScript | Suppresses the mission script | - |

**Global game-state block.** These are the globals this note refers to by name:
map width and height in cells, world width and height, viewport
left/top/right/bottom and viewport width/height, camera position, **sea
level**, the explored mask, the graphic tile map, the map-cell grid, the
feature type table, the corpse instance table, the scene Z-row buckets, the
frame/tick counters, the unit array, and the per-player records.

Each **map cell** carries: the occupying unit id (16-bit), terrain height, a
feature id, a corpse id, and two flag bytes.

## Open items

- The water-amplitude scalar and the phase-rate scalar are globals whose values
  have not been read out of the binary's data section. Both are needed for
  exact water animation. Until then the shimmer can only be qualitatively
  matched.
- The concrete EmitSfx body is not recoverable from a data-only vtable. It
  needs a runtime trace, plus the Cavedog `sfxtype.h` header and the effects
  TDFs.
- The 0x1007x000 / 0x1008x000 COB host opcode rows need one focused
  re-verification pass (see the discrepancy above).
- Two definition flag bits are known only by value: 0x2000000 (suppresses the
  shadow and enables 2×AA) and 0x1000000 (corpse does not sink). Their FBI key
  names should be recoverable from an FBI dump.

## Ranked visual gap list

1. **Fog overlay rewrite** (fully specified above, and it kills grid artifacts).
2. **Animated water + shoreline** (large screen area on most maps).
3. **Underwater tint / hull clip** (per-pixel height key + BLUE_TABLE).
4. **Unit shadows** (every unit every frame).
5. **Corpses + magic-death dissolve** (very visible after fights).
6. **SFX/EMIT host** (remaining unknown, vtable body unrecoverable, so use
   Cavedog sfxtype.h + effects TDFs + runtime trace).
7. Health bars & selection rings to exact spec (cheap now).
8. Z-row bucketing with short/tall feature split.
9. Team colour = pre-baked texture variants (replaces the indices-16..23 guess).
10. **TA_SCALE → 1 unit = 1 px (do FIRST, it rescales everything).**
11. Nanoframe 0.5 threshold re-check (invisible above 0.5!).
12. 2×AA path, stone/freeze recolour (niche).
