/*
 * Station qualité de l'air : carte ModuleAir (ESP32)
 * Envoi DIRECT au serveur AirCarto, avec stockage des mesures en flash (LittleFS) quand le réseau est coupé, puis renvoi
 * automatique quand il revient.
 * Capteurs : NextPM (Modbus, UART1) | MH-Z19C (UART2) | écran PxMatrix 64x32
 * Dans le moniteur série (115200 bauds) :
 *   wifi ent eduroam identifiant@etablissement.fr MOT_DE_PASSE_ENT   (WPA2 Entreprise)
 *   wifi add MaBox MotDePasse                                      (Wi-Fi classique)
 *   wifi add "iPhone de Paul" MotDePasse                           (nom avec espaces : guillemets)
 *   set cle LA_CLE_DE_32_CARACTERES
 * La carte garde jusqu'à 5 réseaux en mémoire et se connecte toute seule au meilleur réseau
 * connu visible (pas besoin de reboot). Le type (entreprise / classique) est choisi PAR RÉSEAU.
 *   wifi list | wifi del <n|nom> | wifi scan | wifi connect
 * Autres commandes : aide | status | verbose on/off | calib oui | vider_file | oublier | maj
 *   bt appairer | bt demarrage on/off | bt liste | bt oublier [n]   (Bluetooth, câble USB seulement)
 *   set relais <url> | set jeton <jeton>   (console à distance, voir relais/LISEZMOI.md)
 *
 * CONSOLE SANS FIL (le moniteur série, sans câble) :
 *   - À DISTANCE, par Internet (CONSOLE_DISTANTE) : la carte envoie son journal à TON relais Cloudflare (HTTPS) et
 *     y prend les commandes tapées dans la page console. Sert aussi aux MISES À JOUR : « maj installer » télécharge
 *     la dernière release GitHub, vérifie sa SIGNATURE (clé privée sur ton PC seulement), l'installe dans l'autre
 *     emplacement de la mémoire et revient tout seul à l'ancienne version si la nouvelle ne marche pas.
 *   - À CÔTÉ, par Bluetooth (BLUETOOTH_ACTIF) : appairage avec le code à 6 chiffres affiché sur l'écran. Un NOUVEAU
 *     téléphone ne peut s'appairer que dans les 2 min qui suivent « bt appairer » tapé au câble USB (ou une mise sous
 *     tension, si « bt demarrage on ») ; les téléphones déjà appairés se reconnectent à tout moment.
 *   Par Bluetooth et à distance, SEULES les commandes sans secret, sans effacement et sans effet durable sont
 *   acceptées (aide, status, verbose, calib, reboot, wifi scan, wifi connect, maj, diag tls), une par seconde au plus
 *   (wifi scan / wifi connect / maj : une fois par 30 s ; reboot et diag tls : une fois par 5 min) :
 *   Wi-Fi, clé, jeton, relais, oublier, vider_file, calib oui = câble USB uniquement, et ce qu'affichent Wi-Fi,
 *   set et oublier ne quitte jamais le câble USB. (Le nom du réseau connecté, lui, apparaît dans « status ».)
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
 *   ANIMATIONS (clignement des yeux, chiffres qui glissent, sommet de la courbe qui clignote),
 *   AFFICHER_ETAT (page d'icônes intercalée).
 *   Pages (UNE mesure par écran) : CO2 | AERER (si CO2 élevé) | PM1 | PM2.5 | PM10 | INDICE ATMO | courbe CO2
 *   | Wi-Fi (seulement hors ligne). Chaque page de mesure : nom + unité, filet à la couleur du niveau, valeur en
 *   grand, visage, et la jauge des niveaux ATMO avec un CURSEUR sous la position exacte de la valeur.
 *   Page ÉTAT (toutes les PERIODE_ETAT_MS, pendant DUREE_ETAT_MS) : heure, grandes icônes Wi-Fi (signal),
 *   serveur (envoi / mesures en attente) et capteurs, chacune colorée selon son état.
 *   ALERTE HORS NORME : dès qu'un polluant franchit son seuil d'alerte (NIVEAU_ALERTE_PM / NIVEAU_ALERTE_CO2),
 *   l'écran saute AUSSITÔT sur sa page (sans fondu), clignote plein écran à la couleur du niveau, puis la valeur
 *   pulse ; tant que l'alerte dure, sa page revient une page sur deux (voir verifierAlertes).
 *   INDICE ATMO : règle officielle de l'indice ATMO = le PIRE des sous-indices (pas de moyenne, pas de
 *   pondération), calculé ici sur PM2.5, PM10 et PM1 (PM1 avec les seuils des PM2.5, voir niveauIndiceATMO).
 *   Une seule police (standard 5x7, zéro sans barre), pas de blanc : titres en cyan.
 *   Démarrage : logo animé "LA BONNE M'AIR" (basilique stylisée, voir afficherSplash).
 *
 * CHAMPS ENVOYÉS (JSON plat, nombres uniquement), en HTTPS (la clé du groupe ne circule plus en clair) :
 *   standard : device_id, uptime, rssi, heap, co2, pm1, pm25, pm10, npm_status (octet d'état du NextPM)
 *              (co2 / pm = MOYENNE des lectures de la période : capteurs lus chaque seconde, une mesure toutes
 *               les 10 s en direct, 30 s hors ligne ou pendant un rattrapage, 60 s sur batterie)
 *   perso :
 *     x_age_s          secondes entre la mesure et son envoi
 *                       (0-2 en direct, plus pour une mesure rejouée)
 *     x_co2_max         pic de CO2 sur la période (ppm)
 *     x_pm25_max        pic de PM2.5 sur la période (ug/m3)
 *     x_aeration        1 = chute rapide du CO2 détectée (fenêtre ouverte ?)
 *     x_pct_co2_1000    % du temps au-dessus de 1000 ppm depuis le démarrage
 *     x_reset           cause du dernier redémarrage (esp_reset_reason)
 *     x_alim            1 = secteur, 0 = batterie
 *     x_temp_pm, x_hum_pm   température / humidité internes du NextPM
 *   => 9 champs x_ (10 au maximum, guide AirCarto) : l'état du NextPM est passé dans le champ officiel npm_status.
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
 *   - en direct, une mesure toutes les 10 s : elle est bouclée au moment précis où le serveur accepte l'envoi
 *     suivant (10 s + la marge), et part aussitôt. Sans ce calage, une mesure produite toutes les 10 s pile
 *     arrive un peu plus vite que le serveur ne les accepte : elles s'empileraient ;
 *   - des mesures en retard : une mesure fraîche toutes les 30 s (2 créneaux sur 3 pour le retard), et toutes
 *     les 60 s au-delà de 10 en retard ; "status" donne le temps de rattrapage estimé.
 *   - hors ligne : une mesure toutes les 30 s, pour garder ~10 h dans la mémoire flash (3 h seulement à 10 s).
 *
 * VERSIONS : ce programme est écrit pour ESP32 Arduino 2.x. Dans platformio.ini :
 *   platform = espressif32@^6.0.0
 * et fixe les versions de lib_deps (pio pkg list te donne celles qui marchent aujourd'hui).
 */

// VUE D'ENSEMBLE DU PROGRAMME
//
// 1. setup() prépare tout, une seule fois : écran, capteurs, Wi-Fi, mémoire flash, tâche d'envoi.
// 2. loop() tourne ensuite sans arrêt (cœur 1). Elle ne fait aucun travail long : chaque travail se déclenche
//    quand son délai est écoulé (lecture des capteurs chaque seconde, une mesure toutes les 10 s, etc.).
// 3. Quatre acteurs travaillent en parallèle (plus la pile Bluetooth) :
//    - loop()       : capteurs, animations, écran, commandes (USB, Bluetooth, relais) ;
//    - refreshTask  : rafraîchit l'écran LED en continu (cœur 1 avec loop, réveillée par un timer toutes les 4 ms) ;
//    - tacheEnvoi   : envoie les mesures au serveur ; sans réseau, les range dans un fichier en flash
//                     puis les renvoie quand le réseau revient ;
//    - tacheRelais  : console à distance et mises à jour (HTTPS vers ton relais Cloudflare, cœur 0).
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
#define ECRAN_ETEINT_NUIT         0  // 1 = écran éteint pendant la plage de nuit (commande « nuit »)
#define AFFICHER_ETAT             1  // 1 = page ÉTAT (heure + grandes icônes Wi-Fi / serveur / capteurs) intercalée
                                     //     toutes les PERIODE_ETAT_MS pendant DUREE_ETAT_MS (voir dessinerPageEtat)
#define MASQUER_SECRETS           0  // 1 = les mots de passe saisis s'affichent en ****
#define DOUBLE_BUFFER             1  // 1 = dessin sans scintillement | 0 si l'écran reste noir ou s'affiche mal
#define ANIMATIONS                0  // 1 = clignement, chiffres qui glissent, sommet de courbe qui clignote | 0 = calme
#define TRANSITION_FONDU          1  // 1 = fondu au noir entre deux pages (~0,6 s, ~0,4 s vers / depuis ÉTAT) | 0 = sec
#ifndef AIRCARTO_HTTPS
#define AIRCARTO_HTTPS            1  // 1 = mesures envoyées en HTTPS (clé chiffrée, serveur vérifié) | 0 = HTTP simple
                                     //     (clé du groupe lisible sur le réseau : seulement si le HTTPS pose problème)
#endif
#ifndef EDUROAM_IDENTITE_ANONYME
#define EDUROAM_IDENTITE_ANONYME  1  // 1 = eduroam : « anonymous@ton-domaine » circule en clair à la place de ton identifiant
                                     //     (le vrai ne part que dans le tunnel chiffré) | 0 si ton université le refuse
#endif
#ifndef CONSOLE_DISTANTE
#define CONSOLE_DISTANTE          1  // 1 = journal, commandes et mise à jour PAR INTERNET, via ton relais Cloudflare
                                     //     (dossier relais/ ; adresse et jeton : set relais / set jeton, au câble USB)
#endif
#ifndef BLUETOOTH_ACTIF
#define BLUETOOTH_ACTIF           1  // 1 = console Bluetooth (BLE) : appairage avec le code à 6 chiffres affiché sur l'écran
#endif

// --- Bibliothèques utilisées ---
// WiFi / HTTPClient : connexion et envoi au serveur | LittleFS : fichier en mémoire flash (stockage hors ligne)
// Preferences : mémoire de configuration (mots de passe, clé) | ArduinoJson : fabrication et lecture des mesures en JSON
// ModbusMaster : dialogue avec le NextPM | MHZ19 : capteur de CO2 | PxMatrix : pilotage de l'écran LED
#include <Arduino.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#error "Ce programme est écrit pour ESP32 Arduino 2.x : timerBegin(), esp_task_wdt_init() et esp_wpa2.h changent en 3.x. Dans platformio.ini : platform = espressif32@^6.0.0"
#endif
#include <WiFi.h>
#include <esp_wifi.h>               // esp_wifi_scan_stop : arrêt d'un scan bloqué
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
// Taille des tampons d'image de PxMatrix, réservés pour 64 x 64 par défaut : l'écran fait 64 x 32. À la bonne
// taille, ~6 Ko de mémoire vive en plus (double tampon), sans rien changer à l'affichage. (À définir AVANT l'include.)
#define PxMATRIX_MAX_WIDTH  64
#define PxMATRIX_MAX_HEIGHT 32
#include <PxMatrix.h>
// Police : la 5x7 intégrée à Adafruit GFX, en taille 1 (textes) et 2 (valeurs)

// HTTPS vers AirCarto et vers le relais : certificats racines acceptés (Let's Encrypt, Google, SSL.com, Sectigo)
#include <WiFiClientSecure.h>
#include <mbedtls/error.h>          // diag tls : texte des erreurs
#if __has_include(<mbedtls/ssl_internal.h>)
#include <mbedtls/ssl_internal.h>   // MBEDTLS_SSL_IN/OUT_BUFFER_LEN : taille exacte des tampons d'une connexion
#endif
#include <esp_heap_caps.h>          // diag tls : mémoire réellement utilisable par mbedTLS
#include "racines_tls.h"            // racines HTTPS en texte : la liste à modifier (source)
#include "racines_bundle.h"         // les mêmes au format « bundle » (généré à la compilation par outils/racines_bundle.py)
#include "verif_bundle.h"           // vérification par bundle (bundleCourant : réglé avant chaque connexion, sous mutexTls)
#if WIFI_ACTIVE
#include "esp_wpa2.h"
// eduroam : certificat de l'autorité qui signe le serveur d'authentification de l'université (facultatif mais
// conseillé : sans lui, un faux point d'accès « eduroam » peut récupérer ton identifiant et ton mot de passe ENT).
// Fichier à créer : voir ca_eduroam.h.exemple.
#if __has_include("ca_eduroam.h")
#include "ca_eduroam.h"
#include <mbedtls/x509_crt.h>
#define CA_EDUROAM_PRESENT 1
#else
#define CA_EDUROAM_PRESENT 0
#endif
#endif
#if CONSOLE_DISTANTE
// Console à distance et mise à jour : écriture du nouveau programme, vérification de signature
#include <esp_ota_ops.h>         // écriture du nouveau programme (esp_ota_begin efface l'emplacement en une fois)
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#endif
#if BLUETOOTH_ACTIF
#include <NimBLEDevice.h>    // pile Bluetooth basse consommation, plus légère que celle d'origine (lib_deps)
#include <nimble/nimble/host/include/host/ble_hs.h>   // ble_hs_cfg : réglages de sécurité de la pile (sm_sc_only)
#include <nvs.h>                                          // status : place libre dans la mémoire des réglages (NVS)
#endif
// Numéro de version : fabriqué à chaque compilation par outils/ota.py (date de la dernière modification du code,
// AAMMJJhhmm). La carte n'accepte une mise à jour que si son numéro est PLUS GRAND que le sien.
#if __has_include("version_fw.h")
#include "version_fw.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION 0UL
#endif
// Clé PUBLIQUE de signature des mises à jour, créée une fois par « pio run -t cles » (la clé privée reste sur ton PC).
// Sans elle, la carte refuse toute mise à jour à distance.
#if CONSOLE_DISTANTE && __has_include("cle_ota.h")
#include "cle_ota.h"
#define OTA_POSSIBLE 1
#else
#define OTA_POSSIBLE 0
#endif

// =====================================================================
// CONFIGURATION
// =====================================================================
// Les identifiants (Wi-Fi, ENT, clé du groupe) ne sont PAS ici : ils sont
// enregistrés dans la mémoire de la carte par les commandes "wifi ..." et "set cle".
// Adresse du serveur AirCarto qui reçoit les mesures, en HTTPS : la clé du groupe (en-tête X-Group-Key) est chiffrée
// et la réponse du serveur ne peut plus être falsifiée en route. Certificat Let's Encrypt (chaîne YR2 -> Root YR ->
// ISRG Root X1, vérifiée le 22/11/2026) : seules les racines de Let's Encrypt sont acceptées (BUNDLE_AIRCARTO).
// AirCarto accepte les deux (http:// et https://, voir sa documentation openapi.yaml) : AIRCARTO_HTTPS choisit.
#if AIRCARTO_HTTPS
static const char* URL_SERVEUR  = "https://api.aircarto.fr/iut-amu/push";
#else
static const char* URL_SERVEUR  = "http://api.aircarto.fr/iut-amu/push";
#endif
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
static const char* const STATUS_BITS[8] = {
  "veille", "mode degrade", "demarrage en cours", "humidite trop forte",
  "erreur capteur T/HR", "erreur ventilateur", "erreur memoire", "erreur laser"
};
#define STATUS_CRITICAL  0xAC  // démarrage, humidité, ventilateur, laser
#define STATUS_DEMARRAGE 0x04

// --- MH-Z19C ---
#define MHZ_RX 36  // l'ESP32 reçoit (fil TX du capteur)
#define MHZ_TX 27  // l'ESP32 émet (fil RX du capteur)

// --- Détection de coupure secteur (pont diviseur sur GPIO 34) ---
// Désactivée par défaut : sur la carte actuelle, GPIO 34 lit 0 en permanence (x_alim = 0 sur 100 % des mesures
// du serveur, alors que la carte est branchée). Elle se croyait donc toujours sur batterie : une mesure par
// minute au lieu de 30 s et l'écran bridé à LUMINOSITE_BATTERIE. Mettre true seulement si le pont est câblé.
static const bool DETECTION_SECTEUR_ACTIVE = false;
#define PIN_SECTEUR 34

// --- Matériel optionnel : -1 = absent (aucun code actif) ---
#define PIN_BOUTON -1  // GPIO libre, bouton vers GND : appui court = page suivante, long = envoi immédiat
#define PIN_BUZZER -1  // GPIO libre, buzzer ACTIF : bip si CO2 > SEUIL_ALARME_PPM

// --- Luminosité ---
// 10 niveaux, réglables séparément pour le jour et la nuit (commande « lum », câble USB ou Bluetooth ; gardés en
// mémoire). PxMatrix allume chaque ligne 30 µs × luminosité / 255 : les valeurs tombent sur 1, 2, 3, 4, 6, 8, 11,
// 15, 21 et 30 µs, soit environ +40 % par niveau, des pas que l'œil voit réguliers. (Avant : 40 / 255 la nuit,
// soit 4 µs : l'œil percevant la lumière de façon logarithmique, l'écran paraissait à peine moins fort.)
static const uint8_t NIVEAUX_LUMI[10]    = {9, 17, 26, 34, 51, 68, 94, 128, 179, 255};
static const uint8_t NIVEAU_JOUR_DEFAUT  = 10;   // 255 : le maximum
static const uint8_t NIVEAU_NUIT_DEFAUT  = 2;    // 17 : 2 µs, quinze fois moins que le jour
static const uint8_t LUMINOSITE_BATTERIE = 60;
// Plage de nuit par défaut (heures, 0..23) : de DEBUT inclus à FIN exclu, peut passer minuit. Avant : 0 h -> 7 h,
// la soirée restait donc en plein jour. Réglable sans reflasher : « nuit 22 7 » (« nuit off » : jamais).
static const uint8_t HEURE_NUIT_DEBUT_DEFAUT = 22;
static const uint8_t HEURE_NUIT_FIN_DEFAUT   = 7;

// --- Intervalles (ms) ---
// Rythme de chaque travail : loop() lance un travail quand (maintenant - dernier passage) >= son intervalle.
static const unsigned long INTERVALLE_LECTURE_MS   = 1000;       // lecture des capteurs : chaque seconde (les deux capteurs
                                                                  // font eux-mêmes une mesure par seconde : plus vite = doublons)
static const unsigned long LECTURE_EN_PANNE_MS     = 10000;      // capteur en panne : relu toutes les 10 s seulement (une lecture
                                                                  // ratée attend la fin du délai de réponse : jusqu'à 2 s en Modbus)
static const uint8_t ECHECS_AVANT_PANNE            = 10;         // lectures ratées de suite avant « en panne » (~10 s)
static const unsigned long INTERVALLE_PAGE_MS       = 8000;       // durée d'affichage de chaque page (une seule information par page)
static const unsigned long PERIODE_MESURE_MS        = 10000;      // en direct : une mesure (moyenne + pic des lectures) toutes les 10 s
static const unsigned long PERIODE_MESURE_HORS_LIGNE_MS = 30000;  // hors ligne, ou quelques mesures en retard : toutes les 30 s
static const unsigned long PERIODE_MESURE_BATT_MS   = 60000;      // sur batterie
static const unsigned long PERIODE_MESURE_RATTRAPAGE_MS = 60000;  // pendant le renvoi d'un gros retard (voir SEUIL_RATTRAPAGE)
static const uint32_t SEUIL_RATTRAPAGE              = 10;         // plus de 10 mesures en retard : 1 mesure / min le temps de rattraper
static const unsigned long ATTENTE_CRENEAU_MAX_MS   = 30000;      // en direct, une mesure attend le créneau d'envoi 30 s au plus
static const unsigned long ECART_PUSH_MIN_MS        = 10000;      // entre 2 envois (le serveur exige >= 10 s, pas d'envoi groupé)
static const unsigned long MARGE_PUSH_INIT_MS       = 800;        // marge au-dessus des 10 s (absorbe la variation du temps de connexion)
static const unsigned long MARGE_PUSH_MIN_MS        = 400;        // la marge descend jusque-là tant que le serveur ne dit rien
static const unsigned long MARGE_PUSH_MAX_MS        = 5000;       // ... et remonte (+500 ms) à chaque refus 429 "trop rapproché"
static const unsigned long ATTENTE_NTP_REESSAI_MS   = 2000;       // une mesure attend l'heure NTP : on réessaie toutes les 2 s
static const unsigned long INTERVALLE_RESEAU_MS     = 15000;
static const unsigned long INTERVALLE_LUMI_MS        = 10000;
static const unsigned long INTERVALLE_HIST_MS        = MODE_DEMO ? 5000UL : 60000UL;  // un point de la courbe CO2 par minute (5 s en démo)
static const unsigned long CHAUFFE_MHZ_MS            = 180000UL;  // MH-Z19C : environ 3 min pour se stabiliser (fiche technique)
// CO2 plausible : l'air extérieur ne descend pas sous ~400 ppm ; 5000 = haut de la gamme du MH-Z19C.
// En dessous de CO2_MIN_PLAUSIBLE, la lecture est écartée (vu sur le serveur : dérive jusqu'à 37 ppm la nuit) :
// après 3 lectures écartées de suite, la page CO2 affiche « A CALIBRER ».
static const int CO2_MIN_PLAUSIBLE = 300;
static const int CO2_MAX_PLAUSIBLE = 5000;
// CO2 très élevé : au-dessus de CO2_CONFIRMATION_PPM, une lecture ne compte qu'une fois CONFIRMÉE par
// CO2_NB_CONFIRMATION lectures de suite (une par seconde) au-dessus du seuil ET proches les unes des autres
// (toutes à ± CO2_ECART_CONFIRMATION de leur médiane). Un pic isolé ou des valeurs qui sautent sont ignorés.
static const int CO2_CONFIRMATION_PPM    = 3000;
static const uint8_t CO2_NB_CONFIRMATION = 5;
static const float CO2_ECART_CONFIRMATION = 0.10f;
static const unsigned long ANTIREBOND_SECTEUR_MS     = 500;
static const unsigned long WIFI_TIMEOUT_PSK_MS       = 20000UL;   // délai max pour se connecter à un Wi-Fi classique
static const unsigned long WIFI_TIMEOUT_EAP_MS       = 35000UL;   // eduroam est plus lent (authentification ENT)
static const unsigned long WIFI_SCAN_MAX_MS          = 12000UL;   // un scan des réseaux visibles ne dure jamais plus
static const unsigned long WIFI_PAUSE_MS             = 30000UL;   // aucun réseau connu joignable : nouvel essai après 30 s,
static const unsigned long WIFI_PAUSE_MAX_MS         = 120000UL;  // puis 60 s, puis 2 min (un scan occupe la radio, Bluetooth compris)
static const uint32_t      WIFI_SCAN_MS_CANAL        = 150;       // écoute par canal d'un scan (300 par défaut) : ~2 s au lieu de ~4 s
static const unsigned long WIFI_REESSAI_MS           = 2000UL;    // 1er essai raté : le même réseau est retenté 2 s après
                                                                  // (après un redémarrage, beaucoup de box refusent le 1er)
static const unsigned long WIFI_LOG_ECHEC_MS         = 600000UL;  // échecs en arrière-plan : une ligne au journal / 10 min
static const unsigned long WIFI_RESULTAT_MS          = 3000UL;    // durée d'affichage de "CONNECTE" / "ECHEC" à l'écran
static const unsigned long WIFI_REBOOT_MS            = 3600000UL; // 1 h sans Wi-Fi : on redémarre la carte
static const uint32_t HEAP_MIN_OCTETS                = 25000;     // en dessous : redémarrage (fuite mémoire ?)
static const uint32_t BLOC_MIN_OCTETS                = 10000;     // plus grand bloc libre en dessous : mémoire morcelée, redémarrage
// Anti-blocage des envois (vus en simulation sur 4 jours : chacun arrêtait les envois sans faire planter la carte)
static const unsigned long ESSAI_CLE_MS              = 900000UL;  // clé refusée (401) : nouvel essai toutes les 15 min, jamais d'arrêt définitif
static const uint8_t       ECHECS_AVANT_RECONNEXION  = 5;         // POST sans réponse alors que « connecté » : on refait la connexion Wi-Fi
static const unsigned long SANS_REPONSE_REBOOT_MS    = 6UL * 3600000UL;  // 6 h connecté sans aucune réponse du serveur : redémarrage
static const unsigned long ATTENTE_SERVEUR_MAX_MS    = 300000UL;  // délai demandé par le serveur (next_push_in_s) : 5 min au plus
// Mémoire pour une connexion HTTPS (mesurée par « diag tls » le 23/11/2026, racines en bundle) : ~47 Ko occupés pendant
// la connexion, dont deux tampons de ~16,7 Ko d'un seul tenant, plus les calculs de la poignée de main. Réussi avec
// 63 Ko libres ; échouait vers 58 Ko quand les racines étaient décodées en RAM (~6 Ko). En dessous : on attend.
// Plus grand bloc : il en faut un pour le premier tampon ; le second se loge dans un autre bloc ou le reste du premier.
// Exiger les deux d'un seul tenant bloquait tout : après quelques heures, le plus grand bloc tombe vers 35 Ko (43 Ko au
// premier envoi), et l'ancien seuil de 45 Ko arrêtait les envois ET la console à distance, sans message.
// Si la mémoire manque malgré tout pendant la poignée de main, la connexion échoue proprement et sera retentée.
// Ces seuils servent quand la réserve HTTPS n'est pas en place ; avec elle, voir memoireTlsOK (réserve comptée).
static const uint32_t TLS_MEMOIRE_MIN_OCTETS         = 56000;     // mémoire libre utilisable par mbedTLS, au total
static const uint32_t TLS_BLOC_MIN_OCTETS            = 18000;     // plus grand bloc : au moins un tampon de ~16,7 Ko
static const int      REPONSE_MAX_OCTETS             = 2048;      // réponse d'AirCarto lue au plus (quelques centaines d'octets)
static const uint32_t WDT_TIMEOUT_S                  = 60;        // watchdog : redémarrage si la carte se fige

// --- Stockage hors ligne ---
// Stockage hors ligne : une mesure JSON par ligne dans FICHIER_BACKLOG.
// FICHIER_TMP sert de copie de travail pendant le compactage (voir compacterBacklog).
static const char* FICHIER_BACKLOG = "/backlog.jsonl";
static const char* FICHIER_TMP     = "/backlog.tmp";
static const size_t TAILLE_MAX_BACKLOG = 400000;   // ~400 Ko (~10 h hors ligne). Le compactage recopie le fichier : il faut
                                                   // de la place pour l'original ET la copie (partition LittleFS : 896 Ko,
                                                   // voir partitions_mair.csv : le reste sert aux 2 emplacements du programme)
// (le compactage ne se fait plus pendant le renvoi : il bloquait les envois plusieurs secondes. Le fichier est
//  supprimé quand tout est renvoyé, et compacté seulement s'il est plein : voir stockerRecord)
static const unsigned long ATTENTE_HEURE_MAX_MS = 900000UL;  // 15 min : au-delà sans heure NTP, une mesure d'un démarrage précédent est abandonnée

// --- Initiatives : paramètres ---
// Page courbe CO2 : COURBE_NB_BARRES barres de COURBE_MIN_PAR_BARRE minutes = la dernière heure.
#define COURBE_NB_BARRES     15
#define COURBE_MIN_PAR_BARRE 4
#define HIST_N (COURBE_NB_BARRES * COURBE_MIN_PAR_BARRE - 1)  // minutes terminées gardées (59) + la minute en cours = 1 h
static const float SEUIL_AERER_PPM      = 1000;    // page "AERER" au-dessus de 1000 (1000 pile = encore « moyen ») et x_pct_co2_1000
static const float SEUIL_ALARME_PPM     = 1500;    // buzzer au-dessus de 1500 (niveau « mauvais »)
static const float AERATION_CHUTE_PPM   = 100;     // chute de CO2 en ~2 min = aération
static const unsigned long BUZZER_PAUSE_MS = 300000UL;  // un bip toutes les 5 min au plus

// --- Alerte « hors norme » (changement brutal de l'écran) ---
// Niveaux : 0 bon | 1 moyen | 2 dégradé | 3 mauvais | 4 très mauvais | 5 extrêmement mauvais (indice ATMO)
// PM : alerte à partir de « MAUVAIS » (PM2.5 et PM1 > 25 ug/m3, PM10 > 50 ug/m3 : 50 = valeur limite journalière UE des PM10).
// CO2 : alerte à partir du niveau 2 (> 1000 ppm = SEUIL_AERER_PPM) : la page AERER s'affiche d'un coup.
static const int NIVEAU_ALERTE_PM  = 3;
static const int NIVEAU_ALERTE_CO2 = 2;
static const unsigned long ALERTE_FLASH_MS    = 150;     // durée d'une image du clignotement plein écran
static const uint8_t       ALERTE_NB_FLASH    = 6;       // 6 images = 3 éclairs (~0,9 s)
static const unsigned long ALERTE_PULSE_MS    = 500;     // la valeur en alerte pulse (vif / atténué)
static const unsigned long ALERTE_REARME_MS   = 60000UL; // un même polluant ne refait pas clignoter l'écran plus d'1 fois / min

// --- Seuils d'affichage ---
// Une échelle = ses seuils + la façon de les comparer :
//   inclus = false : valeur <  seuil[i] => niveau i   (seuils "à partir de" : plus utilisé)
//   inclus = true  : valeur <= seuil[i] => niveau i   (seuils "jusqu'à", valeur arrondie comme à l'écran)
// Règle commune à la carte et au site : une valeur PILE sur un seuil est classée dans le niveau le plus BAS
// (PM2.5 = 10 -> bon ; CO2 = 800 -> bon), et l'on compare la valeur arrondie à l'unité (10,4 -> 10 -> bon).
struct Echelle { const float* seuils; uint8_t n; bool inclus; };

// CO2 (ppm) : bon <= 800 | moyen <= 1000 | dégradé <= 1500 | mauvais > 1500
static const float SEUILS_CO2[] = {800, 1000, 1500};
static const Echelle ECH_CO2 = {SEUILS_CO2, 3, true};
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
// Qualificatifs ATMO. Page "ATMO" : 7 caractères par ligne au plus ; un libellé plus long est coupé
// en deux lignes à l'espace ("TRES" / "MAUVAIS").
static const char* const LABELS_NIVEAU[NB_NIVEAUX] = {"BON", "MOYEN", "DEGRADE", "MAUVAIS", "TRES MAUVAIS", "EXTREM. MAUVAIS"};

// --- Écran : fondu entre les pages, page ÉTAT et animations ---
// Fondu au noir entre deux pages : l'ancienne s'éteint en FONDU_DEMI_MS, la nouvelle s'allume en FONDU_DEMI_MS
// (~0,6 s en tout : avant ~3 s, trop lent). Vers ou depuis la page ÉTAT (icônes), et du logo de démarrage à la
// 1re page : ~0,4 s. Le temps gagné va aux pages elles-mêmes (INTERVALLE_PAGE_MS : 8 s au lieu de 5 s).
// Le fondu ne compte pas dans la durée d'une page : chacune reste ensuite INTERVALLE_PAGE_MS en pleine lumière.
#if TRANSITION_FONDU
static const unsigned long FONDU_DEMI_MS       = 300;
static const unsigned long FONDU_COURT_DEMI_MS = 200;
#else
static const unsigned long FONDU_DEMI_MS       = 0;
static const unsigned long FONDU_COURT_DEMI_MS = 0;
#endif
// Page ÉTAT (icônes) : revient après 3 pages de mesure (PERIODE_ETAT_MS, fondus compris), reste DUREE_ETAT_MS.
// L'état (Wi-Fi, envoi, capteurs) change rarement : un coup d'oeil suffit.
static const unsigned long PERIODE_ETAT_MS = 3 * INTERVALLE_PAGE_MS + 4 * FONDU_DEMI_MS;
static const unsigned long DUREE_ETAT_MS   = 4000;
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
// parallèle du reste du programme.
// Cœur 1 (celui de loop) et non plus cœur 0 : chaque ligne reste allumée 15 à 120 µs, mesurés en boucle ; sur le
// cœur 0, les interruptions du Wi-Fi et du Bluetooth (très nombreuses pendant un scan, une connexion ou le
// téléchargement d'une mise à jour) allongeaient au hasard ces durées : lignes plus claires par instants, donc
// scintillement au démarrage et pendant la mise à jour. Autre effet : loop() (qui dessine) ne tourne jamais PENDANT
// un rafraîchissement, donc elle ne peut plus redessiner un tampon que l'écran est encore en train d'afficher.
hw_timer_t* timer = NULL;
SemaphoreHandle_t refreshSem;
volatile bool rafraichissementEnCours = false;   // un envoi à l'écran est en cours (presenter attend sa fin)
volatile uint32_t nbRafraichissements = 0;
volatile bool ecranSuspendu = false;             // écran éteint, rafraîchissement arrêté (effacement de la flash)

void IRAM_ATTR onTimer() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(refreshSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void refreshTask(void*) {
  for (;;) {
    if (xSemaphoreTake(refreshSem, portMAX_DELAY)) {
      if (ecranSuspendu) continue;   // le dernier envoi a laissé les LED éteintes : écran noir, sans clignoter
      rafraichissementEnCours = true;
      display.display(30);  // durée d'allumage de chaque ligne, en µs
      rafraichissementEnCours = false;
      nbRafraichissements++;
    }
  }
}

static const BaseType_t COEUR_ECRAN = 1;   // cœur de refreshTask (le même que loop : voir plus haut)

void setupDisplay() {
  display.begin(16);  // balayage 1/16
  display.setDriverChip(SHIFT);
  display.setColorOrder(RRBBGG);  // couleurs inversées ? essayez RRGGBB
  display.setBrightness(255);     // luminosité : 0 à 255, ici le maximum
  display.clearDisplay();
  // Le timer « donne » ce sémaphore toutes les 4 ms ; refreshTask l'attend, se réveille et rafraîchit l'écran.
  // (le timer est créé ici, depuis setup, donc sur le cœur 1 lui aussi : réveil sans passer d'un cœur à l'autre)
  refreshSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(refreshTask, "refresh", 4096, NULL, configMAX_PRIORITIES - 1, NULL, COEUR_ECRAN);  // priorité max
  timer = timerBegin(0, 80, true);        // 80 MHz / 80 : un tic par µs
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 4000, true);     // toutes les 4000 µs, en boucle
  timerAlarmEnable(timer);
}

// Luminosité de l'écran, avec la cadence de rafraîchissement qui va avec (à utiliser à la place de setBrightness).
// PxMatrix affiche un « plan de couleur » (1 bit sur 4) par appel : une image complète demande 4 appels, soit 16 ms
// à 4 ms d'intervalle (~60 images/s). À faible luminosité, chaque ligne ne s'allume que 1 à 4 µs par plan et les
// plans les plus faibles tombent à 0 µs : un pixel ne s'allume plus qu'UNE fois par image, un éclair bref 60 fois
// par seconde, qui se voit (scintillement, aussi pendant les fondus, qui passent par ces niveaux). En pleine lumière,
// chaque pixel s'allume sur plusieurs plans, l'œil ne voit rien.
// Un appel est alors bien plus court : sous ~50 % de luminosité, on rafraîchit 2 fois plus souvent (toutes les 2 ms,
// ~125 images/s), et chaque éclair est raccourci d'autant (luminosité passée à PxMatrix × période / 4 ms) : la
// lumière perçue reste la même à chaque niveau de « lum », seul le scintillement disparaît. La part du processeur
// reste celle de la pleine lumière (~35 % du cœur 1). Pas en dessous de 2 ms : les éclairs, comptés en µs entières,
// deviendraient trop courts et les couleurs perdraient des nuances.
static uint16_t periodeRafraichissementUs = 4000;
void reglerLuminositeEcran(uint8_t lum) {
  if (timer == NULL) { display.setBrightness(lum); return; }   // (avant setupDisplay : période fixe de 4 ms)
  // durée moyenne d'un appel : 16 lignes × (~25 µs d'envoi + allumage moyen des 4 plans, 56 µs à 255)
  uint32_t appelUs = 16UL * (25UL + 56UL * lum / 255UL);
  uint32_t p = appelUs * 100UL / 35UL;          // ~35 % du cœur 1
  p = (p + 250UL) / 500UL * 500UL;              // par pas de 500 µs : pas de réglage à chaque image d'un fondu
  if (p < 2000UL) p = 2000UL;
  if (p > 4000UL) p = 4000UL;
  uint32_t lumEcran = ((uint32_t)lum * p + 2000UL) / 4000UL;   // même lumière qu'à 4 ms
  if (lum > 0 && lumEcran == 0) lumEcran = 1;
  display.setBrightness((uint8_t)lumEcran);
  if (p == periodeRafraichissementUs) return;
  periodeRafraichissementUs = (uint16_t)p;
  timerAlarmWrite(timer, p, true);
  timerWrite(timer, 0);   // sinon, si le compteur avait déjà dépassé la nouvelle échéance, il ne la croiserait plus
}

// =====================================================================
// TYPES ET VARIABLES GLOBALES
// =====================================================================
// ---------- Connexions HTTPS (AirCarto, relais) ----------
// Arduino-ESP32 2.x vérifie l'autorité et le NOM du serveur, mais PAS les dates des certificats (option
// MBEDTLS_HAVE_TIME_DATE absente). Cette classe ajoute le contrôle de la date de fin du certificat du serveur à
// CHAQUE ouverture de connexion, y compris celles que HTTPClient ouvre lui-même : la clé du groupe ou le jeton du
// relais ne partent qu'ensuite. (HTTPClient appelle connect(hote, port, délai), virtuelle : c'est elle qu'on
// remplace.)
bool certificatsALaDate(WiFiClientSecure &c, const char* quoi);
// Racines : « bundle » en mémoire flash (racines_bundle.h) et non liste en texte (setCACert). Avec setCACert, la
// carte décodait toutes les racines en mémoire vive pendant la poignée de main (~6 Ko pour AirCarto, ~18 Ko pour le
// relais) : il ne restait plus assez de mémoire pour vérifier la signature RSA 4096 bits de la chaîne d'AirCarto
// (« diag tls » : BIGNUM - Memory allocation failed, vu comme « not correctly signed by the trusted CA »).
// Avec le bundle, la vérification est la même (chaîne, signatures, nom du serveur) ; seule la racine utile est lue,
// au moment de vérifier la signature du haut de la chaîne. Bundle illisible : aucune racine, connexion refusée.
class ClientTlsDate : public WiFiClientSecure {
 public:
  explicit ClientTlsDate(const char* q) : quoi(q) {}
  // Le bundle n'est réglé qu'à l'ouverture (connect), sous le verrou mutexTls : l'autre tâche peut être en pleine
  // poignée de main avec le sien.
  void racines(const uint8_t* b) { bundle = b; _CA_cert = nullptr; _use_insecure = false; _use_ca_bundle = (b != nullptr); }
  using WiFiClientSecure::connect;
  int connect(const char* hote, uint16_t port, int32_t delai) override {
    if (bundle == nullptr) return 0;               // jamais sans racines
    bundleCourant = bundle;                        // commun à toute la carte : on remet le nôtre (sous mutexTls)
    if (!WiFiClientSecure::connect(hote, port, delai)) return 0;
    if (!certificatsALaDate(*this, quoi)) { stop(); return 0; }
    tOuverture = millis() | 1UL;
    return 1;
  }
  unsigned long tOuverture = 0;   // fin de la poignée de main (la requête part juste après) ; 0 = pas ouverte
 private:
  const char* quoi;
  const uint8_t* bundle = nullptr;
};
// Une seule connexion HTTPS à la fois (~40 Ko de mémoire chacune) : tacheEnvoi (AirCarto) et tacheRelais se passent
// ce verrou. AirCarto attend jusqu'à 5 s (sinon l'envoi est reporté au créneau suivant), le relais jusqu'à 20 s ;
// le relais laisse toujours au moins SYNC_LIVE_MS libres entre deux échanges, dans lesquels AirCarto passe.
SemaphoreHandle_t mutexTls = NULL;
TaskHandle_t hTacheLoop = NULL, hTacheEnvoi = NULL, hTacheRelais = NULL;   // pile restante (commande status)
// ---------- Réserve mémoire des connexions HTTPS ----------
// Une connexion HTTPS demande deux tampons de ~16,7 Ko d'un seul tenant (réception, émission). Pris puis rendus à
// chaque échange (AirCarto toutes les 10 s quand des mesures sont en retard, relais toutes les 2 à 30 s), ils
// morcelaient la mémoire : de petits blocs alloués pendant la connexion restaient ensuite au milieu de leur place.
// Après quelques heures : 60 Ko libres mais plus un seul bloc de 17 Ko (« échange reporté, mémoire insuffisante »),
// et quand la connexion partait quand même, la vérification du certificat manquait de mémoire en pleine poignée de
// main (« X509 - A fatal error occurred... vrfy callback failed »).
// Désormais ces tampons sont réservés UNE FOIS, au démarrage (mémoire encore d'un seul tenant) ; mbedTLS les reçoit à
// chaque connexion (mbedtls_platform_set_calloc_free) et ils ne passent plus par la mémoire générale. Toute autre
// demande de mbedTLS va à la mémoire générale comme avant, de même qu'un grand tampon de plus si la réserve est
// prise (eduroam qui se réauthentifie pendant un échange). Sécurité inchangée : même bibliothèque, mêmes
// vérifications ; seule change l'adresse des tampons.
#if defined(MBEDTLS_SSL_IN_BUFFER_LEN) && defined(MBEDTLS_SSL_OUT_BUFFER_LEN)
static const size_t TAMPON_TLS_OCTETS = (MBEDTLS_SSL_IN_BUFFER_LEN > MBEDTLS_SSL_OUT_BUFFER_LEN)
                                        ? MBEDTLS_SSL_IN_BUFFER_LEN : MBEDTLS_SSL_OUT_BUFFER_LEN;
#define NB_TAMPONS_TLS ((MBEDTLS_SSL_OUT_BUFFER_LEN > 8192) ? 2 : 1)   // émission courte (4 Ko) : un seul grand
#else
static const size_t TAMPON_TLS_OCTETS = 16384 + 512;   // 16 Ko de contenu + en-tête et marge de chiffrement
#define NB_TAMPONS_TLS 2
#endif
static const size_t TAMPON_TLS_MIN_OCTETS = 8192;      // demande plus petite : mémoire générale
// Avec la réserve, la mémoire générale ne sert plus qu'aux calculs de la poignée de main (blocs de 3 Ko au plus).
static const uint32_t TLS_BLOC_RESERVE_MIN_OCTETS = 6144;
static const uint32_t BLOC_MIN_RESERVE_OCTETS     = 6144;   // verifierMemoire, réserve en place

extern "C" int mbedtls_platform_set_calloc_free(void* (*)(size_t, size_t), void (*)(void*)) __attribute__((weak));
static uint8_t* tamponsTls[NB_TAMPONS_TLS] = {};
static volatile bool tamponTlsPris[NB_TAMPONS_TLS] = {};
static uint8_t nbTamponsTls = 0;                    // tampons réellement réservés (0 : fonctionnement d'avant)
static portMUX_TYPE verrouTamponsTls = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t nbGrandsHorsReserve = 0;          // grands tampons servis par la mémoire générale (réserve prise)

static void* tlsCalloc(size_t n, size_t taille) {
  if (n != 0 && taille > SIZE_MAX / n) return nullptr;
  size_t total = n * taille;
  if (total > TAMPON_TLS_MIN_OCTETS && total <= TAMPON_TLS_OCTETS) {
    for (uint8_t i = 0; i < nbTamponsTls; i++) {
      bool libre = false;
      portENTER_CRITICAL(&verrouTamponsTls);
      if (!tamponTlsPris[i]) { tamponTlsPris[i] = true; libre = true; }
      portEXIT_CRITICAL(&verrouTamponsTls);
      if (libre) { memset(tamponsTls[i], 0, total); return tamponsTls[i]; }
    }
    if (nbTamponsTls > 0) nbGrandsHorsReserve++;
  }
  return heap_caps_calloc(n, taille, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tlsFree(void* p) {
  if (p == nullptr) return;
  for (uint8_t i = 0; i < nbTamponsTls; i++) {
    if (p == tamponsTls[i]) {
      portENTER_CRITICAL(&verrouTamponsTls);
      tamponTlsPris[i] = false;
      portEXIT_CRITICAL(&verrouTamponsTls);
      return;
    }
  }
  heap_caps_free(p);   // (même mémoire que l'allocateur d'origine d'ESP-IDF)
}

uint8_t tamponsTlsLibres() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < nbTamponsTls; i++) if (!tamponTlsPris[i]) n++;
  return n;
}
uint32_t octetsReserveLibres() { return (uint32_t)tamponsTlsLibres() * TAMPON_TLS_OCTETS; }
bool reserveTlsComplete() { return nbTamponsTls == NB_TAMPONS_TLS; }

// Au tout début de setup(), avant le Wi-Fi et le Bluetooth : la mémoire est encore d'un seul tenant.
// Faux si cette version de mbedTLS ne permet pas de lui fournir sa mémoire (la carte fonctionne alors comme avant).
bool reserverTamponsTls() {
  if (mbedtls_platform_set_calloc_free == nullptr) return false;
  for (uint8_t i = 0; i < NB_TAMPONS_TLS; i++) {
    uint8_t* t = (uint8_t*)heap_caps_malloc(TAMPON_TLS_OCTETS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (t == nullptr) break;
    tamponsTls[nbTamponsTls++] = t;
  }
  if (nbTamponsTls > 0) mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  return true;
}

// Assez de mémoire pour ouvrir une connexion HTTPS ? (mémoire « 8 bits » interne : celle que mbedTLS peut utiliser)
// Réserve complète et libre : il ne faut plus, dans la mémoire générale, que la place des calculs de la poignée de
// main (même total qu'avant, réserve comprise) et des blocs de quelques Ko. Sinon : les seuils d'avant.
bool memoireTlsOK() {
  uint32_t libre = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  uint32_t bloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (reserveTlsComplete() && tamponsTlsLibres() == nbTamponsTls)
    return libre + octetsReserveLibres() >= TLS_MEMOIRE_MIN_OCTETS && bloc >= TLS_BLOC_RESERVE_MIN_OCTETS;
  return libre >= TLS_MEMOIRE_MIN_OCTETS && bloc >= TLS_BLOC_MIN_OCTETS;
}

HardwareSerial nextpmSerial(1);  // UART n°1
ModbusMaster nextpm;
MHZ19 mhz;
Preferences prefs;  // mémoire de configuration (espace "aircarto")

// ---------- Sortie texte de la carte ----------
// Tout ce que la carte écrit passe par « Console » : moniteur série (USB) + journal à distance (relais Cloudflare)
// + Bluetooth. Deux exceptions, qui ne quittent JAMAIS le câble USB :
//  - l'écho de ce que tu tapes au clavier (traiterSerie) ;
//  - tout ce qu'écrit une commande « sensible » tapée en USB (wifi ..., set ..., oublier, bt oublier) :
//    noms de réseaux, identifiant ENT, longueur des mots de passe... (voir executerCommandeSource).
volatile uint8_t sortieUsbSeule = 0;   // > 0 : ce qui s'écrit reste sur l'USB
void journalEcrire(const uint8_t* b, size_t n);   // journal à distance (section CONSOLE À DISTANCE)
void btEcrire(const uint8_t* b, size_t n);        // Bluetooth (section BLUETOOTH)
// D'où vient une commande : le câble USB peut tout ; Bluetooth et relais seulement la liste blanche.
enum SourceCommande : uint8_t { SOURCE_USB, SOURCE_BLUETOOTH, SOURCE_RELAIS };
void deposerCommande(const char* texte, uint8_t source);   // file d'attente, exécutée par loop()
void diagnosticTls();                                      // commande « diag tls » (section COMMANDES)

class SortieConsole : public Print {
 public:
  using Print::write;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    Serial.write(b, n);
    if (sortieUsbSeule == 0) {
      journalEcrire(b, n);
      btEcrire(b, n);
    }
    return n;
  }
};
SortieConsole Console;

// Écrit SEULEMENT à distance (journal + Bluetooth) : sert à y recopier les commandes tapées en USB,
// que le moniteur série affiche déjà grâce à l'écho.
void ecrireADistance(const char* texte) {
  size_t n = strlen(texte);
  journalEcrire((const uint8_t*)texte, n);
  btEcrire((const uint8_t*)texte, n);
}

// Affichage série automatique : false = la carte ne parle plus toute seule
// (commande "verbose on" / "verbose off", enregistrée en mémoire)
bool verbose = false;
#define LOG(...)  do { if (verbose) Console.printf(__VA_ARGS__); } while (0)

enum EtatCapteur { CAPTEUR_CHAUFFE, CAPTEUR_OK, CAPTEUR_ERREUR };   // CAPTEUR_CHAUFFE = 0 : état de départ (voir mCO2...)

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
// (variables globales : tout part à zéro, donc état CAPTEUR_CHAUFFE, aucune lecture, affichage pas encore initialisé)
Mesure mCO2, mPM1, mPM25, mPM10;

float tempPM = NAN;   // température interne du NextPM
float humPM  = NAN;   // humidité interne du NextPM
uint8_t statutPM = 0;

// Agrégats sur la période de mesure (moyenne + pic)
struct Agregat { float somme; uint16_t n; float maxi; };
Agregat agCO2, agPM1, agPM25, agPM10;   // (variables globales : à zéro au démarrage)

// Statistiques CO2
uint32_t nCO2Total = 0, nCO2Haut = 0;  // pour x_pct_co2_1000
float tamponCO2[12];                   // ~2 min de CO2, un point toutes les 10 s (détection d'aération)
uint8_t tamponIdx = 0, tamponN = 0;
bool aerationPeriode = false;
uint16_t histCO2[HIST_N];              // courbe : moyenne du CO2 de chacune des dernières minutes (0 = minute sans mesure)
uint8_t histIdx = 0, histN = 0;
float histSomme = 0;                   // somme des lectures CO2 de la minute en cours
uint16_t histNb = 0;                   // nombre de lectures correspondantes

// Configuration (chargée depuis la mémoire de la carte)
String cfgCle;
String cfgRelais;     // adresse du relais Cloudflare (https://....workers.dev) : « set relais », câble USB
String cfgJeton;      // jeton SECRET de la carte auprès du relais : « set jeton », câble USB
bool btAppairageDemarrage = false;   // « bt demarrage on » (câble USB) : nouveau téléphone accepté 2 min après la mise sous tension
String deviceId;      // adresse MAC, lue une fois au démarrage
uint32_t bootId = 0;  // identifiant aléatoire de ce démarrage

// Alimentation
bool estSurSecteur = true;
bool etatCandidat = true;
unsigned long tChangementSecteur = 0;

// Réseau et envoi
// REJETE : le serveur a refusé la mesure (400/413) | INVALIDE : écartée sur la carte, SANS envoi (ligne illisible,
// heure inconnue) : ne consomme pas le créneau de 10 s | ATTENDRE : l'heure NTP manque encore
// REPORTE : rien n'a été accepté pour une raison passagère côté carte ou « trop tôt » (connexion HTTPS du relais en
// cours, mémoire, refus 429) : une mesure fraîche reste en mémoire vive pour le créneau suivant, SANS passer par la
// flash. Avant, elle y était rangée : le nuage de la page ÉTAT affichait « 1 » alors que tout allait bien.
enum ResultatPush { PUSH_OK, PUSH_REESSAYER, PUSH_REJETE, PUSH_INVALIDE, PUSH_CLE_INVALIDE, PUSH_ATTENDRE, PUSH_REPORTE };
// Mesures fraîches en attente d'envoi (RAM) : loop() les dépose, la tâche d'envoi les prend.
// Protégées par mutexEnvoi. Au-delà de NB_PENDANTES, la plus ancienne est sacrifiée.
#define NB_PENDANTES 4
#define TAILLE_PENDANTE 512   // place réservée au démarrage pour chaque mesure en attente (setup)
String pendantes[NB_PENDANTES];
uint8_t nbPendantes = 0;
SemaphoreHandle_t mutexEnvoi = NULL;
volatile bool demandeVider = false;  // commande "vider_file" : la tâche d'envoi s'en charge
unsigned long tAttenteHeure = 0;     // depuis quand une mesure attend l'heure NTP (0 = personne n'attend)
unsigned long tEssaiNtp = 0;         // dernier essai d'une mesure qui attend l'heure NTP
unsigned long tDernierPush = 0;      // dernier POST : fin de sa poignée de main HTTPS (= son arrivée au serveur)
bool dejaPoste = false;              // aucun POST depuis le démarrage : le premier part tout de suite
unsigned long ecartMinMs = ECART_PUSH_MIN_MS;   // délai demandé par le serveur (next_push_in_s, 10 s au moins)
unsigned long margePushMs = MARGE_PUSH_INIT_MS; // marge ajoutée, ajustée selon les refus 429
uint8_t succesDeSuite = 0;           // envois réussis consécutifs (pour réduire la marge)
uint8_t echecsPush = 0;              // échecs réseau / serveur consécutifs (espacement progressif)
bool cleInvalide = false;  // le serveur a refusé la clé (401)
unsigned long tCleRefusee = 0;       // dernier refus 401 : nouvel essai toutes les ESSAI_CLE_MS (pas d'arrêt définitif)
uint8_t echecsReseauConnecte = 0;    // POST sans aucune réponse alors que le Wi-Fi se dit connecté (liaison « fantôme »)
volatile bool demandeReconnexion = false;  // posée par la tâche d'envoi, traitée par gererWiFi (loop)
unsigned long tDerniereReponse = 0;  // dernière réponse HTTP reçue du serveur, quelle qu'elle soit
uint8_t etatEnvoi = 0;     // 0 gris | 1 vert (OK) | 2 orange (réessai) | 3 rouge (refus)
int dernierCodeHttp = 0;
unsigned long tVisibleDepuis = 0;    // depuis quand un réseau en mémoire est visible SANS que la connexion réussisse (0 = non)
uint32_t nbCoupuresWifi = 0;
uint32_t nbSansHeure = 0;            // mesures d'un démarrage précédent abandonnées faute d'heure (commande status)
ClientTlsDate clientAircarto("AirCarto");   // connexion chiffrée vers AirCarto (certificats réglés dans setup)
unsigned long tPasAvant = 0;         // refus 429 : le serveur a demandé d'attendre jusqu'à cet instant (0 = rien)
uint32_t nbReportsMemoire = 0;       // envois reportés faute de mémoire pour HTTPS (commande status)

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
enum EtatWifi { WIFI_INACTIF, WIFI_SCAN, WIFI_CONNEXION, WIFI_REESSAI, WIFI_CONNECTE, WIFI_PAUSE };
EtatWifi etatWifi = WIFI_INACTIF;
// Point d'accès PRÉCIS vu au scan pour chaque candidat (adresse BSSID + canal), avec la bonne sécurité : la carte se
// connecte à lui et à aucun autre du même nom (sinon le pilote rescanne seul et prend le plus fort, peut-être un
// faux point d'accès ouvert nommé « eduroam »).
struct Candidat {
  int32_t canal;
  int8_t reseau;      // numéro dans reseaux[]
  uint8_t bssid[6];
  bool bssidOk;       // bssid et canal connus (vus au scan)
};
Candidat candidats[NB_WIFI_MAX];    // réseaux à essayer, dans l'ordre (le plus fort d'abord)
Candidat apCourant;                 // point d'accès de la connexion en cours (reconnexion après une coupure)
bool identiteReelle[NB_WIFI_MAX] = {false};   // eduroam : l'identité anonyme a été refusée, on envoie l'identifiant réel
// eduroam refusé (identifiant, mot de passe ou certificat) : nombre de refus de suite, et début de la pause imposée.
// Un refus ne s'arrange pas tout seul : réessayer sans arrêt occuperait la radio (le Bluetooth ne passe plus) et
// pourrait faire bloquer le compte ENT par l'université (trop d'essais ratés).
uint8_t refusEap[NB_WIFI_MAX] = {0};
unsigned long tBlocageEap[NB_WIFI_MAX] = {0};   // 0 = pas de pause
static const unsigned long WIFI_BLOCAGE_EAP_MS = 600000UL;   // 10 min (« wifi connect » réessaie tout de suite)
bool essaiSansCert = false;   // « wifi ent test oui » : UN essai sans vérifier le certificat (diagnostic)
uint8_t nbCandidats = 0, idxCandidat = 0;
uint8_t essaiCandidat = 0;          // 0 = 1er essai du candidat courant | 1 = 2e (dernier) essai
unsigned long tDebutCandidat = 0;   // début du 1er essai du candidat courant (secondes de l'écran de connexion)
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
// PAGE_AERER n'apparaît que si le CO2 est élevé, PAGE_COURBE qu'après 2 minutes de mesures, PAGE_WIFI que hors ligne.
// PAGE_ETAT n'est pas dans la rotation : elle s'intercale toutes les PERIODE_ETAT_MS (voir pageSuivante).
enum Page { PAGE_CO2, PAGE_AERER, PAGE_PM1, PAGE_PM25, PAGE_PM10, PAGE_GLOBAL, PAGE_COURBE, PAGE_WIFI, PAGE_ETAT, NB_PAGES };
uint8_t pageCourante = PAGE_CO2;
bool aRedessiner = false;  // un seul redessin par tour de loop()
bool aTransition = false;  // fondu à faire au prochain affichage (changement de page)
bool ecranEteint = false;
uint8_t luminositeCourante = 255;  // luminosité choisie par majLuminosite (le fondu y revient)
// Fondu en cours (sans bloquer loop() : majFondu() avance à chaque tour) : 0 = aucun, 1 = l'ancienne page s'éteint,
// 2 = la nouvelle s'allume. allongementPage : durée du fondu qui a amené la page courante (ajoutée à sa durée).
uint8_t phaseFondu = 0;
unsigned long tFondu = 0, demiFondu = 0, allongementPage = 0;
bool fonduCourt = false;          // prochain fondu court (~0,4 s) : logo de démarrage, fin du code Bluetooth
uint8_t pageAvantFondu = 0;       // page quittée (ÉTAT -> fondu court)

// Arrête net un fondu (alerte, code Bluetooth, écran Wi-Fi, nuit) : pleine luminosité tout de suite.
void arreterFondu() {
  if (phaseFondu != 0) reglerLuminositeEcran(luminositeCourante);
  phaseFondu = 0;
  allongementPage = 0;
}
uint8_t niveauJour = NIVEAU_JOUR_DEFAUT, niveauNuit = NIVEAU_NUIT_DEFAUT;            // 1..10 (commande « lum »)
uint8_t heureNuitDebut = HEURE_NUIT_DEBUT_DEFAUT, heureNuitFin = HEURE_NUIT_FIN_DEFAUT;  // commande « nuit »
unsigned long tAnim = 0;       // dernière image d'animation
bool etatCligne = false;       // les yeux sont-ils fermés (dernière image) ?
uint32_t phaseChauffe = 0;     // seconde affichée par le compte à rebours de préchauffage
uint8_t phasePoint = 0;          // point clignotant de la page courbe (0 / 1, change toutes les 500 ms)
// Écrans qui passent devant tout le reste (voir dessinerEcranSpecial)
volatile uint32_t codeAppairage = 0;        // code Bluetooth à taper sur le téléphone (0 = aucun)
volatile unsigned long tCodeAppairage = 0;
volatile uint8_t majProgression = 0xFF;     // téléchargement d'une mise à jour : 0..100 % (0xFF = aucune)

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
  if (a > b) std::swap(a, b);
  if (b > c) std::swap(b, c);
  if (a > b) std::swap(a, b);
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
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Le certificat de ca_eduroam.h est-il lisible ? (« vérifié » dans status seulement dans ce cas ; sinon la connexion
// eduroam échoue, elle ne s'ouvre jamais sans contrôle.)
int8_t caEduroamValide = -1;   // -1 pas de fichier | 0 illisible | 1 lisible
void verifierCaEduroam() {
#if WIFI_ACTIVE && CA_EDUROAM_PRESENT
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  caEduroamValide = (mbedtls_x509_crt_parse(&crt, (const unsigned char *)CA_EDUROAM, sizeof(CA_EDUROAM)) == 0) ? 1 : 0;
  mbedtls_x509_crt_free(&crt);
  if (!caEduroamValide) Console.println("ATTENTION : ca_eduroam.h illisible (encore l'exemple ?) : eduroam ne pourra pas se connecter.");
#endif
}

// Nom de réseau Wi-Fi prêt à afficher. Un nom de Wi-Fi peut contenir N'IMPORTE QUELS octets (retours à la ligne,
// séquences d'échappement du terminal...) : un point d'accès voisin bien nommé pourrait sinon écrire dans le moniteur
// série ou fabriquer de fausses lignes dans le journal du relais. Seul l'ASCII imprimable passe : le reste devient
// « ? » (y compris les accents et les caractères Unicode invisibles qui changent le sens d'écriture ou la ligne).
String nomAffichable(const String &s) {
  String r = s;
  for (unsigned i = 0; i < r.length(); i++) {
    uint8_t c = (uint8_t)r[i];
    if (c < 32 || c >= 127) r.setCharAt(i, '?');
  }
  return r;
}

// ---------- Dates des certificats HTTPS (voir ClientTlsDate) ----------
// Référence : la date de COMPILATION du programme (FW_VERSION), gravée dans la carte. Un certificat déjà expiré à
// la compilation est refusé. On n'utilise PAS l'heure NTP : elle n'est pas chiffrée, et quelqu'un qui peut se
// faire passer pour le serveur peut aussi la falsifier (vers le passé, elle ne protégerait de rien ; vers le futur,
// elle ferait refuser tous les certificats : panne volontaire).
static int64_t joursDepuis1970(int a, unsigned m, unsigned j) {   // calendrier grégorien (algorithme de H. Hinnant)
  a -= (m <= 2);
  const int64_t ere = (a >= 0 ? a : a - 399) / 400;
  const unsigned ae = (unsigned)(a - ere * 400);
  const unsigned jda = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + j - 1;
  const unsigned jde = ae * 365 + ae / 4 - ae / 100 + jda;
  return ere * 146097 + (int64_t)jde - 719468;
}

// Date de compilation (FW_VERSION = AAMMJJhhmm, heure locale), moins un jour de marge. 0 si inconnue.
int64_t secondesCompilation() {
  uint32_t v = (uint32_t)FW_VERSION;
  unsigned aa = v / 100000000UL, mm = (v / 1000000UL) % 100, jj = (v / 10000UL) % 100;
  unsigned hh = (v / 100UL) % 100, mi = v % 100;
  if (v == 0) return 0;
  // valeurs bornées (une version « 2611221960 » reste une date de 2026) : le contrôle n'est jamais désactivé
  mm = constrain(mm, 1u, 12u);
  jj = constrain(jj, 1u, 31u);
  hh = min(hh, 23u);
  mi = min(mi, 59u);
  return joursDepuis1970(2000 + (int)aa, mm, jj) * 86400LL + hh * 3600LL + mi * 60LL - 86400LL;
}

// Le certificat du SERVEUR (le premier envoyé) est-il encore valable ? Seul lui compte : un intermédiaire superflu
// et périmé, que certains serveurs envoient en plus, ne doit pas tout faire refuser.
bool certificatsALaDate(WiFiClientSecure &c, const char* quoi) {
  const mbedtls_x509_crt* crt = c.getPeerCertificate();
  if (crt == nullptr || crt->raw.len == 0) {
    Console.printf("%s : certificat du serveur illisible, connexion refusée.\n", quoi);
    return false;
  }
  int64_t ref = secondesCompilation();
  if (ref <= 0) return true;   // date de compilation inconnue (pas de version_fw.h) : impossible de juger
  const mbedtls_x509_time &f = crt->valid_to;
  int64_t fin = joursDepuis1970(f.year, (unsigned)f.mon, (unsigned)f.day) * 86400LL + f.hour * 3600LL + f.min * 60LL + f.sec;
  if (fin < ref) {
    Console.printf("%s : certificat EXPIRÉ le %04d-%02d-%02d, connexion refusée (faux serveur ?).\n", quoi,
                   f.year, f.mon, f.day);
    return false;
  }
  return true;
}

// Lit la réponse du serveur dans une chaîne, sans jamais dépasser max octets, qu'elle annonce sa taille ou qu'elle
// arrive par morceaux (« chunked ») : une réponse énorme ou sans fin ne peut pas épuiser la mémoire.
class FluxBorne : public Stream {
 public:
  FluxBorne(String &d, size_t m) : dest(d), max(m) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (dest.length() + n > max) { trop = true; return 0; }
    dest.concat((const char*)b, n);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  bool trop = false;
 private:
  String &dest;
  size_t max;
};

String lireReponse(HTTPClient &http, size_t max) {
  String corps;
  int annonce = http.getSize();
  if (annonce > (int)max) { LOG("Réponse ignorée : %d octets annoncés.\n", annonce); return corps; }
  FluxBorne f(corps, max);
  http.writeToStream(&f);
  if (f.trop) { LOG("Réponse ignorée : plus de %u octets.\n", (unsigned)max); corps = ""; }
  return corps;
}

// Nuit = de heureNuitDebut (inclus) à heureNuitFin (exclu), en heure locale (France, heure d'été comprise).
// Fonctionne aussi si la plage passe minuit (ex. 22 -> 7). Début = fin : jamais la nuit.
// Sans heure (pas encore de Wi-Fi depuis la mise sous tension) : la carte ne sait pas s'il fait nuit -> jour.
bool estNuit() {
  if (!heureValide() || heureNuitDebut == heureNuitFin) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour;
  if (heureNuitDebut < heureNuitFin) return (h >= heureNuitDebut && h < heureNuitFin);
  return (h >= heureNuitDebut || h < heureNuitFin);
}

// Durée d'une période de mesure (la moyenne de toutes les lectures de la période est envoyée) :
//   - en direct, rien en retard : 10 s ;
//   - hors ligne (ou clé absente / refusée) : 30 s, pour que la mémoire flash garde ~10 h ;
//   - des mesures en retard : 30 s, puis 60 s au-delà de SEUIL_RATTRAPAGE. Le serveur n'accepte qu'un envoi
//     toutes les 10 s : chaque mesure fraîche en moins laisse un créneau au retard ;
//   - sur batterie : 60 s (énergie).
bool envoiPossible();   // (plus bas, avec l'envoi)
unsigned long periodeMesureMs() {
  if (!estSurSecteur) return PERIODE_MESURE_BATT_MS;
  if (!envoiPossible()) return PERIODE_MESURE_HORS_LIGNE_MS;
  if (nbLignesAttente > SEUIL_RATTRAPAGE) return PERIODE_MESURE_RATTRAPAGE_MS;
  if (nbLignesAttente > 0) return PERIODE_MESURE_HORS_LIGNE_MS;
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
  cfgRelais  = prefs.getString("relais", "");
  cfgJeton   = prefs.getString("jeton", "");
  offsetRejeu = prefs.getULong("off", 0);
  verbose    = prefs.getBool("verbose", false);
  btAppairageDemarrage = prefs.getBool("bt_dem", false);
  niveauJour     = prefs.getUChar("lum_j", NIVEAU_JOUR_DEFAUT);
  niveauNuit     = prefs.getUChar("lum_n", NIVEAU_NUIT_DEFAUT);
  heureNuitDebut = prefs.getUChar("nuit_d", HEURE_NUIT_DEBUT_DEFAUT);
  heureNuitFin   = prefs.getUChar("nuit_f", HEURE_NUIT_FIN_DEFAUT);
  if (niveauJour < 1 || niveauJour > 10) niveauJour = NIVEAU_JOUR_DEFAUT;
  if (niveauNuit < 1 || niveauNuit > 10) niveauNuit = NIVEAU_NUIT_DEFAUT;
  if (heureNuitDebut > 23 || heureNuitFin > 23) { heureNuitDebut = HEURE_NUIT_DEBUT_DEFAUT; heureNuitFin = HEURE_NUIT_FIN_DEFAUT; }
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
// Une clé par champ de ReseauWifi, dans cet ordre : s = ssid, p = mdp, i = eapId.
static const struct { char lettre; String ReseauWifi::* champ; } CHAMPS_RESEAU[] = {
  {'s', &ReseauWifi::ssid}, {'p', &ReseauWifi::mdp}, {'i', &ReseauWifi::eapId}
};
static void cleReseau(char* buf, size_t n, char type, uint8_t i) {
  snprintf(buf, n, "w_%c%u", type, (unsigned)i);
}

void sauverReseaux() {
  char k[8];
  for (uint8_t i = 0; i < NB_WIFI_MAX; i++) { identiteReelle[i] = false; refusEap[i] = 0; tBlocageEap[i] = 0; }   // les numéros changent
  prefs.putUChar("w_n", nbReseaux);
  for (uint8_t i = 0; i < NB_WIFI_MAX; i++)
    for (const auto &c : CHAMPS_RESEAU) {
      cleReseau(k, sizeof(k), c.lettre, i);
      if (i < nbReseaux) prefs.putString(k, (reseaux[i].*c.champ).c_str());
      else prefs.remove(k);
    }
  prefs.putChar("w_last", dernierReseau);
}

// Ajoute (ou met à jour, si le nom existe déjà) un réseau. Renvoie son numéro, ou -1 si le cache est plein.
int ajouterReseau(const String &ssid, const String &mdp, const String &eapId) {
  uint8_t i = 0;
  while (i < nbReseaux && reseaux[i].ssid != ssid) i++;
  if (i == nbReseaux) {   // nom inconnu : nouveau réseau, à la fin du cache
    if (nbReseaux >= NB_WIFI_MAX) return -1;
    reseaux[nbReseaux++].ssid = ssid;
  }
  reseaux[i].mdp = mdp;
  reseaux[i].eapId = eapId;
  sauverReseaux();
  return i;
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
    if (nbReseaux > 0) Console.printf("Ancien réglage Wi-Fi repris dans le cache : %s\n", nomAffichable(reseaux[0].ssid).c_str());
    return;
  }
  nbReseaux = (n > NB_WIFI_MAX) ? NB_WIFI_MAX : n;
  for (uint8_t i = 0; i < nbReseaux; i++)
    for (const auto &c : CHAMPS_RESEAU) {
      cleReseau(k, sizeof(k), c.lettre, i);
      reseaux[i].*c.champ = prefs.getString(k, "");
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
void surDeconnexion(WiFiEvent_t, WiFiEventInfo_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r != WIFI_REASON_ASSOC_LEAVE) raisonDeco = r;  // ASSOC_LEAVE = c'est nous qui avons coupé
}

// Raisons 208 / 209 (protection des trames de gestion : la box garde l'ancienne connexion de la carte) : en chiffres,
// les noms WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG / SA_QUERY_TIMEOUT n'existent pas avant Arduino-ESP32 2.0.10.
static const uint8_t RAISON_BOX_OCCUPEE = 208, RAISON_BOX_SA_QUERY = 209;

// Texte court (écran, 6 caractères max) et explication (moniteur série) de la raison d'un échec
const char* raisonCourte(uint8_t r) {
  switch (r) {
    case 0:                                  return "DELAI";
    case WIFI_REASON_NO_AP_FOUND:            return "ABSENT";
    case WIFI_REASON_BEACON_TIMEOUT:         return "PERDU";
    case WIFI_REASON_AUTH_EXPIRE:            // la box n'a pas répondu à temps (fréquent au 1er essai après un redémarrage)
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "DELAI";
    case RAISON_BOX_OCCUPEE:
    case RAISON_BOX_SA_QUERY:                return "OCCUPE";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "MDP ?";
    default:                                 return "REFUS";
  }
}

const char* raisonLongue(uint8_t r) {
  switch (r) {
    case 0:                                  return "pas de réponse dans le délai";
    case WIFI_REASON_NO_AP_FOUND:            return "réseau introuvable (hors de portée ? nom exact ?)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "signal perdu (borne éteinte ou trop loin)";
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "identifiant ou mot de passe ENT refusé (ou ca_eduroam.h faux, ou identité anonyme refusée : EDUROAM_IDENTITE_ANONYME 0)";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "la box n'a pas répondu à temps (fréquent au 1er essai après un redémarrage)";
    case RAISON_BOX_OCCUPEE:
    case RAISON_BOX_SA_QUERY:                return "la box garde encore l'ancienne connexion de la carte (après un redémarrage)";
    case WIFI_REASON_AUTH_FAIL:              return "mot de passe refusé (ou mauvais type : wifi add / wifi ent ?)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "clé refusée : mot de passe faux, ou box pas encore prête (fréquent au 1er essai après un redémarrage)";
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
  // Point d'accès précis vu au scan (sinon : n'importe lequel de ce nom, seulement pour un Wi-Fi à mot de passe)
  // 2e essai d'un Wi-Fi à mot de passe : n'importe quel point d'accès de ce nom (la box a pu changer de canal depuis
  // le scan, et le pilote rescanne alors tous les canaux). Un réseau entreprise reste attaché au point d'accès vu.
  bool avecBssid = (idxCandidat < nbCandidats && candidats[idxCandidat].reseau == i && candidats[idxCandidat].bssidOk)
                   && !(essaiCandidat > 0 && !estEntreprise(i));
  if (avecBssid) apCourant = candidats[idxCandidat];
  else apCourant.bssidOk = false;
  reseauCourant = i;
  WiFi.disconnect(false, false);
  delay(50);
  raisonDeco = 0;
  bool entreprise = r.eapId.length() > 0;
  if (entreprise) {
    // WPA2 Entreprise (eduroam) : identité + identifiant + mot de passe ENT, puis on active le mode entreprise.
    // Certificat de l'autorité du serveur d'authentification (ca_eduroam.h) : la carte refuse un faux serveur, qui
    // récupérerait sinon l'empreinte du mot de passe ENT. Pas de vérification des dates : la carte n'a pas encore
    // l'heure (elle arrive par Internet, une fois connectée). Limite d'ESP32 Arduino 2.x : le NOM du serveur n'est
    // pas vérifié, seulement l'autorité qui l'a signé.
#if CA_EDUROAM_PRESENT
    if (essaiSansCert) esp_wifi_sta_wpa2_ent_clear_ca_cert();   // diagnostic demandé (« wifi ent test oui »)
    else esp_wifi_sta_wpa2_ent_set_ca_cert((const unsigned char *)CA_EDUROAM, sizeof(CA_EDUROAM));
    esp_wifi_sta_wpa2_ent_set_disable_time_check(true);
#endif
    // Identité « extérieure », lisible par tous en clair : anonymous@domaine (EDUROAM_IDENTITE_ANONYME) ; le vrai
    // identifiant (set_username) ne part que dans le tunnel chiffré. (ESP-IDF copie la chaîne.)
    String exterieure = r.eapId;
#if EDUROAM_IDENTITE_ANONYME
    // (si l'université refuse l'identité anonyme, la carte repasse seule à l'identifiant réel : voir gererWiFi)
    int arobase = r.eapId.indexOf('@');
    // « anonymous » + la fin de l'identifiant à partir de l'arobase (bâtie sur place, sans chaînes intermédiaires)
    if (arobase >= 0 && !identiteReelle[i]) { exterieure = "anonymous"; exterieure += r.eapId.c_str() + arobase; }
#endif
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)exterieure.c_str(), exterieure.length());
    esp_wifi_sta_wpa2_ent_set_username((uint8_t *)r.eapId.c_str(), r.eapId.length());
    esp_wifi_sta_wpa2_ent_set_password((uint8_t *)r.mdp.c_str(), r.mdp.length());
    // Méthode interne d'eduroam AMU : TTLS-PAP (voir ca_eduroam.h). Sans cette ligne, ESP-IDF propose MSCHAPv2 par défaut
    // dans le tunnel TTLS : le serveur le refuse, d'où « identifiant ou mot de passe refusé » alors qu'ils sont bons.
    // (sans effet si le serveur choisit PEAP)
    esp_wifi_sta_wpa2_ent_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_PAP);
    esp_wifi_sta_wpa2_ent_enable();
  } else {
    // Wi-Fi classique : le mode entreprise DOIT être coupé, sinon la box refuse la connexion
    esp_wifi_sta_wpa2_ent_disable();
  }
  // (mot de passe donné ici seulement pour un Wi-Fi classique : celui de l'ENT est déjà passé au mode entreprise)
  WiFi.begin(r.ssid.c_str(), (!entreprise && r.mdp.length()) ? r.mdp.c_str() : nullptr,
             avecBssid ? apCourant.canal : 0, avecBssid ? apCourant.bssid : nullptr);
  etatWifi = WIFI_CONNEXION;
  tEtatWifi = millis();
  if (essaiCandidat == 0) tDebutCandidat = tEtatWifi;
  aRedessiner = true;
  if (wifiInteractif || verbose)
    Console.printf("Wi-Fi : connexion à %s (%s, réseau %u/%u, essai %u/2)...\n", nomAffichable(r.ssid).c_str(), typeReseau(i),
                  (unsigned)(idxCandidat + 1), (unsigned)nbCandidats, (unsigned)(essaiCandidat + 1));
#endif
}

// Connexion ou 2e essai en cours (écran de connexion, page Wi-Fi)
bool wifiEnConnexion() {
  return etatWifi == WIFI_CONNEXION || etatWifi == WIFI_REESSAI;
}

// Essais ratés en arrière-plan (sans écran de connexion ni « verbose on ») : une ligne au journal quand la raison
// change, au plus une toutes les 10 min sinon. On voit ainsi, aussi à distance, pourquoi la carte ne se connecte pas.
void journaliserEchecWifi(uint8_t r) {
  static uint8_t rPrec = 0xFF;
  static unsigned long tPrec = 0;
  if (wifiInteractif || verbose || reseauCourant < 0) return;   // déjà écrit par gererWiFi
  unsigned long now = millis();
  if (r == rPrec && tPrec != 0 && now - tPrec < WIFI_LOG_ECHEC_MS) return;
  rPrec = r;
  tPrec = now | 1UL;
  Console.printf("Wi-Fi : essai raté sur %s : %s (code %u). Nouvel essai automatique.\n",
                 nomAffichable(reseaux[reseauCourant].ssid).c_str(), raisonLongue(r), (unsigned)r);
}

// Lance une recherche des réseaux visibles (asynchrone : la carte continue de tourner)
void lancerScan() {
#if WIFI_ACTIVE
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  // Écoute courte sur chaque canal : la radio est partagée avec le Bluetooth, qui ne peut ni diffuser ni répondre
  // pendant un scan (un téléphone qui se connecte à ce moment-là échoue : « gatt status 133 »).
  WiFi.scanNetworks(true, false, false, WIFI_SCAN_MS_CANAL);
  etatWifi = WIFI_SCAN;
  tEtatWifi = millis();
  aRedessiner = true;
#endif
}

// Le réseau vu au scan a-t-il la sécurité du réseau enregistré ? Un faux point d'accès OUVERT nommé « eduroam » ou
// comme ta box est ainsi ignoré (pour un réseau entreprise, la carte n'exigerait sinon aucun chiffrement).
bool securiteCompatible(uint8_t i, wifi_auth_mode_t m) {
  if (estEntreprise(i)) return m == WIFI_AUTH_WPA2_ENTERPRISE;
  if (reseaux[i].mdp.length() == 0) return m == WIFI_AUTH_OPEN;
  return m != WIFI_AUTH_OPEN && m != WIFI_AUTH_WEP && m != WIFI_AUTH_WPA2_ENTERPRISE;
}

// Réseau qu'on peut essayer SANS l'avoir vu au scan : seulement un Wi-Fi à mot de passe (le pilote exige alors au
// moins WPA, voir setMinSecurity). Un réseau entreprise ou ouvert doit avoir été vu avec la bonne sécurité.
bool essayableSansScan(uint8_t i) {
  return !estEntreprise(i) && reseaux[i].mdp.length() > 0;
}

bool eapEnPause(uint8_t i);   // (plus bas : réseau entreprise en pause après des refus)

// Range les réseaux connus visibles du plus fort au plus faible. n < 0 : le scan a échoué.
void preparerCandidats(int n) {
  nbCandidats = 0;
  idxCandidat = 0;
  essaiCandidat = 0;
  rescannerApresEchec = false;
  for (uint8_t k = 0; k < NB_WIFI_MAX; k++) candidats[k].bssidOk = false;
  if (n < 0) {  // scan raté : on essaie les Wi-Fi à mot de passe, le dernier réseau qui a marché d'abord
    if (dernierReseau >= 0 && essayableSansScan(dernierReseau)) candidats[nbCandidats++].reseau = dernierReseau;
    for (uint8_t i = 0; i < nbReseaux; i++)
      if ((int8_t)i != dernierReseau && essayableSansScan(i)) candidats[nbCandidats++].reseau = i;
    return;
  }
  int32_t force[NB_WIFI_MAX];
  for (uint8_t i = 0; i < nbReseaux; i++) {
    if (estEntreprise(i) && eapEnPause(i)) continue;   // refusé récemment : on n'insiste pas
    if (essaiSansCert && !estEntreprise(i)) continue;   // test du certificat : seulement les réseaux entreprise
    // Point d'accès le plus fort de ce nom, avec la bonne sécurité. Les fiches du scan sont lues sur place (ce que
    // renvoient WiFi.SSID(j), RSSI(j)...) : sans fabriquer une String pour chaque nom vu, à chaque réseau connu.
    const wifi_ap_record_t* meilleurAp = nullptr;
    for (int j = 0; j < n; j++) {
      const wifi_ap_record_t* ap = (const wifi_ap_record_t*)WiFi.getScanInfoByIndex(j);
      if (ap != nullptr && reseaux[i].ssid == (const char*)ap->ssid && securiteCompatible(i, ap->authmode) &&
          (meilleurAp == nullptr || ap->rssi > meilleurAp->rssi))
        meilleurAp = ap;
    }
    if (meilleurAp == nullptr) continue;
    int32_t meilleur = meilleurAp->rssi;
    if ((int8_t)i == dernierReseau) meilleur += 5;  // petit bonus : à force égale, on garde ses habitudes
    // insertion triée (du plus fort au plus faible)
    uint8_t pos = nbCandidats;
    while (pos > 0 && force[pos - 1] < meilleur) {
      force[pos] = force[pos - 1];
      candidats[pos] = candidats[pos - 1];
      pos--;
    }
    force[pos] = meilleur;
    Candidat &c = candidats[pos];
    c.reseau = i;
    memcpy(c.bssid, meilleurAp->bssid, 6);
    c.canal = meilleurAp->primary;
    c.bssidOk = true;   // (une fiche du scan a toujours son BSSID)
    nbCandidats++;
  }
  // Un réseau connu est-il visible ? (sert à surveillerWifiLongue : visible mais connexion impossible = pile bloquée)
  if (nbCandidats > 0) { if (tVisibleDepuis == 0) tVisibleDepuis = millis() | 1UL; }
  else tVisibleDepuis = 0;
  // Rien de visible : peut-être un réseau caché, on tente le dernier réseau qui a fonctionné (Wi-Fi à mot de passe)
  if (nbCandidats == 0 && dernierReseau >= 0 && essayableSansScan(dernierReseau)) candidats[nbCandidats++].reseau = dernierReseau;
  if (wifiInteractif || verbose)
    Console.printf("Wi-Fi : %d réseaux visibles, %u connus parmi eux.\n", n, (unsigned)nbCandidats);
  // Les Wi-Fi à mot de passe que le scan n'a PAS vus (réseau caché, borne un peu loin, scan incomplet) passent en fin de
  // liste, essayés « à l'aveugle » après les réseaux visibles. Avant, dès qu'UN réseau connu était visible (eduroam), les
  // autres n'étaient jamais essayés, même si le premier échouait.
  for (uint8_t i = 0; i < nbReseaux && !essaiSansCert; i++) {
    if (!essayableSansScan(i)) continue;
    bool deja = false;
    for (uint8_t k = 0; k < nbCandidats; k++) if (candidats[k].reseau == i) deja = true;
    if (!deja) { candidats[nbCandidats].reseau = i; candidats[nbCandidats].bssidOk = false; nbCandidats++; }
  }
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

// Pause entre deux séries d'essais quand aucun réseau connu ne répond : 30 s, 60 s, puis 2 min. Chaque série commence
// par un scan, qui occupe la radio (le Bluetooth en pâtit) : inutile de scanner toutes les 30 s une borne éteinte.
uint8_t nbPausesWifi = 0;
unsigned long pauseWifiMs() {
  unsigned long p = WIFI_PAUSE_MS << nbPausesWifi;
  return p < WIFI_PAUSE_MAX_MS ? p : WIFI_PAUSE_MAX_MS;
}
bool btOccupe(unsigned long now);   // section BLUETOOTH : un téléphone est en train de se connecter (voir plus bas)

// Réseau entreprise (eduroam) refusé : on alterne identité anonyme et identifiant réel (certaines universités
// refusent l'identité anonyme, ou son domaine). Avant, la carte ne passait à l'identifiant réel que si DEUX bornes
// différentes avaient refusé : à poste fixe, c'est presque toujours la même borne (la plus forte), et ce repli
// n'arrivait jamais. Un faux point d'accès qui refuserait exprès n'obtient que ton identifiant, jamais ton mot de
// passe : il ne part que dans le tunnel chiffré, vers un serveur dont le certificat est vérifié (ca_eduroam.h).
// Deux refus de suite (les deux identités) : pause de WIFI_BLOCAGE_EAP_MS sur ce réseau.
void refusEntreprise(int8_t i, unsigned long now) {
  if (essaiSansCert) {
    essaiSansCert = false;
    Console.println("eduroam : refusé MÊME sans vérifier le certificat. Le certificat n'y est pour rien : vérifie "
                    "l'identifiant (identifiant@etablissement.fr, en entier) et le mot de passe de l'ENT.");
  }
#if EDUROAM_IDENTITE_ANONYME
  identiteReelle[i] = !identiteReelle[i];
  if (identiteReelle[i]) Console.println("eduroam : refusé avec l'identité anonyme, prochain essai avec ton identifiant réel.");
#endif
  if (++refusEap[i] < 2) return;
  refusEap[i] = 0;
  tBlocageEap[i] = now | 1UL;
  Console.printf("eduroam : refusé deux fois de suite (identifiant, mot de passe ENT ou certificat). Pause de %lu min "
                 "sur ce réseau, pour ne pas faire bloquer ton compte ENT ni occuper la radio du Bluetooth. "
                 "'wifi connect' : réessayer ; 'wifi ent test' : savoir si le certificat est en cause.\n",
                 WIFI_BLOCAGE_EAP_MS / 60000UL);
}

// Réseau entreprise en pause après des refus (sauf essai demandé à la main : « wifi connect », réseau ajouté...)
bool eapEnPause(uint8_t i) {
  if (tBlocageEap[i] == 0) return false;
  if (wifiInteractif || millis() - tBlocageEap[i] >= WIFI_BLOCAGE_EAP_MS) { tBlocageEap[i] = 0; return false; }
  return true;
}

void connexionReussie() {
  etatWifi = WIFI_CONNECTE;
  if (reseauCourant >= 0) { refusEap[reseauCourant] = 0; tBlocageEap[reseauCourant] = 0; }
  if (essaiSansCert && reseauCourant >= 0 && estEntreprise(reseauCourant)) {
    Console.println("eduroam : connecté SANS vérifier le certificat. C'est donc ca_eduroam.h qui bloque (l'université a "
                    "changé d'autorité ?) : il faut le mettre à jour (voir ca_eduroam.h.exemple). La prochaine connexion "
                    "vérifiera de nouveau le certificat.");
  }
  essaiSansCert = false;
  nbPausesWifi = 0;
  tVisibleDepuis = 0;
  if (dernierReseau != reseauCourant) {
    dernierReseau = reseauCourant;
    prefs.putChar("w_last", dernierReseau);
  }
  configTzTime(TZ_FRANCE, NTP_SERVER);  // heure : indispensable pour dater les mesures stockées hors ligne
  Console.printf("Wi-Fi connecté à %s | IP %s | %d dBm\n", nomAffichable(WiFi.SSID()).c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  montrerResultatWifi(true);
}

// Tous les candidats ont échoué
void finTentatives(unsigned long now) {
  if (essaiSansCert && !rescannerApresEchec) {
    essaiSansCert = false;
    Console.println("eduroam : test sans certificat impossible (réseau introuvable ou sans réponse). Réessaie près d'une borne.");
  }
  if (rescannerApresEchec) { lancerScan(); return; }  // coupure : le même réseau ne répond plus, on cherche ailleurs
  WiFi.disconnect(false, false);
  if (wifiInteractif || verbose)
    Console.printf("Wi-Fi : aucun réseau connu joignable. Nouvel essai dans %lu s ('wifi scan' pour voir les réseaux).\n",
                  pauseWifiMs() / 1000UL);
  montrerResultatWifi(false);   // (remet aussi wifiInteractif à false)
  etatWifi = WIFI_PAUSE;
  tEtatWifi = now;
  aRedessiner = true;
}

// Relance tout (démarrage, réseau ajouté / supprimé, commande "wifi connect")
void relancerWifi(bool interactif) {
#if WIFI_ACTIVE
  resultatWifiAffiche = false;
  nbPausesWifi = 0;           // (pause de nouveau à 30 s)
  tVisibleDepuis = 0;         // le compte à rebours d'1 h repart (nouveau réseau, nouveau mot de passe : on lui laisse sa chance)
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
  // Canal Wi-Fi de 20 MHz (et non 40) : le Bluetooth partage la radio, et avec un canal de 40 MHz certains téléphones
  // Android ne voient plus la carte (avis Espressif AR2024-001). Une mesure toutes les 10 s n'a pas besoin de plus.
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  WiFi.disconnect(false, true);        // efface un ancien réseau mémorisé par le pilote (anciennes versions du programme)
  WiFi.setAutoReconnect(false);        // la reconnexion est gérée par gererWiFi (sinon les deux se marchent dessus)
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);         // accepte aussi les vieilles box en WPA (pas seulement WPA2)
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);      // eduroam a plein de bornes : on prend la plus forte
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent(surDeconnexion, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (nbReseaux == 0) {
    Console.println("Aucun Wi-Fi en mémoire : tape 'wifi add <nom> <mdp>' ou 'wifi ent <nom> <identifiant> <mdp>'.");
  }
  relancerWifi(true);
#else
  Console.println("WiFi désactivé (WIFI_ACTIVE 0) : mode hors ligne.");
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
      // Scan raté ou bloqué (la bibliothèque abandonne au bout de 6 s, le pilote peut encore scanner) : on l'arrête
      // vraiment, sinon la connexion qui suit tombe en plein scan et échoue.
      if (n < 0) { esp_wifi_scan_stop(); WiFi.scanDelete(); n = -1; }
      preparerCandidats(n);
      if (nbCandidats == 0) { finTentatives(now); return; }
      lancerConnexion(candidats[0].reseau);
      return;
    }

    case WIFI_CONNEXION: {
      if (WiFi.status() == WL_CONNECTED) { connexionReussie(); return; }
      // Le pilote a donné la raison de l'échec (réseau absent, refus, délai de la box...) : la tentative est finie
      // (reconnexion automatique coupée), inutile d'attendre la fin du délai. Sinon : délai écoulé sans nouvelles.
      uint8_t r = raisonDeco;
      if (r == 0 && now - tEtatWifi < delaiConnexion(reseauCourant)) return;
      if (wifiInteractif || verbose)
        Console.printf("Wi-Fi : échec sur %s (essai %u/2) : %s (code %u)\n", nomAffichable(reseaux[reseauCourant].ssid).c_str(),
                      (unsigned)(essaiCandidat + 1), raisonLongue(r), (unsigned)r);
      else journaliserEchecWifi(r);
      if (r == WIFI_REASON_802_1X_AUTH_FAILED && estEntreprise(reseauCourant)) refusEntreprise(reseauCourant, now);
      // 1er essai raté : le MÊME réseau est retenté 2 s plus tard. Après un redémarrage de la carte, la box croit
      // encore l'ancienne connexion ouverte et refuse souvent le 1er essai (délai, « occupé »), le 2e passe. C'est
      // ce que faisait à la main « wifi connect » ; avant, la carte attendait 30 s, puis ne refaisait qu'UN essai,
      // refusé à son tour (le pilote ne retente plus seul : reconnexion automatique coupée).
      WiFi.disconnect(false, false);
      // (sauf réseau introuvable alors qu'il n'avait pas été vu au scan : essai « à l'aveugle », inutile de doubler)
      if (essaiCandidat == 0 && !(r == WIFI_REASON_NO_AP_FOUND && !apCourant.bssidOk)) {
        etatWifi = WIFI_REESSAI;
        tEtatWifi = now;
        return;
      }
      essaiCandidat = 0;
      if (++idxCandidat < nbCandidats) { lancerConnexion(candidats[idxCandidat].reseau); return; }
      finTentatives(now);
      return;
    }

    case WIFI_REESSAI:
      // (un téléphone en train de se connecter en Bluetooth : on lui laisse la radio quelques secondes)
      if (now - tEtatWifi < WIFI_REESSAI_MS || btOccupe(now)) return;
      essaiCandidat = 1;
      lancerConnexion(candidats[idxCandidat].reseau);
      return;

    case WIFI_CONNECTE:
      if (demandeReconnexion) {   // connecté en apparence, mais plus rien ne passe : on refait la connexion
        demandeReconnexion = false;
        nbCoupuresWifi++;
        Console.println("Wi-Fi connecté mais le serveur ne répond plus : reconnexion...");
        relancerWifi(false);
        return;
      }
      if (WiFi.status() == WL_CONNECTED) return;
      nbCoupuresWifi++;
      Console.printf("Wi-Fi perdu (%s) : reconnexion...\n", raisonLongue(raisonDeco));
      // on retente d'abord le même réseau ; s'il ne revient pas, on rescanne
      nbCandidats = 1;
      idxCandidat = 0;
      essaiCandidat = 0;
      candidats[0] = apCourant;           // même point d'accès qu'avant la coupure
      candidats[0].reseau = reseauCourant;
      lancerConnexion(reseauCourant);
      rescannerApresEchec = true;
      return;

    case WIFI_PAUSE:
      if (now - tEtatWifi < pauseWifiMs() || btOccupe(now)) return;
      if (pauseWifiMs() < WIFI_PAUSE_MAX_MS) nbPausesWifi++;   // la prochaine pause sera plus longue
      lancerScan();
      return;
  }
#endif
}

// Toutes les 15 s : un réseau connu VISIBLE depuis 1 h sans que la connexion réussisse = la pile Wi-Fi est sans doute
// bloquée : dernier recours, on redémarre la carte. Si aucun réseau connu n'est visible (borne éteinte la nuit, carte
// déplacée), redémarrer ne servirait à rien : avant, la carte redémarrait alors toutes les heures toute la nuit.
// (le compte part du moment où le réseau réapparaît : la carte a le temps de s'y connecter normalement)
void surveillerWifiLongue(unsigned long now) {
#if WIFI_ACTIVE
  if (nbReseaux == 0 || etatWifi == WIFI_CONNECTE) return;
  bool bloque = tVisibleDepuis != 0 && now - tVisibleDepuis > WIFI_REBOOT_MS;
  // mot de passe / identifiant refusé : un redémarrage ne le corrigerait pas non plus
  uint8_t r = raisonDeco;
  bool refusIdentifiants = (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_802_1X_AUTH_FAILED ||
                            r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
  if (bloque && !refusIdentifiants) {
    Console.println("Wi-Fi visible mais connexion impossible depuis 1 h : redémarrage de la carte.");
    delay(100);
    ESP.restart();
  }
#endif
}

// Une mémoire qui fond en continu (fuite) finit par planter : mieux vaut redémarrer
// Dernier recours : Wi-Fi « connecté » depuis 6 h sans AUCUNE réponse du serveur alors que la carte essaie d'envoyer
// (même après les reconnexions) : on redémarre. Les mesures en attente sont en flash, l'heure survit au redémarrage.
void surveillerEnvois(unsigned long now) {
#if WIFI_ACTIVE && !MODE_DEMO
  if (etatWifi != WIFI_CONNECTE || !dejaPoste || !cleOK()) return;
  if (now - tDerniereReponse > SANS_REPONSE_REBOOT_MS && now - tDernierPush < 600000UL) {
    Console.println("Aucune réponse du serveur depuis 6 h malgré le Wi-Fi : redémarrage de la carte.");
    delay(100);
    ESP.restart();
  }
#endif
}

// Cause du dernier redémarrage, en clair (numéros d'esp_reset_reason, ESP-IDF 4.4)
const char* texteCauseRedemarrage(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "mise sous tension";
    case ESP_RST_EXT:       return "bouton reset";
    case ESP_RST_SW:        return "redémarrage voulu par le programme";
    case ESP_RST_PANIC:     return "plantage";
    case ESP_RST_INT_WDT:   return "programme bloqué (chien de garde des interruptions)";
    case ESP_RST_TASK_WDT:  return "programme bloqué (chien de garde des tâches)";
    case ESP_RST_WDT:       return "blocage grave (chien de garde matériel)";
    case ESP_RST_DEEPSLEEP: return "réveil de veille profonde";
    case ESP_RST_BROWNOUT:  return "baisse de tension";
    case ESP_RST_SDIO:      return "SDIO (jamais sur cette carte)";
    default:                return "inconnue";
  }
}

// Pendant une connexion HTTPS (AirCarto, relais, mise à jour), mbedTLS occupe ~47 Ko : il ne reste qu'environ 17 Ko
// libres et un plus grand bloc d'environ 12 Ko (« diag tls »). Ce n'est ni une fuite ni un morcellement : la mémoire
// revient à la fermeture. On ne juge donc la mémoire que hors connexion (verrou mutexTls libre), et seulement après
// 3 constats de suite (45 s), pour ne pas redémarrer sur un creux passager.
void verifierMemoire() {
  static uint8_t constats = 0;
  if (mutexTls && xSemaphoreTake(mutexTls, 0) != pdTRUE) return;   // connexion HTTPS en cours : on ne juge pas
  // La réserve HTTPS (libre hors connexion) compte comme mémoire disponible ; avec elle, la mémoire générale n'a
  // plus besoin de blocs de 17 Ko : le seuil de morcellement descend à BLOC_MIN_RESERVE_OCTETS.
  uint32_t bloc = ESP.getMaxAllocHeap(), libre = ESP.getFreeHeap() + octetsReserveLibres();
  if (mutexTls) xSemaphoreGive(mutexTls);
  uint32_t blocMin = reserveTlsComplete() ? BLOC_MIN_RESERVE_OCTETS : BLOC_MIN_OCTETS;
  if (bloc >= blocMin && libre >= HEAP_MIN_OCTETS) { constats = 0; return; }
  if (++constats < 3) return;
  if (bloc < blocMin)
    Console.printf("Mémoire morcelée (plus grand bloc libre : %u octets) : redémarrage.\n", (unsigned)bloc);
  else
    Console.printf("Mémoire libre trop basse (%u octets) : redémarrage.\n", (unsigned)libre);
  delay(100);
  ESP.restart();
}

// =====================================================================
// CAPTEURS
// =====================================================================
// Met à jour une Mesure après une tentative de lecture.
// Lecture réussie : filtre médian sur les 3 dernières valeurs (écarte une valeur aberrante isolée).
// Lecture ratée : la valeur n'est plus considérée à jour, et après ECHECS_AVANT_PANNE échecs de suite (~10 s)
// le capteur passe en erreur.
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
    if (m.echecs < 255) m.echecs++;
    if (m.echecs >= ECHECS_AVANT_PANNE) m.etat = CAPTEUR_ERREUR;  // tolère quelques échecs isolés
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

// Renvoie -1 si le capteur n'a pas répondu.
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
    if (mPM25.etat == CAPTEUR_ERREUR) { tempPM = NAN; humPM = NAN; }   // plus de vieilles valeurs envoyées en boucle
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
// Le capteur est-il resté alimenté pendant le redémarrage ? (redémarrage logiciel, watchdog, plantage :
// seul l'ESP32 repart, le MH-Z19C est déjà chaud). Dans ce cas, pas de nouveau préchauffage de 3 min.
bool capteursDejaChauds() {
  esp_reset_reason_t r = esp_reset_reason();
  return r == ESP_RST_SW || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

bool co2HorsPlage = false;        // la dernière lecture écartée l'était pour une valeur impossible
uint32_t nbCO2HorsPlage = 0;      // lectures écartées depuis le démarrage (commande status)
uint32_t nbCO2NonConfirmes = 0;   // lectures > CO2_CONFIRMATION_PPM ignorées faute de confirmation (commande status)

// Lecture au-dessus de CO2_CONFIRMATION_PPM : est-elle confirmée ? On garde les dernières lectures hautes CONSÉCUTIVES ;
// confirmé = CO2_NB_CONFIRMATION lectures de suite au-dessus du seuil, toutes à ± CO2_ECART_CONFIRMATION de leur
// médiane (des valeurs « pareilles »). Une lecture sous le seuil remet la série à zéro.
bool co2HautConfirme(int co2) {
  static int serie[CO2_NB_CONFIRMATION];
  static uint8_t n = 0;
  if (co2 <= CO2_CONFIRMATION_PPM) { n = 0; return true; }   // pas concerné
  if (n == CO2_NB_CONFIRMATION) {                              // série pleine : on décale (la plus ancienne sort)
    for (uint8_t i = 1; i < n; i++) serie[i - 1] = serie[i];
    n--;
  }
  serie[n++] = co2;
  if (n < CO2_NB_CONFIRMATION) return false;
  int tri[CO2_NB_CONFIRMATION];
  memcpy(tri, serie, sizeof(tri));
  for (uint8_t i = 1; i < n; i++)                              // tri par insertion (5 valeurs)
    for (uint8_t j = i; j > 0 && tri[j - 1] > tri[j]; j--) std::swap(tri[j], tri[j - 1]);
  float med = tri[n / 2];
  return (tri[0] >= med * (1.0f - CO2_ECART_CONFIRMATION)) && (tri[n - 1] <= med * (1.0f + CO2_ECART_CONFIRMATION));
}

// Dernier code d'erreur de la bibliothèque MH-Z19 (RESULT_*), affiché dans le journal et par « status ».
uint8_t codeMhz = RESULT_OK;
const char* texteCodeMhz(uint8_t c) {
  switch (c) {
    case RESULT_NULL:    return "aucune trame reçue : fils RX/TX, masse ou alimentation 5 V du capteur";
    case RESULT_TIMEOUT: return "délai dépassé : le capteur n'a pas répondu à temps";
    case RESULT_MATCH:   return "réponse qui ne correspond pas à la demande";
    case RESULT_CRC:     return "trame abîmée (somme de contrôle fausse) : fils trop longs ou parasites";
    case RESULT_FILTER:  return "mesure filtrée par le capteur (en préchauffage)";
    default:             return "valeur valide mais 0 ppm";
  }
}

void lireMHZ() {
#if MHZ_ACTIVE
  // (drapeau : sans lui, millis() qui repasse à 0 au bout de 49 jours relancerait 3 min de "préchauffage")
  static bool chauffeFinie = capteursDejaChauds();
  if (!chauffeFinie) {
    if (millis() < CHAUFFE_MHZ_MS) { forcerEtat(mCO2, CAPTEUR_CHAUFFE); return; }
    chauffeFinie = true;
  }
  int co2 = mhz.getCO2();
  bool repond = (mhz.errorCode == RESULT_OK && co2 > 0);
  codeMhz = (uint8_t)mhz.errorCode;
  if (!repond) LOG("CO2 : le MH-Z19C ne répond pas (code %u : %s)\n", (unsigned)codeMhz, texteCodeMhz(codeMhz));
  bool plausible = repond && co2 >= CO2_MIN_PLAUSIBLE && co2 <= CO2_MAX_PLAUSIBLE;
  if (repond && !plausible) {
    nbCO2HorsPlage++;
    LOG("CO2 = %d ppm : valeur impossible, lecture écartée (capteur à recalibrer ?)\n", co2);
  }
  if (repond) co2HorsPlage = !plausible;
  if (plausible && !co2HautConfirme(co2)) {
    // très élevé mais pas (encore) confirmé : ni affiché, ni moyenné, ni envoyé. Ce n'est pas une panne.
    nbCO2NonConfirmes++;
    mCO2.aJour = false;
    LOG("CO2 = %d ppm : au-dessus de %d ppm, en attente de confirmation\n", co2, CO2_CONFIRMATION_PPM);
    return;
  }
  majMesure(mCO2, plausible, (float)co2);
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
// Un capteur en panne n'est relu que toutes les LECTURE_EN_PANNE_MS : chaque lecture ratée bloque jusqu'à la fin
// du délai de réponse, ce qui figerait loop() presque en permanence avec une lecture par seconde.
void lireCapteurs() {
  unsigned long now = millis();
#if MODE_DEMO
  lectureDemo();
#else
  static unsigned long tCO2 = 0, tPM = 0;
  static bool premiere = true;
  if (premiere || mCO2.etat != CAPTEUR_ERREUR || now - tCO2 >= LECTURE_EN_PANNE_MS) { tCO2 = now; lireMHZ(); }
  else mCO2.aJour = false;
  if (premiere || mPM25.etat != CAPTEUR_ERREUR || now - tPM >= LECTURE_EN_PANNE_MS) { tPM = now; lireNextPM(); }
  else { mPM1.aJour = false; mPM25.aJour = false; mPM10.aJour = false; }
  premiere = false;
#endif
  // journal : une ligne toutes les 10 s (une par seconde rendrait le moniteur illisible)
  static unsigned long tLog = 0;
  if (now - tLog < 10000UL) return;
  tLog = now;
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
// Le tampon garde une valeur toutes les 10 s (12 valeurs = 2 min), quel que soit le rythme de lecture.
void detecterAeration(float co2) {
  static unsigned long tPoint = 0;
  static bool init = false;
  unsigned long now = millis();
  if (init && now - tPoint < 10000UL) return;
  init = true;
  tPoint = now;
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

// Fin d'un compactage (backlog.tmp COMPLET, drapeau "cpt" levé) : suppression de l'ancien, renommage, puis le renvoi
// repart du début de la copie. Sert aussi au démarrage après une coupure : une étape déjà faite est sautée.
static void terminerCompactage() {
  if (LittleFS.exists(FICHIER_TMP)) {
    if (LittleFS.exists(FICHIER_BACKLOG)) LittleFS.remove(FICHIER_BACKLOG);
    LittleFS.rename(FICHIER_TMP, FICHIER_BACKLOG);
  }
  offsetRejeu = 0;
  prefs.putULong("off", 0);
  prefs.putBool("cpt", false);
}

// Au démarrage : reprend un compactage interrompu, puis retrouve la taille du fichier et le nombre de lignes à renvoyer
void initialiserBacklog() {
  tailleBacklog = 0;
  nbLignesAttente = 0;
  if (!fsOK) return;
  // Coupure pendant un compactage ? Le drapeau "cpt" n'est levé qu'une fois backlog.tmp COMPLET :
  // on termine le travail (suppression de l'ancien, renommage) sans rien perdre.
  if (prefs.getBool("cpt", false)) {
    terminerCompactage();
    Console.println("Compactage interrompu par une coupure : repris, aucune mesure perdue.");
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
  Console.printf("Mesures en attente d'envoi : %u\n", (unsigned)nbLignesAttente);
}

// Réécrit le fichier en ne gardant que ce qui suit la position "depuis" (récupère la place).
// Ordre pensé pour survivre à une coupure : copie complète -> drapeau "cpt" -> suppression -> renommage
// (initialiserBacklog termine le travail si la carte redémarre au milieu).
// Renvoie false si la copie a échoué : l'original reste alors intact.
bool compacterBacklog(size_t depuis) {
  if (depuis > tailleBacklog) depuis = tailleBacklog;
  size_t aCopier = tailleBacklog - depuis;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < aCopier + 16384) {
    Console.println("Flash insuffisante pour compacter le fichier hors ligne.");
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
    Console.println("Erreur d'écriture pendant le compactage : fichier hors ligne inchangé.");
    return false;
  }
  prefs.putBool("cpt", true);  // à partir d'ici, un redémarrage termine le travail
  terminerCompactage();
  tailleBacklog = total;
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
    Console.printf("Mémoire hors-ligne pleine : %u mesures les plus anciennes supprimées.\n", (unsigned)perdues);
  }
}

void stockerRecord(const String &ligne) {
  if (!fsOK) { Console.println("Mémoire flash indisponible : mesure perdue."); return; }
  const size_t besoin = ligne.length() + 1;
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG && offsetRejeu > 0) compacterBacklog(offsetRejeu);  // récupère les lignes déjà renvoyées
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG) purgerAnciennes();                               // toujours plein : les plus vieilles partent
  if (tailleBacklog + besoin > TAILLE_MAX_BACKLOG) {
    Console.println("Mémoire hors-ligne pleine : mesure abandonnée.");
    return;
  }
  // Mode « a » (append) : la mesure est ajoutée à la fin du fichier, une mesure par ligne.
  File f = LittleFS.open(FICHIER_BACKLOG, "a");
  if (!f) { Console.println("Erreur ouverture du fichier hors ligne."); return; }
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
  // (plus de compactage ici : recopier jusqu'à 400 Ko bloquait les envois plusieurs secondes en plein rattrapage)
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
  if (b == bootId) {  // même démarrage : l'uptime suffit
    // millis() repasse à 0 au bout de ~49,7 jours : millis()/1000 repart alors de 0 après 4 294 967 s.
    // Sans ce cas, une mesure prise juste avant ce passage donnait un âge négatif et était abandonnée
    // (toutes celles d'une coupure Wi-Fi qui le chevauche, vu en simulation).
    uint32_t maintenantS = millis() / 1000UL;
    if (maintenantS >= u) return (long)(maintenantS - u);
    return (long)(maintenantS + (0xFFFFFFFFUL / 1000UL - u));
  }
  if (e == 0) return -1;
  if (!heureValide()) return -2;
  uint32_t maintenant = (uint32_t)time(nullptr);
  return (maintenant >= e) ? (long)(maintenant - e) : 0;
}

#if AIRCARTO_HTTPS || CONSOLE_DISTANTE
// Verrou mutexTls (une seule connexion HTTPS à la fois) : pris en attendant ms au plus (vrai aussi s'il n'existe pas), rendu.
static bool prendreVerrouTls(uint32_t ms) { return !mutexTls || xSemaphoreTake(mutexTls, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void rendreVerrouTls() { if (mutexTls) xSemaphoreGive(mutexTls); }
// Échange HTTPS reporté faute de mémoire : message commun à AirCarto et au relais (debut = qui reporte quoi).
static void signalerMemoireTls(const char* debut) {
  Console.printf("%smémoire insuffisante pour HTTPS (libre %u, plus grand bloc %u octets, réserve %u/%u).\n", debut,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)tamponsTlsLibres(), (unsigned)nbTamponsTls);
}
#endif

// Un POST ; la réponse du serveur décide de la suite
ResultatPush posterAuServeur(const String &payload) {
  // Le serveur compte 10 s entre le début de deux requêtes : on note l'heure AVANT l'envoi
  // (avant, elle était notée après la réponse : la durée du POST s'ajoutait à chaque créneau).
  tDernierPush = millis();
  dejaPoste = true;
#if AIRCARTO_HTTPS
  // Une seule connexion HTTPS à la fois : si le relais en utilise une, on attend qu'il finisse (5 s au plus, bien
  // en dessous du watchdog). Puis HTTPS demande ~56 Ko libres (memoireTlsOK). Sinon : on réessaie au créneau suivant,
  // sans compter un échec (la mesure est gardée).
  if (!prendreVerrouTls(5000)) {
    nbReportsMemoire++;
    LOG("Envoi reporté : une autre connexion HTTPS est en cours.\n");
    return PUSH_REPORTE;     // ce n'est pas un échec d'envoi : le témoin du serveur reste tel quel
  }
  if (!memoireTlsOK()) {
    rendreVerrouTls();
    nbReportsMemoire++;
    if (verbose) signalerMemoireTls("Envoi reporté : ");
    return PUSH_REPORTE;
  }
#endif
  // Un POST vers le serveur, avec des délais courts pour ne pas bloquer longtemps la tâche d'envoi.
  // Une connexion par envoi (setReuse(false)) : rien ne reste en attente dans la connexion d'un envoi à l'autre.
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.setReuse(false);
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  String corps;
#if AIRCARTO_HTTPS
  // clientAircarto vérifie certificat, nom du serveur et date à l'ouverture (ClientTlsDate) : la clé part après.
  bool pret = http.begin(clientAircarto, URL_SERVEUR);
#else
  bool pret = http.begin(URL_SERVEUR);
#endif
  if (pret) {
    // En-têtes : le contenu est du JSON, et la clé du groupe identifie la station auprès du serveur.
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Group-Key", lireCle());

    LOG("Envoi : %s\n", payload.c_str());
    // Envoie la requête. code = code HTTP du serveur (200 = OK, 401 = clé refusée...) ou valeur négative si le réseau a échoué.
#if AIRCARTO_HTTPS
    clientAircarto.tOuverture = 0;
#endif
    code = http.POST(payload);
#if AIRCARTO_HTTPS
    // Le serveur compte ses 10 s à l'ARRIVÉE de la requête, c.-à-d. à la fin de la poignée de main HTTPS, dont la
    // durée varie (0,5 à 3 s). Comptées depuis le début, deux requêtes pouvaient arriver à moins de 10 s d'écart
    // (refus 429, mesure mise de côté). On compte donc depuis la fin de la poignée de main.
    if (clientAircarto.tOuverture != 0) tDernierPush = clientAircarto.tOuverture;
#endif
    if (code > 0) corps = lireReponse(http, REPONSE_MAX_OCTETS);   // quelques centaines d'octets attendus
  }
  http.end();
#if AIRCARTO_HTTPS
  clientAircarto.stop();   // (déjà fait par http.end, sauf si la connexion a échoué avant)
  rendreVerrouTls();
#endif
  dernierCodeHttp = code;

  if (code <= 0) {  // Pas de réponse : Wi-Fi, serveur injoignable, ou certificat du serveur refusé
    char err[96] = "";
    clientAircarto.lastError(err, sizeof(err));
    LOG("Échec de l'envoi : %s%s%s\n", HTTPClient::errorToString(code).c_str(), err[0] ? " | dernière erreur TLS : " : "", err);
    echecsPush++; etatEnvoi = 2;
    // Le Wi-Fi se dit connecté mais rien ne passe (bail DHCP perdu, borne qui a changé de clé…) : gererWiFi ne le
    // voit pas (WiFi.status() reste « connecté »). Après quelques échecs de suite, on refait la connexion.
    if (reseauOK() && ++echecsReseauConnecte >= ECHECS_AVANT_RECONNEXION) {
      echecsReseauConnecte = 0;
      demandeReconnexion = true;
    }
    return PUSH_REESSAYER;
  }
  echecsReseauConnecte = 0;
  tDerniereReponse = millis();
  LOG("Réponse HTTP %d\n", code);
  LOG("%s\n", corps.c_str());  // le message du serveur dit quoi corriger

  // Le serveur répond en JSON ; next_push_in_s = délai à respecter avant le prochain envoi.
  JsonDocument rep;
  long suivant = 0;
  if (!deserializeJson(rep, corps)) suivant = rep["next_push_in_s"] | 0L;
  // une réponse aberrante (2 jours…) suspendait les envois d'autant : on plafonne
  suivant = constrain(suivant, 0L, (long)(ATTENTE_SERVEUR_MAX_MS / 1000UL));

  // Succès : on respecte le délai demandé par le serveur (next_push_in_s). La marge au-dessus diminue
  // doucement tant que le serveur ne refuse rien (10 succès de suite : -100 ms).
  if (code >= 200 && code < 300) {
    echecsPush = 0;
    if (cleInvalide) Console.println("Clé acceptée de nouveau : envois repris.");
    cleInvalide = false;
    ecartMinMs = max(ECART_PUSH_MIN_MS, (unsigned long)suivant * 1000UL);
    if (++succesDeSuite >= 10) {
      succesDeSuite = 0;
      if (margePushMs >= MARGE_PUSH_MIN_MS + 100) margePushMs -= 100;
    }
    etatEnvoi = 1;
    return PUSH_OK;
  }
  succesDeSuite = 0;
  if (code == 401) {  // clé absente, fausse, révoquée ou expirée… ou refus passager du serveur
    // Avant : arrêt DÉFINITIF des envois jusqu'au prochain démarrage. Un seul 401 passager (maintenance du serveur)
    // bloquait donc tout pendant des jours. Maintenant : nouvel essai toutes les 15 min, mesures gardées en mémoire.
    if (!cleInvalide) Console.println("Clé refusée : envois suspendus, nouvel essai toutes les 15 min (mesures gardées en mémoire). Vérifie 'set cle ...'.");
    cleInvalide = true;
    tCleRefusee = millis();
    etatEnvoi = 3;
    return PUSH_CLE_INVALIDE;
  }
  if (code == 429) {  // envoi trop rapproché : on agrandit la marge (ce n'est pas une panne : pas d'espacement progressif)
    margePushMs = min(margePushMs + 500, MARGE_PUSH_MAX_MS);
    // Le serveur dit exactement combien attendre (retry_after_s) : le prochain envoi part à ce moment-là, pas avant.
    long attente = rep["retry_after_s"] | 0L;
    attente = constrain(attente, 0L, (long)(ATTENTE_SERVEUR_MAX_MS / 1000UL));
    if (attente > 0) tPasAvant = (millis() + (unsigned long)attente * 1000UL) | 1UL;
    if (suivant > 0) ecartMinMs = max(ECART_PUSH_MIN_MS, (unsigned long)suivant * 1000UL);
    etatEnvoi = 2;
    return PUSH_REPORTE;     // mesure valable, juste trop tôt : elle repart au moment indiqué
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
    // Mesure prise lors d'un démarrage précédent qui n'a jamais eu l'heure (courant coupé avant le retour du
    // Wi-Fi) : impossible de la dater. On le signale toujours (pas seulement en verbose) : c'est une perte.
    if (nbSansHeure++ == 0) Console.println("Mesures d'un démarrage précédent sans heure (pas de Wi-Fi avant la coupure) : impossibles à dater, abandonnées.");
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
  // Ajout d'un champ entier, positif ou arrondi à 0,1 : un seul exemplaire du code d'ajout pour tous les champs.
  // JsonString(nom, true) : clé gardée par adresse, sans copie (comme avec doc["..."]).
  auto entier = [&doc](const char* nom, int v) { doc[JsonString(nom, true)] = v; };
  auto positif = [&doc](const char* nom, uint32_t v) { doc[JsonString(nom, true)] = v; };
  auto dixieme = [&doc](const char* nom, float v) { doc[JsonString(nom, true)] = arrondi1(v); };
  // Champs standard attendus par AirCarto : identifiant de la carte, uptime, signal Wi-Fi, mémoire libre.
  doc["device_id"] = deviceId;
  positif("uptime", millis() / 1000);
  if (reseauOK()) {
    int rssi = WiFi.RSSI();
    if (rssi >= -120 && rssi <= 0) entier("rssi", rssi);
  }
  positif("heap", ESP.getFreeHeap() + octetsReserveLibres());   // disponible, réserve HTTPS libre comprise (comme avant)

  // CO2 : moyenne de la période (co2) et pic (x_co2_max).
  if (aCO2) {
    float moy = agCO2.somme / agCO2.n;
    if (dansBornes("co2", moy, 0, 10000)) entier("co2", (int)(moy + 0.5f));
    if (dansBornes("x_co2_max", agCO2.maxi, 0, 10000)) entier("x_co2_max", (int)agCO2.maxi);
  }
  // Particules : moyennes PM1 / PM2.5 / PM10, pic de PM2.5, état du NextPM et sa température / humidité.
  if (aPM) {
    // même nom pour le champ et pour le message de dansBornes (champ omis hors bornes)
    auto pm = [&dixieme](const char* nom, float v) { if (dansBornes(nom, v, 0, 1000)) dixieme(nom, v); };
    pm("pm1", agPM1.somme / agPM1.n);
    pm("pm25", agPM25.somme / agPM25.n);
    pm("pm10", agPM10.somme / agPM10.n);
    pm("x_pm25_max", agPM25.maxi);
    entier("npm_status", (int)statutPM);   // champ officiel d'AirCarto (0 à 255) : libère une place de champ x_
    if (!isnan(tempPM)) dixieme("x_temp_pm", tempPM);
    if (!isnan(humPM)) dixieme("x_hum_pm", humPM);
  }
  // Champs personnalisés « x_ » (10 au maximum, imposé par AirCarto ; 9 utilisés) : alimentation, aération détectée,
  // % du temps au-dessus de 1000 ppm, cause du dernier redémarrage.
  entier("x_alim", estSurSecteur ? 1 : 0);
  entier("x_aeration", aerationPeriode ? 1 : 0);
  if (nCO2Total > 0) dixieme("x_pct_co2_1000", 100.0f * nCO2Haut / nCO2Total);
  entier("x_reset", (int)esp_reset_reason());

  // Métadonnées internes (retirées avant l'envoi) : quand la mesure a été faite
  positif("_u", millis() / 1000);
  positif("_e", heureValide() ? (uint32_t)time(nullptr) : (uint32_t)0);
  positif("_b", bootId);

  ligne = "";
  serializeJson(doc, ligne);

  agregatRaz(agCO2); agregatRaz(agPM1); agregatRaz(agPM25); agregatRaz(agPM10);
  aerationPeriode = false;
  return true;
}

// Retire la plus ancienne mesure déposée (mutexEnvoi déjà pris). Copies et non déplacements : chaque case garde
// la place réservée au démarrage (TAILLE_PENDANTE).
static void retirerPremierePendante() {
  for (uint8_t i = 1; i < nbPendantes; i++) pendantes[i - 1] = pendantes[i];
  nbPendantes--;
  pendantes[nbPendantes] = "";
}

// loop() dépose la mesure ; la tâche d'envoi la prend (réseau, ou fichier hors ligne si pas de réseau)
void deposerMesure(const String &ligne) {
  xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  // la tâche d'envoi n'a rien traité depuis plusieurs périodes : on sacrifie la plus ancienne
  if (nbPendantes >= NB_PENDANTES) retirerPremierePendante();
  pendantes[nbPendantes++] = ligne;
  xSemaphoreGive(mutexEnvoi);
}

// Côté tâche d'envoi : remet en TÊTE une mesure fraîche dont l'envoi a été reporté (PUSH_REPORTE). Seulement s'il
// reste de la place pour la mesure suivante (sinon loop sacrifierait la plus ancienne) : sinon, false -> flash.
bool remettrePendante(const String &ligne) {
  bool ok = false;
  xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  if (nbPendantes < NB_PENDANTES - 1) {
    for (int i = nbPendantes; i > 0; i--) pendantes[i] = pendantes[i - 1];
    pendantes[0] = ligne;
    nbPendantes++;
    ok = true;
  }
  xSemaphoreGive(mutexEnvoi);
  return ok;
}

// Côté tâche d'envoi : prend la plus ancienne mesure déposée (false s'il n'y en a pas)
bool sortirPendante(String &ligne) {
  bool ok = false;
  xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  if (nbPendantes > 0) {
    ligne = pendantes[0];
    retirerPremierePendante();
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
    Console.println("File d'attente vidée.");
  }

  bool enLigne = reseauOK() && cleOK() && (!cleInvalide || now - tCleRefusee >= ESSAI_CLE_MS);   // clé refusée : un essai / 15 min

  String ligne;
  // Pas de réseau (ou pas de clé valide) : tout ce qui attend en RAM est mis de côté en flash
  if (!enLigne) {
    tAttenteHeure = 0;
    while (sortirPendante(ligne)) {
      stockerRecord(ligne);
      if (etatEnvoi == 1) etatEnvoi = 2;
    }
    return;
  }
  if (dejaPoste && now - tDernierPush < ecartCourantMs()) return;  // le serveur refuse les envois trop rapprochés
  if (tPasAvant != 0) {                                          // refus 429 : attente demandée par le serveur
    if ((long)(now - tPasAvant) < 0) return;
    tPasAvant = 0;
  }

  if (sortirPendante(ligne)) {
    ResultatPush r = envoyerLigne(ligne);
    if (r == PUSH_REPORTE && remettrePendante(ligne)) return;   // rien n'est parti : elle attend en RAM le créneau suivant
    if (r == PUSH_REESSAYER || r == PUSH_CLE_INVALIDE || r == PUSH_ATTENDRE || r == PUSH_REPORTE) stockerRecord(ligne);
    return;
  }
  if (nbLignesAttente == 0) return;
  if (tAttenteHeure != 0 && now - tEssaiNtp < ATTENTE_NTP_REESSAI_MS) return;  // l'heure NTP n'est pas encore là
  rejouerUneLigne();
}

// Le réseau et la clé permettent-ils d'envoyer ? (pour choisir le rythme des mesures : voir periodeMesureMs)
bool envoiPossible() {
  return reseauOK() && cleOK() && !cleInvalide;
}

// La mesure de la période est-elle due ? Appelée par loop().
// En direct (10 s), la mesure est bouclée quand le serveur accepte l'envoi suivant (10 s + la marge) et que plus
// rien n'attend en RAM : elle part dans les 50 ms, avec x_age_s = 0. Bouclée à 10 s pile, elle arriverait un peu
// plus vite que le serveur ne les accepte : les mesures s'empileraient. Elle couvre donc 10 s ou un peu plus
// (toutes les lectures depuis la précédente : aucune n'est perdue). Sécurité : ATTENTE_CRENEAU_MAX_MS au plus.
bool mesureDue(unsigned long now, unsigned long tDebut) {
  unsigned long p = periodeMesureMs();
  unsigned long ecoule = now - tDebut;
  if (ecoule < p) return false;
  if (p != PERIODE_MESURE_MS) return true;                 // hors ligne, rattrapage, batterie : rythme fixe
  if (ecoule >= ATTENTE_CRENEAU_MAX_MS) return true;
  if (nbPendantes > 0) return false;                       // la précédente n'est pas encore partie
  if (!dejaPoste) return true;
  return millis() - tDernierPush + 100UL >= ecartCourantMs();
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
// CONSOLE À DISTANCE (relais Cloudflare) ET MISE À JOUR PAR INTERNET
// =====================================================================
// Le relais est un petit programme gratuit qui tourne chez Cloudflare (dossier relais/ du dépôt). Il sert de boîte
// aux lettres entre la carte et ta page « console » : la carte n'a besoin que d'HTTPS sortant (port 443), qui passe
// partout, eduroam compris.
//  - JOURNAL : tout ce que la carte écrit (Console) est gardé dans journalTampon, puis envoyé au relais par paquets :
//    toutes les SYNC_REPOS_MS au repos, toutes les SYNC_LIVE_MS quand la page console est ouverte.
//  - COMMANDES : la réponse du relais contient les commandes tapées dans la console. Elles passent par la même
//    porte que celles du Bluetooth : seule une LISTE BLANCHE est acceptée (voir commandeAutoriseeADistance) ;
//    Wi-Fi, clé, jeton, relais et effacements restent réservés au câble USB, même si le relais était piraté.
//  - MISE À JOUR (« maj » puis « maj installer ») : le relais donne le manifeste de la dernière release GitHub
//    (version, taille, empreinte SHA-256, signature). La carte vérifie la SIGNATURE avec la clé publique de cle_ota.h
//    (la clé privée ne quitte pas ton PC), télécharge le programme, vérifie son empreinte, l'écrit dans l'AUTRE
//    emplacement de la mémoire, puis redémarre dessus. L'ancien programme reste en place : si le nouveau redémarre
//    en boucle ou ne rejoint pas le relais en 10 min, la carte revient toute seule à l'ancien (verifierDemarrageOta).
//  - SÉCURITÉ RÉSEAU : HTTPS avec vérification du certificat du relais (racines_tls.h) ; la carte se présente avec
//    son JETON (secret, entré au câble USB), différent du mot de passe de la console.
#if CONSOLE_DISTANTE
static const unsigned long SYNC_REPOS_MS        = 30000UL;   // personne ne regarde : un paquet toutes les 30 s
static const unsigned long SYNC_LIVE_MS         = 2000UL;    // console ouverte : presque en direct
static const unsigned long SYNC_ERREUR_MAX_MS   = 300000UL;  // relais injoignable : on espace jusqu'à 5 min
static const size_t        JOURNAL_TAILLE       = 6144;      // journal en attente d'envoi (les plus vieilles lignes sautent)
static const size_t        PAQUET_MAX_OCTETS    = 3072;      // au plus par envoi
static const unsigned long OTA_VALIDATION_MS    = 60000UL;   // nouvelle version : validée après 1 min + un échange réussi
static const unsigned long OTA_DELAI_MAX_MS     = 600000UL;  // ... sinon, après 10 min de Wi-Fi : retour à l'ancienne
static const uint8_t       OTA_DEMARRAGES_MAX   = 3;         // 3 redémarrages sans validation : retour à l'ancienne
static const unsigned long OTA_ESSAI_MAX_MS     = 1200000UL; // 20 min sans AUCUN échange avec le relais, Wi-Fi ou pas
                                                             // (une version qui casse le Wi-Fi) : retour à l'ancienne

char journalTampon[JOURNAL_TAILLE];
size_t journalLong = 0;              // octets en attente dans journalTampon
uint32_t journalBase = 0;            // nombre d'octets déjà sortis du début du tampon (envoyés ou perdus)
uint32_t journalPerdues = 0;         // lignes perdues (tampon plein), signalées au relais
bool journalDebutLigne = true;
SemaphoreHandle_t mutexJournal = NULL;
volatile bool relaisConfigure = false;   // relais ET jeton présents : sinon on ne garde rien
volatile bool consoleLive = false;       // le relais dit qu'une page console est ouverte
// Où en est la tâche du relais (affiché par status) : pour voir tout de suite où elle attend si les échanges cessent.
enum EtatRelais : uint8_t { RELAIS_DEMARRAGE, RELAIS_SANS_RESEAU, RELAIS_ATTENTE, RELAIS_VERROU, RELAIS_MEMOIRE,
                            RELAIS_ECHANGE, RELAIS_MAJ };
volatile uint8_t etatRelais = RELAIS_DEMARRAGE;
volatile unsigned long tEtatRelais = 0;            // depuis quand (millis)
volatile unsigned long attenteRelaisMs = 0;        // délai avant l'échange suivant
static void changerEtatRelais(uint8_t e) { if (etatRelais != e) { etatRelais = e; tEtatRelais = millis(); } }
volatile bool syncDemandee = false;      // envoyer tout de suite (commande reçue, mise à jour...)
volatile uint8_t demandeMaj = 0;         // 1 = vérifier | 2 = vérifier et installer (commande « maj »)
unsigned long tDernierSyncOK = 0;        // dernier échange réussi avec le relais (0 = jamais)
int dernierCodeRelais = 0;
uint32_t nbSyncOK = 0, nbSyncEchecs = 0;
Preferences prefsOta;                    // espace « ota » : suivi d'une mise à jour en cours d'essai
bool otaEnEssai = false;                 // ce démarrage est celui d'une nouvelle version pas encore validée
String otaAncienne;                      // emplacement (app0 / app1) de la version précédente

void retourAncienneVersion(const char* raison, bool marquer);

// Arduino valide d'office une nouvelle version dès son démarrage : on préfère la valider nous-mêmes (surveillerOta).
extern "C" bool verifyRollbackLater() { return true; }

// --- journal ---
// Fait sortir les r premiers octets du tampon (r <= journalLong ; mutexJournal pris).
static void journalSortir(size_t r) {
  memmove(journalTampon, journalTampon + r, journalLong - r);
  journalLong -= r;
  journalBase += r;
}

// Retire les plus vieilles lignes pour faire de la place (au moins "besoin" octets).
static void journalLibererPlace(size_t besoin) {
  if (journalLong + besoin <= JOURNAL_TAILLE) return;
  size_t aRetirer = journalLong + besoin - JOURNAL_TAILLE;
  if (aRetirer < JOURNAL_TAILLE / 4) aRetirer = JOURNAL_TAILLE / 4;     // on libère d'un coup un quart du tampon
  if (aRetirer > journalLong) aRetirer = journalLong;
  // coupe sur une fin de ligne pour ne pas envoyer une ligne amputée
  while (aRetirer < journalLong && journalTampon[aRetirer - 1] != '\n') aRetirer++;
  for (size_t i = 0; i < aRetirer; i++) if (journalTampon[i] == '\n') journalPerdues++;
  journalSortir(aRetirer);
}

static void journalAjouter(const char* b, size_t n) {
  if (n > JOURNAL_TAILLE / 2) n = JOURNAL_TAILLE / 2;
  journalLibererPlace(n);
  memcpy(journalTampon + journalLong, b, n);
  journalLong += n;
}

// Chaque ligne du journal commence par l'instant où elle a été écrite (millis) et une tabulation :
// le relais en déduit l'heure exacte de chaque ligne, même envoyée 30 s plus tard.
void journalEcrire(const uint8_t* b, size_t n) {
  if (!relaisConfigure || mutexJournal == NULL) return;
  if (xSemaphoreTake(mutexJournal, pdMS_TO_TICKS(50)) != pdTRUE) return;   // jamais bloquer celui qui écrit
  for (size_t i = 0; i < n; i++) {
    char c = (char)b[i];
    if (c == '\r') continue;
    if (journalDebutLigne) {
      char pre[16];
      int k = snprintf(pre, sizeof(pre), "%lu\t", millis());
      journalAjouter(pre, (size_t)k);
      journalDebutLigne = false;
    }
    journalAjouter(&c, 1);
    if (c == '\n') journalDebutLigne = true;
  }
  xSemaphoreGive(mutexJournal);
}

// Copie dans dest le début du journal, coupé sur une fin de ligne (une ligne commencée attend sa fin, sauf si
// elle est démesurée). Renvoie la longueur ; *base = position de départ (pour journalRetirer).
static size_t journalExtraire(char* dest, size_t max, uint32_t* base) {
  xSemaphoreTake(mutexJournal, portMAX_DELAY);
  *base = journalBase;
  size_t n = (journalLong < max) ? journalLong : max;
  size_t k = n;
  while (k > 0 && journalTampon[k - 1] != '\n') k--;
  if (k > 0 || n < max) n = k;            // sinon : une seule ligne plus longue que max, envoyée coupée
  memcpy(dest, journalTampon, n);
  xSemaphoreGive(mutexJournal);
  return n;
}

// Le relais a bien reçu n octets partis de base : on les retire (sauf ceux déjà sortis entre-temps, tampon plein).
static void journalRetirer(uint32_t base, size_t n) {
  xSemaphoreTake(mutexJournal, portMAX_DELAY);
  uint32_t dejaSortis = journalBase - base;
  if (n > dejaSortis) journalSortir(min((size_t)(n - dejaSortis), journalLong));
  xSemaphoreGive(mutexJournal);
}

// Copie de cfgRelais ou de cfgJeton, prise sous mutexEnvoi (« set relais » et « set jeton » les changent sous ce verrou).
static String lireReglageRelais(const String &reglage) {
  String r;
  if (mutexEnvoi) xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  r = reglage;
  if (mutexEnvoi) xSemaphoreGive(mutexEnvoi);
  return r;
}

void majRelaisConfigure() {
  relaisConfigure = cfgRelais.startsWith("https://") && cfgJeton.length() >= 16;
}

// --- signature et empreinte ---
// Empreinte SHA-256 d'un texte, en hexadécimal minuscule (64 caractères).
static void enHexa(const uint8_t* b, size_t n, char* sortie) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { sortie[2 * i] = H[b[i] >> 4]; sortie[2 * i + 1] = H[b[i] & 15]; }
  sortie[2 * n] = 0;
}

#if OTA_POSSIBLE   // (sans clé publique, verifierMiseAJour refuse avant toute vérification)
static int valHexa(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// La signature du manifeste est-elle bien celle de TA clé privée ?
// Message signé : "MAIR-OTA|<version>|<taille>|<sha256>" (ECDSA P-256 + SHA-256, signature DER en hexadécimal).
static bool signatureValide(const char* message, const char* sigHexa) {
  uint8_t sig[80];
  size_t lg = strlen(sigHexa), n = lg / 2;
  if (lg % 2 != 0 || n < 8 || n > sizeof(sig)) return false;
  for (size_t i = 0; i < n; i++) {
    int h = valHexa(sigHexa[2 * i]), l = valHexa(sigHexa[2 * i + 1]);
    if (h < 0 || l < 0) return false;
    sig[i] = (uint8_t)(h * 16 + l);
  }
  uint8_t empreinte[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == NULL || mbedtls_md(info, (const unsigned char*)message, strlen(message), empreinte) != 0) return false;
  mbedtls_pk_context cle;
  mbedtls_pk_init(&cle);
  bool ok = mbedtls_pk_parse_public_key(&cle, (const unsigned char*)CLE_PUBLIQUE_OTA, strlen(CLE_PUBLIQUE_OTA) + 1) == 0
            && mbedtls_pk_verify(&cle, MBEDTLS_MD_SHA256, empreinte, sizeof(empreinte), sig, n) == 0;
  mbedtls_pk_free(&cle);
  return ok;
}
#endif

// --- échanges avec le relais ---
ClientTlsDate clientRelais("Relais");   // vérifie aussi la date du certificat à chaque ouverture

// Prépare une requête vers le relais (chemin = "/carte/...") avec le jeton de la carte.
// Une connexion par échange (setReuse(false)) : clientRelais (ClientTlsDate) vérifie certificat, nom et date à
// chaque ouverture, avant que le jeton ne parte ; aucune connexion chiffrée ne reste ouverte entre deux échanges.
static bool ouvrirRequete(HTTPClient& http, const char* chemin) {
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(clientRelais, lireReglageRelais(cfgRelais) + chemin)) return false;
  http.addHeader("Authorization", "Bearer " + lireReglageRelais(cfgJeton));
  return true;
}

// Un échange : envoie un paquet du journal, reçoit l'état de la console et les commandes en attente.
static bool synchroniserRelais() {
  static char paquet[PAQUET_MAX_OCTETS];
  uint32_t base = 0;
  size_t n = journalExtraire(paquet, sizeof(paquet), &base);
  HTTPClient http;
  if (!ouvrirRequete(http, "/carte/sync")) { dernierCodeRelais = HTTPC_ERROR_CONNECTION_REFUSED; return false; }
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  static const char* const EN_TETES[] = {"X-Millis", "X-Version", "X-Boot", "X-Perdues"};
  const unsigned long valeurs[] = {millis(), (unsigned long)FW_VERSION, (unsigned long)bootId, (unsigned long)journalPerdues};
  for (uint8_t i = 0; i < 4; i++) {
    char v[24];
    snprintf(v, sizeof(v), "%lu", valeurs[i]);
    http.addHeader(EN_TETES[i], v);
  }
  int code = http.POST((uint8_t*)paquet, n);
  dernierCodeRelais = code;
  if (code != 200) {
    http.end();
    if (code == 401) LOG("Relais : jeton refusé (set jeton, au câble USB).\n");
    return false;
  }
  String rep = lireReponse(http, 8192);   // état de la console + commandes en attente : quelques centaines d'octets
  http.end();
  journalRetirer(base, n);
  journalPerdues = 0;
  JsonDocument doc;
  if (deserializeJson(doc, rep)) return true;   // reçu, mais réponse illisible
  consoleLive = doc["live"] | false;
  for (JsonVariant c : doc["cmds"].as<JsonArray>()) {
    const char* t = c.as<const char*>();
    if (t && *t) deposerCommande(t, SOURCE_RELAIS);
  }
  return true;
}

// Reçoit le programme téléchargé morceau par morceau : calcule son empreinte et l'écrit dans l'autre emplacement.
class FluxMaj : public Stream {
 public:
  FluxMaj(size_t total, esp_ota_handle_t h) : total(total), ota(h) {
    mbedtls_md_init(&ctx);
    ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0 && mbedtls_md_starts(&ctx) == 0;
  }
  ~FluxMaj() { mbedtls_md_free(&ctx); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (!ok || recu + n > total) { ok = false; return 0; }
    if (esp_ota_write(ota, b, n) != ESP_OK) { ok = false; return 0; }   // l'emplacement est déjà effacé : pages seulement
    mbedtls_md_update(&ctx, b, n);
    recu += n;
    majProgression = (uint8_t)(recu * 100ULL / total);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void empreinte(char* hexa) {
    uint8_t e[32];
    mbedtls_md_finish(&ctx, e);
    enHexa(e, 32, hexa);
  }
  size_t total, recu = 0;
  bool ok = false;
 private:
  esp_ota_handle_t ota;
  mbedtls_md_context_t ctx;
};

// Commande « maj » : cherche la dernière version publiée ; installer = true : la télécharge et l'installe.
static void verifierMiseAJour(bool installer) {
#if !OTA_POSSIBLE
  Console.println("Mise à jour à distance impossible : pas de clé publique (pio run -t cles, puis flash USB).");
  return;
#else
  HTTPClient http;
  if (!ouvrirRequete(http, "/carte/manifeste")) { Console.println("Mise à jour : relais injoignable."); return; }
  int code = http.GET();
  if (code != 200) {
    Console.printf("Mise à jour : pas de manifeste (code %d). Release GitHub publiée avec firmware.json ?\n", code);
    http.end();
    return;
  }
  String rep = lireReponse(http, 4096);
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, rep)) { Console.println("Mise à jour : manifeste illisible."); return; }
  uint32_t version = doc["version"] | 0UL;
  uint32_t taille  = doc["taille"] | 0UL;
  const char* sha = doc["sha256"] | "";   // (textes gardés dans doc jusqu'à la fin de la fonction : pas de copie)
  const char* sig = doc["sig"] | "";
  const char* tag = doc["tag"] | "";
  char message[100];   // 95 caractères au plus quand sha en fait 64 (seul cas où la signature est vérifiée)
  snprintf(message, sizeof(message), "MAIR-OTA|%lu|%lu|%s", (unsigned long)version, (unsigned long)taille, sha);
  if (strlen(sha) != 64 || *tag == 0 || !signatureValide(message, sig)) {
    Console.println("Mise à jour REFUSÉE : signature invalide (ce programme ne vient pas de ta clé).");
    return;
  }
  Console.printf("Version installée : %lu | dernière publiée : %lu (%s, %lu octets, signature OK)\n",
                 (unsigned long)FW_VERSION, (unsigned long)version, tag, (unsigned long)taille);
  if (version <= (uint32_t)FW_VERSION) { Console.println("La carte est à jour."); return; }
  // Une version qui a déjà échoué sur cette carte (retour automatique à l'ancienne) n'est pas réinstallée à distance :
  // sinon une console volée pourrait la réinstaller en boucle (« maj installer » puis « reboot »), en usant la flash.
  if (version == prefsOta.getULong("refusee", 0)) {
    Console.printf("Mise à jour REFUSÉE : la version %lu a déjà échoué sur cette carte (redémarrages en boucle). Publie "
                   "une version corrigée, ou tape 'maj oublier' au câble USB pour l'autoriser à nouveau.\n", (unsigned long)version);
    return;
  }
  if (!installer) { Console.println("Pour l'installer : maj installer"); return; }

  const esp_partition_t* cible = esp_ota_get_next_update_partition(NULL);
  const esp_partition_t* actuelle = esp_ota_get_running_partition();
  if (cible == NULL || actuelle == NULL || taille > cible->size) {
    Console.println("Mise à jour impossible : pas d'emplacement libre assez grand (table de partitions ?).");
    return;
  }
  // Effacement de l'emplacement en UNE fois, avant le téléchargement. Pendant un effacement, la mémoire flash est
  // inaccessible : rien ne tourne, l'écran ne peut plus être rafraîchi. Avant (bibliothèque Update), un bloc de 64 Ko
  // était effacé tous les 64 Ko reçus : l'écran s'éteignait une fraction de seconde une vingtaine de fois pendant la
  // mise à jour (clignotement). Maintenant : écran éteint proprement 2 à 5 s, puis le téléchargement n'écrit plus que
  // des pages (pauses de moins d'1 ms, invisibles) et la progression s'affiche sans à-coups.
  Console.printf("Mise à jour : préparation de l'emplacement %s (%lu octets, écran éteint quelques secondes)...\n",
                 cible->label, (unsigned long)taille);
  majProgression = 0;          // écran MISE A JOUR (dessiné par loop) dès que l'écran revient
  ecranSuspendu = true;
  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(cible, taille, &ota);
  ecranSuspendu = false;
  if (err != ESP_OK) {
    majProgression = 0xFF;
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE)
      Console.println("Mise à jour : la version actuelle est encore en essai (validée 1 min après un échange avec le relais). Réessaie ensuite.");
    else
      Console.printf("Mise à jour : préparation impossible (%s).\n", esp_err_to_name(err));
    return;
  }
  String chemin = String("/carte/firmware?tag=") + tag;
  if (!ouvrirRequete(http, chemin.c_str())) {
    esp_ota_abort(ota); majProgression = 0xFF;
    Console.println("Mise à jour : relais injoignable.");
    return;
  }
  http.setTimeout(20000);
  code = http.GET();
  if (code != 200) {
    esp_ota_abort(ota); majProgression = 0xFF;
    Console.printf("Mise à jour : téléchargement refusé (code %d).\n", code);
    http.end();
    return;
  }
  Console.printf("Téléchargement de %lu octets vers %s...\n", (unsigned long)taille, cible->label);
  FluxMaj flux(taille, ota);
  int ecrits = http.writeToStream(&flux);
  http.end();
  char empreinte[65];
  flux.empreinte(empreinte);
  if (ecrits != (int)taille || !flux.ok || flux.recu != taille || strcmp(sha, empreinte) != 0) {
    esp_ota_abort(ota);
    majProgression = 0xFF;
    Console.printf("Mise à jour ANNULÉE : fichier incomplet ou modifié (%d / %lu octets). L'ancienne version reste.\n",
                   ecrits, (unsigned long)taille);
    return;
  }
  // esp_ota_end vérifie aussi l'image écrite (en-tête, somme de contrôle, empreinte ajoutée à la compilation)
  err = esp_ota_end(ota);
  if (err == ESP_OK) err = esp_ota_set_boot_partition(cible);
  if (err != ESP_OK) {
    majProgression = 0xFF;
    Console.printf("Mise à jour ANNULÉE : %s. L'ancienne version reste.\n", esp_err_to_name(err));
    return;
  }
  // Suivi de l'essai : la nouvelle version devra se valider (verifierDemarrageOta / surveillerOta).
  prefsOta.putString("anc", actuelle->label);
  prefsOta.putString("nouv", cible->label);
  prefsOta.putULong("vers", version);
  prefsOta.putUChar("essai", 0);
  Console.printf("Mise à jour installée (version %lu) : redémarrage.\n", (unsigned long)version);
  synchroniserRelais();     // dernier paquet du journal avant de redémarrer
  delay(300);
  ESP.restart();
#endif
}

// Oublie le suivi de l'essai en cours (garde « refusee » : la dernière version qui a échoué).
void oublierEssaiOta() {
  prefsOta.remove("anc"); prefsOta.remove("nouv"); prefsOta.remove("vers"); prefsOta.remove("essai");
}

// Au démarrage : suivi d'une mise à jour récente (appelée tout au début de setup).
void verifierDemarrageOta() {
  prefsOta.begin("ota", false);
  String nouv = prefsOta.getString("nouv", "");
  const esp_partition_t* actuelle = esp_ota_get_running_partition();
  if (nouv.length() == 0 || actuelle == NULL) {
    esp_ota_mark_app_valid_cancel_rollback();   // rien en essai (flash USB, démarrage normal)
    return;
  }
  // Ce n'est pas la version installée à distance (flash USB entre-temps, ou retour en arrière déjà fait) : on oublie.
  // Si l'on tourne sur l'ANCIEN emplacement avec un programme plus ancien, la nouvelle version a échoué (retour fait
  // par le démarreur de l'ESP32 ou par nous) : on la note, pour ne pas la réinstaller à distance.
  bool ailleurs = nouv != actuelle->label;
  uint32_t vers = prefsOta.getULong("vers", 0);
  if (ailleurs || vers != (uint32_t)FW_VERSION) {
    oublierEssaiOta();
    if (ailleurs && vers > (uint32_t)FW_VERSION) prefsOta.putULong("refusee", vers);
    esp_ota_mark_app_valid_cancel_rollback();
    return;
  }
  uint8_t essai = prefsOta.getUChar("essai", 0) + 1;
  prefsOta.putUChar("essai", essai);
  otaAncienne = prefsOta.getString("anc", "");
  otaEnEssai = true;
  if (essai > OTA_DEMARRAGES_MAX) retourAncienneVersion("redémarrages en boucle", true);
}

// Revient au programme précédent, resté intact dans l'autre emplacement. marquer = la version elle-même est en cause
// (redémarrages en boucle) : elle ne sera plus réinstallée à distance (« maj oublier » au câble USB pour l'autoriser).
// Pas de marque si la cause peut être extérieure (relais injoignable, Wi-Fi) : on pourra la réinstaller.
void retourAncienneVersion(const char* raison, bool marquer) {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, otaAncienne.c_str());
  oublierEssaiOta();
  otaEnEssai = false;
  if (p == NULL || esp_ota_set_boot_partition(p) != ESP_OK) {
    Console.printf("Nouvelle version en échec (%s), mais retour impossible : on la garde.\n", raison);
    esp_ota_mark_app_valid_cancel_rollback();
    return;
  }
  if (marquer) prefsOta.putULong("refusee", (uint32_t)FW_VERSION);   // pas de réinstallation à distance de cette version
  Console.printf("Nouvelle version en échec (%s) : retour à l'ancienne (%s), redémarrage.\n", raison, otaAncienne.c_str());
  delay(300);
  ESP.restart();
}

// Nouvelle version en essai : validée dès qu'elle a tourné 1 min ET parlé au relais. Retour à l'ancienne si elle
// a eu le Wi-Fi pendant 10 min sans jamais joindre le relais (sans Wi-Fi, on ne peut rien conclure : on attend).
void surveillerOta() {
  static unsigned long tPrec = 0, avecReseau = 0;
  if (!otaEnEssai) return;
  unsigned long now = millis();
  if (reseauOK()) avecReseau += now - tPrec;
  tPrec = now;
  if (now >= OTA_VALIDATION_MS && tDernierSyncOK != 0) {
    otaEnEssai = false;
    oublierEssaiOta();
    esp_ota_mark_app_valid_cancel_rollback();
    Console.printf("Nouvelle version %lu validée.\n", (unsigned long)FW_VERSION);
  } else if (avecReseau >= OTA_DELAI_MAX_MS) {
    retourAncienneVersion("relais jamais joint après 10 min de Wi-Fi", false);
  } else if (now >= OTA_ESSAI_MAX_MS) {
    retourAncienneVersion("aucun échange avec le relais en 20 min (Wi-Fi perdu ?)", false);
  }
}

// Tâche du relais (cœur 0, priorité basse) : ses échanges HTTPS (1 à 3 s) ne ralentissent ni l'écran ni les envois.
void tacheRelais(void* param) {
  clientRelais.racines(BUNDLE_TLS);        // 10 racines publiques (Cloudflare change d'autorité)
  clientRelais.setHandshakeTimeout(15);
  unsigned long tSync = 0, attente = 5000;   // premier échange ~5 s après le démarrage
  uint8_t echecs = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    unsigned long now = millis();
    surveillerOta();
    if (!relaisConfigure || !reseauOK()) { consoleLive = false; changerEtatRelais(RELAIS_SANS_RESEAU); continue; }
    if (demandeMaj != 0) {
      changerEtatRelais(RELAIS_MAJ);
      bool installer = (demandeMaj == 2);
      demandeMaj = 0;
      if (!prendreVerrouTls(20000)) {
        Console.println("Mise à jour : une autre connexion HTTPS est en cours, réessaie.");
      } else {
        if (!memoireTlsOK()) Console.println("Mise à jour : mémoire insuffisante, réessaie.");
        else verifierMiseAJour(installer);
        rendreVerrouTls();
      }
      syncDemandee = true;
    }
    bool presquePlein = journalLong > JOURNAL_TAILLE / 2;
    attenteRelaisMs = attente;
    if (!syncDemandee && !presquePlein && now - tSync < attente) { changerEtatRelais(RELAIS_ATTENTE); continue; }
    syncDemandee = false;
    tSync = now;
    changerEtatRelais(RELAIS_VERROU);
    if (!prendreVerrouTls(20000)) { attente = 2000; continue; }
    if (!memoireTlsOK()) {
      changerEtatRelais(RELAIS_MEMOIRE);
      rendreVerrouTls();
      static unsigned long tMsgMemoire = 0;      // message au plus une fois par minute (USB, Bluetooth)
      if (tMsgMemoire == 0 || millis() - tMsgMemoire > 60000UL) {
        tMsgMemoire = millis() | 1UL;
        signalerMemoireTls("Relais : échange reporté, ");
      }
      attente = 10000;
      continue;
    }
    changerEtatRelais(RELAIS_ECHANGE);
    bool syncOK = synchroniserRelais();
    clientRelais.stop();    // une connexion par échange : on libère tout de suite la mémoire de la connexion chiffrée
    rendreVerrouTls();
    tSync = millis();       // l'attente suivante part de la FIN de l'échange : AirCarto a sa place entre deux
    if (syncOK) {
      echecs = 0;
      nbSyncOK++;
      tDernierSyncOK = millis();
      attente = consoleLive ? SYNC_LIVE_MS : SYNC_REPOS_MS;
    } else {
      nbSyncEchecs++;
      consoleLive = false;
      if (echecs < 10) echecs++;
      attente = min(SYNC_ERREUR_MAX_MS, SYNC_REPOS_MS * echecs);   // 30 s, 60 s, 90 s... jusqu'à 5 min
      // Visible (USB, Bluetooth, puis journal à distance dès que le relais répond) : sans ce message, un relais
      // injoignable ressemblait à une console qui ignore les commandes.
      char err[96] = "";
      if (dernierCodeRelais < 0) clientRelais.lastError(err, sizeof(err));
      Console.printf("Relais : échange échoué (%d : %s%s%s), nouvel essai dans %lu s.\n", dernierCodeRelais,
                     dernierCodeRelais < 0 ? HTTPClient::errorToString(dernierCodeRelais).c_str() : "réponse HTTP",
                     err[0] ? " | TLS : " : "", err, attente / 1000UL);
    }
  }
}

#else   // CONSOLE_DISTANTE 0
void journalEcrire(const uint8_t*, size_t) {}
#endif

// =====================================================================
// BLUETOOTH (console de dépannage, à côté de la carte)
// =====================================================================
// La carte se présente comme un « port série Bluetooth » standard (service UART de Nordic, en BLE) sous le nom
// MAir-XXXX. Applis : « Serial Bluetooth Terminal » (Android), « Bluefruit Connect » ou « nRF Toolbox » (iPhone).
// Marche sans Wi-Fi ni Internet : utile justement quand la carte n'arrive plus à se connecter.
// SÉCURITÉ :
//  - à la première connexion, l'écran affiche un CODE À 6 CHIFFRES à taper sur le téléphone : il faut VOIR l'écran
//    pour s'appairer (appairage chiffré, protégé contre l'interception) ; le téléphone est ensuite reconnu ;
//  - un NOUVEAU téléphone n'est accepté que pendant FENETRE_APPAIRAGE_MS après « bt appairer » au câble USB, ou,
//    si l'option est activée (« bt demarrage on », câble USB ; désactivée par défaut), après une mise sous tension
//    (débrancher / rebrancher la carte). Le reste du temps, le code n'est jamais affiché : quelqu'un qui voit l'écran
//    depuis la pièce ne peut pas appairer son téléphone. Un redémarrage demandé à distance (reboot) ne rouvre jamais
//    la fenêtre. La fenêtre se referme dès qu'un nouveau téléphone s'est appairé ;
//  - « bt liste » / « bt oublier <n> » (câble USB) : voir les téléphones appairés, en retirer UN ;
//  - appairage « Secure Connections » seulement, clé de 16 octets (platformio.ini : MYNEWT_VAL_BLE_SM_SC_ONLY) :
//    l'ancien appairage « legacy », dont le code se retrouve en écoutant l'échange, est refusé ;
//  - tant que l'appairage n'est pas fait, la carte n'envoie rien et ignore tout ce qu'elle reçoit ; une connexion
//    qui ne s'appaire pas est coupée au bout de BT_SANS_APPAIRAGE_MS (elle occupe la seule place de connexion) ;
//  - mêmes commandes qu'à distance seulement (liste blanche) : Wi-Fi, clé, jeton, effacements = câble USB ;
//  - « bt oublier » (au câble USB) efface les téléphones connus.
// FIABILITÉ (analyse du 25/11 : connexions aléatoires, « gatt status 133 », « write descriptor failed ») :
//  - la carte ne lance l'appairage que si le lien n'est pas déjà chiffré (un téléphone connu chiffre de lui-même) ;
//  - elle n'envoie rien avant que le téléphone se soit abonné aux réponses (sinon les premières lignes se perdent) ;
//  - la diffusion est surveillée chaque seconde : relancée si elle s'est arrêtée (liaison avortée avant onConnect) ;
//  - mémoire des abonnements agrandie (platformio.ini : MAX_CCCDS, MAX_CSFCS) et jamais un téléphone effacé pour
//    faire de la place à un abonnement ; la raison de chaque déconnexion est affichée (avec un conseil) ;
//  - Wi-Fi : scans plus courts et plus espacés, et mis en attente pendant qu'un téléphone se connecte.
#if BLUETOOTH_ACTIF
static const char* NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";   // le téléphone écrit ici
static const char* NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";   // la carte répond ici (notifications)
static const size_t BT_TAMPON  = 2048;            // texte en attente d'envoi (au-delà, les plus vieilles lignes sautent)
static const unsigned long BT_PAS_MS = 15;        // un paquet au plus toutes les 15 ms (la radio suit sans perte)
static const unsigned long FENETRE_APPAIRAGE_MS = 120000UL;  // nouveau téléphone : 2 min après mise sous tension / « bt appairer »
static const unsigned long BT_SANS_APPAIRAGE_MS = 35000UL;   // connexion non appairée coupée au bout de 35 s (l'appairage
                                                             // lui-même abandonne au bout de 30 s : inutile d'attendre plus)
static const unsigned long BT_RECONNEXION_MS = 30000UL;      // après une déconnexion : le Wi-Fi attend aussi (btOccupe)
static const unsigned long BT_PREPARATION_MS = 15000UL;      // début de connexion : le Wi-Fi attend pour scanner (btOccupe)

NimBLECharacteristic* btTx = nullptr;
volatile bool btConnecte = false;     // un téléphone est connecté
volatile bool btAutorise = false;     // ... et appairé (chiffré + code vérifié) : on peut lui parler
volatile uint16_t btConnexion = 0xFFFF;
volatile uint16_t btMtu = 23;         // taille des paquets négociée (23 par défaut : 20 octets de texte)
volatile unsigned long tFenetreAppairage = 0;   // ouverture de la fenêtre d'appairage (0 = fermée)
volatile unsigned long tBtConnexion = 0;        // début de la connexion en cours
volatile unsigned long tBtDeconnexion = 0;   // 0 = pas de déconnexion récente
volatile bool btRefuser = false;                // nouveau téléphone hors fenêtre : à déconnecter (par loop)
volatile bool btCodeMontre = false;             // un code a été affiché pour cette connexion (nouvel appairage)
volatile bool btAbonne = false;                 // le téléphone s'est abonné aux réponses (notifications de TX)
volatile int  btRaisonDeco = -1;                // raison de la dernière déconnexion, affichée par loop() (-1 : rien)
volatile bool btCleInconnue = false;            // échec de chiffrement d'un téléphone inconnu : conseil affiché par loop()

char btTampon[BT_TAMPON];
size_t btLong = 0;
SemaphoreHandle_t mutexBt = NULL;
String btNom;

bool appairageOuvert() {
  unsigned long t = tFenetreAppairage;
  return t != 0 && millis() - t < FENETRE_APPAIRAGE_MS;
}

// Commande « bt appairer » (câble USB) : un nouveau téléphone peut s'appairer pendant FENETRE_APPAIRAGE_MS.
void ouvrirAppairage() {
  tFenetreAppairage = millis() | 1UL;
  Console.printf("Bluetooth : appairage ouvert pendant %lu s. Connecte-toi à %s et tape le code affiché sur l'écran.\n",
                 FENETRE_APPAIRAGE_MS / 1000UL, btNom.c_str());
}

// Retire les r premiers octets du tampon (r <= btLong ; mutexBt pris).
static void btRetirer(size_t r) {
  memmove(btTampon, btTampon + r, btLong - r);
  btLong -= r;
}

// Plus de téléphone (déconnexion, ou connexion fantôme) : ni connecté, ni appairé, ni abonné, rien à refuser.
static void btEffacerConnexion() {
  btConnecte = false; btAutorise = false; btAbonne = false; btRefuser = false; btCodeMontre = false;
}

void btEcrire(const uint8_t* b, size_t n) {
  if (!btAutorise || mutexBt == NULL) return;
  if (xSemaphoreTake(mutexBt, pdMS_TO_TICKS(20)) != pdTRUE) return;
  if (n > BT_TAMPON) { b += n - BT_TAMPON; n = BT_TAMPON; }
  if (btLong + n > BT_TAMPON) btRetirer(btLong + n - BT_TAMPON);   // plein : on oublie le début
  memcpy(btTampon + btLong, b, n);
  btLong += n;
  xSemaphoreGive(mutexBt);
}

// Appelée à chaque tour de loop() : envoie un paquet du tampon (taille négociée, 180 octets au plus).
void btEnvoyer(unsigned long now) {
  static unsigned long tPaquet = 0, tSurveillance = 0;
  static uint8_t incoherences = 0;
  // Messages notés par les rappels de NimBLE (autre tâche) : écrits ici, pour ne pas bloquer la pile Bluetooth.
  int raison = btRaisonDeco;
  if (raison >= 0) {
    btRaisonDeco = -1;
    // codes HCI + 0x200 : 0x208 liaison perdue, 0x213 fermée par le téléphone, 0x216 par la carte, 0x23E jamais établie
    // (radio occupée), 0x206 clé absente (appairage perdu d'un côté), 0x23D échec de chiffrement (MIC : le téléphone et la
    // carte n'ont pas la même clé), 0x205 code faux ou appairage raté
    Console.printf("Bluetooth : téléphone déconnecté (raison 0x%03X%s).\n", raison,
                   raison == 0x208 ? ", liaison perdue" : raison == 0x213 ? ", fermée par le téléphone" :
                   raison == 0x216 ? ", fermée par la carte" : raison == 0x23E ? ", liaison jamais établie : radio occupée" :
                   raison == 0x206 || raison == 0x23D ? ", clés différentes (appairage périmé d'un côté)" : raison == 0x205 ? ", appairage raté" : "");
  }
  if (btCleInconnue || raison == 0x23D || raison == 0x206) {
    btCleInconnue = false;
    Console.printf("Bluetooth : ce téléphone a sans doute gardé un ancien appairage que la carte n'a plus. Sur le téléphone : "
                   "réglages Bluetooth > %s > Oublier, puis 'bt appairer' au câble USB et reconnecte-toi.\r\n", btNom.c_str());
  }
  // Chaque seconde : la carte doit diffuser dès qu'aucun téléphone n'est connecté. Une liaison qui avorte avant
  // d'être établie (radio occupée) n'appelle ni onConnect ni onDisconnect : sans cette relance, la carte resterait
  // invisible jusqu'au prochain redémarrage (le téléphone n'obtiendrait que des « gatt status 133 »).
  if (now - tSurveillance >= 1000UL) {
    tSurveillance = now;
    NimBLEServer* serveur = NimBLEDevice::getServer();
    if (serveur != nullptr && serveur->getConnectedCount() == 0) {
      // marqué connecté alors que la pile n'a plus aucune connexion (deux contrôles de suite) : on remet tout à zéro
      if (btConnecte && ++incoherences >= 2) {
        btEffacerConnexion();
        codeAppairage = 0;
        LOG("Bluetooth : connexion fantôme effacée.\n");
      }
      if (!btConnecte && !NimBLEDevice::getAdvertising()->isAdvertising() && NimBLEDevice::startAdvertising())
        LOG("Bluetooth : diffusion relancée.\n");
    } else {
      incoherences = 0;
    }
  }
  // Connexion pas (encore) appairée : nouveau téléphone hors fenêtre d'appairage, ou qui traîne sans s'appairer.
  // (comparaison signée : onConnect, sur l'autre cœur, peut avoir noté une heure plus récente que « now »)
  if (btConnecte && !btAutorise && (btRefuser || (long)(now - tBtConnexion) >= (long)BT_SANS_APPAIRAGE_MS)) {
    bool refus = btRefuser;
    btRefuser = false;
    tBtConnexion = now;   // une seule tentative de déconnexion par délai
    NimBLEDevice::getServer()->disconnect(btConnexion);
    if (refus) Console.println("Bluetooth : nouveau téléphone refusé (appairage fermé). Pour en ajouter un : 'bt appairer' au câble USB, ou débranche / rebranche la carte.");
    else LOG("Bluetooth : connexion sans appairage coupée.\n");
    return;
  }
  // (rien avant l'abonnement du téléphone : Android jetterait ces paquets, et la 1re ligne se perdrait)
  if (!btAutorise || !btAbonne || btTx == nullptr || btLong == 0 || now - tPaquet < BT_PAS_MS) return;
  tPaquet = now;
  uint8_t paquet[180];
  size_t max = (btMtu > 3) ? btMtu - 3 : 20;
  if (max > sizeof(paquet)) max = sizeof(paquet);
  xSemaphoreTake(mutexBt, portMAX_DELAY);
  size_t n = (btLong < max) ? btLong : max;
  memcpy(paquet, btTampon, n);
  xSemaphoreGive(mutexBt);
  if (!btTx->notify(paquet, n, btConnexion)) return;   // radio occupée : on réessaie au prochain tour
  xSemaphoreTake(mutexBt, portMAX_DELAY);
  btRetirer(min(n, btLong));
  xSemaphoreGive(mutexBt);
}

// Lien sûr : chiffré + code vérifié + clé de 16 octets (128 bits)
static bool lienSur(const NimBLEConnInfo& info) {
  return info.isEncrypted() && info.isAuthenticated() && info.getSecKeySize() == 16;
}

class BtServeur : public NimBLEServerCallbacks {
  // Selon la version de NimBLE, le chiffrement d'un téléphone connu, le code ou l'échange de MTU peuvent arriver AVANT
  // onConnect : on part donc de l'état réel du lien, sans effacer ce qui s'est déjà passé (btRefuser, btCodeMontre et
  // btAbonne sont remis à zéro à la déconnexion).
  void onConnect(NimBLEServer* serveur, NimBLEConnInfo& info) override {
    btConnexion = info.getConnHandle();
    tBtConnexion = millis();
    btMtu = info.getMTU();
    btAutorise = lienSur(info);
    btConnecte = true;   // en dernier : loop() lit les valeurs ci-dessus dès qu'il voit la connexion
    // Appairage demandé seulement si le lien n'est pas déjà chiffré : un téléphone connu chiffre de lui-même ; une
    // 2e demande pendant son chiffrement le ferait parfois échouer.
    if (!info.isEncrypted()) NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer* serveur, NimBLEConnInfo& info, int raison) override {
    btEffacerConnexion();
    btMtu = 23;
    btRaisonDeco = raison;   // affichée par loop() (btEnvoyer)
    tBtDeconnexion = millis() | 1UL;   // le Wi-Fi laisse la radio au téléphone qui va sans doute se reconnecter
    codeAppairage = 0;
    if (mutexBt && xSemaphoreTake(mutexBt, pdMS_TO_TICKS(50)) == pdTRUE) {   // btEcrire peut écrire en même temps
      btLong = 0;
      xSemaphoreGive(mutexBt);
    }
    LOG("Bluetooth : téléphone déconnecté.\n");
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override { btMtu = mtu; }
  // Le téléphone demande le code (nouvel appairage) : on en tire un au hasard et on l'affiche sur l'écran,
  // SEULEMENT si la fenêtre d'appairage est ouverte. Sinon le code n'est montré nulle part : l'appairage échoue,
  // et loop() coupe la connexion (btEnvoyer). Les téléphones déjà appairés ne passent jamais par ici.
  uint32_t onPassKeyDisplay() override {
    uint32_t code = 100000 + esp_random() % 900000;
    if (!appairageOuvert()) {
      codeAppairage = 0;
      btRefuser = true;
    } else {
      codeAppairage = code;
      tCodeAppairage = millis();
      btCodeMontre = true;
    }
    return code;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    codeAppairage = 0;
    // chiffré + code vérifié + clé de 16 octets (128 bits)
    if (lienSur(info)) {
      btAutorise = true;
      if (btCodeMontre) tFenetreAppairage = 0;   // un nouveau téléphone par fenêtre : elle se referme
      // Liaison plus robuste : 30 à 50 ms entre deux échanges, et 6 s sans nouvelles avant de la déclarer perdue
      // (assez pour traverser un scan Wi-Fi, assez court pour libérer vite l'unique place de connexion).
      NimBLEDevice::getServer()->updateConnParams(info.getConnHandle(), 24, 40, 0, 600);
      // (gardé dans le tampon jusqu'à l'abonnement du téléphone, puis envoyé)
      Console.printf("Bluetooth : téléphone appairé (console %s). Tape 'aide'.\n", btNom.c_str());
    } else {
      // Appairage chiffré mais SANS code (« Just Works », téléphone qui se dit sans clavier) : NimBLE l'a quand même
      // enregistré ; on l'efface, sinon ces appairages s'accumulent et poussent dehors ceux des vrais téléphones.
      // (Pas d'effacement sur un simple échec de chiffrement : quelqu'un qui usurperait l'adresse d'un vrai téléphone
      // ferait sinon effacer son appairage.)
      // (ici, lien pas sûr : chiffré veut donc dire sans code vérifié ou clé de moins de 16 octets)
      if (info.isEncrypted())
        NimBLEDevice::deleteBond(info.getIdAddress());
      // Pas chiffré et téléphone inconnu de la carte : il présente sans doute une ancienne clé (carte effacée,
      // 'bt oublier'...). Il ne s'en sortira pas seul : loop() affiche quoi faire.
      // (pas un nouveau téléphone refusé, btRefuser, ni un code mal tapé, btCodeMontre)
      else if (!btRefuser && !btCodeMontre && !NimBLEDevice::isBonded(info.getIdAddress()))
        btCleInconnue = true;
      NimBLEDevice::getServer()->disconnect(info.getConnHandle());   // code faux ou appairage sans code : dehors
    }
  }
};

// Mémoire des appairages pleine (5 téléphones, CONFIG_BT_NIMBLE_MAX_BONDS) : NimBLE effacerait le plus ancien pour faire de la place. Hors de la
// fenêtre d'appairage, on refuse : personne ne peut pousser dehors les téléphones déjà appairés.
// Abonnements (CCCD) et fonctions du téléphone (CSFC) : NimBLE les mémorise aussi pour chaque téléphone appairé. Leur
// mémoire est dimensionnée pour 5 téléphones (platformio.ini) ; si elle débordait quand même, on n'efface JAMAIS un
// téléphone pour leur faire de la place (l'écriture du téléphone échoue : « write descriptor failed »).
class BtStockage : public NimBLEDeviceCallbacks {
  int onStoreStatus(struct ble_store_status_event* e, void* arg) override {
    if (e->event_code == BLE_STORE_EVENT_OVERFLOW && e->overflow.obj_type != BLE_STORE_OBJ_TYPE_OUR_SEC &&
        e->overflow.obj_type != BLE_STORE_OBJ_TYPE_PEER_SEC) return BLE_HS_ESTORE_CAP;
    if (!appairageOuvert()) return BLE_HS_ESTORE_CAP;
    // fenêtre ouverte : on ne fait de place (en oubliant le plus ancien) que pour un appairage AVEC code
    if (e->event_code == BLE_STORE_EVENT_OVERFLOW) return btCodeMontre ? ble_store_util_status_rr(e, arg) : BLE_HS_ESTORE_CAP;
    return 0;
  }
};

// Le téléphone s'abonne (ou se désabonne) aux réponses de la carte. NimBLE rappelle aussi cette fonction quand il
// rétablit l'abonnement mémorisé d'un téléphone appairé.
class BtAbonnement : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t valeur) override { btAbonne = (valeur & 1) != 0; }
};

class BtReception : public NimBLECharacteristicCallbacks {
  String ligne;
  // Fin de ligne : la commande (sans les espaces autour) part dans la file de loop(), puis la ligne repart à vide.
  void deposerLigne() {
    ligne.trim();
    if (ligne.length() > 0) deposerCommande(ligne.c_str(), SOURCE_BLUETOOTH);
    ligne = "";
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!lienSur(info)) return;   // pas appairé : ignoré
    NimBLEAttValue v = c->getValue();
    const uint8_t* d = v.data();
    for (size_t i = 0; i < v.size(); i++) {
      char ch = (char)d[i];
      if (ch == '\r' || ch == '\n') deposerLigne();
      else if ((uint8_t)ch >= 32 && ligne.length() < 100) ligne += ch;
    }
    // beaucoup d'applis envoient la ligne sans retour à la ligne : on l'exécute quand même
    deposerLigne();
  }
};

void initialiserBluetooth() {
  mutexBt = xSemaphoreCreateMutex();
  // nom = MAir- + les 4 derniers chiffres de l'adresse MAC (AA:BB:CC:DD:EE:FF -> MAir-EEFF)
  unsigned l = deviceId.length();
  char nom[12] = "MAir";
  if (l >= 5) snprintf(nom, sizeof(nom), "MAir-%.2s%s", deviceId.c_str() + l - 5, deviceId.c_str() + l - 2);
  btNom = nom;
  NimBLEDevice::init(btNom.c_str());
  NimBLEDevice::setPower(9);   // +9 dBm (3 par défaut) : la carte est sur secteur, la liaison gagne en portée et en marge
  NimBLEDevice::setDeviceCallbacks(new BtStockage());
  NimBLEDevice::setSecurityAuth(true, true, true);           // mémoriser le téléphone, code obligatoire, chiffrement moderne
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);    // la carte AFFICHE le code, le téléphone le tape
  NimBLEDevice::setMTU(185);
  // « Secure Connections » seulement, clé de 16 octets : déjà réglé dans platformio.ini (MYNEWT_VAL_BLE_SM_SC_ONLY),
  // forcé ici aussi (NimBLE 2.x le lit à chaque appairage), puis vérifié : on prévient si ce n'est pas actif.
  ble_hs_cfg.sm_sc_only = 1;
  if (!ble_hs_cfg.sm_sc_only || !ble_hs_cfg.sm_sc || !ble_hs_cfg.sm_mitm)
    Console.println("ATTENTION Bluetooth : appairage sécurisé (Secure Connections + code) NON actif !");
  // Option « bt demarrage on » : fenêtre d'appairage ouverte à la mise sous tension (débrancher / rebrancher, bouton
  // EN), jamais après un redémarrage logiciel (reboot à distance, mise à jour, plantage).
  if (btAppairageDemarrage && esp_reset_reason() == ESP_RST_POWERON) tFenetreAppairage = millis() | 1UL;
  NimBLEServer* serveur = NimBLEDevice::createServer();
  serveur->setCallbacks(new BtServeur());
  serveur->advertiseOnDisconnect(true);
  NimBLEService* service = serveur->createService(NUS_SERVICE);
  btTx = service->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
  btTx->setCallbacks(new BtAbonnement());
  NimBLECharacteristic* rx = service->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                                                           NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new BtReception());
  // (plus de service->start() : NimBLE démarre les services avec le serveur, au lancement de la diffusion ci-dessous)
  auto* pub = NimBLEDevice::getAdvertising();   // (NimBLEAdvertising)
  pub->setName(btNom.c_str());
  pub->start();
}

// Commande « bt liste » (câble USB) : téléphones appairés, numérotés pour « bt oublier <n> ».
void listerTelephones() {
  int n = NimBLEDevice::getNumBonds();
  if (n <= 0) { Console.println("Aucun téléphone Bluetooth appairé."); return; }
  Console.printf("Téléphones Bluetooth appairés (%d) :\n", n);
  for (int i = 0; i < n; i++) Console.printf("  %d) %s\n", i + 1, NimBLEDevice::getBondedAddress(i).toString().c_str());
}

// Commande « bt oublier <n> » (câble USB) : retire UN téléphone (numéro de « bt liste »).
void oublierTelephone(int numero) {
  int n = NimBLEDevice::getNumBonds();
  if (numero < 1 || numero > n) { Console.println("Numéro invalide : voir 'bt liste'."); return; }
  NimBLEAddress a = NimBLEDevice::getBondedAddress(numero - 1);
  if (!NimBLEDevice::deleteBond(a)) { Console.println("Suppression impossible."); return; }
  // s'il est connecté en ce moment, on le déconnecte
  if (btConnecte) {
    NimBLEConnInfo info = NimBLEDevice::getServer()->getPeerInfoByHandle(btConnexion);
    if (info.getIdAddress() == a) NimBLEDevice::getServer()->disconnect(btConnexion);
  }
  Console.printf("Téléphone %d (%s) oublié. Il lui faudra un nouvel appairage.\n", numero, a.toString().c_str());
}

// Commande « bt oublier » (câble USB) : efface les téléphones mémorisés.
void oublierTelephones() {
  NimBLEDevice::deleteAllBonds();
  if (btConnecte) NimBLEDevice::getServer()->disconnect(btConnexion);
  Console.println("Téléphones Bluetooth oubliés : un nouvel appairage (code à l'écran) sera demandé.");
}

// Un téléphone vient de se connecter (appairage, découverte des services, abonnement : les 15 premières secondes) : le
// Wi-Fi retarde ses scans et ses nouveaux essais, qui occuperaient la radio à ce moment-là. (Durée bornée : un téléphone
// qui reste connecté ne bloque pas le Wi-Fi ; une connexion sans appairage est coupée au bout de 35 s.)
// Aussi pendant BT_RECONNEXION_MS après une déconnexion : on se reconnecte souvent juste après (appli fermée puis
// rouverte). Si le Wi-Fi scanne ou cherche un réseau à ce moment-là (eduroam qui refuse, box absente), la radio est
// prise et le téléphone n'obtient que « gatt status 133 » ou « 147 » (connexion jamais établie).
bool btOccupe(unsigned long now) {
  if (tBtDeconnexion != 0 && (long)(now - tBtDeconnexion) >= (long)BT_RECONNEXION_MS) tBtDeconnexion = 0;
  return (btConnecte && (long)(now - tBtConnexion) < (long)BT_PREPARATION_MS) || (!btConnecte && tBtDeconnexion != 0);
}

#else   // BLUETOOTH_ACTIF 0
void btEcrire(const uint8_t*, size_t) {}
bool btOccupe(unsigned long) { return false; }
#endif

// =====================================================================
// AFFICHAGE (écran 64 x 32)
// =====================================================================
// Une seule police : la standard 5x7 (6 px par caractère en taille 1, 12 px en taille 2), nette sur un panneau LED.
// Le zéro est écrit sans barre (glyphe du "O") et le point décimal ne prend que 2 px : "12.5" et non "12 .5".
//
// Grille des pages de mesure (CO2, PM1, PM2.5, PM10), y en pixels :
//   0..6     en-tête : nom de la mesure (cyan) à gauche, unité à droite (CO2 : flèche de tendance au milieu)
//   8        filet sur toute la largeur, à la couleur du niveau
//   10..23   valeur en grand (taille 2), centrée dans x 0..47 | visage 13x13 à droite (x 50..62)
//   25..26   jauge fine : un segment par niveau ATMO (6 pour les PM, 4 pour le CO2), le niveau atteint allumé,
//            les autres en sourdine (on voit toute l'échelle et où l'on se trouve)
//   28..30   curseur (triangle), détaché de la jauge par 1 px : position exacte de la valeur dans son niveau
// Plus de témoins de 1 pixel dans les coins : Wi-Fi, envoi au serveur, capteurs et heure ont leur page à eux,
// PAGE_ETAT (grandes icônes), intercalée toutes les PERIODE_ETAT_MS pendant DUREE_ETAT_MS.

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
uint16_t colFond[NB_NIVEAUX];    // niveau non atteint : segment en sourdine de la jauge
// (PxMatrix code chaque couleur sur 4 bits par canal : en dessous de ~30 %, un vert ou un violet s'éteignent presque)
static const uint8_t SOURDINE_JAUGE = 110;  // ~43 % de la couleur pleine : on reconnaît chaque couleur de l'échelle,
                                            // le niveau atteint (plein) ressort quand même nettement

// Calcule une seule fois les couleurs pleines et atténuées de chaque niveau, à partir de PALETTE.
void initialiserCouleurs() {
  for (uint8_t i = 0; i < NB_NIVEAUX; i++) {
    colNiv[i]  = rgb565(PALETTE[i][0], PALETTE[i][1], PALETTE[i][2]);
    colFond[i] = atten(colNiv[i], SOURDINE_JAUGE);
  }
}

// Couleurs neutres (jamais utilisées pour un niveau de qualité). Pas de blanc : il rend mal sur un panneau LED.
static const uint16_t COL_TITRE   = rgb565(0, 255, 230);   // cyan vif : titres (les LED bleues sont faibles : on mise sur le vert)
static const uint16_t COL_DISCRET = rgb565(0, 150, 140);   // cyan sombre : unités, textes secondaires
static const uint16_t COL_FILET   = rgb565(0, 110, 150);   // filet quand il n'y a pas de niveau (attente, page ÉTAT)
static const uint16_t COL_PISTE   = rgb565(40, 45, 60);    // fond d'une barre de progression
static const uint16_t COL_ETEINT  = rgb565(60, 70, 90);    // arcs éteints du logo Wi-Fi (restent visibles sur 4 bits)
static const uint16_t COL_ERREUR  = rgb565(230, 0, 0);
static const uint16_t COL_OR      = rgb565(255, 170, 0);   // sable du sablier, statue du logo

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

// Curseur de la jauge : où se situe la valeur DANS son niveau (0.0 = début du segment, 1.0 = fin).
// Un niveau va du seuil précédent au seuil suivant. Premier niveau : à partir de "bas" (0 ug/m3 pour les PM,
// 400 ppm pour le CO2 = air extérieur). Dernier niveau, ouvert : jusqu'à 1,5 x le dernier seuil (curseur en butée au-delà).
float fractionDansNiveau(float v, int niv, const Echelle &e, float bas) {
  if (e.inclus) v = arrondiAffiche(v);
  float b = (niv <= 0) ? bas : e.seuils[niv - 1];
  float h = (niv < e.n) ? e.seuils[niv] : e.seuils[e.n - 1] * 1.5f;
  if (h <= b) return 0.5f;
  return constrain((v - b) / (h - b), 0.0f, 1.0f);
}

// INDICE GLOBAL : comment les polluants sont-ils « pondérés » ?
// Ils ne le sont PAS, et c'est voulu. On applique la règle officielle de l'indice ATMO (arrêté du 10 juillet 2020,
// https://www.atmo-france.org/article/lindice-atmo) : chaque polluant reçoit un SOUS-INDICE (1 à 6) sur sa propre
// échelle, et l'indice est le PIRE de ces sous-indices. Pas de moyenne : les seuils de chaque échelle sont déjà
// calés sur la nocivité du polluant (25 ug/m3 de PM2.5 et 50 ug/m3 de PM10 donnent tous deux « mauvais ») ;
// une moyenne, pondérée ou non, laisserait un polluant très élevé être « caché » par un autre resté bas.
// Ce qui entre dans l'indice :
//   - PM2.5 et PM10 : oui (seuils ATMO officiels) ;
//   - PM1  : oui, avec les MÊMES seuils que les PM2.5 (l'indice ATMO ne fixe pas de seuil propre aux PM1).
//            Les PM1 font partie des PM2.5 (PM1 <= PM2.5) : elles ne changent l'indice que si le capteur les
//            mesure plus haut ; à niveau égal, c'est PM2.5 qui est affiché comme responsable ;
//   - CO2  : non, ce n'est pas un polluant de l'indice ATMO mais un indicateur de CONFINEMENT (aération) :
//            il garde ses propres pages (CO2, AERER) et ne modifie jamais l'indice.
// L'ATMO officiel inclut aussi O3, NO2 et SO2, que la carte ne mesure pas : cet indice est donc PARTIEL (particules).
// Renvoie -1 si le NextPM n'est pas prêt, + le nom du polluant responsable (et sa Mesure / son échelle pour le curseur).
int niveauIndiceATMO(const char** pire, const Mesure** mPire = nullptr, const Echelle** ePire = nullptr) {
  int n25 = niveauMesure(mPM25, ECH_PM25);
  int n10 = niveauMesure(mPM10, ECH_PM10);
  int n1  = niveauMesure(mPM1,  ECH_PM25);   // PM1 : seuils des PM2.5
  int pireN = n25;
  const char* nom = "PM2.5";
  const Mesure* m = &mPM25;
  const Echelle* e = &ECH_PM25;
  if (n10 > pireN) { pireN = n10; nom = "PM10"; m = &mPM10; e = &ECH_PM10; }
  if (n1  > pireN) { pireN = n1;  nom = "PM1";  m = &mPM1;  e = &ECH_PM25; }  // strictement pire seulement : à égalité, PM2.5 / PM10 restent affichés
  if (pire) *pire = nom;
  if (mPire) *mPire = m;
  if (ePire) *ePire = e;
  return pireN;
}

// Vrai si le CO2 dépasse le seuil d'aération : déclenche la page « AERER ».
bool alerteAerer() {
  return mCO2.etat == CAPTEUR_OK && mCO2.valeur > SEUIL_AERER_PPM;   // 1000 pile = encore « moyen »
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

// ---------- texte (police standard 5x7) ----------
// Largeur réelle d'un texte (sans l'espace final)
int largeurTexte(const char* s, uint8_t taille) {
  return (int)strlen(s) * 6 * taille - taille;
}

// (x, y) = coin haut-gauche ; un caractère de taille 1 fait 5 px de large et 7 de haut.
// Le "0" de cette police est barré : on dessine le "O", identique sans la barre (plus lisible en grand).
void ecrireTexte(const char* s, int x, int y, uint8_t taille, uint16_t couleur) {
  display.setFont(nullptr);
  display.setTextSize(taille);
  display.setTextColor(couleur);
  display.setCursor(x, y);
  for (const char* p = s; *p; p++) display.write((uint8_t)(*p == '0' ? 'O' : *p));
}

void ecrireCentre(const char* s, int cx, int y, uint8_t taille, uint16_t couleur) {
  ecrireTexte(s, cx - largeurTexte(s, taille) / 2, y, taille, couleur);
}

// xFin = dernière colonne occupée par le texte
void ecrireDroite(const char* s, int xFin, int y, uint8_t taille, uint16_t couleur) {
  ecrireTexte(s, xFin - largeurTexte(s, taille) + 1, y, taille, couleur);
}

// Nombre avec un point décimal ÉTROIT (taille x taille pixels, au lieu d'un caractère entier)
int largeurNombre(const char* s, uint8_t t) {
  int w = 0;
  for (const char* p = s; *p; p++) w += (*p == '.') ? 2 * t : 6 * t;
  return w - t;
}

// (x, y) = coin haut-gauche des chiffres
void ecrireNombre(const char* s, int x, int y, uint8_t t, uint16_t c) {
  char ch[2] = {0, 0};
  for (const char* p = s; *p; p++) {
    if (*p == '.') { display.fillRect(x, y + 6 * t, t, t, c); x += 2 * t; continue; }
    ch[0] = *p;
    ecrireTexte(ch, x, y, t, c);
    x += 6 * t;
  }
}

void ecrireNombreCentre(const char* s, int cx, int y, uint8_t t, uint16_t c) {
  ecrireNombre(s, cx - largeurNombre(s, t) / 2, y, t, c);
}

// Valeur de particules : 1 décimale sous 10, entier au-dessus, 999 au plus (comme arrondiAffiche)
void formaterPM(char* buf, size_t n, float v) {
  v = constrain(arrondiAffiche(v), 0.0f, 999.0f);
  if (v < 9.95f) snprintf(buf, n, "%d.%d", (int)v, (int)roundf(v * 10.0f) % 10);
  else snprintf(buf, n, "%d", (int)v);
}

// Unité des particules fines : µg/m³ (masse par volume d'air ; le ppm ne s'applique qu'aux gaz comme le CO2).
// La police 5x7 n'a ni "µ" ni "³" : on les dessine pixel par pixel, "g/m" vient de la police.
// Largeur 27 px : de xFin - 26 à xFin ; y = haut du texte.
void ecrireUnitePM(int xFin, int y, uint16_t c) {
  const int x = xFin - 26;
  for (int r = 2; r <= 7; r++) display.drawPixel(x, y + r, c);       // µ : jambage gauche (descend sous la ligne)
  for (int r = 2; r <= 6; r++) display.drawPixel(x + 4, y + r, c);   // µ : jambage droit
  display.drawFastHLine(x + 1, y + 6, 3, c);                         // µ : fond arrondi
  ecrireTexte("g/m", x + 6, y, 1, c);                                // x + 6 .. x + 22
  display.drawFastHLine(x + 24, y, 3, c);                            // ³ : petit 3 en exposant (3 x 5 px)
  display.drawPixel(x + 26, y + 1, c);
  display.drawFastHLine(x + 25, y + 2, 2, c);
  display.drawPixel(x + 26, y + 3, c);
  display.drawFastHLine(x + 24, y + 4, 3, c);
}

// ---------- éléments communs ----------
// En-tête complet : titre cyan à gauche (x 1), texte à droite (fin en x 62, si droite), filet dessous (y 8) de
// couleur colFilet. Sert aussi aux pannes ("PANNE" en rouge à droite).
// En-tête des écrans Wi-Fi : "WIFI" à gauche, état à droite, filet de la couleur de l'état
void dessinerEnteteTexte(const char* titre, const char* droite, uint16_t colDroite, uint16_t colFilet) {
  ecrireTexte(titre, 1, 0, 1, COL_TITRE);
  if (droite) ecrireDroite(droite, 62, 0, 1, colDroite);
  display.drawFastHLine(0, 8, 64, colFilet);
}

// En-tête : titre cyan à gauche (x 1), filet dessous (y 8) à la couleur du niveau (niv < 0 : filet neutre).
void dessinerEntete(const char* titre, int niv) {
  dessinerEnteteTexte(titre, nullptr, 0, (niv >= 0) ? colNiv[niv] : COL_FILET);
}

// Deux pixels symétriques par rapport à la colonne cx : (cx - dx, y) et (cx + dx, y) (flèche, visage).
void pixelsSymetriques(int cx, int dx, int y, uint16_t c) {
  display.drawPixel(cx - dx, y, c);
  display.drawPixel(cx + dx, y, c);
}

// Flèche de tendance 5x5 (cx = centre, y = haut) : dir > 0 monte | dir < 0 descend | 0 stable
// (montée et descente : même hampe, pointe en y + 1 ou en y + 3)
void dessinerFleche(int cx, int y, int dir, uint16_t c) {
  if (dir != 0) {
    display.drawFastVLine(cx, y, 5, c);
    pixelsSymetriques(cx, 1, (dir > 0) ? y + 1 : y + 3, c);
    pixelsSymetriques(cx, 2, y + 2, c);
  } else {
    display.drawFastHLine(cx - 2, y + 2, 5, c);
    display.drawPixel(cx + 1, y + 1, c); display.drawPixel(cx + 1, y + 3, c);
  }
}

// Jauge (y 25..26) : un segment par niveau, 1 px d'écart, sur toute la largeur (x 0..63).
// Le niveau atteint est allumé à fond, les autres en sourdine : on voit toute l'échelle ATMO d'un coup d'oeil.
// Curseur (y 28..30) : triangle à la couleur du niveau, pointé sur la position EXACTE de la valeur dans son niveau
// (frac : 0.0 = début du segment, 1.0 = fin). Il est DÉTACHÉ de la jauge (ligne 27 vide) et la jauge reste
// entière : l'ancien triangle collé sous une encoche noire formait un « T » qui coupait le segment et se lisait mal.
// niv < 0 : toute la jauge en sourdine, sans curseur.
void dessinerJauge(int nbNiveaux, int niv, float frac) {
  int xCurseur = -1;
  for (int i = 0; i < nbNiveaux; i++) {
    int xs = (i * 65) / nbNiveaux;
    int xe = ((i + 1) * 65) / nbNiveaux - 2;     // 1 px de séparation ; le dernier segment finit à x 63
    display.fillRect(xs, 25, xe - xs + 1, 2, (i == niv) ? colNiv[i] : colFond[i]);
    if (i == niv) xCurseur = xs + (int)(frac * (xe - xs) + 0.5f);
  }
  if (xCurseur < 0) return;
  uint16_t c = colNiv[niv];
  int xt = constrain(xCurseur, 2, 61);                         // le triangle (5 px de large) reste dans l'écran
  display.drawPixel(xt, 28, c);
  display.drawFastHLine(xt - 1, 29, 3, c);
  display.drawFastHLine(xt - 2, 30, 5, c);
}

// Visage 13 x 13 (x, y = coin haut-gauche), traits en noir, une expression par niveau ATMO :
//   0 grand sourire | 1 sourire | 2 bouche droite | 3 bouche triste | 4 bouche ouverte + sourcils froncés |
//   5 yeux en croix + grande bouche ouverte | -1 (capteurs pas prêts) : bouche droite
void dessinerVisage(int x, int y, int niv, uint16_t c, bool clin) {
  int cx = x + 6, cy = y + 6;
  display.fillCircle(cx, cy, 6, c);
  // yeux
  if (niv >= 5) {                                          // yeux en croix (3 x 3)
    for (int k = -1; k <= 1; k += 2) {
      int ex = cx + 3 * k, ey = cy - 2;
      pixelsSymetriques(ex, 1, ey - 1, 0);
      display.drawPixel(ex, ey, 0);
      pixelsSymetriques(ex, 1, ey + 1, 0);
    }
  } else if (clin) {                                       // clignement
    display.drawFastHLine(cx - 3, cy - 2, 2, 0);
    display.drawFastHLine(cx + 2, cy - 2, 2, 0);
  } else {
    display.fillRect(cx - 3, cy - 3, 2, 2, 0);
    display.fillRect(cx + 2, cy - 3, 2, 2, 0);
  }
  if (niv == 4) {                                          // sourcils froncés, pointes vers le nez
    pixelsSymetriques(cx, 4, cy - 5, 0);
    pixelsSymetriques(cx, 3, cy - 4, 0);
  }
  // bouche
  switch (niv) {
    case 0:                                                // grand sourire
      pixelsSymetriques(cx, 3, cy + 1, 0);                 // = coins relevés + le sourire du niveau 1
      // fall through
    case 1:                                                // sourire
      pixelsSymetriques(cx, 2, cy + 2, 0);
      display.drawFastHLine(cx - 1, cy + 3, 3, 0);
      break;
    case 3:                                                // triste
      display.drawFastHLine(cx - 1, cy + 2, 3, 0);
      pixelsSymetriques(cx, 2, cy + 3, 0);
      break;
    case 4:                                                // bouche ouverte
      display.fillRect(cx - 1, cy + 2, 3, 3, 0);
      break;
    case 5:                                                // grande bouche ouverte
      display.fillRect(cx - 2, cy + 1, 5, 4, 0);
      break;
    default:                                               // 2 et -1 : bouche droite
      display.drawFastHLine(cx - 2, cy + 3, 5, 0);
      break;
  }
}

// Dessine un motif écrit en texte (une chaîne par rangée : largeur x hauteur ; x, y = coin haut-gauche) :
// le caractère codes[i] prend la couleur couleurs[i] ; tout autre caractère ('.') reste noir (rien n'est dessiné).
// Sert au sablier, au nuage, au ventilateur et à la basilique du logo.
void dessinerMotif(const char* const* motif, int largeur, int hauteur, int x, int y, const char* codes,
                   const uint16_t* couleurs) {
  for (int r = 0; r < hauteur; r++) {
    for (int k = 0; k < largeur; k++) {
      const char* p = strchr(codes, motif[r][k]);
      if (p) display.drawPixel(x + k, y + r, couleurs[p - codes]);
    }
  }
}

// Sablier 9 x 13 (x, y = coin haut-gauche) : capteur qui chauffe / démarre.
//   C cadre (cyan) | S sable (or)
static const char* SABLIER[13] = {
  "CCCCCCCCC",
  ".C.....C.",
  ".CSSSSSC.",
  "..CSSSC..",
  "...CSC...",
  "....C....",
  "....S....",
  "....C....",
  "...C.C...",
  "..C.S.C..",
  ".C.SSS.C.",
  ".CSSSSSC.",
  "CCCCCCCCC",
};
void dessinerSablier(int x, int y) {
  static const uint16_t COULEURS[2] = {COL_TITRE, COL_OR};   // C | S
  dessinerMotif(SABLIER, 9, 13, x, y, "CS", COULEURS);
}

// Icône d'erreur : cercle rouge barré d'une croix (13 x 13)
void dessinerIconeErreur(int x, int y, uint16_t c) {
  int cx = x + 6, cy = y + 6;
  display.drawCircle(cx, cy, 6, c);
  display.drawLine(cx - 3, cy - 3, cx + 3, cy + 3, c);
  display.drawLine(cx + 3, cy - 3, cx - 3, cy + 3, c);
}

// ---------- pages CO2, PM1, PM2.5, PM10 : UNE mesure par page ----------
// CO2 : unité "PPM" + flèche de tendance (3 dernières minutes) ; jauge à 4 niveaux (800 / 1000 / 1500 ppm).
// PM  : unité µg/m³ ; jauge à 6 niveaux ATMO ; PM1 suit les MÊMES seuils que les PM2.5 (couleur, alerte, indice).
// Polluant en alerte : la valeur pulse (vif / atténué toutes les ALERTE_PULSE_MS).
static const float BAS_JAUGE_CO2 = 400;   // le curseur part de l'air extérieur (~400 ppm)
bool alertePage(uint8_t p);

void dessinerPageMesure(const Mesure &m, const char* titre, const Echelle &e, uint8_t page) {
  const bool co2 = (&m == &mCO2);
  const int nbNiv = e.n + 1;

  if (m.etat == CAPTEUR_CHAUFFE) {                    // préchauffage / démarrage : sablier
    dessinerEntete(titre, -1);
    dessinerSablier(52, 10);
    if (co2) {                                        // MH-Z19C : compte à rebours m:ss + barre de progression
      unsigned long t = millis();
      unsigned long reste = (t < CHAUFFE_MHZ_MS) ? (CHAUFFE_MHZ_MS - t + 999UL) / 1000UL : 0;
      char buf[12];
      // minutes:secondes ("2:18") : un nombre seul ("138") se lisait comme une valeur de CO2 en ppm
      snprintf(buf, sizeof(buf), "%lu:%02lu", reste / 60UL, reste % 60UL);
      ecrireDroite("CHAUFFE", 62, 0, 1, COL_DISCRET);
      ecrireCentre(buf, 24, 10, 2, COL_TITRE);
      display.fillRect(0, 25, 64, 2, COL_PISTE);       // même épaisseur que la jauge des niveaux
      int w = (t < CHAUFFE_MHZ_MS) ? (int)(64UL * t / CHAUFFE_MHZ_MS) : 64;
      if (w > 0) display.fillRect(0, 25, w, 2, COL_TITRE);
    } else {                                          // NextPM : durée inconnue
      ecrireCentre("--", 24, 10, 2, COL_DISCRET);
      ecrireCentre("DEMARRAGE", 32, 25, 1, COL_DISCRET);
    }
    return;
  }
  if (m.etat == CAPTEUR_ERREUR) {                     // panne, ou CO2 impossible (capteur à recalibrer)
    bool calib = co2 && co2HorsPlage;
    dessinerEnteteTexte(titre, calib ? nullptr : "PANNE", COL_ERREUR, COL_ERREUR);
    ecrireCentre(calib ? "CAL" : "ERR", 24, 10, 2, COL_ERREUR);
    dessinerIconeErreur(50, 10, COL_ERREUR);
    if (calib) ecrireCentre("A CALIBRER", 32, 25, 1, COL_ERREUR);
    else for (int x = 0; x < 64; x += 4) display.drawFastHLine(x, 26, 2, atten(COL_ERREUR, 140));
    return;
  }

  float v = valAff(m);
  int niv = niveau(v, e);
  uint16_t c = colNiv[niv];
  dessinerEntete(titre, niv);
  char buf[8];
  if (co2) {
    ecrireDroite("PPM", 62, 0, 1, COL_DISCRET);
    int dir;
    if (tendanceCO2(dir)) dessinerFleche(31, 1, dir, couleurTendance(dir));
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(constrain(v, 0.0f, 9999.0f)));
  } else {
    ecrireUnitePM(62, 0, COL_DISCRET);
    formaterPM(buf, sizeof(buf), v);
  }
  uint16_t cVal = c;
  if (alertePage(page) && ((millis() / ALERTE_PULSE_MS) % 2UL) == 1) cVal = atten(c, 110);
  ecrireNombreCentre(buf, 24, 10, 2, cVal);
  dessinerVisage(50, 10, niv, c, yeuxFermes());
  dessinerJauge(nbNiv, niv, fractionDansNiveau(v, niv, e, co2 ? BAS_JAUGE_CO2 : 0.0f));
}

// ---------- page AERER (seulement quand le CO2 dépasse SEUIL_AERER_PPM) ----------
// Deux rubans de signalisation (rayures en diagonale qui défilent en sens inverse), "AERER" en grand qui pulse,
// et en bas : CO2 <valeur> + flèche de tendance.
uint16_t phaseRuban = 0;    // décalage des rayures (avance toutes les 120 ms, voir animer)

void dessinerRuban(int y, int h, int sens, uint16_t c) {
  for (int yy = 0; yy < h; yy++) {
    for (int x = 0; x < 64; x++) {
      int v = x + sens * yy - sens * (int)phaseRuban;
      if ((v & 7) < 4) display.drawPixel(x, y + yy, c);
    }
  }
}

void dessinerPageAerer() {
  int niv = niveauMesure(mCO2, ECH_CO2);
  if (niv < 0) niv = 3;
  uint16_t c = colNiv[niv];
  uint16_t cr = atten(c, 150);
  dessinerRuban(0, 3, 1, cr);                                       // y 0..2
  bool fort = ((millis() / 600UL) % 2UL) == 0;                      // pulsation : vif / atténué toutes les 0,6 s
  ecrireCentre("AERER", 32, 5, 2, fort ? c : atten(c, 120));        // y 5..18
  dessinerRuban(20, 3, -1, cr);                                     // y 20..22

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(mCO2.valeur));
  ecrireTexte("CO2", 1, 25, 1, COL_TITRE);                          // x 1..17
  ecrireTexte(buf, 22, 25, 1, c);                                   // 4 chiffres max : x 22..44
  int dir;
  if (tendanceCO2(dir)) dessinerFleche(58, 26, dir, couleurTendance(dir));
  else ecrireDroite("PPM", 62, 25, 1, COL_DISCRET);
}

// ---------- page INDICE ATMO (synthèse des particules, règle du pire sous-indice : voir niveauIndiceATMO) ----------
// En-tête : ATMO + polluant responsable (quand l'air n'est pas "bon") | visage + qualificatif ATMO (1 ou 2 lignes)
// | jauge des 6 niveaux, curseur placé d'après le polluant responsable. Le CO2 n'y entre pas.
void dessinerPageGlobal() {
  const char* cause = "";
  const Mesure* mp = nullptr;
  const Echelle* ep = nullptr;
  int niv = niveauIndiceATMO(&cause, &mp, &ep);
  if (niv < 0) {
    if (mPM25.etat == CAPTEUR_ERREUR) {                  // NextPM en panne : pas d'indice (avant : « ATTENTE » + sablier,
      dessinerEnteteTexte("ATMO", "PANNE", COL_ERREUR, COL_ERREUR);   // comme s'il démarrait)
      dessinerIconeErreur(1, 10, COL_ERREUR);
      ecrireCentre("INDISPO", 39, 13, 1, COL_ERREUR);
    } else {                                             // capteurs pas encore prêts
      dessinerEntete("ATMO", -1);
      dessinerSablier(3, 10);
      ecrireCentre("ATTENTE", 38, 13, 1, COL_TITRE);
    }
    dessinerJauge(NB_NIVEAUX, -1, 0);
    return;
  }
  uint16_t c = colNiv[niv];
  dessinerEntete("ATMO", niv);
  if (niv > 0) ecrireDroite(cause, 62, 0, 1, c);
  dessinerVisage(1, 10, niv, c, yeuxFermes());
  // qualificatif : 7 caractères par ligne (x 16..62) ; plus long : coupé en 2 lignes à l'espace
  const char* label = LABELS_NIVEAU[niv];
  const char* espace = strchr(label, ' ');
  if (!espace) {
    ecrireCentre(label, 39, 13, 1, c);
  } else {
    char l1[12];
    size_t n = min((size_t)(espace - label), sizeof(l1) - 1);
    memcpy(l1, label, n);
    l1[n] = '\0';
    ecrireCentre(l1, 39, 10, 1, c);
    ecrireCentre(espace + 1, 39, 17, 1, c);   // y 17..23 : une ligne vide (24) avant la jauge, sinon le texte la touchait
  }
  dessinerJauge(NB_NIVEAUX, niv, fractionDansNiveau(mp->valeur, niv, *ep, 0.0f));
}

// ---------- page courbe CO2 ----------
// Histogramme pensé pour un panneau LED : formes pleines, couleurs vives, rien de fin.
//  - COURBE_NB_BARRES barres de 3 px (1 px d'écart), chacune = moyenne de COURBE_MIN_PAR_BARRE minutes ;
//    la plus récente à droite (elle inclut la minute en cours) : 15 x 4 min = la dernière heure
//  - échelle FIXE 400..2000 ppm : une hauteur veut toujours dire la même chose (au-delà : pleine hauteur)
//  - à gauche, une règle de 2 px aux couleurs des niveaux : on lit d'un coup d'oeil où se situe chaque barre
//  - minute sans mesure (capteur indisponible) : ignorée ; barre sans aucune mesure : vide (l'axe du temps reste juste)
//  - en-tête : CO2, "1H", flèche de tendance et valeur actuelle ; le sommet de la barre en cours clignote (ANIMATIONS)
//   0..6 en-tête | 8 filet | 10..31 barres
static const int COURBE_MIN_PPM = 400;
static const int COURBE_MAX_PPM = 2000;
static_assert(COURBE_NB_BARRES * COURBE_MIN_PAR_BARRE == HIST_N + 1, "HIST_N minutes terminées + la minute en cours");

// Moyenne d'une barre (j = 0 : la plus récente) ; 0 = aucune mesure dans ces minutes
float valeurBarre(int j) {
  float somme = 0;
  int n = 0;
  for (int k = 0; k < COURBE_MIN_PAR_BARRE; k++) {
    int depuisFin = j * COURBE_MIN_PAR_BARRE + k;  // 0 = minute en cours, 1 = dernière minute terminée...
    uint16_t v = 0;
    if (depuisFin == 0) v = minuteEnCours();
    else if (depuisFin <= (int)histN) v = valeurHistorique(histN - depuisFin);
    if (v > 0) { somme += v; n++; }
  }
  return n ? somme / n : 0.0f;
}

void dessinerPageCourbe() {
  // valeur actuelle : la mesure en direct, sinon la dernière minute connue
  float actuel = (mCO2.etat == CAPTEUR_OK) ? mCO2.valeur : 0.0f;
  for (int i = (int)histN - 1; i >= 0 && actuel <= 0; i--) actuel = valeurHistorique(i);
  if (actuel <= 0) {
    dessinerEntete("CO2", -1);
    ecrireCentre("ATTENTE", 32, 16, 1, COL_DISCRET);
    return;
  }
  int nvAct = niveau(actuel, ECH_CO2);
  dessinerEntete("CO2", nvAct);
  ecrireTexte("1H", 21, 0, 1, COL_DISCRET);   // x 21..31 : 3 px après CO2, 2 px avant la flèche (CO2 à 4 chiffres)
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)roundf(actuel));
  ecrireDroite(buf, 62, 0, 1, colNiv[nvAct]);
  int dir;
  if (tendanceCO2(dir)) dessinerFleche(59 - largeurTexte(buf, 1), 1, dir, couleurTendance(dir));

  const int yBase = 31, hMax = yBase - 9;             // zone de dessin : y 10..31 (22 px)
  // règle des niveaux (x 0..1)
  for (int y = 10; y <= yBase; y++) {
    float v = COURBE_MIN_PPM + (yBase - y) * (float)(COURBE_MAX_PPM - COURBE_MIN_PPM) / (hMax - 1);
    display.drawFastHLine(0, y, 2, colNiv[niveau(v, ECH_CO2)]);
  }
  // barres : j = 0 pour la plus récente (x 61..63), puis de 4 px en 4 px vers la gauche
  for (int j = 0; j < COURBE_NB_BARRES; j++) {
    float v = valeurBarre(j);
    if (v <= 0) continue;
    int h = 1 + (int)((v - COURBE_MIN_PPM) * (hMax - 1) / (float)(COURBE_MAX_PPM - COURBE_MIN_PPM) + 0.5f);
    h = constrain(h, 1, hMax);
    int xr = 63 - 4 * j;
    int yT = yBase - h + 1;
    uint16_t c = colNiv[niveau(v, ECH_CO2)];
    display.fillRect(xr - 2, yT, 3, h, c);
#if ANIMATIONS
    if (j == 0 && phasePoint) display.drawFastHLine(xr - 2, yT, 3, COL_TITRE);  // barre en cours : sommet qui clignote
#endif
  }
}

// ---------- Wi-Fi : logo, page, écran de connexion ----------
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
  return colNiv[3 - (nv & 3)];  // 0 arc = rouge ... 3 arcs = vert
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

// Page Wi-Fi de la rotation : seulement quand la carte N'EST PAS connectée (connectée, le Wi-Fi est sur la page ÉTAT).
//   0..6 "WIFI" + état | 8 filet | logo 13x11 (y 10..20) + état en clair (y 10) + détail (y 18) | 25..31 réseau / aide
void dessinerPageWifi() {
#if WIFI_ACTIVE
  dessinerEnteteTexte("WIFI", "OFF", COL_ERREUR, COL_ERREUR);
  dessinerLogoWifi(8, 20, -1, COL_ETEINT, COL_ETEINT);
  dessinerCroix(11, 16);
  if (nbReseaux == 0) {
    ecrireTexte("AUCUN", 19, 10, 1, COL_ERREUR);
    ecrireTexte("RESEAU", 19, 18, 1, COL_ERREUR);
    ecrireLigneDefilante("tape: wifi add <nom> <mdp>", 25, COL_DISCRET);
    return;
  }
  const char* etat = (etatWifi == WIFI_SCAN) ? "RECHER" : wifiEnConnexion() ? "CONNEX" : "ABSENT";
  ecrireTexte(etat, 19, 10, 1, COL_TITRE);
  char mem[16];
  snprintf(mem, sizeof(mem), "%u CONNU%s", (unsigned)nbReseaux, nbReseaux > 1 ? "S" : "");
  ecrireTexte(mem, 19, 18, 1, COL_DISCRET);
  // (19 + 8 x 6 = 67 : "5 CONNUS" déborde de 3 px à droite, sans gêne)
  if (wifiEnConnexion() && reseauCourant >= 0) ecrireLigneDefilante(reseaux[reseauCourant].ssid.c_str(), 25, COL_TITRE);
  else ecrireLigneDefilante("HORS LIGNE", 25, COL_DISCRET);
#endif
}

// L'écran de connexion recouvre les pages : pendant une connexion "interactive"
// (démarrage, commande wifi) et quelques secondes pour annoncer le résultat.
bool ecranWifiActif() {
#if WIFI_ACTIVE
  if (resultatWifiAffiche) return true;
  return wifiInteractif && (etatWifi == WIFI_SCAN || wifiEnConnexion());
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
      snprintf(buf, sizeof(buf), "%ddB", rssi);
      dessinerEnteteTexte("WIFI", buf, couleurSignal(niveauSignal(rssi)), colNiv[0]);
      dessinerLogoWifi(8, 20, 3, colNiv[0], COL_ETEINT);
      ecrireCentre("OK", 40, 10, 2, colNiv[0]);
      ecrireLigneDefilante(WiFi.SSID().c_str(), 25, COL_TITRE);
    } else {                                              // échec : logo éteint + croix, raison
      dessinerEnteteTexte("WIFI", "ECHEC", COL_ERREUR, COL_ERREUR);
      dessinerLogoWifi(8, 20, -1, COL_ETEINT, COL_ETEINT);
      dessinerCroix(11, 16);
      ecrireTexte(nbCandidats ? raisonCourte(raisonDeco) : "ABSENT", 19, 10, 1, COL_ERREUR);
      ecrireTexte("ESSAI", 19, 18, 1, COL_DISCRET);
      ecrireCentre("DANS 30 S", 32, 25, 1, COL_DISCRET);
    }
    return;
  }
  // Connexion en cours : les arcs s'allument l'un après l'autre
  int phase = (int)((now / 300UL) % 4UL) - 1;            // -1 (point seul) .. 2
  dessinerLogoWifi(8, 20, phase + 1, COL_TITRE, COL_ETEINT);
  if (etatWifi == WIFI_SCAN) {
    dessinerEnteteTexte("WIFI", "SCAN", COL_TITRE, COL_FILET);
    ecrireTexte("RECHER", 19, 10, 1, COL_TITRE);
    char mem[16];
    snprintf(mem, sizeof(mem), "%u CONNU%s", (unsigned)nbReseaux, nbReseaux > 1 ? "S" : "");
    ecrireLigneDefilante(mem, 25, COL_DISCRET);
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(idxCandidat + 1), (unsigned)nbCandidats);
    dessinerEnteteTexte("WIFI", buf, COL_TITRE, COL_FILET);
    ecrireTexte((essaiCandidat > 0 || etatWifi == WIFI_REESSAI) ? "ESSAI 2" : "CONNEX", 19, 10, 1, COL_TITRE);
    if (reseauCourant >= 0) ecrireLigneDefilante(reseaux[reseauCourant].ssid.c_str(), 25, COL_TITRE);
  }
  // points animés (y 20..21) + secondes écoulées (y 18..24)
  int nbPoints = (int)((now / 400UL) % 4UL);
  for (int i = 0; i < nbPoints; i++) display.fillRect(19 + i * 4, 20, 2, 2, COL_TITRE);
  char sec[12];
  // secondes depuis le 1er essai du réseau (elles continuent pendant le 2e essai au lieu de repartir de 0)
  snprintf(sec, sizeof(sec), "%lus", (now - (wifiEnConnexion() ? tDebutCandidat : tEtatWifi)) / 1000UL);
  ecrireDroite(sec, 62, 18, 1, COL_DISCRET);
#endif
}

// ---------- page ÉTAT : toutes les icônes sur une page ----------
// Intercalée dans la rotation toutes les PERIODE_ETAT_MS, pendant DUREE_ETAT_MS (voir pageSuivante).
//   0..6    heure (cyan) à gauche | date à droite (ou "BATT" en orange sur batterie) ; "--:--" sans heure NTP
//   8       filet
//   10..21  3 grandes icônes, une par tuile de 21 px : Wi-Fi | serveur (nuage) | capteurs (ventilateur)
//   24..30  sous chaque icône, un mot de 3 caractères au plus, de la couleur de l'icône
// Couleurs : vert = OK | jaune / orange = à surveiller | rouge = problème | cyan = en cours | gris = rien encore
static const int X_TUILE[3] = {10, 32, 53};            // centre de chaque tuile

// Nuage 15 x 10 creusé d'une flèche vers le haut (envoi au serveur), (cx, y) = centre / haut
static const char* NUAGE[10] = {
  ".....#####.....",
  "...####.####...",
  "..####...####..",
  ".####.....####.",
  "#######.#######",
  "#######.#######",
  "#######.#######",
  "#######.#######",
  ".#############.",
  "..###########..",
};
void dessinerNuage(int cx, int y, uint16_t c) {
  dessinerMotif(NUAGE, 15, 10, cx - 7, y, "#", &c);
}

// Ventilateur 11 x 11, 4 pales (capteurs : le NextPM aspire l'air avec un ventilateur), (cx, cy) = centre
static const char* VENTILO[11] = {
  "....###....",
  "...####....",
  "...###.....",
  "....##..##.",
  "##..#######",
  "#####.#####",
  "#######..##",
  ".##..##....",
  ".....###...",
  "....####...",
  "....###....",
};
void dessinerVentilo(int cx, int cy, uint16_t c) {
  dessinerMotif(VENTILO, 11, 11, cx - 5, cy - 5, "#", &c);
}

void dessinerTuile(int i, const char* mot, uint16_t c) {
  ecrireCentre(mot, X_TUILE[i], 24, 1, c);
}

void dessinerPageEtat() {
  static const uint16_t GRIS = rgb565(110, 110, 125), ORANGE = rgb565(255, 120, 0);
  // --- en-tête : heure et date (heure NTP) ---
  char buf[12];
  if (heureValide()) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    ecrireTexte(buf, 1, 0, 1, COL_TITRE);
    snprintf(buf, sizeof(buf), "%02d/%02d", lt.tm_mday, lt.tm_mon + 1);
    if (estSurSecteur) ecrireDroite(buf, 62, 0, 1, COL_DISCRET);
  } else {
    ecrireTexte("--:--", 1, 0, 1, GRIS);
  }
  if (!estSurSecteur) ecrireDroite("BATT", 62, 0, 1, ORANGE);
  display.drawFastHLine(0, 8, 64, COL_FILET);

  // --- tuile 1 : Wi-Fi (arcs allumés = force du signal, mot = signal en dBm) ---
  uint16_t c = GRIS;
  const char* mot = "OFF";
#if WIFI_ACTIVE
  if (reseauOK()) {
    int rssi = WiFi.RSSI();
    rssi = constrain(rssi, -99, 0);           // 3 caractères au plus
    int nv = niveauSignal(rssi);
    c = couleurSignal(nv);
    dessinerLogoWifi(X_TUILE[0], 20, nv, c, COL_ETEINT);
    snprintf(buf, sizeof(buf), "%d", rssi);
    mot = buf;
  } else if (etatWifi == WIFI_SCAN || wifiEnConnexion()) {
    c = COL_TITRE;
    dessinerLogoWifi(X_TUILE[0], 20, 0, c, COL_ETEINT);
    mot = "...";
  } else {
    c = COL_ERREUR;
    dessinerLogoWifi(X_TUILE[0], 20, -1, COL_ETEINT, COL_ETEINT);
    dessinerCroix(X_TUILE[0] + 2, 16);
  }
#else
  dessinerLogoWifi(X_TUILE[0], 20, -1, COL_ETEINT, COL_ETEINT);
#endif
  dessinerTuile(0, mot, c);

  // --- tuile 2 : envoi au serveur (mot = mesures en attente d'envoi, s'il y en a) ---
  char att[8];
  if (!cleOK() || cleInvalide)  { c = COL_ERREUR; mot = "CLE"; }  // clé absente ou refusée (401)
  else if (etatEnvoi == 3)      { c = COL_ERREUR; mot = "ERR"; }  // mesure refusée / adresse du serveur
  else if (nbLignesAttente > 0) {                                  // mesures gardées en flash, renvoi en cours
    c = ORANGE;
    snprintf(att, sizeof(att), "%u", (unsigned)min((uint32_t)nbLignesAttente, (uint32_t)999));
    mot = att;
  }
  else if (etatEnvoi == 2)      { c = ORANGE; mot = "..."; }      // réessai
  else if (etatEnvoi == 1)      { c = colNiv[0]; mot = "OK"; }
  else                          { c = GRIS; mot = "--"; }         // rien envoyé depuis le démarrage
  dessinerNuage(X_TUILE[1], 10, c);
  dessinerTuile(1, mot, c);

  // --- tuile 3 : capteurs (NextPM et MH-Z19C) ---
  bool errCO2 = (mCO2.etat == CAPTEUR_ERREUR), errPM = (mPM25.etat == CAPTEUR_ERREUR);
  if (errCO2 && errPM)        { c = COL_ERREUR; mot = "ERR"; }
  else if (errCO2)            { c = COL_ERREUR; mot = co2HorsPlage ? "CAL" : "CO2"; }
  else if (errPM)             { c = COL_ERREUR; mot = "PM"; }
  else if (mCO2.etat == CAPTEUR_CHAUFFE || mPM25.etat == CAPTEUR_CHAUFFE) { c = COL_TITRE; mot = "..."; }
  else                        { c = colNiv[0]; mot = "OK"; }
  dessinerVentilo(X_TUILE[2], 15, c);
  dessinerTuile(2, mot, c);
}

// ---------- pages : sélection ----------
bool pageActive(uint8_t p) {
  if (p == PAGE_AERER) return alerteAerer();   // seulement quand le CO2 est élevé
  if (p == PAGE_COURBE) return histN >= 2;     // dès 2 minutes de mesures
  if (p == PAGE_WIFI) return WIFI_ACTIVE && !reseauOK();   // connecté : le Wi-Fi est sur la page ÉTAT
  if (p == PAGE_ETAT) return false;            // pas dans la rotation : intercalée par pageSuivante
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
  arreterFondu();
  if (!ecranEteint) {
    tFlashDebut = now;
    if (tFlashDebut == 0) tFlashDebut = 1;
    couleurFlash = colNiv[niv];
    nomFlash = nom;
  }
  aRedessiner = true;
  LOG("ALERTE %s : niveau %d (%s)\n", nom, niv, LABELS_NIVEAU[niv]);
}

// Appelée après chaque lecture des capteurs (chaque seconde ; le filtre médian écarte déjà un pic isolé).
// Une alerte se déclenche quand un polluant FRANCHIT son seuil, ou quand il EMPIRE pendant l'alerte.
// Anti-rebond : une valeur qui oscille autour du seuil ne refait pas clignoter l'écran plus d'une fois par minute.
// PM1 suit les seuils des PM2.5. Comme PM1 <= PM2.5, les deux passent souvent en alerte ensemble : un seul éclair
// (priorité à PM2.5 à niveau égal), mais la page PM1 entre aussi dans le tourniquet des pages en alerte.
void verifierAlertes(unsigned long now) {
  static const Mesure* const m[4] = {&mCO2, &mPM1, &mPM25, &mPM10};
  static const Echelle* const e[4] = {&ECH_CO2, &ECH_PM25, &ECH_PM25, &ECH_PM10};
  static const char* NOMS[4] = {"CO2", "PM1", "PM2.5", "PM10"};
  int choix = -1, nivChoix = -1;
  for (uint8_t i = 0; i < 4; i++) {
    EtatAlerte &a = alertes[i];
    int niv = niveauMesure(*m[i], *e[i]);
    int seuil = (i == 0) ? NIVEAU_ALERTE_CO2 : NIVEAU_ALERTE_PM;
    if (niv < seuil) { a.active = false; a.nivMax = -1; continue; }
    bool empire = a.active && niv > a.nivMax;
    bool entree = !a.active;
    a.active = true;
    if (niv > a.nivMax) a.nivMax = niv;
    bool rearme = (a.tFlash == 0) || (now - a.tFlash >= ALERTE_REARME_MS);
    // PM1 fait partie des PM2.5 : si PM2.5 est déjà en alerte à un niveau au moins égal, un éclair « PM1 » de plus
    // n'apprend rien (vu en simulation : PM1 franchissait le seuil quelques minutes après PM2.5 -> 2e éclair).
    // La page PM1 reste quand même dans le tourniquet des pages en alerte.
    if (i == 1 && alertes[2].active && alertes[2].nivMax >= niv) continue;
    if ((entree && rearme) || empire) {
      // plusieurs à la fois : le plus grave d'abord ; à égalité, PM2.5 passe devant PM1
      if (niv > nivChoix || (niv == nivChoix && choix == 1)) { choix = i; nivChoix = niv; }
    }
  }
  if (choix >= 0) declencherAlerte(choix, nivChoix, NOMS[choix], now);
}

// ---------- rendu ----------
// Dessine la page courante dans le tampon (sans l'envoyer à l'écran)
// ---------- écrans qui passent devant tout ----------
static const unsigned long CODE_BT_VISIBLE_MS = 60000UL;   // le code d'appairage reste affiché 1 min au plus

bool ecranSpecialActif() {
  if (codeAppairage != 0 && millis() - tCodeAppairage < CODE_BT_VISIBLE_MS) return true;
  return majProgression <= 100;
}

// Progression d'une mise à jour, ou code d'appairage Bluetooth (à taper sur le téléphone).
//   0..6 titre | 8 filet | pourcentage (y 11, taille 2) ou code : 6 chiffres espacés (y 12) | 24..30 consigne / barre
bool dessinerEcranSpecial() {
  // la mise à jour passe avant le code Bluetooth (elle ne dure que quelques secondes)
  if (majProgression <= 100) {
    ecrireTexte("MISE", 1, 0, 1, COL_TITRE);   // "MISE A JOUR" fait 65 px : espaces resserrés pour tenir en 64
    ecrireTexte("A", 28, 0, 1, COL_TITRE);
    ecrireTexte("JOUR", 37, 0, 1, COL_TITRE);
    display.drawFastHLine(0, 8, 64, COL_FILET);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)majProgression);
    ecrireCentre(pct, 32, 11, 2, COL_OR);
    display.fillRect(4, 27, 56, 3, COL_PISTE);
    display.fillRect(4, 27, 56 * majProgression / 100, 3, COL_TITRE);
    return true;
  }
  if (codeAppairage != 0 && millis() - tCodeAppairage < CODE_BT_VISIBLE_MS) {
    ecrireTexte("BLUETOOTH", 1, 0, 1, COL_TITRE);
    display.drawFastHLine(0, 8, 64, COL_FILET);
    char code[8];
    snprintf(code, sizeof(code), "%06lu", (unsigned long)codeAppairage);
    // 6 chiffres de 5 px, espacés de 3 px, en deux groupes de 3 (x 8..55)
    char ch[2] = {0, 0};
    for (int i = 0; i < 6; i++) {
      ch[0] = code[i];
      ecrireTexte(ch, 8 + i * 8 + (i >= 3 ? 3 : 0), 12, 1, COL_OR);
    }
    ecrireCentre("SUR LE TEL", 32, 24, 1, COL_DISCRET);   // à taper sur le téléphone
    return true;
  }
  return false;
}

void dessinerContenu() {
  display.clearDisplay();
  display.setTextWrap(false);

  if (dessinerEcranSpecial()) return;   // code Bluetooth ou mise à jour : devant tout le reste

  if (ecranWifiActif()) {   // écran de connexion Wi-Fi (démarrage, commande wifi)
    dessinerEcranConnexion();
    return;
  }

  // Éclair d'alerte : une image sur deux, tout l'écran à la couleur du niveau, "ALERTE" + polluant en noir
  if (flashEnCours() && (((millis() - tFlashDebut) / ALERTE_FLASH_MS) % 2UL) == 0) {
    display.fillScreen(couleurFlash);
    ecrireCentre("ALERTE", 32, 3, 1, 0);
    ecrireNombreCentre(nomFlash, 32, 14, 2, 0);
    return;
  }

  switch (pageCourante) {
    case PAGE_CO2:    dessinerPageMesure(mCO2,  "CO2",   ECH_CO2,  PAGE_CO2);  break;
    case PAGE_AERER:  dessinerPageAerer(); break;
    case PAGE_PM1:    dessinerPageMesure(mPM1,  "PM1",   ECH_PM25, PAGE_PM1);  break;   // mêmes seuils que PM2.5
    case PAGE_PM25:   dessinerPageMesure(mPM25, "PM2.5", ECH_PM25, PAGE_PM25); break;
    case PAGE_PM10:   dessinerPageMesure(mPM10, "PM10",  ECH_PM10, PAGE_PM10); break;
    case PAGE_GLOBAL: dessinerPageGlobal(); break;
    case PAGE_COURBE: dessinerPageCourbe(); break;
    case PAGE_WIFI:   dessinerPageWifi(); break;
    case PAGE_ETAT:   dessinerPageEtat(); break;
    default: break;
  }
}

// Envoie le tampon dessiné à l'écran (double tampon : bascule instantanée, sans scintillement)
// Après la bascule, le prochain dessin se fait dans l'ANCIEN tampon visible : si l'écran est justement en train de
// l'envoyer (refreshTask sur un autre cœur que l'appelant), on attend la fin de cet envoi, sinon une image à moitié
// effacée passerait à l'écran (éclair). Avec refreshTask sur le cœur de loop, l'attente ne se produit jamais.
void presenter() {
#if DOUBLE_BUFFER
  uint32_t n = nbRafraichissements;
  display.showBuffer();
  if (rafraichissementEnCours) {
    unsigned long t0 = millis();
    while (nbRafraichissements == n && millis() - t0 < 10) delay(1);
  }
#endif
}

// Transition entre deux pages : fondu au noir puis retour, en jouant sur la LUMINOSITÉ de tout l'écran
// (aucun trait ni balayage de couleur), SANS bloquer loop() (capteurs, console, Bluetooth continuent pendant le fondu).
// Luminosité en carré de l'avancement : l'œil voit une baisse régulière (une variation linéaire semble finir d'un coup).
void lumiFondu(unsigned long t, bool sortie) {
  uint32_t f = demiFondu ? (uint32_t)255 * t / demiFondu : 255;   // 0..255
  if (f > 255) f = 255;
  if (sortie) f = 255 - f;
  reglerLuminositeEcran((uint16_t)((uint32_t)luminositeCourante * f * f / 65025UL));
}

// Lance le fondu vers la page courante (déjà choisie) : l'ancienne image reste affichée et s'éteint ;
// la nouvelle sera dessinée dans le noir par majFondu().
void demarrerFondu() {
  bool court = fonduCourt || pageAvantFondu == PAGE_ETAT || pageCourante == PAGE_ETAT;
  fonduCourt = false;
  unsigned long demi = court ? FONDU_COURT_DEMI_MS : FONDU_DEMI_MS;
  if (demi == 0) { arreterFondu(); dessinerContenu(); presenter(); return; }   // TRANSITION_FONDU 0 : changement sec
  unsigned long now = millis();
  unsigned long dejaNoir = 0;   // fondu précédent pas fini (bouton) : on repart de la luminosité où il en était
  if (phaseFondu != 0 && demiFondu) {
    unsigned long t = now - tFondu;
    if (t < demiFondu) dejaNoir = (phaseFondu == 2) ? demi - demi * t / demiFondu : demi * t / demiFondu;
    else if (phaseFondu == 1) dejaNoir = demi;   // déjà au noir
  }
  demiFondu = demi;
  tFondu = now - dejaNoir;
  phaseFondu = 1;
  allongementPage = 2 * demi;
}

// Appelée à chaque tour de loop() : fait avancer le fondu en cours.
void majFondu(unsigned long now) {
  if (phaseFondu == 0) return;
  unsigned long t = now - tFondu;
  if (phaseFondu == 1) {
    if (t < demiFondu) { lumiFondu(t, true); return; }
    reglerLuminositeEcran(0);
    dessinerContenu();   // nouvelle page, dessinée dans le noir
    presenter();
    phaseFondu = 2;
    tFondu = now;
    t = 0;
  }
  if (t < demiFondu) { lumiFondu(t, false); return; }
  reglerLuminositeEcran(luminositeCourante);
  phaseFondu = 0;
}

void afficher() {
  // Écran éteint (mode nuit) : on affiche du noir (sauf code Bluetooth / mise à jour, qu'il faut pouvoir lire).
  if (ecranEteint && !ecranSpecialActif()) {
    aTransition = false; arreterFondu();
    display.clearDisplay(); presenter(); return;
  }
  // pas de fondu sur l'écran de connexion, ni sur le code Bluetooth / la mise à jour : page fixe
  if (ecranWifiActif() || ecranSpecialActif()) { aTransition = false; arreterFondu(); }
  if (aTransition) {
    aTransition = false;
    demarrerFondu();
    return;
  }
  if (phaseFondu == 1) return;   // l'ancienne page s'éteint encore : la nouvelle sera dessinée au noir
  dessinerContenu();
  presenter();
}

// Durée d'affichage de la page courante : la page ÉTAT est courte, les autres durent INTERVALLE_PAGE_MS
// (plus le fondu qui l'a amenée, pour qu'elle reste toute cette durée en pleine lumière).
unsigned long dureePage() {
  return ((pageCourante == PAGE_ETAT) ? DUREE_ETAT_MS : INTERVALLE_PAGE_MS) + allongementPage;
}

// Passe à la page suivante qui est active (saute AERER, COURBE et WIFI quand elles n'ont rien à montrer)
// et prépare le fondu.
// Toutes les PERIODE_ETAT_MS passées sur les autres pages, la page ÉTAT (icônes) s'intercale, puis la rotation
// reprend là où elle s'était arrêtée.
// Pendant une alerte : une page sur deux est une page EN ALERTE (tourniquet s'il y en a plusieurs) ; entre deux,
// la rotation normale reprend là où elle s'était arrêtée, pour que les autres mesures restent visibles.
unsigned long tFinEtat = 0;     // fin du dernier passage de la page ÉTAT
void pageSuivante() {
  unsigned long now = millis();
  aTransition = true;
  aRedessiner = true;
  pageAvantFondu = pageCourante;
  allongementPage = 0;
  if (pageCourante == PAGE_ETAT) {
    tFinEtat = now;
  } else if (AFFICHER_ETAT && now - tFinEtat >= PERIODE_ETAT_MS) {
    pageCourante = PAGE_ETAT;
    return;
  }
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
  if (nbReseaux == 0) return true;
  if (wifiEnConnexion() && reseauCourant >= 0) return texteDefile(reseaux[reseauCourant].ssid.c_str());
#endif
  return false;
}

// Une animation change d'image quand ph (= now / sa période) n'est plus celui retenu dans phase : vrai, et on
// retient le nouveau.
static bool nouvelleImage(uint32_t &phase, uint32_t ph) {
  if (ph == phase) return false;
  phase = ph;
  return true;
}

// Appelée à chaque tour de loop() : une image d'animation toutes les 40 ms au plus, et on ne
// redessine l'écran que si quelque chose a réellement bougé sur la page affichée.
void animer(unsigned long now) {
  if (ecranEteint) return;
  if (now - tAnim < ANIM_PAS_MS) return;
  tAnim = now;
  bool maj = false;
  bool pageVisible = !ecranWifiActif();
  static uint32_t phaseWifi = 0, phasePulse = 0, phaseAerer = 0, phaseEtat = 0;

  // écran de connexion / page Wi-Fi : logo animé, texte défilant, secondes (une image toutes les 60 ms)
  if (ecranWifiAnime() && nouvelleImage(phaseWifi, now / 60UL)) maj = true;

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
  if (pageVisible && pageCourante != PAGE_AERER && alertePage(pageCourante) &&
      nouvelleImage(phasePulse, now / ALERTE_PULSE_MS)) maj = true;

  // page AERER : rubans qui défilent (120 ms) et "AERER" qui pulse (0,6 s), même sans ANIMATIONS : c'est une alerte
  if (pageVisible && pageCourante == PAGE_AERER && nouvelleImage(phaseAerer, now / 120UL)) {
    phaseRuban = (uint16_t)(phaseRuban + 1);
    maj = true;
  }

  // page ÉTAT : l'heure (redessinée à chaque seconde, au cas où la minute change pendant les 3 s)
  if (pageVisible && pageCourante == PAGE_ETAT && nouvelleImage(phaseEtat, now / 1000UL)) maj = true;

#if ANIMATIONS
  // chiffres qui glissent en douceur vers la nouvelle valeur
  static Mesure* const tab[4] = {&mCO2, &mPM1, &mPM25, &mPM10};
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

  // clignement des yeux (pages de mesure et page ATMO : elles ont un visage)
  bool cl = yeuxFermes();
  if (cl != etatCligne) {
    etatCligne = cl;
    if (pageVisible && (pageCourante == PAGE_GLOBAL || mesureVisible(mCO2) || mesureVisible(mPM1) ||
                        mesureVisible(mPM25) || mesureVisible(mPM10))) maj = true;
  }

  // page courbe : le sommet de la barre en cours clignote (500 ms)
  if (pageCourante == PAGE_COURBE) {
    uint8_t p = (uint8_t)((now / 500UL) % 2UL);
    if (p != phasePoint) { phasePoint = p; maj = true; }
  }
#endif

  // préchauffage du CO2 : le compte à rebours change chaque seconde
  if (pageVisible && pageCourante == PAGE_CO2 && mCO2.etat == CAPTEUR_CHAUFFE &&
      nouvelleImage(phaseChauffe, now / 1000UL)) maj = true;

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
  static const uint16_t COULEURS[5] = {C_OR, C_PIERRE, C_VERT, C_COLLINE, C_COLLINE_SOMBRE};   // G S V H h
  display.clearDisplay();
  display.setTextWrap(false);

  // basilique, révélée du bas vers le haut
  dessinerMotif(LOGO_BASILIQUE + (32 - rangs), 15, rangs, 0, 32 - rangs, "GSVHh", COULEURS);
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

// Animation de démarrage (~7 s) : la basilique monte, le nom s'écrit, la barre se remplit, la statue scintille, puis
// le logo complet reste affiché 2,5 s pour qu'on ait le temps de le regarder (avant : ~2,7 s en tout, 0,5 s à la fin).
static const uint16_t SPLASH_MONTEE_MS = 50, SPLASH_LETTRE_MS = 90, SPLASH_LETTRE_FIN_MS = 150, SPLASH_NIVEAU_MS = 180,
                      SPLASH_SCINTILLE_MS = 200, SPLASH_FIN_MS = 2500;
void afficherSplash() {
  for (int r = 2; r <= 32; r += 2) { dessinerLogo(r, 0, 0, false); presenter(); delay(SPLASH_MONTEE_MS); }
  for (int l = 1; l <= 13; l++)    { dessinerLogo(32, l, 0, false); presenter(); delay(l <= 8 ? SPLASH_LETTRE_MS : SPLASH_LETTRE_FIN_MS); }
  for (int n = 1; n <= NB_NIVEAUX; n++) { dessinerLogo(32, 13, n, false); presenter(); delay(SPLASH_NIVEAU_MS); }
  for (int k = 0; k < 6; k++)      { dessinerLogo(32, 13, NB_NIVEAUX, k % 2 == 0); presenter(); delay(SPLASH_SCINTILLE_MS); }
  dessinerLogo(32, 13, NB_NIVEAUX, false); presenter();   // logo complet, au repos (sans éclat)
  delay(SPLASH_FIN_MS);
}

// Choisit la luminosité : niveau de jour ou de nuit (« lum »), bridée sur batterie ; peut aussi éteindre l'écran
// (ECRAN_ETEINT_NUIT). Appelée toutes les 10 s : la nuit commence au plus 10 s après l'heure, ou après la 1re heure
// reçue par Internet (avant, jusqu'à 1 min plus tard).
void majLuminosite() {
  bool nuit = estNuit();
  static int8_t nuitAvant = -1;   // pour annoncer le passage jour / nuit (une ligne au journal)
  if ((int8_t)nuit != nuitAvant) {
    if (nuitAvant >= 0 || nuit)
      Console.printf("Mode nuit : %s (luminosité %u/10)\n", nuit ? "activé" : "terminé", nuit ? niveauNuit : niveauJour);
    nuitAvant = nuit;
  }
  uint8_t lum = NIVEAUX_LUMI[(nuit ? niveauNuit : niveauJour) - 1];
  if (!estSurSecteur && lum > LUMINOSITE_BATTERIE) lum = LUMINOSITE_BATTERIE;  // économie sur batterie
  bool eteint = (ECRAN_ETEINT_NUIT && nuit);
  if (eteint != ecranEteint) { ecranEteint = eteint; aRedessiner = true; }
  luminositeCourante = lum;
  if (phaseFondu == 0) reglerLuminositeEcran(lum);   // pendant un fondu, majFondu() y revient tout seul
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
    Console.println(estSurSecteur ? "RETOUR DU COURANT" : "COUPURE DE COURANT : passage sur batterie");
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
      Console.println("Bouton (appui long) : mesure immédiate.");
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
  if (mCO2.etat != CAPTEUR_OK || mCO2.valeur <= SEUIL_ALARME_PPM) return;
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
// ---------- Écran : luminosité et mode nuit (« lum », « nuit ») ----------
// Câble USB et Bluetooth (téléphone appairé, donc à côté de la carte) ; jamais par la console du relais (Wi-Fi).
static bool lireEntier(const String &s, int mini, int maxi, int &v) {
  if (s.length() == 0 || s.length() > 2) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isDigit(s[i])) return false;
  v = s.toInt();
  return v >= mini && v <= maxi;
}

void afficherLuminosite() {
  Console.printf("luminosité : jour %u/10 | nuit %u/10 | en ce moment : %s, %u/255\n", niveauJour, niveauNuit,
                 !heureValide() ? "jour (heure pas encore reçue)" : estNuit() ? "nuit" : "jour",
                 (unsigned)luminositeCourante);
  if (heureNuitDebut == heureNuitFin) Console.println("mode nuit : jamais ('nuit 22 7' pour le remettre)");
  else Console.printf("mode nuit : de %u h à %u h%s\n", heureNuitDebut, heureNuitFin, ECRAN_ETEINT_NUIT ? ", écran éteint" : "");
}

// Commande en minuscules, sans espace au début ni à la fin, espaces répétés réduits à un seul.
static void normaliserCommande(String &bas) {
  bas.toLowerCase();
  bas.trim();
  int i;
  while ((i = bas.indexOf("  ")) >= 0) bas.remove(i, 1);
}

// Reconnaît une commande d'écran ; l'exécute si executer. Faux = pas une commande d'écran valide (sert aussi à la
// liste blanche du Bluetooth : seules les formes exactes ci-dessous passent).
//   lum                      état
//   lum + | lum -            un niveau de plus / de moins pour la période en cours (jour ou nuit)
//   lum jour|nuit +|-|1..10  règle l'autre période, ou un niveau précis
//   lum defaut               jour 10, nuit 2
//   nuit | nuit <début> <fin> | nuit off   plage de nuit (heures 0..23)
bool commandeEcran(const String &commande, bool executer) {
  String bas = commande;
  normaliserCommande(bas);
  int sp = bas.indexOf(' ');
  String mot = (sp < 0) ? bas : bas.substring(0, sp);
  String reste = (sp < 0) ? String("") : bas.substring(sp + 1);
  bool lum = (mot == "lum");
  if (!lum && mot != "nuit") return false;
  if (reste.length() == 0) { if (executer) afficherLuminosite(); return true; }   // « lum » ou « nuit » seul : état

  if (lum) {
    if (reste == "defaut" || reste == "défaut") {
      if (!executer) return true;
      niveauJour = NIVEAU_JOUR_DEFAUT; niveauNuit = NIVEAU_NUIT_DEFAUT;
      prefs.putUChar("lum_j", niveauJour); prefs.putUChar("lum_n", niveauNuit);
      majLuminosite();
      afficherLuminosite();
      return true;
    }
    int cible = -1;                                   // -1 : période en cours | 0 : jour | 1 : nuit
    if (reste.startsWith("jour ")) { cible = 0; reste = reste.substring(5); }
    else if (reste.startsWith("nuit ")) { cible = 1; reste = reste.substring(5); }
    int pas = 0, n = 0;
    if (reste == "+") pas = 1;
    else if (reste == "-") pas = -1;
    else if (!lireEntier(reste, 1, 10, n)) return false;
    if (!executer) return true;
    if (cible < 0) cible = estNuit() ? 1 : 0;
    uint8_t &niv = cible ? niveauNuit : niveauJour;
    int nouveau = pas ? constrain((int)niv + pas, 1, 10) : n;
    const char* periode = cible ? "de nuit" : "de jour";
    if (pas && nouveau == niv) {
      Console.printf("Luminosité %s : déjà au %s (%u/10).\n", periode, pas > 0 ? "maximum" : "minimum", niv);
      return true;
    }
    if (nouveau != niv) { niv = (uint8_t)nouveau; prefs.putUChar(cible ? "lum_n" : "lum_j", niv); }
    majLuminosite();
    bool visible = (cible == 1) == estNuit();
    Console.printf("Luminosité %s : %u/10%s\n", periode, niv, visible ? "" : " (appliquée à la prochaine période)");
    return true;
  }

  // nuit
  int a = 0, b = 0;
  if (reste != "off") {
    int s2 = reste.indexOf(' ');
    if (s2 < 0 || !lireEntier(reste.substring(0, s2), 0, 23, a) || !lireEntier(reste.substring(s2 + 1), 0, 23, b))
      return false;
  }
  if (!executer) return true;
  heureNuitDebut = (uint8_t)a; heureNuitFin = (uint8_t)b;
  prefs.putUChar("nuit_d", heureNuitDebut); prefs.putUChar("nuit_f", heureNuitFin);
  majLuminosite();
  afficherLuminosite();
  return true;
}

// Affiche la liste des commandes disponibles dans le moniteur série.
void afficherAide() {
  static const char* const AIDE[] = {
    "Commandes disponibles :",
    "  status                état de la carte (réseau, file d'attente, clé...)",
    "  wifi list             réseaux Wi-Fi en mémoire (5 au maximum)",
    "  wifi add <nom> [mdp]  ajoute / modifie un Wi-Fi classique (sans mdp = réseau ouvert)",
    "  wifi ent <nom> <identifiant> <mdp>   ajoute un WPA2 Entreprise (ex. eduroam)",
    "  wifi ent test         eduroam refusé : le certificat est-il en cause ? (un essai sans le vérifier)",
    "                        nom avec espaces : entre guillemets, ex. wifi add \"Box de Paul\" mdp",
    "  wifi del <n|nom>       supprime UN réseau : son numéro ('wifi list') ou son nom",
    "  wifi scan             liste les réseaux visibles (* = connu)",
    "  wifi connect          relance la connexion tout de suite",
    "  set cle <clé>         clé du groupe AirCarto (32 caractères)",
    "  set relais <url>      adresse du relais Cloudflare (https://...workers.dev)",
    "  set jeton <jeton>     jeton secret de la carte auprès du relais (16 caractères au moins)",
    "  maj                   cherche une mise à jour publiée (maj installer : l'installe)",
    "  maj oublier           réautorise une version qui a échoué sur la carte",
    "  diag tls              teste les connexions HTTPS (AirCarto, relais) et affiche la raison d'un échec",
    "  bt appairer           autorise un nouveau téléphone Bluetooth pendant 2 min",
    "  bt demarrage on|off   autorise aussi un nouveau téléphone 2 min après la mise sous tension",
    "  bt liste              téléphones Bluetooth appairés",
    "  bt oublier [n]        oublie le téléphone n ('bt liste'), ou tous sans numéro",
    "  lum                   luminosité de l'écran : lum + / lum - (période en cours, jour ou nuit),",
    "                        lum jour|nuit <1..10> ou +/-, lum defaut",
    "  nuit <début> <fin>    heures du mode nuit, ex. nuit 22 7 (nuit off : jamais)",
    "  reboot                redémarre la carte",
    "  verbose on|off        active/coupe l'affichage automatique des mesures",
    "  calib oui             calibrage du zéro CO2 (seulement après 20 min à l'air libre)",
    "  vider_file            supprime les mesures stockées en attente",
    "  oublier               efface identifiants et clé de la carte",
    "Saisie : Retour arrière = efface 1 caractère | Ctrl+U = efface toute la ligne",
    "À distance (console du relais, Bluetooth) : aide, status, verbose, calib, reboot, wifi scan,",
    "wifi connect, maj, diag tls. Bluetooth aussi (pas le relais) : lum, nuit.",
    "Wi-Fi, clé, jeton, relais, calib oui, bt et effacements : câble USB seulement.",
  };
  for (const char* ligne : AIDE) Console.println(ligne);
}

// Résumé de l'état de la carte (réseau, clé, file d'attente...) à la demande, commande « status ».
void afficherStatus() {
  Console.println("--------- STATUS ---------");
  Console.printf("device_id : %s\n", deviceId.c_str());
  Console.printf("redémarrage : cause %d (%s) | boot %u\n", (int)esp_reset_reason(),
                 texteCauseRedemarrage(esp_reset_reason()), (unsigned)bootId);
  Console.printf("uptime / mémoire : %lu s | %u octets libres (plus grand bloc : %u)\n", millis() / 1000UL,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  Console.printf("réserve HTTPS : %u tampon(s) de %u octets, %u libre(s) | grands tampons hors réserve : %lu\n",
                 (unsigned)nbTamponsTls, (unsigned)TAMPON_TLS_OCTETS, (unsigned)tamponsTlsLibres(),
                 (unsigned long)nbGrandsHorsReserve);
  Console.printf("pile libre au plus bas : loop %u | envoi %u | relais %u octets\n",
                 hTacheLoop ? (unsigned)uxTaskGetStackHighWaterMark(hTacheLoop) : 0u,
                 hTacheEnvoi ? (unsigned)uxTaskGetStackHighWaterMark(hTacheEnvoi) : 0u,
                 hTacheRelais ? (unsigned)uxTaskGetStackHighWaterMark(hTacheRelais) : 0u);
  if (reseauOK()) {
    Console.printf("WiFi : connecté à %s (%s) | IP %s | %d dBm\n", nomAffichable(WiFi.SSID()).c_str(), typeReseau(reseauCourant),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!WIFI_ACTIVE) {
    Console.println("WiFi : désactivé (WIFI_ACTIVE 0)");
  } else {
    static const char* ETATS[] = {"aucun réseau en mémoire", "recherche", "connexion en cours", "connecté", "en pause"};
    Console.printf("WiFi : hors ligne (%s) | dernière raison : %s\n", ETATS[etatWifi], raisonLongue(raisonDeco));
  }
  Console.printf("réseaux en mémoire : %u / %u | coupures WiFi : %u\n", (unsigned)nbReseaux, (unsigned)NB_WIFI_MAX,
                (unsigned)nbCoupuresWifi);
#if WIFI_ACTIVE
  bool aEntreprise = false;
  for (uint8_t i = 0; i < nbReseaux; i++) if (estEntreprise(i)) aEntreprise = true;
  if (aEntreprise)
    Console.println(caEduroamValide == 1 ? "certificat du serveur ENT : vérifié (ca_eduroam.h)"
                    : caEduroamValide == 0 ? "certificat du serveur ENT : ca_eduroam.h ILLISIBLE (connexion impossible)"
                    : "certificat du serveur ENT : NON vérifié, mot de passe ENT exposé (voir src/ca_eduroam.h.exemple)");
#endif
  Console.printf("heure NTP : %s | mode nuit : %s\n", heureValide() ? "valide" : "pas encore", estNuit() ? "oui" : "non");
  afficherLuminosite();
  Console.printf("clé du groupe : %s%s\n", cleOK() ? "présente (32 car.)" : "ABSENTE ou mauvaise longueur",
                cleInvalide ? " | REFUSÉE par le serveur (nouvel essai toutes les 15 min)" : "");
  Console.printf("dernier envoi (%s) : code HTTP %d | état %u | écart %lu ms (dont marge %lu ms)",
                AIRCARTO_HTTPS ? "HTTPS" : "HTTP, clé lisible sur le réseau", dernierCodeHttp,
                (unsigned)etatEnvoi, ecartCourantMs(), margePushMs);
  if (nbReportsMemoire > 0) Console.printf(" | reportés (mémoire ou autre connexion HTTPS) : %u", (unsigned)nbReportsMemoire);
  Console.println();
  Console.printf("file d'attente : %u mesures (%u octets de fichier)", (unsigned)nbLignesAttente, (unsigned)tailleBacklog);
  if (nbLignesAttente > 0 && reseauOK()) {
    // créneaux libres pour le retard = 1 envoi / écart, moins les mesures fraîches (1 / période)
    float parS = 1000.0f / ecartCourantMs() - 1000.0f / periodeMesureMs();
    if (parS > 0) Console.printf(" | rattrapé dans ~%u min", (unsigned)(nbLignesAttente / parS / 60.0f + 0.99f));
  }
  Console.println();
  if (nbSansHeure > 0) Console.printf("mesures sans heure abandonnées (coupure de courant hors ligne) : %u\n", (unsigned)nbSansHeure);
  Console.printf("CO2 : %s%s | lectures impossibles écartées : %u%s\n",
                mCO2.etat == CAPTEUR_OK ? "ok" : (mCO2.etat == CAPTEUR_CHAUFFE ? "préchauffage" : "en erreur"),
                (mCO2.etat == CAPTEUR_ERREUR && codeMhz != RESULT_OK) ? (String(" (code ") + codeMhz + " : " + texteCodeMhz(codeMhz) + ")").c_str() : "",
                (unsigned)nbCO2HorsPlage, co2HorsPlage ? " (dernière lecture hors plage : 'calib' à l'air libre ?)" : "");
  if (nbCO2NonConfirmes > 0) Console.printf("CO2 > %d ppm non confirmés (ignorés) : %u\n", CO2_CONFIRMATION_PPM, (unsigned)nbCO2NonConfirmes);
  Console.printf("lecture des capteurs : toutes les %lu ms | période de mesure : %lu s%s\n", INTERVALLE_LECTURE_MS,
                periodeMesureMs() / 1000UL,
                !estSurSecteur ? " (batterie)" : !envoiPossible() ? " (hors ligne)" : nbLignesAttente > 0 ? " (rattrapage du retard)" : " (en direct)");
  Console.printf("alimentation : %s\n", estSurSecteur ? "secteur" : "batterie");
  Console.printf("affichage détaillé : %s\n", verbose ? "oui" : "non");
  Console.printf("version du programme : %lu", (unsigned long)FW_VERSION);
#if CONSOLE_DISTANTE
  const esp_partition_t* part = esp_ota_get_running_partition();
  Console.printf(" (emplacement %s)%s\n", part ? part->label : "?", otaEnEssai ? " | EN ESSAI : se valide après 1 min + un échange avec le relais" : "");
  Console.printf("mise à jour à distance : %s\n", OTA_POSSIBLE ? "possible (clé publique présente)" : "impossible (pas de cle_ota.h)");
  if (!relaisConfigure) {
    Console.printf("console à distance : inactive (%s)\n", cfgRelais.length() == 0 ? "set relais <url>" : "set jeton <jeton>");
  } else {
    Console.printf("console à distance : %s | jeton présent | ", cfgRelais.c_str());
    if (tDernierSyncOK == 0) Console.printf("jamais joint (dernier code %d)", dernierCodeRelais);
    else Console.printf("dernier échange il y a %lu s%s", (millis() - tDernierSyncOK) / 1000UL, consoleLive ? " | console ouverte" : "");
    Console.printf(" | %u ok / %u échecs\n", (unsigned)nbSyncOK, (unsigned)nbSyncEchecs);
    static const char* const ETATS[] = {"démarrage", "attend le Wi-Fi", "attend l'échange suivant", "attend le verrou HTTPS",
                                        "attend de la mémoire", "échange en cours", "mise à jour en cours"};
    unsigned long depuis = (millis() - tEtatRelais) / 1000UL;
    Console.printf("tâche du relais : %s depuis %lu s", etatRelais < 7 ? ETATS[etatRelais] : "?", depuis);
    if (etatRelais == RELAIS_ATTENTE) Console.printf(" (échanges toutes les %lu s)", attenteRelaisMs / 1000UL);
    Console.println();
  }
#else
  Console.println();
#endif
#if BLUETOOTH_ACTIF
  Console.printf("Bluetooth : %s | %s | %d téléphone(s) appairé(s) ('bt liste')\n", btNom.c_str(),
                 btAutorise ? "téléphone appairé connecté" : btConnecte ? "connexion en cours" : "en attente",
                 NimBLEDevice::getNumBonds());
  Console.printf("Bluetooth : nouveau téléphone %s | à la mise sous tension : %s | Secure Connections seul : %s\n",
                 appairageOuvert() ? "ACCEPTÉ (fenêtre ouverte)" : "refusé ('bt appairer' au câble USB)",
                 btAppairageDemarrage ? "oui" : "non", ble_hs_cfg.sm_sc_only ? "oui" : "NON");
  {
    int nAbo = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_CCCD, &nAbo);
    nvs_stats_t nvs = {};
    nvs_get_stats(NULL, &nvs);
    Console.printf("Bluetooth : diffusion %s | abonnements mémorisés %d/%d | mémoire NVS libre %u/%u entrées\n",
                   NimBLEDevice::getAdvertising()->isAdvertising() ? "active" : btConnecte ? "arrêtée (connecté)" : "ARRÊTÉE",
                   nAbo, (int)CONFIG_BT_NIMBLE_MAX_CCCDS, (unsigned)nvs.free_entries, (unsigned)nvs.total_entries);
  }
#endif
}

#if CONSOLE_DISTANTE
// Adresse et jeton du relais, changés ensemble sous mutexEnvoi (la tâche du relais les lit) ; passer cfgRelais ou
// cfgJeton pour garder la valeur actuelle.
static void changerRelais(const String &relais, const String &jeton) {
  if (mutexEnvoi) xSemaphoreTake(mutexEnvoi, portMAX_DELAY);
  cfgRelais = relais;
  cfgJeton = jeton;
  majRelaisConfigure();
  if (mutexEnvoi) xSemaphoreGive(mutexEnvoi);
}
#endif

void definirParametre(const String &nom, const String &valeur) {
  if (nom == "ssid" || nom == "wifi_pwd" || nom == "eap_id" || nom == "eap_pwd") {
    Console.println("Commande remplacée par le cache Wi-Fi :");
    Console.println("  wifi ent eduroam <identifiant> <mdp>   ou   wifi add <nom> <mdp>   (tape 'aide')");
    return;
  } else if (nom == "cle") {
    definirCle(valeur); prefs.putString("cle", valeur);
    cleInvalide = false;  // nouvelle clé : les envois reprennent
    if (!cleOK()) Console.printf("Attention : la clé fait %u caractères (32 attendus).\n", (unsigned)valeur.length());
#if CONSOLE_DISTANTE
  } else if (nom == "relais") {
    String url = valeur;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    if (!url.startsWith("https://")) { Console.println("L'adresse du relais commence par https://"); return; }
    changerRelais(url, cfgJeton);
    prefs.putString("relais", url);
    Console.printf("Relais enregistré : %s\n", url.c_str());   // une adresse n'est pas un secret
    syncDemandee = true;
    return;
  } else if (nom == "jeton") {
    if (valeur.length() < 16) { Console.println("Jeton trop court : 16 caractères au moins (32 conseillés)."); return; }
    changerRelais(cfgRelais, valeur);
    prefs.putString("jeton", valeur);
    syncDemandee = true;
#endif
  } else {
    Console.println("Paramètre inconnu. Tape 'aide'.");
    return;
  }
  // on n'affiche jamais la valeur : ce sont des secrets
  Console.printf("%s enregistré (%u caractères).\n", nom.c_str(), (unsigned)valeur.length());
}

// Mot suivant de s à partir de pos ; "entre guillemets" pour un nom qui contient des espaces.
String motSuivant(const String &s, int &pos) {
  int len = s.length();
  while (pos < len && s[pos] == ' ') pos++;
  if (pos >= len) return "";
  bool guillemets = (s[pos] == '"');
  int debut = guillemets ? pos + 1 : pos;
  int fin = s.indexOf(guillemets ? '"' : ' ', debut);   // guillemet fermant, ou espace après le mot
  if (fin < 0) fin = len;
  pos = (guillemets && fin < len) ? fin + 1 : fin;      // on saute le guillemet fermant
  return s.substring(debut, fin);
}

// Le reste de la ligne (mot de passe : peut contenir des espaces), guillemets autour retirés
String resteLigne(const String &s, int pos) {
  String r = s.substring(pos);
  r.trim();
  if (r.length() >= 2 && r[0] == '"' && r[r.length() - 1] == '"') r = r.substring(1, r.length() - 1);
  return r;
}

void listerReseaux() {
  if (nbReseaux == 0) { Console.println("Aucun réseau en mémoire. Tape 'aide'."); return; }
  Console.printf("Réseaux en mémoire (%u / %u) :\n", (unsigned)nbReseaux, (unsigned)NB_WIFI_MAX);
  for (uint8_t i = 0; i < nbReseaux; i++) {
    bool actif = reseauOK() && reseauCourant == (int8_t)i;
    Console.printf("  %u) %-24s %-6s", (unsigned)(i + 1), nomAffichable(reseaux[i].ssid).c_str(), typeReseau(i));
    if (estEntreprise(i)) Console.printf(" id %s", nomAffichable(reseaux[i].eapId).c_str());
    if (actif) Console.print("  <- connecté");
    else if ((int8_t)i == dernierReseau) Console.print("  (dernier utilisé)");
    Console.println();
  }
}

void scannerReseaux() {
#if WIFI_ACTIVE
  if (etatWifi == WIFI_SCAN || wifiEnConnexion()) {
    Console.println("Connexion en cours : réessaie dans quelques secondes.");
    return;
  }
  Console.println("Recherche des réseaux visibles...");
  int n = WiFi.scanNetworks(false, false);
  if (n <= 0) { Console.println("Aucun réseau visible."); WiFi.scanDelete(); return; }
  // Le repère « * » (réseau enregistré dans la carte) n'est montré qu'au câble USB : à distance et en Bluetooth,
  // la liste des réseaux visibles ne révèle pas ceux que la carte connaît (commande sensible = sortie USB seule).
  bool montrerConnus = (sortieUsbSeule > 0);
  for (int j = 0; j < n; j++) {
    String ssid = WiFi.SSID(j);
    bool connu = false;
    if (montrerConnus) for (uint8_t i = 0; i < nbReseaux; i++) if (reseaux[i].ssid == ssid) connu = true;
    wifi_auth_mode_t securite = WiFi.encryptionType(j);
    bool ent = (securite == WIFI_AUTH_WPA2_ENTERPRISE), ouvert = (securite == WIFI_AUTH_OPEN);
    Console.printf(" %c %-28s %4d dBm  %s\n", connu ? '*' : ' ', nomAffichable(ssid).c_str(), WiFi.RSSI(j),
                  ent ? "ENTREPRISE (wifi ent)" : (ouvert ? "OUVERT" : "WPA (wifi add)"));
  }
  WiFi.scanDelete();
#else
  Console.println("WiFi désactivé (WIFI_ACTIVE 0).");
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
  } else if (sous == "ent" && (resteLigne(s, pos).equalsIgnoreCase("test") || resteLigne(s, pos).equalsIgnoreCase("test oui"))) {
    // Diagnostic : eduroam refusé à cause du certificat, ou de l'identifiant / mot de passe ?
#if CA_EDUROAM_PRESENT
    bool oui = resteLigne(s, pos).equalsIgnoreCase("test oui");
    bool entreprise = false;
    for (uint8_t i = 0; i < nbReseaux; i++) if (estEntreprise(i)) entreprise = true;
    if (!entreprise) { Console.println("Aucun réseau entreprise enregistré ('wifi ent <nom> <identifiant> <mdp>')."); return; }
    if (!oui) {
      Console.println("Test : UN essai de connexion à eduroam SANS vérifier le certificat du serveur. Il dit si c'est le "
                      "certificat (ca_eduroam.h) qui bloque. Ton mot de passe part alors vers un serveur non vérifié : "
                      "fais-le seulement à la fac, sur le vrai eduroam. Pour lancer : 'wifi ent test oui'.");
      return;
    }
    essaiSansCert = true;
    Console.println("Test sans certificat : connexion à eduroam...");
    relancerWifi(true);
#else
    Console.println("Pas de ca_eduroam.h : la carte ne vérifie déjà aucun certificat, le test n'apporterait rien.");
#endif
  } else if (sous == "add" || sous == "ent") {
    bool ent = (sous == "ent");
    String ssid = motSuivant(s, pos);
    String id = ent ? motSuivant(s, pos) : String("");
    String mdp = resteLigne(s, pos);
    if (ssid.length() == 0 || ssid.length() > 32) { Console.println("Nom de réseau absent ou trop long (32 max)."); return; }
    if (ent && (id.length() == 0 || mdp.length() == 0)) { Console.println("Usage : wifi ent <nom> <identifiant> <mdp>"); return; }
    if (ent && caEduroamValide != 1)
      Console.println("ATTENTION : sans certificat vérifié (src/ca_eduroam.h), un faux point d'accès peut capturer ce mot de passe. "
                      "Préfère un réseau dédié aux objets, ou ajoute le certificat.");
    if (!ent && mdp.length() > 0 && (mdp.length() < 8 || mdp.length() > 63)) {
      Console.println("Mot de passe Wi-Fi : 8 à 63 caractères."); return;
    }
    int i = ajouterReseau(ssid, mdp, id);
    if (i < 0) { Console.println("Mémoire pleine (5 réseaux) : supprime-en un avec 'wifi del <n>'."); return; }
    Console.printf("Réseau %u enregistré : %s (%s). Connexion...\n", (unsigned)(i + 1), nomAffichable(ssid).c_str(), typeReseau(i));
    relancerWifi(true);
  } else if (sous == "del") {
    // « wifi del 2 » (numéro de 'wifi list') ou « wifi del MaBox » / « wifi del "Box de Paul" » (nom exact)
    String quoi = motSuivant(s, pos);
    int i = -1;
    bool chiffres = quoi.length() > 0;
    for (unsigned k = 0; k < quoi.length(); k++) if (!isDigit(quoi[k])) chiffres = false;
    int num = chiffres ? quoi.toInt() : 0;
    if (num >= 1 && num <= nbReseaux) i = num - 1;
    for (uint8_t k = 0; i < 0 && k < nbReseaux; k++) if (reseaux[k].ssid == quoi) i = k;
    if (i < 0) { Console.println("Réseau introuvable : donne son numéro ou son nom exact (voir 'wifi list')."); return; }
    Console.printf("Réseau %s supprimé.\n", nomAffichable(reseaux[i].ssid).c_str());
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
    Console.println("Relance de la connexion Wi-Fi...");
    relancerWifi(sortieUsbSeule > 0);   // à distance : pas d'écran de connexion ni de noms des réseaux essayés
  } else {
    Console.println("Commande wifi inconnue. Tape 'aide'.");
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
    Console.println("Redémarrage...");
    delay(200);
    ESP.restart();
  } else if (commandeEcran(bas, true)) {
    // lum ..., nuit ... (exécutée)
  } else if (bas == "lum" || bas.startsWith("lum ") || bas == "nuit" || bas.startsWith("nuit ")) {
    Console.println("Écran : lum | lum + | lum - | lum jour|nuit <1..10> | lum defaut | nuit <début> <fin> | nuit off");
  } else if (bas == "verbose on" || bas == "verbose off") {
    verbose = (bas == "verbose on");
    prefs.putBool("verbose", verbose);
    Console.printf("Affichage détaillé : %s\n", verbose ? "activé" : "désactivé");
  } else if (bas == "wifi" || bas.startsWith("wifi ")) {
    commandeWifi(cmd);
  } else if (bas.startsWith("set ")) {
    // Commande « set <nom> <valeur> » : on sépare le nom de la valeur au premier espace.
    String reste = cmd.substring(4);
    reste.trim();
    int sp = reste.indexOf(' ');
    if (sp < 0) { Console.println("Usage : set <nom> <valeur> (tape 'aide')"); return; }
    String nom = reste.substring(0, sp);
    nom.toLowerCase();
    String valeur = reste.substring(sp + 1);
    valeur.trim();
    definirParametre(nom, valeur);
  } else if (bas == "calib oui") {
#if MHZ_ACTIVE
    // Ordre au capteur de se caler : l'air actuel devient la référence 400 ppm (le zéro).
    mhz.calibrate();
    Console.println("Calibrage du zéro CO2 envoyé au MH-Z19C (à faire après 20 min à l'air libre).");
#else
    Console.println("MH-Z19C désactivé (MHZ_ACTIVE 0).");
#endif
  } else if (bas == "calib") {
    Console.println("Calibrage : l'air doit être extérieur (~400 ppm) depuis 20 min. Tape 'calib oui' au câble USB pour confirmer.");
  } else if (bas == "maj" || bas == "maj installer") {
#if CONSOLE_DISTANTE
    if (!relaisConfigure) { Console.println("Mise à jour : relais non configuré (set relais / set jeton, câble USB)."); return; }
    if (!reseauOK()) { Console.println("Mise à jour : pas de Wi-Fi."); return; }
    demandeMaj = (bas == "maj") ? 1 : 2;
    Console.println(demandeMaj == 2 ? "Recherche et installation de la mise à jour..." : "Recherche d'une mise à jour...");
#else
    Console.println("Mise à jour à distance désactivée (CONSOLE_DISTANTE 0).");
#endif
  } else if (bas == "diag tls") {   // aussi à distance (lecture seule, aucun secret) : une fois par 5 min
    diagnosticTls();
  } else if (bas == "maj oublier") {   // câble USB seulement (pas dans la liste blanche)
#if CONSOLE_DISTANTE
    prefsOta.remove("refusee");
    Console.println("Mise à jour : plus aucune version refusée, 'maj installer' peut réinstaller la dernière publiée.");
#endif
  } else if (bas == "bt appairer") {
#if BLUETOOTH_ACTIF
    ouvrirAppairage();
#else
    Console.println("Bluetooth désactivé (BLUETOOTH_ACTIF 0).");
#endif
  } else if (bas == "bt demarrage on" || bas == "bt demarrage off") {
    btAppairageDemarrage = (bas == "bt demarrage on");
    prefs.putBool("bt_dem", btAppairageDemarrage);
    Console.println(btAppairageDemarrage
      ? "Bluetooth : un nouveau téléphone pourra s'appairer pendant 2 min après chaque mise sous tension."
      : "Bluetooth : nouveau téléphone seulement après 'bt appairer' (câble USB).");
  } else if (bas == "bt liste") {
#if BLUETOOTH_ACTIF
    listerTelephones();
#else
    Console.println("Bluetooth désactivé (BLUETOOTH_ACTIF 0).");
#endif
  } else if (bas.startsWith("bt oublier ")) {
#if BLUETOOTH_ACTIF
    oublierTelephone(bas.substring(11).toInt());
#else
    Console.println("Bluetooth désactivé (BLUETOOTH_ACTIF 0).");
#endif
  } else if (bas == "bt oublier") {
#if BLUETOOTH_ACTIF
    oublierTelephones();
#else
    Console.println("Bluetooth désactivé (BLUETOOTH_ACTIF 0).");
#endif
  } else if (bas == "vider_file") {
    demandeVider = true;  // c'est la tâche d'envoi qui touche au fichier
    Console.println("Vidage de la file d'attente demandé.");
  } else if (bas == "oublier") {
    prefs.remove("ssid"); prefs.remove("wifi_pwd"); prefs.remove("eap_id");
    prefs.remove("eap_pwd"); prefs.remove("cle");
    for (uint8_t i = 0; i < NB_WIFI_MAX; i++) reseaux[i] = ReseauWifi();
    nbReseaux = 0;
    dernierReseau = -1;
    sauverReseaux();
    definirCle("");
    cleInvalide = false;
#if CONSOLE_DISTANTE
    prefs.remove("relais"); prefs.remove("jeton");
    changerRelais("", "");
#endif
#if BLUETOOTH_ACTIF
    oublierTelephones();
#endif
    relancerWifi(false);
    Console.println("Réseaux Wi-Fi, identifiants, clé, jeton du relais et téléphones effacés de la carte.");
  } else {
    Console.println("Commande inconnue. Tape 'aide'.");
  }
}

// Commande « diag tls » (câble USB) : rejoue des connexions HTTPS en affichant ce que la bibliothèque
// WiFiClientSecure ne montre pas (elle efface tout dès l'échec). AUCUNE donnée n'est envoyée (ni clé, ni mesure,
// ni jeton) : poignée de main seulement, puis fermeture. Mémoire comptée en « 8 bits » : ESP.getFreeHeap() compte
// aussi la mémoire IRAM, inutilisable par mbedTLS.
static void memoireTls(const char* quand) {
  Console.printf("    mémoire %s : utilisable %u (plus grand bloc %u) | réserve HTTPS libre %u/%u | pile restante %u\n",
                 quand, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)tamponsTlsLibres(), (unsigned)nbTamponsTls, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// bundle == nullptr : SANS vérification (diagnostic seulement, la connexion est fermée aussitôt).
static void essaiTls(const char* hote, const uint8_t* bundle, const char* nom) {
  Console.printf("--- %s\n", nom);
  memoireTls("avant");
  IPAddress ip;
  if (!WiFi.hostByName(hote, ip)) { Console.println("    nom introuvable (DNS)"); return; }
  if (bundle) bundleCourant = bundle;
  sslclient_context* ctx = new sslclient_context;
  ssl_init(ctx);
  ctx->socket = -1;
  ctx->handshake_timeout = 15000;
  int ret = start_ssl_client(ctx, ip, 443, hote, 8000, nullptr, bundle != nullptr, nullptr, nullptr, nullptr, nullptr,
                             bundle == nullptr, nullptr);
  char texte[160];
  if (ret >= 0) {
    Console.printf("    connexion OK (%s, %s)\n", mbedtls_ssl_get_version(&ctx->ssl_ctx), mbedtls_ssl_get_ciphersuite(&ctx->ssl_ctx));
    memoireTls("pendant la connexion");
  } else {
    mbedtls_strerror(ret, texte, sizeof(texte));
    Console.printf("    ÉCHEC %d (-0x%04X) : %s\n", ret, (unsigned)-ret, texte);
  }
  uint32_t drapeaux = mbedtls_ssl_get_verify_result(&ctx->ssl_ctx);
  if (bundle != nullptr && drapeaux != 0 && drapeaux != (uint32_t)-1) {
    char info[300];
    mbedtls_x509_crt_verify_info(info, sizeof(info), "    raison : ", drapeaux);
    Console.printf("    drapeaux 0x%08lx\n%s", (unsigned long)drapeaux, info);
  }
  const mbedtls_x509_crt* c = nullptr;
  if (ctx->ssl_ctx.session_negotiate) c = ctx->ssl_ctx.session_negotiate->peer_cert;
  if (c == nullptr && ctx->ssl_ctx.session) c = ctx->ssl_ctx.session->peer_cert;
  for (int i = 0; c != nullptr && i < 5; c = c->next, i++) {
    if (c->raw.len == 0) continue;
    char sujet[100], emetteur[100];
    mbedtls_x509_dn_gets(sujet, sizeof(sujet), &c->subject);
    mbedtls_x509_dn_gets(emetteur, sizeof(emetteur), &c->issuer);
    Console.printf("    reçu %d : %s | émis par %s | %s %u bits | fin %04d-%02d-%02d\n", i, sujet, emetteur,
                   mbedtls_pk_get_name(&c->pk), (unsigned)mbedtls_pk_get_bitlen(&c->pk),
                   c->valid_to.year, c->valid_to.mon, c->valid_to.day);
  }
  stop_ssl_socket(ctx, nullptr, nullptr, nullptr);
  delete ctx;
  memoireTls("après");
}

void diagnosticTls() {
  if (!reseauOK()) { Console.println("diag tls : pas de Wi-Fi."); return; }
  if (mutexTls && xSemaphoreTake(mutexTls, pdMS_TO_TICKS(20000)) != pdTRUE) {
    Console.println("diag tls : une autre connexion HTTPS est en cours, réessaie.");
    return;
  }
  Console.printf("Diagnostic HTTPS v4 (racines en bundle, version %lu). Aucune donnée n'est envoyée.\n",
                 (unsigned long)FW_VERSION);
  Console.printf("    bundles : AirCarto %u racines (%u octets), relais %u racines (%u octets)\n",
                 (unsigned)(BUNDLE_AIRCARTO[0] << 8 | BUNDLE_AIRCARTO[1]), (unsigned)sizeof(BUNDLE_AIRCARTO),
                 (unsigned)(BUNDLE_TLS[0] << 8 | BUNDLE_TLS[1]), (unsigned)sizeof(BUNDLE_TLS));
  essaiTls("api.aircarto.fr", BUNDLE_AIRCARTO, "1) AirCarto, racines de Let's Encrypt (comme les envois)");
  esp_task_wdt_reset();
  // relais : son nom seulement (https://<nom>/...), jamais le jeton
  String hote = cfgRelais.startsWith("https://") ? cfgRelais.substring(8) : String("");
  int fin = hote.indexOf('/');
  if (fin >= 0) hote = hote.substring(0, fin);
  if (hote.length() > 0 && hote.indexOf(':') < 0) {
    essaiTls(hote.c_str(), BUNDLE_TLS, "2) relais, 10 racines (comme la console à distance)");
  } else {
    Console.println("--- 2) relais : pas d'adresse (set relais au câble USB), essai sauté");
  }
  esp_task_wdt_reset();
  essaiTls("api.aircarto.fr", nullptr, "3) AirCarto SANS vérification (diagnostic seulement)");
  if (mutexTls) xSemaphoreGive(mutexTls);
  Console.printf("Vérifications par bundle depuis le démarrage : %lu (0 = vérification La Bonne M'Air pas branchée)\n",
                 (unsigned long)nbVerifsBundle);
  Console.println("Fin du diagnostic : copie toutes ces lignes.");
}

// Commandes acceptées par Bluetooth et par la console à distance : rien qui touche aux mots de passe, à la clé,
// au jeton, au relais, rien qui efface, rien qui ait un effet durable. Comparaison sur la commande ENTIÈRE (en
// minuscules, espaces réduits). « calib oui » n'y est plus : lancé depuis un téléphone appairé ou une console dont
// le mot de passe aurait fuité, il fausserait les mesures de CO2 jusqu'au prochain calibrage (câble USB seulement).
// MÊME LISTE dans relais/src/index.js (COMMANDES_AUTORISEES).
bool commandeAutoriseeADistance(const String &bas) {
  static const char* const AUTORISEES[] = {
    "aide", "help", "status", "verbose on", "verbose off", "calib", "reboot",
    "wifi scan", "wifi connect", "maj", "maj installer", "diag tls"
  };
  for (const char* a : AUTORISEES) if (bas == a) return true;
  return false;
}

// Bluetooth : il faut un téléphone appairé, à portée de la carte. Il a la liste blanche du relais, plus le réglage
// de l'écran (« lum », « nuit »), refusé au relais : régler l'écran ne se fait pas à distance par le Wi-Fi.
bool commandeAutoriseeBluetooth(const String &bas) {
  return commandeAutoriseeADistance(bas) || commandeEcran(bas, false);
}

// Commande dont la sortie ne doit pas quitter le câble USB (noms de réseaux, identifiant ENT, longueurs...).
bool commandeSensible(const String &bas) {
  return bas == "wifi" || bas.startsWith("wifi ") || bas.startsWith("set ") || bas == "oublier" || bas.startsWith("bt ");
}

// Limites des commandes à distance (Bluetooth, relais) : rien ne doit pouvoir tourner en boucle.
static const unsigned long ECART_COMMANDES_DISTANTES_MS = 1000UL;    // une commande par seconde (les suivantes attendent)
static const unsigned long ECART_RESEAU_DISTANT_MS      = 30000UL;   // wifi scan / wifi connect / maj : une fois par 30 s
static const unsigned long ECART_REBOOT_DISTANT_MS      = 300000UL;  // reboot : une fois par 5 min
static const unsigned long ECART_DIAG_DISTANT_MS        = 300000UL;  // diag tls (~20 s de connexions) : une fois par 5 min
static const uint32_t      MARQUE_REBOOT_DISTANT        = 0x52454244UL;
// Mémoire RTC : garde sa valeur pendant un redémarrage logiciel (pas après une coupure de courant). Elle dit si
// CE démarrage vient d'un « reboot » demandé à distance : un compteur en RAM repartirait de zéro à chaque redémarrage
// et n'empêcherait pas des redémarrages en boucle. (Pendant l'essai d'une nouvelle version, reboot et maj sont de
// toute façon refusés à distance : un seul redémarrage avant validation la ferait revenir à l'ancienne.)
RTC_NOINIT_ATTR uint32_t rebootDistant;
bool demarreParRebootDistant = false;
unsigned long tDerniereDistante = 0;   // dernière commande à distance exécutée (traiterCommandesAttente)

// Au démarrage (setup) : ce démarrage vient-il d'un reboot demandé à distance ?
void lireRebootDistant() {
  demarreParRebootDistant = (rebootDistant == MARQUE_REBOOT_DISTANT && esp_reset_reason() == ESP_RST_SW);
  rebootDistant = 0;
}

// Refuse une commande à distance trop rapprochée de la précédente du même genre (message, puis false).
bool commandeDistantePermise(const String &bas, const char* origine) {
  // Un délai PAR genre de commande : avant, « maj » (chercher) et « maj installer » partageaient le même délai de
  // 30 s, et « Installer » juste après « Chercher une mise à jour » était refusé alors qu'on n'avait appuyé qu'une fois.
  static unsigned long tWifiDistant = 0, tMajDistant = 0, tInstallDistant = 0;
  unsigned long now = millis();
  bool maj = bas.startsWith("maj");
  bool reseau = bas.startsWith("wifi ") || maj;
  unsigned long &tReseauDistant = bas.startsWith("wifi ") ? tWifiDistant : (bas == "maj installer" ? tInstallDistant : tMajDistant);
#if CONSOLE_DISTANTE
  // Nouvelle version en essai (1 min + un échange avec le relais) : le démarreur de l'ESP32 revient à l'ancienne
  // version au PREMIER redémarrage avant validation. Un reboot à distance pourrait donc annuler toute mise à jour.
  if (otaEnEssai && (bas == "reboot" || maj)) {
    Console.printf("> %s : refusée depuis %s pendant l'essai d'une nouvelle version (réessaie dans 1 min).\n",
                   bas.c_str(), origine);
    return false;
  }
#endif
  if (reseau && tReseauDistant != 0 && now - tReseauDistant < ECART_RESEAU_DISTANT_MS) {
    Console.printf("> %s : refusée depuis %s (une fois par %lu s au plus).\n", bas.c_str(), origine,
                   ECART_RESEAU_DISTANT_MS / 1000UL);
    return false;
  }
  if (bas == "reboot" && demarreParRebootDistant && now < ECART_REBOOT_DISTANT_MS) {
    Console.printf("> reboot : refusé depuis %s (déjà redémarrée à distance il y a moins de %lu min).\n", origine,
                   ECART_REBOOT_DISTANT_MS / 60000UL);
    return false;
  }
  // diag tls : ~20 s de connexions HTTPS pendant lesquelles les envois attendent (mesures gardées) ; sans secret.
  static unsigned long tDiagDistant = 0;
  if (bas == "diag tls") {
    if (tDiagDistant != 0 && now - tDiagDistant < ECART_DIAG_DISTANT_MS) {
      Console.printf("> diag tls : refusée depuis %s (une fois par %lu min au plus).\n", origine, ECART_DIAG_DISTANT_MS / 60000UL);
      return false;
    }
    tDiagDistant = now | 1UL;
  }
  if (reseau) tReseauDistant = now | 1UL;
  if (bas == "reboot") rebootDistant = MARQUE_REBOOT_DISTANT;   // relu au démarrage suivant (lireRebootDistant)
  return true;
}

// Point d'entrée de TOUTES les commandes (USB, Bluetooth, relais).
void executerCommandeSource(const String &cmd, uint8_t source) {
  String bas = cmd;
  normaliserCommande(bas);   // "verbose   on" = "verbose on"
  if (source != SOURCE_USB) {
    const char* origine = (source == SOURCE_BLUETOOTH) ? "Bluetooth" : "console à distance";
    bool permise = (source == SOURCE_BLUETOOTH) ? commandeAutoriseeBluetooth(bas) : commandeAutoriseeADistance(bas);
    if (!permise) {
      // on ne recopie que le premier mot : la suite peut contenir un mot de passe
      int sp = bas.indexOf(' ');
      String debut = (sp < 0) ? bas : bas.substring(0, sp) + " ...";
      Console.printf("> %s : refusée depuis %s (%s). Tape 'aide'.\n", debut.c_str(), origine,
                     commandeEcran(bas, false) ? "écran : câble USB ou Bluetooth seulement" : "réservée au câble USB");
      return;
    }
    if (!commandeDistantePermise(bas, origine)) return;
    Console.printf("> %s   (%s)\n", bas.c_str(), origine);
    executerCommande(bas);
    return;
  }
  // USB : tout est permis. Seules les commandes de la liste blanche (sans secret) sont recopiées à distance avec ce
  // qu'elles affichent ; toutes les autres restent sur l'USB, y compris une commande mal tapée qui contiendrait un
  // mot de passe (« wfi ent eduroam id MDP »).
  if (commandeSensible(bas) || !commandeAutoriseeADistance(bas)) {
    sortieUsbSeule++;
    executerCommande(cmd);
    sortieUsbSeule--;
    return;
  }
  String echo = "> " + cmd + "   (USB)\n";
  ecrireADistance(echo.c_str());
  executerCommande(cmd);
}

// File des commandes reçues par Bluetooth ou par le relais : déposées par leur tâche, exécutées par loop()
// (comme une commande tapée au clavier, jamais en même temps qu'elle).
#define NB_COMMANDES_ATTENTE 8
struct CommandeAttente { char texte[104]; uint8_t source; };
CommandeAttente commandesAttente[NB_COMMANDES_ATTENTE];
uint8_t nbCommandesAttente = 0;
SemaphoreHandle_t mutexCommandes = NULL;

void deposerCommande(const char* texte, uint8_t source) {
  if (mutexCommandes == NULL) return;
  xSemaphoreTake(mutexCommandes, portMAX_DELAY);
  if (nbCommandesAttente < NB_COMMANDES_ATTENTE) {
    CommandeAttente &c = commandesAttente[nbCommandesAttente++];
    snprintf(c.texte, sizeof(c.texte), "%s", texte);
    c.source = source;
    texte = nullptr;
  }
  xSemaphoreGive(mutexCommandes);
  if (texte != nullptr) LOG("Commande ignorée : déjà %u en attente.\n", (unsigned)NB_COMMANDES_ATTENTE);
}

void traiterCommandesAttente() {
  if (mutexCommandes == NULL || nbCommandesAttente == 0) return;
  // Bluetooth et relais : une commande par seconde au plus ; les suivantes attendent leur tour dans la file.
  if (tDerniereDistante != 0 && millis() - tDerniereDistante < ECART_COMMANDES_DISTANTES_MS) return;
  tDerniereDistante = millis() | 1UL;
  CommandeAttente c;
  xSemaphoreTake(mutexCommandes, portMAX_DELAY);
  c = commandesAttente[0];
  for (uint8_t i = 1; i < nbCommandesAttente; i++) commandesAttente[i - 1] = commandesAttente[i];
  nbCommandesAttente--;
  xSemaphoreGive(mutexCommandes);
  String cmd = c.texte;
  cmd.trim();
  if (cmd.length() > 0) executerCommandeSource(cmd, c.source);
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
        Serial.print("\r\n");  // retour à la ligne après ce qui a été tapé (écho : USB seulement)
        cmd.trim();
        if (cmd.length() > 0) executerCommandeSource(cmd, SOURCE_USB);
      }
    } else if (c == 0x08 || c == 0x7F || c == 0x15) {  // retour arrière | Ctrl+U : efface toute la ligne
      while (ligneSerie.length() > 0) {
        ligneSerie.remove(ligneSerie.length() - 1);
        Serial.print("\b \b");  // recule, efface, recule
        if (c != 0x15) break;     // retour arrière : un seul caractère
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
  Console.printf("\n=== La Bonne M'Air | cause du redémarrage : %d (%s) ===\n", (int)esp_reset_reason(),
                 texteCauseRedemarrage(esp_reset_reason()));
  Console.println("    0 inconnue | 1 mise sous tension | 2 bouton reset | 3 redémarrage voulu par le programme");
  Console.println("    4 plantage | 5 et 6 programme bloqué (chien de garde) | 7 blocage grave (chien de garde matériel)");
  Console.println("    8 réveil de veille profonde | 9 baisse de tension (alimentation trop faible) | 10 SDIO (jamais sur cette carte)");

  mutexEnvoi = xSemaphoreCreateMutex();  // avant tout : loop() et la tâche d'envoi s'en servent
  mutexCommandes = xSemaphoreCreateMutex();   // commandes reçues par Bluetooth / relais
  mutexTls = xSemaphoreCreateMutex();         // une seule connexion HTTPS à la fois (AirCarto / relais)
  hTacheLoop = xTaskGetCurrentTaskHandle();   // setup() et loop() tournent dans la même tâche
  // Réserve HTTPS : avant le Wi-Fi et le Bluetooth, tant que la mémoire est d'un seul tenant.
  if (!reserverTamponsTls())
    Console.println("HTTPS : réserve mémoire impossible avec cette version de mbedTLS (fonctionnement d'avant).");
  else
    Console.printf("HTTPS : %u tampon(s) de %u octets réservés.\n", (unsigned)nbTamponsTls, (unsigned)TAMPON_TLS_OCTETS);
  // Mesures en attente : leur place est prise maintenant, une fois pour toutes (une mesure fait ~350 octets). Prise
  // au fil de l'eau, elle tombait n'importe où, souvent dans la place que venait de libérer une connexion HTTPS.
  for (uint8_t i = 0; i < NB_PENDANTES; i++) pendantes[i].reserve(TAILLE_PENDANTE);
  lireRebootDistant();                        // ce démarrage vient-il d'un « reboot » demandé à distance ?
#if CONSOLE_DISTANTE
  mutexJournal = xSemaphoreCreateMutex();     // journal à distance
  verifierDemarrageOta();                     // nouvelle version en essai ? (retour à l'ancienne si elle boucle)
#endif
  // Identifiant aléatoire de CE démarrage : permet de savoir si une mesure stockée date du même démarrage (voir calculerAge).
  bootId = esp_random();
  if (bootId == 0) bootId = 1;
  // Identifiant de la carte = son adresse MAC (unique).
  deviceId = lireMac();
  // Relit Wi-Fi, clé du groupe, position de rejeu... enregistrés en mémoire permanente.
  chargerConfig();
  verifierCaEduroam();
  for (uint8_t i = 0; i < nbReseaux; i++)
    if (estEntreprise(i) && caEduroamValide != 1) {
      Console.println("ATTENTION : réseau entreprise sans certificat vérifié (ca_eduroam.h) : un faux point d'accès peut capturer ton mot de passe ENT.");
      break;
    }
#if CONSOLE_DISTANTE
  majRelaisConfigure();
  Console.printf("Version du programme : %lu%s\n", (unsigned long)FW_VERSION, otaEnEssai ? " (nouvelle, en essai)" : "");
#endif

  // Monte la mémoire flash comme un petit disque (true = la formate si c'est le premier démarrage).
  fsOK = LittleFS.begin(true);
  if (!fsOK) Console.println("Erreur montage LittleFS : pas de stockage hors ligne.");
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
  afficherSplash();  // écran d'accueil : logo animé LA BONNE M'AIR (~7 s)

  // NextPM : Modbus, 8E1 (parité paire !), adresse 1
  nextpmSerial.begin(115200, SERIAL_8E1, NEXTPM_RX, NEXTPM_TX);
  nextpm.begin(1, nextpmSerial);

#if MHZ_ACTIVE
  // MH-Z19C : liaison série à 9600 bauds ; l'auto-calibration (ABC) recale seule le zéro à 400 ppm.
  Serial2.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
  mhz.begin(Serial2);
  mhz.autoCalibration(MHZ_ABC);  // ABC : 0 si la pièce n'est jamais aérée
#endif

#if BLUETOOTH_ACTIF
  // Bluetooth AVANT le Wi-Fi : la radio est partagée. Démarré pendant le scan ou la connexion (comme avant), il
  // coupait la radio au mauvais moment et le 1er essai Wi-Fi du démarrage échouait.
  initialiserBluetooth();
#endif

  // Lance la connexion Wi-Fi en arrière-plan (scan puis meilleur réseau connu) : le programme n'attend pas,
  // l'écran montre la progression (page de connexion) puis le résultat.
  initialiserWiFi();
  if (!cleOK()) Console.println("Clé du groupe absente : les mesures seront gardées en mémoire. Tape 'aide'.");

  // Watchdog : la carte redémarre toute seule si loop() reste bloquée 60 s
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#if AIRCARTO_HTTPS
  // HTTPS vers AirCarto : seules les racines de Let's Encrypt sont acceptées (BUNDLE_AIRCARTO), le nom
  // api.aircarto.fr et la date du certificat sont vérifiés ; poignée de main 10 s au plus.
  clientAircarto.racines(BUNDLE_AIRCARTO);   // racines de Let's Encrypt seulement
  clientAircarto.setHandshakeTimeout(10);
#endif
  // réseau + fichier hors ligne, cœur 1 comme loop() ; pile agrandie (8 -> 12 Ko) pour la poignée de main HTTPS
  xTaskCreatePinnedToCore(tacheEnvoi, "envoi", 12288, NULL, 1, &hTacheEnvoi, 1);
#if CONSOLE_DISTANTE
  xTaskCreatePinnedToCore(tacheRelais, "relais", 12288, NULL, 1, &hTacheRelais, 0); // console à distance + mise à jour (HTTPS)
#endif

  aTransition = true;  // fondu (court) du logo vers la première page
  fonduCourt = true;
  afficher();          // première page : CO2 en préchauffage
  Console.println("Prêt. Tape 'aide' pour la liste des commandes.");
}

// =====================================================================
// LOOP
// =====================================================================
// loop() tourne sans arrêt sur le cœur 1. Chaque travail ci-dessous se déclenche quand son délai est écoulé.
// L'envoi au serveur n'est PAS ici : il est fait par tacheEnvoi, pour ne jamais figer l'écran.
void loop() {
  unsigned long now = millis();
  esp_task_wdt_reset();

  // À chaque tour : commandes tapées (USB), reçues (Bluetooth, relais), coupure de courant, bouton.
  traiterSerie();
  traiterCommandesAttente();
#if BLUETOOTH_ACTIF
  btEnvoyer(now);
#endif
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
    surveillerEnvois(now);
    verifierMemoire();
  }

  // Chaque seconde : lecture des capteurs, puis mise à jour des moyennes et des pics.
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

  // Toutes les 10 s en direct (30 s hors ligne ou en rattrapage, 60 s sur batterie) : la moyenne de la période
  // est prête, on la dépose pour l'envoi (en direct, calée sur le créneau du serveur : voir mesureDue).
  if (mesureDue(now, tPeriode)) {
    tPeriode = now;
    finaliserPeriode();
  }

  // Toutes les 5 s (3 s pour la page ÉTAT) : page suivante de l'écran, avec un fondu
  // (en pause pendant l'écran de connexion Wi-Fi, le code Bluetooth et la mise à jour : la page reste fixe,
  // sans fondu ni redessin toutes les 5 s, et la rotation reprend où elle en était).
  if (ecranWifiActif() || ecranSpecialActif()) tPage = now;
  if (now - tPage >= dureePage()) {
    tPage = now;
    pageSuivante();
  }

  animer(now);  // animations (défilement, chiffres, yeux, préchauffage) : demande un redessin si besoin

  // Code Bluetooth ou progression d'une mise à jour : redessin dès que ça change (ou que ça disparaît)
  static uint32_t signatureSpeciale = 0;
  uint32_t sig = ecranSpecialActif() ? (codeAppairage ^ ((uint32_t)majProgression << 24)) + 1 : 0;
  if (sig != signatureSpeciale) {
    if (sig == 0) { aTransition = true; fonduCourt = true; }   // code / mise à jour finis : retour en douceur (~0,7 s)
    signatureSpeciale = sig;
    aRedessiner = true;
  }

  majFondu(now);   // fondu entre deux pages : un cran par tour

  // Un seul redessin, même si lecture + changement de page + animation tombent ensemble
  if (aRedessiner) {
    aRedessiner = false;
    afficher();
  }

  // Toutes les 10 s : ajuste la luminosité (jour / nuit / batterie).
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