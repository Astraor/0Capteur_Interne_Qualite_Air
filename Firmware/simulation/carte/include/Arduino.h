// Simulation PC de l'environnement Arduino-ESP32 (2.x) pour le firmware ModuleAir.
// Seul ce dont main.cpp a besoin est fourni. Le temps est VIRTUEL : il n'avance que quand le programme attend
// (delay, vTaskDelay, réseau...), ce qui permet de simuler des journées entières en quelques secondes.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <algorithm>
#include <time.h>

#ifndef ARDUINO
#define ARDUINO 10819
#endif
#define ESP32 1
#define ESP_ARDUINO_VERSION_MAJOR 2
#define IRAM_ATTR
#define PROGMEM
#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define SERIAL_8N1 0x800001c
#define SERIAL_8E1 0x800001e
#ifndef PI
#define PI 3.14159265358979
#endif

using std::isnan;
using std::isinf;
#define radians(d) ((d) * PI / 180.0)
#define degrees(r) ((r) * 180.0 / PI)
typedef bool boolean;
typedef uint8_t byte;

// ---------------- temps virtuel ----------------
namespace sim {
uint64_t maintenantUs();
void attendre(uint64_t us);          // cède la main aux autres tâches pendant us (temps virtuel)
time_t heureSysteme();               // time(nullptr) de la carte (0..1970 tant que NTP n'a pas répondu)
}
inline unsigned long millis() { return (unsigned long)(sim::maintenantUs() / 1000ULL); }
inline unsigned long micros() { return (unsigned long)sim::maintenantUs(); }
inline void delay(unsigned long ms) { sim::attendre((uint64_t)ms * 1000ULL); }
inline void yield() { sim::attendre(0); }

#include <type_traits>
template <class T, class U> inline std::common_type_t<T, U> min(T a, U b) { return a < b ? a : b; }
template <class T, class U> inline std::common_type_t<T, U> max(T a, U b) { return a > b ? a : b; }
template <class T, class L, class H> inline T constrain(T v, L lo, H hi) { return v < (T)lo ? (T)lo : (v > (T)hi ? (T)hi : v); }

// ---------------- String (API Arduino, sur std::string) ----------------
class __FlashStringHelper;
class String {
 public:
  std::string s;
  String() {}
  String(const char* c) { if (c) s = c; }
  String(const std::string& x) : s(x) {}
  String(const String& o) = default;
  String(String&& o) = default;
  explicit String(char c) : s(1, c) {}
  explicit String(int v) : s(std::to_string(v)) {}
  explicit String(unsigned v) : s(std::to_string(v)) {}
  explicit String(long v) : s(std::to_string(v)) {}
  explicit String(unsigned long v) : s(std::to_string(v)) {}
  String& operator=(const String& o) = default;
  String& operator=(String&& o) = default;
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  unsigned int length() const { return (unsigned)s.size(); }
  const char* c_str() const { return s.c_str(); }
  bool concat(const char* c) { if (c) s += c; return true; }
  bool concat(const String& o) { s += o.s; return true; }
  bool concat(char c) { s += c; return true; }
  bool reserve(unsigned n) { s.reserve(n); return true; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* c) { if (c) s += c; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* c) const { return s == (c ? c : ""); }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator!=(const char* c) const { return !(*this == c); }
  bool operator<(const String& o) const { return s < o.s; }
  char operator[](unsigned i) const { return i < s.size() ? s[i] : 0; }
  char& operator[](unsigned i) { return s[i]; }
  char charAt(unsigned i) const { return (*this)[i]; }
  String substring(unsigned a) const { return a >= s.size() ? String() : String(s.substr(a)); }
  String substring(unsigned a, unsigned b) const {
    if (a > b) std::swap(a, b);
    if (a >= s.size()) return String();
    return String(s.substr(a, std::min<size_t>(b, s.size()) - a));
  }
  int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const String& x, unsigned from = 0) const { auto p = s.find(x.s, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char* x, unsigned from = 0) const { auto p = s.find(x, from); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const String& p) const { return s.compare(0, p.s.size(), p.s) == 0; }
  bool endsWith(const String& p) const { return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0; }
  void trim() {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    s = s.substr(a, b - a);
  }
  void toLowerCase() { for (auto& c : s) c = (char)tolower((unsigned char)c); }
  void toUpperCase() { for (auto& c : s) c = (char)toupper((unsigned char)c); }
  void remove(unsigned i) { if (i < s.size()) s.erase(i); }
  void remove(unsigned i, unsigned n) { if (i < s.size()) s.erase(i, n); }
  long toInt() const { return atol(s.c_str()); }
  float toFloat() const { return (float)atof(s.c_str()); }
  bool isEmpty() const { return s.empty(); }
};

// ---------------- Print / Serial ----------------
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { size_t k = 0; while (n--) k += write(*b++); return k; }
  size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t write(char c) { return write((uint8_t)c); }
  size_t print(const char* s) { return write(s); }
  size_t print(const String& s) { return write(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return printf("%d", v); }
  size_t print(unsigned v) { return printf("%u", v); }
  size_t print(long v) { return printf("%ld", v); }
  size_t print(unsigned long v) { return printf("%lu", v); }
  size_t print(double v, int d = 2) { return printf("%.*f", d, v); }
  size_t println() { return write("\r\n"); }
  template <class T> size_t println(const T& v) { size_t n = print(v); return n + println(); }
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n < 0) return 0;
    return write((const uint8_t*)buf, std::min<size_t>((size_t)n, sizeof(buf) - 1));
  }
  virtual void flush() {}
};

class HardwareSerial : public Print {
 public:
  explicit HardwareSerial(int n = 0) : num(n) {}
  int num;
  void begin(unsigned long, uint32_t = SERIAL_8N1, int = -1, int = -1) {}
  size_t write(uint8_t c) override;
  using Print::write;
  int available();
  int read();
  operator bool() const { return true; }
};
extern HardwareSerial Serial;
extern HardwareSerial Serial2;

// ---------------- broches ----------------
void pinMode(uint8_t, uint8_t);
int digitalRead(uint8_t);
void digitalWrite(uint8_t, uint8_t);

// ---------------- ESP ----------------
class EspClass {
 public:
  uint32_t getFreeHeap();
  uint32_t getMaxAllocHeap();
  [[noreturn]] void restart();
};
extern EspClass ESP;

typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
               ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();
uint32_t esp_random();
typedef enum { ESP_MAC_WIFI_STA } esp_mac_type_t;
int esp_read_mac(uint8_t* mac, esp_mac_type_t);
typedef int esp_err_t;
esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool panic);
esp_err_t esp_task_wdt_add(void* tache);
esp_err_t esp_task_wdt_reset();
esp_err_t esp_wifi_sta_wpa2_ent_set_identity(const uint8_t*, int);
esp_err_t esp_wifi_sta_wpa2_ent_set_username(const uint8_t*, int);
esp_err_t esp_wifi_sta_wpa2_ent_set_password(const uint8_t*, int);
esp_err_t esp_wifi_sta_wpa2_ent_enable();
esp_err_t esp_wifi_sta_wpa2_ent_disable();

// ---------------- FreeRTOS (ordonnanceur coopératif simulé) ----------------
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0xFFFFFFFFu
#define configMAX_PRIORITIES 25
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portYIELD_FROM_ISR()
SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t*);
BaseType_t xTaskCreatePinnedToCore(void (*f)(void*), const char* nom, uint32_t pile, void* p, unsigned prio,
                                   TaskHandle_t* h, int coeur);
void vTaskDelay(TickType_t ticks);

struct hw_timer_t { int n; };
hw_timer_t* timerBegin(uint8_t, uint16_t, bool);
void timerAttachInterrupt(hw_timer_t*, void (*)(), bool);
void timerAlarmWrite(hw_timer_t*, uint64_t, bool);
void timerAlarmEnable(hw_timer_t*);

// ---------------- heure (NTP simulé) ----------------
void configTzTime(const char* tz, const char* serveur);
#define time(x) sim::heureSysteme()
