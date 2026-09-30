/*
 * player_id.c -- a leaderboard player from their device token.
 *
 * SHA-256 over a fixed label and the token, first eight bytes big
 * endian. The label keeps this hash of the token apart from any other
 * use of it.
 */

#include "tak_net_player.h"
#include "tak_sha256.h"

static const char LABEL[] = "OpenKingdoms leaderboard player";

uint64_t TAK_Player_FromToken(const uint8_t token[TAK_NET_TOKEN_BYTES]) {
    int set = 0;
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) set |= token[i];
    if (!set) return 0;
    TAK_Sha256 h;
    uint8_t d[TAK_SHA256_BYTES];
    TAK_Sha256_Init(&h);
    TAK_Sha256_Update(&h, LABEL, sizeof LABEL);
    TAK_Sha256_Update(&h, token, TAK_NET_TOKEN_BYTES);
    TAK_Sha256_Final(&h, d);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
    return v ? v : 1u;
}
