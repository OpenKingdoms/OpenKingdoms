/*
 * tak_gl3d.h -- the 3D view's renderer, the one place that touches GL.
 *
 * Everything the 3D view draws goes through this small API: a frame,
 * a camera, textures, meshes and three kinds of draw. The GL calls
 * live in gl3d.c and nowhere else, so moving to another graphics API
 * later is a change to one file. The feature level is the OpenGL ES
 * 2.0 subset of ES 3.0, which is what WebGL exposes and what SDL's
 * desktop GL context gives us in a compatibility profile.
 *
 * The SDL renderer keeps drawing the HUD through the same context, so
 * a frame here flushes SDL's queue first and puts back every piece of
 * GL state SDL caches before it returns.
 */
#ifndef TAK_GL3D_H
#define TAK_GL3D_H

#include <SDL.h>
#include <stdint.h>

typedef struct GL3D_Texture GL3D_Texture;
typedef struct GL3D_Mesh    GL3D_Mesh;

/* Vertex layouts, floats per vertex in brackets.
 *   TERRAIN: x y z, nx ny nz, u v                          (8)
 *   MODEL:   x y z, nx ny nz, u v, r g b a, node           (13)
 *   SPRITE:  x y z, u v, r g b a                           (9) */
typedef enum GL3D_Layout {
    GL3D_LAYOUT_TERRAIN = 0,
    GL3D_LAYOUT_MODEL   = 1,
    GL3D_LAYOUT_SPRITE  = 2,
    /* MODEL with a tangent after the node: what an artist's model
     * with a normal map draws through. */
    GL3D_LAYOUT_MODEL_PBR = 3
} GL3D_Layout;

int GL3D_LayoutFloats(GL3D_Layout layout);

/* One draw range of a model: a texture (an SDL texture the atlas
 * already uploaded, or one of ours, or neither for flat colour) and
 * the index range whose vertices belong to nodes node_lo..node_hi. */
typedef struct GL3D_ModelBatch {
    SDL_Texture  *sdl_tex;
    GL3D_Texture *tex;
    int           first_index;
    int           index_count;
    int           node_lo;
    int           node_hi;     /* inclusive */
    /* How the surface takes light, for a mesh in the MODEL_PBR
     * layout. A 3DO model leaves all of this zero. */
    GL3D_Texture *normal_tex;  /* NULL for none */
    GL3D_Texture *mr_tex;      /* rough in green, metal in blue, or NULL */
    GL3D_Texture *emissive_tex;
    float         emissive[3];
    float         metallic;
    float         roughness;
    float         normal_scale;
    float         alpha_cutoff; /* fainter fragments are dropped, 0 for none */
    uint8_t       blend;        /* drawn over what is there, not depth written */
    uint8_t       double_sided;
} GL3D_ModelBatch;

/* Bring up GL on the renderer's context. Returns 0, or -1 when the
 * renderer has no GL context or it lacks shaders. */
int  GL3D_Init(SDL_Window *window, SDL_Renderer *renderer);
void GL3D_Shutdown(void);
int  GL3D_Available(void);

/* Nodes one model draw can carry, from the context's uniform budget. */
int  GL3D_MaxNodesPerDraw(void);

/* Flush the SDL renderer, remember its GL state, and open the depth
 * buffer over the viewport (window pixels, y down). The sky colour
 * clears the viewport. EndFrame puts SDL's state back. */
void GL3D_BeginFrame(SDL_Renderer *renderer, int win_w, int win_h,
                     const SDL_Rect *viewport, const float sky[3]);
void GL3D_EndFrame(void);

void GL3D_SetCamera(const float viewproj[16], const float eye[3],
                    const float light_dir[3]);
/* The fog lookup: one byte per cell over the whole map, 0 unexplored,
 * 128 explored, 255 in sight. NULL draws everything lit. */
void GL3D_SetFog(GL3D_Texture *fog, float map_w, float map_h);

GL3D_Texture *GL3D_UploadTextureRGBA(const uint32_t *rgba, int w, int h,
                                     int mipmap, int linear);
GL3D_Texture *GL3D_UploadTextureGrey(const uint8_t *grey, int w, int h);
void GL3D_UpdateTextureGrey(GL3D_Texture *tex, const uint8_t *grey, int w, int h);
void GL3D_FreeTexture(GL3D_Texture *tex);

GL3D_Mesh *GL3D_UploadMesh(GL3D_Layout layout, const float *verts, int vert_count,
                           const uint16_t *indices, int index_count);
void GL3D_FreeMesh(GL3D_Mesh *mesh);

/* Opaque terrain, one texture, one index range of a TERRAIN mesh. */
void GL3D_DrawTerrain(const GL3D_Mesh *mesh, GL3D_Texture *tex,
                      int first_index, int index_count);

/* A model: its MODEL mesh, a model matrix, twelve floats per node
 * (a row major 3x3 rotation then a translation, node local to model
 * space), which nodes are hidden, and its batches. alpha under one
 * blends the whole model. */
void GL3D_DrawModel(const GL3D_Mesh *mesh, const float model[16],
                    const float *node_xforms, const uint8_t *node_hidden,
                    int node_count, const GL3D_ModelBatch *batches,
                    int batch_count, float alpha);

/* Streamed SPRITE geometry: billboards with an alpha cut, or blended
 * translucent surfaces such as water with no depth write. */
void GL3D_DrawSprites(const float *verts, int vert_count,
                      const uint16_t *indices, int index_count,
                      GL3D_Texture *tex, int alpha_cut, int blend);

#endif /* TAK_GL3D_H */
