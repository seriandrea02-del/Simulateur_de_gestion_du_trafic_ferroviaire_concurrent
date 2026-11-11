/*
 * control.c - Poste central de commande (PCC).
 *
 * Processus indépendant (créé par fork) qui :
 *   1. reconstruit l'état du réseau à partir des messages reçus des
 *      trains par le pipe d'événements ;
 *   2. vérifie l'invariant de sécurité (jamais plus de trains que la
 *      capacité d'une ressource => aucune collision) ;
 *   3. surveille les priorités (inversion : un TGV bloqué par un FRET) ;
 *   4. détecte les interblocages par l'algorithme de RÉDUCTION du graphe
 *      d'allocation (Holt), valable pour des ressources multi-exemplaires ;
 *   5. résout les interblocages en ordonnant à un train « victime »
 *      (le moins prioritaire) de se dégager sur une voie de garage,
 *      via le pipe de commandes ;
 *   6. affiche un tableau de bord et un bilan final.
 */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "control.h"
#include "network.h"
#include "util.h"

typedef enum { TS_INIT, TS_RUN, TS_WAIT, TS_SIDING, TS_DONE } tstate_t;

static const char *state_name[] = {
    "initial", "en marche", "EN ATTENTE", "VOIE GARAGE", "au depot"
};

static int      N;
static int      cnt[MAX_RES];
static unsigned char hold[MAX_TRAINS][MAX_RES];
static int      waiting[MAX_TRAINS];
static int64_t  wait_since[MAX_TRAINS];
static tstate_t state[MAX_TRAINS];
static int      pos[MAX_TRAINS];
static int      trips[MAX_TRAINS];
static int64_t  total_wait[MAX_TRAINS];
static int      nwaits[MAX_TRAINS];
static int      nsiding[MAX_TRAINS];
static int      inv_reported[MAX_TRAINS];
static int      starv_reported[MAX_TRAINS];
static int64_t  max_wait[MAX_TRAINS];

static int      st_deadlocks, st_violations, st_proto_err;
static int      st_inversions, st_starvation, st_events;

/* ---------- Affichage ---------- */

static const char *tcol(int t)
{
    static const char *c[] = { "\033[1;35m", "\033[1;32m", "\033[1;36m",
                               "\033[1;33m", "\033[1;34m", "\033[0;32m",
                               "\033[0;33m", "\033[0;36m" };
    return g_color ? c[t % 8] : "";
}
static const char *RST(void) { return g_color ? "\033[0m" : ""; }
static const char *RED(void) { return g_color ? "\033[1;31m" : ""; }
static const char *BLD(void) { return g_color ? "\033[1m" : ""; }

static void stamp(int64_t ms)
{
    printf("[%7.3fs] ", ms / 1000.0);
}

static const char *tname(int t) { return g_tdef[t].name; }

static void print_holders(int r)
{
    int first = 1;
    for (int h = 0; h < N; h++)
        if (hold[h][r]) {
            printf("%s%s", first ? "" : ", ", tname(h));
            first = 0;
        }
    if (first)
        printf("personne");
}

static void dashboard(int64_t now)
{
    printf("%s", BLD());
    printf("+----------------------------------------------------------------------------+\n");
    printf("| PCC - tableau de bord   t = %6.1fs   mode = %-11s                     |\n",
           now / 1000.0, g_mode == MODE_DETECTION ? "detection" : "prevention");
    printf("+------------+------+-------------+-------------------------------+---------+\n");
    printf("| Train      | Prio | Etat        | Position                      | Trajets |\n");
    printf("+------------+------+-------------+-------------------------------+---------+\n");
    printf("%s", RST());
    for (int t = 0; t < N; t++) {
        const char *p = pos[t] >= 0 ? g_res[pos[t]].name : "-";
        if (state[t] == TS_SIDING)
            p = "voie de garage";
        printf("| %s%-10s%s | %4d | %-11s | %-29.29s | %7d |\n",
               tcol(t), tname(t), RST(), g_tdef[t].prio,
               state_name[state[t]], p, trips[t]);
    }
    printf("+------------+------+-------------+-------------------------------+---------+\n");
    printf("  Interblocages resolus : %d   Inversions de priorite : %d   Violations : %s%d%s\n\n",
           st_deadlocks, st_inversions,
           st_violations ? RED() : "", st_violations, RST());
    fflush(stdout);
}

/* ---------- Mise à jour du modèle ---------- */

static void on_event(const msg_t *m)
{
    int t = m->train, r = m->res;
    st_events++;
    if (t < 0 || t >= N)
        return;

    switch (m->type) {
    case EV_START:
        state[t] = TS_INIT;
        if (g_verbose) {
            stamp(m->t_ms);
            printf("%s%-10s%s mis en service a %s\n", tcol(t), tname(t), RST(), g_res[r].name);
        }
        break;

    case EV_WAIT:
        if (waiting[t] != r) {
            waiting[t] = r;
            wait_since[t] = m->t_ms;
            inv_reported[t] = 0;
            starv_reported[t] = 0;
            if (g_res[r].type != RES_ZONE || g_verbose) {
                stamp(m->t_ms);
                printf("%s%-10s%s attend  %-30s (occupe par : ", tcol(t), tname(t), RST(), g_res[r].name);
                print_holders(r);
                printf(")\n");
            }
        }
        state[t] = TS_WAIT;
        break;

    case EV_ACQ:
        if (cnt[r] >= g_res[r].capacity) {
            st_violations++;
            stamp(m->t_ms);
            printf("%s!!! ALERTE SECURITE : %s depasse sa capacite (%d) a l'entree de %s%s\n",
                   RED(), g_res[r].name, g_res[r].capacity, tname(t), RST());
        }
        cnt[r]++;
        hold[t][r]++;
        if (waiting[t] == r) {
            int64_t w = m->t_ms - wait_since[t];
            total_wait[t] += w;
            nwaits[t]++;
            if (w > max_wait[t])
                max_wait[t] = w;
            waiting[t] = -1;
        }
        if (waiting[t] < 0)
            state[t] = TS_RUN;
        if (g_res[r].type != RES_ZONE)
            pos[t] = r;
        if (g_verbose || g_res[r].type == RES_STATION) {
            stamp(m->t_ms);
            printf("%s%-10s%s %s %s\n", tcol(t), tname(t), RST(),
                   g_res[r].type == RES_STATION ? "-> quai  " : "occupe  ", g_res[r].name);
        }
        break;

    case EV_REL:
        if (hold[t][r] == 0 || cnt[r] == 0) {
            st_proto_err++;
            break;
        }
        hold[t][r]--;
        cnt[r]--;
        if (g_verbose) {
            stamp(m->t_ms);
            printf("%s%-10s%s libere  %s\n", tcol(t), tname(t), RST(), g_res[r].name);
        }
        break;

    case EV_SIDING:
        state[t] = TS_SIDING;
        if (waiting[t] >= 0) {
            int64_t w = m->t_ms - wait_since[t];
            total_wait[t] += w;
            nwaits[t]++;
            if (w > max_wait[t])
                max_wait[t] = w;
        }
        waiting[t] = -1;
        nsiding[t]++;
        stamp(m->t_ms);
        printf("%s%-10s%s se degage sur une voie de garage (ordre du PCC)\n", tcol(t), tname(t), RST());
        break;

    case EV_RESUME:
        state[t] = TS_RUN;
        stamp(m->t_ms);
        printf("%s%-10s%s quitte la voie de garage\n", tcol(t), tname(t), RST());
        break;

    case EV_TRIP:
        trips[t]++;
        stamp(m->t_ms);
        printf("%s%-10s%s a boucle son itineraire (%d)\n", tcol(t), tname(t), RST(), trips[t]);
        break;

    case EV_END:
        state[t] = TS_DONE;
        waiting[t] = -1;
        break;
    }
}

/* ---------- Détection d'interblocage (réduction de Holt) ----------
 *
 * D = trains bloqués. On retire itérativement de D tout train dont la
 * ressource attendue : (a) a encore un exemplaire libre, ou (b) est
 * détenue par au moins un train hors de D (qui finira par la libérer).
 * À point fixe, D non vide <=> interblocage (chaque train attend une
 * seule ressource à la fois : modèle « single-request »). */
static unsigned deadlock_set(void)
{
    unsigned D = 0;
    for (int t = 0; t < N; t++)
        if (waiting[t] >= 0 && state[t] == TS_WAIT)
            D |= 1u << t;

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int t = 0; t < N; t++) {
            if (!(D & (1u << t)))
                continue;
            int r = waiting[t];
            int reducible = cnt[r] < g_res[r].capacity;
            for (int h = 0; h < N && !reducible; h++)
                if (hold[h][r] && !(D & (1u << h)))
                    reducible = 1;
            if (reducible) {
                D &= ~(1u << t);
                changed = 1;
            }
        }
    }
    return D;
}

static int holds_any(int t)
{
    for (int r = 0; r < g_nres; r++)
        if (hold[t][r])
            return 1;
    return 0;
}

static unsigned last_D;
static int      confirm;
static int64_t  global_cooldown;

static void check_deadlock(int64_t now, int cmd_fd)
{
    unsigned D = deadlock_set();
    if (!D) {
        last_D = 0;
        confirm = 0;
        return;
    }
    /* On exige que le même ensemble persiste sur 2 contrôles successifs
     * pour éviter de réagir sur un état transitoire. */
    if (D == last_D)
        confirm++;
    else {
        last_D = D;
        confirm = 1;
    }
    if (confirm < 2 || now < global_cooldown)
        return;

    int victim = -1;
    for (int t = 0; t < N; t++) {
        if (!(D & (1u << t)) || !holds_any(t))
            continue;
        if (victim < 0 || g_tdef[t].prio < g_tdef[victim].prio ||
            (g_tdef[t].prio == g_tdef[victim].prio && wait_since[t] > wait_since[victim]))
            victim = t;
    }
    if (victim < 0)
        return;

    st_deadlocks++;
    stamp(now);
    printf("%s*** INTERBLOCAGE DETECTE (#%d) - cycle d'attente :%s\n", RED(), st_deadlocks, RST());
    for (int t = 0; t < N; t++)
        if (D & (1u << t)) {
            printf("             %s%-10s%s attend %-30s detenu par ", tcol(t), tname(t), RST(),
                   g_res[waiting[t]].name);
            print_holders(waiting[t]);
            printf("\n");
        }
    printf("             => ordre de degagement envoye a %s%s%s (priorite %d, la plus faible)\n",
           tcol(victim), tname(victim), RST(), g_tdef[victim].prio);
    fflush(stdout);

    msg_t c = { CMD_BACKOFF, victim, -1, 0, now };
    if (write(cmd_fd, &c, sizeof c) != (ssize_t)sizeof c)
        perror("PCC: write cmd");

    global_cooldown = now + 800;
    last_D = 0;
    confirm = 0;
}

static void check_priorities(int64_t now)
{
    for (int t = 0; t < N; t++) {
        int r = waiting[t];
        if (r < 0 || state[t] != TS_WAIT)
            continue;
        int64_t w = now - wait_since[t];
        if (!inv_reported[t] && w > 400) {
            for (int h = 0; h < N; h++)
                if (hold[h][r] && g_tdef[h].prio < g_tdef[t].prio) {
                    st_inversions++;
                    inv_reported[t] = 1;
                    stamp(now);
                    printf("%s(!) inversion de priorite%s : %s%s%s (prio %d) bloque sur %s par %s%s%s (prio %d)\n",
                           BLD(), RST(), tcol(t), tname(t), RST(), g_tdef[t].prio, g_res[r].name,
                           tcol(h), tname(h), RST(), g_tdef[h].prio);
                    break;
                }
        }
        if (!starv_reported[t] && w > 6000) {
            starv_reported[t] = 1;
            st_starvation++;
            stamp(now);
            printf("%s(!) famine : %s attend %s depuis %.1fs%s\n", RED(), tname(t),
                   g_res[r].name, w / 1000.0, RST());
        }
    }
}

static void final_report(int64_t now)
{
    printf("\n%s==================== BILAN DU POSTE CENTRAL ====================%s\n", BLD(), RST());
    printf("Duree : %.1fs   Mode : %s   Evenements recus : %d\n\n", now / 1000.0,
           g_mode == MODE_DETECTION ? "detection" : "prevention", st_events);
    printf("%-10s %-5s %8s %8s %12s %12s %8s\n", "Train", "Prio", "Trajets", "Attentes",
           "Moy. (ms)", "Max (ms)", "Garages");
    int tot_trips = 0;
    for (int t = 0; t < N; t++) {
        tot_trips += trips[t];
        printf("%-10s %-5d %8d %8d %12.0f %12lld %8d\n", tname(t), g_tdef[t].prio, trips[t],
               nwaits[t], nwaits[t] ? (double)total_wait[t] / nwaits[t] : 0.0,
               (long long)max_wait[t], nsiding[t]);
    }
    printf("\nTrajets complets           : %d\n", tot_trips);
    printf("Interblocages resolus      : %d\n", st_deadlocks);
    printf("Inversions de priorite     : %d\n", st_inversions);
    printf("Alertes de famine          : %d\n", st_starvation);
    printf("Erreurs de protocole       : %d\n", st_proto_err);
    printf("Violations de securite     : %s%d%s  (%s)\n", st_violations ? RED() : "", st_violations,
           RST(), st_violations ? "COLLISION POSSIBLE" : "aucune collision");
    int residual = 0;
    for (int r = 0; r < g_nres; r++)
        residual += cnt[r];
    printf("Ressources encore occupees : %d\n", residual);
    fflush(stdout);
}

int control_run(int evt_fd, int cmd_fd, int ntrains)
{
    N = ntrains;
    for (int t = 0; t < N; t++) {
        waiting[t] = -1;
        pos[t] = -1;
    }
    printf("%s[PCC] Poste central demarre (pid %d), %d trains a surveiller%s\n",
           BLD(), (int)getpid(), N, RST());
    fflush(stdout);

    int64_t next_check = 0, next_dash = 3000;
    struct pollfd pfd = { evt_fd, POLLIN, 0 };

    for (;;) {
        int64_t now = now_ms();
        int timeout = (int)(next_check - now);
        if (timeout < 0)
            timeout = 0;
        int pr = poll(&pfd, 1, timeout);
        if (pr > 0) {
            msg_t m;
            int rc = msg_recv(evt_fd, &m);
            if (rc <= 0)          /* EOF : tous les trains sont rentrés */
                break;
            on_event(&m);
        }
        now = now_ms();
        if (now >= next_check) {
            check_deadlock(now, cmd_fd);
            check_priorities(now);
            next_check = now + 250;
            fflush(stdout);
        }
        if (now >= next_dash) {
            dashboard(now);
            next_dash = now + 5000;
        }
    }

    final_report(now_ms());
    close(cmd_fd);
    return (st_violations || st_proto_err) ? 2 : 0;
}
