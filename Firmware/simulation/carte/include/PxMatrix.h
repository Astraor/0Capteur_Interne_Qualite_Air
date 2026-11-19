#pragma once
#include "Arduino.h"
#include "Adafruit_GFX.h"
enum driver_chips { SHIFT, FM6124, FM6126A, ICN2038S, MBI5124, SM5266P, DP3246_SM5368 };
enum color_orders { RRGGBB, RRBBGG, GGRRBB, GGBBRR, BBRRGG, BBGGRR };
class PxMATRIX : public Adafruit_GFX {
 public:
  PxMATRIX(uint16_t w, uint16_t h, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) : Adafruit_GFX(w, h) {}
  void begin(uint8_t) {}
  void setDriverChip(driver_chips) {}
  void setColorOrder(color_orders) {}
  void setBrightness(uint8_t b) { luminosite = b; }
  void clearDisplay() { memset(dessin, 0, sizeof dessin); }
  void fillScreen(uint16_t c) override { for (auto& l : dessin) for (auto& p : l) p = c; }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || y < 0 || x >= 64 || y >= 32) { horsCadre++; return; }
    dessin[y][x] = c;
  }
  void display(uint16_t = 0) {}
  void showBuffer();                 // double tampon : l'image dessinée devient visible
  uint16_t dessin[32][64] = {{0}};   // tampon de dessin
  uint8_t luminosite = 255;
  unsigned long horsCadre = 0;       // pixels dessinés hors de l'écran (texte qui déborde)
};
