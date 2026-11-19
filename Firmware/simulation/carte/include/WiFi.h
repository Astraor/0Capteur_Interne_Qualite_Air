#pragma once
#include "Arduino.h"
#include <functional>
typedef enum { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3,
               WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 } wl_status_t;
typedef enum { WIFI_OFF = 0, WIFI_STA = 1 } wifi_mode_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
               WIFI_AUTH_WPA2_ENTERPRISE } wifi_auth_mode_t;
typedef enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY } wifi_sort_method_t;
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)
enum {
  WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_ASSOC_LEAVE = 8, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
  WIFI_REASON_802_1X_AUTH_FAILED = 23, WIFI_REASON_BEACON_TIMEOUT = 200, WIFI_REASON_NO_AP_FOUND = 201,
  WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_ASSOC_FAIL = 203, WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
  WIFI_REASON_CONNECTION_FAIL = 205
};
typedef enum { ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 5 } arduino_event_id_t;
typedef arduino_event_id_t WiFiEvent_t;
typedef union { struct { uint8_t reason; } wifi_sta_disconnected; } WiFiEventInfo_t;
typedef void (*WiFiEventFuncCb)(WiFiEvent_t, WiFiEventInfo_t);

class IPAddress {
 public:
  uint8_t o[4] = {0, 0, 0, 0};
  String toString() const { char b[16]; snprintf(b, sizeof b, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]); return String(b); }
};

class WiFiClass {
 public:
  void persistent(bool) {}
  bool mode(wifi_mode_t) { return true; }
  bool disconnect(bool = false, bool = false);
  bool setAutoReconnect(bool) { return true; }
  void setMinSecurity(wifi_auth_mode_t) {}
  void setScanMethod(wifi_scan_method_t) {}
  void setSortMethod(wifi_sort_method_t) {}
  int onEvent(WiFiEventFuncCb cb, arduino_event_id_t) { evenement = cb; return 0; }
  wl_status_t begin(const char* ssid, const char* mdp = nullptr);
  wl_status_t status();
  int16_t scanNetworks(bool async = false, bool hidden = false);
  int16_t scanComplete();
  void scanDelete();
  String SSID(uint8_t i) const;
  String SSID() const;
  int32_t RSSI(uint8_t i) const;
  int32_t RSSI() const;
  wifi_auth_mode_t encryptionType(uint8_t i) const;
  IPAddress localIP() const { IPAddress a; a.o[0] = 192; a.o[1] = 168; a.o[2] = 1; a.o[3] = 42; return a; }
  WiFiEventFuncCb evenement = nullptr;
};
extern WiFiClass WiFi;
