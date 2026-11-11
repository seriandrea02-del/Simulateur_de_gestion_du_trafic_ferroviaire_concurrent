/*
 * util.h - Temps, sommeil interruptible, messages sur pipe.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <time.h>
#include "common.h"

extern int64_t g_t0_ms;

int64_t mono_ms(void);                 /* horloge monotone absolue (ms)   */
int64_t now_ms(void);                  /* temps écoulé depuis le départ   */
void    abs_deadline(struct timespec *ts, int ms); /* CLOCK_REALTIME + ms */
void    sleep_ms(int ms);

int msg_send(int fd, int type, int train, int res);
int msg_recv(int fd, msg_t *m);        /* 1 = ok, 0 = EOF, -1 = erreur    */

#endif
