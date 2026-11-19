// Surveillance du capteur : lancé toutes les 5 min par .github/workflows/surveillance-capteur.yml
//
// 1. Demande au serveur AirCarto les mesures de la dernière heure.
// 2. Regarde l'heure de RÉCEPTION de la plus récente (champ "time" : c'est l'heure à laquelle le serveur l'a reçue,
//    donc exactement « la carte envoie-t-elle encore ? »).
// 3. Plus de SEUIL_MIN minutes sans rien : ouvre une issue « Capteur muet » -> GitHub envoie un MAIL au propriétaire
//    du dépôt (notification d'issue). Une seule issue tant que la panne dure (pas de mail toutes les 5 min).
// 4. Les mesures reviennent : commentaire « Capteur revenu » + fermeture de l'issue (-> second mail).
//
// Variables d'environnement (fournies par le workflow) :
//   AIRCARTO_CLE   clé du groupe (secret du dépôt) ; vide -> lue dans index.html (const CLE = "...")
//   DEVICE_ID      adresse MAC d'une carte (facultatif, "" = toutes les cartes du groupe)
//   SEUIL_MIN      minutes sans mesure avant l'alerte (défaut 10)
//   GITHUB_TOKEN, GITHUB_REPOSITORY, GITHUB_REPOSITORY_OWNER : fournis par GitHub Actions
import { readFileSync } from "node:fs";

const URL_API = "https://api.aircarto.fr/iut-amu/data";
const LABEL = "capteur-muet";
const SEUIL_MIN = Number(process.env.SEUIL_MIN || 10);
const DEVICE_ID = process.env.DEVICE_ID || "";
const DEPOT = process.env.GITHUB_REPOSITORY;
const PROPRIO = process.env.GITHUB_REPOSITORY_OWNER;
const TOKEN = process.env.GITHUB_TOKEN;

function lireCle() {
  if (process.env.AIRCARTO_CLE) return process.env.AIRCARTO_CLE.trim();
  try {
    const m = /const\s+CLE\s*=\s*"([0-9a-fA-F]{32})"/.exec(readFileSync("index.html", "utf8"));
    if (m) return m[1];
  } catch (e) { /* pas d'index.html */ }
  return "";
}

// Mesures de la dernière heure. Points bruts d'abord ; si le serveur exige "freq", moyenne d'1 min.
async function lirePoints(cle) {
  for (const freq of ["", "1m"]) {
    const params = new URLSearchParams({ key: cle, start: "-1h" });
    if (freq) params.set("freq", freq);
    if (DEVICE_ID) params.set("device_id", DEVICE_ID);
    const rep = await fetch(URL_API + "?" + params, { signal: AbortSignal.timeout(30000) });
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

async function main() {
  const cle = lireCle();
  if (!cle) throw new Error("clé AirCarto introuvable (secret AIRCARTO_CLE ou index.html)");

  let points;
  try {
    points = await lirePoints(cle);
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
    : `Aucune mesure reçue depuis 1 h -> MUET`);

  // L'issue d'alerte déjà ouverte, s'il y en a une
  const ouvertes = await github("GET", `/issues?state=open&labels=${LABEL}&per_page=10`);
  const issue = (ouvertes || []).find(i => !i.pull_request);

  if (muet && !issue) {
    await github("POST", "/labels", { name: LABEL, color: "d73a4a", description: "Le capteur n'envoie plus de mesures" });
    const depuis = dernier ? `depuis le **${heureFr(dernier)}** (heure de Paris)` : "depuis **plus d'une heure**";
    const cree = await github("POST", "/issues", {
      title: `⚠️ Capteur muet : aucune mesure ${dernier ? "depuis " + Math.round(silenceMin) + " min" : "depuis plus d'1 h"}`,
      labels: [LABEL],
      assignees: PROPRIO ? [PROPRIO] : [],
      body: [
        `@${PROPRIO} le serveur AirCarto ne reçoit plus aucune mesure ${depuis}.`,
        "",
        `- Seuil d'alerte : ${SEUIL_MIN} min sans mesure`,
        DEVICE_ID ? `- Carte surveillée : \`${DEVICE_ID}\`` : "- Cartes surveillées : toutes celles du groupe",
        "",
        "**À vérifier** : alimentation de la carte, Wi-Fi (commande `status` dans le moniteur série), clé du groupe (`set cle`).",
        "La carte garde ses mesures en mémoire pendant la coupure et les renverra au retour du réseau.",
        "",
        "_Cette issue se fermera toute seule dès que les mesures reviendront._",
      ].join("\n"),
    });
    console.log("Issue d'alerte ouverte : " + (cree && cree.html_url));
  } else if (!muet && issue) {
    await github("POST", `/issues/${issue.number}/comments`, {
      body: `✅ Capteur revenu : mesure reçue le ${heureFr(dernier)} (heure de Paris). Fermeture automatique.`,
    });
    await github("PATCH", `/issues/${issue.number}`, { state: "closed", state_reason: "completed" });
    console.log("Capteur revenu : issue #" + issue.number + " fermée.");
  } else {
    console.log(muet ? "Alerte déjà ouverte (issue #" + issue.number + ") : pas de nouveau mail." : "Rien à faire.");
  }
}

main().catch(e => {
  console.error(e);
  process.exit(1);
});
