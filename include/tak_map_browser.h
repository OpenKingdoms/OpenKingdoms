#ifndef TAK_MAP_BROWSER_H
#define TAK_MAP_BROWSER_H

/*
 * The lobby's map list with a search, filters and an order, and the
 * start positions drawn on a map's picture for a seat to claim. Shared
 * by the skirmish screen and the room's map chooser. The search, the
 * filters and the orders are the remaster lobby's (MapCatalog in the
 * Unity front end), so both front ends list the same maps the same way.
 */

#include <SDL.h>
#include <stddef.h>

#include "tak_battle_config.h"

struct Font;
struct TranslateTable;
struct TAK_Platform;

/* A map's start positions, as the engine reads them. */
#define TAK_MAP_STARTS_MAX TAK_MAX_PLAYERS

typedef struct TAK_MapSummary {
    int size_x, size_y;       /* the .ota's "15 x 15", 512 px each way */
    int max_players;          /* the largest lineup numplayers lists */
    int start_count;
    /* Cells from the map's corner, StartPos1 first. The first
     * TAK_MAP_STARTS_MAX in the file, sorted by their number, which is
     * the order a seat's start_pos counts in. */
    int start_x[TAK_MAP_STARTS_MAX], start_z[TAK_MAP_STARTS_MAX];
} TAK_MapSummary;

/* Read a map's .ota. 0 on success. */
int TAK_MapSummary_Read(const char *key, TAK_MapSummary *out);

/* ── The list and its query ─────────────────────────────────────────── */

enum {
    TAK_MAPSIZE_ANY = 0, TAK_MAPSIZE_SMALL, TAK_MAPSIZE_MEDIUM,
    TAK_MAPSIZE_LARGE, TAK_MAPSIZE_HUGE, TAK_MAPSIZE_COUNT
};
enum {
    TAK_MAPSORT_NAME = 0, TAK_MAPSORT_NAME_DESC, TAK_MAPSORT_PLAYERS,
    TAK_MAPSORT_PLAYERS_DESC, TAK_MAPSORT_SIZE, TAK_MAPSORT_SIZE_DESC,
    TAK_MAPSORT_COUNT
};

typedef struct TAK_MapRow {
    char key[96];
    char display[128];
    char fold[128];
    int  players;             /* start positions, else the largest lineup */
    int  size_x, size_y;
} TAK_MapRow;

typedef struct TAK_MapQuery {
    char text[40];
    int  players;             /* 0 any, else exactly this many */
    int  size;                /* TAK_MAPSIZE_* */
    int  sort;                /* TAK_MAPSORT_* */
} TAK_MapQuery;

/* Lower case letters and digits, anything else one space, apostrophes
 * dropped, so "angvir" finds "Angvir's Maze". */
void TAK_MapQuery_Fold(const char *in, char *out, size_t cap);
/* By area in .ota units: up to 8 x 8, 12 x 12, 16 x 16, and beyond. */
int  TAK_MapQuery_SizeClass(int size_x, int size_y);
const char *TAK_MapQuery_SizeName(int size);
const char *TAK_MapQuery_SortName(int sort);
/* Any, then 2 to 8, stepping by by and wrapping. */
int  TAK_MapQuery_StepPlayers(int players, int by);
/* The rows the query shows, as indices into rows, in its order. Every
 * word of the text has to be in the name. Returns the count. */
int  TAK_MapQuery_Run(const TAK_MapRow *rows, int n, const TAK_MapQuery *q,
                      int *out);

/* The heading line over a list: the search and the three choosers. */
typedef struct TAK_MapStrip {
    SDL_Rect search, players, size, sort;
} TAK_MapStrip;

typedef struct TAK_MapBrowser {
    TAK_MapRow  *rows;
    int          count;
    int         *shown;
    int          shown_count;
    TAK_MapQuery q;
} TAK_MapBrowser;

/* Every map a skirmish can choose, each with its .ota read, in name
 * order. 0 on success. */
int  MapBrowser_Load(TAK_MapBrowser *b, const struct TranslateTable *tt);
void MapBrowser_Free(TAK_MapBrowser *b);
/* Run the query again after it changed. */
void MapBrowser_Refresh(TAK_MapBrowser *b);
/* Where row stands in the shown list, or -1 when the query hides it. */
int  MapBrowser_ShownAt(const TAK_MapBrowser *b, int row);
/* A press on the strip, by +1 for the left button and -1 for the right.
 * Returns 1 when the query changed. */
int  MapBrowser_StripPress(TAK_MapBrowser *b, const TAK_MapStrip *s,
                           int mx, int my, int by);
/* Typed characters and Backspace into the search. 1 when it changed. */
int  MapBrowser_Type(TAK_MapBrowser *b, const struct TAK_Platform *p,
                     int backspace);
void MapBrowser_DrawStrip(const TAK_MapBrowser *b, const TAK_MapStrip *s,
                          SDL_Surface *off, struct Font *font);

/* ── Start positions on a map's picture ─────────────────────────────── */

typedef struct TAK_StartMarks {
    int count;
    int x[TAK_MAP_STARTS_MAX], z[TAK_MAP_STARTS_MAX];   /* cells */
    /* The colour of the seat that claimed each start, or of the seat the
     * rule deals it to, -1 for none. claimed is 1 for a claim. */
    int colour[TAK_MAP_STARTS_MAX];
    int claimed[TAK_MAP_STARTS_MAX];
} TAK_StartMarks;

/* The starts of a lineup: claims, and with random starts off, where the
 * rest will stand. */
void StartMarks_FromConfig(TAK_StartMarks *m, const TAK_MapSummary *map,
                           const BattleConfig *cfg);

/* A map's picture fitted into area keeping its shape, centred. map_w and
 * map_h are the map's size in cells. */
SDL_Rect StartMap_Fit(SDL_Rect area, int content_w, int content_h);
/* Where start i lands in the picture's content rect. */
void StartMap_Point(SDL_Rect content, int map_w, int map_h,
                    const TAK_StartMarks *m, int i, int *x, int *y);
/* The start under a point, or -1. */
int  StartMap_Hit(SDL_Rect content, int map_w, int map_h,
                  const TAK_StartMarks *m, int x, int y);
void StartMap_Draw(SDL_Surface *off, SDL_Rect content, int map_w, int map_h,
                   const TAK_StartMarks *m, struct Font *font);

/* Claims on a lineup, as the skirmish screen makes them. start is 0
 * based. Take: seat claims start, swapping with the seat holding it, or
 * gives it back when it held it already. Move: the seat holding from
 * moves to to, swapping. Each returns 1 when something changed. */
int  StartClaims_Take(BattleConfig *cfg, int seat, int start, int count);
int  StartClaims_Move(BattleConfig *cfg, int from, int to, int count);
/* After the map or a seat changes: claims past the map's starts, on a
 * closed seat or held twice are let go. */
void StartClaims_Tidy(BattleConfig *cfg, int count);

#endif /* TAK_MAP_BROWSER_H */
