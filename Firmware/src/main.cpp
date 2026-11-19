/*
 * Station qualité de l'air : carte ModuleAir (ESP32)
 * Envoi DIRECT au serveur AirCarto, avec stockage des mesures en flash (LittleFS) quand le réseau est coupé, puis renvoi
 * automatique quand il revient.
 * Capteurs : NextPM (Modbus, UART1) | MH-Z19C (UART2) | écran PxMatrix 64x32
 * Dans le moniteur série (115200 bauds) :
 *   wifi ent eduroam prenom.nom@etu.univ-amu.fr MOT_DE_PASSE_ENT   (WPA2 Entreprise)
 *   wifi add MaBox MotDePasse                                      (Wi-Fi classique)
 *   wifi add "iPhone de Paul" MotDePasse                           (nom avec espaces : guillemets)
 *   set cle LA_CLE_DE_32_CARACTERES
 * La carte garde jusqu'à 5 réseaux en mémoire et se connecte toute seule au meilleur réseau
 * connu visible (pas besoin de reboot). Le type (entreprise / classique) est choisi PAR RÉSEAU.
 *   wifi list | wifi del <n> | wifi scan | wifi connect
 * Autres commandes : aide | status | verbose on/off | calib oui | vider_file | oublier
 * SAISIE : ce que tu tapes s'affiche (écho), Retour arrière efface un caractère,
 * Ctrl+U efface toute la ligne. Les flèches / Suppr sont ignorées.
 * Avant de rendre ou prêter la carte : tape "oublier" (ou pio run -t erase).
 * AFFICHAGE SÉRIE : par défaut la carte est SILENCIEUSE (verbose off), pour
 * pouvoir taper les commandes sans que le texte défile. "verbose on" réactive
 * les logs automatiques (mesures, envois) ; "status" donne l'état à la demande.
 * Pour couper aussi les logs du framework ESP32, ajoute dans platformio.ini :
 *   build_flags = -DCORE_DEBUG_LEVEL=0
 *
 * ÉCRAN : interrupteurs en haut du fichier
 *   DOUBLE_BUFFER (dessin sans scintillement), TRANSITION_FONDU (fondu entre les pages),
 *   ANIMATIONS (clignement, chiffres qui glissent, sommet de la courbe qui clignote).
 *   Pages (UNE information par écran) : CO2 | AERER (si CO2 élevé) | PM1 | PM2.5 | PM10 | INDICE ATMO | courbe CO2 | Wi-Fi.
 *   ALERTE HORS NORME : dès qu'un polluant franchit son seuil d'alerte (NIVEAU_ALERTE_PM / NIVEAU_ALERTE_CO2),
 *   l'écran saute AUSSITÔT sur sa page (sans fondu), clignote plein écran à la couleur du niveau, puis la valeur
 *   pulse ; tant que l'alerte dure, sa page revient une page sur deux (voir verifierAlertes).
 *   INDICE ATMO : règle officielle de l'indice ATMO = le PIRE des sous-indices (pas de moyenne, pas de
 *   pondération), calculé ici sur PM2.5 et PM10 seulement (voir niveauIndiceATMO).
 *   Trois polices (petite Picopixel, 5x7, grande FreeSansBold), pas de blanc : titres en cyan.
 *   Ancienne version de l'affichage : sauvegarde/main_avant_refonte.cpp
 *   Démarrage : logo animé "LA BONNE M'AIR" (basilique stylisée, voir afficherSplash).
 *   En bas à droite : mini logo Wi-Fi coloré selon le signal + repères secteur / heure / envoi (dessinerTemoins).
 *
 * CHAMPS ENVOYÉS (JSON plat, nombres uniquement) :
 *   standard : device_id, uptime, rssi, heap, co2, pm1, pm25, pm10
 *              (co2 / pm = MOYENNE sur la période de mesure)
 *   perso :
 *     x_age_s          secondes entre la mesure et son envoi
 *                       (0-12 en direct, plus pour une mesure rejouée)
 *     x_co2_max         pic de CO2 sur la période (ppm)
 *     x_pm25_max        pic de PM2.5 sur la période (ug/m3)
 *     x_aeration        1 = chute rapide du CO2 détectée (fenêtre ouverte ?)
 *     x_pct_co2_1000    % du temps au-dessus de 1000 ppm depuis le démarrage
 *     x_reset           cause du dernier redémarrage (esp_reset_reason)
 *     x_alim            1 = secteur, 0 = batterie
 *     x_pm_statut       octet d'état du NextPM
 *     x_temp_pm, x_hum_pm   température / humidité internes du NextPM
 *   => 10 champs x_ exactement (maximum du guide AirCarto).
 *
 * HORS LIGNE : le serveur date chaque mesure à sa RÉCEPTION. Une mesure
 * rejouée plus tard est donc enregistrée à la mauvaise heure ; on corrige à
 * l'analyse : heure réelle = heure de réception - x_age_s.
 *
 * ENVOI : une tâche dédiée (tacheEnvoi) gère le réseau ET le fichier hors ligne, pour que l'écran,
 * les capteurs et la détection secteur ne se figent plus pendant un envoi. loop() se contente de
 * lui déposer les mesures (deposerMesure) : un seul acteur touche au fichier, donc aucun conflit.
 *
 * RYTHME D'ENVOI : le serveur n'accepte qu'UN envoi toutes les 10 s, sans envoi groupé. Le renvoi des
 * mesures stockées est donc limité à ~6 par minute, quoi qu'on fasse sur la carte. Pour s'en approcher :
 *   - l'écart est compté entre les DÉBUTS de deux envois (10 s + une petite marge auto-ajustée sur les 429) ;
 *   - une ligne écartée sans envoi (illisible, heure inconnue) ne consomme pas de créneau ;
 *   - plus de 10 mesures en retard : une mesure fraîche par minute au lieu de 30 s (plus de créneaux pour
 *     le retard) ; "status" donne le temps de rattrapage estimé.
 *   100 mesures en retard : ~20 min (contre ~35 min avant). Plancher absolu imposé par le serveur : ~17 min.
 *
 * VERSIONS : ce programme est écrit pour ESP32 Arduino 2.x. Dans platformio.ini :
 *   platform = espressif32@^6.0.0
 * et fixe les versions de lib_deps (pio pkg list te donne celles qui marchent aujourd'hui).
 */

// VUE D'ENSEMBLE DU PROGRAMME
//
// 1. setup() prépare tout, une seule fois : écran, capteurs, Wi-Fi, mémoire flash, tâche d'envoi.
// 2. loop() tourne ensuite sans arrêt (cœur 1). Elle ne fait aucun travail long : chaque travail se déclenche
//    quand son délai est écoulé (lecture des capteurs toutes les 10 s, une mesure toutes les 30 s, etc.).
// 3. Trois acteurs travaillent en parallèle :
//    - loop()       : capteurs, animations, écran, commandes tapées dans le moniteur série ;
//    - refreshTask  : rafraîchit l'écran LED en continu (cœur 0, réveillée par un timer toutes les 4 ms) ;
//    - tacheEnvoi   : envoie les mesures au serveur ; sans réseau, les range dans un fichier en flash
//                     puis les renvoie quand le réseau revient.
// 4. Le chemin d'une mesure :
//    capteur -> lireCapteurs() -> accumulerLectures() (moyenne + pic) -> finaliserPeriode() (JSON)
//    -> deposerMesure() -> tacheEnvoi -> serveur AirCarto
//                                     \-> (pas de réseau) stockerRecord() -> /backlog.jsonl -> rejouerUneLigne()

// =====================================================================
// INTERRUPTEURS (doivent rester AVANT les #include)
// =====================================================================
#define WIFI_ACTIVE            1  // 0 = hors ligne (test du stockage) | 1 = connexion WiFi
// (plus de WIFI_ENTREPRISE : chaque réseau en mémoire dit lui-même s'il est entreprise ou classique)
#define MODE_DEMO               0  // 1 = fausses valeurs qui défilent, aucun envoi
#define MHZ_ACTIVE               1  // 0 = ignorer le MH-Z19C
#define MHZ_ABC                   1  // 1 = auto-calibration ; 0 si la pièce n'est jamais aérée
#define NEXTPM_MOYENNE_60S        0  // 0 = moyennes 10 s | 1 = moyennes 60 s
#define ECRAN_ETEINT_NUIT         0  // 1 = écran éteint entre HEURE_NUIT_DEBUT et HEURE_NUIT_FIN
#define AFFICHER_TEMOINS          1  // 1 = zone d'état en bas à droite (7x7 px) : logo Wi-Fi coloré selon le signal,
                                     //     et dessous 3 repères : secteur | heure NTP | envoi (voir dessinerTemoins)
#define MASQUER_SECRETS           0  // 1 = les mots de passe saisis s'affichent en ****
#define DOUBLE_BUFFER             1  // 1 = dessin sans scintillement | 0 si l'écran reste noir ou s'affiche mal
#define ANIMATIONS                0  // 1 = clignement, chiffres qui glissent, sommet de courbe qui clignote | 0 = calme
#define TRANSITION_FONDU          1  // 1 = fondu au noir (~150 ms) entre deux pages | 0 = changement sec

// --- Bibliothèques utilisées ---
// WiFi / HTTPClient : connexion et envoi au serveur | LittleFS : fichier en mémoire flash (stockage hors ligne)
// Preferences : mémoire de configuration (mots de passe, clé) | ArduinoJson : fabrication et lecture des mesures en JSON
// ModbusMaster : dialogue avec le NextPM | MHZ19 : capteur de CO2 | PxMatrix : pilotage de l'écran LED
#include <Arduino.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#error "Ce programme est écrit pour ESP32 Arduino 2.x : timerBegin(), esp_task_wdt_init() et esp_wpa2.h changent en 3.x. Dans platformio.ini : platform = espressif32@^6.0.0"
#endif
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <ModbusMaster.h>
#include <MHZ19.h>
#if DOUBLE_BUFFER
#define double_buffer  // à définir AVANT l'include de PxMatrix : active showBuffer()
#endif
#include <PxMatrix.h>
// Polices fournies avec Adafruit GFX (dossier Fonts de la bibliothèque)
#include <Fonts/Picopixel.h>           // petite : 5 px de haut, libellés et unités
#include <Fonts/FreeSansBold9pt7b.h>   // moyenne-grande : "AERER", "OK"
#include <Fonts/FreeSansBold12pt7b.h>  // grande : chiffres de 17 px (valeur du CO2)

#if WIFI_ACTIVE
#include "esp_wpa2.h"
#endif

// =====================================================================
// CONFIGURATION
// =====================================================================
// Les identifiants (Wi-Fi, ENT, clé du groupe) ne sont PAS ici : ils sont
// enregistrés dans la mémoire de la carte par les commandes "wifi ..." et "set cle".
// Adresse du serveur AirCarto qui reçoit les mesures (en http, pas https).
static const char* URL_SERVEUR  = "http://api.aircarto.fr/iut-amu/push";
// Serveur d'heure (NTP) et fuseau horaire de la France : heure d'hiver CET (UTC+1), heure d'été CEST,
// bascule le dernier dimanche de mars et d'octobre.
static const char* NTP_SERVER   = "pool.ntp.org";
static const char* TZ_FRANCE    = "CET-1CEST,M3.5.0,M10.5.0/3";

// --- NextPM ---
#define NEXTPM_RX 39  // l'ESP32 reçoit sur GPIO 39 (fil TX du capteur)
#define NEXTPM_TX 32  // l'ESP32 émet sur GPIO 32 (fil RX du capteur)
// Adresses des registres Modbus du NextPM : l'état du capteur, puis les particules, l'humidité et la température.
#define REG_STATUS 0x13
// Les PM existent en moyenne sur 10 s ou sur 60 s : on choisit les registres correspondants.
#if NEXTPM_MOYENNE_60S
  #define REG_PM1  0x44
  #define REG_PM25 0x46
  #define REG_PM10 0x48
#else
  #define REG_PM1  0x38
  #define REG_PM25 0x3A
  #define REG_PM10 0x3C
#endif
#define REG_HUM  0x6A  // centièmes de %
#define REG_TEMP 0x6B  // centièmes de °C

// Texte associé à chaque bit de l'octet d'état du NextPM (bit 0 = veille ... bit 7 = erreur laser), pour le journal série.
const char* STATUS_BITS[8] = {
  "veille", "mode degrade", "demarrage en cours", "humidite trop forte",
  "erreur capteur T/HR", "erreur ventilateur", "erreur memoire", "erreur laser"
};
#define STATUS_CRITICAL  0xAC  // démarrage, humidité, ventilateur, laser
#define STATUS_DEMARRAGE 0x04

// --- MH-Z19C ---
#define MHZ_RX 36  // l'ESP32 reçoit (fil TX du capteur)
#define MHZ_TX 27  // l'ESP32 émet (fil RX du capteur)

// --- Détection de coupure secteur (pont diviseur sur GPIO 34) ---
static const bool DETECTION_SECTEUR_ACTIVE = true;  // false si le pont n'est pas câblé
#define PIN_SECTEUR 34

// --- Matériel optionnel : -1 = absent (aucun code actif) ---
#define PIN_BOUTON -1  // GPIO libre, bouton vers GND : appui court = page suivante, long = envoi immédiat
#define PIN_BUZZER -1  // GPIO libre, buzzer ACTIF : bip si CO2 >= SEUIL_ALARME_PPM

// --- Luminosité (0..255) ---
// Luminosité de l'écran selon la situation (255 = maximum).
static const uint8_t LUMINOSITE_JOUR     = 255;
static const uint8_t LUMINOSITE_NUIT     = 40;
static const uint8_t LUMINOSITE_BATTERIE = 60;
// Plage de nuit (heures, 0..23) : de DEBUT inclus à FIN exclu. Peut passer minuit (ex. 22 -> 6).
static const int HEURE_NUIT_DEBUT = 0;
static const int HEURE_NUIT_FIN   = 7;

// --- Intervalles (ms) ---
// Rythme de chaque travail : loop() lance un travail quand (maintenant - dernier passage) >= son intervalle.
static const unsigned long INTERVALLE_LECTURE_MS   = 10000;      // lecture des capteurs
static const unsigned long INTERVALLE_PAGE_MS       = 5000;       // durée d'affichage de chaque page (une seule information par page)
static const unsigned long PERIODE_MESURE_MS        = 30000;      // une mesure (moyenne + pic) toutes les 30 s
static const unsigned long PERIODE_MESURE_BATT_MS   = 60000;      // idem sur batterie
static const unsigned long PERIODE_MESURE_RATTRAPAGE_MS = 60000;  // pendant le renvoi d'un gros retard (voir SEUIL_RATTRAPAGE)
static const uint32_t SEUIL_RATTRAPAGE              = 10;         // plus de 10 mesures en retard : 1 mesure / min le temps de rattraper
static const unsigned long ECART_PUSH_MIN_MS        = 10000;      // entre 2 envois (le serveur exige >= 10 s, pas d'envoi groupé)
static const unsigned long MARGE_PUSH_INIT_MS       = 800;        // marge au-dessus des 10 s (absorbe la variation du temps de connexion)
static const unsigned long MARGE_PUSH_MIN_MS        = 400;        // la marge descend jusque-là tant que le serveur ne dit rien
static const unsigned long MARGE_PUSH_MAX_MS        = 5000;       // ... et remonte (+500 ms) à chaque refus 429 "trop rapproché"
static const unsigned long ATTENTE_NTP_REESSAI_MS   = 2000;       // une mesure attend l'heure NTP : on réessaie toutes les 2 s
static const unsigned long INTERVALLE_RESEAU_MS     = 15000;
static const unsigned long INTERVALLE_LUMI_MS        = 60000;
static const unsigned long INTERVALLE_HIST_MS        = MODE_DEMO ? 5000UL : 60000UL;  // un point de la courbe CO2 par minute (5 s en démo)
static const unsigned long CHAUFFE_MHZ_MS            = 180000UL;  // MH-Z19C : environ 3 min pour se stabiliser (fiche technique)
static const unsigned long ANTIREBOND_SECTEUR_MS     = 500;
static const unsigned long WIFI_TIMEOUT_PSK_MS       = 20000UL;   // délai max pour se connecter à un Wi-Fi classique
static const unsigned long WIFI_TIMEOUT_EAP_MS       = 35000UL;   // eduroam est plus lent (authentification ENT)
static const unsigned long WIFI_SCAN_MAX_MS          = 12000UL;   // un scan des réseaux visibles ne dure jamais plus
static const unsigned long WIFI_PAUSE_MS             = 30000UL;   // aucun réseau connu joignable : nouvel essai après 30 s
static const unsigned long WIFI_RESULTAT_MS          = 3000UL;    // durée d'affichage de "CONNECTE" / "ECHEC" à l'écran
static const unsigned long WIFI_REBOOT_MS            = 3600000UL; // 1 h sans Wi-Fi : on redémarre la carte
static const uint32_t HEAP_MIN_OCTETS                = 25000;     // en dessous : redémarrage (fuite mémoire ?)
static const uint32_t WDT_TIMEOUT_S                  = 60;        // watchdog : redémarrage si la carte se fige

// --- Stockage hors ligne ---
// Stockage hors ligne : une mesure JSON par ligne dans FICHIER_BACKLOG.
// FICHIER_TMP sert de copie de travail pendant le compactage (voir compacterBacklog).
static const char* FICHIER_BACKLOG = "/backlog.jsonl";
static const char* FICHIER_TMP     = "/backlog.tmp";
static const size_t TAILLE_MAX_BACKLOG = 500000;   // ~500 Ko (~12 h hors ligne). Le compactage recopie le fichier : il faut
                                                   // de la place pour l'original ET la copie (partition LittleFS : ~1,4 Mo)
// (le compactage ne se fait plus pendant le renvoi : il bloquait les envois plusieurs secondes. Le fichier est
//  supprimé quand tout est renvoyé, et compacté seulement s'il est plein : voir stockerRecord)
static const unsigned long ATTENTE_HEURE_MAX_MS = 900000UL;  // 15 min : au-delà sans heure NTP, une mesure d'un démarrage précédent est abandonnée

// --- Initiatives : paramètres ---
// Zone des témoins : x 57..63, y 25..31. Le bas des pages (thermomètre, VU-mètres, courbe) s'arrête donc à x 55 (x 56 = séparation).
#define LARGEUR_BAS (AFFICHER_TEMOINS ? 56 : 64)
#define HIST_N (LARGEUR_BAS - 3)  // points de la courbe CO2 : minutes terminées (53) + la minute en cours = 54 colonnes
static const float SEUIL_AERER_PPM      = 1000;    // page "AERER" au-dessus (et x_pct_co2_1000)
static const float SEUIL_ALARME_PPM     = 1500;    // buzzer au-dessus
static const float AERATION_CHUTE_PPM   = 100;     // chute de CO2 en ~2 min = aération
static const unsigned long BUZZER_PAUSE_MS = 300000UL;  // un bip toutes les 5 min au plus

// --- Alerte « hors norme » (changement brutal de l'écran) ---
// Niveaux : 0 bon | 1 moyen | 2 dégradé | 3 mauvais | 4 très mauvais | 5 extrêmement mauvais (indice ATMO)
// PM : alerte à partir de « MAUVAIS » (PM2.5 > 25 ug/m3, PM10 > 50 ug/m3 : 50 = valeur limite journalière UE des PM10).
// CO2 : alerte à partir du niveau 2 (>= 1000 ppm = SEUIL_AERER_PPM) : la page AERER s'affiche d'un coup.
static const int NIVEAU_ALERTE_PM  = 3;
static const int NIVEAU_ALERTE_CO2 = 2;
static const unsigned long ALERTE_FLASH_MS    = 150;     // durée d'une image du clignotement plein écran
static const uint8_t       ALERTE_NB_FLASH    = 6;       // 6 images = 3 éclairs (~0,9 s)
static const unsigned long ALERTE_PULSE_MS    = 500;     // la valeur en alerte pulse (vif / atténué)
static const unsigned long ALERTE_REARME_MS   = 60000UL; // un même polluant ne refait pas clignoter l'écran plus d'1 fois / min

// --- Seuils d'affichage ---
// Une échelle = ses seuils + la façon de les comparer :
//   inclus = false : valeur <  seuil[i] => niveau i   (CO2 : seuils "à partir de")
//   inclus = true  : valeur <= seuil[i] => niveau i   (indice ATMO : seuils "jusqu'à", valeur arrondie au 1/10)
struct Echelle { const float* seuils; uint8_t n; bool inclus; };

// CO2 (ppm) : vert < 800 | jaune < 1000 | orange < 1500 | rouge >= 1500
static const float SEUILS_CO2[] = {800, 1000, 1500};
static const Echelle ECH_CO2 = {SEUILS_CO2, 3, false};
// PM2.5 (ug/m3), indice ATMO (arrêté du 10 juillet 2020, https://www.atmo-france.org/article/lindice-atmo) :
//   bon <= 10 | moyen <= 20 | dégradé <= 25 | mauvais <= 50 | très mauvais <= 75 | extrêmement mauvais > 75
// Aussi utilisé pour PM1 (pas de seuils réglementaires). L'ATMO porte sur des moyennes JOURNALIÈRES :
// ici on l'applique à la valeur instantanée, à titre indicatif.
static const float SEUILS_PM25[] = {10, 20, 25, 50, 75};
static const Echelle ECH_PM25 = {SEUILS_PM25, 5, true};
// PM10 (ug/m3), indice ATMO :
//   bon <= 20 | moyen <= 40 | dégradé <= 50 | mauvais <= 100 | très mauvais <= 150 | extrêmement mauvais > 150
static const float SEUILS_PM10[] = {20, 40, 50, 100, 150};
static const Echelle ECH_PM10 = {SEUILS_PM10, 5, true};

// Niveaux : 0 vert | 1 jaune | 2 orange | 3 rouge | 4 pourpre | 5 magenta (le CO2 s'arrête au rouge)
// (sur un panneau LED, un "jaune" trop orangé (255,170,0) paraît orange : on met plus de vert ;
//  le pourpre est éclairci, sinon il se confond avec le rouge)
#define NB_NIVEAUX 6
static const uint8_t PALETTE[NB_NIVEAUX][3] = {
  {0, 200, 0}, {255, 220, 0}, {255, 100, 0}, {255, 0, 0}, {255, 0, 120}, {170, 0, 255}
};
// Qualificatifs ATMO. Page "QUALITE AIR" : 7 caractères par ligne au plus ; un libellé plus long est coupé
// en deux lignes à l'espace ("TRES" / "MAUVAIS").
static const char* LABELS_NIVEAU[NB_NIVEAUX] = {"BON", "MOYEN", "DEGRADE", "MAUVAIS", "TRES MAUVAIS", "EXTREM. MAUVAIS"};
// Versions courtes, utilisées quand le libellé complet ne tient pas (bas des pages PM1 / PM2.5 / PM10)
static const char* LABELS_COURTS[NB_NIVEAUX] = {"BON", "MOYEN", "DEGRADE", "MAUVAIS", "TRES MAUV.", "EXTREME"};

// --- Animations d'écran ---
static const unsigned long ANIM_PAS_MS = 40;           // une image d'animation toutes les 40 ms (25 i/s)

// =====================================================================
// ÉCRAN
// =====================================================================
// Broches de la carte ModuleAir vers le connecteur HUB75 de l'écran
#define P_LAT 25
#define P_A   17
#define P_B   33
#define P_C   4
#define P_D   12
#define P_E   15
#define P_OE  16

// L'objet écran : 64 colonnes x 32 lignes, puis les broches de contrôle (latch, sortie OE, sélection de lignes A à E).
// Tout le dessin passe par lui : display.drawPixel, fillRect, print...
PxMATRIX display(64, 32, P_LAT, P_OE, P_A, P_B, P_C, P_D, P_E);

// L'écran n'allume que 2 lignes sur 32 à la fois : il faut le redessiner sans
// arrêt, toutes les 4 ms. Un timer réveille une tâche qui s'en charge, en
// parallèle du reste du programme. Ce bloc se recopie tel quel.
hw_timer_t* timer = NULL;
SemaphoreHandle_t refreshSem;

void IRAM_ATTR onTimer() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(refreshSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void refreshTask(void* param) {
  for (;;) {
    if (xSemaphoreTake(refreshSem, portMAX_DELAY)) {
      display.display(30);  // durée d'allumage de chaque ligne, en µs
    }
  }
}

void setupDisplay() {
  display.begin(16);  // balayage 1/16
  display.setDriverChip(SHIFT);
  display.setColorOrder(RRBBGG);  // couleurs inversées ? essayez RRGGBB
  display.setBrightness(255);     // luminosité : 0 à 255, ici le maximum
  display.clearDisplay();
  // Le timer « donne » ce sémaphore toutes les 4 ms ; refreshTask l'attend, se réveille et rafraîchit l'écran.
  refreshSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(refreshTask, "refresh", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0);  // priorité max, cœur 0
  timer = timerBegin(0, 80, true);        // 80 MHz / 80 : un tic par µs
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 4000, true);     // toutes les 4000 µs, en boucle
  timerAlarmEnable(timer);
}

// =====================================================================
// TYPES ET VARIABLES GLOBALES
// =====================================================================
HardwareSerial nextpmSerial(1);  // UART n°1
ModbusMaster nextpm;
MHZ19 mhz;
Preferences prefs;  // mémoire de configuration (espace "aircarto")

// Affichage série automatique : false = la carte ne parle plus toute seule
// (commande "verbose on" / "verbose off", enregistrée en mémoire)
bool verbose = false;
#define LOG(...)  do { if (verbose) Serial.printf(__VA_ARGS__); } while (0)

enum EtatCapteur { CAPTEUR_CHAUFFE, CAPTEUR_OK, CAPTEUR_ERREUR };

struct Mesure {
  float valeur;
  EtatCapteur etat;
  uint8_t echecs;
  bool aJour;     // true = la valeur vient de la dernière lecture réussie
  float h[3];     // 3 dernières lectures brutes (filtre médian)
  uint8_t nh;
  float aff;      // valeur AFFICHÉE : glisse en douceur vers "valeur"
  bool affInit;   // aff a-t-elle été initialisée ?
};

// Une Mesure par grandeur (CO2, PM1, PM2.5, PM10) : valeur filtrée, état du capteur, valeur affichée qui glisse...
Mesure mCO2  = {0, CAPTEUR_CHAUFFE, 0, false, {0, 0, 0}, 0};
Mesure mPM1  = {0, CAPTEUR_CHAUFFE, 0, false, {0, 0, 0}, 0};
Mesure mPM25 = {0, CAPTEUR_CHAUFFE, 0, false, {0, 0, 0}, 0};
Mesure mPM10 = {0, CAPTEUR_CHAUFFE, 0, false, {0, 0, 0}, 0};

float tempPM = NAN;   // température interne du NextPM
float humPM  = NAN;   // humidité interne du NextPM
uint8_t statutPM = 0;

// Agrégats sur la période de mesure (moyenne + pic)
struct Agregat { float somme; uint16_t n; float maxi; };
Agregat agCO2  = {0, 0, 0};
Agregat agPM1  = {0, 0, 0};
Agregat agPM25 = {0, 0, 0};
Agregat agPM10 = {0, 0, 0};

// Statistiques CO2
uint32_t nCO2Total = 0, nCO2Haut = 0;  // pour x_pct_co2_1000
float tamponCO2[12];                   // ~2 min de lectures (détection d'aération)
uint8_t tamponIdx = 0, tamponN = 0;
bool aerationPeriode = false;
uint16_t histCO2[HIST_N];              // courbe : moyenne du CO2 de chacune des dernières minutes (0 = minute sans mesure)
uint8_t histIdx = 0, histN = 0;
float histSomme = 0;                   // somme des lectures CO2 de la minute en cours
uint16_t histNb = 0;                   // nombre de lectures correspondantes

// Configuration (chargée depuis la mémoire de la carte)
String cfgCle;
String deviceId;      // adresse MAC, lue une fois au démarrage
uint32_t bootId = 0;  // identifiant aléatoire de ce démarrage

// Alimentation
bool estSurSecteur = true;
bool etatCandidat = true;
unsigned long tChangementSecteur = 0;

// Réseau et envoi
// REJETE : le serveur a refusé la mesure (400/413) | INVALIDE : écartée sur la carte, SANS envoi (ligne illisible,
// heure inconnue) : ne consomme pas le créneau de 10 s | ATTENDRE : l'heure NTP manque encore
enum ResultatPush { PUSH_OK, PUSH_REESSAYER, PUSH_REJETE, PUSH_INVALIDE, PUSH_CLE_INVALIDE, PUSH_ATTENDRE };
// Mesures fraîches en attente d'envoi (RAM) : loop() les dépose, la tâche d'envoi les prend.
// Protégées par mutexEnvoi. Au-delà de NB_PENDANTES, la plus ancienne est sacrifiée.
#define NB_PENDANTES 4
String pendantes[NB_PENDANTES];
uint8_t nbPendantes = 0;
SemaphoreHandle_t mutexEnvoi = NULL;
volatile bool demandeVider = false;  // commande "vider_file" : la tâche d'envoi s'en charge
unsigned long tAttenteHeure = 0;     // depuis quand une mesure attend l'heure NTP (0 = personne n'attend)
unsigned long tEssaiNtp = 0;         // dernier essai d'une mesure qui attend l'heure NTP
unsigned long tDernierPush = 0;      // DÉBUT du dernier POST (c'est ce que le serveur compte)
bool dejaPoste = false;              // aucun POST depuis le démarrage : le premier part tout de suite
unsigned long ecartMinMs = ECART_PUSH_MIN_MS;   // délai demandé par le serveur (next_push_in_s, 10 s au moins)
unsigned long margePushMs = MARGE_PUSH_INIT_MS; // marge ajoutée, ajustée selon les refus 429
uint8_t succesDeSuite = 0;           // envois réussis consécutifs (pour réduire la marge)
uint8_t echecsPush = 0;              // échecs réseau / serveur consécutifs (espacement progressif)
bool cleInvalide = false;  // le serveur a refusé la clé (401)
uint8_t etatEnvoi = 0;     // 0 gris | 1 vert (OK) | 2 orange (réessai) | 3 rouge (refus)
int dernierCodeHttp = 0;
unsigned long tDernierWifiOK = 0;
uint32_t nbCoupuresWifi = 0;

// Cache des réseaux Wi-Fi connus (enregistré en mémoire permanente, clés "w_...")
#define NB_WIFI_MAX 5
struct ReseauWifi {
  String ssid;
  String mdp;     // mot de passe du Wi-Fi (classique) ou mot de passe ENT (entreprise) ; vide = réseau ouvert
  String eapId;   // vide = Wi-Fi classique | rempli = WPA2 Entreprise (eduroam), identifiant ENT
};
ReseauWifi reseaux[NB_WIFI_MAX];
uint8_t nbReseaux = 0;
int8_t dernierReseau = -1;  // dernier réseau qui a fonctionné : essayé en premier à force égale

// Machine à états de la connexion (gererWiFi, appelée à chaque tour de loop, jamais bloquante)
enum EtatWifi { WIFI_INACTIF, WIFI_SCAN, WIFI_CONNEXION, WIFI_CONNECTE, WIFI_PAUSE };
EtatWifi etatWifi = WIFI_INACTIF;
int8_t candidats[NB_WIFI_MAX];      // réseaux à essayer, dans l'ordre (le plus fort d'abord)
uint8_t nbCandidats = 0, idxCandidat = 0;
int8_t reseauCourant = -1;          // réseau en cours d'essai ou connecté
bool rescannerApresEchec = false;   // après une coupure : on retente le même réseau, puis on rescanne
unsigned long tEtatWifi = 0;        // début de l'état courant
volatile uint8_t raisonDeco = 0;    // raison de la dernière déconnexion (donnée par le pilote Wi-Fi)

// Retour à l'écran
bool wifiInteractif = true;         // true : l'écran montre la page de connexion (démarrage, commande "wifi")
bool resultatWifiAffiche = false;   // "CONNECTE" / "ECHEC" à l'écran pendant WIFI_RESULTAT_MS
bool resultatWifiOK = false;
unsigned long tResultatWifi = 0;

// Stockage hors ligne
bool fsOK = false;
size_t offsetRejeu = 0;       // position de la prochaine ligne à renvoyer
size_t tailleBacklog = 0;     // taille du fichier (lignes déjà renvoyées comprises)
volatile uint32_t nbLignesAttente = 0;  // écrit par la tâche d'envoi, lu par loop() (période de rattrapage)

// Affichage
// Pages de l'écran, affichées en rotation (une toutes les INTERVALLE_PAGE_MS). NB_PAGES = nombre de pages.
// PAGE_AERER n'apparaît que si le CO2 est élevé, PAGE_COURBE qu'après 2 minutes de mesures.
enum Page { PAGE_CO2, PAGE_AERER, PAGE_PM1, PAGE_PM25, PAGE_PM10, PAGE_GLOBAL, PAGE_COURBE, PAGE_WIFI, NB_PAGES };
uint8_t pageCourante = PAGE_CO2;
bool aRedessiner = false;  // un seul redessin par tour de loop()
bool aTransition = false;  // fondu à faire au prochain affichage (changement de page)
bool ecranEteint = false;
uint8_t luminositeCourante = 255;  // luminosité choisie par majLuminosite (le fondu y revient)
unsigned long tAnim = 0;       // dernière image d'animation
bool etatCligne = false;       // les yeux sont-ils fermés (dernière image) ?
uint32_t phaseChauffe = 0;     // seconde affichée par le compte à rebours de préchauffage
uint8_t phasePoint = 0;          // point clignotant de la page courbe (0 / 1, change toutes les 500 ms)

// Alertes hors norme : une entrée par polluant (0 CO2 | 1 PM1 | 2 PM2.5 | 3 PM10)
struct EtatAlerte {
  bool active;            // le polluant est au-dessus de son seuil d'alerte
  int8_t nivMax;          // pire niveau atteint depuis le début de l'alerte (re-clignote s'il empire)
  unsigned long tFlash;   // dernier clignotement déclenché par ce polluant
};
EtatAlerte alertes[4] = {{false, -1, 0}, {false, -1, 0}, {false, -1, 0}, {false, -1, 0}};
unsigned long tFlashDebut = 0;   // début du clignotement plein écran (0 = aucun en cours)
uint16_t couleurFlash = 0;       // couleur du niveau qui a déclenché l'alerte
const char* nomFlash = "";       // nom du polluant écrit en noir pendant l'éclair

// Dates (en ms depuis le démarrage) du dernier passage de chaque travail de loop().
unsigned long tLecture = 0, tPeriode = 0, tReseau = 0, tLumi = 0, tPage = 0, tHist = 0;
unsigned long tDernierBip = 0;
String ligneSerie;
uint8_t etatEsc = 0;  // 0 normal | 1 après ESC | 2 dans une séquence ESC [ ... (flèches, Suppr...)

// =====================================================================
// UTILITAIRES
// =====================================================================
bool heureValide() {
  return time(nullptr) > 1700000000;  // horloge réglée par NTP
}

// Vrai si le Wi-Fi est activé dans le programme ET réellement connecté.
bool reseauOK() {
  return WIFI_ACTIVE && WiFi.status() == WL_CONNECTED;
}

// Arrondit à 1 décimale (12.34 -> 12.3) pour garder des messages JSON courts.
float arrondi1(float v) {
  return roundf(v * 10.0f) / 10.0f;
}

// Médiane de 3 valeurs : écarte une lecture aberrante isolée
float mediane3(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// Un seul champ hors bornes fait refuser TOUTE la requête (400) :
// on vérifie donc chaque valeur avant de l'ajouter au JSON.
bool dansBornes(const char* nom, float v, float vmin, float vmax) {
  if (!isnan(v) && v >= vmin && v <= vmax) return true;
  LOG("Champ %s ignoré (valeur %.1f hors bornes)\n", nom, v);
  return false;
}

// Adresse MAC lue directement dans la puce : marche même sans Wi-Fi démarré
String lireMac() {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Nuit = de HEURE_NUIT_DEBUT (inclus) à HEURE_NUIT_FIN (exclu), en heure locale.
// Fonctionne aussi si la plage passe minuit (ex. 22 -> 6).
bool estNuit() {
  if (!heureValide()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour;
  if (HEURE_NUIT_DEBUT <= HEURE_NUIT_FIN) return (h >= HEURE_NUIT_DEBUT && h < HEURE_NUIT_FIN);
  return (h >= HEURE_NUIT_DEBUT || h < HEURE_NUIT_FIN);
}

// Durée d'une période de mesure : plus longue sur batterie (énergie), et pendant le renvoi d'un gros retard :
// le serveur n'accepte qu'un envoi toutes les 10 s, donc chaque mesure fraîche en moins laisse un créneau au retard.
unsigned long periodeMesureMs() {
  if (!estSurSecteur) return PERIODE_MESURE_BATT_MS;
  if (nbLignesAttente > SEUIL_RATTRAPAGE && reseauOK()) return PERIODE_MESURE_RATTRAPAGE_MS;
  return PERIODE_MESURE_MS;
}

// =====================================================================
// CONFIGURATION (mémoire de la carte)
// =====================================================================
// Relit la configuration enregistrée dans la mémoire permanente de la carte (NVS) par les commandes « set ... ».
// S'il n'y a rien, on prend la valeur par défaut.
void chargerReseaux();
void chargerConfig() {
  prefs.begin("aircarto", false);
  chargerReseaux();
  cfgCle     = prefs.getString("cle", "");
  offsetRejeu = prefs.getULong("off", 0);
  verbose    = prefs.getBool("verbose", false);
}

// La clé du groupe AirCarto doit faire exactement 32 caractères.
bool cleOK() {
  return cfgCle.length() == 32;
}

// La clé est lue par la tâche d'envoi pendant que le moniteur série peut la modifier : accès protégé
String lireCle() {
  String c;
  if (mutexEnvoi) xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  c = cfgCle;
  if (mutexEnvoi) xSemaphoreGive(mutexEnvoi);
  return c;
}

void definirCle(const String &v) {
  if (mutexEnvoi) xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  cfgCle = v;
  if (mutexEnvoi) xSemaphoreGive(mutexEnvoi);
}

// =====================================================================
// RÉSEAU : CACHE DE PLUSIEURS WI-FI + CONNEXION AUTOMATIQUE
// =====================================================================
// Jusqu'à NB_WIFI_MAX réseaux sont gardés en mémoire permanente. Le type est propre à chaque réseau :
// eapId rempli = WPA2 Entreprise (eduroam), eapId vide = Wi-Fi classique (ou ouvert si pas de mot de passe).
// Déroulement (gererWiFi, jamais bloquant) :
//   SCAN -> on garde les réseaux connus visibles, triés du plus fort au plus faible
//   CONNEXION -> on les essaie l'un après l'autre (délai max par réseau)
//   CONNECTE -> si la liaison tombe, on retente le même réseau, puis on rescanne
//   PAUSE -> aucun réseau connu joignable : nouvel essai 30 s plus tard

// Clés de la mémoire permanente : "w_s0", "w_p0", "w_i0"... (15 caractères max)
static void cleReseau(char* buf, size_t n, char type, uint8_t i) {
  snprintf(buf, n, "w_%c%u", type, (unsigned)i);
}

void sauverReseaux() {
  char k[8];
  prefs.putUChar("w_n", nbReseaux);
  for (uint8_t i = 0; i < NB_WIFI_MAX; i++) {
    if (i < nbReseaux) {
      cleReseau(k, sizeof(k), 's', i); prefs.putString(k, reseaux[i].ssid);
      cleReseau(k, sizeof(k), 'p', i); prefs.putString(k, reseaux[i].mdp);
      cleReseau(k, sizeof(k), 'i', i); prefs.putString(k, reseaux[i].eapId);
    } else {
      cleReseau(k, sizeof(k), 's', i); prefs.remove(k);
      cleReseau(k, sizeof(k), 'p', i); prefs.remove(k);
      cleReseau(k, sizeof(k), 'i', i); prefs.remove(k);
    }
  }
  prefs.putChar("w_last", dernierReseau);
}

// Ajoute (ou met à jour, si le nom existe déjà) un réseau. Renvoie son numéro, ou -1 si le cache est plein.
int ajouterReseau(const String &ssid, const String &mdp, const String &eapId) {
  for (uint8_t i = 0; i < nbReseaux; i++) {
    if (reseaux[i].ssid == ssid) {
      reseaux[i].mdp = mdp;
      reseaux[i].eapId = eapId;
      sauverReseaux();
      return i;
    }
  }
  if (nbReseaux >= NB_WIFI_MAX) return -1;
  reseaux[nbReseaux].ssid = ssid;
  reseaux[nbReseaux].mdp = mdp;
  reseaux[nbReseaux].eapId = eapId;
  nbReseaux++;
  sauverReseaux();
  return nbReseaux - 1;
}

// Relit le cache. Au premier démarrage avec ce programme, reprend les anciens réglages
// (set ssid / wifi_pwd / eap_id / eap_pwd) pour ne rien avoir à retaper.
void chargerReseaux() {
  char k[8];
  uint8_t n = prefs.getUChar("w_n", 255);
  if (n == 255) {  // pas encore de cache : migration des anciennes clés
    nbReseaux = 0;
    dernierReseau = -1;
    String s = prefs.getString("ssid", ""), p = prefs.getString("wifi_pwd", "");
    String id = prefs.getString("eap_id", ""), e = prefs.getString("eap_pwd", "");
    if (id.length() > 0 && e.length() > 0) ajouterReseau(s.length() ? s : String("eduroam"), e, id);
    else if (s.length() > 0 && s != "eduroam") ajouterReseau(s, p, "");
    prefs.remove("ssid"); prefs.remove("wifi_pwd"); prefs.remove("eap_id"); prefs.remove("eap_pwd");
    sauverReseaux();
    if (nbReseaux > 0) Serial.printf("Ancien réglage Wi-Fi repris dans le cache : %s\n", reseaux[0].ssid.c_str());
    return;
  }
  nbReseaux = (n > NB_WIFI_MAX) ? NB_WIFI_MAX : n;
  for (uint8_t i = 0; i < nbReseaux; i++) {
    cleReseau(k, sizeof(k), 's', i); reseaux[i].ssid  = prefs.getString(k, "");
    cleReseau(k, sizeof(k), 'p', i); reseaux[i].mdp   = prefs.getString(k, "");
    cleReseau(k, sizeof(k), 'i', i); reseaux[i].eapId = prefs.getString(k, "");
  }
  dernierReseau = prefs.getChar("w_last", -1);
  if (dernierReseau >= (int8_t)nbReseaux) dernierReseau = -1;
}

bool estEntreprise(int8_t i) {
  return i >= 0 && i < (int8_t)nbReseaux && reseaux[i].eapId.length() > 0;
}

const char* typeReseau(int8_t i) {
  if (i < 0 || i >= (int8_t)nbReseaux) return "?";
  if (estEntreprise(i)) return "ENT";
  return reseaux[i].mdp.length() ? "WPA" : "OUVERT";
}

// Le pilote Wi-Fi donne la raison de chaque déconnexion : on la garde pour le diagnostic
void surDeconnexion(WiFiEvent_t ev, WiFiEventInfo_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r != WIFI_REASON_ASSOC_LEAVE) raisonDeco = r;  // ASSOC_LEAVE = c'est nous qui avons coupé
}

// Texte court (écran, 6 caractères max) et explication (moniteur série) de la raison d'un échec
const char* raisonCourte(uint8_t r) {
  switch (r) {
    case 0:                                  return "DELAI";
    case WIFI_REASON_NO_AP_FOUND:            return "ABSENT";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "MDP ?";
    default:                                 return "REFUS";
  }
}

const char* raisonLongue(uint8_t r) {
  switch (r) {
    case 0:                                  return "pas de réponse dans le délai";
    case WIFI_REASON_NO_AP_FOUND:            return "réseau introuvable (hors de portée ? nom exact ?)";
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "identifiant ou mot de passe ENT refusé";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "mot de passe refusé (ou mauvais type : wifi add / wifi ent ?)";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:        return "le point d'accès refuse la connexion";
    default:                                 return "refusé par le réseau";
  }
}

unsigned long delaiConnexion(int8_t i) {
  return estEntreprise(i) ? WIFI_TIMEOUT_EAP_MS : WIFI_TIMEOUT_PSK_MS;
}

// Essaie de se connecter au réseau i (la réponse arrive plus tard : voir gererWiFi)
void lancerConnexion(int8_t i) {
#if WIFI_ACTIVE
  const ReseauWifi &r = reseaux[i];
  reseauCourant = i;
  WiFi.disconnect(false, false);
  delay(50);
  raisonDeco = 0;
  if (r.eapId.length() > 0) {
    // WPA2 Entreprise (eduroam) : identité + identifiant + mot de passe ENT, puis on active le mode entreprise
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)r.eapId.c_str(), r.eapId.length());
    esp_wifi_sta_wpa2_ent_set_username((uint8_t *)r.eapId.c_str(), r.eapId.length());
    esp_wifi_sta_wpa2_ent_set_password((uint8_t *)r.mdp.c_str(), r.mdp.length());
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(r.ssid.c_str());
  } else {
    // Wi-Fi classique : le mode entreprise DOIT être coupé, sinon la box refuse la connexion
    esp_wifi_sta_wpa2_ent_disable();
    WiFi.begin(r.ssid.c_str(), r.mdp.length() ? r.mdp.c_str() : nullptr);
  }
  etatWifi = WIFI_CONNEXION;
  tEtatWifi = millis();
  aRedessiner = true;
  if (wifiInteractif || verbose)
    Serial.printf("Wi-Fi : connexion à %s (%s, essai %u/%u)...\n", r.ssid.c_str(), typeReseau(i),
                  (unsigned)(idxCandidat + 1), (unsigned)nbCandidats);
#endif
}

// Lance une recherche des réseaux visibles (asynchrone : la carte continue de tourner)
void lancerScan() {
#if WIFI_ACTIVE
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
  etatWifi = WIFI_SCAN;
  tEtatWifi = millis();
  aRedessiner = true;
#endif
}

// Range les réseaux connus visibles du plus fort au plus faible. n < 0 : le scan a échoué.
void preparerCandidats(int n) {
  nbCandidats = 0;
  idxCandidat = 0;
  rescannerApresEchec = false;
  if (n < 0) {  // scan raté : on essaie quand même tout, le dernier réseau qui a marché d'abord
    if (dernierReseau >= 0) candidats[nbCandidats++] = dernierReseau;
    for (uint8_t i = 0; i < nbReseaux; i++) if ((int8_t)i != dernierReseau) candidats[nbCandidats++] = i;
    return;
  }
  int32_t force[NB_WIFI_MAX];
  for (uint8_t i = 0; i < nbReseaux; i++) {
    int32_t meilleur = -1000;
    for (int j = 0; j < n; j++) {
      if (WiFi.SSID(j) == reseaux[i].ssid && WiFi.RSSI(j) > meilleur) meilleur = WiFi.RSSI(j);
    }
    if (meilleur <= -1000) continue;
    if ((int8_t)i == dernierReseau) meilleur += 5;  // petit bonus : à force égale, on garde ses habitudes
    // insertion triée (du plus fort au plus faible)
    uint8_t pos = nbCandidats;
    while (pos > 0 && force[pos - 1] < meilleur) {
      force[pos] = force[pos - 1];
      candidats[pos] = candidats[pos - 1];
      pos--;
    }
    force[pos] = meilleur;
    candidats[pos] = i;
    nbCandidats++;
  }
  // Rien de visible : peut-être un réseau caché, on tente le dernier réseau qui a fonctionné
  if (nbCandidats == 0 && dernierReseau >= 0) candidats[nbCandidats++] = dernierReseau;
  if (wifiInteractif || verbose)
    Serial.printf("Wi-Fi : %d réseaux visibles, %u connus parmi eux.\n", n, (unsigned)nbCandidats);
  WiFi.scanDelete();
}

void montrerResultatWifi(bool ok) {
  if (!wifiInteractif && !ok) return;  // reconnexions en arrière-plan : on ne cache pas les mesures pour un échec
  resultatWifiAffiche = true;
  resultatWifiOK = ok;
  tResultatWifi = millis();
  wifiInteractif = false;
  aRedessiner = true;
}

void connexionReussie(unsigned long now) {
  etatWifi = WIFI_CONNECTE;
  tDernierWifiOK = now;
  if (dernierReseau != reseauCourant) {
    dernierReseau = reseauCourant;
    prefs.putChar("w_last", dernierReseau);
  }
  configTzTime(TZ_FRANCE, NTP_SERVER);  // heure : indispensable pour dater les mesures stockées hors ligne
  Serial.printf("Wi-Fi connecté à %s | IP %s | %d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  montrerResultatWifi(true);
}

// Tous les candidats ont échoué
void finTentatives(unsigned long now) {
  if (rescannerApresEchec) { lancerScan(); return; }  // coupure : le même réseau ne répond plus, on cherche ailleurs
  WiFi.disconnect(false, false);
  if (wifiInteractif || verbose)
    Serial.printf("Wi-Fi : aucun réseau connu joignable. Nouvel essai dans %lu s ('wifi scan' pour voir les réseaux).\n",
                  WIFI_PAUSE_MS / 1000UL);
  montrerResultatWifi(false);
  wifiInteractif = false;
  etatWifi = WIFI_PAUSE;
  tEtatWifi = now;
  aRedessiner = true;
}

// Relance tout (démarrage, réseau ajouté / supprimé, commande "wifi connect")
void relancerWifi(bool interactif) {
#if WIFI_ACTIVE
  resultatWifiAffiche = false;
  tDernierWifiOK = millis();  // le compte à rebours d'1 h repart
  if (nbReseaux == 0) {
    WiFi.disconnect(false, false);
    etatWifi = WIFI_INACTIF;
    reseauCourant = -1;
    aRedessiner = true;
    return;
  }
  wifiInteractif = interactif;
  lancerScan();
#endif
}

// Préparation du Wi-Fi, une fois au démarrage
void initialiserWiFi() {
#if WIFI_ACTIVE
  WiFi.persistent(false);              // les identifiants sont dans NOTRE cache : la carte n'en garde pas une 2e copie
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);        // efface un ancien réseau mémorisé par le pilote (anciennes versions du programme)
  WiFi.setAutoReconnect(false);        // la reconnexion est gérée par gererWiFi (sinon les deux se marchent dessus)
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);         // accepte aussi les vieilles box en WPA (pas seulement WPA2)
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);      // eduroam a plein de bornes : on prend la plus forte
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent(surDeconnexion, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (nbReseaux == 0) {
    Serial.println("Aucun Wi-Fi en mémoire : tape 'wifi add <nom> <mdp>' ou 'wifi ent <nom> <identifiant> <mdp>'.");
  }
  relancerWifi(true);
#else
  Serial.println("WiFi désactivé (WIFI_ACTIVE 0) : mode hors ligne.");
#endif
}

// Machine à états de la connexion : appelée à chaque tour de loop(), ne bloque jamais
void gererWiFi(unsigned long now) {
#if WIFI_ACTIVE
  // fin de l'affichage "CONNECTE" / "ECHEC"
  if (resultatWifiAffiche && now - tResultatWifi >= WIFI_RESULTAT_MS) {
    resultatWifiAffiche = false;
    aRedessiner = true;
  }

  switch (etatWifi) {
    case WIFI_INACTIF:
      return;

    case WIFI_SCAN: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && now - tEtatWifi < WIFI_SCAN_MAX_MS) return;
      if (n == WIFI_SCAN_RUNNING) n = -1;  // scan bloqué : on fait sans
      preparerCandidats(n);
      if (nbCandidats == 0) { finTentatives(now); return; }
      lancerConnexion(candidats[0]);
      return;
    }

    case WIFI_CONNEXION: {
      if (WiFi.status() == WL_CONNECTED) { connexionReussie(now); return; }
      // Mauvais mot de passe / réseau absent : inutile d'attendre la fin du délai
      uint8_t r = raisonDeco;
      bool refusNet = (r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_AUTH_FAIL ||
                       r == WIFI_REASON_802_1X_AUTH_FAILED || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
      if (!refusNet && now - tEtatWifi < delaiConnexion(reseauCourant)) return;
      if (wifiInteractif || verbose)
        Serial.printf("Wi-Fi : échec sur %s : %s\n", reseaux[reseauCourant].ssid.c_str(), raisonLongue(r));
      if (++idxCandidat < nbCandidats) { lancerConnexion(candidats[idxCandidat]); return; }
      finTentatives(now);
      return;
    }

    case WIFI_CONNECTE:
      if (WiFi.status() == WL_CONNECTED) { tDernierWifiOK = now; return; }
      nbCoupuresWifi++;
      Serial.printf("Wi-Fi perdu (%s) : reconnexion...\n", raisonLongue(raisonDeco));
      // on retente d'abord le même réseau ; s'il ne revient pas, on rescanne
      nbCandidats = 1;
      idxCandidat = 0;
      candidats[0] = reseauCourant;
      lancerConnexion(reseauCourant);
      rescannerApresEchec = true;
      return;

    case WIFI_PAUSE:
      if (now - tEtatWifi >= WIFI_PAUSE_MS) lancerScan();
      return;
  }
#endif
}

// Toutes les 15 s : 1 h sans Wi-Fi (avec des réseaux en mémoire) = dernier recours, on redémarre la carte.
void surveillerWifiLongue(unsigned long now) {
#if WIFI_ACTIVE
  if (nbReseaux == 0 || etatWifi == WIFI_CONNECTE) return;
  if (now - tDernierWifiOK > WIFI_REBOOT_MS) {
    Serial.println("Wi-Fi absent depuis 1 h : redémarrage de la carte.");
    delay(100);
    ESP.restart();
  }
#endif
}

// Une mémoire qui fond en continu (fuite) finit par planter : mieux vaut redémarrer
void verifierMemoire() {
  if (ESP.getFreeHeap() < HEAP_MIN_OCTETS) {
    Serial.printf("Mémoire libre trop basse (%u octets) : redémarrage.\n", (unsigned)ESP.getFreeHeap());
    delay(100);
    ESP.restart();
  }
}

// =====================================================================
// CAPTEURS
// =====================================================================
// Met à jour une Mesure après une tentative de lecture.
// Lecture réussie : filtre médian sur les 3 dernières valeurs (écarte une valeur aberrante isolée).
// Lecture ratée : la valeur n'est plus considérée à jour, et après 3 échecs de suite le capteur passe en erreur.
void majMesure(Mesure &m, bool lectureOk, float valeur) {
  if (lectureOk) {
    // filtre médian sur les 3 dernières lectures brutes
    m.h[0] = m.h[1]; m.h[1] = m.h[2]; m.h[2] = valeur;
    if (m.nh < 3) m.nh++;
    m.valeur = (m.nh >= 3) ? mediane3(m.h[0], m.h[1], m.h[2]) : valeur;
    m.etat = CAPTEUR_OK;
    m.echecs = 0;
    m.aJour = true;
  } else {
    m.aJour = false;  // l'ancienne valeur ne sera plus envoyée
    if (++m.echecs >= 3) m.etat = CAPTEUR_ERREUR;  // tolère 2 échecs consécutifs
  }
}

// Impose un état au capteur (préchauffage / OK / erreur) et remet son filtre à zéro.
void forcerEtat(Mesure &m, EtatCapteur etat) {
  m.etat = etat;
  m.echecs = 0;
  m.nh = 0;
  m.aJour = (etat == CAPTEUR_OK);
  if (etat != CAPTEUR_OK) m.affInit = false;  // à la reprise, l'affichage repart de la vraie valeur
}

// Une valeur PM = 2 registres de 16 bits (poids faible d'abord), en millièmes de ug/m3.
// i = position de la valeur dans la réponse (0 pour le 1er couple de registres, 1 pour le 2e...).
float pmDepuisReponse(uint8_t i) {
  uint32_t raw = ((uint32_t)nextpm.getResponseBuffer(2 * i + 1) << 16) | nextpm.getResponseBuffer(2 * i);
  return raw / 1000.0f;
}

// Renvoie -1 si le capteur n'a pas répondu (exemple des profs).
float lirePM(uint16_t reg) {
  if (nextpm.readHoldingRegisters(reg, 2) != nextpm.ku8MBSuccess) return -1;
  return pmDepuisReponse(0);
}

// PM1, PM2.5, PM10 se suivent (6 registres) : une seule requête Modbus au lieu de trois.
// Si le capteur refuse la lecture groupée, on revient définitivement à la lecture une par une.
bool lectureGroupeePM = true;
bool lireTroisPM(float &pm1, float &pm25, float &pm10) {
  if (lectureGroupeePM) {
    if (nextpm.readHoldingRegisters(REG_PM1, 6) == nextpm.ku8MBSuccess) {
      pm1 = pmDepuisReponse(0); pm25 = pmDepuisReponse(1); pm10 = pmDepuisReponse(2);
      return true;
    }
    lectureGroupeePM = false;
    LOG("NextPM : lecture groupée refusée, lecture registre par registre.\n");
  }
  pm1 = lirePM(REG_PM1); pm25 = lirePM(REG_PM25); pm10 = lirePM(REG_PM10);
  return pm1 >= 0 && pm25 >= 0 && pm10 >= 0;
}

// Lit le NextPM (particules fines) en Modbus : d'abord son état, puis PM1 / PM2.5 / PM10,
// puis la température et l'humidité internes.
void lireNextPM() {
  // 1) l'état d'abord : dit si le capteur répond et si la mesure est fiable
  uint8_t result = nextpm.readHoldingRegisters(REG_STATUS, 1);
  if (result != nextpm.ku8MBSuccess) {
    LOG("NextPM : pas de réponse (erreur Modbus 0x%02X) : broches ? parité 8E1 ?\n", result);
    majMesure(mPM1, false, 0); majMesure(mPM25, false, 0); majMesure(mPM10, false, 0);
    return;
  }
  statutPM = nextpm.getResponseBuffer(0) & 0xFF;

  if (statutPM != 0) {
    LOG("NextPM état %d :", statutPM);
    for (int bit = 0; bit < 8; bit++) {
      if (statutPM & (1 << bit)) LOG(" [%s]", STATUS_BITS[bit]);
    }
    LOG("\n");
  }

  // 2) mesure inutilisable : on ne lit pas les PM
  if (statutPM & STATUS_CRITICAL) {
    EtatCapteur e = (statutPM & STATUS_DEMARRAGE) ? CAPTEUR_CHAUFFE : CAPTEUR_ERREUR;
    forcerEtat(mPM1, e); forcerEtat(mPM25, e); forcerEtat(mPM10, e);
    return;
  }

  // 3) particules, puis température / humidité internes
  float pm1 = -1, pm25 = -1, pm10 = -1;
  bool ok = lireTroisPM(pm1, pm25, pm10);
  majMesure(mPM1, ok, pm1); majMesure(mPM25, ok, pm25); majMesure(mPM10, ok, pm10);

  // humidité (0x6A) et température (0x6B) se suivent : une seule requête
  if (nextpm.readHoldingRegisters(REG_HUM, 2) == nextpm.ku8MBSuccess) {
    humPM  = nextpm.getResponseBuffer(0) / 100.0f;
    tempPM = (int16_t)nextpm.getResponseBuffer(1) / 100.0f;  // peut être négative
  } else {
    if (nextpm.readHoldingRegisters(REG_TEMP, 1) == nextpm.ku8MBSuccess)
      tempPM = (int16_t)nextpm.getResponseBuffer(0) / 100.0f;
    if (nextpm.readHoldingRegisters(REG_HUM, 1) == nextpm.ku8MBSuccess)
      humPM = nextpm.getResponseBuffer(0) / 100.0f;
  }
}

// Lit le MH-Z19C (CO2 en ppm). Pendant le préchauffage il n'y a pas de valeur fiable : on le signale simplement.
void lireMHZ() {
#if MHZ_ACTIVE
  // (drapeau : sans lui, millis() qui repasse à 0 au bout de 49 jours relancerait 3 min de "préchauffage")
  static bool chauffeFinie = false;
  if (!chauffeFinie) {
    if (millis() < CHAUFFE_MHZ_MS) { forcerEtat(mCO2, CAPTEUR_CHAUFFE); return; }
    chauffeFinie = true;
  }
  int co2 = mhz.getCO2();
  majMesure(mCO2, (mhz.errorCode == RESULT_OK && co2 > 0), (float)co2);
#else
  forcerEtat(mCO2, CAPTEUR_ERREUR);
#endif
}

// Fausses valeurs : un aller-retour entre le minimum et le maximum en 60 s
void lectureDemo() {
  float phase = (millis() % 60000UL) / 60000.0f;
  float tri = (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;  // 0 -> 1 -> 0
  mCO2.valeur = 400 + tri * 1800;   // 400 à 2200 ppm
  mPM25.valeur = tri * 90;          // 0 à 90 ug/m3 : parcourt les 6 niveaux ATMO
  mPM1.valeur = mPM25.valeur * 0.6f;
  mPM10.valeur = mPM25.valeur * 1.3f;
  forcerEtat(mCO2, CAPTEUR_OK); forcerEtat(mPM1, CAPTEUR_OK);
  forcerEtat(mPM25, CAPTEUR_OK); forcerEtat(mPM10, CAPTEUR_OK);
}

// Lit les deux capteurs, puis écrit une ligne de journal (visible seulement avec « verbose on »).
void lireCapteurs() {
#if MODE_DEMO
  lectureDemo();
#else
  lireMHZ();
  lireNextPM();
#endif
  LOG("CO2=%.0f ppm (etat %d) | PM1=%.1f PM2.5=%.1f PM10=%.1f ug/m3 (etat %d)",
      mCO2.valeur, mCO2.etat, mPM1.valeur, mPM25.valeur, mPM10.valeur, mPM25.etat);
  if (!isnan(tempPM)) LOG(" | T=%.1f C", tempPM);
  if (!isnan(humPM)) LOG(" HR=%.1f %%", humPM);
  LOG("\n");
}

// =====================================================================
// AGRÉGATS, DÉTECTION D'AÉRATION, HISTORIQUE
// =====================================================================
// Ajoute une valeur à l'agrégat : cumule la somme (pour la moyenne), compte les valeurs et retient le maximum (le pic).
void agregatAjouter(Agregat &a, float v) {
  if (a.n == 0 || v > a.maxi) a.maxi = v;
  a.somme += v;
  a.n++;
}

// Remet l'agrégat à zéro pour la période de mesure suivante.
void agregatRaz(Agregat &a) {
  a.somme = 0; a.n = 0; a.maxi = 0;
}

// Chute rapide du CO2 par rapport à il y a ~2 min : fenêtre ouverte ?
void detecterAeration(float co2) {
  if (tamponN == 12) {
    float ancien = tamponCO2[tamponIdx];  // la plus ancienne, sur le point d'être écrasée
    if (ancien - co2 >= AERATION_CHUTE_PPM) aerationPeriode = true;
  }
  // On range la lecture dans un tampon circulaire de 12 valeurs (~2 min) : la plus ancienne est écrasée.
  tamponCO2[tamponIdx] = co2;
  tamponIdx = (tamponIdx + 1) % 12;
  if (tamponN < 12) tamponN++;
}

// Appelée après chaque lecture : alimente moyennes, pics et statistiques
void accumulerLectures() {
  if (mCO2.etat == CAPTEUR_OK && mCO2.aJour) {
    agregatAjouter(agCO2, mCO2.valeur);
    nCO2Total++;
    if (mCO2.valeur > SEUIL_AERER_PPM) nCO2Haut++;
    detecterAeration(mCO2.valeur);
    histSomme += mCO2.valeur;  // pour la moyenne de la minute (histogramme)
    histNb++;
  }
  if (mPM25.etat == CAPTEUR_OK && mPM25.aJour) {
    // PM1, PM2.5 et PM10 sont lus ensemble
    agregatAjouter(agPM1, mPM1.valeur);
    agregatAjouter(agPM25, mPM25.valeur);
    agregatAjouter(agPM10, mPM10.valeur);
  }
}

// Moyenne du CO2 de la minute EN COURS (0 si aucune lecture valide pour l'instant) : dernière colonne de la courbe
uint16_t minuteEnCours() {
  return histNb ? (uint16_t)(histSomme / histNb + 0.5f) : 0;
}

// Une fois par minute : range la MOYENNE du CO2 de la minute écoulée (pas une valeur instantanée).
// Une minute sans lecture valide est rangée comme un trou (0) : l'axe du temps reste juste.
void ajouterHistorique() {
  if (histNb == 0 && histN == 0) return;  // rien encore (préchauffage) : la courbe ne commence pas par des trous
  histCO2[histIdx] = minuteEnCours();
  histIdx = (histIdx + 1) % HIST_N;
  if (histN < HIST_N) histN++;
  histSomme = 0;
  histNb = 0;
}

// i = 0 : le point le plus ancien (0 = minute sans mesure)
uint16_t valeurHistorique(uint8_t i) {
  uint8_t debut = (histN < HIST_N) ? 0 : histIdx;
  return histCO2[(debut + i) % HIST_N];
}

// Tendance du CO2 sur ~3 min : dir = +1 monte | -1 descend | 0 stable. false si pas assez de points.
bool tendanceCO2(int &dir) {
  if (histN < 4) return false;
  uint16_t recent = valeurHistorique(histN - 1), ancien = valeurHistorique(histN - 4);
  if (recent == 0 || ancien == 0) return false;  // trou dans les mesures
  int diff = (int)recent - (int)ancien;
  dir = (diff > 30) ? 1 : ((diff < -30) ? -1 : 0);
  return true;
}

// =====================================================================
// STOCKAGE HORS LIGNE (LittleFS, 1 mesure par ligne)
// =====================================================================
// Supprime le fichier hors ligne et remet les compteurs à zéro (tout a été renvoyé, ou vidage volontaire).
void reinitialiserBacklog() {
  if (fsOK && LittleFS.exists(FICHIER_BACKLOG)) LittleFS.remove(FICHIER_BACKLOG);
  offsetRejeu = 0;
  tailleBacklog = 0;
  nbLignesAttente = 0;
  prefs.putULong("off", 0);
}

// Au démarrage : reprend un compactage interrompu, puis retrouve la taille du fichier et le nombre de lignes à renvoyer
void initialiserBacklog() {
  tailleBacklog = 0;
  nbLignesAttente = 0;
  if (!fsOK) return;
  // Coupure pendant un compactage ? Le drapeau "cpt" n'est levé qu'une fois backlog.tmp COMPLET :
  // on termine le travail (suppression de l'ancien, renommage) sans rien perdre.
  if (prefs.getBool("cpt", false)) {
    if (LittleFS.exists(FICHIER_TMP)) {
      if (LittleFS.exists(FICHIER_BACKLOG)) LittleFS.remove(FICHIER_BACKLOG);
      LittleFS.rename(FICHIER_TMP, FICHIER_BACKLOG);
    }
    offsetRejeu = 0;
    prefs.putULong("off", 0);
    prefs.putBool("cpt", false);
    Serial.println("Compactage interrompu par une coupure : repris, aucune mesure perdue.");
  } else if (LittleFS.exists(FICHIER_TMP)) {
    LittleFS.remove(FICHIER_TMP);  // copie inachevée : l'original est intact
  }
  if (!LittleFS.exists(FICHIER_BACKLOG)) {
    if (offsetRejeu != 0) { offsetRejeu = 0; prefs.putULong("off", 0); }
    return;
  }
  File f = LittleFS.open(FICHIER_BACKLOG, "r");
  if (!f) return;
  tailleBacklog = f.size();
  if (offsetRejeu > tailleBacklog) offsetRejeu = 0;  // incohérent : on repart du début
  f.seek(offsetRejeu);
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\n') nbLignesAttente++;
    }
  }
  f.close();
  Serial.printf("Mesures en attente d'envoi : %u\n", (unsigned)nbLignesAttente);
}

// Réécrit le fichier en ne gardant que ce qui suit la position "depuis" (récupère la place).
// Ordre pensé pour survivre à une coupure : copie complète -> drapeau "cpt" -> suppression -> renommage
// (initialiserBacklog termine le travail si la carte redémarre au milieu).
// Renvoie false si la copie a échoué : l'original reste alors intact.
bool compacterBacklog(size_t depuis) {
  if (depuis > tailleBacklog) depuis = tailleBacklog;
  size_t aCopier = tailleBacklog - depuis;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < aCopier + 16384) {
    Serial.println("Flash insuffisante pour compacter le fichier hors ligne.");
    return false;
  }
  File src = LittleFS.open(FICHIER_BACKLOG, "r");
  if (!src) return false;
  src.seek(depuis);
  File dst = LittleFS.open(FICHIER_TMP, "w");
  if (!dst) { src.close(); return false; }
  uint8_t buf[256];
  size_t total = 0;
  bool ok = true;
  while (src.available()) {
    size_t n = src.read(buf, sizeof(buf));
    if (n == 0) break;
    if (dst.write(buf, n) != n) { ok = false; break; }
    total += n;
    esp_task_wdt_reset();  // une grosse copie peut durer plusieurs secondes
  }
  src.close();
  dst.close();
  if (!ok) {
    LittleFS.remove(FICHIER_TMP);
    Serial.println("Erreur d'écriture pendant le compactage : fichier hors ligne inchangé.");
    return false;
  }
  prefs.putBool("cpt", true);  // à partir d'ici, un redémarrage termine le travail
  LittleFS.remove(FICHIER_BACKLOG);
  LittleFS.rename(FICHIER_TMP, FICHIER_BACKLOG);
  offsetRejeu = 0;
  tailleBacklog = total;
  prefs.putULong("off", 0);
  prefs.putBool("cpt", false);
  return true;
}

// Fichier plein : on sacrifie le quart le plus ancien (une mesure récente vaut plus qu'une vieille)
void purgerAnciennes() {
  if (tailleBacklog <= offsetRejeu) return;
  size_t cible = offsetRejeu + (tailleBacklog - offsetRejeu) / 4;
  File f = LittleFS.open(FICHIER_BACKLOG, "r");
  if (!f) return;
  f.seek(offsetRejeu);
  uint8_t buf[256];
  size_t pos = offsetRejeu;   // position juste après la dernière ligne complète lue
  uint32_t perdues = 0;
  bool fini = false;
  while (!fini && f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      pos++;
      if (buf[i] == '\n') {
        perdues++;
        if (pos >= cible) { fini = true; break; }  // on s'arrête à la fin de la ligne qui contient la cible
      }
    }
    esp_task_wdt_reset();
  }
  f.close();
  if (perdues == 0) return;
  if (compacterBacklog(pos)) {
    nbLignesAttente = (nbLignesAttente > perdues) ? nbLignesAttente - perdues : 0;
    Serial.printf("Mémoire hors-ligne pleine : %u mesures les plus anciennes supprimées.\n", (unsigned)perdues);
  }
}

void stockerRecord(const String &ligne) {
  if (!fsOK) { Serial.println("Mémoire flash indisponible : mesure perdue."); return; }
  const size_t besoin = ligne.length() + 1;
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG && offsetRejeu > 0) compacterBacklog(offsetRejeu);  // récupère les lignes déjà renvoyées
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG) purgerAnciennes();                               // toujours plein : les plus vieilles partent
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG) {
    Serial.println("Mémoire hors-ligne pleine : mesure abandonnée.");
    return;
  }
  // Mode « a » (append) : la mesure est ajoutée à la fin du fichier, une mesure par ligne.
  File f = LittleFS.open(FICHIER_BACKLOG, "a");
  if (!f) { Serial.println("Erreur ouverture du fichier hors ligne."); return; }
  f.print(ligne);
  f.write('\n');
  f.close();
  tailleBacklog += besoin;
  nbLignesAttente++;
  LOG("Mesure stockée hors ligne (%u en attente).\n", (unsigned)nbLignesAttente);
}

// Lit la prochaine ligne à renvoyer ; "suite" = position juste après elle
bool lireProchaineLigne(String &ligne, size_t &suite) {
  File f = LittleFS.open(FICHIER_BACKLOG, "r");
  if (!f) return false;
  if (!f.seek(offsetRejeu) || !f.available()) { f.close(); return false; }
  ligne = f.readStringUntil('\n');
  suite = f.position();
  f.close();
  return true;
}

// La ligne vient d'être traitée (envoyée ou refusée définitivement) : on avance
void validerLigne(size_t suite) {
  offsetRejeu = suite;
  if (nbLignesAttente > 0) nbLignesAttente--;
  if (offsetRejeu >= tailleBacklog) {  // tout est renvoyé
    reinitialiserBacklog();
    LOG("Historique entièrement renvoyé : mémoire libérée.\n");
    return;
  }
  prefs.putULong("off", (uint32_t)offsetRejeu);
  // (plus de compactage ici : recopier jusqu'à 500 Ko bloquait les envois plusieurs secondes en plein rattrapage)
}

// =====================================================================
// ENVOI DIRECT AU SERVEUR AIRCARTO
// =====================================================================
// Secondes écoulées entre la mesure et maintenant.
//   >= 0 : l'âge
//   -1   : impossible à connaître (mesure d'un démarrage précédent, faite sans heure NTP)
//   -2   : connaissable bientôt (mesure d'un démarrage précédent, l'heure NTP n'est pas encore arrivée)
long calculerAge(JsonDocument &doc) {
  uint32_t u = doc["_u"] | 0UL;
  uint32_t e = doc["_e"] | 0UL;
  uint32_t b = doc["_b"] | 0UL;
  if (b == bootId) return (long)((millis() / 1000UL) - u);  // même démarrage : l'uptime suffit
  if (e == 0) return -1;
  if (!heureValide()) return -2;
  uint32_t maintenant = (uint32_t)time(nullptr);
  return (maintenant >= e) ? (long)(maintenant - e) : 0;
}

// Un POST ; la réponse du serveur décide de la suite
ResultatPush posterAuServeur(const String &payload) {
  // Un POST HTTP vers le serveur, avec des délais courts pour ne pas bloquer longtemps la tâche d'envoi.
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  // Le serveur compte 10 s entre le début de deux requêtes : on note l'heure AVANT l'envoi
  // (avant, elle était notée après la réponse : la durée du POST s'ajoutait à chaque créneau).
  tDernierPush = millis();
  dejaPoste = true;
  if (!http.begin(URL_SERVEUR)) {
    LOG("Échec : http.begin() a refusé l'URL.\n");
    echecsPush++; etatEnvoi = 2;
    return PUSH_REESSAYER;
  }
  // En-têtes : le contenu est du JSON, et la clé du groupe identifie la station auprès du serveur.
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Group-Key", lireCle());

  LOG("Envoi : %s\n", payload.c_str());
  // Envoie la requête. code = code HTTP du serveur (200 = OK, 401 = clé refusée...) ou valeur négative si le réseau a échoué.
  int code = http.POST(payload);
  dernierCodeHttp = code;
  String corps;
  if (code > 0) corps = http.getString();
  http.end();

  if (code <= 0) {  // Pas de réponse : Wi-Fi, URL (http://, pas https) ou serveur injoignable
    LOG("Échec de l'envoi : %s\n", HTTPClient::errorToString(code).c_str());
    echecsPush++; etatEnvoi = 2;
    return PUSH_REESSAYER;
  }
  LOG("Réponse HTTP %d\n", code);
  LOG("%s\n", corps.c_str());  // le message du serveur dit quoi corriger

  // Le serveur répond en JSON ; next_push_in_s = délai à respecter avant le prochain envoi.
  JsonDocument rep;
  int suivant = 0;
  if (!deserializeJson(rep, corps)) suivant = rep["next_push_in_s"] | 0;

  // Succès : on respecte le délai demandé par le serveur (next_push_in_s). La marge au-dessus diminue
  // doucement tant que le serveur ne refuse rien (10 succès de suite : -100 ms).
  if (code >= 200 && code < 300) {
    echecsPush = 0;
    unsigned long voulu = (unsigned long)suivant * 1000UL;
    ecartMinMs = (voulu > ECART_PUSH_MIN_MS) ? voulu : ECART_PUSH_MIN_MS;
    if (++succesDeSuite >= 10) {
      succesDeSuite = 0;
      if (margePushMs >= MARGE_PUSH_MIN_MS + 100) margePushMs -= 100;
    }
    etatEnvoi = 1;
    return PUSH_OK;
  }
  succesDeSuite = 0;
  if (code == 401) {  // clé absente, fausse, révoquée ou expirée : inutile d'insister
    cleInvalide = true;
    etatEnvoi = 3;
    Serial.println("Clé refusée : envois suspendus (mesures gardées en mémoire). Corrige avec 'set cle ...'.");
    return PUSH_CLE_INVALIDE;
  }
  if (code == 429) {  // envoi trop rapproché : on agrandit la marge (ce n'est pas une panne : pas d'espacement progressif)
    margePushMs += 500;
    if (margePushMs > MARGE_PUSH_MAX_MS) margePushMs = MARGE_PUSH_MAX_MS;
    if (suivant > 0) ecartMinMs = max(ECART_PUSH_MIN_MS, (unsigned long)suivant * 1000UL);
    etatEnvoi = 2;
    return PUSH_REESSAYER;
  }
  if (code == 400 || code == 413) {  // cette mesure est refusée : la renvoyer ne servirait à rien
    etatEnvoi = 3;
    return PUSH_REJETE;
  }
  // 404, 405, 5xx... : problème d'URL ou de serveur, pas de la mesure : on la garde
  echecsPush++;
  etatEnvoi = (code >= 500) ? 2 : 3;
  return PUSH_REESSAYER;
}

// Prépare une ligne stockée (retire les métadonnées, ajoute x_age_s) et l'envoie
ResultatPush envoyerLigne(const String &ligne) {
  JsonDocument doc;
  if (deserializeJson(doc, ligne)) {
    LOG("Ligne illisible : abandonnée.\n");
    return PUSH_INVALIDE;
  }
  long age = calculerAge(doc);
  if (age == -2) {  // l'heure NTP va arriver : on garde la ligne et on réessaiera, on ne la jette pas
    unsigned long now = millis();
    if (tAttenteHeure == 0) tAttenteHeure = now;
    if (now - tAttenteHeure < ATTENTE_HEURE_MAX_MS) return PUSH_ATTENDRE;
    LOG("Heure NTP absente depuis trop longtemps : mesure abandonnée.\n");  // tAttenteHeure reste armé : les suivantes suivent sans attendre
    return PUSH_INVALIDE;
  }
  tAttenteHeure = 0;
  if (age < 0) {
    LOG("Heure de la mesure inconnue (redémarrage sans heure NTP) : abandonnée.\n");
    return PUSH_INVALIDE;
  }
  doc.remove("_u"); doc.remove("_e"); doc.remove("_b");
  doc["x_age_s"] = age;
  String payload;
  serializeJson(doc, payload);
  return posterAuServeur(payload);
}

// Construit la mesure de la période écoulée (moyennes + pics) et remet les agrégats à zéro
// Fabrique la mesure de la période écoulée en JSON (moyennes, pics, état de la carte), puis remet les agrégats à zéro.
// Un champ hors bornes est omis : un seul champ invalide ferait refuser toute la requête par le serveur.
bool construireRecord(String &ligne) {
  bool aCO2 = (agCO2.n > 0);
  bool aPM = (agPM25.n > 0);
  if (!aCO2 && !aPM) return false;

  JsonDocument doc;
  // Champs standard attendus par AirCarto : identifiant de la carte, uptime, signal Wi-Fi, mémoire libre.
  doc["device_id"] = deviceId;
  doc["uptime"] = millis() / 1000;
  if (reseauOK()) {
    int rssi = WiFi.RSSI();
    if (rssi >= -120 && rssi <= 0) doc["rssi"] = rssi;
  }
  doc["heap"] = ESP.getFreeHeap();

  // CO2 : moyenne de la période (co2) et pic (x_co2_max).
  if (aCO2) {
    float moy = agCO2.somme / agCO2.n;
    if (dansBornes("co2", moy, 0, 10000)) doc["co2"] = (int)(moy + 0.5f);
    if (dansBornes("x_co2_max", agCO2.maxi, 0, 10000)) doc["x_co2_max"] = (int)agCO2.maxi;
  }
  // Particules : moyennes PM1 / PM2.5 / PM10, pic de PM2.5, état du NextPM et sa température / humidité.
  if (aPM) {
    float m1  = agPM1.somme / agPM1.n;
    float m25 = agPM25.somme / agPM25.n;
    float m10 = agPM10.somme / agPM10.n;
    if (dansBornes("pm1", m1, 0, 1000)) doc["pm1"] = arrondi1(m1);
    if (dansBornes("pm25", m25, 0, 1000)) doc["pm25"] = arrondi1(m25);
    if (dansBornes("pm10", m10, 0, 1000)) doc["pm10"] = arrondi1(m10);
    if (dansBornes("x_pm25_max", agPM25.maxi, 0, 1000)) doc["x_pm25_max"] = arrondi1(agPM25.maxi);
    doc["x_pm_statut"] = (int)statutPM;
    if (!isnan(tempPM)) doc["x_temp_pm"] = arrondi1(tempPM);
    if (!isnan(humPM)) doc["x_hum_pm"] = arrondi1(humPM);
  }
  // Champs personnalisés « x_ » (10 au maximum, imposé par AirCarto) : alimentation, aération détectée,
  // % du temps au-dessus de 1000 ppm, cause du dernier redémarrage.
  doc["x_alim"] = estSurSecteur ? 1 : 0;
  doc["x_aeration"] = aerationPeriode ? 1 : 0;
  if (nCO2Total > 0) doc["x_pct_co2_1000"] = arrondi1(100.0f * nCO2Haut / nCO2Total);
  doc["x_reset"] = (int)esp_reset_reason();

  // Métadonnées internes (retirées avant l'envoi) : quand la mesure a été faite
  doc["_u"] = millis() / 1000;
  doc["_e"] = heureValide() ? (uint32_t)time(nullptr) : (uint32_t)0;
  doc["_b"] = bootId;

  ligne = "";
  serializeJson(doc, ligne);

  agregatRaz(agCO2); agregatRaz(agPM1); agregatRaz(agPM25); agregatRaz(agPM10);
  aerationPeriode = false;
  return true;
}

// loop() dépose la mesure ; la tâche d'envoi la prend (réseau, ou fichier hors ligne si pas de réseau)
void deposerMesure(const String &ligne) {
  xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  if (nbPendantes >= NB_PENDANTES) {  // la tâche d'envoi n'a rien traité depuis plusieurs périodes : on sacrifie la plus ancienne
    for (uint8_t i = 1; i < nbPendantes; i++) pendantes[i - 1] = pendantes[i];
    nbPendantes--;
    pendantes[nbPendantes] = "";
  }
  pendantes[nbPendantes++] = ligne;
  xSemaphoreGive(mutexEnvoi);
}

// Côté tâche d'envoi : prend la plus ancienne mesure déposée (false s'il n'y en a pas)
bool sortirPendante(String &ligne) {
  bool ok = false;
  xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  if (nbPendantes > 0) {
    ligne = pendantes[0];
    for (uint8_t i = 1; i < nbPendantes; i++) pendantes[i - 1] = pendantes[i];
    nbPendantes--;
    pendantes[nbPendantes] = "";
    ok = true;
  }
  xSemaphoreGive(mutexEnvoi);
  return ok;
}

// Fin d'une période de mesure : la mesure est prête, la tâche d'envoi s'en occupe
void finaliserPeriode() {
#if MODE_DEMO
  return;  // les fausses valeurs ne partent jamais vers le serveur
#endif
  String ligne;
  if (!construireRecord(ligne)) {
    LOG("Période sans mesure valide : rien à enregistrer.\n");
    return;
  }
  deposerMesure(ligne);
}

// Renvoie la plus ancienne mesure stockée. Les lignes écartées SANS envoi (vides, illisibles, heure inconnue)
// sont passées à la suite, sans attendre le créneau de 10 s : avant, chacune coûtait un créneau entier.
void rejouerUneLigne() {
  for (uint8_t essais = 0; essais < 50 && nbLignesAttente > 0; essais++) {
    String ligne;
    size_t suite = 0;
    if (!lireProchaineLigne(ligne, suite)) {  // fichier incohérent : on repart propre
      reinitialiserBacklog();
      return;
    }
    ligne.trim();
    if (ligne.length() == 0) { validerLigne(suite); continue; }
    LOG("Renvoi d'une mesure stockée (%u en attente)...\n", (unsigned)nbLignesAttente);
    ResultatPush r = envoyerLigne(ligne);
    if (r == PUSH_INVALIDE) { validerLigne(suite); esp_task_wdt_reset(); continue; }
    if (r == PUSH_ATTENDRE) { tEssaiNtp = millis(); return; }   // on relira dans ATTENTE_NTP_REESSAI_MS
    if (r == PUSH_OK || r == PUSH_REJETE) validerLigne(suite);
    return;  // un POST a eu lieu : le créneau est pris
  }
}

// Délai minimal entre le début de deux envois : délai du serveur + marge,
// multiplié (x2 à x6) après des pannes réseau / serveur consécutives.
unsigned long ecartCourantMs() {
  unsigned long facteur = 1 + (echecsPush > 5 ? 5 : echecsPush);
  return (ecartMinMs + margePushMs) * facteur;
}

// Exécutée UNIQUEMENT par la tâche d'envoi (tacheEnvoi). Elle seule touche au réseau et au fichier hors ligne.
// La mesure fraîche passe d'abord, l'historique profite des créneaux libres.
void gererEnvois(unsigned long now) {
#if MODE_DEMO
  return;
#endif
  if (demandeVider) {  // commande "vider_file"
    demandeVider = false;
    reinitialiserBacklog();
    Serial.println("File d'attente vidée.");
  }

  bool enLigne = reseauOK() && cleOK() && !cleInvalide;

  // Pas de réseau (ou pas de clé valide) : tout ce qui attend en RAM est mis de côté en flash
  if (!enLigne) {
    tAttenteHeure = 0;
    String ligne;
    while (sortirPendante(ligne)) {
      stockerRecord(ligne);
      if (etatEnvoi == 1) etatEnvoi = 2;
    }
    return;
  }
  if (dejaPoste && now - tDernierPush < ecartCourantMs()) return;  // le serveur refuse les envois trop rapprochés

  String ligne;
  if (sortirPendante(ligne)) {
    ResultatPush r = envoyerLigne(ligne);
    if (r == PUSH_REESSAYER || r == PUSH_CLE_INVALIDE || r == PUSH_ATTENDRE) stockerRecord(ligne);
    return;
  }
  if (nbLignesAttente == 0) return;
  if (tAttenteHeure != 0 && now - tEssaiNtp < ATTENTE_NTP_REESSAI_MS) return;  // l'heure NTP n'est pas encore là
  rejouerUneLigne();
}

// Tâche d'envoi : un POST qui traîne (jusqu'à ~5 s) ne bloque plus l'écran ni les capteurs.
// Réveil toutes les 50 ms : le créneau suivant est pris au plus près des 10 s.
void tacheEnvoi(void* param) {
  esp_task_wdt_add(NULL);  // surveillée par le watchdog, comme loop()
  for (;;) {
    esp_task_wdt_reset();
    gererEnvois(millis());
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =====================================================================
// AFFICHAGE (écran 64 x 32)
// =====================================================================
// Trois polices :
//   PETITE  Picopixel (5 px de haut)          : libellés, unités, qualificatifs     -> ecrireP / ecrirePDroite
//   MOYENNE police standard 5x7 (taille 1)    : valeurs des particules, textes      -> ecrireTexte / ecrireNombre
//   GRANDE  FreeSansBold 12 pt (chiffres 17 px) : valeur du CO2 ; 9 pt pour "AERER" -> ecrireGrand
// Avec Picopixel et FreeSans (polices "GFX"), le curseur est la LIGNE DE BASE du texte : les fonctions
// ci-dessous prennent toujours le HAUT du texte (ou la ligne de base pour ecrireGrand), et remettent
// la police standard après usage.
//
// Grille commune (y en pixels) :
//   0..5     ligne de titre en petite police : nom à gauche, unité / qualificatif / état à droite
//   7..23    contenu principal (grand chiffre, lignes de particules, visage...)
//   26..31   bas de page (barre, VU-mètre), sur x 0..LARGEUR_BAS-1
//   x 57..63, y 25..31 : zone des témoins (Wi-Fi, secteur, heure, envoi), jamais recouverte

// ---------- couleurs ----------
// Convertit une couleur (rouge, vert, bleu de 0 à 255) en 16 bits (5-6-5), le format des pixels de l'écran.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Assombrit une couleur 565 : k = 0..255 (255 = presque inchangée)
uint16_t atten(uint16_t c, uint8_t k) {
  uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (r * k) >> 8; g = (g * k) >> 8; b = (b * k) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Couleurs des niveaux, calculées une seule fois depuis PALETTE
uint16_t colNiv[NB_NIVEAUX];     // pleine
uint16_t colFond[NB_NIVEAUX];    // niveau non atteint (barre éteinte du VU-mètre, fond du thermomètre)

// Calcule une seule fois les couleurs pleines et atténuées de chaque niveau, à partir de PALETTE.
void initialiserCouleurs() {
  for (uint8_t i = 0; i < NB_NIVEAUX; i++) {
    colNiv[i]  = rgb565(PALETTE[i][0], PALETTE[i][1], PALETTE[i][2]);
    colFond[i] = atten(colNiv[i], 70);
  }
}

// Couleurs neutres (jamais utilisées pour un niveau de qualité). Pas de blanc : il rend mal sur un panneau LED.
static const uint16_t COL_TITRE   = rgb565(0, 255, 230);   // cyan vif : titres (les LED bleues sont faibles : on mise sur le vert)
static const uint16_t COL_DISCRET = rgb565(0, 140, 130);   // cyan sombre : unités, textes secondaires
static const uint16_t COL_PISTE   = rgb565(40, 45, 60);    // fond d'une barre de progression, visage en attente
static const uint16_t COL_ERREUR  = rgb565(230, 0, 0);

// ---------- niveaux ----------
// Valeur telle qu'affichée : 1 décimale sous 10, entier au-dessus (les seuils ATMO sont des entiers).
float arrondiAffiche(float v) {
  return (v < 9.95f) ? roundf(v * 10.0f) / 10.0f : roundf(v);
}

// Niveau d'une valeur : numéro du premier seuil qu'elle n'a pas dépassé (0 = vert, 1 = jaune...).
// Exemples : CO2 = 900 ppm (800 / 1000 / 1500, "<")  -> niveau 1 (jaune)
//            PM2.5 = 10.0 (10 / 20 / 25..., "<=")   -> niveau 0 (bon) ; 10.1 -> niveau 1 (moyen)
// Échelle "inclus" : on compare la valeur arrondie comme à l'écran (couleur et chiffre toujours d'accord).
uint8_t niveau(float v, const Echelle &e) {
  if (e.inclus) v = arrondiAffiche(v);
  for (uint8_t i = 0; i < e.n; i++) {
    if (e.inclus ? (v <= e.seuils[i]) : (v < e.seuils[i])) return i;
  }
  return e.n;
}

// Même chose pour une Mesure, mais -1 tant que le capteur n'est pas prêt.
int niveauMesure(const Mesure &m, const Echelle &e) {
  if (m.etat != CAPTEUR_OK) return -1;
  return niveau(m.valeur, e);
}

// INDICE GLOBAL : comment les polluants sont-ils « pondérés » ?
// Ils ne le sont PAS, et c'est voulu. On applique la règle officielle de l'indice ATMO (arrêté du 10 juillet 2020,
// https://www.atmo-france.org/article/lindice-atmo) : chaque polluant reçoit un SOUS-INDICE (1 à 6) sur sa propre
// échelle, et l'indice est le PIRE de ces sous-indices. Pas de moyenne : les seuils de chaque échelle sont déjà
// calés sur la nocivité du polluant (25 ug/m3 de PM2.5 et 50 ug/m3 de PM10 donnent tous deux « mauvais ») ;
// une moyenne, pondérée ou non, laisserait un polluant très élevé être « caché » par un autre resté bas.
// Ce qui entre dans l'indice :
//   - PM2.5 et PM10 : oui (seuils ATMO officiels) ;
//   - PM1  : non, l'indice ATMO ne lui fixe aucun seuil (la page PM1 reste colorée à titre indicatif) ;
//   - CO2  : non, ce n'est pas un polluant de l'indice ATMO mais un indicateur de CONFINEMENT (aération) :
//            il garde ses propres pages (CO2, AERER) et ne modifie jamais l'indice.
// L'ATMO officiel inclut aussi O3, NO2 et SO2, que la carte ne mesure pas : cet indice est donc PARTIEL (particules).
// Renvoie -1 si le NextPM n'est pas prêt, + le nom du polluant responsable.
int niveauIndiceATMO(const char** pire) {
  int n25 = niveauMesure(mPM25, ECH_PM25);
  int n10 = niveauMesure(mPM10, ECH_PM10);
  int pireN = n25;
  const char* nom = "PM2.5";
  if (n10 > pireN) { pireN = n10; nom = "PM10"; }
  if (pire) *pire = nom;
  return pireN;
}

// Vrai si le CO2 dépasse le seuil d'aération : déclenche la page « AERER ».
bool alerteAerer() {
  return mCO2.etat == CAPTEUR_OK && mCO2.valeur >= SEUIL_AERER_PPM;
}

// Valeur à afficher : celle qui glisse vers la vraie valeur (ou la vraie valeur si ANIMATIONS 0)
float valAff(const Mesure &m) {
#if ANIMATIONS
  return m.affInit ? m.aff : m.valeur;
#else
  return m.valeur;
#endif
}

// Clignement des yeux : 150 ms toutes les 4,3 s
bool yeuxFermes() {
#if ANIMATIONS
  return (millis() % 4300UL) < 150UL;
#else
  return false;
#endif
}

// Flèche de tendance : monte = rouge, baisse = vert, stable = discret
uint16_t couleurTendance(int dir) {
  return (dir > 0) ? colNiv[3] : ((dir < 0) ? colNiv[0] : COL_DISCRET);
}

// ---------- texte : police standard 5x7 ----------
// Largeur réelle d'un texte (sans l'espace final)
int largeurTexte(const char* s, uint8_t taille) {
  return (int)strlen(s) * 6 * taille - taille;
}

// (x, y) = coin haut-gauche ; un caractère de taille 1 fait 5 px de large et 7 de haut
void ecrireTexte(const char* s, int x, int y, uint8_t taille, uint16_t couleur) {
  display.setFont(nullptr);
  display.setTextSize(taille);
  display.setTextColor(couleur);
  display.setCursor(x, y);
  display.print(s);
}

void ecrireCentre(const char* s, int cx, int y, uint8_t taille, uint16_t couleur) {
  ecrireTexte(s, cx - largeurTexte(s, taille) / 2, y, taille, couleur);
}

// Nombre en 5x7 avec un point décimal ÉTROIT (1 px au lieu d'un caractère entier : "4.2" et non "4 .2").
int largeurNombre(const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) w += (*p == '.') ? 2 : 6;
  return w - 1;
}

// xFin = dernière colonne occupée ; y = haut des chiffres
void ecrireNombre(const char* s, int xFin, int y, uint16_t c) {
  int x = xFin - largeurNombre(s) + 1;
  char ch[2] = {0, 0};
  for (const char* p = s; *p; p++) {
    if (*p == '.') { display.drawPixel(x, y + 6, c); x += 2; continue; }
    ch[0] = *p;
    ecrireTexte(ch, x, y, 1, c);
    x += 6;
  }
}

// Valeur de particules : 1 décimale sous 10, entier au-dessus, 999 au plus (3 caractères)
void formaterPM(char* buf, size_t n, float v) {
  v = constrain(arrondiAffiche(v), 0.0f, 999.0f);
  if (v < 9.95f) snprintf(buf, n, "%d.%d", (int)v, (int)roundf(v * 10.0f) % 10);
  else snprintf(buf, n, "%d", (int)v);
}

// ---------- texte : polices GFX (petite et grande) ----------
// Encre d'un texte : décalage x1 du premier pixel allumé et largeur w (comme getTextBounds)
void bornesTexte(const GFXfont* f, const char* s, int16_t &x1, uint16_t &w) {
  int16_t y1; uint16_t h;
  display.setFont(f);
  display.setTextSize(1);
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setFont(nullptr);
}

int largeurP(const char* s) {
  int16_t x1; uint16_t w;
  bornesTexte(&Picopixel, s, x1, w);
  return w;
}

// Petite police, yHaut = haut des majuscules (5 px). Renvoie le x juste après le texte (pour enchaîner).
int ecrireP(const char* s, int x, int yHaut, uint16_t c) {
  display.setFont(&Picopixel);
  display.setTextSize(1);
  display.setTextColor(c);
  display.setCursor(x, yHaut + 4);   // Picopixel : la ligne de base est la dernière rangée des majuscules
  display.print(s);
  int xFin = display.getCursorX();
  display.setFont(nullptr);
  return xFin;
}

// xFin = dernière colonne occupée par le texte. Renvoie le x de début.
int ecrirePDroite(const char* s, int xFin, int yHaut, uint16_t c) {
  int16_t x1; uint16_t w;
  bornesTexte(&Picopixel, s, x1, w);
  int x = xFin - (int)w + 1 - x1;
  ecrireP(s, x, yHaut, c);
  return x + x1;
}

// Grande police, centrée sur la colonne cx ; base = ligne de base (bas des chiffres)
void ecrireGrand(const char* s, int cx, int base, uint16_t c, const GFXfont* f = &FreeSansBold12pt7b) {
  int16_t x1; uint16_t w;
  bornesTexte(f, s, x1, w);
  display.setFont(f);
  display.setTextSize(1);
  display.setTextColor(c);
  display.setCursor(cx - (int)w / 2 - x1, base);
  display.print(s);
  display.setFont(nullptr);
}

// Unité des particules en petite police : µg/m³ (masse par volume d'air). Picopixel n'a ni "µ" ni "³" :
// µ = "u" + un jambage d'1 px, ³ = petit 3 de 3x3 en exposant. xFin = dernière colonne occupée.
void ecrireUniteUgM3(int xFin, int yHaut, uint16_t c) {
  int16_t x1; uint16_t w;
  bornesTexte(&Picopixel, "ug/m", x1, w);
  int x = xFin - ((int)w + 4) + 1;
  ecrireP("ug/m", x, yHaut, c);
  display.drawPixel(x, yHaut + 5, c);                      // jambage du µ
  int xe = x + w + 1;                                      // ³ : 3 x 3 en haut
  display.drawFastHLine(xe, yHaut, 3, c);
  display.drawFastHLine(xe + 1, yHaut + 1, 2, c);
  display.drawFastHLine(xe, yHaut + 2, 3, c);
}

// ---------- éléments graphiques ----------
// Flèche de tendance 5x5 (cx = centre, y = haut) : dir > 0 monte | dir < 0 descend | 0 stable
void dessinerFleche(int cx, int y, int dir, uint16_t c) {
  if (dir > 0) {
    display.drawFastVLine(cx, y, 5, c);
    display.drawPixel(cx - 1, y + 1, c); display.drawPixel(cx + 1, y + 1, c);
    display.drawPixel(cx - 2, y + 2, c); display.drawPixel(cx + 2, y + 2, c);
  } else if (dir < 0) {
    display.drawFastVLine(cx, y, 5, c);
    display.drawPixel(cx - 1, y + 3, c); display.drawPixel(cx + 1, y + 3, c);
    display.drawPixel(cx - 2, y + 2, c); display.drawPixel(cx + 2, y + 2, c);
  } else {
    display.drawFastHLine(cx - 2, y + 2, 5, c);
    display.drawPixel(cx + 1, y + 1, c); display.drawPixel(cx + 1, y + 3, c);
  }
}

// VU-mètre : 6 barres de 2 px, de hauteurs croissantes (2 à 7 px), une par niveau ATMO.
// Les barres jusqu'au niveau atteint sont allumées, chacune à SA couleur (vert, jaune, orange...) ;
// les suivantes restent sombres. niv = -1 : tout éteint. x = gauche, yBas = bas des barres. Largeur 17 px.
void dessinerVU(int x, int yBas, int niv) {
  for (int i = 0; i < NB_NIVEAUX; i++) {
    int h = 2 + i;
    display.fillRect(x + 3 * i, yBas - h + 1, 2, h, (i <= niv) ? colNiv[i] : colFond[i]);
  }
}

// Visage rond de rayon 8 (17 x 17), centré en (cx, cy). Traits en noir, un par niveau :
//   0 grand sourire | 1 sourire | 2 bouche droite | 3 bouche triste | 4 bouche ouverte + sourcils |
//   5 bouche grande ouverte + yeux en croix | -1 (capteurs pas prêts) : visage gris, bouche droite
void dessinerVisage(int cx, int cy, int niv, uint16_t c, bool clin) {
  display.fillCircle(cx, cy, 8, c);
  // yeux
  if (niv >= 5) {                                          // yeux en croix (3 x 3)
    for (int k = -1; k <= 1; k += 2) {
      int ex = cx + 4 * k, ey = cy - 3;
      display.drawPixel(ex - 1, ey - 1, 0); display.drawPixel(ex + 1, ey - 1, 0);
      display.drawPixel(ex, ey, 0);
      display.drawPixel(ex - 1, ey + 1, 0); display.drawPixel(ex + 1, ey + 1, 0);
    }
  } else if (clin) {                                       // clignement
    display.drawFastHLine(cx - 4, cy - 2, 2, 0);
    display.drawFastHLine(cx + 3, cy - 2, 2, 0);
  } else {
    display.fillRect(cx - 4, cy - 3, 2, 2, 0);
    display.fillRect(cx + 3, cy - 3, 2, 2, 0);
  }
  // bouche
  switch (niv) {
    case 0:                                                // grand sourire
      display.drawPixel(cx - 4, cy + 1, 0); display.drawPixel(cx + 4, cy + 1, 0);
      display.drawPixel(cx - 3, cy + 2, 0); display.drawPixel(cx + 3, cy + 2, 0);
      display.drawFastHLine(cx - 2, cy + 3, 5, 0);
      break;
    case 1:                                                // sourire
      display.drawPixel(cx - 3, cy + 2, 0); display.drawPixel(cx + 3, cy + 2, 0);
      display.drawFastHLine(cx - 2, cy + 3, 5, 0);
      break;
    case 3:                                                // triste
      display.drawFastHLine(cx - 2, cy + 2, 5, 0);
      display.drawPixel(cx - 3, cy + 3, 0); display.drawPixel(cx + 3, cy + 3, 0);
      break;
    case 4:                                                // bouche ouverte + sourcils froncés
      display.fillRect(cx - 2, cy + 1, 5, 3, 0);
      display.drawFastHLine(cx - 5, cy - 5, 2, 0); display.drawFastHLine(cx + 4, cy - 5, 2, 0);
      break;
    case 5:                                                // grande bouche ouverte
      display.fillRect(cx - 2, cy + 1, 5, 4, 0);
      break;
    default:                                               // 2 et -1 : bouche droite
      display.drawFastHLine(cx - 3, cy + 3, 7, 0);
      break;
  }
}

// ---------- page CO2 ----------
// Ligne de titre : CO2 ppm | flèche de tendance | qualificatif (couleur du niveau)
// Centre : la valeur en grand (FreeSansBold 12 pt), couleur du niveau
// Bas : thermomètre PROPORTIONNEL de 400 à 2000 ppm (x 0..LARGEUR_BAS-1) : chaque zone (vert < 800,
//       jaune < 1000, orange < 1500, rouge) est allumée jusqu'à la valeur, sombre au-delà ; un curseur marque la valeur.
static const float THERMO_MIN_PPM = 400, THERMO_MAX_PPM = 2000;

int xThermometre(float ppm) {
  int x = (int)lroundf((ppm - THERMO_MIN_PPM) * (LARGEUR_BAS - 1) / (THERMO_MAX_PPM - THERMO_MIN_PPM));
  return constrain(x, 0, LARGEUR_BAS - 1);
}

void dessinerThermometre(float v, int niv) {
  const float bornes[5] = {THERMO_MIN_PPM, SEUILS_CO2[0], SEUILS_CO2[1], SEUILS_CO2[2], THERMO_MAX_PPM};
  int xv = xThermometre(v);
  for (int i = 0; i < 4; i++) {
    int x0 = xThermometre(bornes[i]);
    int x1 = xThermometre(bornes[i + 1]) - ((i < 3) ? 1 : 0);   // 1 px noir entre deux zones
    for (int x = x0; x <= x1; x++) display.drawFastVLine(x, 28, 2, (x <= xv) ? colNiv[i] : colFond[i]);
  }
  display.drawFastVLine(xv, 26, 6, colNiv[niv]);                  // curseur
}

void dessinerPageCO2() {
  const Mesure &m = mCO2;
  ecrireP("CO2", 0, 0, COL_TITRE);

  if (m.etat == CAPTEUR_CHAUFFE) {                     // préchauffage : compte à rebours + barre de progression
    unsigned long t = millis();
    unsigned long reste = (t < CHAUFFE_MHZ_MS) ? (CHAUFFE_MHZ_MS - t + 999UL) / 1000UL : 0;
    char buf[8];
    snprintf(buf, sizeof(buf), "%lu", reste);
    ecrirePDroite("PRECHAUFFAGE", 63, 0, COL_TITRE);
    ecrireGrand(buf, 32, 23, COL_TITRE);
    display.fillRect(0, 28, LARGEUR_BAS, 2, COL_PISTE);
    int w = (t < CHAUFFE_MHZ_MS) ? (int)((unsigned long)LARGEUR_BAS * t / CHAUFFE_MHZ_MS) : LARGEUR_BAS;
    display.fillRect(0, 28, w, 2, COL_TITRE);
    return;
  }
  if (m.etat == CAPTEUR_ERREUR) {                      // panne
    ecrirePDroite("PANNE", 63, 0, COL_ERREUR);
    ecrireGrand("ERR", 32, 23, COL_ERREUR);
    for (int x = 0; x < LARGEUR_BAS; x += 4) display.drawFastHLine(x, 28, 2, atten(COL_ERREUR, 140));
    return;
  }

  float v = valAff(m);
  int niv = niveau(v, ECH_CO2);
  uint16_t c = colNiv[niv];
  ecrireP("ppm", 13, 0, COL_DISCRET);
  ecrirePDroite(LABELS_NIVEAU[niv], 63, 0, c);
  int dir;
  if (tendanceCO2(dir)) dessinerFleche(31, 0, dir, couleurTendance(dir));
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(constrain(v, 0.0f, 9999.0f)));
  ecrireGrand(buf, 32, 23, c);
  dessinerThermometre(v, niv);
}

// ---------- pages PM1, PM2.5, PM10 (une grandeur par écran) ----------
// Même grille que la page CO2 :
//   0..5   nom (cyan) à gauche | unité ug/m3 à droite
//   7..23  la valeur en grand (FreeSansBold 12 pt), couleur du niveau ATMO ; elle PULSE si le polluant est en alerte
//   25..31 qualificatif ATMO (petite police, couleur du niveau) à gauche | VU-mètre des 6 niveaux (x 39..55)
// PM1 : pas de seuil ATMO, il est coloré avec l'échelle des PM2.5, à titre indicatif.
bool alertePage(uint8_t p);
void dessinerPagePM(const Mesure &m, const char* nom, const Echelle &e, uint8_t page) {
  ecrireP(nom, 0, 0, COL_TITRE);
  if (m.etat == CAPTEUR_CHAUFFE) {
    ecrirePDroite("DEMARRAGE", 63, 0, COL_TITRE);
    ecrireGrand("--", 28, 23, COL_DISCRET);
    dessinerVU(39, 31, -1);
    return;
  }
  if (m.etat == CAPTEUR_ERREUR) {
    ecrirePDroite("PANNE", 63, 0, COL_ERREUR);
    ecrireGrand("ERR", 28, 23, COL_ERREUR);
    dessinerVU(39, 31, -1);
    return;
  }
  ecrireUniteUgM3(63, 0, COL_DISCRET);
  float v = valAff(m);
  int niv = niveau(v, e);
  uint16_t c = colNiv[niv];
  // En alerte : la valeur pulse (vif / atténué toutes les ALERTE_PULSE_MS)
  uint16_t cVal = c;
  if (alertePage(page) && ((millis() / ALERTE_PULSE_MS) % 2UL) == 1) cVal = atten(c, 110);
  char buf[8];
  formaterPM(buf, sizeof(buf), v);
  ecrireGrand(buf, 28, 23, cVal);
  const char* label = LABELS_NIVEAU[niv];
  if (largeurP(label) > 37) label = LABELS_COURTS[niv];   // x 0..37 : il reste la place du VU-mètre
  ecrireP(label, 0, 26, c);
  dessinerVU(39, 31, niv);
}

// ---------- page AERER (seulement quand le CO2 dépasse SEUIL_AERER_PPM) ----------
// Ligne de titre : CO2 <valeur> ppm + tendance | "AERER" en grand qui pulse doucement | consigne en bas.
void dessinerPageAerer() {
  int niv = niveauMesure(mCO2, ECH_CO2);
  if (niv < 0) niv = 3;
  uint16_t c = colNiv[niv];
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(mCO2.valeur));
  ecrireP("CO2", 0, 0, COL_TITRE);
  int x = ecrireP(buf, 14, 0, c);
  ecrireP("ppm", x + 2, 0, COL_DISCRET);
  int dir;
  if (tendanceCO2(dir)) dessinerFleche(61, 0, dir, couleurTendance(dir));

  bool fort = ((millis() / 600UL) % 2UL) == 0;        // pulsation : vif / atténué toutes les 0,6 s
  ecrireGrand("AERER", 32, 21, fort ? c : atten(c, 110), &FreeSansBold9pt7b);
  const char* consigne = "OUVRIR FENETRE";
  ecrireP(consigne, (LARGEUR_BAS - largeurP(consigne)) / 2, 26, COL_TITRE);
}

// ---------- page INDICE ATMO (synthèse des particules, règle du pire sous-indice : voir niveauIndiceATMO) ----------
// Visage (couleur du niveau) | qualificatif ATMO en 5x7 (1 ou 2 lignes) | VU-mètre du niveau.
// En haut à droite : le polluant responsable (PM2.5 ou PM10) quand l'air n'est pas "bon". Le CO2 n'y entre pas.
void dessinerPageGlobal() {
  ecrireP("INDICE ATMO", 0, 0, COL_TITRE);
  const char* cause = "";
  int niv = niveauIndiceATMO(&cause);
  if (niv < 0) {                                         // capteurs pas encore prêts
    dessinerVisage(9, 18, -1, COL_PISTE, false);
    ecrireTexte("ATTENTE", 21, 12, 1, COL_TITRE);
    dessinerVU(21, 31, -1);
    return;
  }
  uint16_t c = colNiv[niv];
  if (niv > 0) ecrirePDroite(cause, 63, 0, c);
  dessinerVisage(9, 18, niv, c, yeuxFermes());
  // qualificatif : 7 caractères par ligne (x 21..61) ; plus long : coupé en 2 lignes à l'espace
  const char* label = LABELS_NIVEAU[niv];
  const char* espace = strchr(label, ' ');
  if (!espace) {
    ecrireTexte(label, 21, 12, 1, c);
  } else {
    char l1[12];
    size_t n = min((size_t)(espace - label), sizeof(l1) - 1);
    memcpy(l1, label, n);
    l1[n] = '\0';
    ecrireTexte(l1, 21, 8, 1, c);
    ecrireTexte(espace + 1, 21, 16, 1, c);
  }
  dessinerVU(21, 31, niv);
}

// ---------- page courbe CO2 ----------
// Courbe "aire" de la dernière heure, pensée pour un panneau LED :
//  - 1 colonne de 1 px = 1 minute (moyenne) : 53 minutes terminées + la minute en cours, à droite (x 55)
//  - chaque colonne prend la couleur du niveau de sa valeur : corps atténué, sommet à pleine intensité
//    -> le sommet dessine la courbe (trait vif), l'aire colorée montre d'un coup d'oeil les périodes à risque
//  - échelle automatique : de 400 ppm (air extérieur) à 1200 ppm au moins, agrandie par paliers de 200 ppm si
//    le pic de l'heure dépasse. La règle à gauche (x 0) donne la couleur des niveaux à chaque hauteur.
//  - pointillés à 1000 ppm (seuil AERER), visibles seulement là où la courbe est en dessous
//  - minute sans mesure (capteur indisponible) : colonne vide, l'axe du temps reste juste
//  - ligne de titre : CO2, durée couverte | flèche de tendance + valeur actuelle ;
//    le sommet de la minute en cours clignote en cyan (ANIMATIONS)
//   0..5 ligne de titre | 7..31 courbe, x 0..LARGEUR_BAS-1 (la zone des témoins est à droite)
static const int COURBE_MIN_PPM      = 400;    // bas de l'échelle
static const int COURBE_HAUT_MIN_PPM = 1200;   // haut de l'échelle, au minimum
static const int COURBE_PALIER_PPM   = 200;    // l'échelle s'agrandit par paliers
static const int COURBE_HAUT_MAX_PPM = 5000;
static const int COURBE_X0           = 2;                       // x 0 : règle | x 1 : vide | x 2.. : colonnes
static const int COURBE_NB_COL       = LARGEUR_BAS - COURBE_X0; // 54 colonnes (62 sans témoins)
static_assert(COURBE_NB_COL == HIST_N + 1, "la courbe affiche HIST_N minutes + la minute en cours");

// Valeur de la colonne col (0 = la plus à gauche, la dernière = minute en cours) ; 0 = pas de mesure
uint16_t valeurColonne(int col) {
  int depuisDroite = COURBE_NB_COL - 1 - col;
  if (depuisDroite == 0) return minuteEnCours();
  int i = (int)histN - depuisDroite;      // index dans l'historique (0 = le plus ancien)
  return (i >= 0) ? valeurHistorique(i) : 0;
}

// Hauteur (1..hMax pixels) d'une valeur sur l'échelle COURBE_MIN_PPM..haut
int hauteurCourbe(float v, int haut, int hMax) {
  int h = 1 + (int)((v - COURBE_MIN_PPM) * (hMax - 1) / (float)(haut - COURBE_MIN_PPM) + 0.5f);
  return constrain(h, 1, hMax);
}

void dessinerPageCourbe() {
  // --- valeur actuelle : la mesure en direct, sinon la dernière minute connue ---
  float actuel = (mCO2.etat == CAPTEUR_OK) ? mCO2.valeur : 0.0f;
  for (int i = (int)histN - 1; i >= 0 && actuel <= 0; i--) actuel = valeurHistorique(i);
  ecrireP("CO2", 0, 0, COL_TITRE);
  if (actuel <= 0) {
    ecrireCentre("ATTENTE", 28, 15, 1, COL_TITRE);
    return;
  }

  // --- ligne de titre : durée couverte, flèche de tendance, valeur actuelle ---
  char buf[12];
  snprintf(buf, sizeof(buf), "%d min", COURBE_NB_COL);
  ecrireP(buf, 14, 0, COL_DISCRET);
  int nvAct = niveau(actuel, ECH_CO2);
  snprintf(buf, sizeof(buf), "%d", (int)roundf(actuel));
  int xVal = ecrirePDroite(buf, 63, 0, colNiv[nvAct]);
  int dir;
  if (tendanceCO2(dir)) dessinerFleche(xVal - 4, 0, dir, couleurTendance(dir));   // juste à gauche de la valeur

  const int yBase = 31;
  const int hMax = yBase - 6;                         // zone de dessin : y 7..yBase (25 px)

  // --- valeurs des colonnes et échelle (agrandie si le pic de l'heure dépasse) ---
  uint16_t vals[COURBE_NB_COL];
  uint16_t maxi = 0;
  for (int c = 0; c < COURBE_NB_COL; c++) {
    vals[c] = valeurColonne(c);
    if (vals[c] > maxi) maxi = vals[c];
  }
  int haut = COURBE_HAUT_MIN_PPM;
  while (haut < maxi && haut < COURBE_HAUT_MAX_PPM) haut += COURBE_PALIER_PPM;

  // --- règle des niveaux (x 0) : la rangée de hauteur k a la couleur du niveau d'une colonne de hauteur k ---
  for (int y = yBase - hMax + 1; y <= yBase; y++) {
    float v = COURBE_MIN_PPM + (yBase - y) * (float)(haut - COURBE_MIN_PPM) / (hMax - 1);
    display.drawPixel(0, y, colNiv[niveau(v, ECH_CO2)]);
  }

  // --- pointillés à 1000 ppm (dessinés d'abord : les colonnes passent par-dessus) ---
  const int yAerer = yBase - hauteurCourbe(SEUIL_AERER_PPM, haut, hMax) + 1;
  const uint16_t cAerer = atten(colNiv[2], 110);
  for (int x = COURBE_X0 + 1; x < COURBE_X0 + COURBE_NB_COL; x += 3) display.drawPixel(x, yAerer, cAerer);

  // --- colonnes : corps atténué, sommet vif ---
  for (int c = 0; c < COURBE_NB_COL; c++) {
    if (vals[c] == 0) continue;                       // minute sans mesure
    int h = hauteurCourbe(vals[c], haut, hMax);
    int x = COURBE_X0 + c;
    int yT = yBase - h + 1;
    uint16_t col = colNiv[niveau(vals[c], ECH_CO2)];
    if (h > 1) display.drawFastVLine(x, yT + 1, h - 1, atten(col, 100));
    uint16_t cTop = col;
#if ANIMATIONS
    if (c == COURBE_NB_COL - 1 && phasePoint) cTop = COL_TITRE;  // minute en cours : sommet qui clignote
#endif
    display.drawPixel(x, yT, cTop);
  }
}

// ---------- Wi-Fi : logo, page, écran de connexion ----------
static const uint16_t COL_ETEINT = rgb565(40, 45, 60);   // arcs éteints du logo

// Logo Wi-Fi 13 x 11 : un point + 3 arcs. (cx, yBas) = centre du point, en bas du logo (arcs vers le haut).
// allumes = nombre d'arcs allumés (0..3), les autres restent sombres.
void dessinerLogoWifi(int cx, int yBas, int allumes, uint16_t cOn, uint16_t cOff) {
  for (int dy = -10; dy <= 0; dy++) {
    for (int dx = -7; dx <= 7; dx++) {
      int d2 = dx * dx + dy * dy;
      int arc = -1;
      if (d2 <= 2 && dy <= 0) arc = 0;                        // point (3 x 2)
      else if (abs(dx) <= -dy) {                              // cône à 45° vers le haut
        if (d2 >= 7 && d2 <= 17) arc = 1;
        else if (d2 >= 30 && d2 <= 48) arc = 2;
        else if (d2 >= 68 && d2 <= 95) arc = 3;
      }
      if (arc < 0) continue;
      display.drawPixel(cx + dx, yBas + dy, (arc <= allumes) ? cOn : cOff);
    }
  }
}

// Petite croix rouge (5 x 5) : pas de connexion
void dessinerCroix(int x, int y) {
  display.drawLine(x, y, x + 4, y + 4, COL_ERREUR);
  display.drawLine(x + 4, y, x, y + 4, COL_ERREUR);
}

// Force du signal : 3 = fort ... 0 = très faible
int niveauSignal(int rssi) {
  if (rssi >= -60) return 3;
  if (rssi >= -70) return 2;
  if (rssi >= -80) return 1;
  return 0;
}

uint16_t couleurSignal(int nv) {
  static const uint8_t idx[4] = {3, 2, 1, 0};  // 0 arc = rouge ... 3 arcs = vert
  return colNiv[idx[nv & 3]];
}

// Texte sur toute la largeur : centré s'il tient (10 caractères), sinon il défile en boucle.
bool texteDefile(const char* s) {
  return largeurTexte(s, 1) > 62;
}

void ecrireLigneDefilante(const char* s, int y, uint16_t c) {
  int w = largeurTexte(s, 1);
  if (w <= 62) { ecrireCentre(s, 32, y, 1, c); return; }
  int cycle = w + 19;                                     // texte + 3 espaces avant la répétition
  int dx = (int)((millis() / 60UL) % (unsigned long)cycle);  // 1 pixel toutes les 60 ms
  ecrireTexte(s, 1 - dx, y, 1, c);
  ecrireTexte(s, 1 - dx + cycle, y, 1, c);
}

// Ligne de titre des écrans Wi-Fi (même style que les autres pages) : "WIFI" à gauche, état à droite
void dessinerEnteteWifi(const char* droite, uint16_t colDroite) {
  ecrireP("WIFI", 0, 0, COL_TITRE);
  if (droite) ecrirePDroite(droite, 63, 0, colDroite);
}

// Page de la rotation : état du Wi-Fi
//   0..5 "WIFI" + force en dBm | logo 13x11 (y 9..19) | à droite : état en 5x7 (y 9) + détail en petit (y 18)
//   25..31 nom du réseau (défile s'il est trop long)
void dessinerPageWifi() {
#if WIFI_ACTIVE
  if (reseauOK()) {
    int rssi = WiFi.RSSI();
    int nv = niveauSignal(rssi);
    uint16_t c = couleurSignal(nv);
    static const char* QUALITE[4] = {"FAIBLE", "MOYEN", "BON", "FORT"};
    char buf[8];
    snprintf(buf, sizeof(buf), "%d dBm", rssi);
    dessinerEnteteWifi(buf, c);
    dessinerLogoWifi(7, 19, nv, c, COL_ETEINT);
    ecrireTexte(QUALITE[nv], 17, 9, 1, c);
    ecrireP(typeReseau(reseauCourant), 17, 18, COL_DISCRET);
    ecrireLigneDefilante(WiFi.SSID().c_str(), 25, COL_TITRE);
    return;
  }
  dessinerEnteteWifi("HORS LIGNE", COL_ERREUR);
  dessinerLogoWifi(7, 19, -1, COL_ETEINT, COL_ETEINT);
  dessinerCroix(10, 15);
  if (nbReseaux == 0) {
    ecrireTexte("AUCUN", 17, 9, 1, COL_ERREUR);
    ecrireP("RESEAU", 17, 18, COL_DISCRET);
    ecrireLigneDefilante("tape: wifi add <nom> <mdp>", 25, COL_TITRE);
    return;
  }
  const char* etat = (etatWifi == WIFI_SCAN) ? "RECHER" : (etatWifi == WIFI_CONNEXION) ? "CONNEX" : "ABSENT";
  ecrireTexte(etat, 17, 9, 1, COL_TITRE);
  char mem[16];
  snprintf(mem, sizeof(mem), "%u CONNUS", (unsigned)nbReseaux);
  ecrireP(mem, 17, 18, COL_DISCRET);
  if (etatWifi == WIFI_CONNEXION && reseauCourant >= 0) ecrireLigneDefilante(reseaux[reseauCourant].ssid.c_str(), 25, COL_TITRE);
  else ecrireLigneDefilante("HORS LIGNE", 25, COL_DISCRET);
#else
  dessinerEnteteWifi("OFF", COL_DISCRET);
  dessinerLogoWifi(7, 19, -1, COL_ETEINT, COL_ETEINT);
  ecrireTexte("DESACT.", 17, 9, 1, COL_DISCRET);
#endif
}

// L'écran de connexion recouvre les pages : pendant une connexion "interactive"
// (démarrage, commande wifi) et quelques secondes pour annoncer le résultat.
bool ecranWifiActif() {
#if WIFI_ACTIVE
  if (resultatWifiAffiche) return true;
  return wifiInteractif && (etatWifi == WIFI_SCAN || etatWifi == WIFI_CONNEXION);
#else
  return false;
#endif
}

void dessinerEcranConnexion() {
#if WIFI_ACTIVE
  unsigned long now = millis();
  if (resultatWifiAffiche) {
    if (resultatWifiOK) {                                 // connecté : logo plein, "OK" en grand
      int rssi = WiFi.RSSI();
      char buf[8];
      snprintf(buf, sizeof(buf), "%d dBm", rssi);
      dessinerEnteteWifi(buf, couleurSignal(niveauSignal(rssi)));
      dessinerLogoWifi(7, 19, 3, colNiv[0], COL_ETEINT);
      ecrireGrand("OK", 40, 21, colNiv[0], &FreeSansBold9pt7b);
      ecrireLigneDefilante(WiFi.SSID().c_str(), 25, COL_TITRE);
    } else {                                              // échec : logo éteint + croix, raison
      dessinerEnteteWifi("ECHEC", COL_ERREUR);
      dessinerLogoWifi(7, 19, -1, COL_ETEINT, COL_ETEINT);
      dessinerCroix(10, 15);
      ecrireTexte(nbCandidats ? raisonCourte(raisonDeco) : "ABSENT", 17, 9, 1, COL_ERREUR);
      ecrireP("ESSAI 30 S", 17, 18, COL_DISCRET);
      ecrireLigneDefilante("wifi list / wifi add", 25, COL_DISCRET);
    }
    return;
  }
  // Connexion en cours : les arcs s'allument l'un après l'autre
  int phase = (int)((now / 300UL) % 4UL) - 1;            // -1 (point seul) .. 2
  if (etatWifi == WIFI_SCAN) {
    dessinerEnteteWifi("RECHERCHE", COL_TITRE);
    dessinerLogoWifi(7, 19, phase + 1, COL_TITRE, COL_ETEINT);
    ecrireTexte("RECHER", 17, 9, 1, COL_TITRE);
    char mem[16];
    snprintf(mem, sizeof(mem), "%u CONNUS", (unsigned)nbReseaux);
    ecrireLigneDefilante(mem, 25, COL_DISCRET);
  } else {
    char buf[12];
    snprintf(buf, sizeof(buf), "ESSAI %u/%u", (unsigned)(idxCandidat + 1), (unsigned)nbCandidats);
    dessinerEnteteWifi(buf, COL_TITRE);
    dessinerLogoWifi(7, 19, phase + 1, COL_TITRE, COL_ETEINT);
    ecrireTexte("CONNEX", 17, 9, 1, COL_TITRE);
    if (reseauCourant >= 0) ecrireLigneDefilante(reseaux[reseauCourant].ssid.c_str(), 25, COL_TITRE);
  }
  // points animés + secondes écoulées (y 18..22)
  int nbPoints = (int)((now / 400UL) % 4UL);
  for (int i = 0; i < nbPoints; i++) display.fillRect(17 + i * 4, 20, 2, 2, COL_TITRE);
  char sec[8];
  snprintf(sec, sizeof(sec), "%lu s", (now - tEtatWifi) / 1000UL);
  ecrirePDroite(sec, 63, 18, COL_DISCRET);
#endif
}

// ---------- pages : sélection ----------
bool pageActive(uint8_t p) {
  if (p == PAGE_AERER) return alerteAerer();   // seulement quand le CO2 est élevé
  if (p == PAGE_COURBE) return histN >= 2;     // dès 2 minutes de mesures
  if (p == PAGE_WIFI) return WIFI_ACTIVE;
  return true;
}

// ---------- alertes hors norme ----------
// Index des alertes : 0 CO2 | 1 PM1 | 2 PM2.5 | 3 PM10. Page affichée pour chacune (le CO2 saute sur AERER).
static const uint8_t PAGE_ALERTE[4] = {PAGE_AERER, PAGE_PM1, PAGE_PM25, PAGE_PM10};
uint8_t dernierePageAlerte = PAGE_AERER;   // tourniquet entre plusieurs pages en alerte
uint8_t curseurRotation = PAGE_CO2;        // dernière page « normale » montrée (la rotation reprend là)

// La page p montre-t-elle un polluant actuellement en alerte ?
bool alertePage(uint8_t p) {
  for (uint8_t i = 0; i < 4; i++) if (PAGE_ALERTE[i] == p && alertes[i].active) return true;
  return false;
}

bool alerteEnCours() {
  for (uint8_t i = 0; i < 4; i++) if (alertes[i].active && pageActive(PAGE_ALERTE[i])) return true;
  return false;
}

bool flashEnCours() {
  return tFlashDebut != 0 && millis() - tFlashDebut < ALERTE_FLASH_MS * ALERTE_NB_FLASH;
}

// Changement BRUTAL : la page du polluant s'affiche tout de suite (pas de fondu), et l'écran clignote plein écran.
void declencherAlerte(uint8_t i, int niv, const char* nom, unsigned long now) {
  alertes[i].tFlash = now;
  pageCourante = PAGE_ALERTE[i];
  dernierePageAlerte = pageCourante;
  tPage = now;                  // la page reste affichée une durée complète
  aTransition = false;          // pas de fondu : le changement doit sauter aux yeux
  if (!ecranEteint) {
    tFlashDebut = now;
    if (tFlashDebut == 0) tFlashDebut = 1;
    couleurFlash = colNiv[niv];
    nomFlash = nom;
  }
  aRedessiner = true;
  LOG("ALERTE %s : niveau %d (%s)\n", nom, niv, LABELS_NIVEAU[niv]);
}

// Appelée après chaque lecture des capteurs (toutes les 10 s).
// Une alerte se déclenche quand un polluant FRANCHIT son seuil, ou quand il EMPIRE pendant l'alerte.
// Anti-rebond : une valeur qui oscille autour du seuil ne refait pas clignoter l'écran plus d'une fois par minute.
// PM1 n'a pas de seuil réglementaire : il ne déclenche pas d'alerte (PM2.5 >= PM1, donc PM2.5 alerte avant lui).
void verifierAlertes(unsigned long now) {
  const Mesure* m[4] = {&mCO2, &mPM1, &mPM25, &mPM10};
  const Echelle* e[4] = {&ECH_CO2, &ECH_PM25, &ECH_PM25, &ECH_PM10};
  static const char* NOMS[4] = {"CO2", "PM1", "PM2.5", "PM10"};
  int choix = -1, nivChoix = -1;
  for (uint8_t i = 0; i < 4; i++) {
    EtatAlerte &a = alertes[i];
    int niv = niveauMesure(*m[i], *e[i]);
    int seuil = (i == 0) ? NIVEAU_ALERTE_CO2 : NIVEAU_ALERTE_PM;
    if (i == 1 || niv < seuil) { a.active = false; a.nivMax = -1; continue; }
    bool empire = a.active && niv > a.nivMax;
    bool entree = !a.active;
    a.active = true;
    if (niv > a.nivMax) a.nivMax = niv;
    bool rearme = (a.tFlash == 0) || (now - a.tFlash >= ALERTE_REARME_MS);
    if ((entree && rearme) || empire) {
      if (niv > nivChoix) { choix = i; nivChoix = niv; }   // plusieurs à la fois : le plus grave d'abord
    }
  }
  if (choix >= 0) declencherAlerte(choix, nivChoix, NOMS[choix], now);
}

#if AFFICHER_TEMOINS
// Zone d'état en bas à droite (x 57..63, y 25..31), que les pages ne recouvrent jamais :
//
//   y 25..29   mini logo Wi-Fi 7x5 : point + 2 arcs
//                connecté   : couleur = force du signal (vert fort, jaune, orange, rouge faible),
//                             arcs allumés : point seul (très faible) / + petit arc / + grand arc
//                recherche  : arcs éteints, point cyan
//                hors ligne : arcs éteints, point rouge
//   y 31       3 repères :  x 57-58 alimentation (vert secteur | orange batterie)
//                           x 60    heure NTP    (bleu réglée | gris pas encore)
//                           x 62-63 envoi        (vert OK | orange mesures en attente ou réessai | rouge refus | gris rien)
static const char* WIFI_MINI[5] = {   // '1' point, '2' petit arc, '3' grand arc
  ".33333.",
  "3.....3",
  "..222..",
  ".2...2.",
  "...1...",
};
static const int X_TEMOINS = 57, Y_TEMOINS = 25;

void dessinerMiniWifi() {
  uint8_t allumes = 0;            // nombre d'éléments allumés : 1 = point, 2 = + petit arc, 3 = + grand arc
  uint16_t c = COL_ERREUR;
#if WIFI_ACTIVE
  if (reseauOK()) {
    int nv = niveauSignal(WiFi.RSSI());
    c = couleurSignal(nv);
    allumes = (nv >= 2) ? 3 : nv + 1;
  } else {
    allumes = 1;
    c = (etatWifi == WIFI_SCAN || etatWifi == WIFI_CONNEXION) ? COL_TITRE : COL_ERREUR;
  }
#endif
  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 7; x++) {
      char e = WIFI_MINI[y][x];
      if (e == '.') continue;
      display.drawPixel(X_TEMOINS + x, Y_TEMOINS + y, (e - '0' <= allumes) ? c : COL_ETEINT);
    }
  }
}

void dessinerTemoins() {
  static const uint16_t GRIS = rgb565(50, 50, 50), VERT = rgb565(0, 200, 0), ORANGE = rgb565(255, 120, 0);
  dessinerMiniWifi();
  uint16_t cAlim = estSurSecteur ? VERT : ORANGE;
  uint16_t cNtp  = heureValide() ? rgb565(0, 100, 255) : GRIS;
  uint16_t cEnv  = GRIS;
  if (etatEnvoi == 3) cEnv = COL_ERREUR;
  else if (etatEnvoi == 2 || nbLignesAttente > 0) cEnv = ORANGE;
  else if (etatEnvoi == 1) cEnv = VERT;
  display.drawFastHLine(X_TEMOINS, 31, 2, cAlim);
  display.drawPixel(X_TEMOINS + 3, 31, cNtp);
  display.drawFastHLine(X_TEMOINS + 5, 31, 2, cEnv);
}
#endif

// ---------- rendu ----------
// Dessine la page courante dans le tampon (sans l'envoyer à l'écran)
void dessinerContenu() {
  display.clearDisplay();
  display.setTextWrap(false);

  if (ecranWifiActif()) {   // écran de connexion : il a déjà son grand logo Wi-Fi, pas de témoins
    dessinerEcranConnexion();
    return;
  }

  // Éclair d'alerte : une image sur deux, tout l'écran à la couleur du niveau, nom du polluant en noir
  if (flashEnCours() && (((millis() - tFlashDebut) / ALERTE_FLASH_MS) % 2UL) == 0) {
    display.fillScreen(couleurFlash);
    ecrireGrand(nomFlash, 32, 22, 0, &FreeSansBold9pt7b);
    return;
  }

  switch (pageCourante) {
    case PAGE_CO2:    dessinerPageCO2(); break;
    case PAGE_AERER:  dessinerPageAerer(); break;
    case PAGE_PM1:    dessinerPagePM(mPM1,  "PM1",   ECH_PM25, PAGE_PM1);  break;   // échelle PM2.5, indicative
    case PAGE_PM25:   dessinerPagePM(mPM25, "PM2.5", ECH_PM25, PAGE_PM25); break;
    case PAGE_PM10:   dessinerPagePM(mPM10, "PM10",  ECH_PM10, PAGE_PM10); break;
    case PAGE_GLOBAL: dessinerPageGlobal(); break;
    case PAGE_COURBE: dessinerPageCourbe(); break;
    case PAGE_WIFI:   dessinerPageWifi(); break;
    default: break;
  }
#if AFFICHER_TEMOINS
  if (pageCourante != PAGE_WIFI) dessinerTemoins();   // la page Wi-Fi a déjà son grand logo (et un texte qui défile en bas)
#endif
}

// Envoie le tampon dessiné à l'écran (double tampon : bascule instantanée, sans scintillement)
void presenter() {
#if DOUBLE_BUFFER
  display.showBuffer();
#endif
}

// Transition entre deux pages : fondu au noir puis retour, en jouant sur la LUMINOSITÉ de tout l'écran
// (aucun trait ni balayage de couleur). 4 paliers de 18 ms dans chaque sens : ~150 ms en tout.
// sortie = true : la page affichée s'éteint ; false : la nouvelle page s'allume.
void fondu(bool sortie) {
#if TRANSITION_FONDU
  for (int k = 1; k <= 4; k++) {
    int palier = sortie ? 4 - k : k;                  // sortie : 3, 2, 1, 0 | entrée : 1, 2, 3, 4
    display.setBrightness((uint16_t)luminositeCourante * palier / 4);
    delay(18);
  }
#endif
}

void afficher() {
  // Écran éteint (mode nuit) : on affiche du noir.
  if (ecranEteint) { display.clearDisplay(); presenter(); return; }
  if (ecranWifiActif()) aTransition = false;  // pas de fondu sur l'écran de connexion
  if (aTransition) {
    aTransition = false;
    fondu(true);
    dessinerContenu();
    presenter();
    fondu(false);
    return;
  }
  dessinerContenu();
  presenter();
}

// Passe à la page suivante qui est active (saute AERER et COURBE quand elles n'ont rien à montrer)
// et prépare le fondu.
// Pendant une alerte : une page sur deux est une page EN ALERTE (tourniquet s'il y en a plusieurs) ; entre deux,
// la rotation normale reprend là où elle s'était arrêtée, pour que les autres mesures restent visibles.
void pageSuivante() {
  bool enAlerte = alerteEnCours();
  bool trouvee = false;
  if (enAlerte && !alertePage(pageCourante)) {
    for (uint8_t i = 1; i <= NB_PAGES && !trouvee; i++) {
      uint8_t p = (dernierePageAlerte + i) % NB_PAGES;
      if (alertePage(p) && pageActive(p)) { pageCourante = dernierePageAlerte = p; trouvee = true; }
    }
  }
  for (uint8_t i = 0; i < NB_PAGES && !trouvee; i++) {
    curseurRotation = (curseurRotation + 1) % NB_PAGES;
    if (pageActive(curseurRotation) && !(enAlerte && alertePage(curseurRotation))) {
      pageCourante = curseurRotation;
      trouvee = true;
    }
  }
  aTransition = true;
  aRedessiner = true;
}

// ---------- animations ----------
// La page courante affiche-t-elle cette mesure ? (pour ne redessiner que si ce qui bouge est visible)
bool mesureVisible(const Mesure &m) {
  if (pageCourante == PAGE_CO2)  return &m == &mCO2;
  if (pageCourante == PAGE_PM1)  return &m == &mPM1;
  if (pageCourante == PAGE_PM25) return &m == &mPM25;
  if (pageCourante == PAGE_PM10) return &m == &mPM10;
  return false;
}

// L'écran de connexion ou la page Wi-Fi bougent-ils ? (logo animé, nom de réseau qui défile)
bool ecranWifiAnime() {
  if (ecranWifiActif()) return true;
  if (pageCourante != PAGE_WIFI) return false;
#if WIFI_ACTIVE
  if (reseauOK()) return texteDefile(WiFi.SSID().c_str());
  if (nbReseaux == 0) return true;
  if (etatWifi == WIFI_CONNEXION && reseauCourant >= 0) return texteDefile(reseaux[reseauCourant].ssid.c_str());
#endif
  return false;
}

// Appelée à chaque tour de loop() : une image d'animation toutes les 40 ms au plus, et on ne
// redessine l'écran que si quelque chose a réellement bougé sur la page affichée.
void animer(unsigned long now) {
  if (ecranEteint) return;
  if (now - tAnim < ANIM_PAS_MS) return;
  tAnim = now;
  bool maj = false;
  bool pageVisible = !ecranWifiActif();

  // écran de connexion / page Wi-Fi : logo animé, texte défilant, secondes (une image toutes les 60 ms)
  if (ecranWifiAnime()) {
    static uint32_t phaseWifi = 0;
    uint32_t ph = (uint32_t)(now / 60UL);
    if (ph != phaseWifi) { phaseWifi = ph; maj = true; }
  }

  // éclair d'alerte : une image toutes les ALERTE_FLASH_MS, puis retour à la page normale
  if (tFlashDebut != 0) {
    static uint32_t imgFlash = 0xFFFFFFFF;
    if (flashEnCours()) {
      uint32_t img = (uint32_t)((now - tFlashDebut) / ALERTE_FLASH_MS);
      if (img != imgFlash) { imgFlash = img; maj = true; }
    } else {
      tFlashDebut = 0;
      imgFlash = 0xFFFFFFFF;
      maj = true;
    }
  }

  // page PM en alerte : la valeur pulse (même sans ANIMATIONS : c'est une alerte)
  if (pageVisible && pageCourante != PAGE_AERER && alertePage(pageCourante)) {
    static uint32_t phasePulse = 0;
    uint32_t ph = (uint32_t)(now / ALERTE_PULSE_MS);
    if (ph != phasePulse) { phasePulse = ph; maj = true; }
  }

  // page AERER : "AERER" pulse (vif / atténué toutes les 0,6 s), même sans ANIMATIONS : c'est une alerte
  if (pageVisible && pageCourante == PAGE_AERER) {
    static uint32_t phaseAerer = 0;
    uint32_t ph = (uint32_t)(now / 600UL);
    if (ph != phaseAerer) { phaseAerer = ph; maj = true; }
  }

#if ANIMATIONS
  // chiffres qui glissent en douceur vers la nouvelle valeur
  Mesure* tab[4] = {&mCO2, &mPM1, &mPM25, &mPM10};
  for (uint8_t i = 0; i < 4; i++) {
    Mesure &m = *tab[i];
    if (m.etat != CAPTEUR_OK) continue;
    if (!m.affInit) {
      m.aff = m.valeur;
      m.affInit = true;
      if (pageVisible && mesureVisible(m)) maj = true;
      continue;
    }
    float d = m.valeur - m.aff;
    if (d == 0.0f) continue;
    if (fabsf(d) < 0.05f) m.aff = m.valeur;
    else                  m.aff += d * 0.3f;
    if (pageVisible && mesureVisible(m)) maj = true;
  }

  // clignement des yeux (seule la page QUALITE AIR a un visage)
  bool cl = yeuxFermes();
  if (cl != etatCligne) {
    etatCligne = cl;
    if (pageVisible && pageCourante == PAGE_GLOBAL) maj = true;
  }

  // page courbe : le sommet de la minute en cours clignote (500 ms)
  if (pageCourante == PAGE_COURBE) {
    uint8_t p = (uint8_t)((now / 500UL) % 2UL);
    if (p != phasePoint) { phasePoint = p; maj = true; }
  }
#endif

  // préchauffage du CO2 : le compte à rebours change chaque seconde
  if (pageVisible && pageCourante == PAGE_CO2 && mCO2.etat == CAPTEUR_CHAUFFE) {
    uint32_t ph = (uint32_t)(now / 1000UL);
    if (ph != phaseChauffe) { phaseChauffe = ph; maj = true; }
  }

  if (maj) aRedessiner = true;
}

// ---------- écran d'accueil : logo "LA BONNE M'AIR" ----------
// La "Bonne Mère" (Notre-Dame-de-la-Garde) stylisée en pixel art, à gauche (x 0..14, toute la hauteur) :
// statue dorée au sommet du clocher, pierre rayée gris-bleu / vert comme les façades de la basilique, colline.
// À droite : "LA BONNE" (y 1..7), "M'AIR" en grand (y 10..23, "AIR" en vert = air sain),
// et la barre des 6 niveaux de qualité de l'air (y 27..29), aux couleurs des niveaux.
//   G statue (or) | S pierre | V bande verte | H colline | h colline sombre | . rien (noir)
static const char* LOGO_BASILIQUE[32] = {
  ".......G.......",  //  0  tête de la statue
  "......GGG......",  //  1
  ".....GGGG......",  //  2  l'enfant dans les bras
  "......GGG......",  //  3
  "......GGG......",  //  4
  ".....GGGGG.....",  //  5  robe
  "......SSS......",  //  6  socle
  ".....SSSSS.....",  //  7
  ".....S.S.S.....",  //  8  petites baies
  ".....S.S.S.....",  //  9
  ".....SSSSS.....",  // 10
  "....SSSSSSS....",  // 11  corniche du clocher
  "....SVVVVVS....",  // 12
  "....S..S..S....",  // 13  grandes baies du clocher
  "....S..S..S....",  // 14
  "....S..S..S....",  // 15
  "....SSSSSSS....",  // 16
  "....SVVVVVS....",  // 17  pierre rayée
  "....SSSSSSS....",  // 18
  "....SVVVVVS....",  // 19
  "..SSSSSSSSSSS..",  // 20  toit de la basilique
  "..SVVVVVVVVVS..",  // 21
  "..SS.SSSSS.SS..",  // 22  fenêtres
  "..SV.VVVVV.VS..",  // 23
  "..SSSSSSSSSSS..",  // 24
  ".SSSSSSSSSSSSS.",  // 25  terrasse
  ".HHHHHHHHHHHHH.",  // 26  colline
  "HHHHHHHHHHHHHHH",  // 27
  "hhhhhhhhhhhhhhh",  // 28
  "hhhhhhhhhhhhhhh",  // 29
  "hhhhhhhhhhhhhhh",  // 30
  "hhhhhhhhhhhhhhh",  // 31
};

// Dessine le logo dans le tampon, partiellement selon l'étape de l'animation :
//   rangs   : nombre de rangées de la basilique visibles, en partant du bas (0..32)
//   lettres : "LA BONNE" puis "M'AIR" écrits lettre à lettre (0..13)
//   niveaux : segments de la barre allumés (0..NB_NIVEAUX)
//   eclat   : petit scintillement à côté de la statue
void dessinerLogo(int rangs, int lettres, int niveaux, bool eclat) {
  static const uint16_t C_OR = rgb565(255, 170, 0), C_PIERRE = rgb565(90, 150, 175), C_VERT = rgb565(0, 150, 90);
  static const uint16_t C_COLLINE = rgb565(0, 140, 40), C_COLLINE_SOMBRE = rgb565(0, 70, 20);
  display.clearDisplay();
  display.setTextWrap(false);

  // basilique, révélée du bas vers le haut
  for (int y = 32 - rangs; y < 32; y++) {
    for (int x = 0; x < 15; x++) {
      uint16_t c;
      switch (LOGO_BASILIQUE[y][x]) {
        case 'G': c = C_OR; break;
        case 'S': c = C_PIERRE; break;
        case 'V': c = C_VERT; break;
        case 'H': c = C_COLLINE; break;
        case 'h': c = C_COLLINE_SOMBRE; break;
        default: continue;
      }
      display.drawPixel(x, y, c);
    }
  }
  if (eclat) {                                           // étoile à 4 branches à droite de la statue
    uint16_t ce = rgb565(255, 240, 120);
    display.drawPixel(11, 1, ce);
    display.drawPixel(10, 1, C_OR); display.drawPixel(12, 1, C_OR);
    display.drawPixel(11, 0, C_OR); display.drawPixel(11, 2, C_OR);
  }

  // "LA BONNE" (8 caractères, x 16..62)
  char ligne1[9];
  int n1 = constrain(lettres, 0, 8);
  memcpy(ligne1, "LA BONNE", n1);
  ligne1[n1] = '\0';
  ecrireTexte(ligne1, 16, 1, 1, COL_TITRE);

  // "M'AIR" en taille 2, espacé à la main pour tenir dans x 16..63 (5 caractères normaux feraient 58 px)
  int n2 = lettres - 8;
  if (n2 >= 1) ecrireTexte("M", 16, 10, 2, COL_TITRE);             // x 16..25
  if (n2 >= 2) {                                                   // apostrophe : x 27..28, y 10..13
    display.fillRect(27, 10, 2, 3, COL_TITRE);
    display.drawPixel(27, 13, COL_TITRE);
  }
  if (n2 >= 3) ecrireTexte("A", 30, 10, 2, colNiv[0]);             // x 30..39
  if (n2 >= 4) ecrireTexte("I", 42, 10, 2, colNiv[0]);             // x 44..49 (glyphe étroit)
  if (n2 >= 5) ecrireTexte("R", 54, 10, 2, colNiv[0]);             // x 54..63

  // barre des niveaux (x 16..63, y 27..29)
  for (int i = 0; i < NB_NIVEAUX; i++) {
    int xs = 16 + (i * 49) / NB_NIVEAUX;
    int xe = 16 + ((i + 1) * 49) / NB_NIVEAUX - 2;
    display.fillRect(xs, 27, xe - xs + 1, 3, (i < niveaux) ? colNiv[i] : colFond[i]);
  }
}

// Animation de démarrage (~2,7 s) : la basilique monte, le nom s'écrit, la barre se remplit, la statue scintille.
void afficherSplash() {
  for (int r = 2; r <= 32; r += 2) { dessinerLogo(r, 0, 0, false); presenter(); delay(25); }
  for (int l = 1; l <= 13; l++)    { dessinerLogo(32, l, 0, false); presenter(); delay(l <= 8 ? 45 : 90); }
  for (int n = 1; n <= NB_NIVEAUX; n++) { dessinerLogo(32, 13, n, false); presenter(); delay(100); }
  for (int k = 0; k < 4; k++)      { dessinerLogo(32, 13, NB_NIVEAUX, k % 2 == 0); presenter(); delay(130); }
  delay(500);
}

// Choisit la luminosité : réduite la nuit et sur batterie ; peut aussi éteindre l'écran (ECRAN_ETEINT_NUIT).
void majLuminosite() {
  bool nuit = estNuit();
  uint8_t lum = nuit ? LUMINOSITE_NUIT : LUMINOSITE_JOUR;
  if (!estSurSecteur && lum > LUMINOSITE_BATTERIE) lum = LUMINOSITE_BATTERIE;  // économie sur batterie
  bool eteint = (ECRAN_ETEINT_NUIT && nuit);
  if (eteint != ecranEteint) { ecranEteint = eteint; aRedessiner = true; }
  luminositeCourante = lum;
  display.setBrightness(lum);
}

// =====================================================================
// ALIMENTATION SECTEUR, BOUTON, BUZZER
// =====================================================================
void surveillerSecteur(unsigned long now) {
  if (!DETECTION_SECTEUR_ACTIVE) return;

  // Lit la broche du pont diviseur : 1 = le secteur alimente la carte, 0 = elle est sur batterie.
  bool lecture = digitalRead(PIN_SECTEUR);
  // Anti-rebond : un changement n'est validé que s'il dure au moins 500 ms (évite les fausses alertes).
  if (lecture != etatCandidat) { etatCandidat = lecture; tChangementSecteur = now; }
  if (etatCandidat != estSurSecteur && now - tChangementSecteur >= ANTIREBOND_SECTEUR_MS) {
    estSurSecteur = etatCandidat;
    Serial.println(estSurSecteur ? "RETOUR DU COURANT" : "COUPURE DE COURANT : passage sur batterie");
    majLuminosite();
    aRedessiner = true;
  }
}

#if PIN_BOUTON >= 0
bool boutonAppuye = false;
unsigned long tAppui = 0;
// Bouton optionnel : appui court = page suivante, appui long (>= 1,5 s) = mesure envoyée tout de suite.
void surveillerBouton(unsigned long now) {
  bool appuye = (digitalRead(PIN_BOUTON) == LOW);
  if (appuye && !boutonAppuye) {
    boutonAppuye = true;
    tAppui = now;
  } else if (!appuye && boutonAppuye) {
    boutonAppuye = false;
    unsigned long duree = now - tAppui;
    if (duree >= 1500) {  // appui long : on enregistre la mesure tout de suite
      Serial.println("Bouton (appui long) : mesure immédiate.");
      tPeriode = now;
      finaliserPeriode();
    } else if (duree >= 50) {  // appui court : page suivante
      tPage = now;
      pageSuivante();
    }
  }
}
#endif

#if PIN_BUZZER >= 0
// Buzzer optionnel : un bip si le CO2 dépasse le seuil d'alarme, au plus toutes les 5 min, jamais la nuit.
void gererBuzzer(unsigned long now) {
  if (mCO2.etat != CAPTEUR_OK || mCO2.valeur < SEUIL_ALARME_PPM) return;
  if (estNuit()) return;  // pas de bip la nuit
  if (tDernierBip != 0 && now - tDernierBip < BUZZER_PAUSE_MS) return;
  tDernierBip = now;
  digitalWrite(PIN_BUZZER, HIGH);
  delay(200);
  digitalWrite(PIN_BUZZER, LOW);
}
#endif

// =====================================================================
// COMMANDES SÉRIE (moniteur série)
// =====================================================================
// Affiche la liste des commandes disponibles dans le moniteur série.
void afficherAide() {
  Serial.println("Commandes disponibles :");
  Serial.println("  status                état de la carte (réseau, file d'attente, clé...)");
  Serial.println("  wifi list             réseaux Wi-Fi en mémoire (5 au maximum)");
  Serial.println("  wifi add <nom> [mdp]  ajoute / modifie un Wi-Fi classique (sans mdp = réseau ouvert)");
  Serial.println("  wifi ent <nom> <identifiant> <mdp>   ajoute un WPA2 Entreprise (ex. eduroam)");
  Serial.println("                        nom avec espaces : entre guillemets, ex. wifi add \"Box de Paul\" mdp");
  Serial.println("  wifi del <n>          supprime le réseau n (numéro de 'wifi list')");
  Serial.println("  wifi scan             liste les réseaux visibles (* = connu)");
  Serial.println("  wifi connect          relance la connexion tout de suite");
  Serial.println("  set cle <clé>         clé du groupe AirCarto (32 caractères)");
  Serial.println("  reboot                redémarre la carte");
  Serial.println("  verbose on|off        active/coupe l'affichage automatique des mesures");
  Serial.println("  calib oui             calibrage du zéro CO2 (seulement après 20 min à l'air libre)");
  Serial.println("  vider_file            supprime les mesures stockées en attente");
  Serial.println("  oublier               efface identifiants et clé de la carte");
  Serial.println("Saisie : Retour arrière = efface 1 caractère | Ctrl+U = efface toute la ligne");
}

// Résumé de l'état de la carte (réseau, clé, file d'attente...) à la demande, commande « status ».
void afficherStatus() {
  Serial.println("--------- STATUS ---------");
  Serial.printf("device_id : %s\n", deviceId.c_str());
  Serial.printf("redémarrage : cause %d | boot %u\n", (int)esp_reset_reason(), (unsigned)bootId);
  Serial.printf("uptime / mémoire : %lu s | %u octets libres\n", millis() / 1000UL, (unsigned)ESP.getFreeHeap());
  if (reseauOK()) {
    Serial.printf("WiFi : connecté à %s (%s) | IP %s | %d dBm\n", WiFi.SSID().c_str(), typeReseau(reseauCourant),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!WIFI_ACTIVE) {
    Serial.println("WiFi : désactivé (WIFI_ACTIVE 0)");
  } else {
    static const char* ETATS[] = {"aucun réseau en mémoire", "recherche", "connexion en cours", "connecté", "en pause"};
    Serial.printf("WiFi : hors ligne (%s) | dernière raison : %s\n", ETATS[etatWifi], raisonLongue(raisonDeco));
  }
  Serial.printf("réseaux en mémoire : %u / %u | coupures WiFi : %u\n", (unsigned)nbReseaux, (unsigned)NB_WIFI_MAX,
                (unsigned)nbCoupuresWifi);
  Serial.printf("heure NTP : %s | mode nuit : %s\n", heureValide() ? "valide" : "pas encore", estNuit() ? "oui" : "non");
  Serial.printf("clé du groupe : %s%s\n", cleOK() ? "présente (32 car.)" : "ABSENTE ou mauvaise longueur",
                cleInvalide ? " | REFUSÉE par le serveur" : "");
  Serial.printf("dernier envoi : code HTTP %d | état %u | écart %lu ms (dont marge %lu ms)\n", dernierCodeHttp,
                (unsigned)etatEnvoi, ecartCourantMs(), margePushMs);
  Serial.printf("file d'attente : %u mesures (%u octets de fichier)", (unsigned)nbLignesAttente, (unsigned)tailleBacklog);
  if (nbLignesAttente > 0 && reseauOK()) {
    // créneaux libres pour le retard = 1 envoi / écart, moins les mesures fraîches (1 / période)
    float parS = 1000.0f / ecartCourantMs() - 1000.0f / periodeMesureMs();
    if (parS > 0) Serial.printf(" | rattrapé dans ~%u min", (unsigned)(nbLignesAttente / parS / 60.0f + 0.99f));
  }
  Serial.println();
  Serial.printf("période de mesure : %lu s%s\n", periodeMesureMs() / 1000UL,
                (estSurSecteur && periodeMesureMs() == PERIODE_MESURE_RATTRAPAGE_MS) ? " (rattrapage du retard)" : "");
  Serial.printf("alimentation : %s\n", estSurSecteur ? "secteur" : "batterie");
  Serial.printf("affichage détaillé : %s\n", verbose ? "oui" : "non");
}

void definirParametre(const String &nom, const String &valeur) {
  if (nom == "ssid" || nom == "wifi_pwd" || nom == "eap_id" || nom == "eap_pwd") {
    Serial.println("Commande remplacée par le cache Wi-Fi :");
    Serial.println("  wifi ent eduroam <identifiant> <mdp>   ou   wifi add <nom> <mdp>   (tape 'aide')");
    return;
  } else if (nom == "cle") {
    definirCle(valeur); prefs.putString("cle", valeur);
    cleInvalide = false;  // nouvelle clé : les envois reprennent
    if (!cleOK()) Serial.printf("Attention : la clé fait %u caractères (32 attendus).\n", (unsigned)valeur.length());
  } else {
    Serial.println("Paramètre inconnu. Tape 'aide'.");
    return;
  }
  // on n'affiche jamais la valeur : ce sont des secrets
  Serial.printf("%s enregistré (%u caractères).\n", nom.c_str(), (unsigned)valeur.length());
}

// Mot suivant de s à partir de pos ; "entre guillemets" pour un nom qui contient des espaces.
String motSuivant(const String &s, int &pos) {
  int len = s.length();
  while (pos < len && s[pos] == ' ') pos++;
  if (pos >= len) return "";
  int fin;
  String mot;
  if (s[pos] == '"') {
    fin = s.indexOf('"', pos + 1);
    if (fin < 0) fin = len;
    mot = s.substring(pos + 1, fin);
    pos = (fin < len) ? fin + 1 : len;
  } else {
    fin = s.indexOf(' ', pos);
    if (fin < 0) fin = len;
    mot = s.substring(pos, fin);
    pos = fin;
  }
  return mot;
}

// Le reste de la ligne (mot de passe : peut contenir des espaces), guillemets autour retirés
String resteLigne(const String &s, int pos) {
  String r = s.substring(pos);
  r.trim();
  if (r.length() >= 2 && r[0] == '"' && r[r.length() - 1] == '"') r = r.substring(1, r.length() - 1);
  return r;
}

void listerReseaux() {
  if (nbReseaux == 0) { Serial.println("Aucun réseau en mémoire. Tape 'aide'."); return; }
  Serial.printf("Réseaux en mémoire (%u / %u) :\n", (unsigned)nbReseaux, (unsigned)NB_WIFI_MAX);
  for (uint8_t i = 0; i < nbReseaux; i++) {
    bool actif = reseauOK() && reseauCourant == (int8_t)i;
    Serial.printf("  %u) %-24s %-6s", (unsigned)(i + 1), reseaux[i].ssid.c_str(), typeReseau(i));
    if (estEntreprise(i)) Serial.printf(" id %s", reseaux[i].eapId.c_str());
    if (actif) Serial.print("  <- connecté");
    else if ((int8_t)i == dernierReseau) Serial.print("  (dernier utilisé)");
    Serial.println();
  }
}

void scannerReseaux() {
#if WIFI_ACTIVE
  if (etatWifi == WIFI_SCAN || etatWifi == WIFI_CONNEXION) {
    Serial.println("Connexion en cours : réessaie dans quelques secondes.");
    return;
  }
  Serial.println("Recherche des réseaux visibles...");
  int n = WiFi.scanNetworks(false, false);
  if (n <= 0) { Serial.println("Aucun réseau visible."); WiFi.scanDelete(); return; }
  for (int j = 0; j < n; j++) {
    bool connu = false;
    for (uint8_t i = 0; i < nbReseaux; i++) if (reseaux[i].ssid == WiFi.SSID(j)) connu = true;
    bool ent = (WiFi.encryptionType(j) == WIFI_AUTH_WPA2_ENTERPRISE);
    bool ouvert = (WiFi.encryptionType(j) == WIFI_AUTH_OPEN);
    Serial.printf(" %c %-28s %4d dBm  %s\n", connu ? '*' : ' ', WiFi.SSID(j).c_str(), WiFi.RSSI(j),
                  ent ? "ENTREPRISE (wifi ent)" : (ouvert ? "OUVERT" : "WPA (wifi add)"));
  }
  WiFi.scanDelete();
#else
  Serial.println("WiFi désactivé (WIFI_ACTIVE 0).");
#endif
}

// Commandes "wifi ..." (s = la ligne d'origine, casse conservée)
void commandeWifi(const String &s) {
  int pos = 0;
  motSuivant(s, pos);                 // "wifi"
  String sous = motSuivant(s, pos);
  sous.toLowerCase();

  if (sous == "" || sous == "list") {
    listerReseaux();
  } else if (sous == "add" || sous == "ent") {
    bool ent = (sous == "ent");
    String ssid = motSuivant(s, pos);
    String id = ent ? motSuivant(s, pos) : String("");
    String mdp = resteLigne(s, pos);
    if (ssid.length() == 0 || ssid.length() > 32) { Serial.println("Nom de réseau absent ou trop long (32 max)."); return; }
    if (ent && (id.length() == 0 || mdp.length() == 0)) { Serial.println("Usage : wifi ent <nom> <identifiant> <mdp>"); return; }
    if (!ent && mdp.length() > 0 && (mdp.length() < 8 || mdp.length() > 63)) {
      Serial.println("Mot de passe Wi-Fi : 8 à 63 caractères."); return;
    }
    int i = ajouterReseau(ssid, mdp, id);
    if (i < 0) { Serial.println("Mémoire pleine (5 réseaux) : supprime-en un avec 'wifi del <n>'."); return; }
    Serial.printf("Réseau %u enregistré : %s (%s). Connexion...\n", (unsigned)(i + 1), ssid.c_str(), typeReseau(i));
    relancerWifi(true);
  } else if (sous == "del") {
    int n = motSuivant(s, pos).toInt();
    if (n < 1 || n > nbReseaux) { Serial.println("Numéro invalide : voir 'wifi list'."); return; }
    int i = n - 1;
    Serial.printf("Réseau %s supprimé.\n", reseaux[i].ssid.c_str());
    for (int k = i + 1; k < nbReseaux; k++) reseaux[k - 1] = reseaux[k];
    nbReseaux--;
    reseaux[nbReseaux] = ReseauWifi();
    bool etaitCourant = (reseauCourant == i);
    if (dernierReseau == i) dernierReseau = -1;
    else if (dernierReseau > i) dernierReseau--;
    if (reseauCourant > i) reseauCourant--;
    sauverReseaux();
    if (etaitCourant || etatWifi != WIFI_CONNECTE) relancerWifi(true);
  } else if (sous == "scan") {
    scannerReseaux();
  } else if (sous == "connect") {
    Serial.println("Relance de la connexion Wi-Fi...");
    relancerWifi(true);
  } else {
    Serial.println("Commande wifi inconnue. Tape 'aide'.");
  }
}

void executerCommande(const String &cmd) {
  // Les mots-clés ne tiennent pas compte de la casse ("STATUS" marche) ;
  // les valeurs (mots de passe...) gardent leur casse d'origine.
  String bas = cmd;
  bas.toLowerCase();

  // Chaque branche ci-dessous correspond à une commande : aide, status, reboot, verbose, set, calib, vider_file, oublier.
  if (bas == "aide" || bas == "help") {
    afficherAide();
  } else if (bas == "status") {
    afficherStatus();
  } else if (bas == "reboot") {
    Serial.println("Redémarrage...");
    delay(200);
    ESP.restart();
  } else if (bas == "verbose on" || bas == "verbose off") {
    verbose = (bas == "verbose on");
    prefs.putBool("verbose", verbose);
    Serial.printf("Affichage détaillé : %s\n", verbose ? "activé" : "désactivé");
  } else if (bas == "wifi" || bas.startsWith("wifi ")) {
    commandeWifi(cmd);
  } else if (bas.startsWith("set ")) {
    // Commande « set <nom> <valeur> » : on sépare le nom de la valeur au premier espace.
    String reste = cmd.substring(4);
    reste.trim();
    int sp = reste.indexOf(' ');
    if (sp < 0) { Serial.println("Usage : set <nom> <valeur> (tape 'aide')"); return; }
    String nom = reste.substring(0, sp);
    nom.toLowerCase();
    String valeur = reste.substring(sp + 1);
    valeur.trim();
    definirParametre(nom, valeur);
  } else if (bas == "calib oui") {
#if MHZ_ACTIVE
    // Ordre au capteur de se caler : l'air actuel devient la référence 400 ppm (le zéro).
    mhz.calibrate();
    Serial.println("Calibrage du zéro CO2 envoyé au MH-Z19C (à faire après 20 min à l'air libre).");
#else
    Serial.println("MH-Z19C désactivé (MHZ_ACTIVE 0).");
#endif
  } else if (bas == "calib") {
    Serial.println("Calibrage : l'air doit être extérieur (~400 ppm) depuis 20 min. Tape 'calib oui' pour confirmer.");
  } else if (bas == "vider_file") {
    demandeVider = true;  // c'est la tâche d'envoi qui touche au fichier
    Serial.println("Vidage de la file d'attente demandé.");
  } else if (bas == "oublier") {
    prefs.remove("ssid"); prefs.remove("wifi_pwd"); prefs.remove("eap_id");
    prefs.remove("eap_pwd"); prefs.remove("cle");
    for (uint8_t i = 0; i < NB_WIFI_MAX; i++) reseaux[i] = ReseauWifi();
    nbReseaux = 0;
    dernierReseau = -1;
    sauverReseaux();
    definirCle("");
    cleInvalide = false;
    relancerWifi(false);
    Serial.println("Réseaux Wi-Fi, identifiants et clé effacés de la carte.");
  } else {
    Serial.println("Commande inconnue. Tape 'aide'.");
  }
}

// La ligne en cours de saisie est-elle un mot de passe ? (pour l'afficher en ****)
bool saisieSecrete() {
#if MASQUER_SECRETS
  // on masque ce qui suit le nom du réseau (wifi add) ou l'identifiant (wifi ent)
  String b = ligneSerie;
  b.toLowerCase();
  int nbMots = b.startsWith("wifi add ") ? 3 : (b.startsWith("wifi ent ") ? 4 : 0);
  if (nbMots == 0) return false;
  int pos = 0;
  for (int i = 0; i < nbMots; i++) motSuivant(ligneSerie, pos);
  return pos < (int)ligneSerie.length();
#else
  return false;
#endif
}

// Lecture de la saisie caractère par caractère, avec ÉCHO : la carte renvoie ce que tu tapes
// (le moniteur PlatformIO n'affiche pas les touches tapées, il les envoie seulement).
// Retour arrière (0x08 / 0x7F) : efface 1 caractère. Ctrl+U (0x15) : efface toute la ligne.
// Les séquences des flèches / Suppr (ESC [ ...) sont ignorées.
void traiterSerie() {
  // On traite tous les caractères reçus depuis le dernier passage.
  while (Serial.available()) {
    char c = (char)Serial.read();

    // séquences d'échappement : on les avale
    if (etatEsc == 1) {
      if (c == '[' || c == 'O') { etatEsc = 2; continue; }
      etatEsc = 0;  // simple Échap : on traite le caractère normalement
    } else if (etatEsc == 2) {
      if ((uint8_t)c >= 0x40 && (uint8_t)c <= 0x7E) etatEsc = 0;  // octet final de la séquence
      continue;
    }
    if (c == 0x1B) { etatEsc = 1; continue; }

    // Touche Entrée : la ligne est terminée, on l'exécute comme une commande.
    if (c == '\r' || c == '\n') {
      if (ligneSerie.length() > 0) {
        String cmd = ligneSerie;
        ligneSerie = "";
        Serial.print("\r\n");  // retour à la ligne après ce qui a été tapé
        cmd.trim();
        if (cmd.length() > 0) executerCommande(cmd);
      }
    } else if (c == 0x08 || c == 0x7F) {  // retour arrière
      if (ligneSerie.length() > 0) {
        ligneSerie.remove(ligneSerie.length() - 1);
        Serial.print("\b \b");  // recule, efface, recule
      }
    } else if (c == 0x15) {  // Ctrl+U : efface toute la ligne
      while (ligneSerie.length() > 0) {
        ligneSerie.remove(ligneSerie.length() - 1);
        Serial.print("\b \b");
      }
    } else if ((uint8_t)c >= 32 && ligneSerie.length() < 120) {
      Serial.write(saisieSecrete() ? '*' : c);  // écho (masqué pour les mots de passe si MASQUER_SECRETS)
      ligneSerie += c;
    }
  }
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  // Liaison série avec l'ordinateur (moniteur série à 115200 bauds).
  Serial.begin(115200);
  delay(10);
  Serial.printf("\n=== Station ModuleAir | cause du redémarrage : %d ===\n", (int)esp_reset_reason());

  mutexEnvoi = xSemaphoreCreateMutex();  // avant tout : loop() et la tâche d'envoi s'en servent
  // Identifiant aléatoire de CE démarrage : permet de savoir si une mesure stockée date du même démarrage (voir calculerAge).
  bootId = esp_random();
  if (bootId == 0) bootId = 1;
  // Identifiant de la carte = son adresse MAC (unique).
  deviceId = lireMac();
  // Relit Wi-Fi, clé du groupe, position de rejeu... enregistrés en mémoire permanente.
  chargerConfig();

  // Monte la mémoire flash comme un petit disque (true = la formate si c'est le premier démarrage).
  fsOK = LittleFS.begin(true);
  if (!fsOK) Serial.println("Erreur montage LittleFS : pas de stockage hors ligne.");
  // Compte les mesures qui attendent d'être renvoyées (et termine un compactage interrompu par une coupure).
  initialiserBacklog();

  // Broche de détection secteur (entrée seule) : on lit aussi l'état de départ.
  pinMode(PIN_SECTEUR, INPUT);
  if (DETECTION_SECTEUR_ACTIVE) {
    estSurSecteur = digitalRead(PIN_SECTEUR);
    etatCandidat = estSurSecteur;
  }
#if PIN_BOUTON >= 0
  pinMode(PIN_BOUTON, INPUT_PULLUP);
#endif
#if PIN_BUZZER >= 0
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#endif

  // Écran : initialisation, couleurs, luminosité, puis écran d'accueil (~1,3 s).
  setupDisplay();
  display.setTextWrap(false);
  initialiserCouleurs();
  majLuminosite();
  afficherSplash();  // écran d'accueil : logo animé LA BONNE M'AIR (~2,7 s)

  // NextPM : Modbus, 8E1 (parité paire !), adresse 1
  nextpmSerial.begin(115200, SERIAL_8E1, NEXTPM_RX, NEXTPM_TX);
  nextpm.begin(1, nextpmSerial);

#if MHZ_ACTIVE
  // MH-Z19C : liaison série à 9600 bauds ; l'auto-calibration (ABC) recale seule le zéro à 400 ppm.
  Serial2.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
  mhz.begin(Serial2);
  mhz.autoCalibration(MHZ_ABC);  // ABC : 0 si la pièce n'est jamais aérée
#endif

  // Lance la connexion Wi-Fi en arrière-plan (scan puis meilleur réseau connu) : le programme n'attend pas,
  // l'écran montre la progression (page de connexion) puis le résultat.
  initialiserWiFi();
  if (!cleOK()) Serial.println("Clé du groupe absente : les mesures seront gardées en mémoire. Tape 'aide'.");

  // Watchdog : la carte redémarre toute seule si loop() reste bloquée 60 s
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  xTaskCreatePinnedToCore(tacheEnvoi, "envoi", 8192, NULL, 1, NULL, 1);  // réseau + fichier hors ligne, cœur 1 comme loop()

  aTransition = true;  // fondu du logo vers la première page
  afficher();          // première page : CO2 en préchauffage
  Serial.println("Prêt. Tape 'aide' pour la liste des commandes.");
}

// =====================================================================
// LOOP
// =====================================================================
// loop() tourne sans arrêt sur le cœur 1. Chaque travail ci-dessous se déclenche quand son délai est écoulé.
// L'envoi au serveur n'est PAS ici : il est fait par tacheEnvoi, pour ne jamais figer l'écran.
void loop() {
  unsigned long now = millis();
  esp_task_wdt_reset();

  // À chaque tour : commandes tapées, coupure de courant, bouton.
  traiterSerie();
  surveillerSecteur(now);
#if PIN_BOUTON >= 0
  surveillerBouton(now);
#endif

  // À chaque tour : connexion Wi-Fi (machine à états, jamais bloquante).
  gererWiFi(now);

  // Toutes les 15 s : Wi-Fi absent depuis trop longtemps ? mémoire libre ?
  if (now - tReseau >= INTERVALLE_RESEAU_MS) {
    tReseau = now;
    surveillerWifiLongue(now);
    verifierMemoire();
  }

  // Toutes les 10 s : lecture des capteurs, puis mise à jour des moyennes et des pics.
  if (now - tLecture >= INTERVALLE_LECTURE_MS) {
    tLecture = now;
    lireCapteurs();
    accumulerLectures();
    verifierAlertes(now);   // polluant hors norme : saut immédiat sur sa page + éclair
    aRedessiner = true;
  }

  // Toutes les minutes : une barre de plus dans l'histogramme CO2.
  if (now - tHist >= INTERVALLE_HIST_MS) {
    tHist = now;
    ajouterHistorique();
  }

  // Toutes les 30 s (60 s sur batterie) : la mesure de la période est prête, on la dépose pour l'envoi.
  if (now - tPeriode >= periodeMesureMs()) {
    tPeriode = now;
    finaliserPeriode();
  }

  // Toutes les 6 s : page suivante de l'écran (avec un fondu) (en pause pendant l'écran de connexion Wi-Fi).
  if (ecranWifiActif()) tPage = now;
  if (now - tPage >= INTERVALLE_PAGE_MS) {
    tPage = now;
    pageSuivante();
  }

  animer(now);  // animations (défilement, chiffres, yeux, préchauffage) : demande un redessin si besoin

  // Un seul redessin, même si lecture + changement de page + animation tombent ensemble
  if (aRedessiner) {
    aRedessiner = false;
    afficher();
  }

  // Toutes les minutes : ajuste la luminosité (jour / nuit / batterie).
  if (now - tLumi >= INTERVALLE_LUMI_MS) {
    tLumi = now;
    majLuminosite();
  }

#if PIN_BUZZER >= 0
  gererBuzzer(now);
#endif

  // Petite pause : laisse respirer le processeur (et le Wi-Fi) entre deux tours.
  delay(20);
}