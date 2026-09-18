/*
 * test_gltf.c -- the glTF reader, against files built here.
 *
 * Every case assembles a .glb in memory, so this needs no game data
 * and runs everywhere CI runs. Half of it is the shapes a model can
 * take and half is the shapes a hostile or broken file can take,
 * because these files are art dropped in a folder rather than data we
 * ship.
 */
#include "test_framework.h"

#include "tak_gltf.h"
#include "tak_model_gltf.h"
#include "tak_crash.h"
#include "tak_memory.h"
#include "miniz.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 0.001f)

/* ── building a .glb ──────────────────────────────────────────────── */

static uint8_t *make_glb(const char *json, const uint8_t *bin, size_t bin_len,
                         size_t *out_size) {
    size_t jlen = strlen(json);
    size_t jpad = (4 - (jlen & 3)) & 3;
    size_t bpad = bin_len ? ((4 - (bin_len & 3)) & 3) : 0;
    size_t total = 12 + 8 + jlen + jpad + (bin_len ? 8 + bin_len + bpad : 0);
    uint8_t *b = (uint8_t *)tak_malloc(total);
    if (!b) return NULL;
    size_t at = 0;
    #define PUT32(v) do { uint32_t _v = (uint32_t)(v); b[at]=(uint8_t)_v; \
        b[at+1]=(uint8_t)(_v>>8); b[at+2]=(uint8_t)(_v>>16); \
        b[at+3]=(uint8_t)(_v>>24); at += 4; } while (0)
    PUT32(0x46546C67u);
    PUT32(2u);
    PUT32(total);
    PUT32(jlen + jpad);
    PUT32(0x4E4F534Au);
    memcpy(b + at, json, jlen);
    memset(b + at + jlen, ' ', jpad);
    at += jlen + jpad;
    if (bin_len) {
        PUT32(bin_len + bpad);
        PUT32(0x004E4942u);
        memcpy(b + at, bin, bin_len);
        memset(b + at + bin_len, 0, bpad);
        at += bin_len + bpad;
    }
    #undef PUT32
    *out_size = total;
    return b;
}

/* A triangle's worth of binary: three positions then three indices. */
static size_t tri_bin(uint8_t *out, const float pos[9]) {
    memcpy(out, pos, 36);
    uint16_t idx[3] = { 0, 1, 2 };
    memcpy(out + 36, idx, 6);
    return 42;
}

/* The JSON around that binary. `extra` is spliced in at the root. */
static void tri_json(char *out, size_t cap, const char *nodes,
                     const char *materials, const char *prim_extra,
                     const char *extra) {
    snprintf(out, cap,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":%s,"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1%s}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]"
        "%s%s"
        "}",
        nodes, prim_extra, materials, extra);
}

static GltfModel *load_tri(const char *nodes, const char *materials,
                           const char *prim_extra, const char *extra,
                           const float pos[9]) {
    static const float unit[9] = { 0,0,0, 1,0,0, 0,1,0 };
    uint8_t bin[64];
    size_t bin_len = tri_bin(bin, pos ? pos : unit);
    char json[2048];
    tri_json(json, sizeof(json), nodes, materials ? materials : "",
             prim_extra ? prim_extra : "", extra ? extra : "");
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, bin_len, &size);
    if (!glb) return NULL;
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    return rc == 0 ? m : NULL;
}

/* ── the shapes a model takes ─────────────────────────────────────── */

TEST(a_triangle_loads_with_its_positions) {
    GltfModel *m = load_tri("[{\"mesh\":0,\"name\":\"spire\"}]", NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, m->node_count);
    ASSERT_EQ_INT(1, m->prim_count);
    ASSERT_EQ_INT(3, m->prims[0].vert_count);
    ASSERT_EQ_INT(1, m->prims[0].tri_count);
    ASSERT(strcmp(m->nodes[0].name, "spire") == 0);
    ASSERT(NEAR(m->prims[0].pos[3], 1.0f));
    ASSERT(NEAR(m->prims[0].pos[7], 1.0f));
    ASSERT_EQ_INT(0, m->prims[0].idx[0]);
    ASSERT_EQ_INT(2, m->prims[0].idx[2]);
    ASSERT_EQ_INT(-1, m->prims[0].surface.image);
    ASSERT_EQ_INT(0, (int)m->prims[0].surface.team_color);
    ASSERT(NEAR(m->scale_hint, 1.0f));
    Gltf_Free(m);
}

TEST(a_child_node_keeps_its_name_and_its_offset_from_the_parent) {
    GltfModel *m = load_tri(
        "[{\"name\":\"base\",\"translation\":[10,0,0],\"children\":[1]},"
        " {\"name\":\"crystal\",\"translation\":[0,5,0],\"mesh\":0}]",
        NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(2, m->node_count);
    ASSERT(strcmp(m->nodes[0].name, "base") == 0);
    ASSERT(strcmp(m->nodes[1].name, "crystal") == 0);
    ASSERT_EQ_INT(-1, m->nodes[0].parent);
    ASSERT_EQ_INT(0, m->nodes[1].parent);
    ASSERT(NEAR(m->nodes[0].offset[0], 10.0f));
    ASSERT(NEAR(m->nodes[1].offset[1], 5.0f));
    ASSERT(NEAR(m->nodes[1].offset[0], 0.0f));
    /* The mesh hangs off the child, which is where a piece would be. */
    ASSERT_EQ_INT(1, m->prims[0].node);
    Gltf_Free(m);
}

/* A quarter turn about Y sends x toward -z. The node keeps only its
 * offset, so the turn has to be in the vertices. */
TEST(a_turned_node_has_its_turn_baked_into_the_vertices) {
    GltfModel *m = load_tri(
        "[{\"mesh\":0,\"rotation\":[0,0.70710678,0,0.70710678]}]",
        NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(m);
    /* Vertex 1 is (1,0,0) in the file. */
    ASSERT(NEAR(m->prims[0].pos[3], 0.0f));
    ASSERT(NEAR(m->prims[0].pos[5], -1.0f));
    Gltf_Free(m);
}

/* A child under a turned parent lands where the turn puts it, because
 * the offset is worked out in model space, not the parent's. */
TEST(a_child_under_a_turned_parent_is_offset_in_model_space) {
    GltfModel *m = load_tri(
        "[{\"name\":\"base\",\"rotation\":[0,0.70710678,0,0.70710678],\"children\":[1]},"
        " {\"name\":\"arm\",\"translation\":[2,0,0],\"mesh\":0}]",
        NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->nodes[1].offset[0], 0.0f));
    ASSERT(NEAR(m->nodes[1].offset[2], -2.0f));
    Gltf_Free(m);
}

TEST(a_scaled_node_has_its_scale_baked_in) {
    GltfModel *m = load_tri("[{\"mesh\":0,\"scale\":[3,3,3]}]", NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->prims[0].pos[3], 3.0f));
    ASSERT(NEAR(m->prims[0].pos[7], 3.0f));
    Gltf_Free(m);
}

TEST(a_base_colour_factor_comes_through) {
    GltfModel *m = load_tri("[{\"mesh\":0}]",
        ",\"materials\":[{\"name\":\"stone\",\"pbrMetallicRoughness\":"
        "{\"baseColorFactor\":[0.25,0.5,0.75,1]}}]",
        ",\"material\":0", NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->prims[0].surface.base_color[0], 0.25f));
    ASSERT(NEAR(m->prims[0].surface.base_color[1], 0.5f));
    ASSERT(NEAR(m->prims[0].surface.base_color[2], 0.75f));
    ASSERT_EQ_INT(0, (int)m->prims[0].surface.team_color);
    Gltf_Free(m);
}

TEST(a_material_named_teamcolor_is_marked_for_the_players_colour) {
    GltfModel *m = load_tri("[{\"mesh\":0}]",
        ",\"materials\":[{\"name\":\"TeamColor\"}]", ",\"material\":0", NULL, NULL);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, (int)m->prims[0].surface.team_color);
    Gltf_Free(m);
}

TEST(the_scale_hint_at_the_root_is_read) {
    GltfModel *m = load_tri("[{\"mesh\":0}]", NULL, NULL,
                            ",\"extras\":{\"tak_scale\":2.5}", NULL);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->scale_hint, 2.5f));
    Gltf_Free(m);
}

/* ── an embedded picture ──────────────────────────────────────────── */

/* A PNG of one pixel, built here so the test owns every byte of it. */
static size_t make_png(uint8_t *out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    uint8_t raw[4] = { 0x00, r, g, b };            /* filter 0, then RGB */
    mz_ulong comp_len = mz_compressBound(sizeof(raw));
    uint8_t comp[256];
    if (comp_len > sizeof(comp)) { printf("(bound %u) ", (unsigned)comp_len); return 0; }
    int zrc = mz_compress(comp, &comp_len, raw, sizeof(raw));
    if (zrc != MZ_OK) { printf("(mz_compress %d) ", zrc); return 0; }
    if (cap < 8 + 25 + 12 + comp_len + 12) { printf("(cap) "); return 0; }

    size_t at = 0;
    memcpy(out, sig, 8); at = 8;
    #define BE32(p, v) do { (p)[0]=(uint8_t)((v)>>24); (p)[1]=(uint8_t)((v)>>16); \
        (p)[2]=(uint8_t)((v)>>8); (p)[3]=(uint8_t)(v); } while (0)
    #define CHUNK(type, data, len) do { \
        BE32(out + at, (uint32_t)(len)); at += 4; \
        memcpy(out + at, type, 4); \
        memcpy(out + at + 4, data, len); \
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, out + at, 4 + (len)); \
        at += 4 + (len); \
        BE32(out + at, crc); at += 4; } while (0)

    uint8_t ihdr[13];
    BE32(ihdr, 1); BE32(ihdr + 4, 1);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    CHUNK("IHDR", ihdr, 13);
    CHUNK("IDAT", comp, (size_t)comp_len);
    CHUNK("IEND", comp, 0);
    #undef CHUNK
    #undef BE32
    return at;
}

TEST(an_embedded_png_is_decoded_and_bound_to_its_primitive) {
    uint8_t png[256];
    size_t png_len = make_png(png, sizeof(png), 10, 200, 30);
    ASSERT(png_len > 0);

    uint8_t bin[512];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t at = tri_bin(bin, pos);
    size_t uv_off = at;
    float uv[6] = { 0,0, 1,0, 0,1 };
    memcpy(bin + at, uv, sizeof(uv)); at += sizeof(uv);
    size_t png_off = at;
    ASSERT(at + png_len < sizeof(bin));
    memcpy(bin + at, png, png_len); at += png_len;

    char json[2048];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":2},"
        "\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}],"
        "\"buffers\":[{\"byteLength\":%u}]}",
        (unsigned)uv_off, (unsigned)png_off, (unsigned)png_len, (unsigned)at);

    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, at, &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, m->image_count);
    ASSERT_EQ_INT(1, m->images[0].w);
    ASSERT_EQ_INT(1, m->images[0].h);
    uint32_t px = m->images[0].rgba[0];
    ASSERT_EQ_INT(10, (int)(px & 0xFF));
    ASSERT_EQ_INT(200, (int)((px >> 8) & 0xFF));
    ASSERT_EQ_INT(30, (int)((px >> 16) & 0xFF));
    ASSERT_EQ_INT(0, m->prims[0].surface.image);
    ASSERT(NEAR(m->prims[0].uv[2], 1.0f));
    Gltf_Free(m);
}

/* A textured triangle: positions, indices, two UV sets and one PNG.
 * `material` is the whole materials member, `attrs` any attribute
 * past POSITION (the UV sets are accessors 2 and 3). */
static uint8_t *textured_glb(const char *material, const char *attrs, size_t *out_size) {
    uint8_t png[256];
    size_t png_len = make_png(png, sizeof(png), 10, 200, 30);
    if (png_len == 0) return NULL;
    static uint8_t bin[1024];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t at = tri_bin(bin, pos);
    while (at % 4) bin[at++] = 0;
    size_t uv0_off = at;
    float uv0[6] = { 0,0, 1,0, 0,1 };
    memcpy(bin + at, uv0, sizeof(uv0)); at += sizeof(uv0);
    size_t uv1_off = at;
    float uv1[6] = { 0.5f,0.5f, 0.25f,0.75f, 0,1 };
    memcpy(bin + at, uv1, sizeof(uv1)); at += sizeof(uv1);
    size_t png_off = at;
    memcpy(bin + at, png, png_len); at += png_len;

    char json[4096];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0%s},"
        "\"indices\":1,\"material\":0}]}],"
        "\"materials\":%s,"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}],"
        "\"buffers\":[{\"byteLength\":%u}]}",
        attrs, material,
        (unsigned)uv0_off, (unsigned)uv1_off, (unsigned)png_off, (unsigned)png_len, (unsigned)at);
    return make_glb(json, bin, at, out_size);
}

TEST(a_materials_pictures_are_laid_by_the_uv_set_it_names) {
    size_t size = 0;
    uint8_t *glb = textured_glb(
        "[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}]",
        ",\"TEXCOORD_0\":2,\"TEXCOORD_1\":3", &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, (int)m->prims[0].surface.uv_set);
    ASSERT_EQ_INT(0, m->prims[0].surface.image);
    /* The second set's second vertex. */
    ASSERT(NEAR(m->prims[0].uv[2], 0.25f));
    ASSERT(NEAR(m->prims[0].uv[3], 0.75f));
    Gltf_Free(m);
}

TEST(a_picture_laid_by_another_uv_set_than_its_material_is_left_out) {
    size_t size = 0;
    uint8_t *glb = textured_glb(
        "[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":0}},"
        "\"normalTexture\":{\"index\":0,\"texCoord\":1}}]",
        ",\"TEXCOORD_0\":2,\"TEXCOORD_1\":3", &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(0, (int)m->prims[0].surface.uv_set);
    ASSERT_EQ_INT(0, m->prims[0].surface.image);
    ASSERT_EQ_INT(-1, m->prims[0].surface.normal_image);
    Gltf_Free(m);
}

TEST(a_geometry_only_reading_numbers_the_pictures_alike) {
    size_t size = 0;
    uint8_t *glb = textured_glb(
        "[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]",
        ",\"TEXCOORD_0\":2", &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *full = NULL, *bare = NULL;
    ASSERT_EQ_INT(0, Gltf_LoadFromMemoryEx(&full, glb, size, GLTF_WITH_IMAGES));
    ASSERT_EQ_INT(0, Gltf_LoadFromMemoryEx(&bare, glb, size, GLTF_GEOMETRY_ONLY));
    tak_free(glb);
    ASSERT_NOT_NULL(full);
    ASSERT_NOT_NULL(bare);
    ASSERT_EQ_INT(1, full->image_count);
    ASSERT_EQ_INT(1, bare->image_count);
    ASSERT_EQ_INT(0, full->prims[0].surface.image);
    ASSERT_EQ_INT(0, bare->prims[0].surface.image);
    ASSERT_NOT_NULL(full->images[0].rgba);
    ASSERT(bare->images[0].rgba == NULL);
    Gltf_Free(full);
    Gltf_Free(bare);
}

TEST(a_materials_surface_comes_through) {
    size_t size = 0;
    uint8_t *glb = textured_glb(
        "[{\"pbrMetallicRoughness\":{\"metallicRoughnessTexture\":{\"index\":0},"
        "\"baseColorFactor\":[1,1,1,0.5],\"metallicFactor\":0.25,\"roughnessFactor\":0.5},"
        "\"normalTexture\":{\"index\":0,\"scale\":0.75},"
        "\"emissiveFactor\":[1,0.5,0],\"alphaMode\":\"BLEND\",\"doubleSided\":true}]",
        ",\"TEXCOORD_0\":2", &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(m);
    const GltfSurface *s = &m->prims[0].surface;
    ASSERT_EQ_INT(-1, s->image);
    ASSERT_EQ_INT(0, s->mr_image);
    ASSERT_EQ_INT(0, s->normal_image);
    ASSERT(NEAR(s->metallic, 0.25f));
    ASSERT(NEAR(s->roughness, 0.5f));
    ASSERT(NEAR(s->normal_scale, 0.75f));
    ASSERT(NEAR(s->emissive[0], 1.0f));
    ASSERT(NEAR(s->emissive[1], 0.5f));
    ASSERT(NEAR(s->emissive[2], 0.0f));
    ASSERT_EQ_INT(1, (int)s->blend);
    ASSERT_EQ_INT(1, (int)s->double_sided);
    Gltf_Free(m);
}

TEST(a_material_with_a_mask_keeps_its_cutoff) {
    GltfModel *g = load_tri("[{\"mesh\":0}]",
        ",\"materials\":[{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3}]", ",\"material\":0",
        NULL, NULL);
    ASSERT_NOT_NULL(g);
    ASSERT(NEAR(g->prims[0].surface.alpha_cutoff, 0.3f));
    ASSERT_EQ_INT(0, (int)g->prims[0].surface.blend);
    Gltf_Free(g);
}

/* A triangle with a normal and a tangent on every vertex. The tangent
 * is what a spec following exporter writes for this triangle laid
 * with u along x and v along y: glTF puts the top of a picture at
 * v = 0, the normal map's up runs against v, so the bitangent runs
 * along -y, and with the normal +z and the tangent +x that is w = -1.
 * `tan` overrides it. */
static GltfModel *load_tri_with_frame_tan(const float tan_in[4]) {
    static uint8_t bin[256];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t at = tri_bin(bin, pos);
    while (at % 4) bin[at++] = 0;
    size_t nrm_off = at;
    float nrm[9] = { 0,0,1, 0,0,1, 0,0,1 };
    memcpy(bin + at, nrm, sizeof(nrm)); at += sizeof(nrm);
    size_t tan_off = at;
    float tan[12];
    for (int v = 0; v < 3; v++) memcpy(&tan[4 * v], tan_in, sizeof(float) * 4);
    memcpy(bin + at, tan, sizeof(tan)); at += sizeof(tan);
    char json[2048];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":2,\"TANGENT\":3},"
        "\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":48}],"
        "\"buffers\":[{\"byteLength\":%u}]}",
        (unsigned)nrm_off, (unsigned)tan_off, (unsigned)at);
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, at, &size);
    if (!glb) return NULL;
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    return rc == 0 ? m : NULL;
}

static GltfModel *load_tri_with_frame(void) {
    static const float spec[4] = { 1, 0, 0, -1 };
    return load_tri_with_frame_tan(spec);
}

TEST(normals_and_tangents_are_read) {
    GltfModel *g = load_tri_with_frame();
    ASSERT_NOT_NULL(g);
    ASSERT_NOT_NULL(g->prims[0].nrm);
    ASSERT_NOT_NULL(g->prims[0].tan);
    ASSERT(NEAR(g->prims[0].nrm[2], 1.0f));
    ASSERT(NEAR(g->prims[0].tan[0], 1.0f));
    ASSERT(NEAR(g->prims[0].tan[3], -1.0f));
    Gltf_Free(g);
}

/* ── the shapes a bad file takes ──────────────────────────────────── */

static int refuses(const char *json, const uint8_t *bin, size_t bin_len) {
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, bin_len, &size);
    if (!glb) return 0;
    GltfModel *m = (GltfModel *)(void *)1;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    if (rc == 0) { Gltf_Free(m); return 0; }
    return m == NULL;
}

TEST(a_file_that_is_not_a_glb_is_refused) {
    GltfModel *m = NULL;
    uint8_t junk[64];
    memset(junk, 0xAB, sizeof(junk));
    ASSERT(Gltf_LoadFromMemory(&m, junk, sizeof(junk)) != 0);
    ASSERT(m == NULL);
    ASSERT(Gltf_LoadFromMemory(&m, junk, 4) != 0);
    ASSERT(Gltf_LoadFromMemory(&m, NULL, 100) != 0);
}

TEST(a_glb_of_another_version_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[2048];
    tri_json(json, sizeof(json), "[{\"mesh\":0}]", "", "", "");
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, bin_len, &size);
    ASSERT_NOT_NULL(glb);
    glb[4] = 3;                      /* version 3 */
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    ASSERT(rc != 0);
    ASSERT(m == NULL);
}

TEST(a_truncated_glb_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[2048];
    tri_json(json, sizeof(json), "[{\"mesh\":0}]", "", "", "");
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, bin_len, &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *m = NULL;
    /* Every prefix of a good file is a bad file. */
    for (size_t cut = 12; cut < size; cut += 7) {
        m = NULL;
        if (Gltf_LoadFromMemory(&m, glb, cut) == 0) {
            printf("(a %u byte prefix loaded) ", (unsigned)cut);
            Gltf_Free(m);
            tak_free(glb);
            ASSERT(0);
        }
        ASSERT(m == NULL);
    }
    tak_free(glb);
}

TEST(malformed_json_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    ASSERT(refuses("{\"nodes\":[", bin, bin_len));
    ASSERT(refuses("{\"nodes\":[{\"mesh\":0}", bin, bin_len));
    ASSERT(refuses("not json at all", bin, bin_len));
    ASSERT(refuses("{\"nodes\":[{\"name\":\"unterminated}]}", bin, bin_len));
}

TEST(an_index_past_the_vertices_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    uint16_t bad[3] = { 0, 1, 9 };
    memcpy(bin + 36, bad, 6);
    char json[2048];
    tri_json(json, sizeof(json), "[{\"mesh\":0}]", "", "", "");
    ASSERT(refuses(json, bin, bin_len));
}

TEST(an_accessor_past_the_binary_chunk_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[2048];
    /* A count far past what the view holds. */
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":9000,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}");
    ASSERT(refuses(json, bin, bin_len));

    /* A view that starts past the end of the chunk. */
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":100000,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}");
    ASSERT(refuses(json, bin, bin_len));

    /* An offset that would wrap if it were added rather than compared. */
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"byteOffset\":2147483640,\"componentType\":5126,"
        "\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}");
    ASSERT(refuses(json, bin, bin_len));
}

TEST(a_node_that_is_its_own_ancestor_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[2048];
    tri_json(json, sizeof(json),
        "[{\"children\":[1]},{\"children\":[0],\"mesh\":0}]", "", "", "");
    ASSERT(refuses(json, bin, bin_len));
}

TEST(more_nodes_than_the_mesh_can_hold_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    /* A chain longer than the node budget, each node holding the next. */
    char nodes[8192];
    size_t at = 0;
    at += (size_t)snprintf(nodes + at, sizeof(nodes) - at, "[");
    const int N = GLTF_MAX_NODES + 8;
    for (int i = 0; i < N; i++) {
        at += (size_t)snprintf(nodes + at, sizeof(nodes) - at,
                               "%s{\"children\":[%d]}", i ? "," : "", i + 1);
        if (at > sizeof(nodes) - 64) break;
    }
    at += (size_t)snprintf(nodes + at, sizeof(nodes) - at, ",{\"mesh\":0}]");
    char json[16384];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":%s,"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}", nodes);
    ASSERT(refuses(json, bin, bin_len));
}

TEST(json_nested_past_all_reason_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[4096];
    size_t at = 0;
    at += (size_t)snprintf(json + at, sizeof(json) - at, "{\"nodes\":");
    for (int i = 0; i < 400 && at < sizeof(json) - 8; i++) json[at++] = '[';
    json[at] = '\0';
    ASSERT(refuses(json, bin, bin_len));
}

TEST(a_file_that_draws_nothing_is_refused) {
    uint8_t bin[64];
    static const float pos[9] = { 0,0,0, 1,0,0, 0,1,0 };
    size_t bin_len = tri_bin(bin, pos);
    char json[2048];
    tri_json(json, sizeof(json), "[{\"name\":\"empty\"}]", "", "", "");
    ASSERT(refuses(json, bin, bin_len));
}

/* ── into a unit mesh ─────────────────────────────────────────────── */

#define TEAM 0xFF204080u

TEST(a_model_becomes_a_mesh_in_the_units_the_engine_holds) {
    GltfModel *g = load_tri("[{\"mesh\":0,\"name\":\"spire\"}]", NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, m->node_count);
    ASSERT_EQ_INT(3, m->vert_count);
    ASSERT_EQ_INT(1, m->tri_count);
    ASSERT_EQ_INT(1, m->batch_count);
    ASSERT(strcmp(m->nodes[0].name, "spire") == 0);
    /* A unit to the pixel, and a pixel is 65536 of what a mesh holds. */
    ASSERT(NEAR(m->positions[3], TA_UNITS_PER_PIXEL));
    /* The far axis turns around, and the winding turns with it. */
    ASSERT_EQ_INT(0, m->indices[0]);
    ASSERT_EQ_INT(2, m->indices[1]);
    ASSERT_EQ_INT(1, m->indices[2]);
    ASSERT_EQ_INT(0, Gltf_ValidateMesh(m, "test"));
    Gltf_FreeUnitMesh(m);
}

TEST(the_far_axis_turns_around_in_the_mesh) {
    static const float pos[9] = { 0,0,0, 0,0,4, 0,1,0 };
    GltfModel *g = load_tri("[{\"mesh\":0}]", NULL, NULL, NULL, pos);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->positions[5], -4.0f * TA_UNITS_PER_PIXEL));
    Gltf_FreeUnitMesh(m);
}

TEST(a_teamcolor_material_paints_its_vertices_the_players_colour) {
    GltfModel *g = load_tri("[{\"mesh\":0}]",
        ",\"materials\":[{\"name\":\"teamcolor\"}]", ",\"material\":0", NULL, NULL);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT(m->colors[0] == TEAM);
    Gltf_FreeUnitMesh(m);
}

TEST(a_piece_keeps_its_name_and_its_offset_in_the_mesh) {
    GltfModel *g = load_tri(
        "[{\"name\":\"base\",\"translation\":[10,0,0],\"children\":[1]},"
        " {\"name\":\"crystal\",\"translation\":[0,5,0],\"mesh\":0}]",
        NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(2, m->node_count);
    ASSERT(strcmp(m->nodes[1].name, "crystal") == 0);
    ASSERT_EQ_INT(0, m->nodes[1].parent);
    ASSERT(NEAR(m->nodes[1].offset[1], 5.0f * TA_UNITS_PER_PIXEL));
    /* Every vertex sits on the piece that holds the mesh. */
    for (int v = 0; v < m->vert_count; v++) ASSERT_EQ_INT(1, (int)m->vert_node_idx[v]);
    /* The bounds are where the pieces put the model, not where the
     * vertices sit inside their own piece. */
    ASSERT(m->aabb_max[0] >= 10.0f * TA_UNITS_PER_PIXEL);
    ASSERT(m->aabb_max[1] >= 5.0f * TA_UNITS_PER_PIXEL);
    ASSERT_EQ_INT(0, Gltf_ValidateMesh(m, "test"));
    Gltf_FreeUnitMesh(m);
}

TEST(the_scale_hint_scales_the_mesh) {
    GltfModel *g = load_tri("[{\"mesh\":0}]", NULL, NULL,
                            ",\"extras\":{\"tak_scale\":2}", NULL);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT(NEAR(m->positions[3], 2.0f * TA_UNITS_PER_PIXEL));
    Gltf_FreeUnitMesh(m);
}

/* Two primitives on one piece, each with its own material. */
static GltfModel *load_two_materials(const char *materials) {
    static const float unit[9] = { 0,0,0, 1,0,0, 0,1,0 };
    uint8_t bin[64];
    size_t bin_len = tri_bin(bin, unit);
    char json[2048];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0},"
        "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":1}]}],"
        "\"materials\":%s,"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}", materials);
    size_t size = 0;
    uint8_t *glb = make_glb(json, bin, bin_len, &size);
    if (!glb) return NULL;
    GltfModel *m = NULL;
    int rc = Gltf_LoadFromMemory(&m, glb, size);
    tak_free(glb);
    return rc == 0 ? m : NULL;
}

TEST(a_material_that_blends_comes_last_in_the_mesh) {
    GltfModel *g = load_two_materials(
        "[{\"alphaMode\":\"BLEND\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,0.5]}},"
        " {\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]}}]");
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(2, m->batch_count);
    ASSERT_EQ_INT(0, (int)img[0].blend);
    ASSERT_EQ_INT(1, (int)img[1].blend);
    /* The solid material's red vertices come first. */
    ASSERT_EQ_INT(0xFF, (int)(m->colors[m->indices[m->batches[0].first_index]] & 0xFF));
    ASSERT_EQ_INT(0x80, (int)(m->colors[m->indices[m->batches[1].first_index]] >> 24));
    ASSERT_EQ_INT(0, Gltf_ValidateMesh(m, "test"));
    Gltf_FreeUnitMesh(m);
}

TEST(two_materials_that_draw_alike_share_a_batch) {
    GltfModel *g = load_two_materials(
        "[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]}},"
        " {\"pbrMetallicRoughness\":{\"baseColorFactor\":[0,1,0,1]}}]");
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(1, m->batch_count);
    ASSERT_EQ_INT(6, m->vert_count);
    /* Each keeps its own colour on its vertices. */
    ASSERT_EQ_INT(0xFF, (int)(m->colors[0] & 0xFF));
    ASSERT_EQ_INT(0xFF, (int)((m->colors[3] >> 8) & 0xFF));
    Gltf_FreeUnitMesh(m);
}

TEST(normals_and_tangents_turn_the_far_axis_in_the_mesh) {
    GltfModel *g = load_tri_with_frame();
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_NOT_NULL(m->normals);
    ASSERT_NOT_NULL(m->tangents);
    ASSERT(NEAR(m->normals[2], -1.0f));
    ASSERT(NEAR(m->tangents[0], 1.0f));
    /* The handedness turns with the axis, and lands where the made
     * tangent for the same triangle lands, in the case below. */
    ASSERT(NEAR(m->tangents[3], 1.0f));
    Gltf_FreeUnitMesh(m);
}

TEST(a_file_tangent_leaning_along_the_normal_is_laid_on_the_surface) {
    /* Along the normal, with a little along x to keep a direction. */
    static const float leaning[4] = { 0.1f, 0, 1, 1 };
    GltfModel *g = load_tri_with_frame_tan(leaning);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_NOT_NULL(m->tangents);
    for (int v = 0; v < m->vert_count; v++) {
        const float *tn = &m->tangents[4 * v];
        const float *nn = &m->normals[3 * v];
        ASSERT(NEAR(tn[0] * nn[0] + tn[1] * nn[1] + tn[2] * nn[2], 0.0f));
        ASSERT(NEAR(tn[0], 1.0f));
    }
    Gltf_FreeUnitMesh(m);
}

TEST(tangents_are_made_for_a_normal_map_the_file_brought_none_for) {
    size_t size = 0;
    uint8_t *glb = textured_glb("[{\"normalTexture\":{\"index\":0}}]", ",\"TEXCOORD_0\":2", &size);
    ASSERT_NOT_NULL(glb);
    GltfModel *g = NULL;
    int rc = Gltf_LoadFromMemory(&g, glb, size);
    tak_free(glb);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(g);
    GltfBatch img[UNIT_MESH_MAX_BATCHES];
    UnitMesh *m = Gltf_ToUnitMesh(g, "test", TEAM, img);
    Gltf_Free(g);
    ASSERT_NOT_NULL(m);
    ASSERT_NOT_NULL(m->normals);
    ASSERT_NOT_NULL(m->tangents);
    ASSERT_EQ_INT(0, img[0].normal_image);
    /* The picture's u runs along x, so the tangent does, and it lies
     * on the surface. */
    for (int v = 0; v < m->vert_count; v++) {
        const float *tn = &m->tangents[4 * v];
        const float *nn = &m->normals[3 * v];
        float len = sqrtf(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
        ASSERT(NEAR(len, 1.0f));
        ASSERT(NEAR(tn[0] * nn[0] + tn[1] * nn[1] + tn[2] * nn[2], 0.0f));
        ASSERT(NEAR(tn[0], 1.0f));
        ASSERT(NEAR(fabsf(tn[3]), 1.0f));
    }
    /* Worked by hand for this triangle: the far axis turned makes the
     * face normal -z, and the handedness that puts the picture's top
     * against the direction v grows is positive. */
    ASSERT(NEAR(m->normals[2], -1.0f));
    ASSERT(NEAR(m->tangents[3], 1.0f));
    Gltf_FreeUnitMesh(m);
}

/* ── what the checking refuses ────────────────────────────────────── */

/* Two pieces, three vertices each, one triangle apiece, one batch. */
static UnitMesh *hand_mesh(void) {
    UnitMesh *m = (UnitMesh *)tak_malloc(sizeof(UnitMesh));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->node_count = 2;
    m->nodes[0].parent = -1;
    m->nodes[1].parent = 0;
    m->vert_count = 6;
    m->tri_count = 2;
    m->positions     = (float *)tak_malloc(sizeof(float) * 18);
    m->uvs           = (float *)tak_malloc(sizeof(float) * 12);
    m->colors        = (uint32_t *)tak_malloc(sizeof(uint32_t) * 6);
    m->vert_node_idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * 6);
    m->indices       = (uint16_t *)tak_malloc(sizeof(uint16_t) * 6);
    if (!m->positions || !m->uvs || !m->colors || !m->vert_node_idx || !m->indices) {
        Gltf_FreeUnitMesh(m);
        return NULL;
    }
    memset(m->positions, 0, sizeof(float) * 18);
    memset(m->uvs, 0, sizeof(float) * 12);
    memset(m->colors, 0, sizeof(uint32_t) * 6);
    for (int v = 0; v < 6; v++) m->vert_node_idx[v] = (uint16_t)(v / 3);
    for (int i = 0; i < 6; i++) m->indices[i] = (uint16_t)i;
    m->batch_count = 1;
    m->batches[0].first_index = 0;
    m->batches[0].index_count = 6;
    return m;
}

TEST(a_mesh_built_by_hand_passes_the_checking) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(0, Gltf_ValidateMesh(m, "hand"));
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_a_piece_whose_parent_comes_after_it) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->nodes[1].parent = 1;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->nodes[1].parent = 7;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->nodes[0].parent = 0;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_an_index_past_the_vertices) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->indices[4] = 99;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_a_vertex_on_no_piece) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->vert_node_idx[2] = 5;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_a_triangle_across_two_pieces) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->vert_node_idx[2] = 1;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_a_batch_out_of_piece_order) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    /* The second piece's triangle first, which no run could cover. */
    uint16_t swapped[6] = { 3, 4, 5, 0, 1, 2 };
    memcpy(m->indices, swapped, sizeof(swapped));
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_batches_that_do_not_add_up) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->batches[0].index_count = 3;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->batches[0].index_count = 9;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->batches[0].index_count = 4;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->batches[0].index_count = 6;
    m->batches[0].first_index = 1;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->batches[0].first_index = -3;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
}

TEST(the_checking_refuses_counts_outside_what_a_mesh_holds) {
    UnitMesh *m = hand_mesh();
    ASSERT_NOT_NULL(m);
    m->node_count = 0;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->node_count = UNIT_MESH_MAX_NODES + 1;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->node_count = 2;
    m->batch_count = UNIT_MESH_MAX_BATCHES + 1;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    m->batch_count = 1;
    m->tri_count = 0;
    ASSERT(Gltf_ValidateMesh(m, "hand") != 0);
    Gltf_FreeUnitMesh(m);
    ASSERT(Gltf_ValidateMesh(NULL, "nothing") != 0);
}

int main(void) {
    TAK_Crash_Install();
    printf("test_gltf\n");
    TEST_SUITE("A model");
    RUN(a_triangle_loads_with_its_positions);
    RUN(a_child_node_keeps_its_name_and_its_offset_from_the_parent);
    RUN(a_turned_node_has_its_turn_baked_into_the_vertices);
    RUN(a_child_under_a_turned_parent_is_offset_in_model_space);
    RUN(a_scaled_node_has_its_scale_baked_in);
    RUN(a_base_colour_factor_comes_through);
    RUN(a_material_named_teamcolor_is_marked_for_the_players_colour);
    RUN(the_scale_hint_at_the_root_is_read);
    RUN(an_embedded_png_is_decoded_and_bound_to_its_primitive);
    TEST_SUITE("A bad file");
    RUN(a_file_that_is_not_a_glb_is_refused);
    RUN(a_glb_of_another_version_is_refused);
    RUN(a_truncated_glb_is_refused);
    RUN(malformed_json_is_refused);
    RUN(an_index_past_the_vertices_is_refused);
    RUN(an_accessor_past_the_binary_chunk_is_refused);
    RUN(a_node_that_is_its_own_ancestor_is_refused);
    RUN(more_nodes_than_the_mesh_can_hold_is_refused);
    RUN(json_nested_past_all_reason_is_refused);
    RUN(a_file_that_draws_nothing_is_refused);
    RUN(a_materials_pictures_are_laid_by_the_uv_set_it_names);
    RUN(a_picture_laid_by_another_uv_set_than_its_material_is_left_out);
    RUN(a_geometry_only_reading_numbers_the_pictures_alike);
    RUN(a_materials_surface_comes_through);
    RUN(a_material_with_a_mask_keeps_its_cutoff);
    RUN(normals_and_tangents_are_read);
    TEST_SUITE("Into a unit mesh");
    RUN(a_model_becomes_a_mesh_in_the_units_the_engine_holds);
    RUN(the_far_axis_turns_around_in_the_mesh);
    RUN(a_teamcolor_material_paints_its_vertices_the_players_colour);
    RUN(a_piece_keeps_its_name_and_its_offset_in_the_mesh);
    RUN(the_scale_hint_scales_the_mesh);
    RUN(a_material_that_blends_comes_last_in_the_mesh);
    RUN(two_materials_that_draw_alike_share_a_batch);
    RUN(normals_and_tangents_turn_the_far_axis_in_the_mesh);
    RUN(a_file_tangent_leaning_along_the_normal_is_laid_on_the_surface);
    RUN(tangents_are_made_for_a_normal_map_the_file_brought_none_for);
    TEST_SUITE("What the checking refuses");
    RUN(a_mesh_built_by_hand_passes_the_checking);
    RUN(the_checking_refuses_a_piece_whose_parent_comes_after_it);
    RUN(the_checking_refuses_an_index_past_the_vertices);
    RUN(the_checking_refuses_a_vertex_on_no_piece);
    RUN(the_checking_refuses_a_triangle_across_two_pieces);
    RUN(the_checking_refuses_a_batch_out_of_piece_order);
    RUN(the_checking_refuses_batches_that_do_not_add_up);
    RUN(the_checking_refuses_counts_outside_what_a_mesh_holds);
    TEST_REPORT();
}
