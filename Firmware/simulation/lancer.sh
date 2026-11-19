#!/bin/sh
# Simulation des coupures Wi-Fi avec le vrai code d'envoi du firmware (voir LISEZMOI.md).
set -e
cd "$(dirname "$0")"
[ -d ArduinoJson ] || git clone -q --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson ArduinoJson
python3 extraire.py ../src/main.cpp extrait.inc
g++ -std=c++17 -O2 -I shim -I ArduinoJson/src simulateur.cpp -o simulateur
SCENARIOS="coupure2h coupure5h_courant demarrage_sans_wifi_puis_courant coupure14h_memoire_pleine coupure20h_memoire_pleine reseau_instable_redemarrage millis_49j"
for s in $SCENARIOS; do ./simulateur "$s"; done
python3 analyser.py $SCENARIOS
