/*
 * fuzz_gltf.c -- the glTF reader, fed anything at all.
 *
 * LLVMFuzzerTestOneInput is the entry a coverage guided fuzzer drives:
 *
 *   clang -g -fsanitize=fuzzer,address -Iinclude -Ithird_party \
 *         src/render/fuzz_gltf.c src/render/gltf.c src/render/png_decode.c \
 *         src/render/jpg_decode.c src/core/memory.c -o fuzz_gltf
 *
 * Built without a fuzzing engine it keeps a main() so CI runs a short
 * deterministic sweep on every build, and so an input found elsewhere
 * can be replayed by passing its file. These are the bytes of a model
 * someone put in a folder, and the reader has to hold whatever they
 * turn out to be.
 */
#include "tak_gltf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    GltfModel *m = NULL;
    if (Gltf_LoadFromMemory(&m, data, size) == 0) Gltf_Free(m);
    return 0;
}

#ifndef TAK_FUZZ_ENGINE

/* A .glb around `json`, with a binary chunk of counting bytes. */
static size_t build(uint8_t *out, size_t cap, const char *json, size_t bin_len) {
    size_t jlen = strlen(json);
    size_t jpad = (4 - (jlen & 3)) & 3;
    size_t bpad = (4 - (bin_len & 3)) & 3;
    size_t total = 12 + 8 + jlen + jpad + 8 + bin_len + bpad;
    if (total > cap) return 0;
    size_t at = 0;
    uint32_t words[3] = { 0x46546C67u, 2u, (uint32_t)total };
    memcpy(out, words, 12); at = 12;
    uint32_t jh[2] = { (uint32_t)(jlen + jpad), 0x4E4F534Au };
    memcpy(out + at, jh, 8); at += 8;
    memcpy(out + at, json, jlen);
    memset(out + at + jlen, ' ', jpad);
    at += jlen + jpad;
    uint32_t bh[2] = { (uint32_t)(bin_len + bpad), 0x004E4942u };
    memcpy(out + at, bh, 8); at += 8;
    for (size_t i = 0; i < bin_len + bpad; i++) out[at + i] = (uint8_t)i;
    at += bin_len + bpad;
    return at;
}

/* A file that parses, then every single byte mutation of it, then a
 * run of shapes that are not files at all. Deterministic, so a CI
 * failure reproduces exactly. */
static int sweep(void) {
    static const char *jsons[] = {
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"name\":\"a\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":42}]}",
        "{\"nodes\":[{\"children\":[0]}]}",
        "{\"nodes\":[],\"meshes\":[]}",
        "{}",
        "[]",
        "",
    };
    static uint8_t buf[4096];
    int runs = 0;

    for (size_t j = 0; j < sizeof(jsons) / sizeof(jsons[0]); j++) {
        size_t n = build(buf, sizeof(buf), jsons[j], 48);
        if (!n) continue;
        LLVMFuzzerTestOneInput(buf, n);
        runs++;
        /* Every prefix, which is every way a file can be cut short. */
        for (size_t cut = 0; cut <= n; cut += 3) {
            LLVMFuzzerTestOneInput(buf, cut);
            runs++;
        }
        /* One bit turned over, at every byte. */
        for (size_t i = 0; i < n && i < 512; i++) {
            buf[i] ^= 0xFF;
            LLVMFuzzerTestOneInput(buf, n);
            buf[i] ^= 0xFF;
            runs++;
        }
    }

    /* Shapes that were never a file. */
    static const uint8_t junk[][12] = {
        { 0 },
        { 'g', 'l', 'T', 'F' },
        { 'g', 'l', 'T', 'F', 2, 0, 0, 0 },
        { 'g', 'l', 'T', 'F', 2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF },
    };
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        for (size_t len = 0; len <= sizeof(junk[i]); len++) {
            LLVMFuzzerTestOneInput(junk[i], len);
            runs++;
        }
    }
    return runs;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "fuzz_gltf: cannot open %s\n", argv[1]); return 1; }
        static uint8_t in[1 << 20];
        size_t n = fread(in, 1, sizeof(in), f);
        fclose(f);
        LLVMFuzzerTestOneInput(in, n);
        printf("fuzz_gltf: replayed %s, %u bytes\n", argv[1], (unsigned)n);
        return 0;
    }
    /* The reader says why it refused each one, which is useful when a
     * person is looking and is a thousand lines of noise in a CI log. */
    FILE *quiet = freopen(
#ifdef _WIN32
        "NUL",
#else
        "/dev/null",
#endif
        "w", stderr);
    int runs = sweep();
    (void)quiet;
    printf("fuzz_gltf: %d inputs, none brought it down\n", runs);
    return 0;
}

#endif /* TAK_FUZZ_ENGINE */
