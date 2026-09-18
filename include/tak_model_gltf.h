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

/* Builds a mesh from `g`, in the team's colour where a material asks
 * for it. `image_of_batch` takes the glTF image index each batch draws
 * with, or -1 where it draws none, and must hold UNIT_MESH_MAX_BATCHES.
 * NULL when the model will not do, having said why. */
UnitMesh *Gltf_ToUnitMesh(const GltfModel *g, const char *name,
                          uint32_t team_rgba, int *image_of_batch);

/* Everything the code downstream takes on trust about a mesh: piece
 * order, index range, one piece to a triangle, batches that cover the
 * triangles in piece order. 0 when all of it holds. */
int Gltf_ValidateMesh(const UnitMesh *m, const char *name);

void Gltf_FreeUnitMesh(UnitMesh *m);

#endif /* TAK_MODEL_GLTF_H */
