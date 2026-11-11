/*
 * network.h - Description du réseau ferroviaire et primitives
 *             de synchronisation associées à chaque ressource.
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <pthread.h>
#include <semaphore.h>
#include "common.h"

/* Porte à priorité pour les carrefours : mutex + variable de condition.
 * Quand le carrefour se libère, le train de plus haute priorité en
 * attente passe en premier (TGV > TER > FRET). */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             busy;
    int             waiting[PRIO_LEVELS];
} prio_gate_t;

typedef struct {
    const char *name;
    res_type_t  type;
    int         capacity;   /* 1 pour section/aiguillage/carrefour   */
    int         zone;       /* id de la ressource ZONE englobante ou -1 */
    int         base_ms;    /* temps de parcours / d'arrêt de base   */
    int         single;     /* 1 = voie unique (circulation 2 sens)  */

    /* Primitives (une seule utilisée selon le type) */
    pthread_mutex_t mtx;    /* RES_SECTION                          */
    sem_t           sem;    /* RES_STATION / RES_SWITCH / RES_ZONE  */
    prio_gate_t     gate;   /* RES_CROSSING                         */
} resource_t;

typedef struct {
    const char *name;
    const char *kind;       /* "TGV", "TER", "FRET" */
    int         prio;
    double      speed;      /* facteur de vitesse   */
    int         len;
    int         steps[MAX_STEPS];  /* itinéraire bouclé : steps[0] = gare */
} train_def_t;

extern resource_t  g_res[];
extern int         g_nres;
extern train_def_t g_tdef[];
extern int         g_ntdef;

typedef int (*abort_fn)(void *ctx);

int         network_validate(int ntrains);
void        network_init_sync(void);
void        network_destroy_sync(void);
const char *res_type_name(res_type_t t);

/* 0 si obtenu, -1 sinon (non bloquant) */
int  res_trylock(int rid, int prio);
/* Bloquant, mais interruptible : abort(ctx) est consulté toutes les
 * SLICE_MS ms. 0 si obtenu, -1 si abandon. */
int  res_lock(int rid, int prio, abort_fn ab, void *ctx);
void res_unlock(int rid);

#endif
