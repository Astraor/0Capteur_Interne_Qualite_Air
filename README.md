# La Bonne M’air — capteur de qualité de l’air intérieur

Station de mesure de la qualité de l’air basée sur un **ESP32 (carte ModuleAir)** : elle relève les **particules fines (PM1, PM2.5, PM10)**, le **CO₂**, la **température** et l’**humidité**, les affiche sur un **écran LED 64 × 32** et les envoie au serveur **AirCarto**. Un **site web** (hébergé sur GitHub Pages) récupère ces mesures et les présente en graphiques interactifs.

🌐 **Site en ligne :** https://astraor.github.io/0Capteur_Interne_Qualite_Air/

> Les seuils de qualité de l’air utilisés sur la carte **et** sur le site sont ceux de l’**indice ATMO** ([atmo-france.org](https://www.atmo-france.org/article/lindice-atmo), arrêté du 10 juillet 2020). Capteurs à bas coût : les valeurs sont **indicatives** et n’ont pas de valeur réglementaire.

---

## Sommaire

1. [Vue d’ensemble](#vue-densemble)
2. [Matériel et câblage](#matériel-et-câblage)
3. [Organisation du dépôt](#organisation-du-dépôt)
4. [Firmware ESP32](#firmware-esp32)
5. [Site web](#site-web)
6. [Seuils et indice ATMO](#seuils-et-indice-atmo)
7. [Alerte mail « capteur muet »](#alerte-mail--capteur-muet-)
8. [Limites connues](#limites-connues)
9. [Sources](#sources)

---

## Vue d’ensemble

```mermaid
flowchart LR
    subgraph Carte["Carte ModuleAir (ESP32)"]
        PM["NextPM<br/>PM1 · PM2.5 · PM10<br/>T° · HR"] -- Modbus / UART1 --> ESP((ESP32))
        CO2["MH-Z19C<br/>CO₂ (NDIR)"] -- UART2 --> ESP
        ESP -- HUB75 --> LED["Écran LED<br/>64 × 32"]
        ESP -. pas de réseau .-> FS[("Flash LittleFS<br/>~10 h de mesures")]
        FS -. renvoi automatique .-> ESP
    end
    TEL["Téléphone<br/>(Bluetooth)"] <-. "console de dépannage" .-> ESP
    ESP <-- "HTTPS : journal, commandes,<br/>mises à jour signées" --> RELAIS["Relais Cloudflare<br/>+ page console"]
    GH["Release GitHub<br/>firmware signé"] --> RELAIS
    ESP -- "HTTP POST (JSON)<br/>toutes les 10 s" --> API[("Serveur AirCarto")]
    API -- "API /data<br/>(avec la clé du groupe)" --> RELAIS["Relais Cloudflare<br/>mair-relais.astraor.workers.dev"]
    RELAIS -- "/donnees<br/>(lecture seule, sans clé)" --> SITE["Site web<br/>GitHub Pages + Chart.js"]
    RELAIS -- "toutes les 5 min" --> GHA["GitHub Actions<br/>surveillance"]
    GHA -- "issue → mail" --> MAIL["Propriétaire du dépôt"]
```

**Le chemin d’une mesure :**
capteurs lus **chaque seconde** → moyenne + pic sur **10 s** → envoi JSON au serveur AirCarto → relais Cloudflare (qui ajoute la clé du groupe, voir `relais/LISEZMOI.md`) → affichage sur le site.
Sans réseau, la mesure est rangée en flash puis renvoyée plus tard avec son âge (`x_age_s`), ce qui permet au site de la remettre à sa **vraie heure**.

---

## Matériel et câblage

| Élément | Rôle | Liaison |
|---|---|---|
| ESP32 WROOM-32U (carte ModuleAir) | Microcontrôleur, Wi-Fi | — |
| **NextPM** (Tera Sensor) | Particules PM1 / PM2.5 / PM10 par diffusion laser, + T° et humidité internes | Modbus RTU sur UART1 |
| **MH-Z19C** (Winsen) | CO₂ par absorption infrarouge (NDIR) | UART2 |
| Panneau LED RGB **64 × 32** (HUB75, 1/16) | Affichage local | PxMatrix |
| Pont diviseur (optionnel) | Détection secteur / batterie (désactivée par défaut : `DETECTION_SECTEUR_ACTIVE`) | Entrée analogique |

### Brochage

| Fonction | GPIO |
|---|---|
| NextPM — RX ESP32 (fil TX capteur) | 39 |
| NextPM — TX ESP32 (fil RX capteur) | 32 |
| MH-Z19C — RX ESP32 (fil TX capteur) | 36 |
| MH-Z19C — TX ESP32 (fil RX capteur) | 27 |
| Détection secteur | 34 |
| Écran HUB75 — LAT / OE | 25 / 16 |
| Écran HUB75 — A / B / C / D / E | 17 / 33 / 4 / 12 / 15 |
| Bouton (optionnel) | `PIN_BOUTON`, désactivé par défaut (`-1`) |
| Buzzer actif (optionnel) | `PIN_BUZZER`, désactivé par défaut (`-1`) |

> Couleurs inversées sur l’écran ? Dans `setupDisplay()`, remplacer `RRBBGG` par `RRGGBB`.

---

## Organisation du dépôt

```
.
├── index.html                  # Site web (page unique : HTML + CSS + JS)
├── js/i18n.js                  # Moteur de traduction (sans dépendance)
├── js/logo-led.js              # Logo LED de la carte : onglet, écran d'entrée, introduction, chargement, pied de page
├── lang/
│   ├── fr.js                   # Textes en français (langue d'origine)
│   ├── en.js                   # Anglais
│   └── ja.js                   # Japonais
├── img/                        # Affiche (900 et 1696 px), icônes LED (favicon.svg / .ico, icone-192 / 512, icône iPhone)
├── outils/icones.html          # Outil : fabrique les icônes de img/ depuis js/logo-led.js (non publié)
├── patch-lang.js               # Outil : met à jour des clés de traduction dans les 3 langues
├── Firmware/
│   ├── platformio.ini          # Configuration PlatformIO
│   ├── partitions_mair.csv     # Mémoire : 2 × 1,5 Mo pour le programme (mise à jour à distance) + 896 Ko de mesures
│   ├── outils/ota.py           # Version, signature des mises à jour, « pio run -t cles »
│   └── src/
│       ├── main.cpp            # Programme de la carte
│       ├── racines_tls.h       # Certificats acceptés pour joindre le relais
│       └── cle_ota.h           # Clé PUBLIQUE de signature (créée par « pio run -t cles »)
├── relais/                     # Relais Cloudflare : console à distance + mises à jour (voir relais/LISEZMOI.md)
└── .github/
    ├── workflows/surveillance-capteur.yml   # Tâche planifiée toutes les 5 min
    └── scripts/surveiller-capteur.mjs       # Vérifie que le capteur envoie encore
```

---

## Firmware ESP32

### Installation (VS Code + PlatformIO)

1. Installer **VS Code** et l’extension **PlatformIO IDE**.
2. Ouvrir le dossier `Firmware/` comme projet PlatformIO.
3. `Firmware/platformio.ini` fixe déjà la plateforme (`espressif32@^6.8.0`, ESP32 Arduino 2.0.17) : le programme est écrit pour **ESP32 Arduino 2.x** et refuse de compiler en 3.x. Les versions 6.5 / 6.6 rendent la carte invisible pour certains Android : si ta plateforme est plus ancienne, lance une fois `pio pkg update`.
4. Compiler et téléverser (`PlatformIO: Upload`), puis ouvrir le moniteur série à **115 200 bauds**.

Bibliothèques (installées automatiquement par `lib_deps`) :
`ModbusMaster 2.0.1`, `MH-Z19 ^1.5.4`, `PxMatrix 1.8.2`, `Adafruit GFX 1.11.11`, `ArduinoJson ^7.4.3`, `NimBLE-Arduino ^2.3.6` (Bluetooth).

La mémoire flash est découpée par `partitions_mair.csv` : deux emplacements de 1,5 Mo pour le programme (la mise à jour à distance s'écrit dans celui qui ne tourne pas) et 896 Ko pour les mesures hors ligne.

### Configuration par le moniteur série

Aucun identifiant n’est écrit dans le code : tout est enregistré dans la mémoire de la carte.

| Commande | Effet |
|---|---|
| `wifi ent eduroam prenom.nom@etu.univ-amu.fr MOT_DE_PASSE` | Ajoute un réseau WPA2-Entreprise (eduroam) |
| `wifi add MaBox MotDePasse` | Ajoute un Wi-Fi classique |
| `wifi add "iPhone de Paul" MotDePasse` | Nom avec espaces : entre guillemets |
| `wifi list` · `wifi del <n>` ou `wifi del <nom>` · `wifi scan` · `wifi connect` | Gérer les réseaux (5 au maximum) ; `wifi del` en supprime **un seul**, par son numéro ou son nom exact |
| `set cle <32 caractères>` | Clé du groupe AirCarto |
| `status` | État complet (réseau, capteurs, file d’attente, temps de rattrapage) |
| `verbose on` / `verbose off` | Active / coupe les logs automatiques (coupés par défaut) |
| `calib oui` | Calibrage du zéro CO₂ (après 20 min à l’air extérieur, ~400 ppm) |
| `vider_file` | Efface les mesures en attente d’envoi |
| `oublier` | Efface Wi-Fi, clé, jeton du relais et téléphones Bluetooth — **à faire avant de prêter la carte** |
| `set relais <url>` · `set jeton <jeton>` | Adresse du relais Cloudflare et jeton secret de la carte (console à distance) |
| `maj` · `maj installer` | Cherche / installe la dernière version publiée (signée) |
| `maj oublier` | Réautorise une version qui a échoué sur la carte (câble USB) |
| `bt appairer` | Autorise un **nouveau** téléphone Bluetooth pendant 2 min |
| `bt demarrage on` / `off` | Autorise aussi un nouveau téléphone pendant 2 min après chaque mise sous tension (désactivé par défaut) |
| `bt liste` · `bt oublier <n>` | Téléphones appairés (**5 au maximum** en mémoire, un seul connecté à la fois) ; en oublier **un seul** |
| `bt oublier` | Efface tous les téléphones Bluetooth appairés |
| `lum` · `lum +` · `lum -` | Luminosité de l'écran (10 niveaux) : `+` / `-` règlent la période en cours, jour ou nuit — **câble USB ou Bluetooth** |
| `lum jour <1..10>` · `lum nuit <1..10>` · `lum defaut` | Niveau précis de jour / de nuit (par défaut : jour 10, nuit 2), gardé en mémoire |
| `nuit 22 7` · `nuit off` | Heures du mode nuit (par défaut 22 h → 7 h, heure de France) ; `off` : jamais |
| `aide` | Liste des commandes |

La carte se connecte seule au meilleur réseau connu visible, sans redémarrage.

### Console sans fil et mises à jour à distance

Le moniteur série est aussi disponible **sans câble** (mode d'emploi complet : [`relais/LISEZMOI.md`](relais/LISEZMOI.md)) :

- **par Internet**, via un petit relais gratuit chez Cloudflare (`relais/`) : la carte y envoie son journal en HTTPS (toutes les 30 s, toutes les 2 s quand la page console est ouverte) et y prend les commandes. Passe partout, eduroam compris. La page console range le journal (7 jours) par jour : un intertitre « Aujourd'hui », « Hier » ou la date reste collé en haut pendant qu'on lit ses lignes, et « Copier » écrit la date devant chaque ligne ;
- **par Bluetooth**, à côté de la carte : appairage avec le code à 6 chiffres affiché sur l'écran. Un nouveau téléphone n'est accepté que dans les 2 min qui suivent un `bt appairer` (câble USB), ou une mise sous tension si tu as activé `bt demarrage on` : quelqu'un qui voit l'écran depuis la pièce ne peut pas s'appairer à ton insu. Voir [Bluetooth : si la connexion échoue](#bluetooth--si-la-connexion-échoue) ;
- **mises à jour** : `maj installer` télécharge la dernière release GitHub, vérifie sa **signature** (ECDSA, clé privée restée sur ton PC) et son empreinte, l'installe dans l'autre emplacement de la mémoire et revient tout seul à l'ancienne version si la nouvelle ne marche pas.

Sans câble, seules les commandes **sans secret, sans effacement et sans effet durable** passent (`aide`, `status`, `verbose`, `calib`, `reboot`, `wifi scan`, `wifi connect`, `maj`), une par seconde au plus (`wifi scan`, `wifi connect`, `maj` : une fois par 30 s ; `reboot` : une fois par 5 min). Par Bluetooth seulement (pas par le relais) : `lum` et `nuit`, pour régler l'écran à côté de la carte. Wi-Fi, clé, jeton, relais, `calib oui`, `bt …`, `oublier`, `vider_file` : **câble USB uniquement**, et ce qu'ils affichent ne quitte jamais le câble.

#### Bluetooth : si la connexion échoue

Avec **Serial Bluetooth Terminal** (Android) : menu → *Devices* → onglet **Bluetooth LE** (pas *Classic*) → *Scan* → `MAir-XXXX`.

- **1er appairage :** l'appli se déconnecte dès que le téléphone demande le code (« PAIRING requested… Connect again ») : c'est normal, tape le code affiché sur l'écran, puis **reconnecte-toi** une 2e fois.
- **« gatt status 133 » ou « 147 »** : la liaison n'a pas pu s'établir (radio occupée par le Wi-Fi, carte déjà connectée à un autre téléphone, trop loin, antenne absente). Réessaie quelques secondes plus tard ; la carte n'accepte **qu'un téléphone à la fois**. Depuis le 25/11, le Wi-Fi laisse la radio au Bluetooth pendant 30 s après chaque déconnexion (le temps de se reconnecter), et eduroam n'insiste plus en boucle quand il est refusé (voir plus bas).
- **Échec à chaque fois, même téléphone :** le téléphone a gardé un ancien appairage que la carte n'a plus (`bt oublier`, `oublier`, mémoire effacée). Sur le téléphone : réglages Bluetooth → `MAir-XXXX` → **Oublier**, puis `bt appairer` au câble USB et reconnecte-toi. La carte l'écrit elle-même dans son journal quand elle reconnaît ce cas.
- **« write descriptor failed »** : l'abonnement du téléphone aux réponses n'a pas pu être mémorisé (mémoire pleine, réglée depuis le 25/11 : 12 abonnements pour 5 téléphones). Si ça revient : `status` (ligne « abonnements mémorisés ») et `bt liste` / `bt oublier <n>` pour retirer les téléphones inutiles.
- La carte écrit la **raison de chaque déconnexion** dans son journal (`0x23E` liaison jamais établie : radio occupée ; `0x208` liaison perdue ; `0x213` fermée par le téléphone ; `0x206` clé inconnue ; `0x205` appairage raté), et `status` indique si elle **diffuse** (visible par les téléphones).
- **Antenne :** la WROOM-32**U** n'a pas d'antenne intégrée : sans l'antenne externe (connecteur U.FL), Wi-Fi et Bluetooth ne portent qu'à quelques centimètres.

Ce que fait la carte depuis le 25/11 pour fiabiliser la connexion : diffusion surveillée chaque seconde (relancée si une liaison avortée l'a arrêtée), appairage demandé seulement aux téléphones inconnus (un téléphone connu chiffre de lui-même), aucune réponse envoyée avant que le téléphone soit abonné (la 1re ligne ne se perd plus), connexion sans appairage coupée au bout de 35 s (au lieu de 90 s : la seule place de connexion se libère plus vite), paramètres de liaison plus robustes après l'appairage, puissance d'émission +9 dBm. Côté Wi-Fi (même radio) : canal de 20 MHz, scans deux fois plus courts, pause entre deux séries d'essais qui s'allonge (30 s, 60 s, 2 min) quand aucun réseau ne répond, et aucun scan pendant les 15 premières secondes d'une connexion Bluetooth.

**eduroam :** pour que la carte refuse un faux point d'accès « eduroam » (qui récupérerait ton mot de passe ENT), la carte vérifie le certificat du serveur de l'université. Pour **Aix-Marseille Université**, il est déjà fourni : [`Firmware/src/ca_eduroam.h`](Firmware/src/ca_eduroam.h) (autorité « GEANT TLS ECC 1 », tirée de cat.eduroam.org). Pour une autre université : mode d'emploi dans [`Firmware/src/ca_eduroam.h.exemple`](Firmware/src/ca_eduroam.h.exemple). `status` indique si le serveur ENT est vérifié, et la carte prévient au démarrage tant qu'il ne l'est pas. Le plus sûr reste de ne pas mettre ton mot de passe ENT personnel dans la carte (réseau dédié aux objets, partage de connexion d'un téléphone).

**Réseaux usurpés :** la carte ne se connecte qu'au **point d'accès précis** (adresse BSSID et canal) vu au scan **avec la même sécurité** que le réseau enregistré : un faux point d'accès ouvert nommé « eduroam » ou comme ta box est ignoré, même s'il émet plus fort.

**eduroam, identité anonyme :** `anonymous@ton-domaine` circule en clair à la place de ton identifiant (`EDUROAM_IDENTITE_ANONYME`). Si l'université la refuse, le 2e essai se fait avec ton identifiant réel (méthode TTLS-PAP, celle d'AMU).

**eduroam refusé :** après deux refus de suite (identité anonyme puis réelle), la carte laisse eduroam de côté **10 min** : réessayer sans arrêt occuperait la radio (le Bluetooth ne passerait plus) et pourrait faire bloquer ton compte ENT. `wifi connect` réessaie tout de suite. Pour savoir si c'est le certificat qui bloque : `wifi ent test` (explications), puis `wifi ent test oui` : **un** essai sans vérifier le certificat. Connecté → `ca_eduroam.h` n'est plus le bon ; encore refusé → identifiant (`prenom.nom@etu.univ-amu.fr`, en entier) ou mot de passe.

### Interrupteurs de compilation (début de `main.cpp`)

| Constante | Défaut | Rôle |
|---|---|---|
| `WIFI_ACTIVE` | 1 | 0 = hors ligne (test du stockage) |
| `AIRCARTO_HTTPS` | 1 | 1 = mesures envoyées en HTTPS (clé chiffrée) ; 0 = HTTP simple (clé lisible sur le réseau) |
| `EDUROAM_IDENTITE_ANONYME` | 1 | eduroam : `anonymous@domaine` circule en clair à la place de ton identifiant ; 0 si l'université le refuse |
| `MODE_DEMO` | 0 | 1 = fausses valeurs qui défilent, aucun envoi |
| `MHZ_ACTIVE` / `MHZ_ABC` | 1 / 1 | Capteur CO₂ et auto-calibration |
| `NEXTPM_MOYENNE_60S` | 0 | Moyennes NextPM sur 10 s (0) ou 60 s (1) |
| `ECRAN_ETEINT_NUIT` | 0 | Écran éteint la nuit |
| `DOUBLE_BUFFER` | 1 | Dessin sans scintillement |
| `TRANSITION_FONDU` | 1 | Fondu au noir entre les pages (~0,4 s ; ~0,15 s vers / depuis la page ÉTAT), sans bloquer la carte |
| `ANIMATIONS` | 0 | Clignements, chiffres qui glissent |
| `AFFICHER_ETAT` | 1 | Page ÉTAT (heure + icônes Wi-Fi / serveur / capteurs) toutes les 3 pages, pendant 3 s |
| `CONSOLE_DISTANTE` | 1 | Journal, commandes et mises à jour par le relais Cloudflare |
| `BLUETOOTH_ACTIF` | 1 | Console Bluetooth (code d'appairage affiché sur l'écran) |

### Écran LED

- **Démarrage :** logo animé « LA BONNE M’AIR ».
- **Pages (une mesure par écran, 8 s chacune en pleine lumière, fondu au noir de ~0,4 s entre deux) :** CO₂ · AÉRER (si CO₂ > 1 000 ppm) · PM1 · PM2.5 · PM10 · indice ATMO · courbe du CO₂ (dernière heure) · Wi-Fi (seulement hors ligne).
- **Page de mesure :** nom et unité en haut, filet à la couleur du niveau, valeur en grand avec un visage qui change d’expression, puis la **jauge des niveaux ATMO**, fine (6 segments pour les PM, 4 pour le CO₂) : le niveau atteint est allumé, les autres restent en sourdine, et un **curseur** ▲ détaché de la jauge, à la couleur du niveau, indique la position exacte de la valeur dans son niveau.
- **Page ÉTAT (toutes les 3 pages, pendant 4 s, fondu très court de ~0,15 s) :** heure et date, puis trois grandes icônes colorées (vert = OK, orange = à surveiller, rouge = problème) : **Wi-Fi** (arcs selon le signal, en dBm), **serveur** (nuage : envoi OK, nombre de mesures en attente, clé refusée) et **capteurs** (ventilateur : OK, préchauffage, panne, à calibrer). Réglable avec `PERIODE_ETAT_MS` et `DUREE_ETAT_MS`.
- **Couleurs :** chaque valeur prend la couleur de son niveau ATMO.
- **Alerte hors norme :** dès qu’un polluant atteint son seuil d’alerte (PM : « Mauvais », CO₂ : > 1 000 ppm), l’écran saute aussitôt sur sa page, clignote à la couleur du niveau, puis la valeur pulse ; la page revient une fois sur deux tant que l’alerte dure.
- **Luminosité adaptée :** 10 niveaux réglables (`lum`), un pour le jour et un pour la nuit (22 h → 7 h par défaut, `nuit`), bridée sur batterie.
- **Préchauffage du CO₂ :** compte à rebours en minutes:secondes (« 2:18 ») les 3 premières minutes après une mise sous tension (pas après un redémarrage logiciel : le capteur est resté alimenté).
- **Bluetooth / mise à jour :** pendant un appairage, l'écran affiche le code à 6 chiffres à taper sur le téléphone ; pendant une mise à jour, « MISE A JOUR » et le pourcentage téléchargé (même la nuit, écran éteint). L'emplacement du nouveau programme est effacé d'un coup avant le téléchargement (écran noir 2 à 5 s) : plus de clignotement pendant la progression.
- **CO₂ impossible :** une lecture sous 300 ppm est écartée (un capteur qui dérive a envoyé jusqu'à 37 ppm) ; si ça dure, la page CO₂ affiche « A CALIBRER » (commande `calib oui`, à l'air libre).

### Données envoyées

Les deux capteurs sont lus **chaque seconde** (ils mesurent eux-mêmes une fois par seconde : lire plus vite ne donnerait que des doublons). Chaque envoi contient la **moyenne et le pic de toutes les lectures de la période** :

| Situation | Période d'une mesure |
|---|---|
| En direct (Wi-Fi + clé valide, rien en retard) | **10 s** — bouclée au moment exact où le serveur accepte l'envoi suivant (10 s + une petite marge), puis envoyée aussitôt |
| Hors ligne (ou clé absente / refusée) | 30 s — la mémoire flash garde ainsi ~10 h (3 h seulement à 10 s) |
| Quelques mesures en retard / plus de 10 en retard | 30 s / 60 s — laisse des créneaux au rattrapage (le serveur n'accepte qu'un envoi toutes les 10 s) |
| Sur batterie | 60 s |

Pourquoi « 10 s ou un peu plus » en direct : le serveur refuse deux envois à moins de 10 s d'écart. Une mesure produite toutes les 10 s pile arriverait un peu plus vite qu'il ne les accepte et elles s'empileraient. La carte boucle donc la mesure quand le créneau s'ouvre : elle couvre 10 s (parfois 10,5 s), aucune lecture n'est perdue, et `x_age_s` vaut 0 à 2 s. Vérifié en simulation : une mesure toutes les ~11 s en moyenne en direct, aucune perte.

**CO₂ très élevé :** au-dessus de **3 000 ppm**, une lecture n'est prise en compte qu'une fois **confirmée par 5 lectures de suite** (5 s) au-dessus du seuil et proches les unes des autres (± 10 % de leur médiane). Un pic isolé ou des valeurs qui sautent sont ignorés (compteur dans `status`). Réglage : `CO2_CONFIRMATION_PPM`, `CO2_NB_CONFIRMATION`, `CO2_ECART_CONFIRMATION`.

Adresse d'envoi : `https://api.aircarto.fr/iut-amu/push` (HTTPS : la clé du groupe ne circule plus en clair ; seules les deux racines Let's Encrypt d'AirCarto sont acceptées, et la date de chaque certificat est vérifiée). En cas de souci, l'interrupteur `AIRCARTO_HTTPS 0` (début de `main.cpp`) revient au HTTP simple, que le serveur accepte aussi.

| Champ | Contenu |
|---|---|
| `device_id`, `uptime`, `rssi`, `heap` | Identité et santé de la carte |
| `npm_status` | Octet d’état du NextPM (champ officiel d'AirCarto) |
| `co2`, `pm1`, `pm25`, `pm10` | Moyennes sur la période |
| `x_age_s` | Secondes entre la mesure et son envoi (mesures rejouées) |
| `x_co2_max`, `x_pm25_max` | Pics sur la période |
| `x_aeration` | 1 = chute rapide du CO₂ (fenêtre ouverte ?) |
| `x_pct_co2_1000` | % du temps au-dessus de 1 000 ppm |
| `x_reset` | Cause du dernier redémarrage |
| `x_alim` | 1 = secteur, 0 = batterie (toujours 1 tant que la détection secteur est désactivée) |
| `x_temp_pm`, `x_hum_pm` | Température / humidité internes du NextPM |

### Robustesse

- **Mode hors ligne :** jusqu’à ~400 Ko (~10 h) de mesures stockées en flash, renvoyées automatiquement (le serveur impose 10 s entre deux envois : ~100 mesures en retard se rattrapent en ~20 min).
- **Tâches séparées :** l’envoi réseau tourne dans sa propre tâche ; l’écran est rafraîchi sur le cœur 1, à l'écart du Wi-Fi et du Bluetooth (cœur 0) dont les interruptions faisaient scintiller l'écran au démarrage et pendant une mise à jour.
- **Sécurités :** watchdog (60 s), redémarrage si la mémoire libre passe sous 25 Ko ou si elle est trop morcelée (plus grand bloc sous 6 Ko, 10 Ko sans réserve HTTPS), heure NTP avec passage heure d’été / d’hiver.
- **Mémoire HTTPS réservée :** les deux tampons de ~16,7 Ko d'une connexion chiffrée sont pris une fois pour toutes au démarrage et prêtés à mbedTLS à chaque échange. Avant, pris et rendus toutes les 10 à 30 s, ils morcelaient la mémoire (« échange reporté, mémoire insuffisante », « X509 - A fatal error »). `status` affiche la réserve et la pile restante de chaque tâche.
- **Wi-Fi :** chaque réseau est essayé **deux fois** (2 s d'écart) : après un redémarrage de la carte, beaucoup de box refusent le premier essai (elles gardent l'ancienne connexion) et acceptent le second ; avant, il fallait taper `wifi connect`. Le Bluetooth démarre avant le Wi-Fi (radio partagée). Les essais ratés en arrière-plan sont notés au journal (une ligne par raison, au plus toutes les 10 min).
- **Wi-Fi bloqué :** la carte ne redémarre que si un réseau connu est **visible** depuis 1 h sans que la connexion réussisse (pile Wi-Fi bloquée) ; une borne éteinte la nuit ne provoque plus de redémarrage toutes les heures.
- **Envoi reporté sans fausse alerte :** une mesure qui n'a pas pu partir pour une raison passagère (connexion HTTPS du relais en cours, mémoire, refus 429 « trop tôt ») attend en mémoire vive le créneau suivant au lieu d'aller en flash ; le nuage de la page ÉTAT n'affiche plus « 1 » alors que la carte est connectée. Les 10 s du serveur sont comptées depuis la fin de la poignée de main HTTPS (son arrivée réelle au serveur), ce qui évite les refus 429.
- **Anti-blocage des envois** (pannes trouvées en simulation sur plusieurs jours) :
  - clé refusée (401) : nouvel essai toutes les 15 min, au lieu d'un arrêt définitif jusqu'au débranchement ;
  - Wi-Fi « connecté » mais plus rien ne passe : reconnexion après 5 envois sans réponse, redémarrage après 6 h sans aucune réponse du serveur ;
  - délai d'attente demandé par le serveur (`next_push_in_s`) plafonné à 5 min.

### Horodatage des mesures (coupures Wi-Fi)

Le serveur AirCarto date chaque mesure à sa **réception** (champ `time`). Une mesure gardée en flash pendant une coupure arriverait donc à la mauvaise heure. La carte joint à chaque envoi `x_age_s`, le nombre de secondes écoulées entre la mesure et son envoi, et le site calcule : **heure réelle = `time` − `x_age_s`**.

- À la fin de chaque période (10 s en direct, 30 s hors ligne), la mesure est notée avec trois repères internes, retirés avant l'envoi : `_u` (uptime en s), `_e` (heure Unix si l'heure NTP est connue, sinon 0) et `_b` (numéro aléatoire du démarrage).
- Au moment de l'envoi, la carte calcule l'âge. Si c'est le même démarrage : uptime actuel − `_u` (marche même sans heure NTP). Sinon : heure actuelle − `_e`. Si `_e` vaut 0, la mesure ne peut pas être datée et elle est abandonnée.
- L'heure survit à un redémarrage logiciel de l'ESP32 (minuterie RTC), pas à une coupure de courant : après une coupure de courant, la carte attend l'heure NTP (15 min au plus) avant de renvoyer les mesures du démarrage précédent.
- Vues 6 h et 24 h, export CSV jusqu'à 7 jours : le site travaille sur les **mesures brutes** et corrige chacune.
- Vue 7 jours, durées personnalisées de plus de 24 h et export au-delà de 7 jours : le site reçoit des moyennes calculées par le serveur **par heure de réception**, `x_age_s` compris, et datées à la fin de leur tranche. Pendant un rattrapage, une tranche mélange des mesures rejouées et des mesures fraîches. Le site repère ces tranches à leur `x_age_s` moyen (plus de 30 s, alors qu'un envoi en direct a 0 à 2 s), redemande pour elles seules les mesures brutes et remet chacune à son heure. Les autres moyennes sont ramenées au début de leur tranche.
- Le site écarte aussi les mesures reçues deux fois (même heure réelle à ±5 s, mêmes valeurs).
- Précision vérifiée en simulation : **±1 s**.

---

## Site web

Page unique, sans framework, hébergée sur **GitHub Pages** (branche `main`, racine du dépôt). Les graphiques utilisent **Chart.js 4.4.1**.

### Fonctionnalités

- **Plan de la page (00 à 04)** : 00 introduction (nom, affiche de la Bonne Mère, manifeste, points forts) · 01 accueil, verdict « L’air est… » · 02 dernière mesure · 03 graphiques · 04 comprendre les mesures.
- **Défilement par crans** dans l’introduction (molette et clavier, ordinateur) : un cran mène à l’étape suivante de la scène, on ne s’arrête jamais au milieu d’un fondu. Au doigt, le défilement reste libre.
- **Verdict en direct** « L’air est… » avec bandeau qui prend la couleur du niveau ATMO ; **mode jeu** avec les particules (plein écran, boutons − / + pour leur nombre et leur vitesse).
- **Cartes de mesure** PM1, PM2.5, PM10, CO₂, température, humidité : dernière valeur, **incertitude du capteur** (fiches techniques du NextPM et du MH-Z19C, bande hachurée sur la jauge), jauge colorée, min / max de la période.
- **Graphiques interactifs :** périodes 6 h / 24 h / 7 j ou **durée personnalisée** (5 min à 31 jours), choix du **pas de moyenne**, séries à cocher, **seuils en pointillés** (l’axe s’étire pour montrer un seuil proche ; un seuil trop loin est signalé par « ↑ » ou « ↓ »), lecture au survol ou au doigt.
- **Heure réelle des mesures :** les mesures renvoyées après une coupure sont replacées à leur heure grâce à `x_age_s`.
- **Export CSV** (dans la barre collante, qui suit le défilement) : période affichée, dernière heure, 6 h, 24 h, 7 j, 30 j, **tout l'historique** ou **dates choisies dans un calendrier** (jours blancs = mesures disponibles, jours noirs = aucune mesure) ; mesures brutes (31 jours au plus par fichier) ou moyennées (5 min, 15 min, 1 h, 1 j) ; format Excel France (`;`) ou standard (`,`). Les longues périodes sont téléchargées par tranches (le serveur limite chaque requête à 7 jours en brut et 31 jours en moyennes).
- **Section « Comprendre les mesures »** : tailles des particules à l’échelle (avec barre d’échelle), incertitude des mesures, tableau des seuils, explication du calcul de l’indice et sources.
- **3 langues** : français, anglais, japonais (choix mémorisé).
- **Mémoire du navigateur** (`localStorage`, rien n’est envoyé) : dernière période choisie (6 h, 24 h, 7 j ou durée personnalisée), pas des graphes, courbes cochées, langue. Une fois l’introduction vue (barre des mesures atteinte), les visites suivantes s’ouvrent directement sur l’accueil (01) ; on la revoit avec **00** dans le sommaire (ordinateur) ou **Introduction** (téléphone).
- **Barre des réglages** collée en haut ; sur téléphone, elle se range quand on descend et revient dès qu’on remonte.
- **Confort :** écran d’entrée plein écran, bouton flottant « Aller aux mesures » pour sauter l’introduction, animations réductibles, mise à jour automatique toutes les 5 min.
- **Logo LED de la carte** (`js/logo-led.js`, même dessin pixel par pixel que l’animation de démarrage de la carte et que la console) :
  - **onglet animé** : au chargement, la basilique monte, « M’AIR » s’écrit, la barre des niveaux se remplit, la statue scintille (puis toutes les 9 s) ; dès qu’une mesure arrive, la barre n’allume que le niveau ATMO du verdict, comme la jauge de la carte ;
  - **dans la page**, le même logo animé partout (rien d’animé si les animations sont réduites) :
    - **écran d’entrée** et **pied de page** : logo carré qui s’allume en arrivant à l’écran ;
    - **introduction** : panneau de LED 64 × 32 au-dessus du nom, qui s’allume quand l’introduction se découvre (écran d’entrée fermé, ou tout de suite sur iPhone) et se rallume quand on remonte jusqu’à lui ;
    - **chargement** (visites suivantes, la page s’ouvre sur les mesures) : le logo s’allume en plein écran le temps que les mesures arrivent (un clic ou une touche le passe, 12 s au plus) ;
    - **« Dernière mesure » en attente** : le panneau se rallume en boucle ;
    - au repos, la statue scintille de temps en temps ;
  - **icônes fixes** (ordinateur, tablette, téléphone) : `favicon.svg`, `favicon.ico` (16, 32, 48 px), `icone-192.png` / `icone-512.png` (Android), `apple-touch-icon.png` (iPhone, iPad), fabriquées par `outils/icones.html`.
- **Pied de page :** le trajet détaillé d’une mesure en 5 étapes (capteurs → carte → serveur AirCarto → relais Cloudflare → cette page), la veille GitHub Actions et la console à côté, puis les mesures (grandeurs, cadence, incertitude) et toutes les sources avec liens (FR, EN, JA).
- **Mise à jour légère :** toutes les 5 min, le site ne redemande que les mesures arrivées depuis la requête précédente (quelques Ko au lieu de ~2,4 Mo pour 24 h ; Cloudflare compresse au passage ce que l'API AirCarto envoie brut) ; le résultat est identique à un rechargement complet.
- **Valeurs impossibles écartées :** CO₂ hors 300-5 000 ppm, etc. (le serveur garde d'anciennes valeurs fausses d'un capteur qui dérivait).

### Réglages (`index.html`, section « 1) RÉGLAGES »)

| Constante | Rôle |
|---|---|
| `URL_API` | Adresse du relais Cloudflare (`…/donnees`) : la clé AirCarto n’est plus dans la page |
| `DEVICE_ID` | Adresse MAC d’une carte précise, ou `""` pour toutes |
| `RAFRAICHISSEMENT_MS` | Intervalle de mise à jour (5 min) |

### Traductions

Les textes sont dans `lang/*.js` ; le HTML les référence par `data-i18n="clé"`.
Pour ajouter une langue : copier `lang/fr.js` en `lang/xx.js`, traduire les valeurs (jamais les clés), l’inclure dans `index.html` avant `js/i18n.js` et l’ajouter dans `LANGUES` (`js/i18n.js`).
Pour vérifier qu’aucune clé ne manque : `I18N.verifier()` dans la console du navigateur.

### Tester en local

```bash
python -m http.server 8000
# puis ouvrir http://localhost:8000
```

---

## Seuils et indice ATMO

Les mêmes seuils sont codés dans la carte (`SEUILS_PM25`, `SEUILS_PM10`, `SEUILS_CO2`) et dans le site (`SEUILS`).

### Particules (µg/m³) — indice ATMO

| Niveau | PM2.5 (et PM1) | PM10 |
|---|---|---|
| 🟦 Bon | 0 – 10 | 0 – 20 |
| 🟩 Moyen | 11 – 20 | 21 – 40 |
| 🟨 Dégradé | 21 – 25 | 41 – 50 |
| 🟥 Mauvais | 26 – 50 | 51 – 100 |
| 🟪 Très mauvais | 51 – 75 | 101 – 150 |
| 🟣 Extrêmement mauvais | > 75 | > 150 |

### CO₂ (ppm) — indicateur d’aération

| Niveau | CO₂ |
|---|---|
| Bon | 0 – 800 |
| Moyen | 801 – 1 000 |
| Dégradé (page « AÉRER ») | 1 001 – 1 500 |
| Mauvais (buzzer si câblé) | > 1 500 |

**Règle aux limites (carte et site) :** une valeur pile sur un seuil reste dans le niveau le plus **bas** (PM2.5 = 10 → Bon, CO₂ = 800 → Bon), et la valeur est comparée **arrondie à l'unité** (PM2.5 = 10,4 → 10 → Bon ; 10,5 → 11 → Moyen).

### Calcul du verdict

On applique la règle officielle de l’indice ATMO : chaque polluant reçoit un **sous-indice** sur sa propre échelle, et le verdict est le **plus dégradé** des sous-indices — **ni moyenne, ni pondération**.

- Entrent dans le verdict : **PM2.5, PM10 et PM1** (l’indice ATMO ne fixant pas de seuil pour les PM1, on leur applique ceux des PM2.5).
- Le **CO₂** n’est pas un polluant de l’indice : il est affiché à part comme indicateur d’aération.
- L’indice ATMO complet inclut aussi l’ozone, le NO₂ et le SO₂ (non mesurés ici) et porte sur des moyennes **journalières** : notre verdict est donc un **indice partiel et indicatif**.

---

## Alertes mail « capteur muet » et « CO₂ à vérifier »

Le workflow `.github/workflows/surveillance-capteur.yml` interroge le serveur AirCarto **toutes les 5 minutes**.
Si aucune mesure n’est arrivée depuis `SEUIL_MIN` minutes (10 par défaut), il ouvre une issue **« Capteur muet »** qui mentionne le propriétaire du dépôt → GitHub envoie **un mail**. Quand les mesures reviennent, l’issue se ferme toute seule (**un** second mail). GitHub n’écrit que « Closed » dans ce mail : tout le détail utile est donc dans son **objet** (« ✅ Capteur revenu après 47 min de silence (22/11 · 14:02 → 14:49) »), et le compte rendu complet (heures, durée, conseils pour les graphiques) est ajouté à l’issue sur GitHub. L’issue d’alerte contient aussi un tableau récapitulatif et une liste « À vérifier ».

Si la carte envoie bien ses mesures (particules) mais **aucun CO₂ valable** (300 à 5 000 ppm) depuis `SEUIL_CO2_MIN` minutes (60 par défaut), il ouvre de la même façon une issue **« CO₂ à vérifier »** : « CO₂ absent » si le capteur ne répond plus, « CO₂ à recalibrer » s’il ne donne que des valeurs impossibles. L’issue indique quoi vérifier (commande `status`, câblage du MH-Z19C, `calib oui` à l’air libre) et se ferme toute seule dès qu’un CO₂ valable revient. Pas d’alerte CO₂ juste après un démarrage, ni quand la carte est déjà muette (une seule alerte à la fois).

Réglages dans **Settings → Secrets and variables → Actions** :

| Nom | Type | Rôle |
|---|---|---|
| `AIRCARTO_CLE` | secret | Clé du groupe (facultatif : sinon les mesures sont lues par le relais Cloudflare) |
| `SEUIL_MIN` | variable | Minutes sans mesure avant l’alerte (défaut 10) |
| `DEVICE_ID` | variable | Surveiller une seule carte (défaut : toutes) |
| `SEUIL_CO2_MIN` | variable | Minutes sans CO₂ valable avant l’alerte CO₂ (défaut 60) |

Test manuel : onglet **Actions → Surveillance du capteur → Run workflow**.
GitHub peut retarder les tâches planifiées de quelques minutes, et les met en pause après 60 jours sans commit sur un dépôt public.

---

## Limites connues

- Capteurs à bas coût : mesures indicatives, sans valeur réglementaire.
- Le MH-Z19C a besoin d’environ **3 minutes de chauffe** ; l’auto-calibration suppose que la pièce est aérée régulièrement.
- Température et humidité proviennent des capteurs internes du NextPM : elles reflètent l’air dans le boîtier, souvent un peu plus chaud que la pièce.
- Le serveur n’accepte qu’**un envoi toutes les 10 s** : c’est lui qui limite la vitesse de rattrapage après une coupure.
- Carte démarrée **sans Wi-Fi** (donc sans heure) puis coupure de courant avant le retour du Wi-Fi : les mesures de ce démarrage ne peuvent pas être datées et sont abandonnées (la commande `status` les compte). Une horloge à pile (module RTC DS3231) l'éviterait.
- Mise à jour à distance : la toute première installation (clé publique + nouvelle table de partitions) se fait au câble USB. Une nouvelle version jamais validée (pas de relais joint) revient à l'ancienne après 3 redémarrages de suite, même si c'étaient de simples coupures de courant.
- Console Bluetooth et HTTPS prennent de la mémoire vive : la commande `status` affiche la mémoire libre. En cas de manque, mettre `BLUETOOTH_ACTIF` à 0.

---

## Sources

- Atmo France — [L’indice ATMO](https://www.atmo-france.org/article/lindice-atmo) (seuils et couleurs des particules)
- Arrêté du 10 juillet 2020 relatif à l’indice de la qualité de l’air ambiant
- HCSP — recommandations sur l’aération et la ventilation (repères CO₂)
- Atmo France — [guide de calcul de l’indice ATMO](https://www.airpl.org/sites/default/files/reports/guide_calcul_nouvel_indice_ATMO_VF_version14decembre2020%281%29.pdf) (décembre 2020) · [texte de l’arrêté](https://aida.ineris.fr/reglementation/arrete-100720-relatif-a-lindice-qualite-lair-ambiant)
- HCSP — [avis du 28 avril 2021](https://www.hcsp.fr/Explore.cgi/Telecharger?NomFichier=hcspa20210428_covaravenetmesducodanleserp.pdf) (aération, ventilation, mesure du CO₂ ; 800 ppm)
- Tera Sensor — [fiche technique NextPM](https://tera-sensor.gitbook.io/tera-sensor/sensors/nextpm/nextpm-datasheet) (limite de détection < 1 µg/m³, linéarité < 5 %, répétabilité < 3 %) · [évaluation AQ-SPEC, rapport de synthèse PDF](https://www.aqmd.gov/docs/default-source/aq-spec/summary/tera-sensor-nextpm---summary-report.pdf?sfvrsn=6)
- Winsen — [fiche technique MH-Z19C](https://www.winsen-sensor.com/product/mh-z19c.html) (±(50 ppm + 5 % de la valeur lue))
- [AirCarto](https://aircarto.fr) — serveur de collecte des mesures (`api.aircarto.fr`), lu par le site via le relais Cloudflare `mair-relais.astraor.workers.dev` ; projet [ModuleAir](https://github.com/aircarto/ModuleAir_V2), point de départ de la carte
