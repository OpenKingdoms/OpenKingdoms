# Product requirements: the 3D mode

Status: proposed, 2026-09-15. Nothing in this document is built. It
exists so that the engine work, the art work and the tooling can start
in parallel against one agreed target, and so a contributor can tell
what is in scope without asking.

---

## The one sentence

OpenKingdoms gains a second view of the same game, a free camera over
real 3D terrain and models, that a player can switch into and out of,
that plays the same multiplayer match as the classic view, and that
changes nothing about the simulation.

---

## Why this, and why now

Total Annihilation: Kingdoms was always a 3D game wearing a fixed
camera. Every unit is a polygonal model made of named pieces, and every
animation is a script that moves those pieces by name. What reads as
"2.5D" is two things only: the camera cannot move off its angle, and
the terrain is a heightmapped picture rather than a mesh.

That means a 3D mode is not a rewrite. It is a camera that can move, a
terrain that is a surface, and a model loader that can hand back a
modern mesh in place of the original one. The animation scripts, the
unit definitions, the rules, the networking and the save format all
stay exactly as they are.

The engine is the right place for this and the timing is right for
three reasons.

The simulation is done and proven. It is derived from the legacy
reference, it is deterministic, and as of release 0.1.3 Windows, macOS,
Linux and the browser all produce the same simulation hash for the
same match. A 3D view built on that inherits all of it.

The view and the simulation are already separate. The simulation reads
commands and writes state. The renderer reads state and draws. Nothing
the renderer does is hashed, saved or sent over the wire. A second
renderer can be added beside the first without the first noticing.

There is an artist ready to build for it. Modern, fully textured models
for the ships, buildings and inorganic units are being made in Blender
now. What that work needs from the engine is a published contract to
build against and a way to check a model without building the engine.
Both are cheap and both come first in this plan.

---

## Goals

Play the whole game in 3D. Skirmish, story mode, and multiplayer, with
every unit, building, feature, projectile and effect the classic view
draws, drawn in the 3D view.

One game, two views. A player in the 3D view and a player in the
classic view sit in the same room, play the same match, watch the same
replay, and produce the same hash. Switching views is a preference,
never a fork.

Original assets work on day one. The 3D view draws the shipped 3DO
models and the shipped terrain before any new art exists. Modern
models are an upgrade path, not a prerequisite.

Modern assets drop in. A model made to the published contract replaces
the original model for that unit with no engine change and no script
change.

Every platform. Windows, macOS, Linux and the browser, the same four
the classic view ships on.

Parity where it matters. The 3D view is a different camera on the same
world, so what a unit does, when it does it, and what a player can and
cannot see are the classic view's answers. Where the 3D view has to
choose something the original never had to choose, it chooses the
reading a player of the original would recognise.

## Non goals

The simulation does not change. No rule, no number, no order of
evaluation, no hashed field, for any reason this document names. A
change the 3D view needs from the simulation is a design defect in the
3D view.

No new gameplay. No new units, weapons, abilities, resources or modes
of play. A first person "walk the deck" mode is discussed at the end as
a follow on, and it is a camera and an input scheme, not gameplay.

No engine change of platform. This is not a port to Unity, Godot or
any other engine. The reasons are in the section on alternatives.

No shipped art. The engine ships no game content, before this document
and after it. See ASSETS.md. Modern models are a separate project under
their own licence, loaded from the player's machine like every other
asset.

No rights or commercial work. Nothing here depends on owning, licensing
or negotiating for the Total Annihilation: Kingdoms property.

---

## Who this is for

A returning player who wants to see the game they remember from a
camera they never had. The classic view stays exactly as it is for the
player who wants the game they remember from the camera they had.

A modeller who wants their work in a running game rather than in a
folder, with a fast answer to "did I build this right".

A contributor who wants to work on rendering without touching, or
needing to understand, the simulation.

---

## Principles that decide the hard calls

The simulation is the source of truth and it is read only. Every
position, heading, animation state, health value and visibility answer
comes from the simulation. The view never computes a game fact of its
own, and it never writes one back. If the view needs a fact the
simulation does not expose, the fix is a read only accessor on the
simulation, tested, and never a field the view keeps for itself.

Clicks resolve in the view, orders resolve in the simulation. The view
turns a pointer into a world position or a unit. What happens to that
position or unit is the command pipeline's business, and it is the
same pipeline the classic view uses. The release 0.1.3 click work
established this boundary for the classic view. The 3D view adopts it
whole.

Original first, modern second. Every feature lands drawing the shipped
assets before it draws anything new. That keeps the 3D view honest
about parity and keeps it playable at every step.

The art contract is frozen early and changed rarely. An artist builds
for months against it. A change to it is a versioned event with a
migration note, not a commit.

One renderer, one code path. The 3D view is one renderer targeting one
graphics API baseline, with the browser as a first class target rather
than a port. No per platform rendering code.

---

## What the player sees

### Switching views

A single setting, in the options screen and on a key, switches between
the classic view and the 3D view. Switching is instant, in game,
without a reload, and preserves selection, camera focus and every
order in flight. Multiplayer does not know or care which view any
player is in.

### The camera

A free camera with orbit, pan, zoom and tilt, bounded to the map and
to a floor above the terrain. Presets that return to the classic angle
at the classic height, so a player is never lost. Edge scroll,
keyboard scroll and minimap click behave as the classic view's camera
does, translated into the free camera's terms. Following a selected
unit is a toggle.

### Terrain

The map is a mesh built from the heightmap the simulation already
holds, textured with the map's own chunk images. Cliffs, slopes and
water read as such. The water line is the simulation's water line. What
a unit can walk on is unchanged, because that answer comes from the
simulation.

### Units, buildings and features

Every unit and building draws as its model, either the shipped 3DO or
a modern replacement, at the position, heading and animation state the
simulation reports. Piece animation is driven by the same COB script
execution the classic view uses, applied to the same named pieces.
Trees, rocks and every other map feature draw as the models the map
places.

Team colour is visible and correct on every unit in every view, with
the shipped models coloured by their palette indices as the original
does, and modern models coloured through the mask the art contract
requires.

### Combat and effects

Projectiles fly the paths the simulation computes. Explosions, smoke,
magic, weather and every other effect the classic view draws have a 3D
counterpart. Where the original effect is a flat sprite, the 3D view
draws a billboard of the same sprite until a volumetric version
exists. Parity of timing and placement comes first, and prettiness
comes second.

### Fog and line of sight

What is explored, what is visible now and what is hidden are the
simulation's answers, and the 3D view draws exactly those. Unexplored
ground is dark, explored ground out of sight is dimmed, and nothing a
player cannot see in the classic view is visible in the 3D view. This
is a correctness requirement, because it is what keeps the two views
fair against each other in one match.

### Selection and orders

Click, drag select, shift select, control groups, and every order the
classic view takes work in the 3D view. A click lands on the ground
under the pointer, on the terrain surface, not on a flat plane. A drag
box selects what is drawn inside it. Order feedback, the acknowledgement
voice, the cursor and the waypoint marker, fire on click as they do in
the classic view.

### Interface

The HUD, the sidebar, the minimap, the build menus, the in game menus,
the end screens, the Book of Deeds and every cut scene are the classic
view's, unchanged, drawn over the 3D world. The minimap is the flat map
picture it has always been, with the camera's footprint drawn on it as
a frustum rather than a rectangle.

---

## Architecture

### The boundary

Today the simulation and the renderer are separate in practice: the
simulation is hashed, saved and replicated, and the renderer is none of
those. The 3D mode makes that separation a boundary in the code.

A view is anything that implements a small interface: initialise with
a platform, render one frame from a read only world, map a pointer to
a world position and to a unit, and shut down. The classic renderer
becomes the first implementation of that interface. The 3D renderer is
the second. The in game screen owns one active view and forwards to it.

The interface is deliberately narrow. If a view needs something the
interface does not give it, the interface grows by one read only call,
with a test, rather than the view reaching around it.

### The graphics baseline

The classic view draws through SDL's 2D rendering API, which has no
concept of a mesh or a depth buffer. The 3D view needs a real graphics
API, and the choice is decided by the browser.

The baseline is OpenGL ES 3.0, which is what WebGL 2 exposes, reached
through SDL's OpenGL context on the desktop and through Emscripten's
WebGL 2 binding in the browser. One shader language, one feature
level, one code path. On macOS this runs on the system OpenGL, which is
deprecated but present and adequate for this baseline. A Metal path
through a translation layer is a later concern and is not required to
ship.

WebGPU is the eventual target in the browser and the eventual path to
Metal and Vulkan on the desktop. It is not the baseline because it is
not yet where every player's browser is. The renderer keeps its
platform touching code in one place so that move is a bounded change
when the time comes.

### The renderer

A forward renderer with a depth buffer, a single directional light
matching the map's lighting direction, simple shadows, fog by distance
that never hides what line of sight says is visible, and instanced
drawing for repeated models such as trees and unit groups.

Models are baked once at load into GPU buffers, per piece, so that
piece animation is a per piece transform rather than a re upload. This
is the same flattening the classic view does today, kept in GPU memory.

Terrain is a mesh at the heightmap's resolution, chunked to match the
existing chunk texture layout so the existing loading and deduplication
work carries over, with a coarser level for distant chunks.

### The asset path

A unit definition names a model. The loader asks the model store for
that name. The store returns the shipped 3DO unless a modern model for
that name is present in the player's asset directory and passes the
validator, in which case it returns the modern one. Nothing above the
store knows which it got.

Modern models are glTF 2.0, one file per unit, loaded from a directory
beside the player's game files. The engine ships none, indexes what it
finds at start, and reports what it rejected and why.

### Determinism, stated as a guarantee

The 3D view reads the simulation and writes nothing to it. No field of
the simulation, no hash input, no save field and no wire message
changes for this work. The gate that proves it is the existing
simulation probe, which runs a fixed synthetic battle and pins one hash
across all four platforms. That pinned number does not move for any
commit under this document. A commit that moves it is wrong by
definition.

A second gate is added: a scripted match played once in the classic
view and once in the 3D view, from the same seed and the same commands,
must finish with the same hash. That is the proof that the two views
are the same game.

---

## The art contract

This is the part of the document an artist reads, and it is the part
that must not change casually. It is written from what the engine's
loader actually requires, in src/render/obj3d.c and
include/tak_obj3d.h, not from preference.

### Pieces

A unit is a tree of named pieces. Each piece has a name of up to 31
characters, a parent, and an offset from its parent. The animation
scripts move pieces by these names and know nothing else about the
geometry. A modern model must therefore carry the same piece tree: one
node per piece, the same names, character for character, and the same
parenting.

Every piece's origin sits at that piece's pivot, the point its offset
names. Rotations happen about the origin. A turret whose origin is at
its geometric centre rather than at its pivot will swing around the
wrong point, and nothing will flag it until it is seen. This is the
most common mistake and the validator checks it first.

Extra pieces are allowed for detail that the scripts never move, as
children of a real piece. Missing pieces are not allowed. A model with
a piece the script expects and cannot find is rejected.

### Scale and axes

The original stores vertices as 32 bit integers in its own units,
where 65536 is roughly one metre. The contract fixes one conversion
factor between glTF metres and those units, published with the
manifest, and the validator checks each piece's pivot against the
original within a tolerance.

The axis convention is published with the manifest as a worked example
and a reference model, rather than described in prose, because prose
descriptions of handedness are how models arrive mirrored.

### Team colour

The original colours untextured polygons by an index into the faction
palette, which is how a player tells one side from another. A modern
model has no palette. It must carry a team colour mask, a texture
channel that marks where the side's colour is applied, and the engine
tints those regions with the player's colour at draw time. This is a
requirement on the first model and every model after it. It is stated
early because adding it to a finished library of models is the single
most expensive mistake this project could make.

### Selection volume

Each original model carries a selection primitive that the game uses
for picking. A modern model provides an equivalent as a named node, or
the validator derives one from the bounds and warns.

### Level of detail

Optional. A model may carry lower detail variants as named nodes and
the renderer will use them by distance. A model with none is drawn at
full detail always. The contract names the convention and requires
nothing.

### What is not in the contract

Textures, materials, polygon budgets and style. Those are the art
project's decisions. The engine draws what it is given, within a
published per model triangle ceiling that exists to keep the browser
playable and that is generous.

---

## Tooling

### The piece manifest

A script over the shipped 3DO files that emits, per model, the full
piece tree with names, parenting and pivot offsets, plus the selection
primitive and the model's bounds, as one human readable file per model
and one index. This is the first deliverable in the plan because it is
hours of work and it gives the art project an exact target for all 389
models on day one.

### The validator

A command line tool that takes a glTF and a model name and reports
pass or fail with a reason: missing pieces, unexpected names, wrong
parenting, pivots outside tolerance, no team colour mask, triangle
count over ceiling. It runs with no game data and no graphics, on all
four platforms, so an artist can run it locally in seconds. It is the
same code the engine's model store uses to accept or reject a model,
so a model that passes the tool is a model the engine loads.

### The reference model

One shipped unit, rebuilt in Blender to the contract by the art
project, checked in to the art project as the worked example of axes,
scale, pivots and team colour. The validator's own tests use it.

---

## Platforms and performance

Windows, macOS, Linux and the browser, all four, from the first
milestone that draws anything. The browser is not a port at the end.

Targets on a mid range laptop from the last five years, in the browser:
sixty frames per second at 1080p on a large skirmish map with the unit
cap reached, and never below thirty. The desktop builds are expected
to exceed this comfortably.

Load time for the 3D view on a large map within twice the classic
view's load time, with the difference budgeted across the existing
loading screen's frames so the progress bar stays honest.

Memory in the browser under the existing ceiling the classic view
already respects, because the browser is where memory runs out first.

---

## Testing

Every gate the engine already has stays green, including the pinned
simulation hash across all four platforms.

A scripted match gate proves the two views produce the same hash from
the same seed and commands.

A click mapping test for the 3D view, in the shape of the existing
click_map test, pins pointer to ground and pointer to unit in numbers
over a range of camera positions and window sizes, and runs under node
in the browser job.

Golden image tests for the 3D view render fixed scenes and compare
against pinned images with a tolerance, per platform, so a rendering
regression is caught the way the classic view's are.

The validator has its own tests, using the reference model and a set of
deliberately broken variants, one per failure it reports.

Every fix has a failing test first. This is the project's standing
rule and it applies here without exception.

---

## Milestones

Each milestone is playable, on all four platforms, before the next
begins. Each is its own set of pull requests through the full suite.

M0, the contract and the manifest. The art contract published as a
document. The piece manifest generated and checked in. The reference
model requested from the art project. Nothing in the engine changes.
This is days, and it unblocks the art project for the whole duration
of everything below.

M1, the boundary. The view interface defined, the classic renderer
moved behind it, the in game screen owning one view. No visible change.
Proven by the full suite and the pinned hash not moving.

M2, first light. An OpenGL ES 3.0 renderer that draws the shipped 3DO
models with piece animation from the existing COB execution, on a flat
plane, under a free camera, switchable from the classic view in game.
Selection and orders work through the view interface. Multiplayer
between the two views proven by the scripted match gate.

M3, the world. Terrain as a mesh from the heightmap with the chunk
textures, water, map features, the fog and line of sight overlay, the
minimap frustum. Every skirmish map loads and plays.

M4, the modern path. The glTF loader, the model store with fallback,
team colour through the mask, the validator shared between tool and
engine. The reference model draws in game beside the shipped ones.

M5, combat and effects. Projectiles, explosions, magic, weather and
every remaining effect, billboarded first, then improved where it
matters. Story mode and the cut scenes verified in the 3D view.

M6, the browser at speed. Performance work against the targets above,
level of detail, instancing, load budgeting. This is where the browser
becomes a first class 3D platform rather than a working one.

M7, follow on, optional. A first person mode that drops the camera into
a selected unit with direct control, which is a camera preset and an
input scheme on top of everything above, and no simulation change.

---

## Alternatives considered

Rebuild in Unity or another engine. Rejected. It discards the one
component that took the longest and is hardest to reproduce, the parity
verified deterministic simulation, and replaces a browser build that
works with one that works worse. The game's actual difficulty is in the
simulation, and a second project in this space has already stalled at
alpha after building the renderer first. If a general engine is ever
wanted, the path is to embed the C simulation as a native plugin and
let that engine be the view. That path stays open under this plan and
gets easier as the boundary hardens.

Convert the original models to a modern format offline and drop the
3DO path. Rejected. It breaks the rule that the engine ships no
content, and it makes modern art a prerequisite rather than an upgrade.

Begin with WebGPU. Deferred. The right eventual target, not yet where
every player's browser is. The renderer is structured so the move is
bounded.

Build the 3D view as a separate binary. Rejected. It forks the game.
One binary, one setting.

---

## Risks

Piece pivots. The single most likely source of wrong looking animation
on modern models. Mitigated by the validator checking pivots first and
by the reference model being the worked example.

Team colour retrofit. If the mask is not required from the first model,
it will have to be added to every model later. Mitigated by making it
a hard requirement in the contract and a validator failure.

macOS graphics. OpenGL is deprecated there. Mitigated by the baseline
being modest and by keeping the platform touching code in one place so
a Metal path is a bounded later change.

Scope creep into the simulation. Every rendering problem has a tempting
simulation fix. Mitigated by the pinned hash being a hard gate and by
the rule that a needed fact becomes a read only accessor and nothing
else.

Browser performance. A large map at the unit cap in WebGL 2 is real
work. Mitigated by making the browser a target from M2 rather than a
port at M6, so the cost is discovered early.

Art project pace. The engine should never wait on art. Mitigated by
original first, modern second: every milestone is complete with the
shipped assets alone.

---

## Open questions

The exact conversion factor and axis mapping between glTF and the
original's units, to be settled by measurement against a shipped model
and published with the manifest in M0.

Whether shadows ship in M2 or M3. A directional shadow map is cheap on
the desktop and not free in the browser.

How a modern model announces which unit it replaces: by file name, by
a field inside the file, or both. The validator needs one answer.

Whether the classic view's sprite based effects are acceptable as
billboards for the first release of the 3D view, or whether a minimum
set is volumetric. The proposal is billboards first.

---

## What success looks like

A player switches to the 3D view in the middle of a skirmish and keeps
playing without a reload. Two players, one in each view, finish a
multiplayer match with the same hash. A modeller builds a unit in
Blender, runs the validator, sees green, drops the file beside their
game files, and watches it walk across the map animated by the original
script. All of it on a Chromebook in a browser tab.

---

## Related documents

ASSETS.md, on why the engine ships no content and never will.
MULTIPLAYER.md, on the lockstep design the 3D view inherits.
PARITY.md, on what parity means and how it is checked.
docs/notes/2026-09-15-a-click-lands-on-the-ground-under-the-pointer.md,
on the click boundary the 3D view adopts.
