// Surveillance du capteur : lancé toutes les 5 min par .github/workflows/surveillance-capteur.yml
//
// 1. Demande au serveur AirCarto les mesures de la dernière heure.
// 2. Regarde l'heure de RÉCEPTION de la plus récente (champ "time" : c'est l'heure à laquelle le serveur l'a reçue,
//    donc exactement « la carte envoie-t-elle encore ? »).
// 3. Plus de SEUIL_MIN minutes sans rien : ouvre une issue « Capteur muet » -> GitHub envoie un MAIL au propriétaire
//    du dépôt (mention @propriétaire, sans assignation : UN seul mail). Une seule issue tant que la panne dure.
// 4. Les mesures reviennent : titre et corps complétés par « Capteur revenu » (durée, heures) et fermeture dans la même
//    requête (UN seul mail, pas de commentaire séparé). GitHub n'écrit dans ce mail que « Closed » : tout le détail
//    utile (durée du silence, heures de coupure et de reprise) est donc placé dans l'OBJET du mail, c'est-à-dire le
//    titre de l'issue ; le compte rendu complet est ajouté au corps de l'issue, lisible sur GitHub.
// 5. CO2 : la carte envoie bien ses mesures (particules) mais aucun CO2 valable depuis SEUIL_CO2_MIN minutes
//    -> issue « CO2 à vérifier » (capteur débranché, en panne, ou qui dérive vers des valeurs impossibles, < 300 ppm).
//    Fermée toute seule dès qu'un CO2 valable revient. Pas évaluée quand la carte est muette (une seule alerte).
//
// Variables d'environnement (fournies par le workflow) :
//   AIRCARTO_CLE   clé du groupe (secret du dépôt, facultatif) : interroge AirCarto directement ;
//                  vide -> passe par le relais Cloudflare du site (const URL_API = "..." dans index.html), sans clé
//   DEVICE_ID      adresse MAC d'une carte (facultatif, "" = toutes les cartes du groupe)
//   SEUIL_MIN      minutes sans mesure avant l'alerte (défaut 10)
//   SEUIL_CO2_MIN  minutes sans CO2 valable (alors que les autres mesures arrivent) avant l'alerte CO2 (défaut 60)
//   GITHUB_TOKEN, GITHUB_REPOSITORY, GITHUB_REPOSITORY_OWNER : fournis par GitHub Actions
import { readFileSync } from "node:fs";

const URL_API = "https://api.aircarto.fr/iut-amu/data";
const LABEL = "capteur-muet";
const LABEL_CO2 = "co2-a-verifier";
const SEUIL_MIN = Number(process.env.SEUIL_MIN || 10);
const SEUIL_CO2_MIN = Number(process.env.SEUIL_CO2_MIN || 60);
const CO2_PLAUSIBLE = [300, 5000];   // mêmes bornes que la carte (CO2_MIN_PLAUSIBLE) et le site
const CO2_RETOUR_MIN = 15;            // un CO2 valable dans les 15 dernières minutes = capteur revenu
const MESURES_MIN = 10;               // il faut au moins ~5 min de mesures pour juger le CO2 (pas juste après un démarrage)
const DEVICE_ID = process.env.DEVICE_ID || "";
const DEPOT = process.env.GITHUB_REPOSITORY;
const PROPRIO = process.env.GITHUB_REPOSITORY_OWNER;
const TOKEN = process.env.GITHUB_TOKEN;

// Où lire les mesures : AirCarto directement (avec le secret AIRCARTO_CLE), sinon le relais du site (il ajoute la clé).
function source() {
  if (process.env.AIRCARTO_CLE) return { url: URL_API, cle: process.env.AIRCARTO_CLE.trim() };
  try {
    const m = /const\s+URL_API\s*=\s*"(https:\/\/[^"]+)"/.exec(readFileSync("index.html", "utf8"));
    if (m && !m[1].includes("A-REMPLACER")) return { url: m[1], cle: "" };
  } catch (e) { /* pas d'index.html */ }
  return null;
}

// Mesures de la dernière heure (ou plus, pour couvrir SEUIL_CO2_MIN). Points bruts d'abord ; si le serveur exige
// "freq", moyenne d'1 min.
async function lirePoints(src) {
  const fenetreMin = Math.max(60, SEUIL_CO2_MIN + 5);
  for (const freq of ["", "1m"]) {
    const params = new URLSearchParams({ start: "-" + fenetreMin + "m" });
    if (src.cle) params.set("key", src.cle);
    if (freq) params.set("freq", freq);
    if (DEVICE_ID) params.set("device_id", DEVICE_ID);
    const rep = await fetch(src.url + "?" + params, { signal: AbortSignal.timeout(30000) });
    let json = null;
    try { json = await rep.json(); } catch (e) { /* réponse illisible */ }
    if (json && json.ok) return json.points || [];
    if (freq) throw new Error("serveur AirCarto : " + ((json && json.message) || "HTTP " + rep.status));
  }
  return [];
}

async function github(methode, chemin, corps) {
  const rep = await fetch("https://api.github.com/repos/" + DEPOT + chemin, {
    method: methode,
    headers: {
      Authorization: "Bearer " + TOKEN,
      Accept: "application/vnd.github+json",
      "X-GitHub-Api-Version": "2022-11-28",
      "Content-Type": "application/json",
    },
    body: corps ? JSON.stringify(corps) : undefined,
  });
  if (!rep.ok && rep.status !== 422) throw new Error("GitHub " + methode + " " + chemin + " : HTTP " + rep.status);
  return rep.status === 204 ? null : rep.json();
}

const LIEN_RUN = process.env.GITHUB_RUN_ID
  ? `${process.env.GITHUB_SERVER_URL || "https://github.com"}/${DEPOT}/actions/runs/${process.env.GITHUB_RUN_ID}`
  : "";

const heureFr = t => new Date(t).toLocaleString("fr-FR", { timeZone: "Europe/Paris", dateStyle: "short", timeStyle: "medium" });
const fmtParis = (t, opt) => new Date(t).toLocaleString("fr-FR", { timeZone: "Europe/Paris", ...opt });
const heureCourte = t => fmtParis(t, { hour: "2-digit", minute: "2-digit" });
const jourCourt = t => fmtParis(t, { day: "2-digit", month: "2-digit" });

// « 47 min », « 2 h 05 min », « 1 j 3 h » : durée lisible d'un coup d'oeil
function duree(ms) {
  const min = Math.max(0, Math.round(ms / 60000));
  if (min < 60) return min + " min";
  const h = Math.floor(min / 60), m = min % 60;
  if (h < 24) return h + " h " + String(m).padStart(2, "0") + " min";
  return Math.floor(h / 24) + " j" + (h % 24 ? " " + (h % 24) + " h" : "");
}

// « 22/11 · 14:02 → 14:49 » (ou avec les deux dates si la panne passe minuit)
const plage = (a, b) => jourCourt(a) === jourCourt(b)
  ? `${jourCourt(a)} · ${heureCourte(a)} → ${heureCourte(b)}`
  : `${jourCourt(a)} ${heureCourte(a)} → ${jourCourt(b)} ${heureCourte(b)}`;

// Repère invisible (commentaire HTML) glissé dans le corps de l'issue : on le relit à la fermeture
// pour connaître l'heure exacte de la dernière mesure avant la panne.
const marque = (nom, valeur) => `<!-- ${nom}:${valeur} -->`;
function lireMarque(corps, nom) {
  const m = new RegExp(`<!--\\s*${nom}:(\\d+)\\s*-->`).exec(corps || "");
  return m ? Number(m[1]) : 0;
}

// Tableau « Information | Détail » (les lignes vides sont ignorées)
const tableau = lignes => ["| Information | Détail |", "|---|---|",
  ...lignes.filter(Boolean).map(([k, v]) => `| ${k} | ${v} |`)].join("\n");

// Ouvre (une seule fois) ou ferme l'issue d'une alerte. probleme : true / false ;
// texte : { titre(), corps(), retour(issue) -> { titre, corps } }.
async function majAlerte(label, couleur, description, probleme, texte) {
  const ouvertes = await github("GET", `/issues?state=open&labels=${label}&per_page=10`);
  const issue = (ouvertes || []).find(i => !i.pull_request);
  if (probleme && !issue) {
    await github("POST", "/labels", { name: label, color: couleur, description });
    // Pas d'« assignees » : l'assignation est un événement à part qui envoyait un 2e mail (« assigned #N »).
    // La mention @PROPRIO dans le corps suffit à déclencher UN mail.
    const cree = await github("POST", "/issues", {
      title: texte.titre(),
      labels: [label],
      body: texte.corps(),
    });
    console.log(`[${label}] issue d'alerte ouverte : ` + (cree && cree.html_url));
  } else if (!probleme && issue) {
    // Un seul appel : titre et corps de l'issue complétés (modifications silencieuses) et issue fermée dans la même
    // requête -> un seul mail « Closed », dont l'objet est le nouveau titre « ✅ ... » (durée et heures de la panne).
    // (Avant : commentaire + fermeture = 2 mails.)
    const retour = texte.retour(issue);
    await github("PATCH", `/issues/${issue.number}`, {
      title: retour.titre,
      body: (issue.body || "") + "\n\n---\n" + retour.corps,
      state: "closed",
      state_reason: "completed",
    });
    console.log(`[${label}] retour à la normale : issue #${issue.number} fermée.`);
  } else {
    console.log(`[${label}] ` + (probleme ? `alerte déjà ouverte (issue #${issue.number}) : pas de nouveau mail.` : "rien à faire."));
  }
}

// CO2 : parmi les mesures REÇUES pendant les SEUIL_CO2_MIN dernières minutes et qui contiennent des particules
// (preuve que la carte tourne), y a-t-il un CO2 valable ? Renvoie null si on ne peut pas juger (trop peu de mesures,
// ou carte allumée depuis trop peu de temps).
function etatCO2(points, maintenant) {
  const recu = p => new Date(p.time).getTime();
  const valable = p => typeof p.co2 === "number" && p.co2 >= CO2_PLAUSIBLE[0] && p.co2 <= CO2_PLAUSIBLE[1];
  const actives = points.filter(p => typeof p.pm25 === "number" && Number.isFinite(recu(p)));
  const fenetre = actives.filter(p => maintenant - recu(p) <= SEUIL_CO2_MIN * 60000);
  const recents = actives.filter(p => maintenant - recu(p) <= CO2_RETOUR_MIN * 60000);
  const dernierValable = Math.max(0, ...points.filter(valable).map(recu));
  if (recents.some(valable)) return { probleme: false, dernierValable };
  const couvre = actives.length && maintenant - Math.min(...actives.map(recu)) >= (SEUIL_CO2_MIN - 10) * 60000;
  if (fenetre.length < MESURES_MIN || !couvre) return null;
  if (fenetre.some(valable)) return { probleme: false, dernierValable };
  const impossibles = fenetre.filter(p => typeof p.co2 === "number" && !valable(p)).map(p => p.co2);
  return { probleme: true, dernierValable, impossibles, nb: fenetre.length };
}

async function main() {
  const src = source();
  if (!src) throw new Error("ni secret AIRCARTO_CLE, ni adresse du relais (const URL_API dans index.html)");

  let points;
  try {
    points = await lirePoints(src);
  } catch (e) {
    // Le serveur lui-même ne répond pas : ce n'est pas forcément le capteur. On le signale sans ouvrir d'issue
    // (sinon une panne du serveur AirCarto déclencherait une fausse alerte).
    console.log("::warning::Serveur AirCarto injoignable, vérification reportée : " + e.message);
    return;
  }

  const recus = points.map(p => new Date(p.time).getTime()).filter(Number.isFinite);
  const dernier = recus.length ? Math.max(...recus) : 0;
  const maintenant = Date.now();
  const silenceMin = dernier ? (maintenant - dernier) / 60000 : Infinity;
  const muet = silenceMin > SEUIL_MIN;
  console.log(dernier
    ? `Dernière mesure reçue : ${heureFr(dernier)} (il y a ${silenceMin.toFixed(1)} min) -> ${muet ? "MUET" : "OK"}`
    : `Aucune mesure reçue depuis ${Math.max(60, SEUIL_CO2_MIN + 5)} min -> MUET`);

  await majAlerte(LABEL, "d73a4a", "Le capteur n'envoie plus de mesures", muet, {
    titre: () => `⚠️ Capteur muet : aucune mesure ${dernier ? "depuis " + Math.round(silenceMin) + " min" : "depuis plus d'1 h"}`,
    corps: () => [
      // 1re ligne = aperçu du mail dans la boîte de réception : on y met l'essentiel et la mention du propriétaire
      `@${PROPRIO} le serveur AirCarto ne reçoit plus aucune mesure ${dernier ? `depuis le **${heureFr(dernier)}** (heure de Paris)` : "depuis **plus d'une heure**"}.`,
      "",
      "## 🔇 Le capteur ne donne plus signe de vie",
      "",
      tableau([
        ["🕒 Dernière mesure reçue", dernier ? heureFr(dernier) : "aucune dans la dernière heure"],
        ["⏳ Silence constaté", dernier ? `**${duree(maintenant - dernier)}**` : "**plus d'1 h**"],
        ["🎯 Seuil d'alerte", `${SEUIL_MIN} min sans mesure`],
        ["📟 Carte surveillée", DEVICE_ID ? `\`${DEVICE_ID}\`` : "toutes celles du groupe"],
        ["🔎 Détecté le", heureFr(maintenant)],
        LIEN_RUN && ["🤖 Contrôle automatique", `[voir l'exécution](${LIEN_RUN})`],
      ]),
      "",
      "### 🛠️ À vérifier, dans cet ordre",
      "1. **Alimentation** : la carte est-elle sous tension, l'écran allumé ? (secteur ou batterie vide)",
      "2. **Wi-Fi** : commande `status` dans le moniteur série, lignes « WiFi » et « file d'attente ».",
      "3. **Clé du groupe** : un code HTTP 401 dans `status` veut dire que le serveur la refuse (`set cle ...`).",
      "4. **Redémarrage** : `reboot`. La carte redémarre aussi toute seule après 1 h sans Wi-Fi.",
      "",
      "### 💾 Les données ne sont pas perdues",
      "Pendant la coupure, la carte garde ses mesures en mémoire flash (jusqu'à ~12 h) et les renvoie d'elle-même ",
      "dès que le réseau revient.",
      "",
      "> _Cette issue se fermera toute seule dès que les mesures reviendront._",
      marque("debut", dernier),   // repère invisible : sert à calculer la durée exacte du silence au retour
    ].join("\n"),
    retour: issue => {
      const exact = lireMarque(issue.body, "debut");        // dernière mesure avant la panne (ms), 0 si inconnue
      const debut = exact || Date.parse(issue.created_at);
      // Premier signe de vie après la panne = plus ancienne mesure reçue depuis (le serveur date à la RÉCEPTION)
      const apres = recus.filter(t => t > debut).sort((x, y) => x - y)[0] || dernier;
      const silence = Math.max(0, apres - debut);
      const titre = exact
        ? `✅ Capteur revenu après ${duree(silence)} de silence (${plage(debut, apres)})`
        : `✅ Capteur revenu : mesures reçues de nouveau le ${jourCourt(apres)} à ${heureCourte(apres)} (silence de plus d'1 h)`;
      const corps = [
        "## ✅ Retour à la normale",
        "",
        `Le capteur envoie de nouveau ses mesures. L'alerte est **résolue**${exact ? ` après **${duree(silence)}** de silence` : ""}.`,
        "",
        tableau([
          exact && ["🔌 Dernière mesure avant la coupure", heureFr(exact)],
          ["📡 Reprise des envois", heureFr(apres)],
          exact && ["⏱️ Durée du silence", `**${duree(silence)}**`],
          ["🚨 Alerte ouverte le", heureFr(Date.parse(issue.created_at))],
          ["🕒 Dernière mesure reçue", `${heureFr(dernier)} (il y a ${Math.max(0, Math.round(silenceMin))} min)`],
          ["🔎 Retour constaté le", heureFr(maintenant)],
          LIEN_RUN && ["🤖 Contrôle automatique", `[voir l'exécution](${LIEN_RUN})`],
        ]),
        "",
        "### 💾 À savoir pour les graphiques",
        "Les mesures gardées en mémoire par la carte pendant la coupure sont renvoyées au fil de l'eau. ",
        "Le serveur les date à leur **réception** : l'heure réelle d'une mesure rejouée = heure de réception − `x_age_s`.",
        "",
        "_Issue fermée automatiquement. Une nouvelle alerte s'ouvrira si le silence reprend._",
      ].join("\n");
      return { titre, corps };
    },
  });

  // CO2 : seulement si la carte envoie (sinon l'alerte « capteur muet » suffit)
  if (muet) return;
  const co2 = etatCO2(points, maintenant);
  if (!co2) { console.log("[co2] pas assez de mesures récentes pour juger le CO2."); return; }
  console.log(co2.probleme
    ? `CO2 : aucune valeur valable dans les ${co2.nb} mesures des ${SEUIL_CO2_MIN} dernières minutes -> ALERTE`
    : "CO2 : OK");
  await majAlerte(LABEL_CO2, "fbca04", "Le capteur de CO2 ne donne plus de valeur valable", co2.probleme, {
    titre: () => co2.impossibles.length
      ? `⚠️ CO₂ à recalibrer : valeurs impossibles depuis plus de ${SEUIL_CO2_MIN} min`
      : `⚠️ CO₂ absent depuis plus de ${SEUIL_CO2_MIN} min (le reste des mesures arrive)`,
    corps: () => [
      `@${PROPRIO} la carte envoie bien ses mesures (particules), mais **aucune valeur de CO₂ valable** ` +
        `(${CO2_PLAUSIBLE[0]} à ${CO2_PLAUSIBLE[1]} ppm) depuis plus de ${SEUIL_CO2_MIN} min.`,
      "",
      "## 🌫️ Le capteur de CO₂ ne répond plus correctement",
      "",
      tableau([
        ["📡 Carte", "envoie ses mesures normalement (particules reçues)"],
        ["🧪 État du CO₂", co2.impossibles.length
          ? `valeurs **impossibles** : ${[...new Set(co2.impossibles.map(Math.round))].slice(0, 8).join(", ")} ppm (l'air ne descend pas sous ~400 ppm)`
          : "**aucune valeur** (champ `co2` absent des mesures)"],
        ["🕒 Dernier CO₂ valable", co2.dernierValable ? heureFr(co2.dernierValable) : "aucun dans la fenêtre surveillée"],
        ["🎯 Seuil d'alerte", `${SEUIL_CO2_MIN} min sans CO₂ valable`],
        ["🔎 Détecté le", heureFr(maintenant)],
        LIEN_RUN && ["🤖 Contrôle automatique", `[voir l'exécution](${LIEN_RUN})`],
      ]),
      "",
      "### 🛠️ À vérifier",
      "1. Moniteur série, commande `status` : ligne « CO2 » (en erreur ? lectures impossibles écartées ?).",
      "2. Écran, page CO₂ : **« PANNE »** = le capteur ne répond plus (câblage du MH-Z19C, GPIO 36 / 27, alimentation 5 V) ; " +
        "**« A CALIBRER »** = valeurs impossibles.",
      "3. Valeurs impossibles : placer la carte 20 min à l'air libre (fenêtre ouverte), puis lancer `calib oui`.",
      "",
      "> _Cette issue se fermera toute seule dès qu'un CO₂ valable reviendra._",
    ].join("\n"),
    retour: issue => {
      const ouverte = Date.parse(issue.created_at);
      const revenu = co2.dernierValable || maintenant;
      const duree_ = Math.max(0, revenu - ouverte);
      const titre = `✅ CO₂ revenu : valeur valable reçue à ${heureCourte(revenu)} (alerte ouverte depuis ${duree(duree_)})`;
      const corps = [
        "## ✅ Retour à la normale",
        "",
        "Le capteur de CO₂ donne de nouveau une valeur valable. L'alerte est **résolue**.",
        "",
        tableau([
          ["🚨 Alerte ouverte le", heureFr(ouverte)],
          ["🧪 CO₂ valable reçu le", heureFr(revenu)],
          ["⏱️ Alerte restée ouverte", `**${duree(duree_)}**`],
          ["🔎 Retour constaté le", heureFr(maintenant)],
          LIEN_RUN && ["🤖 Contrôle automatique", `[voir l'exécution](${LIEN_RUN})`],
        ]),
        "",
        "_Issue fermée automatiquement. Une nouvelle alerte s'ouvrira si le CO₂ redevient absent ou impossible._",
      ].join("\n");
      return { titre, corps };
    },
  });
}

main().catch(e => {
  console.error(e);
  process.exit(1);
});
