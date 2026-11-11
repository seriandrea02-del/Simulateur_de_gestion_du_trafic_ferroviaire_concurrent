#!/bin/sh
# Lance plusieurs simulations courtes dans les deux modes et vérifie :
#  - code de retour 0 (aucune violation de sécurité, aucun écart de protocole)
#  - toutes les ressources libérées en fin de simulation
#  - l'absence de blocage définitif (le programme se termine)
BIN=${1:-./simu_ferroviaire}
fail=0
for mode in detection prevention; do
  for seed in 1 2 3 42; do
    out=$(timeout 60 "$BIN" -d 15 -m "$mode" -s "$seed" -C 2>&1)
    rc=$?
    dl=$(printf '%s\n' "$out" | sed -n 's/^Interblocages resolus *: *//p')
    res=$(printf '%s\n' "$out" | sed -n 's/^Ressources encore occupees : *//p')
    trips=$(printf '%s\n' "$out" | sed -n 's/^Trajets complets *: *//p')
    if [ "$rc" -eq 0 ] && [ "$res" = "0" ]; then
      echo "OK   mode=$mode seed=$seed trajets=$trips interblocages=$dl"
    else
      echo "FAIL mode=$mode seed=$seed rc=$rc ressources=$res"
      fail=1
    fi
  done
done
[ $fail -eq 0 ] && echo "Tous les tests sont passes." || echo "Des tests ont echoue."
exit $fail
