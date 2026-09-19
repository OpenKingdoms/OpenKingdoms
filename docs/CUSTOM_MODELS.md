# Custom models for the 3D view

The 3D view draws the shipped 3DO models. Put a glTF 2.0 binary in
`models3d` named after a unit's object name and it draws that instead.
Nothing else changes: the classic view keeps the shipped model, and so
does every player without the file.

## Where the file goes

Make a `models3d` folder inside your Total Annihilation Kingdoms
folder, beside the .hpi archives, and put the model in it named after
the unit's object name.

    Total Annihilation Kingdoms/
      data.hpi
      terrain.hpi
      models3d/
        aralode.glb

The object name is the `objectname` line in the unit's FBI. The Aramon
lodestone is `ARALODE`, so its model is `aralode.glb`. Case does not
matter. One file replaces that name wherever it is drawn, which for a
few names means units, map features and projectiles alike.

These are the lodestones:

    aralode.glb   Aramon Lodestone, two by two
    tarlode.glb   Taros Lodestone, two by two
    verlode.glb   Veruna Lodestone, two by two
    zonlode.glb   Zhon Lodestone, two by two
    aramana.glb   Aramon Divine Lodestone, three by three

## Standing stones and other flat scenery

The stones around a mana site, and most trees, rocks and ruins, are
not models in the original. They are flat pictures, and in the 3D view
a picture can only lie on the ground. Each one is a feature with a
sequence name in the `features` data, `VerHenge01` for the first
Veruna standing stone, and a model named after that sequence stands
where the picture would have lain:

    models3d/
      verhenge01.glb

The sequence names for the standing stones are `AraHenge01` to
`AraHenge11`, `TarHenge01` to `TarHenge14`, `VerHenge01` to
`VerHenge11` with `01b` and `05b`, and `ZonHenge01` to `ZonHenge11`.
Each feature has a footprint in build squares of sixteen pixels and a
height in pixels in its definition, and a model should be built to
those rather than to the picture, which the camera foreshortens. The
classic view keeps drawing the picture.

A game built from source with a data directory reads `models3d` from
there too, which is where a developer's loose files live.

If the file is missing, or will not read, or asks for more than a model
can hold, the shipped model is drawn and a line on standard error says
why. A bad model can lose you the new art. It cannot take the game down.

## In a browser

The same folder works. Pick your game folder on the page and anything
in its `models3d` comes along with the archives, so you play with your
models the way you would on the desktop.

A model can also be added on its own at any time, running or not:
drag the .glb onto the page, or use Add 3D models on the screen that
asks for your game files. If you asked the page to remember your game
files, models are remembered with them.

The 3D view asks for a model the first time it draws a unit and again
after a world changes, so a model added during a battle is drawn from
the next battle.

## What the file should hold

One `.glb` with the textures embedded. Blender exports this natively
through File, Export, glTF 2.0, with Format set to glTF Binary.

Pieces stay separate named objects rather than being joined. The names
are how piece animation will address them once that lands, and they
come through into the model the engine holds.

Triangles only. Turn on Triangulate on export, or the exporter's
n-gons are dropped.

The origin sits at the centre of the base, on the ground plane, since
the engine plants a model at the terrain height under it.

One glTF unit is one world pixel. The shipped lodestone is about 64
pixels across and 29 tall on a two by two build footprint, and the
whole of it is twelve vertices, so there is room to spend. If a model
is authored at another scale, give any object a custom property named
`tak_scale` in Blender's Object Properties panel and export with Custom
Properties on. The factor is applied on load, so a model built at five
units across with `tak_scale` of 13 arrives 65 pixels across. A file
that carries it in its root `extras` instead is read the same way.

Textures are PNG or JPEG, embedded. A power of two size in both
dimensions gets mipmaps and stays smooth at a distance. Any other size
draws, but without them, so it shimmers when the camera is far off.
1024 or 2048 square is a good place to sit.

## What a material may say

The shader reads the material as Blender's Principled BSDF writes it.

Base colour, as a factor, a texture, or both multiplied together.

A normal map, with its strength. A tangent is made for every vertex
from the way the picture lies across the surface when the file brings
none, which is what Blender exports.

Metallic and roughness, as factors and as the packed texture with
roughness in green and metal in blue. A rough surface takes a broad
soft highlight, a smooth one a tight bright one, and metal colours the
highlight with the base colour.

Emission, as a colour or a texture, added on top of the lighting. A
crystal that should glow gets its light this way. A material whose name
has `glow` or `pulse` in it breathes, its emission rising and falling
over about two seconds, which is the living light in a lodestone.

Light glances off a smooth surface at its edges, so glass and polished
metal at low roughness catch the sky along their silhouette. Rough stone
does not.

Blend mode. Opaque draws as it is. Alpha Clip drops fragments fainter
than the clip threshold. Alpha Blend draws the part over what is behind
it, after the solid parts of the model, so a glass crystal shows what
it stands on.

Backface culling. A material with it off draws both faces.

Each material's pictures are laid by one UV map. If the material's
pictures name the second map, that is the one used for all of them; a
picture laid by a different map from the rest of its material is left
out and said so in the log.

A material named `teamcolor` takes the owning player's colour instead
of its own. Use it for the parts that should say whose lodestone this
is.

A model's pictures are decoded and sent to the card once, then shared
by every team colour of it. A model with nine 2048 square textures
costs that once, not once for each player who builds one.

## What a model may not exceed

A model holds at most 128 pieces, 65535 vertices, 32 distinct
materials and 32 pictures. A few thousand triangles is nothing to the
renderer. A model past any of those limits is refused and the shipped
one is drawn.

## Pieces that move

The unit's own script animates a custom model, the same script that
rows the oars, swings the legs and blinks the lodestone's light in the
original. It addresses pieces by name. An object in the model named
like a piece of the shipped model takes that piece's turns, moves,
hides and shows; an object with a name of its own stands still. The
lodestone's light is two pieces, `aralode` and `aralode_off`, that the
script shows in turn, so a model with those two objects blinks as the
original does.

For this to look right the object's origin sits where the shipped
piece's pivot sits, an oar's at its oarlock, and the object rests in
the same orientation, since the script sets absolute angles. The
shipped piece tree, with each piece's offset from its parent, can be
read from the 3DO; ask for a listing of any unit. A model may have
more objects than the script knows, and they simply stay put.

## Axes

glTF is right handed with Y up. The engine's model frame is not, so
the reader turns one axis around and winds every triangle the other
way to match. An artist has nothing to do about this. It is written
down here because it is the first thing to suspect if a model ever
arrives inside out.
