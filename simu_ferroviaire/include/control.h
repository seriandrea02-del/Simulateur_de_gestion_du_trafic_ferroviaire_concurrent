/*
 * control.h - Poste central de commande (processus séparé).
 */
#ifndef CONTROL_H
#define CONTROL_H

/* Lit les événements sur evt_fd, envoie des ordres sur cmd_fd.
 * Retourne 0 si aucune violation de sécurité, 2 sinon. */
int control_run(int evt_fd, int cmd_fd, int ntrains);

#endif
