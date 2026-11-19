// Faux environnement Arduino / ESP32 pour compiler sur PC la logique d'envoi du firmware (simulation).
#pragma once
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <algorithm>
#include <functional>

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) { if (c) s = c; }
  String(const std::string& x) : s(x) {}
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
  bool concat(const char* c) { if (c) s += c; return true; }
  bool concat(const char* c, unsigned n) { s.append(c, n); return true; }
  bool concat(char c) { s += c; return true; }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }
  char operator[](unsigned i) const { return s[i]; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == o; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
};

// ---- temps simulé ----
extern uint64_t simReelMs;          // temps réel du monde (ms depuis le début de la simulation)
extern uint64_t simDemarrageMs;     // temps réel du démarrage de la carte en cours
extern double   simEpochDepart;     // heure Unix (s) au début de la simulation
extern bool     simRtcValide;       // l'horloge de la carte est-elle réglée (NTP) ?
extern uint64_t simDecalageMillis;   // pour tester le retour à zéro de millis() (~49,7 jours)
inline uint32_t millis() { return (uint32_t)(simReelMs - simDemarrageMs + simDecalageMillis); }
inline time_t sim_time(time_t*) {
  if (simRtcValide) return (time_t)(simEpochDepart + simReelMs / 1000.0);
  return (time_t)((simReelMs - simDemarrageMs) / 1000);   // horloge non réglée : quelques secondes après 1970
}
inline void delay(unsigned long) {}
template <class T, class A, class B> T constrain(T v, A a, B b) { return v < (T)a ? (T)a : (v > (T)b ? (T)b : v); }
template <class A, class B> auto max(A a, B b) -> decltype(a + b) { return a > b ? a : b; }
using std::isnan;

// ---- port série : on garde les messages ----
struct FauxSerial {
  std::function<void(const std::string&)> sortie;
  void println(const char* t) { if (sortie) sortie(t); }
  void println(const String& t) { println(t.c_str()); }
  void printf(const char* f, ...) { char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); std::string s(b); while (!s.empty() && s.back() == '\n') s.pop_back(); if (sortie) sortie(s); }
};
extern FauxSerial Serial;

// ---- FreeRTOS / ESP ----
typedef int* SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffff
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static int m; return &m; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void esp_task_wdt_reset() {}
extern int simCauseReset;
inline int esp_reset_reason() { return simCauseReset; }
struct FauxESP { uint32_t getFreeHeap() { return 150000; } };
extern FauxESP ESP;

// ---- Wi-Fi ----
#define WL_CONNECTED 3
extern bool simWifiConnecte;
struct FauxWiFi { int status() { return simWifiConnecte ? WL_CONNECTED : 6; } int RSSI() { return -61; } };
extern FauxWiFi WiFi;

// ---- HTTP : branché sur le serveur simulé ----
extern std::function<int(const std::string& corps, std::string& reponse)> simServeurPost;
class HTTPClient {
  std::string rep;
 public:
  void setConnectTimeout(int) {}
  void setTimeout(int) {}
  bool begin(const char*) { return true; }
  void addHeader(const char*, const String&) {}
  void addHeader(const char*, const char*) {}
  int POST(const String& corps) { return simServeurPost(corps.s, rep); }
  String getString() { return String(rep); }
  void end() {}
  static String errorToString(int c) { return String(c == -11 ? "read Timeout" : "connection refused"); }
};

// ---- Mémoire flash (LittleFS) : fichiers en mémoire, conservés d'un démarrage à l'autre ----
struct FauxFichierDonnees { std::string d; };
class File {
  std::shared_ptr<FauxFichierDonnees> f; size_t pos = 0;
 public:
  File() {}
  File(std::shared_ptr<FauxFichierDonnees> x, size_t p) : f(x), pos(p) {}
  explicit operator bool() const { return (bool)f; }
  bool seek(size_t p) { if (!f || p > f->d.size()) return false; pos = p; return true; }
  int available() { return f ? (int)(f->d.size() - pos) : 0; }
  size_t read(uint8_t* b, size_t n) { size_t k = std::min(n, f->d.size() - pos); memcpy(b, f->d.data() + pos, k); pos += k; return k; }
  String readStringUntil(char c) { size_t e = f->d.find(c, pos); std::string r; if (e == std::string::npos) { r = f->d.substr(pos); pos = f->d.size(); } else { r = f->d.substr(pos, e - pos); pos = e + 1; } return String(r); }
  size_t position() { return pos; }
  size_t size() { return f ? f->d.size() : 0; }
  size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) { f->d.insert(pos, (const char*)b, n); pos += n; return n; }
  void close() {}
};
struct FauxLittleFS {
  std::map<std::string, std::shared_ptr<FauxFichierDonnees>> fichiers;
  bool exists(const char* p) { return fichiers.count(p); }
  bool remove(const char* p) { return fichiers.erase(p) > 0; }
  bool rename(const char* a, const char* b) { if (!fichiers.count(a)) return false; fichiers[b] = fichiers[a]; fichiers.erase(a); return true; }
  File open(const char* p, const char* mode) {
    if (mode[0] == 'r') { if (!fichiers.count(p)) return File(); return File(fichiers[p], 0); }
    if (mode[0] == 'w' || !fichiers.count(p)) fichiers[p] = std::make_shared<FauxFichierDonnees>();
    return File(fichiers[p], mode[0] == 'a' ? fichiers[p]->d.size() : 0);
  }
  size_t totalBytes() { return 1441792; }
  size_t usedBytes() { size_t u = 8192; for (auto& kv : fichiers) u += (kv.second->d.size() + 4095) / 4096 * 4096; return u; }
};
extern FauxLittleFS LittleFS;

// ---- Préférences (NVS), conservées d'un démarrage à l'autre ----
struct Preferences {
  std::map<std::string, uint64_t> n; std::map<std::string, std::string> t;
  bool getBool(const char* k, bool d) { return n.count(k) ? n[k] != 0 : d; }
  void putBool(const char* k, bool v) { n[k] = v; }
  uint32_t getULong(const char* k, uint32_t d) { return n.count(k) ? (uint32_t)n[k] : d; }
  void putULong(const char* k, uint32_t v) { n[k] = v; }
  String getString(const char* k, const char* d) { return String(t.count(k) ? t[k] : std::string(d)); }
};
extern Preferences prefs;
