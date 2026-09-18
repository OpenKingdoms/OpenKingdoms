# Custom models for the 3D view

The 3D view draws the shipped 3DO models. Put a glTF 2.0 binary in
`models3d` named after a unit's object name and it draws that instead.
Nothing else changes: the classic view keeps the shipped model, and so
does every player without the file.

## In a browser

Drag the .glb onto the page at any time, or use Add 3D models on the
screen that asks for your game files. Either way it is written where
the engine reads loose files from, and if you asked the page to
remember your game files it is remembered with them.

The 3D view asks for a model the first time it draws a unit and again
after a world changes, so a model added during a battle is drawn from
the next battle.

## Where the file goes

`models3d/<objectname>.glb`, under the data directory the game reads.
The object name is the `objectname` line in the unit's FBI, lowercased.
The Aramon lodestone is `ARALODE`, so its model is
`models3d/aralode.glb`. One file replaces that name wherever it is
drawn, which for a few names means units, map features and projectiles
alike.

If the file is missing, or will not read, or asks for more than a model
can hold, the shipped model is drawn and a line on standard error says
why. A bad model can lose you the new art. It cannot take the game down.

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
is authored at another scale, put the factor in the file's root
`extras` as `tak_scale` and it is applied on load.

Textures are PNG or JPEG, embedded, and power of two in both
dimensions. The browser build is WebGL 1, where a mipmapped texture of
any other size samples as black. 1024 or 2048 square is a good place
to sit.

Base colour is what the shader reads. Metalness, roughness and normal
maps are not read yet, so anything that should be seen belongs in the
base colour.

A material named `teamcolor` takes the owning player's colour instead
of its own. Use it for the parts that should say whose lodestone this
is.

## What a model may not exceed

A model holds at most 128 pieces, 65535 vertices, and 32 distinct
textures. A few thousand triangles is nothing to the renderer. A model
past any of those limits is refused and the shipped one is drawn.

## What is not there yet

A custom model draws at rest. The scripts that swing a unit's legs
address pieces by the position they take in the shipped model, and a
model with its own pieces has its own order, so the two are not joined
until a name map lands. For a lodestone or any other building this
makes no difference.

## Axes

glTF is right handed with Y up. The engine's model frame is not, so
the reader turns one axis around and winds every triangle the other
way to match. An artist has nothing to do about this. It is written
down here because it is the first thing to suspect if a model ever
arrives inside out.
