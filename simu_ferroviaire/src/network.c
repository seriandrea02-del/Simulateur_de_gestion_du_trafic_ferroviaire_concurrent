/*
 * network.c - Réseau ferroviaire lorrain (fictif mais plausible)
 *             et primitives de synchronisation.
 *
 *                         THIONVILLE (2 quais)
 *                             |  ||  double voie (S_M_THIO / S_THIO_M)
 *   WOIPPY triage ===S_WOI=== X  carrefour de Woippy (porte à priorité)
 *     (2 voies)               |
 *                 +-----------+------------------+
 *                 |  ZONE "Noeud de Metz" (cap 4)|
 *                 |   A_SUD -- METZ (3) -- A_EST  |
 *                 +----|---------------------|----+
 *        double voie   |                     |  VOIE UNIQUE  S_M_SA
 *                 PONT-A-MOUSSON (2)     SAINT-AVOLD (2, croisement)
 *        double voie   |                     |  VOIE UNIQUE  S_SA_F
 *                  NANCY (2)              FORBACH (1 quai, terminus)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "network.h"
#include "util.h"

enum {
    G_METZ, G_NANCY, G_THIO, G_PAM, G_STAVOLD, G_FORBACH, G_WOIPPY,
    S_M_PAM, S_PAM_M, S_PAM_N, S_N_PAM, S_M_THIO, S_THIO_M,
    S_M_SA, S_SA_F, S_WOI,
    A_SUD, A_EST, X_WOIPPY, Z_METZ,
    NRES
};

#define ST(n, cap, z, ms) { n, RES_STATION,  cap, z, ms, 0 }
#define SE(n, z, ms, s)   { n, RES_SECTION,  1,   z, ms, s }
#define SW(n, z, ms)      { n, RES_SWITCH,   1,   z, ms, 0 }
#define XR(n, z, ms)      { n, RES_CROSSING, 1,   z, ms, 0 }
#define ZO(n, cap)        { n, RES_ZONE,     cap, -1, 0, 0 }

resource_t g_res[MAX_RES] = {
    [G_METZ]    = ST("Gare de Metz",            3, Z_METZ, 1000),
    [G_NANCY]   = ST("Gare de Nancy",           2, -1,      900),
    [G_THIO]    = ST("Gare de Thionville",      2, -1,      800),
    [G_PAM]     = ST("Gare de Pont-a-Mousson",  2, -1,      600),
    [G_STAVOLD] = ST("Gare de Saint-Avold",     2, -1,      600),
    [G_FORBACH] = ST("Gare de Forbach",         1, -1,      800),
    [G_WOIPPY]  = ST("Triage de Woippy",        2, -1,     1000),

    [S_M_PAM]   = SE("Section Metz->PaM",          -1, 1100, 0),
    [S_PAM_M]   = SE("Section PaM->Metz",          -1, 1100, 0),
    [S_PAM_N]   = SE("Section PaM->Nancy",         -1, 1000, 0),
    [S_N_PAM]   = SE("Section Nancy->PaM",         -1, 1000, 0),
    [S_M_THIO]  = SE("Section Metz->Thionville",   -1, 1200, 0),
    [S_THIO_M]  = SE("Section Thionville->Metz",   -1, 1200, 0),
    [S_M_SA]    = SE("Voie unique Metz-St-Avold",  -1, 1400, 1),
    [S_SA_F]    = SE("Voie unique St-Avold-Forbach",-1, 1000, 1),
    [S_WOI]     = SE("Raccordement Woippy",        -1,  600, 1),

    [A_SUD]     = SW("Aiguillage Metz-Sud", Z_METZ, 200),
    [A_EST]     = SW("Aiguillage Metz-Est", Z_METZ, 200),
    [X_WOIPPY]  = XR("Carrefour de Woippy", Z_METZ, 300),
    [Z_METZ]    = ZO("Zone Noeud de Metz", 4),
};
int g_nres = NRES;

train_def_t g_tdef[MAX_TRAINS] = {
    { "TGV 2401", "TGV", 3, 1.7, 16, {
        G_NANCY, S_N_PAM, G_PAM, S_PAM_M, A_SUD, G_METZ, X_WOIPPY, S_M_THIO,
        G_THIO, S_THIO_M, X_WOIPPY, G_METZ, A_SUD, S_M_PAM, G_PAM, S_PAM_N } },
    { "TER 835A", "TER", 2, 1.0, 10, {
        G_METZ, A_EST, S_M_SA, G_STAVOLD, S_SA_F, G_FORBACH, S_SA_F,
        G_STAVOLD, S_M_SA, A_EST } },
    { "TER 835B", "TER", 2, 1.0, 10, {
        G_FORBACH, S_SA_F, G_STAVOLD, S_M_SA, A_EST, G_METZ, A_EST,
        S_M_SA, G_STAVOLD, S_SA_F } },
    { "FRET 4410", "FRET", 1, 0.7, 14, {
        G_WOIPPY, S_WOI, X_WOIPPY, A_EST, S_M_SA, G_STAVOLD, S_SA_F,
        G_FORBACH, S_SA_F, G_STAVOLD, S_M_SA, A_EST, X_WOIPPY, S_WOI } },
    { "TER 836", "TER", 2, 1.1, 10, {
        G_METZ, A_SUD, S_M_PAM, G_PAM, S_PAM_N, G_NANCY, S_N_PAM, G_PAM,
        S_PAM_M, A_SUD } },
    { "TER 837", "TER", 2, 1.1, 16, {
        G_THIO, S_THIO_M, X_WOIPPY, G_METZ, A_SUD, S_M_PAM, G_PAM, S_PAM_N,
        G_NANCY, S_N_PAM, G_PAM, S_PAM_M, A_SUD, G_METZ, X_WOIPPY, S_M_THIO } },
    { "FRET 4520", "FRET", 1, 0.8, 8, {
        G_WOIPPY, S_WOI, X_WOIPPY, S_M_THIO, G_THIO, S_THIO_M, X_WOIPPY,
        S_WOI } },
};
int g_ntdef = 7;

const char *res_type_name(res_type_t t)
{
    switch (t) {
    case RES_SECTION:  return "section";
    case RES_STATION:  return "gare";
    case RES_SWITCH:   return "aiguillage";
    case RES_CROSSING: return "carrefour";
    case RES_ZONE:     return "zone";
    }
    return "?";
}

/* Vérifie la cohérence du réseau : itinéraires commençant par une gare,
 * ressources valides, placement initial compatible avec les capacités. */
int network_validate(int ntrains)
{
    int occ[MAX_RES] = { 0 };
    for (int t = 0; t < ntrains; t++) {
        const train_def_t *d = &g_tdef[t];
        if (d->len < 2 || g_res[d->steps[0]].type != RES_STATION) {
            fprintf(stderr, "%s : itineraire invalide\n", d->name);
            return -1;
        }
        if (d->prio < PRIO_MIN || d->prio > PRIO_MAX)
            return -1;
        for (int i = 0; i < d->len; i++) {
            int r = d->steps[i];
            if (r < 0 || r >= g_nres || g_res[r].type == RES_ZONE)
                return -1;
            if (d->steps[(i + 1) % d->len] == r)  /* auto-blocage */
                return -1;
        }
        int s0 = d->steps[0];
        occ[s0]++;
        if (g_res[s0].zone >= 0)
            occ[g_res[s0].zone]++;
    }
    for (int r = 0; r < g_nres; r++)
        if (occ[r] > g_res[r].capacity) {
            fprintf(stderr, "Placement initial : %s saturee\n", g_res[r].name);
            return -1;
        }
    return 0;
}

void network_init_sync(void)
{
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    /* ERRORCHECK : un déverrouillage par un thread non propriétaire
     * est détecté (garde-fou pendant le développement). */
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);

    for (int r = 0; r < g_nres; r++) {
        resource_t *x = &g_res[r];
        switch (x->type) {
        case RES_SECTION:
            pthread_mutex_init(&x->mtx, &ma);
            break;
        case RES_STATION:
        case RES_SWITCH:
        case RES_ZONE:
            sem_init(&x->sem, 0, (unsigned)x->capacity);
            break;
        case RES_CROSSING:
            pthread_mutex_init(&x->gate.m, NULL);
            pthread_cond_init(&x->gate.c, NULL);
            x->gate.busy = 0;
            memset(x->gate.waiting, 0, sizeof x->gate.waiting);
            break;
        }
    }
    pthread_mutexattr_destroy(&ma);
}

void network_destroy_sync(void)
{
    for (int r = 0; r < g_nres; r++) {
        resource_t *x = &g_res[r];
        switch (x->type) {
        case RES_SECTION:  pthread_mutex_destroy(&x->mtx); break;
        case RES_STATION:
        case RES_SWITCH:
        case RES_ZONE:     sem_destroy(&x->sem); break;
        case RES_CROSSING:
            pthread_cond_destroy(&x->gate.c);
            pthread_mutex_destroy(&x->gate.m);
            break;
        }
    }
}

/* ---------- Porte à priorité (carrefours) ---------- */

static int higher_waiting(const prio_gate_t *g, int prio)
{
    for (int p = prio + 1; p <= PRIO_MAX; p++)
        if (g->waiting[p] > 0)
            return 1;
    return 0;
}

static int gate_trylock(prio_gate_t *g, int prio)
{
    int ok = 0;
    pthread_mutex_lock(&g->m);
    if (!g->busy && !higher_waiting(g, prio)) {
        g->busy = 1;
        ok = 1;
    }
    pthread_mutex_unlock(&g->m);
    return ok ? 0 : -1;
}

static int gate_lock(prio_gate_t *g, int prio, abort_fn ab, void *ctx)
{
    pthread_mutex_lock(&g->m);
    g->waiting[prio]++;
    for (;;) {
        if (!g->busy && !higher_waiting(g, prio)) {
            g->waiting[prio]--;
            g->busy = 1;
            pthread_mutex_unlock(&g->m);
            return 0;
        }
        if (ab && ab(ctx)) {
            g->waiting[prio]--;
            /* un train moins prioritaire peut maintenant être éligible */
            pthread_cond_broadcast(&g->c);
            pthread_mutex_unlock(&g->m);
            return -1;
        }
        struct timespec ts;
        abs_deadline(&ts, SLICE_MS);
        pthread_cond_timedwait(&g->c, &g->m, &ts);
    }
}

static void gate_unlock(prio_gate_t *g)
{
    pthread_mutex_lock(&g->m);
    g->busy = 0;
    pthread_cond_broadcast(&g->c);   /* le plus prioritaire passera */
    pthread_mutex_unlock(&g->m);
}

/* ---------- API générique ---------- */

int res_trylock(int rid, int prio)
{
    resource_t *x = &g_res[rid];
    switch (x->type) {
    case RES_SECTION:
        return pthread_mutex_trylock(&x->mtx) == 0 ? 0 : -1;
    case RES_STATION:
    case RES_SWITCH:
    case RES_ZONE:
        return sem_trywait(&x->sem) == 0 ? 0 : -1;
    case RES_CROSSING:
        return gate_trylock(&x->gate, prio);
    }
    return -1;
}

int res_lock(int rid, int prio, abort_fn ab, void *ctx)
{
    resource_t *x = &g_res[rid];
    if (x->type == RES_CROSSING)
        return gate_lock(&x->gate, prio, ab, ctx);

    for (;;) {
        if (ab && ab(ctx))
            return -1;
        struct timespec ts;
        abs_deadline(&ts, SLICE_MS);
        if (x->type == RES_SECTION) {
            int rc = pthread_mutex_timedlock(&x->mtx, &ts);
            if (rc == 0)
                return 0;
            if (rc != ETIMEDOUT) {
                fprintf(stderr, "timedlock(%s): %s\n", x->name, strerror(rc));
                abort();
            }
        } else {
            if (sem_timedwait(&x->sem, &ts) == 0)
                return 0;
            if (errno != ETIMEDOUT && errno != EINTR) {
                perror("sem_timedwait");
                abort();
            }
        }
    }
}

void res_unlock(int rid)
{
    resource_t *x = &g_res[rid];
    switch (x->type) {
    case RES_SECTION: {
        int rc = pthread_mutex_unlock(&x->mtx);
        if (rc != 0) {
            fprintf(stderr, "unlock(%s): %s\n", x->name, strerror(rc));
            abort();
        }
        break;
    }
    case RES_STATION:
    case RES_SWITCH:
    case RES_ZONE:
        sem_post(&x->sem);
        break;
    case RES_CROSSING:
        gate_unlock(&x->gate);
        break;
    }
}
