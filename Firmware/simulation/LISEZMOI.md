# Simulation des coupures Wi-Fi

Vérifie, sans carte, que les mesures prises pendant une coupure Wi-Fi arrivent au serveur et retrouvent leur **heure réelle**.

- `extraire.py` copie depuis `../src/main.cpp` le **vrai code** du stockage hors ligne et de l'envoi (fonctions `stockerRecord`, `rejouerUneLigne`, `calculerAge`, `envoyerLigne`, `posterAuServeur`, `gererEnvois`…), sans le modifier. Les types sont ceux de l'ESP32 (`long` et `unsigned long` sur 32 bits).
- `shim/Arduino.h` remplace le matériel : horloge, Wi-Fi, flash LittleFS (en mémoire) et préférences (NVS). La flash et les préférences survivent aux redémarrages. L'heure survit à un redémarrage logiciel, mais pas à une coupure de courant, comme sur l'ESP32.
- `simulateur.cpp` fait tourner la carte par pas de 50 ms : lecture des capteurs toutes les 10 s, une mesure toutes les 30 s, tâche d'envoi. Le faux serveur AirCarto date chaque mesure à sa **réception**, à la seconde près, et refuse deux envois à moins de 10 s d'écart (429). Chaque mesure porte un numéro dans `x_temp_pm` : on la compare ensuite à son heure réelle.
- `analyser.py` refait le calcul du site (heure réelle = heure de réception − `x_age_s`) et le compare à la vérité.

```sh
sh lancer.sh      # télécharge ArduinoJson 7.4.3 la première fois, compile, lance les scénarios, affiche le bilan
```

## Résultats (novembre 2026)

| Scénario | Mesures perdues | Écart d'heure max | Fin du rattrapage |
|---|---|---|---|
| Coupure Wi-Fi de 2 h | 0 | 1 s | 50 min après le retour du Wi-Fi |
| Coupure Wi-Fi de 5 h + coupure de courant de 20 min pendant | 0 (trou de 20 min : carte éteinte) | 1 s | ~2 h |
| Coupure Wi-Fi de 14 h | 0 | 1 s | ~5 h 50 |
| Coupure Wi-Fi de 20 h (mémoire pleine) | 945 : les ~8 h les plus anciennes (voulu : on garde les ~12 h récentes) | 1 s | ~5 h |
| Réseau instable (5 % d'envois perdus, 1 % de réponses perdues) + `ESP.restart()` | 0 (6 mesures reçues deux fois : le site les dédoublonne) | 1 s | 45 min |
| Carte démarrée sans Wi-Fi, puis coupure de courant avant le retour du Wi-Fi | toute la 1re heure (impossible à dater : la carte n'a jamais eu l'heure) | 1 s | — |
| Coupure Wi-Fi qui chevauche le retour à zéro de `millis()` (~49,7 jours) | **avant correction : 61** ; après : 0 | 1 s | 25 min |

Le dernier cas a révélé un bug, corrigé dans `calculerAge` : `millis()/1000` repart de 0 au bout d'environ 49,7 jours. L'âge calculé était alors négatif, et les mesures étaient jetées comme « impossibles à dater ».

## Voir aussi : la carte complète

Le dossier [`carte/`](carte/LISEZMOI.md) fait tourner **tout** le firmware : capteurs, écran LED rendu en images, Wi-Fi, commandes série, redémarrages. Il simule des jours entiers en quelques secondes. C'est lui qui a trouvé les pannes capables de bloquer les envois pendant des jours : refus 401 passager, Wi-Fi « fantôme », délai aberrant du serveur.
