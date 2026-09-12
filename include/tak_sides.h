#ifndef TAK_SIDES_H
#define TAK_SIDES_H

#include <stddef.h>

/* The sides gamedata/sidedata.tdf defines. The original reads SIDE0,
 * SIDE1 and on until one is missing and keeps that count
 * (legacy:164568-164578), so the base game has seven and Iron Plague's
 * copy adds SIDE7 CREON. A side without a commander is never offered to
 * a player (legacy:134964-134972), which leaves out LIFEFORMS,
 * NONPLAYERCHARACTERS and WANDERING_MONSTERS. Side numbers are the
 * SIDEn index, so Creon is 7 wherever a side is stored. */

#define TAK_SIDES_MAX 16

typedef struct TakSideInfo {
    char name[32];                  /* "ARAMON" */
    char prefix[8];                 /* nameprefix, "ARA" */
    char commander[32];             /* the monarch's unitname, "" if none */
    char logogaf[32];               /* badge sheet stem, "colorlogos2" */
    char logoart[32];               /* badge entry, "arateam" */
    char buildsparkle[32];          /* buildsparklygaf */
    char buildsparkle_anim[32];     /* buildsparklyanim */
    char resurrectsparkle[32];      /* resurrectsparklygaf */
    char resurrectsparkle_anim[32]; /* resurrectsparklyanim */
} TakSideInfo;

/* How the side setter treats a choice (legacy:134883-134942). */
typedef enum {
    TAK_SIDES_CAMPAIGN    = 1,  /* the mission decides, nothing turned back */
    TAK_SIDES_SKIRMISH    = 2,  /* sides past Zhon need the expansion */
    TAK_SIDES_MULTIPLAYER = 3   /* ...and a game that allows Creon */
} TakSidesMode;

/* The table loads on first use and again after the VFS is remounted. */
int                Sides_Count(void);
const TakSideInfo *Sides_Get(int side);          /* NULL out of range */
int                Sides_IsPlayable(int side);   /* has a commander */

/* The side button: the next side with a commander, wrapping at the
 * count (legacy:134955-134972). */
int Sides_Next(int side);

/* The side setter. Campaign keeps the side. Skirmish turns a side past
 * the fourth back to Aramon when the expansion is absent
 * (legacy:134933-134936). Multiplayer does the same unless the
 * expansion is present and the game allows Creon (legacy:134910-134923). */
int Sides_Set(int side, TakSidesMode mode, int creon_allowed);

/* One press of the side button: Next, then the setter. */
int Sides_Cycle(int side, TakSidesMode mode, int creon_allowed);

/* The name as the lobby shows it: sidedata's name with all but the
 * first letter lowered, so "CREON" reads "Creon" (legacy:136342-136361).
 * Empty for a side the table does not hold. */
void Sides_DisplayName(int side, char *out, size_t cap);

/* Side index by nameprefix ("CRE") or name ("creon"), -1 if none. */
int Sides_FindByPrefix(const char *prefix);
int Sides_FindByName(const char *name);

/* The side a mission's PlayerN line names: the first side, in SIDEn
 * order, whose name the line holds anywhere, case aside
 * (legacy:169026-169095). -1 when it names none. */
int Sides_FindInText(const char *text);

#endif /* TAK_SIDES_H */
