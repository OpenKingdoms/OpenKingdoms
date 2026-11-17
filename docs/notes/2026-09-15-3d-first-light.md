# The 3D view, first light (2026-09-15)

Press V in a battle and the same world is drawn in 3D under a free
camera. Press V again and the classic view is back with nothing lost.
This note says how to get there, which keys do what, what the 3D view
draws and what it does not yet, how it is built so the classic view
cannot change, and what it costs on the owner's machine.

The product requirements for the whole 3D mode are in the 3D mode
document (docs/PRD_3D_MODE.md once it lands). This is the demo cut of
its milestone M2 with enough of M3 to look like the game.

## Starting a skirmish and entering 3D

Any battle will do. From the menu, Skirmish, pick a map, Play. In the
battle press V. The first press decodes the map's chunk images again
for the 3D terrain, which takes under a second on a small map and a
few seconds on Castle. Every press after that is instant.

For a demo without clicking through the menus:

```
tak-re --skip-logo --perf-probe build8 --reveal --view3d
```

That plays a human seat and seven computer players on Ladron's Tarn
with the map revealed and opens the battle in the 3D view.

## Saying what it is

Entering the 3D view puts one line over the top left of the play area
for six seconds: "3D view, experimental. Press V for the classic view."
It goes through HUD_DrawMessageLine, the same path the speed change
message uses, so it sits where the game already puts a message. The six
seconds are wall time rather than frames, because the frame rate here
runs from 240 to 450 and a frame count would have made the notice last
under a second.

The key is still undocumented anywhere a player would look. The options
screen is where a mode like this belongs, with the word experimental
beside it, and that is its own change rather than part of this one.

## Keys

The classic bindings all still work: WASD and the arrows scroll, edge
scrolling scrolls, the minimap click jumps, digits recall squads, and
so on. The 3D view adds these, on keys the original leaves unbound in
its keys file (LOWER_Q, LOWER_E, LOWER_R, LOWER_F, LOWER_V, LOWER_X,
LOWER_Z are empty there, and Home is not bound).

| Key | Does |
| --- | --- |
| V | Switch between the classic view and the 3D view |
| Q and E | Orbit the camera left and right around its target |
| R and F | Tilt the camera up and down |
| Z and X | Zoom in and out |
| Mouse wheel | Zoom |
| Middle button drag | Orbit and tilt |
| Home | Snap back to the classic angle and height over the same spot |
| WASD, arrows, screen edges | Scroll, in the camera's own directions |

The camera is bounded to the map, to a tilt between 12 and 89 degrees,
to a distance between 48 and 6000 pixels, and to twenty pixels above
the ground under the eye. Scrolling in 3D moves the classic camera with
it, so the minimap's rectangle and the return to the classic view both
land on the same ground.

A left click in the 3D view goes through the same order path the
classic view uses: the pixel is turned into a ray, the ray meets the
terrain, and that ground point is handed to InGame_WorldClick. Select,
move and attack work. Drag boxes are approximate in 3D, because the
box's corners are taken on the ground rather than in the picture.

## What is drawn

Terrain as a mesh at the heightmap's resolution, one vertex per 16
pixel tile, textured with the map's own chunk images. Each 32 pixel
block samples the same square of the same chunk the classic renderer
samples, so the ground reads the same. The chunk images are decoded a
second time into textures of the 3D view's own, with mipmaps, so the
classic renderer's textures are never touched.

Water as a translucent plane at the map's water line.

One directional light, applied to the terrain by its slope and to the
models by their face normals. Ground and models more than 2500 pixels
from the eye fade a little toward the sky colour.

Every unit and building, as its shipped 3DO model, posed by the same
COB piece state the classic view poses it by. Walk cycles, turrets and
wings animate because they come from the same script execution. Team
colour is right because the models are baked from the same per colour
atlases and the same faction palette the classic view uses.

Map features. Ones with a model (corpses, rubble, some rocks) draw as
models. Ones with a sprite (trees, most rocks, plants) stand up as
billboards facing the camera, drawn a little taller than their sprite
because the sprite was painted for the classic tilt. Mana pads lie flat
on the ground.

Fog of war. Unexplored ground is black, ground out of sight is dimmed,
and a unit the local player cannot see is not drawn, by the same tests
the classic view makes.

The selection, as a green ring on the ground under each selected unit
(red for a unit that is not the local player's).

The whole HUD, the sidebar, the minimap, the message line, the chat and
the in game menu, unchanged, drawn over the 3D frame.

## What is not drawn yet

Projectiles, beams, explosions and every sprite effect. Shadows. Health
bars. The construction sparkles. Unit whiteout deaths fade rather than
flash. Animated feature sprites show their first frame. The minimap
still draws the classic rectangle rather than the camera's footprint.
None of this is hard, all of it is later.

## How the classic view cannot change

The battle screen now holds one active view behind a small interface,
include/tak_view.h: draw a frame of a read only world, map a pointer to
a world point and to a unit, scroll. The classic renderer is the first
implementation by delegation, src/ui/view_classic.c, and it makes the
same three calls in the same order the screen always made. The 3D
renderer is the second, src/render/view3d.c. Nothing in the classic
renderer's internals moved.

The 3D view reads the simulation and writes nothing to it. The pieces
it needed that the unit renderer did not already expose are read only
accessors on units.c: bake a model by name, compose a mesh's piece
transforms from a unit's piece state, and read a feature's decoded
sprite frame. test_sim_probe's pinned hash is the gate and did not
move.

The proof for the classic view is src/render/test_view3d.c. It draws a
frozen battle in the classic view, switches to 3D, draws three frames
there, switches back, and the classic bytes are identical to a frame
drawn with no switch at all. The same test proves the camera comes
back over the same ground and the selection survives, that the 3D
pointer lands where the camera looks, that a scroll in 3D moves the
classic camera and a minimap jump moves the 3D camera, and that the
accessors hand out what the view needs.

## The graphics context

The classic renderer draws through SDL's 2D API, which has no depth
buffer. The 3D view needs a real context, and to share the window it
uses the one SDL's own opengl render driver creates. So on the desktop
the platform layer now asks SDL for the opengl driver by name (it used
to take SDL's first choice, Direct3D on Windows), creates the window
with a 24 bit depth buffer, and keeps SDL's draw batching on. A machine
with no usable opengl driver falls back to SDL's own choice and the V
key does nothing there but print a line.

The 3D frame is drawn between the classic renderer's clear and the HUD.
It flushes SDL's command queue, remembers every piece of GL state SDL's
renderer caches (the program, blend, scissor, viewport, the bound
texture, the client arrays, the vertex attribute arrays), draws, and
puts all of it back. An atlas texture SDL owns is bound through
SDL_GL_BindTexture and unbound through SDL_GL_UnbindTexture, which is
what keeps SDL's own texture cache truthful. One trap worth recording:
SDL leaves GL_UNPACK_ROW_LENGTH set from its last texture upload, so a
tightly packed upload of our own has to reset it first.

Every GL call is in src/render/gl3d.c behind a small API (frame,
camera, texture, mesh, three draws). Nothing else includes a GL header.
The shaders are GLSL ES 1.00, which every WebGL context and every
desktop compatibility context accepts, so the browser build compiles
the same file. The feature level used is the ES 2.0 subset of the ES
3.0 baseline the requirements name.

The camera's arithmetic is src/render/camera3d.c, with no SDL and no
GL in it, pinned in numbers by src/render/test_camera3d.c, which the
browser job also runs under node.

Models come from a store, src/render/model_store.c: the view asks by
object name and colour and gets a model baked into GPU buffers with its
named pieces. Today the one source is the 3DO bake. A glTF source
belongs beside it as a second file producing the same intermediate.

## Piece animation on the GPU

Each model's vertices stay in their piece's local space with a piece
index per vertex. Each frame the view composes the piece transforms
from the unit's COB piece state, the same composition the classic
renderer does, and uploads them as a uniform array. A draw covers the
piece range its uniform budget allows, 64 pieces on this machine, and
a model with more pieces than that is split across draws. A hidden
piece gets a zero transform and collapses to a point.

## Frame rate

Measured on the owner's machine (RTX 3070, Windows 10) at 1600 by 900
with vsync off, playing the build8 scenario on Ladron's Tarn with eight
seats: the 3D view's own render call costs about half a millisecond of
CPU per frame and the game runs between 240 and 450 frames per second,
against a simulation that keeps its 60 Hz with no capped frames. With
vsync on it holds the display's rate. The first press of V decodes the
chunk images for the terrain, under a second on this map.

Two things load this machine and show up as choppy movement without
being the 3D view's cost: several game instances running at once (each
spins a core with vsync off), and the browser build in a tab beside
the desktop build.

## Command line switches for captures

These exist for demos and screenshots and change nothing else.

```
--renderer <name>      the SDL render driver, opengl by default
--view3d               open every battle in the 3D view
--cam3d x,z,yaw,pitch,dist   where the 3D camera starts
--reveal               a --perf-probe scenario with the map revealed
--screenshot <bmp>     save the frame at --screenshot-tick and quit
```
