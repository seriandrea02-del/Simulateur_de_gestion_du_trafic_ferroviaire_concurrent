/*
 * util.c - Fonctions utilitaires : temps et messages sur pipes.
 */
#include <errno.h>
#include <unistd.h>
#include "util.h"

int64_t g_t0_ms = 0;

int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_ms(void)
{
    return mono_ms() - g_t0_ms;
}

/* Les fonctions pthread_mutex_timedlock / sem_timedwait /
 * pthread_cond_timedwait attendent une échéance ABSOLUE en CLOCK_REALTIME. */
void abs_deadline(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

int msg_send(int fd, int type, int train, int res)
{
    msg_t m = { type, train, res, 0, now_ms() };
    ssize_t n;
    do {
        n = write(fd, &m, sizeof m);   /* atomique car sizeof m <= PIPE_BUF */
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof m ? 0 : -1;
}

int msg_recv(int fd, msg_t *m)
{
    char  *p = (char *)m;
    size_t got = 0;
    while (got < sizeof *m) {
        ssize_t n = read(fd, p + got, sizeof *m - got);
        if (n == 0)
            return got == 0 ? 0 : -1;  /* EOF */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}
