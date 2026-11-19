#pragma once
#include "Arduino.h"
#include <memory>
class File {
 public:
  File() {}
  File(FILE* f, size_t* compteur) : fp(f) { (void)compteur; }
  operator bool() const { return fp != nullptr; }
  size_t write(uint8_t c);
  size_t write(const uint8_t* b, size_t n);
  size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  int available();
  int read();
  size_t read(uint8_t* b, size_t n);
  bool seek(size_t pos);
  size_t position();
  size_t size();
  String readStringUntil(char fin);
  void close();
 private:
  FILE* fp = nullptr;
};
class LittleFSFS {
 public:
  bool begin(bool formaterSiEchec = false);
  File open(const char* chemin, const char* mode = "r");
  bool exists(const char* chemin);
  bool remove(const char* chemin);
  bool rename(const char* de, const char* vers);
  size_t totalBytes();
  size_t usedBytes();
};
extern LittleFSFS LittleFS;
