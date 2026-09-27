# The engine as a library

`okengine` is the whole engine behind a small C API, `include/ok_embed.h`, for a host that draws the game itself. The Unity remaster is the first host. The engine simulates, and the host draws, plays the interface and sends the player's orders back.

## Building it

It is off by default. Configure a tree with `-DTAK_BUILD_EMBED=ON` and build the `okengine` and `test_embed` targets. Unity is 64-bit, so a Unity host needs an x64 tree:

```
cmake -S . -B build-x64 -A x64 -DTAK_BUILD_EMBED=ON \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON \
  -DTAK_GAME_DIR="<your install>"
cmake --build build-x64 --config Release --target okengine test_embed
```

The library needs `SDL2.dll` next to it. `test_embed` needs the game data and carries the `needs-data` label.

## How it runs

The engine boots the way the battle tests do: the file system over the player's own install, a hidden platform on SDL's dummy video driver with a software renderer, the interface tables, then the loading screen run to the end. Nothing opens a window. Textures the loaders upload keep a CPU copy while an embedding host is attached, so atlases and pictures can be read back.

Sound is off until the host asks for it with `okx_audio`. Then the engine plays its own unit, weapon and interface sounds and the music straight to the audio device, and `okx_set_view` tells it where the host's camera looks.

## What the API covers

| Area | Calls |
| --- | --- |
| Lifetime | `okx_init`, `okx_shutdown`, `okx_last_error`, `okx_set_override_dir`, `okx_audio` |
| Catalogue | maps with their info and overview picture, unit types with costs and build menus, feature types |
| A battle | `okx_start_skirmish`, `okx_tick`, `okx_outcome`, `okx_players`, `okx_economy`, `okx_command` |
| Ground | the height grid, the block table and the chunk pictures, `okx_ground_height`, the fog grid |
| Art | models with their pieces, triangles, texture atlases and vertex colours, sprite pictures, effect strips |
| Each frame | units, features, projectiles and effects, with a pose for every model piece |
| The HUD | build sites, factory queues, unit orders |
| The studio | a unit's script functions, and a pose from any of them played outside the battle |

Positions are the engine's own: world pixels, x east, z south, y up, 16 pixels to a cell. A pose is a row major 3x4 matrix per model piece, from the piece's local space to world pixels. It includes the model scale and the models' mirror in x, so its determinant is negative. A host that keeps its vertices in model units gets matrices with a very small scale in them, which some lighting code handles badly, so the Unity host scales vertices into world units when it builds a mesh and divides the matrix by the same scale.

Orders go through the command queue as `TAK_Cmd_EmitUnit` sends them, so they reach the units on the tick their turn comes round, exactly like the player's clicks.

## Replacement models

`okx_set_override_dir` names a folder of `.glb` files that replace shipped models by name: a unit's or feature's object name, or a sprite feature's sequence name, in lower case. The engine reads them with the glTF loader the 3D view uses. A node named like a piece of the shipped model takes that piece's pose from the unit's script.

## Versions

`OKX_API_VERSION` goes up whenever a function or struct changes shape, and a host checks `okx_api_version` before anything else.
