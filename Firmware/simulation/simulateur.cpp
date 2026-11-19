// Simulation d'une coupure Wi-Fi avec le VRAI code d'envoi du firmware (extrait de main.cpp, non modifié),
// compilé sur PC avec un faux environnement ESP32 (shim/Arduino.h) et un faux serveur AirCarto.
// Une instance de Carte = un démarrage de la carte (la RAM repart à zéro) ; la flash (LittleFS), les préférences (NVS)
// et l'horloge (si redémarrage logiciel) survivent d'un démarrage à l'autre, comme sur la vraie carte.
#include "Arduino.h"
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <ArduinoJson.h>
#include <vector>
#include <random>
#include <fstream>
#include <iostream>

#define MODE_DEMO 0
#define WIFI_ACTIVE 1
#define AFFICHER_TEMOINS 1
#define LARGEUR_BAS (AFFICHER_TEMOINS ? 56 : 64)
#define HIST_N (LARGEUR_BAS - 3)
#define time(x) sim_time(x)

struct Carte {
#include "extrait_carte.inc"
};
#undef time

// ---------------- monde simulé ----------------
uint64_t simReelMs = 0, simDemarrageMs = 0, simDecalageMillis = 0;
double simEpochDepart = 1795068000.0;   // jeudi 19 novembre 2026, 06:00:00 UTC (07:00 à Paris)
bool simRtcValide = false;
bool simWifiConnecte = false;
int simCauseReset = 1;                  // 1 = mise sous tension, 3 = redémarrage logiciel
FauxSerial Serial; FauxESP ESP; FauxWiFi WiFi; FauxLittleFS LittleFS; Preferences prefs;
std::function<int(const std::string&, std::string&)> simServeurPost;

struct Scenario {
  std::string nom;
  double dureeH;                                   // durée simulée (h)
  std::vector<std::pair<double, double>> sansWifi; // [début, fin] en heures depuis 07:00 (heure de Paris)
  std::vector<std::pair<double, double>> sansCourant;
  std::vector<double> redemarragesLogiciels;
  double pEchec = 0, pReponsePerdue = 0;          // réseau instable : POST perdu / réponse perdue après enregistrement
  double uptimeDepartH = 0;                        // uptime de la carte au début (test du retour à zéro de millis)
};

static bool dans(const std::vector<std::pair<double, double>>& v, double h) {
  for (auto& p : v) if (h >= p.first && h < p.second) return true;
  return false;
}

int main(int argc, char** argv) {
  std::string nom = argc > 1 ? argv[1] : "coupure2h";
  std::vector<Scenario> tous = {
    {"coupure2h", 6, {{1, 3}}, {}, {}},
    {"coupure5h_courant", 9, {{1, 6}}, {{3, 3 + 20 / 60.0}}, {}},
    {"demarrage_sans_wifi_puis_courant", 5, {{0, 2}}, {{1, 1 + 5 / 60.0}}, {}},
    {"coupure14h_memoire_pleine", 22, {{1, 15}}, {}, {}},
    {"reseau_instable_redemarrage", 5, {{1, 2.5}}, {}, {1.75}, 0.05, 0.01},
    {"millis_49j", 4, {{1, 2}}, {}, {}, 0, 0, 49.71 * 24 - 1.5},
    {"coupure20h_memoire_pleine", 32, {{1, 21}}, {}, {}},
  };
  Scenario sc; bool trouve = false;
  for (auto& s : tous) if (s.nom == nom) { sc = s; trouve = true; }
  if (!trouve) { std::cerr << "scénario inconnu\n"; return 1; }

  std::mt19937 alea(42);
  std::uniform_real_distribution<double> u01(0, 1);

  // ---------------- serveur AirCarto simulé ----------------
  struct Recu { uint64_t recuMs; std::string corps; };
  std::vector<Recu> serveur;
  uint64_t dernierAccepte = 0; bool aRecu = false;
  simServeurPost = [&](const std::string& corps, std::string& rep) -> int {
    if (!simWifiConnecte) return -1;
    double r = u01(alea);
    if (r < sc.pEchec) return -1;                              // requête perdue en route
    uint64_t recu = simReelMs + 300;                           // ~0,3 s de trajet
    if (aRecu && recu - dernierAccepte < 10000) {              // règle du serveur : un envoi toutes les 10 s
      rep = "{\"ok\":false,\"next_push_in_s\":" + std::to_string((10000 - (recu - dernierAccepte) + 999) / 1000) + "}";
      return 429;
    }
    dernierAccepte = recu; aRecu = true;
    serveur.push_back({recu, corps});
    if (r < sc.pEchec + sc.pReponsePerdue) return -11;         // enregistré, mais la réponse se perd
    rep = "{\"ok\":true,\"next_push_in_s\":10}";
    return 200;
  };

  // ---------------- vérité terrain et journal ----------------
  struct Verite { int id; uint64_t reelMs; uint32_t boot; };
  std::vector<Verite> verite;
  std::vector<std::pair<uint64_t, std::string>> journal;
  Serial.sortie = [&](const std::string& m) { journal.push_back({simReelMs, m}); };

  std::unique_ptr<Carte> carte;
  uint32_t tLecture = 0, tPeriode = 0;
  int idPeriode = 1;
  uint64_t wifiDepuis = 0; bool apVu = false; uint64_t ntpA = 0;
  int nbDemarrages = 0;

  auto demarrer = [&](int cause) {
    simCauseReset = cause;
    if (cause == 1) simRtcValide = false;                      // mise sous tension : l'horloge est perdue
    simDemarrageMs = simReelMs;
    if (nbDemarrages > 0) simDecalageMillis = 0;
    carte.reset(new Carte());
    Carte& c = *carte;
    c.mutexEnvoi = xSemaphoreCreateMutex();
    c.bootId = 1000 + (++nbDemarrages);
    c.deviceId = "AA:BB:CC:DD:EE:FF";
    c.cfgCle = "CLE-RETIREE-DE-L-HISTORIQUE";
    c.offsetRejeu = prefs.getULong("off", 0);
    c.fsOK = true;
    c.initialiserBacklog();
    tLecture = 0; tPeriode = 0;
    simWifiConnecte = false; wifiDepuis = 0; ntpA = 0;
    journal.push_back({simReelMs, "=== démarrage " + std::to_string(c.bootId) + (cause == 1 ? " (mise sous tension)" : " (redémarrage logiciel)")});
  };

  simDecalageMillis = (uint64_t)(sc.uptimeDepartH * 3600000.0);
  simRtcValide = false;
  demarrer(1);
  if (sc.uptimeDepartH > 0) simRtcValide = true;               // carte allumée depuis longtemps : déjà à l'heure

  const uint64_t PAS = 50, FIN = (uint64_t)(sc.dureeH * 3600000.0);
  bool allumee = true;
  for (simReelMs = PAS; simReelMs <= FIN; simReelMs += PAS) {
    double h = simReelMs / 3600000.0;
    // courant
    bool courant = !dans(sc.sansCourant, h);
    if (allumee && !courant) { allumee = false; carte.reset(); simWifiConnecte = false; journal.push_back({simReelMs, "--- coupure de courant"}); }
    if (!allumee && courant) { allumee = true; demarrer(1); }
    if (!allumee) continue;
    for (double r : sc.redemarragesLogiciels)
      if (simReelMs == (uint64_t)(r * 3600000.0) / PAS * PAS) { journal.push_back({simReelMs, "--- ESP.restart()"}); demarrer(3); }
    // Wi-Fi : la box revient -> connexion en 5 s -> heure NTP 2 s plus tard
    bool ap = !dans(sc.sansWifi, h);
    if (!ap) { if (simWifiConnecte) journal.push_back({simReelMs, "--- Wi-Fi perdu"}); simWifiConnecte = false; wifiDepuis = 0; }
    else if (!simWifiConnecte) {
      if (!wifiDepuis) wifiDepuis = simReelMs;
      if (simReelMs - wifiDepuis >= 5000) { simWifiConnecte = true; journal.push_back({simReelMs, "--- Wi-Fi connecté"}); if (!simRtcValide) ntpA = simReelMs + 2000; }
    }
    if (ntpA && simReelMs >= ntpA) { simRtcValide = true; ntpA = 0; }

    Carte& c = *carte;
    uint32_t now = millis();
    // lecture des capteurs toutes les 10 s (même ordre que loop())
    if (now - tLecture >= c.INTERVALLE_LECTURE_MS) {
      tLecture = now;
      double tt = simEpochDepart + simReelMs / 1000.0;     // heure réelle de la lecture
      double co2 = 650 + 300 * std::sin(2 * M_PI * tt / 10800.0);
      double hParis = std::fmod((tt / 3600.0) + 1, 24);   // pic de CO2 de 09:30 à 09:40 (heure de Paris), pendant la coupure : repère facile à retrouver
      if (hParis >= 9.5 && hParis < 9.5 + 10 / 60.0) co2 = 2500;
      double pm = 9 + 6 * std::sin(2 * M_PI * tt / 7200.0);
      c.agregatAjouter(c.agCO2, (float)co2); c.nCO2Total++;
      c.agregatAjouter(c.agPM1, (float)(pm * 0.6)); c.agregatAjouter(c.agPM25, (float)pm); c.agregatAjouter(c.agPM10, (float)(pm * 1.6));
    }
    // fin de période : la mesure est construite ; on note son heure réelle (vérité) et son numéro (dans x_temp_pm)
    if (now - tPeriode >= c.periodeMesureMs()) {
      tPeriode = now;
      c.tempPM = idPeriode / 10.0f;
      verite.push_back({idPeriode, simReelMs, c.bootId});
      c.finaliserPeriode();
      idPeriode++;
    }
    c.gererEnvois(now);   // tâche d'envoi (réveillée toutes les 50 ms)
  }

  // ---------------- sortie ----------------
  std::ofstream o("res_" + nom + ".json");
  o << "{\"epochDepart\":" << (long long)simEpochDepart << ",\"verite\":[";
  for (size_t i = 0; i < verite.size(); i++) o << (i ? "," : "") << "[" << verite[i].id << "," << verite[i].reelMs << "," << verite[i].boot << "]";
  o << "],\"serveur\":[";
  for (size_t i = 0; i < serveur.size(); i++) o << (i ? "," : "") << "{\"recu\":" << serveur[i].recuMs << ",\"corps\":" << serveur[i].corps << "}";
  o << "],\"journal\":[";
  for (size_t i = 0; i < journal.size(); i++) {
    JsonDocument d; d.set(journal[i].second.c_str()); String s; serializeJson(d, s);
    o << (i ? "," : "") << "[" << journal[i].first << "," << s.s << "]";
  }
  o << "],\"enAttenteFin\":" << (carte ? carte->nbLignesAttente : 0) << "}";
  std::cout << nom << " : " << verite.size() << " mesures prises, " << serveur.size() << " reçues par le serveur, "
            << (carte ? carte->nbLignesAttente : 0) << " encore en attente à la fin\n";
}
