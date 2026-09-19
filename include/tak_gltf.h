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

/* How a surface takes light, as its material describes it. */
typedef struct GltfSurface {
    int     image;           /* base colour, index into images, -1 for none */
    int     normal_image;    /* a normal map, -1 for none */
    int     mr_image;        /* metal in blue, rough in green, -1 for none */
    int     emissive_image;  /* light the surface gives off, -1 for none */
    float   emissive[3];     /* emissiveFactor, 0 when it gives off none */
    float   base_color[4];   /* baseColorFactor */
    float   metallic;        /* metallicFactor */
    float   roughness;       /* roughnessFactor */
    float   normal_scale;    /* the normal map's strength */
    float   alpha_cutoff;    /* alphaMode MASK cuts here, 0 when it does not */
    uint8_t blend;           /* alphaMode BLEND: drawn after the solid parts */
    uint8_t double_sided;    /* both faces, rather than the front alone */
    uint8_t team_color;      /* its material is named teamcolor */
    uint8_t uv_set;          /* which TEXCOORD_n its pictures are laid by */
    uint8_t pulse;           /* named for its glow: its emission breathes */
} GltfSurface;

typedef struct GltfPrim {
    int         node;        /* index into nodes */
    GltfSurface surface;
    int         vert_count;
    int         tri_count;
    float      *pos;         /* 3 * vert_count, node local, file units */
    float      *nrm;         /* 3 * vert_count, NULL when the file has none */
    float      *tan;         /* 4 * vert_count, xyz and a sign, or NULL */
    float      *uv;          /* 2 * vert_count */
    uint16_t   *idx;         /* 3 * tri_count, indexes this primitive */
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
    /* extras.tak_scale at the file root, or failing that on a node,
     * which is where Blender puts an object's custom property. 1 when
     * nothing says. */
    float     scale_hint;
} GltfModel;

/* Reads a .glb through the VFS. 0 on success, and *out owns everything. */
int  Gltf_Load(GltfModel **out, const char *vfs_path);

/* The same from bytes already in hand. The bytes are not kept. */
int  Gltf_LoadFromMemory(GltfModel **out, const uint8_t *bytes, size_t size);

/* The same, with a say in whether the pictures are decoded. A model is
 * read once for each team colour and its pictures are the same every
 * time, so the second reading asks for geometry alone and leaves the
 * megabytes of decoding undone. */
#define GLTF_WITH_IMAGES    1
#define GLTF_GEOMETRY_ONLY  0
int  Gltf_LoadEx(GltfModel **out, const char *vfs_path, int with_images);
int  Gltf_LoadFromMemoryEx(GltfModel **out, const uint8_t *bytes, size_t size,
                           int with_images);

void Gltf_Free(GltfModel *m);

#endif /* TAK_GLTF_H */
