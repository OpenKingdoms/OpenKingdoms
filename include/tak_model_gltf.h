/*
 * tak_model_gltf.h -- an artist's glTF turned into a unit mesh.
 *
 * Pure: it reads a parsed glTF and writes a UnitMesh, and touches no
 * GPU and no world. The model store uploads the pictures and caches
 * the result. Kept apart so the checking below can be tested wherever
 * the tests run, with no game data and no graphics.
 */
#ifndef TAK_MODEL_GLTF_H
#define TAK_MODEL_GLTF_H

#include "tak_gltf.h"
#include "tak_unit.h"

/* A world pixel is this many of the TA units a UnitMesh holds. An
 * artist's model is authored a unit to the pixel. */
#define TA_UNITS_PER_PIXEL 65536.0f

/* What one batch of the mesh draws with. The store turns the picture
 * numbers into textures; the rest goes to the shader as it is. */
typedef struct GltfBatch {
    int     base_image;      /* index into the model's images, -1 for none */
    int     normal_image;
    int     mr_image;        /* metal in blue, rough in green */
    int     emissive_image;
    float   emissive[3];     /* light the surface gives off */
    float   metallic;
    float   roughness;
    float   normal_scale;
    float   alpha_cutoff;    /* a fragment fainter than this is dropped */
    uint8_t blend;           /* drawn after the solid parts, not depth written */
    uint8_t double_sided;
    uint8_t pulse;           /* its emission breathes */
} GltfBatch;

/* Builds a mesh from `g`, in the team's colour where a material asks
 * for it. `batches` takes what each batch draws with and must hold
 * UNIT_MESH_MAX_BATCHES. NULL when the model will not do, having said
 * why. */
UnitMesh *Gltf_ToUnitMesh(const GltfModel *g, const char *name,
                          uint32_t team_rgba, GltfBatch *batches);

/* Everything the code downstream takes on trust about a mesh: piece
 * order, index range, one piece to a triangle, batches that cover the
 * triangles in piece order. 0 when all of it holds. */
int Gltf_ValidateMesh(const UnitMesh *m, const char *name);

void Gltf_FreeUnitMesh(UnitMesh *m);

#endif /* TAK_MODEL_GLTF_H */
