# Console sans fil, mises à jour à distance et clé AirCarto cachée

Deux façons de parler à la carte **sans câble USB** :

| | Par Internet (relais Cloudflare) | Par Bluetooth |
|---|---|---|
| Où | N'importe où, eduroam compris (HTTPS sortant uniquement) | À une dizaine de mètres de la carte |
| Journal | Gardé 7 jours (5 000 lignes), presque en direct quand la page est ouverte ; un intertitre par jour (« Aujourd'hui », « Hier », date), date complète au survol de l'heure et dans « Copier » | En direct |
| Commandes | Liste blanche (ci-dessous) | Même liste blanche |
| Mise à jour du programme | Oui (`maj installer`, signée) | Se lance aussi par Bluetooth ; le programme est toujours téléchargé par le Wi-Fi, via le relais |
| Il faut | Un compte Cloudflare gratuit | Une appli « terminal BLE » |

**Commandes acceptées sans câble :** `aide`, `status`, `verbose on`, `verbose off`, `calib`, `reboot`, `wifi scan`, `wifi connect`, `maj`, `maj installer`, `diag tls`. Une par seconde au plus ; `wifi scan`, `wifi connect` et `maj` une fois par 30 s, `reboot` et `diag tls` une fois par 5 min. Dans la page console, chacune a son bouton (plus de saisie de texte). `calib oui` (qui fausserait les mesures de CO₂ s'il était lancé par erreur ou par un intrus) est réservé au câble USB.
**Toujours au câble USB :** `wifi add/ent/del/list`, `set cle`, `set jeton`, `set relais`, `calib oui`, `oublier`, `vider_file`, `bt appairer`, `bt demarrage`, `bt liste`, `bt oublier`. La carte les refuse d'elle-même quand elles arrivent par Bluetooth ou par Internet, et ce qu'elles affichent ne quitte jamais le câble USB.

---

## 1. À faire une seule fois

### a. Clé de signature des mises à jour (sur ton PC)

Dans VS Code, terminal PlatformIO, dossier `Firmware/` :

```
pio run -t cles
```

- crée `Firmware/outils/cle_privee_ota.pem` : ta **clé privée**. Elle n'est **jamais** envoyée sur GitHub (`.gitignore`). **Garde-en une copie** (clé USB, gestionnaire de mots de passe) : perdue, il faudra une nouvelle clé et un flash USB ;
- crée `Firmware/src/cle_ota.h` : la clé **publique**, compilée dans la carte (pas secrète, tu peux la committer) ;
- propose deux secrets au hasard : le **jeton de la carte** et le **mot de passe de la console**. Note-les.

### b. Le relais Cloudflare (gratuit)

1. Crée un compte sur [dash.cloudflare.com](https://dash.cloudflare.com) et active la **validation en deux étapes**.
2. Installe Node.js (LTS) si besoin, puis, dans le dossier `relais/` :
   ```
   npx wrangler login
   npx wrangler deploy
   npx wrangler secret put JETON_CARTE
   npx wrangler secret put MOT_DE_PASSE_CONSOLE
   npx wrangler secret put AIRCARTO_CLE
   ```
   (`wrangler secret put` te demande la valeur : colle le jeton, le mot de passe, puis la clé du groupe AirCarto.)
3. `wrangler deploy` affiche l'adresse du relais, du genre `https://mair-relais.<ton-nom>.workers.dev`.
   Ouvre-la : c'est la **page console**.

**Sans aucune commande (tout dans le navigateur, méthode utilisée ici) :**

1. *Secrets* : dash.cloudflare.com → **Workers & Pages** → `mair-relais` → **Settings** → **Variables and Secrets** →
   **+ Add**, **Type : Secret** (jamais « Text »), nom `JETON_CARTE` puis `MOT_DE_PASSE_CONSOLE` → **Deploy**.
   Si tu as cliqué sur **Save** : la version est seulement enregistrée ; onglet **Deployments** → la déployer à 100 %.
2. *Déploiement automatique depuis GitHub* : **Settings** → **Builds** → **Connect** → dépôt
   `Astraor/0Capteur_Interne_Qualite_Air`, branche `main`, Build command vide, Deploy command `npx wrangler deploy`,
   **Preview builds décochés**, **Path : `/relais`**, **Include paths : `relais/*`**.
   Ensuite, chaque push qui modifie `relais/` redéploie le relais tout seul (onglet **Deployments** pour suivre). Les
   secrets restent en place d'un déploiement à l'autre ; `wrangler.toml` (dans le dépôt) fixe le reste.

### c. Le site lit les mesures par le relais

La clé du groupe AirCarto était écrite dans `index.html`, donc lisible par tous sur GitHub et sur le site. Or elle sert
aussi à **envoyer** des mesures : n'importe qui pouvait en ajouter de fausses au nom du groupe. Maintenant :

- le site demande les mesures à `https://mair-relais.<ton-nom>.workers.dev/donnees` (ligne `const URL_API` de `index.html`) ;
- le relais n'accepte que des paramètres de **lecture** (`start`, `end`, `freq`, `device_id`), ajoute la clé (secret
  `AIRCARTO_CLE`) et renvoie la réponse d'AirCarto ; il ne répond qu'à ton site (`ORIGINES_SITE` dans `wrangler.toml`) ;
- la surveillance GitHub Actions passe aussi par le relais (ou par le secret GitHub `AIRCARTO_CLE` s'il existe).

⚠️ **L'ancienne clé reste visible dans l'historique de GitHub** (anciens commits). Retirer la ligne ne suffit pas :
demande une **nouvelle clé** à AirCarto, puis `set cle <nouvelle>` au câble USB et `npx wrangler secret put AIRCARTO_CLE`.
L'ancienne devient inutile, même pour ceux qui l'ont copiée.

### d. Dernier flash au câble USB

1. `PlatformIO: Upload` (le programme contient maintenant la clé publique et la nouvelle table de partitions).
   ⚠️ La table de partitions change (deux emplacements de 1,5 Mo pour le programme) : les mesures en attente dans la carte sont effacées **une fois**. Vide la file avant si besoin.
2. Dans le moniteur série :
   ```
   set relais https://mair-relais.<ton-nom>.workers.dev
   set jeton <le jeton noté à l'étape a>
   status
   ```
   `status` doit afficher « console à distance : … dernier échange il y a … s ».

### e. Bluetooth

1. Installe une appli : **Serial Bluetooth Terminal** (Android) ou **Bluefruit Connect** / **nRF Toolbox → UART** (iPhone).
2. Au câble USB, tape `bt appairer`, puis dans les **2 minutes** connecte-toi à **MAir-XXXX** (les 4 derniers caractères de l'adresse de la carte). En dehors de ces 2 minutes, un nouveau téléphone est refusé ; ceux déjà appairés se reconnectent quand ils veulent. Sans câble sous la main : `bt demarrage on` (une fois, au câble USB) ouvre aussi ces 2 minutes à chaque mise sous tension (débrancher / rebrancher la carte).
3. L'écran de la carte affiche **BLUETOOTH** et un **code à 6 chiffres** : tape-le sur le téléphone.
4. Tape `status`. Le téléphone est mémorisé : pas de code les fois suivantes. Au câble USB : `bt liste` montre les téléphones appairés, `bt oublier <n>` en retire un, `bt oublier` les retire tous.

---

## 2. Publier une mise à jour

1. Modifie le code, puis **Build**. La fin de la compilation affiche :
   `OTA : version 2611201530 signée -> Firmware/ota/firmware.bin + firmware.json`
2. Sur GitHub : **Releases → Draft a new release**
   - tag : celui affiché (ex. `v2611201530`) ;
   - glisse **les deux fichiers** `Firmware/ota/firmware.bin` et `Firmware/ota/firmware.json` ;
   - **Publish release** (pas « pre-release »).
3. Dans la page console : « Publiée : 2611201530 (plus récente que la carte) » → **Installer sur la carte** (ou commande `maj installer`).
4. La carte télécharge, vérifie la signature et l'empreinte, s'installe dans l'autre emplacement et redémarre. L'écran affiche **MISE A JOUR xx %**.
5. Elle se valide toute seule après 1 min de fonctionnement + un échange avec le relais (« Nouvelle version … validée »).
   Sinon, elle **revient à l'ancienne version** : après 3 redémarrages de suite, ou après 10 min de Wi-Fi sans joindre le relais.

Le numéro de version (AAMMJJhhmm) est fabriqué à la compilation, d'après la date de ta dernière modification du code : la carte refuse une version plus ancienne ou égale à la sienne.

---

## 3. Sécurité : ce qui protège quoi

| Risque | Protection |
|---|---|
| Quelqu'un fabrique un faux programme | Signature ECDSA P-256 : la carte n'installe que ce qui est signé par **ta** clé privée (restée sur ton PC). Même un compte GitHub ou Cloudflare piraté ne suffit pas |
| Fichier abîmé ou modifié en route | Empreinte SHA-256 signée, vérifiée avant d'activer la nouvelle version |
| Retour à une vieille version vulnérable | Numéro de version signé : la carte refuse un numéro plus petit ou égal |
| Nouvelle version qui plante | Ancienne version gardée intacte, retour automatique |
| Faux relais (Wi-Fi piégé) | HTTPS avec vérification du certificat, du nom du serveur **et de sa date** (racines de Google Trust Services, Let's Encrypt, SSL.com, Sectigo : `racines_tls.h`) ; une seule connexion chiffrée à la fois, fermée après chaque échange |
| Quelqu'un tape des commandes à distance | Mot de passe de console ; 10 essais faux = 15 min de blocage ; liste blanche sur le relais **et** sur la carte |
| Fuite des mots de passe Wi-Fi / de la clé | Jamais saisis ni affichés autrement qu'au câble USB |
| Bluetooth : un voisin se connecte | Nouveau téléphone accepté seulement 2 min après `bt appairer` (USB), ou après une mise sous tension si `bt demarrage on` ; `status` donne le nombre de téléphones appairés ; code à 6 chiffres affiché sur l'écran ; appairage chiffré « Secure Connections » seulement ; rien n'est envoyé ni accepté avant ; connexion non appairée coupée après 90 s |
| Commandes à distance en boucle | Une par seconde ; `wifi …` et `maj` une fois par 30 s ; `reboot` une fois par 5 min ; `calib oui` au câble USB seulement |
| Annuler une mise à jour à distance (`reboot` pendant l'essai) | `reboot` et `maj` refusés à distance pendant l'essai d'une nouvelle version (environ 1 min) ; une version qui redémarre en boucle n'est plus réinstallée à distance (publier une version corrigée, ou `maj oublier` au câble USB) |
| Nouvelle version qui casse le Wi-Fi | Sans aucun échange avec le relais en 20 min, Wi-Fi ou pas, retour automatique à l'ancienne version (aussi si le réseau tombe juste après une mise à jour : la version n'est alors pas refusée, `maj installer` la réinstalle) |
| Couper la carte du relais depuis le même réseau (eduroam) | Le jeton de la carte est testé AVANT le blocage par adresse : 10 faux jetons depuis l'adresse partagée bloquent les fraudeurs, pas la vraie carte, qui n'est pas non plus soumise à la limite de débit par adresse |
| Mot de passe de console essayé depuis des milliers d'adresses | Alerte `[sécurité]` dans le journal au-delà de 50 secrets refusés en 1 h ; essais comptés avant toute attente (une rafale ne dépasse pas 10 essais) ; une session ouverte reste utilisable depuis une adresse bloquée |
| Clé AirCarto lue sur le réseau | Envoi des mesures en HTTPS, certificat d'AirCarto vérifié |
| Faux point d'accès eduroam | Certificat du serveur de l'université (`ca_eduroam.h`, voir l'exemple) : sans lui, ton mot de passe ENT peut être capturé |
| Nom de Wi-Fi piégé (retours à la ligne...) | Caractères de contrôle remplacés par `?` ; une ligne de la carte ne peut pas imiter une trace du relais (préfixe « carte : ») |
| Clé AirCarto lue sur GitHub | Plus écrite dans le site : seul le relais la connaît (secret Cloudflare). Change-la (nouvelle clé AirCarto) : l'ancienne reste dans l'historique git |
| Carte volée | `oublier` à distance impossible (exprès) ; change le jeton (`wrangler secret put JETON_CARTE`) et ton mot de passe eduroam |

À faire de ton côté : validation en deux étapes sur **GitHub** et **Cloudflare**, mot de passe de console long (celui proposé par `pio run -t cles`), copie de sauvegarde de la clé privée (`Firmware/outils/cle_privee_ota.pem`, jamais dans git ; garde la copie hors d'un dossier synchronisé comme OneDrive).

---

## 4. Limites de l'offre gratuite Cloudflare

100 000 requêtes par jour, partagées entre la carte, la console et les visiteurs du site (une dizaine de requêtes à
l'ouverture du site, puis une toutes les 5 min). La carte en fait ~3 000 par jour au repos (une toutes les 30 s), et ~1 800 par heure tant que la page console est ouverte (une toutes les 2 s, plus celles de la page). Quand l'onglet est caché, la page s'arrête de demander et la carte revient à 30 s.

## 5. Fichiers

```
relais/
├── wrangler.toml        # configuration (nom, dépôt GitHub, base du relais)
├── src/index.js         # le relais (Worker + Durable Object SQLite)
└── src/console.html     # la page console
Firmware/
├── partitions_mair.csv  # 2 × 1,5 Mo pour le programme + 896 Ko de mesures
├── outils/ota.py        # version, signature, « pio run -t cles »
└── src/racines_tls.h    # certificats acceptés pour joindre le relais
```
