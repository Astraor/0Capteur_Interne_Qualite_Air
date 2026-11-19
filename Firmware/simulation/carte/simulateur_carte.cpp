// Banc de simulation du firmware ModuleAir : implémente sur PC tout ce que main.cpp attend de la carte.
//
// - Ordonnanceur coopératif à TEMPS VIRTUEL : loop() et la tâche d'envoi sont de vrais threads, mais un seul tourne à
//   la fois ; le temps n'avance que quand tous attendent (delay, vTaskDelay, requête réseau). 24 h simulées ~ 1 min.
// - Capteurs (NextPM en Modbus, MH-Z19C), Wi-Fi (points d'accès, coupures), serveur AirCarto (clé, 10 s entre deux
//   envois, 429, pannes), mémoire flash (LittleFS -> dossier), NVS (Preferences -> fichier), watchdog, secteur.
// - Écran : chaque image présentée (showBuffer) est enregistrée dans frames.bin pendant les fenêtres demandées.
// - ESP.restart(), watchdog, coupure de courant : le processus s'arrête (code 42 / 43 / 0) et le script Python
//   relance la « carte » avec la flash et la NVS conservées, comme un vrai redémarrage.
//
// Réglages : variables d'environnement SIM_* (voir main) et un fichier de scénario, une consigne par ligne
// (heures en secondes epoch) : cle <32 car.> | ap <ssid> <rssi> <mdp|-> [ent] | cmd <t> <commande série>
// coupure_wifi <a> <b> | wifi_zombie <a> <b> | panne_serveur <a> <b> <code> | attente_serveur <a> <b> <s>
// batterie <a> <b> | pollution <a> <b> <+PM2.5> | fenetre <a> <b> | derive_co2 <a> <b> <ppm> | images <a> <b> | proba429 <p>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <random>
#include <functional>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "Arduino.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "ModbusMaster.h"
#include "MHZ19.h"
#include "PxMatrix.h"
#include <ArduinoJson.h>

// Variables du firmware lues par le banc (liaison externe)
extern uint8_t pageCourante;
extern uint8_t luminositeCourante;
extern PxMATRIX display;

HardwareSerial Serial(0);
HardwareSerial Serial2(2);
EspClass ESP;
WiFiClass WiFi;
LittleFSFS LittleFS;

namespace sim {
// ---------------------------------------------------------------- réglages
static std::string dossier = "etat";
static double epochBoot = 1795161600;   // heure réelle (virtuelle) au démarrage de la carte
static double dureeS = 3600;            // durée de cette mise sous tension
static int raisonReset = 1;
static uint32_t graine = 1;
static std::mt19937 rng;
static double alea() { return std::uniform_real_distribution<double>(0, 1)(rng); }
static double gauss(double s) { return std::normal_distribution<double>(0, s)(rng); }

struct Fenetre { double a, b; int code = 0; double v1 = 0, v2 = 0; };
struct PointAcces { std::string ssid, mdp; int rssi; bool ent; };
struct Commande { double t; std::string texte; };
static std::string cleServeur;
static std::vector<PointAcces> pointsAcces;
static std::vector<Fenetre> coupuresWifi, pannesServeur, batteries, fenetresImages, pollutions, fenetresOuvertes, derivesCO2, zombies, attentesServeur;
static std::vector<Commande> commandes;
static double probaEchecModbus = 0.005, probaEchecMhz = 0.003, proba429 = 0.0;

static double epoch();   // heure virtuelle (s)
static bool dans(const std::vector<Fenetre>& v, double t, const Fenetre** f = nullptr) {
  for (auto& x : v) if (t >= x.a && t < x.b) { if (f) *f = &x; return true; }
  return false;
}

// ---------------------------------------------------------------- ordonnanceur
struct Tache {
  int id; std::string nom; uint64_t reveil = 0; std::condition_variable cv;
  bool surveillee = false; uint64_t dernierReset = 0;
};
static std::mutex verrou;
static std::vector<Tache*> taches;
static Tache* courante = nullptr;
static thread_local Tache* moi = nullptr;
static uint64_t tUs = 0;
static uint64_t finUs = 0;
static uint32_t delaiWdtS = 0;
static std::FILE* journal = nullptr;
static std::FILE* fichierImages = nullptr;

[[noreturn]] static void finir(int code, const char* raison);

static void verifier() {
  if (tUs >= finUs) finir(0, "fin de la durée simulée (coupure de courant)");
  if (delaiWdtS) {
    for (auto* t : taches) {
      if (t->surveillee && tUs - t->dernierReset > (uint64_t)delaiWdtS * 1000000ULL) {
        std::fprintf(stdout, "\n!!! WATCHDOG : la tâche « %s » n'a pas donné signe de vie depuis %u s\n", t->nom.c_str(), delaiWdtS);
        finir(43, "watchdog");
      }
    }
  }
}

// Passe la main à la tâche qui doit se réveiller le plus tôt (à égalité : la suivante dans l'ordre, tourniquet)
static void basculer(std::unique_lock<std::mutex>& l) {
  Tache* suivante = nullptr;
  size_t n = taches.size();
  for (size_t k = 1; k <= n; k++) {
    Tache* t = taches[(moi->id + k) % n];
    if (!suivante || t->reveil < suivante->reveil) suivante = t;
  }
  if (suivante->reveil > tUs) tUs = suivante->reveil;
  verifier();
  if (suivante == moi) return;
  courante = suivante;
  suivante->cv.notify_one();
  moi->cv.wait(l, [] { return courante == moi; });
}

uint64_t maintenantUs() { return tUs; }
void attendre(uint64_t us) {
  std::unique_lock<std::mutex> l(verrou);
  moi->reveil = tUs + us;
  basculer(l);
}
static double epoch() { return epochBoot + tUs / 1e6; }

// ---------------------------------------------------------------- heure (NTP)
static bool ntpDemande = false, ntpOK = false;
static uint64_t tNtpDemande = 0;
static bool wifiConnecte();
time_t heureSysteme() {
  if (!ntpOK && ntpDemande && wifiConnecte() && tUs - tNtpDemande > 1500000ULL) ntpOK = true;
  if (ntpOK) return (time_t)epoch();
  return (time_t)(tUs / 1000000ULL);   // l'ESP32 compte depuis 1970 tant que NTP n'a pas répondu
}

// ---------------------------------------------------------------- journal série
static bool debutLigne = true;
static void ecrireJournal(char c) {
  if (debutLigne && c != '\r') {
    time_t e = (time_t)epoch();
    struct tm t; localtime_r(&e, &t);
    char b[48];
    std::snprintf(b, sizeof b, "[%02d/%02d %02d:%02d:%02d +%7.1fs] ", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min, t.tm_sec, tUs / 1e6);
    std::fputs(b, stdout);
    if (journal) std::fputs(b, journal);
    debutLigne = false;
  }
  if (c == '\r') return;
  std::fputc(c, stdout);
  if (journal) std::fputc(c, journal);
  if (c == '\n') debutLigne = true;
}
static void evenement(const char* fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  std::string s = std::string("    [sim] ") + buf + "\n";
  if (!debutLigne) ecrireJournal('\n');
  for (char c : s) ecrireJournal(c);
}

static void bilan();
[[noreturn]] static void finir(int code, const char* raison) {
  bilan();
  evenement("arrêt de la carte : %s (code %d)", raison, code);
  std::fflush(stdout);
  if (journal) std::fflush(journal);
  if (fichierImages) std::fflush(fichierImages);
  std::ofstream(dossier + "/arret.txt") << code << " " << (long long)epoch() << " " << (ntpOK ? 1 : 0) << "\n";
  _exit(code);
}

// ---------------------------------------------------------------- capteurs (modèle d'une salle)
// CO2 : la salle se remplit en semaine (8 h-12 h, 13 h 30-17 h 30), on aère aux pauses et selon le scénario.
static double co2 = 480, pm25 = 6;
static double tModele = -1;
static bool occupee(double e) {
  time_t t = (time_t)e; struct tm l; localtime_r(&t, &l);
  if (l.tm_wday == 0 || l.tm_wday == 6) return false;
  double h = l.tm_hour + l.tm_min / 60.0;
  return (h >= 8 && h < 12) || (h >= 13.5 && h < 17.5);
}
static bool aeration(double e) {
  if (dans(fenetresOuvertes, e)) return true;
  time_t t = (time_t)e; struct tm l; localtime_r(&t, &l);
  double h = l.tm_hour + l.tm_min / 60.0;
  return (h >= 10.25 && h < 10.42) || (h >= 15.5 && h < 15.67) || (h >= 12 && h < 12.25);   // pauses
}
static void avancerModele() {
  double e = epoch();
  if (tModele < 0) tModele = e;
  double dt = e - tModele;
  tModele = e;
  if (dt <= 0) return;
  double cible = aeration(e) ? 430 : (occupee(e) ? 1900 : 450);
  double tau = aeration(e) ? 240 : (occupee(e) ? 2700 : 5400);
  co2 += (cible - co2) * (1 - std::exp(-dt / tau));
  const Fenetre* f = nullptr;
  double fondPM = 6 + 2 * std::sin(e / 7200.0);
  if (dans(pollutions, e, &f)) {   // épisode : montée puis descente (triangle)
    double x = (e - f->a) / (f->b - f->a);
    fondPM += f->v1 * (x < 0.5 ? 2 * x : 2 * (1 - x));
  }
  pm25 += (fondPM - pm25) * (1 - std::exp(-dt / 120));
}
static double pm10Rapport = 1.6;

// ---------------------------------------------------------------- Wi-Fi
enum EtatW { W_REPOS, W_CONNEXION, W_CONNECTE };
static EtatW etatW = W_REPOS;
static uint64_t tDebutConnexion = 0;
static std::string ssidVise, mdpVise;
static int raisonEchec = 0;
static int apCourant = -1;
static bool scanEnCours = false;
static uint64_t tScan = 0;
static std::vector<int> resultatsScan;

static int trouverAP(const std::string& ssid) {
  for (size_t i = 0; i < pointsAcces.size(); i++) if (pointsAcces[i].ssid == ssid) return (int)i;
  return -1;
}
static bool apVisible(int i) { return i >= 0 && !dans(coupuresWifi, epoch()); }
static void signalerDeconnexion(uint8_t raison) {
  if (WiFi.evenement) {
    WiFiEventInfo_t info; info.wifi_sta_disconnected.reason = raison;
    WiFi.evenement(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
  }
}
static void majWifi() {
  if (etatW == W_CONNEXION && tUs - tDebutConnexion >= 2500000ULL) {
    int i = trouverAP(ssidVise);
    if (!apVisible(i)) { etatW = W_REPOS; signalerDeconnexion(WIFI_REASON_NO_AP_FOUND); evenement("Wi-Fi : %s introuvable", ssidVise.c_str()); }
    else if (!pointsAcces[i].ent && pointsAcces[i].mdp != mdpVise) { etatW = W_REPOS; signalerDeconnexion(WIFI_REASON_AUTH_FAIL); evenement("Wi-Fi : mauvais mot de passe pour %s", ssidVise.c_str()); }
    else { etatW = W_CONNECTE; apCourant = i; }
  }
  if (etatW == W_CONNECTE && !apVisible(apCourant)) {
    etatW = W_REPOS;
    evenement("Wi-Fi : le point d'accès %s a disparu (coupure simulée)", pointsAcces[apCourant].ssid.c_str());
    signalerDeconnexion(WIFI_REASON_BEACON_TIMEOUT);
  }
}
static bool wifiConnecte() { majWifi(); return etatW == W_CONNECTE; }

// ---------------------------------------------------------------- serveur AirCarto simulé
static double dernierDebutPost = -1e9;
static long nbAcceptes = 0, nb429 = 0, nbRefus = 0, nbEchecsReseau = 0;
static std::string normaliserMac(std::string s) {
  std::string r;
  for (char c : s) if (c != ':') r += (char)toupper((unsigned char)c);
  return r;
}
static int serveurRecoit(const std::string& cle, const std::string& corps, std::string& reponse) {
  double e = epoch();
  const Fenetre* f = nullptr;
  if (dans(pannesServeur, e, &f)) { reponse = "{\"ok\":false,\"message\":\"maintenance\"}"; return f->code ? f->code : 503; }
  if (cle != cleServeur) { nbRefus++; reponse = "{\"ok\":false,\"message\":\"clé invalide\"}"; return 401; }
  if (e - dernierDebutPost < 10.0 || alea() < proba429) {
    nb429++;
    int reste = (int)std::ceil(10.0 - (e - dernierDebutPost));
    if (reste < 1) reste = 1;
    reponse = "{\"ok\":false,\"message\":\"trop rapproché\",\"next_push_in_s\":" + std::to_string(reste) + "}";
    return 429;
  }
  dernierDebutPost = e;
  JsonDocument doc;
  if (deserializeJson(doc, corps)) { nbRefus++; reponse = "{\"ok\":false,\"message\":\"JSON illisible\"}"; return 400; }
  int nbX = 0;
  for (JsonPair p : doc.as<JsonObject>()) {
    std::string k = p.key().c_str();
    if (k.rfind("x_", 0) == 0) nbX++;
    if (k != "device_id" && !p.value().is<double>()) { nbRefus++; reponse = "{\"ok\":false,\"message\":\"champ non numérique : " + k + "\"}"; return 400; }
    if (k[0] == '_') { nbRefus++; reponse = "{\"ok\":false,\"message\":\"champ interne envoyé : " + k + "\"}"; return 400; }
  }
  if (nbX > 10) { nbRefus++; reponse = "{\"ok\":false,\"message\":\"plus de 10 champs x_\"}"; return 400; }
  // enregistrement, daté à la RÉCEPTION (comme le vrai serveur)
  JsonDocument ligne;
  time_t te = (time_t)e; struct tm u; gmtime_r(&te, &u);
  char iso[32]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &u);
  ligne["time"] = iso;
  for (JsonPair p : doc.as<JsonObject>()) {
    if (std::string(p.key().c_str()) == "device_id") ligne["device_id"] = normaliserMac(p.value().as<const char*>());
    else ligne[p.key()] = p.value();
  }
  std::string s; serializeJson(ligne, s);
  std::ofstream(dossier + "/serveur.jsonl", std::ios::app) << s << "\n";
  nbAcceptes++;
  const Fenetre* fa = nullptr;
  long suivant = dans(attentesServeur, e, &fa) ? (long)fa->v1 : 10;
  reponse = "{\"ok\":true,\"next_push_in_s\":" + std::to_string(suivant) + "}";
  return 200;
}

// ---------------------------------------------------------------- Preferences (NVS) persistantes
static std::map<std::string, std::string> nvs;
static bool nvsCharge = false;
static void chargerNvs() {
  if (nvsCharge) return;
  nvsCharge = true;
  std::ifstream f(dossier + "/nvs.txt");
  std::string l;
  while (std::getline(f, l)) { auto p = l.find('\t'); if (p != std::string::npos) nvs[l.substr(0, p)] = l.substr(p + 1); }
}
static long nbEcrituresNvs = 0;
static void sauverNvs() {
  nbEcrituresNvs++;
  std::ofstream f(dossier + "/nvs.txt");
  for (auto& kv : nvs) f << kv.first << "\t" << kv.second << "\n";
}

// ---------------------------------------------------------------- lecture de l'environnement
static void chargerScenario(const std::string& chemin) {
  std::ifstream f(chemin);
  std::string l;
  while (std::getline(f, l)) {
    if (l.empty() || l[0] == '#') continue;
    std::istringstream is(l);
    std::string mot; is >> mot;
    if (mot == "cle") is >> cleServeur;
    else if (mot == "ap") { PointAcces a; std::string ent; is >> a.ssid >> a.rssi >> a.mdp >> ent; a.ent = (ent == "ent"); pointsAcces.push_back(a); }
    else if (mot == "coupure_wifi") { Fenetre w; is >> w.a >> w.b; coupuresWifi.push_back(w); }
    else if (mot == "panne_serveur") { Fenetre w; is >> w.a >> w.b >> w.code; pannesServeur.push_back(w); }
    else if (mot == "batterie") { Fenetre w; is >> w.a >> w.b; batteries.push_back(w); }
    else if (mot == "images") { Fenetre w; is >> w.a >> w.b; fenetresImages.push_back(w); }
    else if (mot == "pollution") { Fenetre w; is >> w.a >> w.b >> w.v1; pollutions.push_back(w); }
    else if (mot == "fenetre") { Fenetre w; is >> w.a >> w.b; fenetresOuvertes.push_back(w); }
    else if (mot == "wifi_zombie") { Fenetre w; is >> w.a >> w.b; zombies.push_back(w); }
    else if (mot == "attente_serveur") { Fenetre w; is >> w.a >> w.b >> w.v1; attentesServeur.push_back(w); }
    else if (mot == "derive_co2") { Fenetre w; is >> w.a >> w.b >> w.v1; derivesCO2.push_back(w); }
    else if (mot == "proba429") is >> proba429;
    else if (mot == "cmd") {   // cmd <epoch> <commande...>
      Commande c; is >> c.t; std::getline(is, c.texte);
      size_t a = c.texte.find_first_not_of(' '); c.texte = (a == std::string::npos) ? "" : c.texte.substr(a);
      commandes.push_back(c);
    }
  }
}
}  // namespace sim

using namespace sim;

// ================================================================ Serial
static std::string entreeSerie;
static size_t prochaineCommande = 0;
size_t HardwareSerial::write(uint8_t c) { if (num == 0) ecrireJournal((char)c); return 1; }
int HardwareSerial::available() {
  if (num != 0) return 0;
  while (prochaineCommande < commandes.size() && commandes[prochaineCommande].t <= epoch()) {
    evenement("on tape : %s", commandes[prochaineCommande].texte.c_str());
    entreeSerie += commandes[prochaineCommande].texte + "\r\n";
    prochaineCommande++;
  }
  return (int)entreeSerie.size();
}
int HardwareSerial::read() {
  if (entreeSerie.empty()) return -1;
  int c = (unsigned char)entreeSerie[0];
  entreeSerie.erase(0, 1);
  return c;
}

// ================================================================ broches, ESP
void pinMode(uint8_t, uint8_t) {}
int digitalRead(uint8_t broche) { return (broche == 34) ? (dans(batteries, epoch()) ? 0 : 1) : 1; }
void digitalWrite(uint8_t, uint8_t) {}
uint32_t EspClass::getMaxAllocHeap() { return 110000; }
uint32_t EspClass::getFreeHeap() { return 214000 - (uint32_t)(alea() * 3000); }
void EspClass::restart() { finir(42, "ESP.restart()"); }
esp_reset_reason_t esp_reset_reason() { return (esp_reset_reason_t)raisonReset; }
uint32_t esp_random() { return (uint32_t)rng(); }
int esp_read_mac(uint8_t* mac, esp_mac_type_t) { uint8_t m[6] = {0x5C, 0x01, 0x3B, 0xF1, 0x53, 0xBC}; memcpy(mac, m, 6); return 0; }
esp_err_t esp_task_wdt_init(uint32_t s, bool) { delaiWdtS = s; return 0; }
esp_err_t esp_task_wdt_add(void*) { moi->surveillee = true; moi->dernierReset = tUs; return 0; }
esp_err_t esp_task_wdt_reset() { if (moi) moi->dernierReset = tUs; return 0; }
esp_err_t esp_wifi_sta_wpa2_ent_set_identity(const uint8_t*, int) { return 0; }
esp_err_t esp_wifi_sta_wpa2_ent_set_username(const uint8_t*, int) { return 0; }
esp_err_t esp_wifi_sta_wpa2_ent_set_password(const uint8_t*, int) { return 0; }
esp_err_t esp_wifi_sta_wpa2_ent_enable() { return 0; }
esp_err_t esp_wifi_sta_wpa2_ent_disable() { return 0; }
void configTzTime(const char* tz, const char*) { setenv("TZ", tz, 1); tzset(); if (!ntpDemande) { ntpDemande = true; tNtpDemande = tUs; } }

// ================================================================ FreeRTOS
SemaphoreHandle_t xSemaphoreCreateBinary() { return (void*)1; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)2; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }   // un seul fil tourne à la fois
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t* w) { if (w) *w = pdFALSE; return pdTRUE; }
void vTaskDelay(TickType_t t) { attendre((uint64_t)t * 1000ULL); }
BaseType_t xTaskCreatePinnedToCore(void (*f)(void*), const char* nom, uint32_t, void* p, unsigned, TaskHandle_t*, int) {
  if (std::string(nom) == "refresh") return pdTRUE;   // rafraîchissement matériel de l'écran : inutile ici
  Tache* t = new Tache();
  t->id = (int)taches.size();
  t->nom = nom;
  t->reveil = tUs;
  taches.push_back(t);
  std::thread([t, f, p] {
    {
      std::unique_lock<std::mutex> l(verrou);
      moi = t;
      t->cv.wait(l, [t] { return courante == t; });
    }
    f(p);
  }).detach();
  return pdTRUE;
}
hw_timer_t* timerBegin(uint8_t n, uint16_t, bool) { static hw_timer_t t; t.n = n; return &t; }
void timerAttachInterrupt(hw_timer_t*, void (*)(), bool) {}
void timerAlarmWrite(hw_timer_t*, uint64_t, bool) {}
void timerAlarmEnable(hw_timer_t*) {}

// ================================================================ Wi-Fi
bool WiFiClass::disconnect(bool, bool) {
  bool etait = (etatW != W_REPOS);
  etatW = W_REPOS;
  if (etait) signalerDeconnexion(WIFI_REASON_ASSOC_LEAVE);
  return true;
}
wl_status_t WiFiClass::begin(const char* ssid, const char* mdp) {
  ssidVise = ssid ? ssid : "";
  mdpVise = mdp ? mdp : "";
  etatW = W_CONNEXION;
  tDebutConnexion = tUs;
  return WL_DISCONNECTED;
}
wl_status_t WiFiClass::status() { return wifiConnecte() ? WL_CONNECTED : WL_DISCONNECTED; }
int16_t WiFiClass::scanNetworks(bool async, bool) {
  resultatsScan.clear();
  scanEnCours = true;
  tScan = tUs;
  if (!async) { attendre(2000000ULL); return scanComplete(); }
  return WIFI_SCAN_RUNNING;
}
int16_t WiFiClass::scanComplete() {
  if (!scanEnCours) return WIFI_SCAN_FAILED;
  if (tUs - tScan < 1800000ULL) return WIFI_SCAN_RUNNING;
  resultatsScan.clear();
  if (!dans(coupuresWifi, epoch()))
    for (size_t i = 0; i < pointsAcces.size(); i++) resultatsScan.push_back((int)i);
  return (int16_t)resultatsScan.size();
}
void WiFiClass::scanDelete() { scanEnCours = false; resultatsScan.clear(); }
String WiFiClass::SSID(uint8_t i) const { return i < resultatsScan.size() ? String(pointsAcces[resultatsScan[i]].ssid.c_str()) : String(); }
String WiFiClass::SSID() const { return apCourant >= 0 && etatW == W_CONNECTE ? String(pointsAcces[apCourant].ssid.c_str()) : String(); }
int32_t WiFiClass::RSSI(uint8_t i) const { return i < resultatsScan.size() ? pointsAcces[resultatsScan[i]].rssi : 0; }
int32_t WiFiClass::RSSI() const {
  if (apCourant < 0 || etatW != W_CONNECTE) return 0;
  return pointsAcces[apCourant].rssi + (int)std::lround(gauss(2.0));
}
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i) const {
  if (i >= resultatsScan.size()) return WIFI_AUTH_OPEN;
  const auto& a = pointsAcces[resultatsScan[i]];
  return a.ent ? WIFI_AUTH_WPA2_ENTERPRISE : (a.mdp == "-" ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK);
}

// ================================================================ HTTP
bool HTTPClient::begin(const String& u) { url = u; return u.startsWith("http"); }
void HTTPClient::addHeader(const String& nom, const String& valeur) { if (nom == "X-Group-Key") cle = valeur; }
String HTTPClient::errorToString(int code) {
  switch (code) {
    case -1: return String("connection refused");
    case -11: return String("read Timeout");
    default: return String("erreur ") + String(code);
  }
}
int HTTPClient::POST(const String& corps) {
  const Fenetre* fz = nullptr;
  bool fantome = dans(zombies, epoch(), &fz) && (epochBoot + tDebutConnexion / 1e6) < fz->a;
  if (!wifiConnecte() || fantome) { attendre((uint64_t)tConnexion * 1000ULL); nbEchecsReseau++; return -1; }
  attendre(150000ULL + (uint64_t)(alea() * 400000));   // aller-retour réseau 150..550 ms
  std::string rep;
  int code = serveurRecoit(cle.s, corps.s, rep);
  reponse = String(rep.c_str());
  return code;
}

// ================================================================ capteurs
uint8_t ModbusMaster::readHoldingRegisters(uint16_t reg, uint16_t n) {
  attendre(8000);   // ~8 ms par échange Modbus
  if (alea() < probaEchecModbus) return ku8MBResponseTimedOut;
  avancerModele();
  memset(tampon, 0, sizeof tampon);
  auto mettre = [&](int i, double v) {
    uint32_t r = (uint32_t)std::lround(std::max(0.0, v) * 1000.0);
    tampon[2 * i] = (uint16_t)(r & 0xFFFF); tampon[2 * i + 1] = (uint16_t)(r >> 16);
  };
  double bruit = 1 + gauss(0.06);
  double p25 = pm25 * bruit, p1 = p25 * 0.66, p10 = p25 * pm10Rapport * (1 + gauss(0.04));
  if (reg == 0x13) { tampon[0] = (tUs < 15000000ULL) ? 0x04 : 0x00; return ku8MBSuccess; }   // démarrage : 15 s
  if (reg == 0x38 || reg == 0x44) { double v[3] = {p1, p25, p10}; for (int i = 0; i < n / 2 && i < 3; i++) mettre(i, v[i]); return ku8MBSuccess; }
  if (reg == 0x3A || reg == 0x46) { mettre(0, p25); return ku8MBSuccess; }
  if (reg == 0x3C || reg == 0x48) { mettre(0, p10); return ku8MBSuccess; }
  if (reg == 0x6A) { tampon[0] = (uint16_t)std::lround((44 + gauss(0.5)) * 100); if (n >= 2) tampon[1] = (uint16_t)(int16_t)std::lround((26.5 + gauss(0.2)) * 100); return ku8MBSuccess; }
  if (reg == 0x6B) { tampon[0] = (uint16_t)(int16_t)std::lround((26.5 + gauss(0.2)) * 100); return ku8MBSuccess; }
  return ku8MBResponseTimedOut;
}
void MHZ19::calibrate() { evenement("MH-Z19C : calibrage du zéro demandé"); }
int MHZ19::getCO2(bool, bool) {
  attendre(20000);
  if (alea() < probaEchecMhz) { errorCode = RESULT_TIMEOUT; return 0; }
  errorCode = RESULT_OK;
  avancerModele();
  double derive = 0;
  const Fenetre* f = nullptr;
  if (dans(derivesCO2, epoch(), &f)) {   // capteur qui dérive (zéro décalé) : descend jusqu'à -v1 ppm puis revient
    double x = (epoch() - f->a) / (f->b - f->a);
    derive = f->v1 * (x < 0.5 ? 2 * x : 2 * (1 - x));
  }
  return (int)std::lround(co2 - derive + gauss(8));
}

// ================================================================ écran
static uint16_t derniereImage[32][64];
static bool imageEnregistree = false;
static long nbImages = 0;
void PxMATRIX::showBuffer() {
  double e = epoch();
  if (!dans(fenetresImages, e)) return;
  if (imageEnregistree && memcmp(derniereImage, dessin, sizeof dessin) == 0) return;
  memcpy(derniereImage, dessin, sizeof dessin);
  imageEnregistree = true;
  if (!fichierImages) fichierImages = std::fopen((dossier + "/images.bin").c_str(), "ab");
  uint32_t entete[3] = {(uint32_t)e, (uint32_t)((e - std::floor(e)) * 1000), (uint32_t)(pageCourante | (luminositeCourante << 8))};
  std::fwrite(entete, sizeof entete, 1, fichierImages);
  std::fwrite(dessin, sizeof dessin, 1, fichierImages);
  nbImages++;
}

// ================================================================ LittleFS -> dossier
static std::string cheminFs(const char* p) { return dossier + "/fs" + p; }
size_t File::write(uint8_t c) { return fp ? std::fwrite(&c, 1, 1, fp) : 0; }
size_t File::write(const uint8_t* b, size_t n) { return fp ? std::fwrite(b, 1, n, fp) : 0; }
size_t File::size() { if (!fp) return 0; long p = std::ftell(fp); std::fseek(fp, 0, SEEK_END); long s = std::ftell(fp); std::fseek(fp, p, SEEK_SET); return (size_t)s; }
size_t File::position() { return fp ? (size_t)std::ftell(fp) : 0; }
int File::available() { return fp ? (int)(size() - position()) : 0; }
int File::read() { return fp ? std::fgetc(fp) : -1; }
size_t File::read(uint8_t* b, size_t n) { return fp ? std::fread(b, 1, n, fp) : 0; }
bool File::seek(size_t pos) { return fp && std::fseek(fp, (long)pos, SEEK_SET) == 0 && pos <= size(); }
String File::readStringUntil(char fin) {
  std::string s; int c;
  while (fp && (c = std::fgetc(fp)) != EOF && c != fin) s += (char)c;
  return String(s);
}
void File::close() { if (fp) std::fclose(fp); fp = nullptr; }
bool LittleFSFS::begin(bool) { mkdir((dossier + "/fs").c_str(), 0755); return true; }
File LittleFSFS::open(const char* p, const char* mode) {
  attendre(3000);   // accès flash
  std::string m = std::string(mode) + "b";
  if (m == "rb" && !exists(p)) return File();
  return File(std::fopen(cheminFs(p).c_str(), m.c_str()), nullptr);
}
bool LittleFSFS::exists(const char* p) { struct stat s; return stat(cheminFs(p).c_str(), &s) == 0; }
bool LittleFSFS::remove(const char* p) { return ::remove(cheminFs(p).c_str()) == 0; }
bool LittleFSFS::rename(const char* a, const char* b) { return ::rename(cheminFs(a).c_str(), cheminFs(b).c_str()) == 0; }
size_t LittleFSFS::totalBytes() { return 1441792; }   // partition « spiffs » par défaut d'un ESP32 4 Mo
size_t LittleFSFS::usedBytes() {
  size_t total = 2 * 4096;
  if (DIR* d = opendir((dossier + "/fs").c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      struct stat s; if (stat((dossier + "/fs/" + e->d_name).c_str(), &s) == 0) total += ((s.st_size + 4095) / 4096) * 4096;
    }
    closedir(d);
  }
  return total;
}

// ================================================================ Preferences
bool Preferences::begin(const char*, bool) { chargerNvs(); return true; }
String Preferences::getString(const char* k, const String& d) { auto it = nvs.find(k); return it == nvs.end() ? d : String(it->second.c_str()); }
size_t Preferences::putString(const char* k, const String& v) { nvs[k] = v.s; sauverNvs(); return v.length(); }
uint32_t Preferences::getULong(const char* k, uint32_t d) { auto it = nvs.find(k); return it == nvs.end() ? d : (uint32_t)std::stoul(it->second); }
size_t Preferences::putULong(const char* k, uint32_t v) { nvs[k] = std::to_string(v); sauverNvs(); return 4; }
bool Preferences::getBool(const char* k, bool d) { auto it = nvs.find(k); return it == nvs.end() ? d : it->second == "1"; }
size_t Preferences::putBool(const char* k, bool v) { nvs[k] = v ? "1" : "0"; sauverNvs(); return 1; }
uint8_t Preferences::getUChar(const char* k, uint8_t d) { auto it = nvs.find(k); return it == nvs.end() ? d : (uint8_t)std::stoi(it->second); }
size_t Preferences::putUChar(const char* k, uint8_t v) { nvs[k] = std::to_string(v); sauverNvs(); return 1; }
int8_t Preferences::getChar(const char* k, int8_t d) { auto it = nvs.find(k); return it == nvs.end() ? d : (int8_t)std::stoi(it->second); }
size_t Preferences::putChar(const char* k, int8_t v) { nvs[k] = std::to_string(v); sauverNvs(); return 1; }
bool Preferences::remove(const char* k) { bool e = nvs.erase(k) > 0; if (e) sauverNvs(); return e; }

// ================================================================ bilan
namespace sim {
static void bilan() {
  evenement("bilan : %ld envois acceptés, %ld refus 429, %ld refusés (clé / format), %ld échecs réseau, %ld écritures NVS, %ld images, %lu pixels hors écran",
            nbAcceptes, nb429, nbRefus, nbEchecsReseau, nbEcrituresNvs, nbImages, display.horsCadre);
  std::ofstream(dossier + "/salle.txt") << co2 << " " << pm25 << "\n";
}
}

// ================================================================ programme principal
void setup();
void loop();
int main() {
  auto env = [](const char* k, const char* d) { const char* v = getenv(k); return std::string(v ? v : d); };
  dossier = env("SIM_DOSSIER", "etat");
  epochBoot = std::stod(env("SIM_EPOCH_BOOT", "1795161600"));
  dureeS = std::stod(env("SIM_DUREE_S", "3600"));
  raisonReset = std::stoi(env("SIM_RESET", "1"));
  ntpOK = env("SIM_NTP", "0") == "1";   // l'heure de l'ESP32 survit à un redémarrage logiciel (pas à une coupure)
  graine = (uint32_t)std::stoul(env("SIM_GRAINE", "1"));
  rng.seed(graine);
  mkdir(dossier.c_str(), 0755);
  chargerScenario(env("SIM_SCENARIO", "scenario.txt"));
  while (prochaineCommande < commandes.size() && commandes[prochaineCommande].t < epochBoot) prochaineCommande++;
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
  // état physique de la salle au démarrage (continuité entre deux mises sous tension)
  { std::ifstream f(dossier + "/salle.txt"); double a, b; if (f >> a >> b) { co2 = a; pm25 = b; } }
  journal = std::fopen((dossier + "/journal.txt").c_str(), "a");
  finUs = (uint64_t)(dureeS * 1e6);
  // le fil principal = la tâche loop()
  Tache* t0 = new Tache(); t0->id = 0; t0->nom = "loopTask"; taches.push_back(t0);
  moi = t0; courante = t0;
  evenement("mise sous tension (raison %d), scénario : %zu AP, %zu commandes", raisonReset, pointsAcces.size(), commandes.size());
  setup();
  for (;;) loop();
}
