# Terrain Rendering Guide

This is your pair-programming doc for the next ~five milestones. We go
from "the game has no terrain renderer" to "the map draws at native
resolution and the camera scrolls smoothly."

The assumption in this guide is that you're new to game graphics as a
topic. That's the main audience. If something feels too basic, skim.
The build tasks start in Part 3.

---

## Part 0: Graphics 101

Five concepts will show up repeatedly. Read this section once, then
come back when a later part uses a term that feels fuzzy.

### 0.1 The CPU / GPU split

Your computer has two processors that do graphics work.

- The **CPU** is general-purpose, very fast at branching logic, and
  terrible at doing the same operation on a million things in parallel.
  It's the processor the rest of our code runs on.
- The **GPU** is specialized hardware that does one thing insanely fast:
  applies the same arithmetic to thousands of pixels in parallel. It's
  on your graphics card (or integrated into your CPU die on laptops).
  When people say "hardware accelerated," they mean "runs on the GPU."

When we *blit* a sprite with our `Blit_RGBA` function, we're using the
CPU to copy pixels one row at a time. 256×256 pixels = 65,536 copies
per sprite. That's fine for small UI sprites (a cursor, a button) but
painful for full-screen terrain at 1920×1080 = 2 million pixels per
frame. The GPU does that in a single instruction.

The catch: getting data *to* the GPU is slow. You upload a pixel buffer
("here, remember this image") once, then draw it many times. The more
frames you reuse an uploaded image, the faster the GPU wins.

### 0.2 Pixels, surfaces, and textures

Three words for things that hold image data:

- **Pixel buffer**: a flat `uint32_t *` array of RGBA pixels. Raw memory.
  `tak_malloc(w * h * 4)`. The thing our game generates internally
  (palette lookups, GAF decodes, JPG decodes).
- **`SDL_Surface`**: CPU-side pixel buffer with extra metadata (width,
  height, pixel format, stride/pitch, a lock for thread-safety). Lives
  in main RAM, and the CPU reads and writes it directly. Our 640×480 UI
  canvas is an `SDL_Surface`.
- **`SDL_Texture`**: pixel data uploaded to the **GPU**. The CPU can't
  read or write it directly. You either upload a whole pixel buffer
  (`SDL_UpdateTexture`) or lock it temporarily to write (`SDL_LockTexture`).
  The GPU can use it in a draw call at native speed.

The trip looks like:

```
   tak_malloc             SDL_CreateRGBSurface             SDL_CreateTexture
        ↓                          ↓                                ↓
  pixel buffer    ───►      SDL_Surface       ───►            SDL_Texture
  (CPU, raw)              (CPU, with format)              (GPU, ready to draw)
```

For terrain, we do each step: decode JPG into a pixel buffer, create an
`SDL_Texture` from it, keep the texture for the lifetime of the map,
free both the pixel buffer and the original JPG bytes as soon as the
upload's done. Only the texture stays around.

### 0.3 Render target and the draw call

A **render target** is "the image you're currently drawing into." Most
of the time it's the window, so whatever you draw ends up on screen. It
can also be a texture (*render to texture*, useful for shadow maps or
offscreen effects that we don't need yet).

A **draw call** is one instruction to the GPU that says "take this
texture and stamp it onto the current render target in this position."
In SDL, the function is `SDL_RenderCopy`. Under the hood it does:

```
SDL_RenderCopy(renderer, tex, src_rect, dst_rect)
  → "put the src_rect portion of tex onto the render target
     stretched into the dst_rect area"
```

`src_rect == NULL` means "the whole texture." `dst_rect == NULL` means
"stretched to fill the render target." If src and dst have the same
dimensions → pixel-exact copy. Different dimensions → GPU scales
automatically, using the texture's scale mode (nearest or linear).

**A frame is a sequence of draw calls bracketed by a clear and a present:**

```
SDL_RenderClear(renderer)       # wipe the render target
SDL_RenderCopy(...)             # stamp texture 1
SDL_RenderCopy(...)             # stamp texture 2
SDL_RenderCopy(...)             # stamp texture N
SDL_RenderPresent(renderer)     # send the assembled image to the display
```

Draw calls compose top-down: later ones cover earlier ones (subject to
blending, see the next section).

### 0.4 Blend modes and alpha

When a draw call stamps a texture onto the render target, it has to
decide what to do with the pixel underneath. That's the **blend mode**.

Four modes matter for us:

- **`SDL_BLENDMODE_NONE`**: overwrite. Whatever was there is gone.
  Fast. Use for terrain (chunks are fully opaque, and nothing should show
  through).
- **`SDL_BLENDMODE_BLEND`**: alpha-compose. `result = src.rgb * src.a +
  dst.rgb * (1 - src.a)`. Use for UI on top of terrain, so transparent
  canvas pixels let terrain show through.
- **`SDL_BLENDMODE_ADD`**: additive. Use for glows/explosions/lasers.
  Not needed yet.
- **`SDL_BLENDMODE_MOD`**: multiplicative. Used for lighting/fog
  overlays. Also not needed yet.

The 4th byte of a pixel (the `A` in RGBA) is the **alpha** channel.
0 = fully transparent, 255 = fully opaque. A pixel with alpha 0 in
`BLEND` mode doesn't affect the render target at all. A pixel with
alpha 128 mixes 50/50 with whatever was there.

Our UI canvas is currently drawn with `BLENDMODE_NONE`, so it covers
everything. In Part 3 we switch it to `BLENDMODE_BLEND` so terrain
underneath shows through transparent canvas regions.

### 0.5 Vsync and frame pacing

Your monitor refreshes at a fixed rate, usually 60 Hz (60 times per
second = every 16.6 ms). If you call `SDL_RenderPresent` faster than
that, you're doing work the monitor can't display: waste of GPU cycles,
and you can get **screen tearing** (the display showing the top half of
frame N and the bottom half of frame N+1 because you swapped buffers
mid-refresh).

**Vsync** makes `SDL_RenderPresent` block until the monitor is ready
for a new frame. No tearing, and your render loop is naturally capped
at the refresh rate. The trade-off: if you can't hit 60 FPS, vsync
drops you to 30 (every second refresh), then 20, etc., in steps.

We have vsync on by default. `--no-vsync` turns it off for benchmarking
or when you want unlocked frame rate during debugging.

### 0.6 Streaming vs static textures

When you create an `SDL_Texture`, you pick how it'll be used:

- **`SDL_TEXTUREACCESS_STATIC`**: you'll upload a pixel buffer once
  (via `SDL_UpdateTexture`), then draw from it many times without
  modifying. Best perf. Use for terrain chunks, sprites, loaded
  images.
- **`SDL_TEXTUREACCESS_STREAMING`**: you'll update the pixels
  frequently (via `SDL_LockTexture` or `SDL_UpdateTexture`). The
  driver keeps the texture in memory that's easier to write to. Use
  for our UI canvas (updated every frame) and for anything with
  live-CPU-generated content like video playback.
- **`SDL_TEXTUREACCESS_TARGET`**: can be used as a render target.
  For render-to-texture effects. Not needed yet.

A mismatch between access mode and usage pattern wastes memory or
perf. For each texture we create, the answer is clear:
- canvas_tex → STREAMING (we upload every frame)
- terrain chunk textures → STATIC (upload once at map load)

### 0.7 Letterboxing at the GPU level is free

You already wrote this without realizing how cheap it is:

```c
SDL_Rect dst = { offset_x, offset_y, scaled_w, scaled_h };
SDL_RenderCopy(renderer, canvas_tex, NULL, &dst);
```

One draw call. The GPU applies a scale transform per pixel during the
copy. It doesn't matter whether `scaled_w` is 640 or 1920. The draw
is ~constant time in GPU units. This is why terrain at native
resolution isn't expensive: we're doing one GPU draw per ~50-chunk
viewport, and each draw is just a textured rect.

Contrast with the old approach of drawing terrain into a 640×480 CPU
surface and scaling that surface up: we'd do 640×480 = 307K pixel
copies on the CPU, then another 1920×1080 = 2M pixel copies to scale.
That's ~10× the work for ~0× the benefit.

---

## Part 1: The current pipeline

Let's trace one frame from start to finish. Open these files
side-by-side as you read:

- `src/main.c`: the outer loop
- `src/render/ui.c`: the canvas
- `src/platform/sdl2_platform.c`: the present path
- any `_Tick` function (try `src/ui/ingame.c`, it's short)

Here's what happens per frame today:

```
main loop iteration:
  1. TAK_Platform_PumpEvents(platform)
       - reads SDL events
       - on SDL_WINDOWEVENT_SIZE_CHANGED, recomputes scale+offset
       - returns 0 (quit) or 1 (keep going)

  2. Timer_Update(&timer)
       - computes frame_dt since last update

  3. <current-state>_Tick(platform, frame_dt):
       a. read input (keys, mouse)
       b. update state (menu hover, camera, etc.)
       c. composite this frame into the 640×480 g_offscreen surface
          (SDL_FillRect, Blit_RGBA, Font_DrawString, Bink frame blit...)
       d. UI_Present(platform)
           → TAK_Platform_UpdateCanvas(platform, g_offscreen)
              → SDL_UpdateTexture(canvas_tex, pixels), uploads to GPU
       e. return next_state

  4. TAK_Platform_Present(platform):
       a. SDL_SetRenderDrawColor(0, 0, 0, 255)
       b. SDL_RenderClear(renderer)            ← wipes window to black
       c. SDL_RenderCopy(canvas_tex, NULL, dst_rect)  ← letterbox blit
       d. SDL_RenderPresent(renderer)          ← show it
```

One draw call per frame. One render target (the window). The canvas
always covers the centre rect of the window, and the letterbox bars stay
black (from the clear).

**What you can't do today:** anything that wants to live *behind* the
canvas. There's no opportunity between steps 4a (clear) and 4c (canvas
blit) for the game to stamp other stuff onto the window. Terrain needs
that gap to exist. That's Part 3's whole job.

---

## Part 2: The plan from here to terrain on screen

Five parts, each building on the last:

```
Part 3  TAK_GPU abstraction     ← the "gap between clear and canvas" lives here
      ├── opaque GPU_Texture handle
      ├── GPU_UploadRGBA / FreeTexture
      ├── GPU_DrawToWindow
      ├── FrameBegin split from Present
      └── canvas transparency (blend mode + per-screen clear)

Part 4  TNT chunk-records parse  ← "which chunks does this map want?"
      └── TNT_Load populates TNTFile.chunk_records

Part 5  JPG decode               ← "turn bytes into pixels"
      └── JPG_DecodeRGBA via libavcodec MJPEG

Part 6  Chunk loader             ← "do the upload work during loading"
      └── Terrain_LoadGrid, budget N chunks per Loading_Tick

Part 7  Terrain_DrawGrid         ← "draw the visible chunks each frame"
      └── culling + draw-call loop using the tools Part 3 gave us
```

By the end of Part 3 you have the plumbing. By the end of Part 7 you see
the map on screen. Nothing in Parts 4, 5 and 6 draws anything new. They
prepare the data for Part 7 to consume.

Each part is a single commit point. Don't start the next one
until the previous one builds clean, tests pass, and the game still
launches. Small diffs = small debugging surface.

---

## Part 3: the `TAK_GPU` abstraction

### Goal (one sentence)

Give the game a way to draw textured rectangles directly onto the
window's render target at any position and size in window pixels.

### What you build

Three things, plus one behavioural change:

1. An opaque `GPU_Texture` handle type.
2. Functions to create (`GPU_UploadRGBA`), free (`GPU_FreeTexture`),
   measure (`GPU_TextureSize`), and draw (`GPU_DrawToWindow`) them.
3. A new `TAK_Platform_FrameBegin` function, and a modified
   `TAK_Platform_Present` that no longer clears.
4. Canvas transparency: `canvas_tex` gets `SDL_BLENDMODE_BLEND`, and
   the in-game screen clears its canvas to alpha-0 instead of opaque
   black.

After these four changes, a terrain chunk texture drawn inside the
new "gap" will appear on screen, behind the (now transparent) canvas.

### Tasks

**Task 3.1: Read and trace (no code).** Open the files from Part 1,
walk through a frame, and write a ~15-line description in your own
words of the current pipeline. The exact level of detail I wrote above.
Don't skip. Your mental model has to be clear before you refactor.

**Verify:** show me your trace. I'll call out anything missing.

**Task 3.2: Design the API, header only.** Create
`include/tak_gpu.h`. Write function declarations with doc comments,
no `.c` yet. Your goal is for the header to read like documentation:
someone who's never seen the implementation should understand what
each function does from the comment above it.

Shape:

```c
typedef struct GPU_Texture GPU_Texture;

/* Upload an RGBA32 pixel buffer to the GPU. pixels is `w * h` uint32s,
 * packed row-major, no pitch. Returns NULL on failure. Caller owns the
 * returned handle and must release via GPU_FreeTexture. */
GPU_Texture *GPU_UploadRGBA(TAK_Platform *plat,
                             const uint32_t *pixels, int w, int h);

/* Release a texture. Safe to call with NULL. */
void GPU_FreeTexture(TAK_Platform *plat, GPU_Texture *tex);

/* Introspection. Returns 0 on success, -1 on null tex. */
int GPU_TextureSize(const GPU_Texture *tex, int *out_w, int *out_h);

/* Stamp a texture into the window render target. src == NULL means
 * "entire texture"; dst == NULL means "stretch to fill the window".
 * Must be called after TAK_Platform_FrameBegin and before
 * TAK_Platform_Present on the same frame. Coordinates in dst are
 * window pixels, not canvas pixels. */
void GPU_DrawToWindow(TAK_Platform *plat, const GPU_Texture *tex,
                       const SDL_Rect *src, const SDL_Rect *dst);
```

Decide: is the `TAK_Platform*` parameter the right way to pass the
renderer around? (Yes. The platform owns the SDL_Renderer and should
stay the authoritative handle. Don't introduce a globals shortcut.)

**Verify:** send me the header. We'll agree on the surface before you
implement.

**Task 3.3: Implement upload/free/size.** Create `src/render/tak_gpu.c`.
The `GPU_Texture` struct goes in this file (full definition). The
header only forward-declares it, so outside code can't peek inside.

```c
struct GPU_Texture {
    SDL_Texture *tex;
    int          w, h;
};
```

- `GPU_UploadRGBA` → `tak_malloc(sizeof(GPU_Texture))`,
  `SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
  SDL_TEXTUREACCESS_STATIC, w, h)`, `SDL_UpdateTexture(tex, NULL,
  pixels, w * 4)`. Store w/h. On any failure, clean up what succeeded
  and return NULL.
- `GPU_FreeTexture` → null-safe, destroy the SDL_Texture, free the
  handle.
- `GPU_TextureSize` → null-safe, copy out fields.

Add the new file to `src/CMakeLists.txt` under `TAK_RENDER_SOURCES`.

**Verify:** build clean. No test yet. We'll test in 3.6 by actually
drawing something.

**Task 3.4: Split `Present`, introduce `FrameBegin`.** The structural
change. In `sdl2_platform.c`:

```c
void TAK_Platform_FrameBegin(TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    SDL_SetRenderDrawColor(plat->renderer, 0, 0, 0, 255);
    SDL_RenderClear(plat->renderer);
}
```

Remove the `SetRenderDrawColor` + `RenderClear` lines from
`TAK_Platform_Present`. They now live in `FrameBegin`.

In `main.c`, add `TAK_Platform_FrameBegin(&platform);` at the top of
each loop iteration, before the state switch.

Also declare `TAK_Platform_FrameBegin` in `tak_platform.h`.

**Verify:** build, run. The game should look exactly the same as
before, since this is a pure refactor. If anything changes visually, either
the clear moved wrong or the present moved wrong.

**Task 3.5: Implement `GPU_DrawToWindow`.** Thin wrapper:

```c
void GPU_DrawToWindow(TAK_Platform *plat, const GPU_Texture *tex,
                      const SDL_Rect *src, const SDL_Rect *dst) {
    if (!plat || !plat->renderer || !tex || !tex->tex) return;
    SDL_RenderCopy(plat->renderer, tex->tex, src, dst);
}
```

That's it. All the power comes from `SDL_RenderCopy`, and we're just
hiding the pointer chase.

**Verify:** call it from 3.6.

**Task 3.6: Smoke test with a gradient.** Prove the whole thing works
before touching terrain. In `src/ui/ingame.c`:

1. Add `GPU_Texture *test_tex;` to the static `ig` struct.
2. In `InGame_Init`, allocate a 256×256 RGBA buffer, fill it with a
   visible gradient (e.g. `px[y*256+x] = (x << 0) | (y << 8) |
   (128 << 16) | (255 << 24)`), upload via `GPU_UploadRGBA`, store
   the handle on `ig`, free the CPU buffer.
3. In `InGame_Tick`, *before* `Terrain_Render`, call
   `GPU_DrawToWindow(platform, ig.test_tex, NULL, NULL)`, which
   stretches the gradient to fill the entire window.
4. Change the `SDL_FillRect(off, NULL, SDL_MapRGBA(off->format, 0,
   0, 0, 255))` at the top of `InGame_Tick` to use alpha `0`
   instead of `255`. Now the canvas starts each frame fully
   transparent.
5. In `TAK_Platform_Init`, add `SDL_SetTextureBlendMode(plat->canvas_tex,
   SDL_BLENDMODE_BLEND);` after the canvas texture is created.
6. In `InGame_Shutdown`, `GPU_FreeTexture(platform, ig.test_tex);`.
   (Careful: `InGame_Shutdown` takes no args, but the platform
   still exists. Either add a platform arg to the shutdown signature
   or call the free in main.c before `TAK_Platform_Shutdown`. The
   cleanest fix is the platform arg.)

**Verify:** Play → Skirmish → pick any map → Launch. You should see
a colour gradient covering the entire window, with the camera's dark-
blue `Terrain_Render` stub invisible (or visible through the still-
transparent bits of the canvas). Press ESC to exit. Take a screenshot.
That's the proof this part works.

**Gotchas here:**

- Forgot to set `BLENDMODE_BLEND` on canvas_tex → canvas still covers
  everything, you see black + canvas, no gradient.
- Forgot to change the canvas clear to alpha-0 → same result.
- Passed `dst` in canvas coords (e.g. `{0,0,640,480}`) → gradient
  draws in the top-left corner at 640×480, not filling the window.
- Freed the CPU pixel buffer before calling `GPU_UploadRGBA` →
  SDL_UpdateTexture reads garbage. Free it *after*.

**Task 3.7: Verify software backend.** `tak-re --sw-renderer`, go
in-game, confirm gradient still shows and blends correctly. If
anything differs from the hardware backend, let me know the symptom.
SW mode should be behaviourally identical, just slower.

**Task 3.8: Pull out the demo.** Remove the test gradient code from
`ingame.c`. Leave the alpha-0 canvas clear in place (the real terrain
goes behind it). Keep `GPU_DrawToWindow`, `GPU_UploadRGBA` et al.
Those are the deliverables.

### Ship-it criteria

- `tak-re` builds, all existing tests pass.
- Alpha-0 canvas clear in ingame.c works, with no visual regression,
  because the stub fill still writes opaque dark blue *to the canvas*,
  which now blends to opaque anyway.
- `GPU_DrawToWindow` proven by the smoke test (you've seen it work,
  then you pulled it).
- Software backend verified.

---

## Part 4: parsing the TNT chunk records

### Goal

Teach `TNT_Load` to populate a `chunk_records` array on the `TNTFile`
struct, so later parts can iterate it without re-parsing the
file.

### Background (the shape of the data)

From the TNT format recon:

- TNT header has a 32-bit pointer at offset 0x20.
- At that pointer lives an array of `chunks_w × chunks_h` 8-byte
  records, where `chunks_w = width_tiles / 16` and `chunks_h =
  height_tiles / 16`.
- Each record is two uint32s: `{ chunk_id, terrain_type }`.
  - `chunk_id` names a JPEG in `terrain.hpi`, at `terrain/<%08x>.jpg`.
  - `terrain_type` is categorical (grass / rock / water / …) used
    for pathfinding. Ignore it in rendering.

So the job is: read offset 0x20, bounds-check it, and hand the caller
a pointer + count.

### Tasks

**Task 4.1: Extend `TNTFile`.** In `include/tak_tnt.h`:

```c
typedef struct TNTChunkRecord {
    uint32_t chunk_id;
    uint32_t terrain_type;
} TNTChunkRecord;

typedef struct TNTFile {
    /* …existing fields… */

    /* Chunk-records array lives at TNT header offset 0x20. The count
     * is (width_tiles/16) * (height_tiles/16); each chunk covers a
     * 512×512-px region of the map (= 16×16 32-px tiles). NULL if
     * the bounds check failed in TNT_Load. */
    const TNTChunkRecord *chunk_records;
    int                   chunks_w;
    int                   chunks_h;
} TNTFile;
```

Keep `chunk_records` as `const TNTChunkRecord *` pointing into the
already-loaded `tnt->raw` buffer. The data is already in memory, so no
copy is needed. Same pattern as `tile_map` and `feature_layer`.

**Task 4.2: Parse in `TNT_Load`.** After the existing offset-0x14
block:

```c
int cw = W / 16;
int ch = H / 16;
if (cw > 0 && ch > 0) {
    uint32_t off_chunks = *(uint32_t*)(tnt_buffer + 0x20);
    size_t   sz_chunks  = (size_t)cw * (size_t)ch * sizeof(TNTChunkRecord);
    if ((size_t)off_chunks + sz_chunks <= tnt_size) {
        out->chunk_records = (const TNTChunkRecord *)(tnt_buffer + off_chunks);
        out->chunks_w      = cw;
        out->chunks_h      = ch;
    } else {
        fprintf(stderr, "TNT: %s chunk_records out of range "
                        "(off=0x%x, need %zu)\n",
                path, off_chunks, sz_chunks);
    }
}
```

Follow the same defensive pattern the existing pointers use.

**Task 4.3: Regression tests.** In `src/game/test_tnt.c`, extend the
per-map layout tests:

```c
static void assert_layout(const char *vfs_path, int W, int H, int tile_count,
                          int expected_chunks_w, int expected_chunks_h) {
    /* …existing asserts… */
    ASSERT_EQ_INT(expected_chunks_w, tnt.chunks_w);
    ASSERT_EQ_INT(expected_chunks_h, tnt.chunks_h);
    ASSERT(tnt.chunk_records != NULL);
    /* First record's chunk_id should be a real JPG in terrain.hpi.
     * Rough check: the recon tool already proved 100% match on these
     * maps, so we just assert chunk_id != 0xFFFFFFFF here. */
    ASSERT(tnt.chunk_records[0].chunk_id != 0xFFFFFFFF);
}
```

Update the per-map callers (Ground War → 10×10, Angvir → 8×8,
CASTLE → 34×34, Muntil → 22×14). Rerun `test_tnt`. All 23 should
still pass, plus the new assertions.

### Ship-it criteria

- All `test_tnt` tests pass.
- No runtime changes visible, since this part is pure data parsing.
- `loading.c`'s log line `"LS_LOAD_TNT: loaded %s"` could be extended
  to print `chunks_w × chunks_h` as a sanity indicator.

---

## Part 5: decoding JPEGs

### Goal

Take the raw bytes of a `terrain/XXXXXXXX.jpg` file (read via VFS),
decode them to an RGBA32 pixel buffer, return (pixels, w, h). That
buffer is the input to `GPU_UploadRGBA` in Part 6.

### Background

**JPEG** is a compressed image format. It's lossy, throwing away
high-frequency detail to save space, but that's invisible for terrain
photos. Browsers decode millions of JPEGs a day, so this is a
well-trodden path.

**libavcodec** (part of FFmpeg) is the decoder we already link for
Bink video playback. Its *MJPEG* codec handles standalone JPEGs, on the
same codepath, just a single frame.

The decoder outputs in a pixel format of its choice (usually
`AV_PIX_FMT_YUVJ420P` or `AV_PIX_FMT_YUV444P` depending on the JPEG's
subsampling). We need RGBA32. **libswscale** (also already linked) does
format conversion via `sws_getContext` + `sws_scale`.

### Tasks

**Task 5.1: Design the API.** `include/tak_jpg.h`:

```c
/* Decode a baseline JPEG to a heap-allocated RGBA32 buffer.
 *   jpg_data / jpg_size: the raw JPEG bytes (e.g. from VFS_ReadFile).
 *   out_pixels:          set to tak_malloc'd uint32_t * (caller frees).
 *   out_w, out_h:        set to decoded image dimensions.
 * Returns 0 on success, -1 on any failure (bad JPEG, alloc fail,
 * libav error). On failure *out_pixels is NULL. */
int JPG_DecodeRGBA(const uint8_t *jpg_data, size_t jpg_size,
                    uint32_t **out_pixels, int *out_w, int *out_h);
```

No state and no context, just a stateless per-call function. Simpler to test.

**Task 5.2: Implement.** New file `src/render/jpg_decode.c`. The
shape:

```c
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

int JPG_DecodeRGBA(const uint8_t *jpg, size_t size,
                    uint32_t **out_px, int *out_w, int *out_h) {
    /* 1. Find the MJPEG codec. */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return -1;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;

    /* 2. Open the codec. */
    if (avcodec_open2(ctx, codec, NULL) < 0) { /* cleanup */ return -1; }

    /* 3. Build an AVPacket wrapping our bytes (no copy). */
    AVPacket *pkt = av_packet_alloc();
    pkt->data = (uint8_t *)jpg;   /* MJPEG decoder won't modify */
    pkt->size = (int)size;

    /* 4. Decode: send packet, receive frame. */
    AVFrame *frame = av_frame_alloc();
    if (avcodec_send_packet(ctx, pkt) < 0 ||
        avcodec_receive_frame(ctx, frame) < 0) {
        /* cleanup */ return -1;
    }

    int w = frame->width, h = frame->height;

    /* 5. Convert frame->format to RGBA32 via sws. */
    struct SwsContext *sws = sws_getContext(
        w, h, frame->format,
        w, h, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { /* cleanup */ return -1; }

    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * h * sizeof(uint32_t));
    uint8_t *dst_data[1]   = { (uint8_t *)px };
    int      dst_stride[1] = { w * 4 };
    sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, h, dst_data, dst_stride);

    /* 6. Success: publish outputs. Cleanup sws, packet, frame, ctx. */
    *out_px = px; *out_w = w; *out_h = h;
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return 0;
}
```

That's the skeleton. The cleanups on failure paths are where bugs
hide. Either use a `goto cleanup` pattern or be very methodical with
every early return. I recommend the `goto cleanup` pattern, which is the
idiomatic libav style.

Add `src/render/jpg_decode.c` to `TAK_RENDER_SOURCES` in the CMake
file. No new link deps, since libav* is already linked.

**Task 5.3: Test with a PPM dump.** Write a tiny test target
`test_jpg` that:

1. `VFS_Init`
2. For 5 known chunk IDs (pick from the recon log), `VFS_ReadFile` the
   JPG.
3. `JPG_DecodeRGBA`.
4. Write out as PPM (P6 ASCII header + raw RGB bytes). Open in an
   image viewer and eyeball that it's a real terrain tile.

PPM is a trivial format, no library needed:

```c
fprintf(f, "P6\n%d %d\n255\n", w, h);
for (i = 0; i < w*h; i++) {
    uint32_t p = pixels[i];
    fwrite(&p, 1, 3, f);   /* R,G,B, the low 3 bytes. A is byte 4 */
}
```

Add assertions: w == 512, h == 512, at least 1000 distinct colours in
the buffer (catches a decoder that's outputting all-zero or all-one).

### Ship-it criteria

- `test_jpg` passes (5/5 known chunks decode to 512×512 with colour
  variety).
- PPM dumps visually look like terrain textures.

---

## Part 6: loading chunks into textures

### Goal

During the existing loading state machine, walk the TNT's
`chunk_records`, decode+upload each one, and store the `GPU_Texture`
handles alongside the records so Part 7 can draw them.

### Background

We have three options for when to do this work:

1. **All at load time.** Simple, but Ground War is ~100 chunks
   ≈ 500 ms of work, CASTLE is 1156 ≈ 5 s. That's a long hitch during
   the loading screen, but it's the loading screen.
2. **Lazy, on first visibility.** Decode a chunk the first frame it
   becomes visible. Spreads cost → smooth loading, occasional scroll
   hitches.
3. **Hybrid.** Decode all chunks during loading but a few per frame,
   so the loading screen progress bar + Bink video stay responsive.

**Go with (3).** It's what the existing loading state machine is
already shaped for (one step per Tick). The new step `LS_LOAD_CHUNKS`
internally tracks "next chunk to process" and does N per tick.

### Tasks

**Task 6.1: Extend `TerrainGrid`.** In
`include/tak_terrain.h` or a new `tak_terrain_chunks.h`:

```c
typedef struct TerrainChunkState {
    uint32_t      chunk_id;
    uint32_t      terrain_type;
    GPU_Texture  *tex;      /* NULL until decoded+uploaded */
} TerrainChunkState;

typedef struct TerrainGrid {
    int                 chunks_w;
    int                 chunks_h;
    TerrainChunkState  *cells;   /* chunks_w * chunks_h */
} TerrainGrid;
```

Allocation goes in `TerrainGrid_Init` (new function), freeing in
`TerrainGrid_Free`.

**Task 6.2: Decode/upload helpers.** In
`src/render/terrain_chunks.c`:

```c
/* Decode + upload one chunk. On success, grid->cells[cell_idx].tex is
 * set. Safe to call on an already-loaded cell (no-op then).*/
int TerrainGrid_LoadChunk(TerrainGrid *grid, int cell_idx,
                           TAK_Platform *plat);
```

Workflow inside:

1. Look at `grid->cells[cell_idx]`.
2. If `tex != NULL`, it's already done, so return 0.
3. Build path `terrain/<chunk_id %08x>.jpg`.
4. `VFS_ReadFile` → bytes.
5. `JPG_DecodeRGBA` → RGBA pixels.
6. `GPU_UploadRGBA` → texture handle.
7. Store the handle. Free the JPG bytes and pixel buffer.

Each of these steps can fail. On failure leave `tex = NULL` and log.
The terrain draw can fall back to a solid colour for missing chunks
(a Part 7 concern).

**Task 6.3: Wire into the loading state machine.** Add a new step
between `LS_LOAD_TNT` and `LS_INIT_WORLD` in `src/ui/loading.c`:

```c
case LS_LOAD_CHUNKS: {
    Loading_SetStatus("Loading terrain chunks...");
    GameWorld *world = World_Get();
    if (!world || !world->tnt.chunk_records) {
        ld.step = LS_INIT_WORLD;
        break;
    }

    /* Lazy-init the grid on first entry. */
    if (!world->grid.cells) {
        TerrainGrid_Init(&world->grid, &world->tnt);
    }

    /* Process N chunks per tick. N=5 keeps the loading screen
     * smooth on commodity hardware; tune empirically. */
    const int BUDGET = 5;
    int total = world->grid.chunks_w * world->grid.chunks_h;
    int done = 0;
    for (int k = 0; k < BUDGET && ld.next_chunk < total; k++) {
        TerrainGrid_LoadChunk(&world->grid, ld.next_chunk++, platform);
        done++;
    }
    Loading_SetProgress(0.55f + 0.20f * ((float)ld.next_chunk / total));

    if (ld.next_chunk >= total) ld.step = LS_INIT_WORLD;
    break;
}
```

Add `next_chunk` to the `ld` state (reset to 0 each map load).

**Task 6.4: Cleanup.** `World_End` (or wherever the map teardown
lives) must call `TerrainGrid_Free`, which iterates cells, calls
`GPU_FreeTexture` on each non-null handle, and frees the array. Leaking GPU textures across map
loads will eventually exhaust VRAM.

### Ship-it criteria

- Loading screen runs for noticeably longer on big maps (CASTLE's 1156
  chunks at 5 per tick = ~230 ticks ≈ 4 s with vsync on, which is
  fine).
- stderr logs show chunk uploads progressing, no errors.
- Still no terrain on screen (that's Part 7's job), but the in-game screen
  has the data it needs.
- Memory isn't leaking across repeated map loads (load a map, quit
  to menu, load another, quit. RSS should plateau, not grow).

---

## Part 7: the actual draw (the payoff)

### Goal

Every frame, figure out which chunks overlap the viewport, and draw
them at native resolution via `GPU_DrawToWindow`. End state: the map
shows on screen, the camera scrolls, and you can see terrain pass by
as you move.

### Coordinate systems, the thing to internalize

Four coordinate spaces exist. Get them clear or you'll be debugging
"why is my camera offset doubled" for hours.

```
  WORLD coords:          pixels of the whole map.
                         Range: [0, map_pixels_w) × [0, map_pixels_h).
                         Ground War: [0, 5120) × [0, 5120).
                         cam_x/cam_y are in world coords.

  CHUNK coords:          which 512×512 tile. Integer.
                         Range: [0, chunks_w) × [0, chunks_h).
                         Ground War: [0, 10) × [0, 10).
                         world_x = chunk_x * 512; chunk_x = world_x / 512.

  VIEWPORT coords:       pixels relative to the top-left of what
                         the camera can see. Same size as the window.
                         Range: [0, viewport_w) × [0, viewport_h).
                         viewport_x = world_x - cam_x.

  WINDOW coords:         pixels of the SDL window.
                         Range: [0, window_w) × [0, window_h).
                         For terrain, drawn straight to the window
                         after Part 3, this equals viewport coords
                         exactly, because terrain renders at native
                         resolution.
```

**The key mapping:** for a chunk at chunk-coords `(cx, cy)`, the
window dst_rect is:

```
window_x = cx * 512 - cam_x
window_y = cy * 512 - cam_y
window_w = 512
window_h = 512
```

### Tasks

**Task 7.1: Culling pass.** Visiting every chunk is wasteful on
CASTLE (1156 chunks, ~50 visible). Compute the visible chunk range
from the camera:

```c
int first_cx = cam_x / 512;
int first_cy = cam_y / 512;
int last_cx  = (cam_x + viewport_w - 1) / 512;
int last_cy  = (cam_y + viewport_h - 1) / 512;

/* Clamp to grid bounds. */
if (first_cx < 0) first_cx = 0;
if (first_cy < 0) first_cy = 0;
if (last_cx >= chunks_w) last_cx = chunks_w - 1;
if (last_cy >= chunks_h) last_cy = chunks_h - 1;
```

That's `(last_cx - first_cx + 1) × (last_cy - first_cy + 1)` chunks
to draw. At 1920×1080 that's ≤ 5×3 = 15 chunks. The other 1141 we
skip.

**Task 7.2: Replace `Terrain_Render`.** In `src/render/terrain.c`:

```c
void Terrain_Render(const GameWorld *world, TAK_Platform *plat) {
    if (!world || !plat) return;
    const TerrainGrid *g = &world->grid;
    if (!g->cells) return;

    /* Compute visible chunk range (Task 7.1). */
    int first_cx, first_cy, last_cx, last_cy;
    /* …clamped as above… */

    for (int cy = first_cy; cy <= last_cy; cy++) {
        for (int cx = first_cx; cx <= last_cx; cx++) {
            const TerrainChunkState *c = &g->cells[cy * g->chunks_w + cx];
            if (!c->tex) continue;  /* not loaded; skip */

            SDL_Rect dst = {
                .x = cx * 512 - world->cam_x,
                .y = cy * 512 - world->cam_y,
                .w = 512,
                .h = 512,
            };
            GPU_DrawToWindow(plat, c->tex, NULL, &dst);
        }
    }
}
```

Note the signature change: `Terrain_Render` now takes `TAK_Platform*`
instead of `SDL_Surface* dst`, because we're drawing to the window, not
a CPU surface. Update the one caller in `src/ui/ingame.c`.

**Task 7.3: Reconcile viewport sizing.** Currently
`world->viewport_w/h = 640, 480` (from loading.c). Terrain now draws
at window resolution, so viewport must track the window:

```c
world->viewport_w = platform->window_w;
world->viewport_h = platform->window_h;
```

Do this in `InGame_Init` (or every tick, since the window can
resize). Don't forget to re-clamp `cam_x/cam_y` against the new
viewport size after resize. Otherwise the camera can drift off-map.

**Task 7.4: Remove the canvas stub.** The old `Terrain_Render` fill
into the 640×480 canvas is now obsolete (terrain draws to the window
directly, and the canvas is transparent in-game). Delete that FillRect from
`InGame_Tick`.

### Ship-it criteria

- **Map visible on screen at native resolution.** Not blurry. Not
  640×480-scaled. Native.
- WASD / arrow keys scroll the camera, terrain updates each frame.
- No frame hitches at 60 FPS on Ground War-class maps.
- Missing chunks (shouldn't happen for shipped maps, but be
  defensive) show as background-coloured gaps, not crashes.
- **Take a screenshot.** This is the part that was worth the effort.

---

## Part 8: what comes after, brief sketches

### SW fallback parity

Run everything under `--sw-renderer`. If any of Parts 3 to 7 used features
that only work on hardware renderers, they surface here. Likely
culprits: blend modes that SW doesn't support, specific pixel formats.
Fix by adjusting to the lowest common denominator. Both backends
should be behaviourally identical, just at different speeds.

### Camera config

Pull the `scroll_px_per_sec`/`boost`/`edge_margin` values out of the
hardcoded constants in `ingame.c` into a `CameraConfig` struct in
`GameWorld`. Load defaults, expose CLI overrides or a config file.
Add mouse-edge scroll and MMB-drag pan while you're there. Both are
small loops of input handling, with no new subsystems.

### Cache + eviction

Only needed if the big maps (CASTLE = 1.1 GB of terrain textures at
full resolution) strain memory. Build a process-wide LRU keyed by
`chunk_id`, cap it at a configurable MB budget, evict cold chunks and
re-decode on demand. This is a "if profiling shows a problem" task.
Skip it until it does.

---

## Appendix: glossary quick reference

- **blit**: copy a block of pixels from one buffer to another. CPU
  operation, usually with per-pixel transparency logic.
- **draw call**: one `SDL_RenderCopy` instruction to the GPU.
- **render target**: the image currently being drawn into (window or
  texture).
- **framebuffer**: the render target for the display. Synonym.
- **vsync**: wait for monitor refresh before presenting. Prevents
  tearing, caps FPS at refresh rate.
- **alpha**: the A channel of RGBA. 0 = transparent, 255 = opaque.
- **blend mode**: arithmetic for combining src and dst pixels.
- **texture**: image data uploaded to the GPU.
- **surface**: image data in CPU memory with format metadata.
- **pitch / stride**: bytes per row of a pixel buffer. Usually
  `width * bytes_per_pixel` for packed buffers but can be larger due
  to alignment.
- **chunk**: our unit of terrain texture. 512×512 pixels, one JPEG
  per chunk in `terrain.hpi`.
- **viewport**: the rectangle of the world the camera is currently
  showing.
- **culling**: skipping work (draws, updates) for things outside the
  viewport.
- **letterbox / pillarbox**: black bars top/bottom (letter) or
  left/right (pillar) when the content's aspect ratio doesn't match
  the container.
- **LRU**: least-recently-used. Eviction policy for caches.

---

Call me in when you start. I'll read your diffs and call out anything
off. You drive.
