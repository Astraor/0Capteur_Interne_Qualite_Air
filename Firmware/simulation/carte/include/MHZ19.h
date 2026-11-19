#pragma once
#include "Arduino.h"
#define RESULT_OK 1
#define RESULT_TIMEOUT 3
class MHZ19 {
 public:
  void begin(Print& port) { (void)port; }
  void autoCalibration(bool on = true) { abc = on; }
  void calibrate();
  int getCO2(bool = true, bool = true);
  uint8_t errorCode = RESULT_OK;
  bool abc = true;
};
