/*
 * train.h - Un train = un thread POSIX indépendant.
 */
#ifndef TRAIN_H
#define TRAIN_H

#include <pthread.h>
#include "common.h"
#include "network.h"

typedef struct {
    int                id;
    const train_def_t *def;
    pthread_t          th;
    atomic_int         backoff;      /* ordre de dégagement reçu du poste */
    int                held[MAX_HELD];
    int                nheld;
    unsigned           seed;
    int                trips;
} train_t;

extern int               g_evt_fd;   /* extrémité écriture du pipe d'événements */
extern pthread_barrier_t g_start_barrier;

void *train_thread(void *arg);

#endif
