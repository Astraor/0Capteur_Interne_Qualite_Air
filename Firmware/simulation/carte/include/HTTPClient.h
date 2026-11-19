#pragma once
#include "Arduino.h"
class HTTPClient {
 public:
  void setConnectTimeout(int32_t ms) { tConnexion = ms; }
  void setTimeout(uint16_t ms) { tReponse = ms; }
  bool begin(const String& url);
  void addHeader(const String& nom, const String& valeur);
  int POST(const String& corps);
  String getString() { return reponse; }
  void end() {}
  static String errorToString(int code);
 private:
  int32_t tConnexion = 5000, tReponse = 5000;
  String url, cle, reponse;
};
