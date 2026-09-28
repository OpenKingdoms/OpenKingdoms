#ifndef TAK_CAPTURE_H
#define TAK_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * A fixed battle capture for comparing the fog with another renderer:
 * --map, --seed, --los and --scout shape a --skirmish, and --fog-dump
 * writes the local player's fog beside the --screenshot. Desktop only:
 * the browser keeps written files inside the page.
 */

struct GameWorld;

typedef struct TAK_CaptureArgs {
    const char *map;         /* file or shown name, NULL for the lobby's */
    int         has_seed;
    uint32_t    seed;
    int         los;         /* 1 on, 0 off, -1 as the lobby has it */
    int         scout;
    const char *fog_dump;
} TAK_CaptureArgs;

void Capture_ArgsInit(TAK_CaptureArgs *a);

/* Take argv[*i] if it is a capture flag, advancing *i past its value.
 * 1 taken, 0 not a capture flag, -1 a bad or missing value with the
 * reason in why. */
int  Capture_TakeArg(TAK_CaptureArgs *a, int argc, char **argv, int *i,
                     char *why, size_t why_cap);

/* The flags only make sense for an automatic single-player skirmish,
 * and --scout and --fog-dump only for one that takes a --screenshot.
 * 0 when they fit, -1 with the reason. */
int  Capture_Check(const TAK_CaptureArgs *a, int skirmish, int screenshot,
                   char *why, size_t why_cap);

/* One cell of the local player's fog as the classic overlay draws it:
 * 0 black, 1 dimmed, 2 clear. With line of sight off, ground seen
 * before draws clear. */
int  Capture_FogCell(const struct GameWorld *w, int cx, int cy);

/* The whole fog, a byte a 16 pixel cell, row 0 north, into path.
 * 0 on success. */
int  Capture_WriteFog(const struct GameWorld *w, const char *path);

/* --scout: the local monarch walks 34 cells toward the middle of the
 * map and back, turning back on arrival or at turn_back_tick, and the
 * view holds the middle of the walk. Nothing happens in a live match.
 * Returns 1 when it gave an order this tick. */
int  Capture_ScoutTick(struct GameWorld *w, uint32_t tick, uint32_t turn_back_tick);
/* Forget the walk, for the next battle. */
void Capture_ScoutReset(void);

#endif /* TAK_CAPTURE_H */
