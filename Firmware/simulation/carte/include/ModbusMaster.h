#pragma once
#include "Arduino.h"
class ModbusMaster {
 public:
  static const uint8_t ku8MBSuccess = 0x00;
  static const uint8_t ku8MBResponseTimedOut = 0xE2;
  void begin(uint8_t adresse, HardwareSerial& port) { (void)adresse; (void)port; }
  uint8_t readHoldingRegisters(uint16_t reg, uint16_t n);
  uint16_t getResponseBuffer(uint8_t i) { return i < 64 ? tampon[i] : 0; }
 private:
  uint16_t tampon[64] = {0};
};
