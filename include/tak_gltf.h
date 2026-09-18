/*
 * tak_gltf.h -- a reader for glTF 2.0 binary models.
 *
 * Reads a .glb into a neutral model: named nodes in a flat depth first
 * order, primitives of positions, texture coordinates and triangles,
 * and images decoded to RGBA. Geometry stays in the file's own units
 * and axes; whoever bakes it decides the scale.
 *
 * The files are art dropped in a folder rather than shipped data, so
 * every offset and count in them is treated as hostile.
 */
#ifndef TAK_GLTF_H
#define TAK_GLTF_H

#include <stdint.h>
#include <stddef.h>

#define GLTF_MAX_NODES   128
#define GLTF_MAX_PRIMS   256
#define GLTF_MAX_IMAGES   32
#define GLTF_NAME_MAX     32

typedef struct GltfImage {
    uint32_t *rgba;          /* w * h pixels, RGBA byte order, owned */
    int       w, h;
} GltfImage;

typedef struct GltfPrim {
    int       node;          /* index into nodes */
    int       image;         /* index into images, -1 for untextured */
    float     base_color[4]; /* the material's baseColorFactor */
    uint8_t   team_color;    /* its material is named teamcolor */
    int       vert_count;
    int       tri_count;
    float    *pos;           /* 3 * vert_count, node local, file units */
    float    *uv;            /* 2 * vert_count */
    uint16_t *idx;           /* 3 * tri_count, indexes this primitive */
} GltfPrim;

typedef struct GltfNode {
    char      name[GLTF_NAME_MAX];
    int       parent;        /* index, -1 for a root */
    float     offset[3];     /* translation from the parent, file units */
} GltfNode;

typedef struct GltfModel {
    GltfNode  nodes[GLTF_MAX_NODES];
    int       node_count;
    GltfPrim *prims;
    int       prim_count;
    GltfImage images[GLTF_MAX_IMAGES];
    int       image_count;
    /* extras.tak_scale at the file root, 1 when it says nothing. */
    float     scale_hint;
} GltfModel;

/* Reads a .glb through the VFS. 0 on success, and *out owns everything. */
int  Gltf_Load(GltfModel **out, const char *vfs_path);

/* The same from bytes already in hand. The bytes are not kept. */
int  Gltf_LoadFromMemory(GltfModel **out, const uint8_t *bytes, size_t size);

void Gltf_Free(GltfModel *m);

#endif /* TAK_GLTF_H */
