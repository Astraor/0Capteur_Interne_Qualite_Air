#!/bin/sh
# Compile le VRAI firmware (../../src/main.cpp, sans le modifier) avec le banc de simulation de la carte.
# La première fois, télécharge les deux bibliothèques qui ne touchent pas au matériel, dans les versions du
# platformio.ini : ArduinoJson (JSON envoyé / reçu) et Adafruit GFX (texte et polices de l'écran, au pixel près).
set -e
cd "$(dirname "$0")"
mkdir -p libs build
[ -d libs/ArduinoJson ] || git -c advice.detachedHead=false clone -q --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson libs/ArduinoJson
[ -d libs/Adafruit-GFX-Library ] || git -c advice.detachedHead=false clone -q --depth 1 --branch 1.11.11 https://github.com/adafruit/Adafruit-GFX-Library libs/Adafruit-GFX-Library
GFX=libs/Adafruit-GFX-Library
[ -f "$GFX/Adafruit_GFX.h" ] || GFX="$GFX/src"   # versions récentes : fichiers dans src/
FLAGS="-DARDUINO=10819 -std=gnu++17 -O2 -Wall -Wno-unused-function -Wno-unused-variable -Wno-format-truncation -pthread \
  -Iinclude -Ilibs/ArduinoJson/src -I$GFX -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_PROGMEM=0"
g++ $FLAGS -w -c "$GFX/Adafruit_GFX.cpp" -o build/gfx.o
g++ $FLAGS -c simulateur_carte.cpp -o build/simulateur.o
g++ $FLAGS -c ../../src/main.cpp -o build/main.o
g++ -pthread build/gfx.o build/simulateur.o build/main.o -o build/carte
echo "Carte simulée prête : build/carte"
