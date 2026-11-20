// Surveillance du capteur : lancé toutes les 5 min par .github/workflows/surveillance-capteur.yml
//
// 1. Demande au serveur AirCarto les mesures de la dernière heure.
// 2. Regarde l'heure de RÉCEPTION de la plus récente (champ "time" : c'est l'heure à laquelle le serveur l'a reçue,
//    donc exactement « la carte envoie-t-elle encore ? »).
// 3. Plus de SEUIL_MIN minutes sans rien : ouvre une issue « Capteur muet » -> GitHub envoie un MAIL au propriétaire
//    du dépôt (mention @propriétaire, sans assignation : UN seul mail). Une seule issue tant que la panne dure.
// 4. Les mesures reviennent : titre/corps complétés par « Capteur revenu » et fermeture dans la même requête
//    (UN seul mail, pas de commentaire séparé).
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

const heureFr = t => new Date(t).toLocaleString("fr-FR", { timeZone: "Europe/Paris", dateStyle: "short", timeStyle: "medium" });

// Ouvre (une seule fois) ou ferme l'issue d'une alerte. probleme : true / false ; texte : { titre, corps, retour }.
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
    // Un seul appel : le message de retour est ajouté au titre et au corps (modifications silencieuses) et l'issue
    // est fermée dans la même requête -> un seul mail « Closed », dont l'objet contient le titre « ✅ ... ».
    // (Avant : commentaire + fermeture = 2 mails.)
    const retour = texte.retour();
    await github("PATCH", `/issues/${issue.number}`, {
      title: retour.split(" : ")[0] + " — " + issue.title.replace(/^⚠️\s*/, ""),
      body: (issue.body || "") + "\n\n---\n" + retour,
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
      `@${PROPRIO} le serveur AirCarto ne reçoit plus aucune mesure ${dernier ? `depuis le **${heureFr(dernier)}** (heure de Paris)` : "depuis **plus d'une heure**"}.`,
      "",
      `- Seuil d'alerte : ${SEUIL_MIN} min sans mesure`,
      DEVICE_ID ? `- Carte surveillée : \`${DEVICE_ID}\`` : "- Cartes surveillées : toutes celles du groupe",
      "",
      "**À vérifier** : alimentation de la carte, Wi-Fi (commande `status` dans le moniteur série), clé du groupe (`set cle`).",
      "La carte garde ses mesures en mémoire pendant la coupure et les renverra au retour du réseau.",
      "",
      "_Cette issue se fermera toute seule dès que les mesures reviendront._",
    ].join("\n"),
    retour: () => `✅ Capteur revenu : mesure reçue le ${heureFr(dernier)} (heure de Paris). Fermeture automatique.`,
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
      co2.impossibles.length
        ? `- Valeurs reçues mais impossibles : ${[...new Set(co2.impossibles.map(Math.round))].slice(0, 8).join(", ")} ppm (l'air ne descend pas sous ~400 ppm).`
        : "- Aucune valeur de CO₂ reçue (champ `co2` absent des mesures).",
      co2.dernierValable ? `- Dernier CO₂ valable reçu : ${heureFr(co2.dernierValable)} (heure de Paris).` : `- Aucun CO₂ valable dans la fenêtre surveillée.`,
      "",
      "**À vérifier** :",
      "- Commande `status` dans le moniteur série : ligne « CO2 » (en erreur ? lectures impossibles écartées ?).",
      "- Écran : page CO₂ « PANNE » (le capteur ne répond plus : câblage du MH-Z19C, GPIO 36 / 27, alimentation 5 V) " +
        "ou « A CALIBRER » (valeurs impossibles).",
      "- Valeurs impossibles : poser la carte 20 min à l'air libre (fenêtre ouverte), puis taper `calib oui`.",
      "",
      "_Cette issue se fermera toute seule dès qu'un CO₂ valable reviendra._",
    ].join("\n"),
    retour: () => `✅ CO₂ revenu : valeur valable reçue le ${heureFr(co2.dernierValable)} (heure de Paris). Fermeture automatique.`,
  });
}

main().catch(e => {
  console.error(e);
  process.exit(1);
});
