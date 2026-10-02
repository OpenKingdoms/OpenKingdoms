# A unit being built is an Intangible Mass (2026-10-02)

Issue #313 asked whether the original draws a unit that is under half
built. It does. The game's own status line for such a unit reads
`UNITMISSIONCODE_BEINGBUILT = Intangible Mass`
(english/translate/unitmissions.tdf), and that is what it draws: the
unit's silhouette, filled with a shimmer of its side's build colours,
fading in over the first half of the build and back out over the second.
This note records what the draw path does and what changed.

## The fraction the draw path reads

A unit keeps the share of its build still to do. It is 1.0 when the
site is placed and drops to 0.0 when the work is done
(legacy:39496-39500). Every test below reads that share, so "under one
half left" means more than half built. We keep the same quantity as
hit points over maximum hit points, and the draw code works from the
share built, health over maximum.

## What the original draws

The model is rasterised into a sprite whenever the unit needs a fresh
one. While the unit is unfinished, the finished raster is also copied
into a second buffer of the same size (legacy:197003-197017).

The body sprite is drawn only while less than half the build is left
(legacy:197474-197476). It goes out at full alpha in the plain blend
mode, the same as a finished unit (legacy:197441-197466). There is no
fade on the body. It appears whole just past half built.

The second buffer is drawn over the body whenever any of the build is
left (legacy:197480-197489). Before it is drawn, every pixel the unit
covers is replaced by a palette index from 0x20 to 0x9f, picked per
pixel from a hash of the pixel's column, its row and the game tick
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
Taros orange, Veruna cyan and Zhon a pale blue that is close to white.
That is the range of colours the gameplay footage shows.

The mass is drawn in blend mode 6. Mode 4 draws with no blending, mode
5 keeps the transparent key and mode 6 blends by the alpha each vertex
carries (legacy:366451-366473). The sprite blit puts its alpha argument
into every vertex over white (legacy:196666, legacy:196730), so mode 6
is a plain alpha blend of the texture. The alpha is worked out from the
share left, `left * 510` while under one half is left and
`(1 - left) * 510` otherwise, cut to a whole number (legacy:197483).

| Built | Body | Mass alpha |
|------:|------|-----------:|
| 0% | not drawn | 0 |
| 25% | not drawn | 127 |
| 49% | not drawn | 249 |
| 50% | not drawn | 255 |
| 75% | opaque | 127 |
| 100% | opaque | no mass |

So the mass is solid at half built, and from there the body is already
under it and shows through as the mass fades.

## The test at legacy:197310

The branch the issue and an older note pointed at belongs to the shadow
pass, which runs only with shadows on. It draws the unit's shadow, not
its body. With nothing left it draws in mode 5, opaque. Otherwise it
draws only while under half is left, in mode 6 at alpha
`(0.5 - left) * 255` (legacy:197309-197322, and legacy:197252-197265
for a unit with a shadow sprite). A shadow fades in to half strength
over the second half of the build and turns solid when the unit is
done. We still draw no shadow for a unit being built, as
docs/MANUAL_DEVIATIONS.md R-003 records.

## What changed

- The side table reads `buildpalette`.
- Classic view. A unit being built is drawn from the moment it is
  placed. The body draws opaque once more than half is built. The mass
  is rasterised as one silhouette into an offscreen target, filled from
  the build palette by the same per pixel hash of column, row and tick,
  and blended over the scene once at the alpha above, so overlapping
  faces do not stack. Like the whiteout, the silhouette covers every
  face of the model, including texels the body keys out.
- 3D view. The body follows the same rule. The mass is the model drawn
  flat in the mean of the side's 128 build colours at the same alpha,
  after a depth only pass so that only the front surface blends.
- The 3D view never sent its tint colour to the shader, so a tint
  mixed toward black. The build preview darkened where it should have
  turned green or red. It is sent now.

Drawing reads the simulation and writes none of it. The sim hash, the
pinned `test_sim_probe` run and the save format are unchanged.

## Tests

- `a_unit_under_half_built_is_an_intangible_mass` (test_ui_screens)
  reads the frame at 0, 25, 49, 50, 75 and 100 percent built against
  the same frame with the unit moved away and with it finished, checks
  that the colour blended in is the build palette's, and checks the sim
  hash across every frame.
- `a_unit_under_half_built_is_an_intangible_mass_in_3d` (test_view3d)
  does the same in the 3D view.
- `base_sides_offer_the_four_kingdoms` (test_sides) reads the new key.
