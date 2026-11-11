/*
 * common.h - Types et constantes partagés par tout le simulateur.
 *
 * Simulateur de circulation ferroviaire multithread.
 * Projet de programmation concurrente (threads POSIX, mutex,
 * sémaphores, pipes, processus de contrôle).
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>

#define MAX_RES     64      /* nombre max de ressources (sections, gares...) */
#define MAX_TRAINS  16      /* nombre max de trains (bitmask sur 32 bits)    */
#define MAX_STEPS   64      /* nombre max d'étapes dans un itinéraire        */
#define MAX_HELD    24      /* ressources détenues simultanément par un train*/

#define PRIO_MIN    1       /* FRET */
#define PRIO_MAX    3       /* TGV  */
#define PRIO_LEVELS (PRIO_MAX + 1)

#define SLICE_MS    100     /* granularité des attentes temporisées          */

/* Types de ressources partagées du réseau */
typedef enum {
    RES_SECTION,   /* section de voie : protégée par un MUTEX            */
    RES_STATION,   /* gare : SÉMAPHORE compteur (nombre de quais)        */
    RES_SWITCH,    /* aiguillage : SÉMAPHORE binaire                     */
    RES_CROSSING,  /* carrefour (croisement à niveau) : porte à priorité */
    RES_ZONE       /* zone de régulation : SÉMAPHORE compteur            */
} res_type_t;

/* Stratégie de gestion des interblocages */
typedef enum {
    MODE_DETECTION,   /* cantonnement naïf + détection/résolution centrale */
    MODE_PREVENTION   /* réservation d'itinéraire tout-ou-rien + détection */
} sim_mode_t;

/* Messages train -> poste central (pipe "événements") */
typedef enum {
    EV_START = 1,   /* le train est créé                                 */
    EV_WAIT,        /* le train attend la ressource res                  */
    EV_ACQ,         /* le train a obtenu la ressource res                */
    EV_REL,         /* le train libère la ressource res (AVANT libération)*/
    EV_SIDING,      /* le train se dégage sur une voie de garage         */
    EV_RESUME,      /* le train quitte la voie de garage                 */
    EV_TRIP,        /* le train a bouclé son itinéraire                  */
    EV_END          /* le train rentre au dépôt (fin de simulation)      */
} ev_type_t;

/* Messages poste central -> trains (pipe "commandes") */
typedef enum {
    CMD_BACKOFF = 1  /* ordre de dégagement (résolution d'interblocage) */
} cmd_type_t;

/* Message de taille fixe : une écriture <= PIPE_BUF est atomique,
 * donc plusieurs threads peuvent écrire dans le même pipe sans
 * entrelacement des octets. */
typedef struct {
    int32_t type;
    int32_t train;
    int32_t res;
    int32_t pad;
    int64_t t_ms;
} msg_t;

_Static_assert(sizeof(msg_t) <= PIPE_BUF, "msg_t doit tenir dans PIPE_BUF");

/* Variables globales de configuration (définies dans main.c) */
extern sim_mode_t  g_mode;
extern atomic_int  g_stop;
extern int         g_verbose;
extern int         g_color;
extern int         g_ntrains_active;

#endif /* COMMON_H */
