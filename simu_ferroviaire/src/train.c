/*
 * train.c - Comportement d'un train (exécuté dans son propre thread).
 *
 * Règle de sécurité (cantonnement) : un train n'entre dans la ressource
 * suivante qu'après l'avoir OBTENUE, et ne libère la ressource courante
 * qu'APRÈS être entré dans la suivante. Deux trains ne peuvent donc
 * jamais occuper la même section : c'est le mutex qui le garantit.
 *
 * Protocole avec le poste central (pipe) :
 *   - EV_WAIT est envoyé AVANT de se bloquer ;
 *   - EV_ACQ est envoyé APRÈS avoir obtenu la ressource ;
 *   - EV_REL est envoyé AVANT de libérer la ressource.
 * Ainsi l'ordre des messages dans le pipe est causalement cohérent :
 * le poste central ne voit jamais une ressource sur-occupée.
 */
#include <assert.h>
#include <stdlib.h>
#include "train.h"
#include "util.h"

int               g_evt_fd = -1;
pthread_barrier_t g_start_barrier;

typedef enum { ST_OK, ST_BACKOFF, ST_STOP } st_t;

static void emit(train_t *t, int type, int res)
{
    msg_send(g_evt_fd, type, t->id, res);
}

static int should_abort(void *ctx)
{
    train_t *t = ctx;
    return atomic_load(&g_stop) || atomic_load(&t->backoff);
}

static int rnd(train_t *t, int lo, int hi)
{
    return lo + (int)(rand_r(&t->seed) % (unsigned)(hi - lo + 1));
}

/* ---------- Ensemble des ressources détenues ---------- */

static int holds(const train_t *t, int r)
{
    for (int i = 0; i < t->nheld; i++)
        if (t->held[i] == r)
            return 1;
    return 0;
}

static void take(train_t *t, int r)        /* appelé après verrouillage */
{
    assert(t->nheld < MAX_HELD);
    t->held[t->nheld++] = r;
    emit(t, EV_ACQ, r);
}

static void drop(train_t *t, int r)        /* REL envoyé avant libération */
{
    for (int i = 0; i < t->nheld; i++)
        if (t->held[i] == r) {
            t->held[i] = t->held[--t->nheld];
            emit(t, EV_REL, r);
            res_unlock(r);
            return;
        }
}

static void drop_zone_if_unused(train_t *t, int z)
{
    if (z < 0 || !holds(t, z))
        return;
    for (int i = 0; i < t->nheld; i++)
        if (t->held[i] != z && g_res[t->held[i]].zone == z)
            return;
    drop(t, z);
}

static void leave(train_t *t, int r)
{
    if (!holds(t, r))
        return;
    drop(t, r);
    drop_zone_if_unused(t, g_res[r].zone);
}

static void release_all(train_t *t)
{
    while (t->nheld > 0)
        drop(t, t->held[t->nheld - 1]);
}

/* ---------- Temporisations ---------- */

static void dwell(train_t *t, int r)
{
    int ms = (int)(g_res[r].base_ms / t->def->speed);
    ms = ms * rnd(t, 80, 120) / 100;
    for (int el = 0; el < ms && !atomic_load(&g_stop); el += 50)
        sleep_ms(ms - el < 50 ? ms - el : 50);
}

/* Dégagement sur voie de garage : ordre du poste central pour
 * casser un interblocage. Le train libère tout ce qu'il occupe. */
static void go_siding(train_t *t)
{
    release_all(t);
    emit(t, EV_SIDING, -1);
    int ms = rnd(t, 1000, 2000);
    for (int el = 0; el < ms && !atomic_load(&g_stop); el += 50)
        sleep_ms(50);
    emit(t, EV_RESUME, -1);
}

/* ---------- Acquisition ---------- */

static st_t lock_one(train_t *t, int r)
{
    if (res_trylock(r, t->def->prio) == 0) {
        take(t, r);
        return ST_OK;
    }
    emit(t, EV_WAIT, r);
    if (res_lock(r, t->def->prio, should_abort, t) == 0) {
        take(t, r);
        return ST_OK;
    }
    if (atomic_load(&g_stop))
        return ST_STOP;
    atomic_store(&t->backoff, 0);
    return ST_BACKOFF;
}

/* Entrée dans une ressource : jeton de zone d'abord, puis la ressource. */
static st_t enter(train_t *t, int r)
{
    atomic_store(&t->backoff, 0);   /* ignore un ordre périmé */
    int z = g_res[r].zone;
    if (z >= 0 && !holds(t, z)) {
        st_t st = lock_one(t, z);
        if (st != ST_OK)
            return st;
    }
    return lock_one(t, r);
}

/* ---------- Mode DÉTECTION : cantonnement pas à pas ---------- */

static void run_detection(train_t *t)
{
    const train_def_t *d = t->def;
    int k = 0;
    while (!atomic_load(&g_stop)) {
        dwell(t, d->steps[k]);
        if (atomic_load(&g_stop))
            break;
        int nx = (k + 1) % d->len;
        st_t st;
        while ((st = enter(t, d->steps[nx])) == ST_BACKOFF) {
            go_siding(t);
            if (atomic_load(&g_stop))
                return;
        }
        if (st == ST_STOP)
            return;
        leave(t, d->steps[k]);
        if (nx == 0) {
            t->trips++;
            emit(t, EV_TRIP, d->steps[0]);
        }
        k = nx;
    }
}

/* ---------- Mode PRÉVENTION : réservation d'itinéraire ---------- */

static int next_station(const train_def_t *d, int k)
{
    for (int i = 1; i <= d->len; i++) {
        int idx = (k + i) % d->len;
        if (g_res[d->steps[idx]].type == RES_STATION)
            return idx;
    }
    return (k + 1) % d->len;
}

static int in_list(const int *l, int n, int v)
{
    for (int i = 0; i < n; i++)
        if (l[i] == v)
            return 1;
    return 0;
}

/* Réserve atomiquement (tout-ou-rien) toutes les ressources jusqu'à la
 * prochaine gare incluse : un train ne s'engage JAMAIS sur une voie unique
 * sans avoir un quai réservé à l'autre bout. La condition « attente en
 * détenant » est supprimée pour les sections de voie. */
static st_t reserve_route(train_t *t, int k, int j)
{
    const train_def_t *d = t->def;
    int last_fail = -1;

    for (;;) {
        if (atomic_load(&g_stop))
            return ST_STOP;
        if (atomic_exchange(&t->backoff, 0)) {
            go_siding(t);
            last_fail = -1;
            continue;
        }

        int need[MAX_STEPS * 2], n = 0;
        for (int i = k + 1;; i++) {
            int idx = i % d->len, r = d->steps[idx], z = g_res[r].zone;
            if (z >= 0 && !holds(t, z) && !in_list(need, n, z))
                need[n++] = z;
            if (!holds(t, r) && !in_list(need, n, r))
                need[n++] = r;
            if (idx == j)
                break;
        }

        int got = 0, fail = -1;
        for (int i = 0; i < n; i++) {
            if (res_trylock(need[i], d->prio) == 0) {
                take(t, need[i]);
                got++;
            } else {
                fail = need[i];
                break;
            }
        }
        if (fail < 0)
            return ST_OK;

        for (int i = got - 1; i >= 0; i--)   /* retour arrière */
            drop(t, need[i]);
        if (fail != last_fail) {
            emit(t, EV_WAIT, fail);
            last_fail = fail;
        }
        /* Les trains moins prioritaires réessaient moins souvent */
        int ms = 40 + (PRIO_MAX - d->prio) * 40 + rnd(t, 0, 60);
        for (int el = 0; el < ms && !should_abort(t); el += 20)
            sleep_ms(20);
    }
}

static void run_prevention(train_t *t)
{
    const train_def_t *d = t->def;
    int k = 0;
    while (!atomic_load(&g_stop)) {
        dwell(t, d->steps[k]);
        if (atomic_load(&g_stop))
            break;
        int j = next_station(d, k);
        if (reserve_route(t, k, j) == ST_STOP)
            return;
        int prev = k;
        for (int i = k + 1;; i++) {
            int idx = i % d->len;
            leave(t, d->steps[prev]);
            if (idx == 0) {
                t->trips++;
                emit(t, EV_TRIP, d->steps[0]);
            }
            if (idx == j)
                break;
            dwell(t, d->steps[idx]);
            if (atomic_load(&g_stop))
                return;
            prev = idx;
        }
        k = j;
    }
}

void *train_thread(void *arg)
{
    train_t *t = arg;
    emit(t, EV_START, t->def->steps[0]);

    /* Placement initial en gare d'origine */
    while (enter(t, t->def->steps[0]) != ST_OK)
        ;
    pthread_barrier_wait(&g_start_barrier);

    if (g_mode == MODE_PREVENTION)
        run_prevention(t);
    else
        run_detection(t);

    release_all(t);                 /* rentre au dépôt */
    emit(t, EV_END, -1);
    return NULL;
}
