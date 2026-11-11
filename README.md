# Simulateur de circulation ferroviaire multithread

Projet de programmation concurrente en C (POSIX) : plusieurs trains
circulent **simultanément** sur un réseau composé de voies, de gares, de
zones, d'aiguillages et de carrefours. Le problème central : **deux trains ne
doivent jamais occuper en même temps une même section de voie**.

## Compilation et exécution

```bash
make                      # compile ./simu_ferroviaire
make run                  # 40 s en mode détection
make run-prev             # 40 s en mode prévention
make test                 # 8 simulations (2 modes x 4 graines), vérifie la sécurité
make tsan                 # recompile avec ThreadSanitizer (détection de courses)
```

Options :

| Option | Rôle |
|---|---|
| `-d SEC` | durée de la simulation (défaut 40 s) |
| `-m detection\|prevention` | stratégie face aux interblocages |
| `-t N` | nombre de trains (1 à 7) |
| `-s SEED` | graine aléatoire (pour rejouer un scénario) |
| `-v` | verbeux : toutes les acquisitions et libérations |
| `-C` | sans couleurs |

`Ctrl-C` arrête proprement : les trains rentrent au dépôt et le poste central
affiche son bilan. Le code de retour vaut 0 si aucune violation de sécurité
n'a été constatée, 2 sinon.

## Correspondance avec le cahier des charges

| Exigence | Réalisation |
|---|---|
| Chaque train est un thread indépendant | `pthread_create` → `train_thread()` (`src/train.c`) |
| Les sections de voie sont des ressources partagées | tableau `g_res[]` (`src/network.c`) |
| Les **mutex** protègent les sections | `pthread_mutex_t` (type ERRORCHECK) par section ; `pthread_mutex_timedlock` |
| Les **sémaphores** contrôlent voies et aiguillages | `sem_t` compteur = nombre de quais d'une gare ; `sem_t` binaire par aiguillage ; `sem_t` compteur par zone |
| Carrefours | porte à priorité (mutex + variable de condition) : TGV > TER > FRET |
| Les **pipes** relient les trains au poste central | pipe *événements* (trains → PCC) et pipe *commandes* (PCC → trains) |
| Un **processus de contrôle** surveille trafic, priorités, blocages | processus fils créé par `fork()` (`src/control.c`) |

## Architecture

```
 processus principal (simulation)              processus fils : PCC
 +----------------------------------+          +--------------------------+
 | thread TGV 2401  --\             |   pipe   | control_run()            |
 | thread TER 835A  ---+-- write -->|=========>|  - modèle du réseau      |
 | ...                 |            |  evt     |  - invariant de sécurité |
 | thread FRET 4520 --/             |          |  - inversions de priorité|
 |                                  |   pipe   |  - détection (Holt)      |
 | thread dispatcher <--- read -----|<=========|  - ordre de dégagement   |
 |   (drapeau atomique backoff)     |  cmd     |  - tableau de bord/bilan |
 +----------------------------------+          +--------------------------+
```

* Le `fork()` a lieu **avant** la création des threads (un `fork` dans un
  programme multithread ne duplique que le thread appelant).
* Les messages ont une taille fixe de 24 octets, inférieure à `PIPE_BUF` :
  chaque `write()` est **atomique**, donc tous les trains peuvent écrire dans
  le même pipe sans que leurs messages s'entremêlent.
* Le PCC s'arrête quand il lit la fin de fichier : tous les trains ont rendu
  leur extrémité du pipe.

## Réseau simulé

```
                        THIONVILLE (2 quais)
                            |  ||  double voie
  WOIPPY triage ==S_WOI==  X  carrefour de Woippy (priorité)
    (2 voies)  voie unique  |
                +-----------+--------------------+
                | ZONE « Noeud de Metz » (cap. 4)|
                |  A_SUD --- METZ (3) --- A_EST   |
                +----|-----------------------|---+
       double voie   |                       |  VOIE UNIQUE
                PONT-A-MOUSSON (2)       SAINT-AVOLD (2, croisement)
       double voie   |                       |  VOIE UNIQUE
                 NANCY (2)               FORBACH (1 quai, terminus)
```

| Train | Type | Prio | Itinéraire (boucle) |
|---|---|---|---|
| TGV 2401 | TGV | 3 | Nancy ↔ Metz ↔ Thionville |
| TER 835A | TER | 2 | Metz → Forbach → Metz |
| TER 835B | TER | 2 | Forbach → Metz → Forbach (sens inverse) |
| FRET 4410 | FRET | 1 | Woippy → Forbach → Woippy |
| TER 836 | TER | 2 | Metz ↔ Nancy |
| TER 837 | TER | 2 | Thionville ↔ Nancy via Metz |
| FRET 4520 | FRET | 1 | Woippy ↔ Thionville |

Les voies doubles sont modélisées par **une section par sens** : les trains de
sens opposés ne s'y gênent pas. Les **voies uniques** sont des sections
uniques parcourues dans les deux sens : c'est là que naissent les conflits.

## Règle de sécurité : le cantonnement

Un train **acquiert la ressource suivante avant de quitter la ressource
courante** (il ne s'engage jamais sur une section qui n'est pas libre). Le
mutex de la section garantit l'exclusion mutuelle : deux trains ne peuvent
pas s'y trouver ensemble.

Le PCC vérifie cet invariant indépendamment : à chaque message `ACQ` il
contrôle que l'occupation ne dépasse pas la capacité. Le compteur
« Violations de sécurité » doit rester à 0 (c'est ce que vérifie `make test`).

### Ordre des messages (cohérence causale)

* `WAIT` est envoyé **avant** de se bloquer ;
* `ACQ` est envoyé **après** avoir obtenu la ressource ;
* `REL` est envoyé **avant** de la libérer.

Un train B ne peut obtenir une ressource qu'après que A l'a libérée, donc
le `REL` de A est forcément écrit dans le pipe avant l'`ACQ` de B. Le modèle
reconstruit par le PCC ne montre donc jamais d'état impossible.

## Interblocages

Le cantonnement réunit les quatre conditions de Coffman :

1. **exclusion mutuelle** : une section ne peut recevoir qu'un train ;
2. **attente en détenant** : le train garde sa section en attendant la suivante ;
3. **pas de réquisition** : on ne retire pas une section à un train ;
4. **attente circulaire** : exemple observé sur la voie unique de Forbach :

```
TER 835B  attend Gare de Forbach                 détenue par FRET 4410
FRET 4410 attend Voie unique St-Avold-Forbach    détenue par TER 835B
```

### Mode `detection` : détection et guérison

Toutes les 250 ms, le PCC applique la **réduction du graphe d'allocation**
(algorithme de Holt), valable pour des ressources à plusieurs exemplaires
(gares, zones) :

1. D = ensemble des trains bloqués ;
2. on retire de D tout train dont la ressource attendue a encore une place
   libre, ou est détenue par au moins un train hors de D (qui finira par la
   libérer) ;
3. on répète jusqu'au point fixe : si D n'est pas vide, il y a interblocage.

Pour écarter les faux positifs dus aux états transitoires, le même ensemble
doit être constaté lors de **deux contrôles successifs**.

**Résolution** : le PCC choisit une victime (le train le **moins prioritaire**
du cycle, qui détient au moins une ressource) et lui envoie `CMD_BACKOFF` par
le pipe de commandes. Le thread *dispatcher* positionne un drapeau atomique ;
les attentes des trains étant **temporisées** (`pthread_mutex_timedlock`,
`sem_timedwait`, `pthread_cond_timedwait` par tranches de 100 ms), la victime
abandonne son attente, libère tout ce qu'elle occupe, se dégage sur une
**voie de garage** pendant 1 à 2 s, puis reprend sa route. On a ainsi
rompu la condition « pas de réquisition ».

### Mode `prevention` : réservation d'itinéraire

Comme dans un vrai poste d'aiguillage, un train en gare réserve **d'un seul
coup** (tout-ou-rien, par `trylock`) tout son itinéraire jusqu'au quai de la
prochaine gare inclus. S'il manque une ressource, il relâche tout et
réessaie plus tard (les trains peu prioritaires attendent plus longtemps
entre deux essais).

Un train ne s'arrête donc **jamais en pleine voie unique** : l'interblocage
« face à face » sur une section disparaît. Il subsiste un cas résiduel
quand les **deux gares** sont pleines (aucun quai pour se croiser), par
exemple :

```
TER 835A  attend Gare de Forbach      détenue par FRET 4410
TER 835B  attend Gare de Forbach      détenue par FRET 4410
FRET 4410 attend Gare de Saint-Avold  détenue par TER 835A, TER 835B
```

Le PCC détecte aussi ce cas et le résout de la même façon. Les mesures
(`make test`) montrent beaucoup moins d'interblocages qu'en mode détection.

## Priorités

* **Carrefour de Woippy** : la porte à priorité donne le passage au train en
  attente le plus prioritaire ; un `trylock` d'un train moins prioritaire
  échoue si un train plus prioritaire attend déjà.
* **Victimes** : en cas d'interblocage, c'est le FRET qui est dégagé en
  premier, puis le TER ; le TGV en dernier.
* **Surveillance** : le PCC signale les **inversions de priorité** (train
  prioritaire bloqué plus de 400 ms par un train moins prioritaire) et la
  **famine** (attente de plus de 6 s).

## Organisation des fichiers

```
include/common.h    types, constantes, format des messages
include/network.h   ressources, itinéraires, API de verrouillage
include/train.h     structure d'un train (thread)
include/control.h   poste central
include/util.h      temps, messages sur pipe
src/main.c          options, pipes, fork, création/attente des threads
src/network.c       description du réseau + primitives (mutex, sémaphores, porte)
src/train.c         comportement d'un train (modes détection et prévention)
src/control.c       PCC : modèle, sécurité, priorités, Holt, tableau de bord
tests/run_tests.sh  tests automatiques
docs/               exemples de sortie
```

## Exemple de bilan (mode détection, 25 s)

```
Train      Prio   Trajets Attentes    Moy. (ms)     Max (ms)  Garages
TGV 2401   3            2        7          653         1168        0
TER 835A   2            2        3         1416         2319        0
FRET 4410  1            1        7          597         1003        3
...
Interblocages resolus      : 3
Violations de securite     : 0  (aucune collision)
Ressources encore occupees : 0
```

## Pistes d'extension

* lire le réseau et les itinéraires depuis un fichier de configuration ;
* remplacer les threads par des processus et les mutex par des sémaphores
  nommés en mémoire partagée (`sem_open`, `shm_open`) ;
* algorithme du banquier pour éviter (et non seulement détecter) les
  interblocages en gare ;
* interface graphique (ncurses) du tableau de bord.
