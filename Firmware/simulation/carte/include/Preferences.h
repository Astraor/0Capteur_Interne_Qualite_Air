#pragma once
#include "Arduino.h"
class Preferences {
 public:
  bool begin(const char* espace, bool lectureSeule = false);
  String getString(const char* k, const String& d = String());
  size_t putString(const char* k, const String& v);
  uint32_t getULong(const char* k, uint32_t d = 0);
  size_t putULong(const char* k, uint32_t v);
  bool getBool(const char* k, bool d = false);
  size_t putBool(const char* k, bool v);
  uint8_t getUChar(const char* k, uint8_t d = 0);
  size_t putUChar(const char* k, uint8_t v);
  int8_t getChar(const char* k, int8_t d = 0);
  size_t putChar(const char* k, int8_t v);
  bool remove(const char* k);
};
