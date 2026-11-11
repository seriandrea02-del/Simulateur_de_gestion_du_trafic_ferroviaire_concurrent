/*
 * main.c - Point d'entrée du simulateur.
 *
 *   processus principal (simulation)          processus fils (PCC)
 *   +--------------------------------+        +-------------------------+
 *   | thread train 0 ---\            |  pipe  |                         |
 *   | thread train 1 ----+-- write ->|=======>| control_run()           |
 *   | thread train N ---/            | evt    |  - modèle du réseau     |
 *   |                                |        |  - sécurité / priorités |
 *   | thread dispatcher <-- read ----|<=======|  - détection interbloc. |
 *   |   (ordres de dégagement)       |  cmd   |                         |
 *   +--------------------------------+        +-------------------------+
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"
#include "control.h"
#include "network.h"
#include "train.h"
#include "util.h"

sim_mode_t g_mode = MODE_DETECTION;
atomic_int g_stop = 0;
int        g_verbose = 0;
int        g_color = 1;
int        g_ntrains_active = 0;

static volatile sig_atomic_t g_sigint = 0;
static train_t trains[MAX_TRAINS];

static void on_sigint(int s)
{
    (void)s;
    g_sigint = 1;
}

/* Thread qui reçoit les ordres du poste central et les transmet aux
 * trains concernés (drapeau atomique consulté pendant leurs attentes). */
static void *dispatcher(void *arg)
{
    int fd = *(int *)arg;
    msg_t m;
    while (msg_recv(fd, &m) == 1) {
        if (m.type == CMD_BACKOFF && m.train >= 0 && m.train < g_ntrains_active)
            atomic_store(&trains[m.train].backoff, 1);
    }
    return NULL;
}

static void usage(const char *p)
{
    printf("Usage : %s [options]\n"
           "  -d SEC    duree de la simulation (defaut 40 s)\n"
           "  -m MODE   detection | prevention (defaut detection)\n"
           "  -t N      nombre de trains (1..%d, defaut %d)\n"
           "  -s SEED   graine aleatoire (defaut : horloge)\n"
           "  -v        mode verbeux (toutes les acquisitions/liberations)\n"
           "  -C        desactive les couleurs\n"
           "  -h        aide\n", p, g_ntdef, g_ntdef);
}

int main(int argc, char **argv)
{
    int duration = 40, ntrains = g_ntdef;
    unsigned seed = (unsigned)time(NULL);
    int opt;

    if (!isatty(STDOUT_FILENO))
        g_color = 0;
    while ((opt = getopt(argc, argv, "d:m:t:s:vCh")) != -1) {
        switch (opt) {
        case 'd': duration = atoi(optarg); break;
        case 'm':
            if (!strcmp(optarg, "detection"))       g_mode = MODE_DETECTION;
            else if (!strcmp(optarg, "prevention")) g_mode = MODE_PREVENTION;
            else { usage(argv[0]); return 1; }
            break;
        case 't': ntrains = atoi(optarg); break;
        case 's': seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'v': g_verbose = 1; break;
        case 'C': g_color = 0; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (ntrains < 1 || ntrains > g_ntdef || duration < 1) {
        usage(argv[0]);
        return 1;
    }
    if (network_validate(ntrains) != 0) {
        fprintf(stderr, "Reseau invalide\n");
        return 1;
    }
    g_ntrains_active = ntrains;

    printf("=== Simulateur de circulation ferroviaire ===\n");
    printf("Mode %s, %d trains, %d s, graine %u\n\n",
           g_mode == MODE_DETECTION ? "DETECTION" : "PREVENTION", ntrains, duration, seed);
    fflush(stdout);

    g_t0_ms = mono_ms();

    int evt[2], cmd[2];
    if (pipe(evt) < 0 || pipe(cmd) < 0) {
        perror("pipe");
        return 1;
    }

    /* fork AVANT la création des threads (fork + threads = dangereux) */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_IGN);    /* le PCC s'arrête sur EOF du pipe */
        close(evt[1]);
        close(cmd[0]);
        int rc = control_run(evt[0], cmd[1], ntrains);
        close(evt[0]);
        _exit(rc);
    }
    close(evt[0]);
    close(cmd[1]);
    g_evt_fd = evt[1];

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    network_init_sync();
    pthread_barrier_init(&g_start_barrier, NULL, (unsigned)ntrains + 1);

    pthread_t disp;
    int cmd_rd = cmd[0];
    pthread_create(&disp, NULL, dispatcher, &cmd_rd);

    for (int i = 0; i < ntrains; i++) {
        trains[i].id = i;
        trains[i].def = &g_tdef[i];
        atomic_init(&trains[i].backoff, 0);
        trains[i].nheld = 0;
        trains[i].seed = seed * 2654435761u + (unsigned)i * 97u;
        trains[i].trips = 0;
        if (pthread_create(&trains[i].th, NULL, train_thread, &trains[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    pthread_barrier_wait(&g_start_barrier);   /* tous les trains sont à quai */

    int64_t end = now_ms() + (int64_t)duration * 1000;
    while (now_ms() < end && !g_sigint)
        sleep_ms(100);
    atomic_store(&g_stop, 1);

    for (int i = 0; i < ntrains; i++)
        pthread_join(trains[i].th, NULL);
    close(g_evt_fd);                  /* EOF pour le PCC */
    pthread_join(disp, NULL);         /* EOF quand le PCC ferme cmd */
    close(cmd_rd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    pthread_barrier_destroy(&g_start_barrier);
    network_destroy_sync();

    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
    printf("\nSimulation terminee (code PCC = %d)\n", rc);
    return rc;
}
