/*
 * fuzz_protocol.c -- the relay's frame parser, fed anything at all.
 *
 * LLVMFuzzerTestOneInput is the entry a coverage guided fuzzer drives:
 *
 *   clang -g -fsanitize=fuzzer,address -Iinclude \
 *         src/net/fuzz_protocol.c src/net/protocol.c -o fuzz_protocol
 *
 * Built without a fuzzing engine it keeps a main() so CI can run a short
 * structured sweep on every build, and so a crashing input found elsewhere
 * can be replayed by passing its file. Both go through the same entry the
 * relay itself uses, so nothing the fuzzer reaches is unreachable in
 * production.
 */

#include "tak_net_protocol.h"
#include "tak_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > TAK_NET_FRAME_MAX) return 0;
    TAK_NetFrame f;
    if (TAK_Net_Split(data, size, &f) == 0) {
        /* A frame that splits must also survive a full decode. */
        (void)TAK_Net_Validate(data, size);
    }
    (void)TAK_Net_Validate(data, size);
    return 0;
}

#ifndef TAK_FUZZ_ENGINE

static uint8_t frame[TAK_NET_FRAME_MAX];

/* Every type byte crossed with a set of payload shapes. Deterministic, so
 * a CI failure reproduces exactly. */
static int sweep(void) {
    static const size_t lens[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 63,
                                   64, 127, 255, 256, 511, 1024, 4096 };
    const size_t nlens = sizeof(lens) / sizeof(lens[0]);
    int cases = 0;
    for (int type = 0; type < 256; type++) {
        for (size_t li = 0; li < nlens; li++) {
            size_t payload = lens[li];
            if (payload + TAK_NET_FRAME_HEADER > sizeof(frame)) continue;
            for (int pattern = 0; pattern < 4; pattern++) {
                frame[0] = (uint8_t)type;
                tak_put_u16(frame + 1, (uint16_t)payload);
                for (size_t i = 0; i < payload; i++) {
                    switch (pattern) {
                    case 0: frame[3 + i] = 0x00; break;
                    case 1: frame[3 + i] = 0xff; break;
                    case 2: frame[3 + i] = (uint8_t)i; break;
                    default: frame[3 + i] = (uint8_t)(i * 31u + 7u); break;
                    }
                }
                LLVMFuzzerTestOneInput(frame, TAK_NET_FRAME_HEADER + payload);
                cases++;
                /* The same bytes with a length field that lies. */
                tak_put_u16(frame + 1, (uint16_t)(payload + 1));
                LLVMFuzzerTestOneInput(frame, TAK_NET_FRAME_HEADER + payload);
                cases++;
                if (payload > 0) {
                    tak_put_u16(frame + 1, (uint16_t)(payload - 1));
                    LLVMFuzzerTestOneInput(frame, TAK_NET_FRAME_HEADER + payload);
                    cases++;
                }
            }
        }
    }
    /* Shorter than a header, and the largest frame we accept. */
    for (size_t n = 0; n < TAK_NET_FRAME_HEADER; n++)
        LLVMFuzzerTestOneInput(frame, n);
    frame[0] = TAK_MSG_TURN;
    tak_put_u16(frame + 1, (uint16_t)TAK_NET_PAYLOAD_MAX);
    memset(frame + 3, 0xa5, TAK_NET_PAYLOAD_MAX);
    LLVMFuzzerTestOneInput(frame, TAK_NET_FRAME_MAX);
    cases += 4;
    return cases;
}

static int replay(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    size_t n = fread(frame, 1, sizeof(frame), fp);
    fclose(fp);
    LLVMFuzzerTestOneInput(frame, n);
    printf("replayed %s, %u bytes\n", path, (unsigned)n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        int rc = 0;
        for (int i = 1; i < argc; i++) rc |= replay(argv[i]);
        return rc;
    }
    int cases = sweep();
    printf("fuzz_protocol: %d frames parsed without a crash\n", cases);
    return 0;
}

#endif /* TAK_FUZZ_ENGINE */
