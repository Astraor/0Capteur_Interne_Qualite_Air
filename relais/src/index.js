// Relais Cloudflare de La Bonne M'Air : boîte aux lettres entre la carte et la page console.
//
//   carte  --HTTPS-->  /carte/sync       (journal)      <-- commandes en attente, « console ouverte ? »
//   carte  --HTTPS-->  /carte/manifeste  (mise à jour : firmware.json de la dernière release GitHub)
//   carte  --HTTPS-->  /carte/firmware   (mise à jour : firmware.bin de cette release, relayé tel quel)
//   admin  --HTTPS-->  /                 (page console)  /api/connexion  /api/journal  /api/commande ...
//   site   --HTTPS-->  /donnees          (mesures AirCarto : le relais ajoute la clé du groupe, que le site ne voit plus)
//
// Secrets (wrangler secret put ...) — 16 caractères minimum, sinon tout est refusé :
//   JETON_CARTE           la carte se présente avec lui (entré au câble USB : set jeton ...)
//   MOT_DE_PASSE_CONSOLE  administrateur, une seule fois par appareil : la page reçoit ensuite un JETON DE SESSION
//                         (le mot de passe n'est plus gardé dans le navigateur). Changer ce secret
//                         déconnecte tous les appareils.
//   AIRCARTO_CLE          clé du groupe AirCarto (elle permettait aussi d'ENVOYER de fausses mesures)
//   GITHUB_TOKEN          seulement si le dépôt devient privé (lecture des releases par l'API GitHub)
// La SIGNATURE des mises à jour est vérifiée par la CARTE (clé publique compilée dedans) : même un relais piraté ne
// peut pas lui faire installer un programme qui n'est pas signé par la clé privée du projet.
// Le relais refuse aussi les commandes sensibles (Wi-Fi, clé...) ; la carte les refuse de toute façon.
import PAGE_CONSOLE from "./console.html";

// Commandes acceptées à distance : même liste que sur la carte (commandeAutoriseeADistance dans main.cpp).
// « calib oui » n'y est plus : il fausserait les mesures de CO2 jusqu'au prochain calibrage (câble USB seulement).
// Commandes « page » : changer la page de l'écran (sans effet durable). MÊMES NOMS que NOMS_PAGES dans main.cpp.
export const NOMS_PAGES = ["co2", "aerer", "aérer", "pm1", "pm2.5", "pm25", "pm10", "atmo", "courbe", "wifi", "etat", "état"];
export const COMMANDES_AUTORISEES = [
  "aide", "help", "status", "verbose on", "verbose off", "calib", "reboot",
  "wifi scan", "wifi connect", "maj", "maj installer", "diag tls",
  "page", ...["suivante", "+", "precedente", "précédente", "-", "fixe", "auto", ...NOMS_PAGES].map((m) => "page " + m),
];
// Lignes que seul le relais écrit (connexions, commandes de la console, séparateurs, alertes de sécurité). Une ligne
// de la CARTE qui commencerait pareil (nom de Wi-Fi piégé, réponse de serveur...) est préfixée par « carte : »,
// pour qu'elle ne puisse pas se faire passer pour une trace du relais.
const MARQUES_RELAIS = /^\s*(»|──|\[sécurité\]|\[…)/;
// Caractères invisibles qui peuvent faire passer un bout de ligne pour une autre ligne ou inverser le sens de
// lecture (séparateurs de ligne Unicode, contrôles C0/C1, marques de direction) : remplacés par « ? ».
const INVISIBLES = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f\u061c\u200b-\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069\ufeff]/g;
export function ligneCarte(txt) {
  txt = String(txt).replace(INVISIBLES, "?");
  return MARQUES_RELAIS.test(txt) ? "carte : " + txt : txt;
}
export function normaliser(cmd) {
  return String(cmd).toLowerCase().trim().replace(/\s+/g, " ");
}

const LIGNES_GARDEES = 5000;               // le journal garde les 5000 dernières lignes...
const DUREE_GARDEE_MS = 7 * 86400000;      // ... et 7 jours au plus
const CONSOLE_LIVE_MS = 20000;             // console vue il y a moins de 20 s : la carte passe en « presque direct »
const PURGE_MS = 3600000;                  // nettoyage du journal (lignes en trop, plus de 7 jours) : une fois par heure
const SAUVEGARDE_MS = 5 * 60000;           // compteurs du quota et état de la carte : gardés toutes les 5 min
const GARDE_APRES_COMMANDE_MS = 60000;     // quota d'écritures presque atteint : on garde quand même ce que la carte
                                           // répond dans la minute qui suit une commande de la console

// Quotas QUOTIDIENS de l'offre gratuite de Cloudflare pour les Durable Objects, remis à zéro à 00:00 UTC. Au-delà,
// tout échoue jusqu'au lendemain (la console affiche « erreur 500 »). Le relais compte lui-même ce qu'il consomme
// (lignes lues / écrites, requêtes) et ralentit bien avant :
//   moins de 50 %   console rafraîchie toutes les 2 s, carte en « presque direct » quand la console est ouverte ;
//   50 à 80 %       mode économie : console toutes les 15 s, carte à son rythme normal (30 s) ;
//   80 % et plus    console toutes les 60 s ; si ce sont les ÉCRITURES, le journal de la carte n'est plus gardé
//                   jusqu'au lendemain, sauf la minute qui suit une commande de la console.
// La page console se met aussi en pause après 5 min sans activité : une console oubliée ne coûte plus rien.
export const QUOTAS = { requetes: 100000, lectures: 5000000, ecritures: 100000 };
const SEUIL_ECONOMIE = 0.5;
const SEUIL_SOBRE = 0.8;
export const RYTHME_CONSOLE_MS = [2000, 15000, 60000];
const ESSAIS_MAX = 10;                     // secrets faux avant blocage...
const BLOCAGE_MS = 15 * 60000;             // ... de 15 min (par adresse IPv4, ou par bloc /64 en IPv6)
const MAX_LIGNES_PAR_ENVOI = 400;
const MAX_OCTETS_PAR_ENVOI = 256 * 1024;   // un paquet de journal de la carte ne dépasse jamais ça
const MAX_COMMANDES_ATTENTE = 10;
const ALERTE_ESSAIS_HEURE = 50;            // secrets refusés en 1 h (toutes adresses) avant une alerte dans le journal
const SESSION_COURTE_MS = 12 * 3600000;    // « Rester connecté » décoché : 12 h
const SESSION_LONGUE_MS = 30 * 86400000;   // coché : 30 jours
const SESSIONS_MAX = 20;
const CACHE_DONNEES_MS = 60000;            // /donnees : même demande dans la minute = pas de nouvel appel AirCarto
const PLAGE_MAX_JOURS_DEFAUT = 400;

const HSTS = "max-age=31536000";
const ENTETES_API = {
  "Content-Type": "application/json; charset=utf-8",
  "Cache-Control": "no-store",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "X-Robots-Tag": "noindex",
  "Strict-Transport-Security": HSTS,
};
const json = (obj, statut = 200, plus = {}) => new Response(JSON.stringify(obj), { status: statut, headers: { ...ENTETES_API, ...plus } });

// Comparaison à temps constant (on compare les empreintes : même longueur, et rien ne fuit du secret).
export async function secretEgal(recu, attendu) {
  if (typeof attendu !== "string" || attendu.length < 16 || typeof recu !== "string") return false;
  const enc = new TextEncoder();
  const [a, b] = await Promise.all([crypto.subtle.digest("SHA-256", enc.encode(recu)),
                                    crypto.subtle.digest("SHA-256", enc.encode(attendu))]);
  if (crypto.subtle.timingSafeEqual) return crypto.subtle.timingSafeEqual(a, b);
  const x = new Uint8Array(a), y = new Uint8Array(b);
  let d = 0;
  for (let i = 0; i < x.length; i++) d |= x[i] ^ y[i];
  return d === 0;
}

async function empreinteHex(texte) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(texte));
  return [...new Uint8Array(d)].map((o) => o.toString(16).padStart(2, "0")).join("");
}

function porteur(request) {
  const h = request.headers.get("Authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7) : "";
}

// Adresse du client, regroupée par bloc /64 en IPv6 : un attaquant IPv6 possède des milliards d'adresses
// dans son /64, il ne doit pas avoir 10 essais par adresse.
export function cleIp(ip) {
  if (!ip || !ip.includes(":")) return ip || "inconnue";
  const [g, d = ""] = ip.toLowerCase().split("::");
  const a = g ? g.split(":") : [], b = d ? d.split(":") : [];
  const plein = [...a, ...Array(Math.max(0, 8 - a.length - b.length)).fill("0"), ...b];
  return plein.slice(0, 4).map((x) => x.replace(/^0+(?=.)/, "")).join(":") + "::/64";
}
const ipClient = (request) => cleIp(request.headers.get("CF-Connecting-IP"));

// Limiteur de débit Cloudflare ([[ratelimits]] dans wrangler.toml) ; absent = pas de limite.
async function tropDeRequetes(limiteur, cle) {
  if (!limiteur) return false;
  try { return !(await limiteur.limit({ key: cle })).success; } catch { return false; }
}

export function appareil(ua = "") {
  const os = /iPhone|iPad/.test(ua) ? "iOS" : /Android/.test(ua) ? "Android" : /Windows/.test(ua) ? "Windows"
    : /Mac OS/.test(ua) ? "macOS" : /Linux/.test(ua) ? "Linux" : "?";
  const nav = /Firefox\//.test(ua) ? "Firefox" : /Edg\//.test(ua) ? "Edge" : /Chrome\//.test(ua) ? "Chrome"
    : /Safari\//.test(ua) ? "Safari" : "?";
  return nav + " / " + os;
}

// ---------------- releases GitHub
// Dépôt public : lien direct. Dépôt privé (GITHUB_TOKEN) : API GitHub ; le jeton n'est envoyé QU'À api.github.com,
// jamais à l'hôte de téléchargement vers lequel GitHub redirige.
const GH_API = "https://api.github.com";
async function telechargerRelease(env, tag, nom) {
  const ua = { "User-Agent": "mair-relais" };
  if (!env.GITHUB_TOKEN) {
    const chemin = tag === "latest" ? `latest/download/${nom}` : `download/${tag}/${nom}`;
    return fetch(`https://github.com/${env.DEPOT_GITHUB}/releases/${chemin}`, { headers: ua, redirect: "follow" });
  }
  const h = { ...ua, Authorization: "Bearer " + env.GITHUB_TOKEN, "X-GitHub-Api-Version": "2022-11-28" };
  const rel = await fetch(`${GH_API}/repos/${env.DEPOT_GITHUB}/releases/${tag === "latest" ? "latest" : "tags/" + tag}`,
    { headers: { ...h, Accept: "application/vnd.github+json" } });
  if (!rel.ok) return new Response(null, { status: rel.status });
  let assets = [];
  try { assets = (await rel.json()).assets || []; } catch {}
  const a = assets.find((x) => x.name === nom);
  if (!a || typeof a.url !== "string" || !a.url.startsWith(GH_API + "/")) return new Response(null, { status: 404 });
  const r = await fetch(a.url, { headers: { ...h, Accept: "application/octet-stream" }, redirect: "manual" });
  const loc = r.headers.get("Location");
  if (r.status >= 300 && r.status < 400 && loc) return fetch(loc, { headers: ua, redirect: "follow" });
  return r;
}

// GitHub injoignable : exception (réponse 502 « GitHub injoignable »), à ne pas confondre avec « aucune release ».
async function manifeste(env) {
  const r = await telechargerRelease(env, "latest", "firmware.json");
  if (!r.ok) return null;
  try {
    const m = await r.json();
    if (!/^v\d{1,12}$/.test(m.tag) || !Number.isInteger(m.version) || m.version <= 0 ||
        !Number.isInteger(m.taille) || m.taille <= 0) return null;
    return m;
  } catch {
    return null;
  }
}

// ---------------- mesures pour le site (/donnees)
// Le site public demandait les mesures avec la clé du groupe écrite dans sa page : n'importe qui pouvait la lire et
// s'en servir pour ENVOYER des mesures. Il passe maintenant par ici : seuls les paramètres de lecture sont acceptés,
// la clé est ajoutée par le relais. Plage limitée, débit limité, petite mémoire de 60 s.
const URL_AIRCARTO = "https://api.aircarto.fr/iut-amu/data";
const PARAMETRES = {
  start: /^(-\d{1,6}[mhd]|\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)$/,
  end: /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/,
  freq: /^\d{1,3}[mhd]$/,
  device_id: /^[0-9A-Fa-f:]{12,17}$/,
};
const UNITE_MS = { m: 60000, h: 3600000, d: 86400000 };
// Mémoire par instance du Worker (fonctionne aussi sur *.workers.dev, où l'API Cache de Cloudflare ne fait rien).
const memoireDonnees = new Map();

function origineAutorisee(env, origine) {
  if (!origine) return true;   // pas un navigateur (GitHub Actions, curl) : les mesures sont publiques de toute façon
  const liste = (env.ORIGINES_SITE || "https://astraor.github.io").split(",").map((o) => o.trim());
  return liste.includes(origine) || /^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/.test(origine);
}
function entetesCors(origine) {
  return origine ? { "Access-Control-Allow-Origin": origine, "Access-Control-Allow-Methods": "GET", "Vary": "Origin" } : {};
}
async function donnees(request, env, url) {
  const origine = request.headers.get("Origin");
  if (!origineAutorisee(env, origine)) return json({ ok: false, message: "site non autorisé" }, 403);
  const cors = entetesCors(origine);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: { ...cors, "Access-Control-Max-Age": "86400" } });
  if (await tropDeRequetes(env.LIMITE_DONNEES, ipClient(request)))
    return json({ ok: false, message: "trop de requêtes : réessayez dans une minute" }, 429, { ...cors, "Retry-After": "60" });
  if (!env.AIRCARTO_CLE) return json({ ok: false, message: "clé AirCarto absente du relais (wrangler secret put AIRCARTO_CLE)" }, 500, cors);

  const lecture = new URLSearchParams();
  for (const [nom, motif] of Object.entries(PARAMETRES)) {
    const v = url.searchParams.get(nom);
    if (v === null || v === "") continue;
    if (!motif.test(v)) return json({ ok: false, message: "paramètre invalide : " + nom }, 400, cors);
    lecture.set(nom, v);
  }
  if (!lecture.has("start")) return json({ ok: false, message: "paramètre start manquant" }, 400, cors);

  // plage demandée : PLAGE_MAX_JOURS au plus (évite de solliciter AirCarto sur des années de données avec la clé du groupe)
  const maintenant = Date.now();
  const s = lecture.get("start");
  const debut = s.startsWith("-") ? maintenant - Number(s.slice(1, -1)) * UNITE_MS[s.slice(-1)] : Date.parse(s);
  const fin = lecture.has("end") ? Date.parse(lecture.get("end")) : maintenant;
  const maxMs = (Number(env.PLAGE_MAX_JOURS) || PLAGE_MAX_JOURS_DEFAUT) * 86400000;
  if (!Number.isFinite(debut) || !Number.isFinite(fin) || fin - debut > maxMs)
    return json({ ok: false, message: "plage trop longue (" + (maxMs / 86400000) + " jours au plus)" }, 400, cors);

  const cle = lecture.toString();
  const enMemoire = memoireDonnees.get(cle);
  if (enMemoire && maintenant - enMemoire.t < CACHE_DONNEES_MS)
    return new Response(enMemoire.corps, { status: 200, headers: { ...ENTETES_API, ...cors, "X-Relais-Cache": "oui" } });

  const params = new URLSearchParams(lecture);
  params.set("key", env.AIRCARTO_CLE);
  let r, corps;
  try {
    r = await fetch(URL_AIRCARTO + "?" + params, { headers: { "User-Agent": "mair-relais" } });
    corps = await r.text();
  } catch {
    return json({ ok: false, message: "serveur AirCarto injoignable" }, 502, cors);
  }
  // Une page d'erreur pourrait recopier l'URL demandée (donc la clé) : on ne relaie QUE les réponses valides,
  // et jamais un texte qui contiendrait la clé.
  if (!r.ok || corps.includes(env.AIRCARTO_CLE))
    return json({ ok: false, message: "AirCarto a répondu " + r.status }, 502, cors);
  // Petite mémoire de 60 s : 30 réponses et 8 Mo au plus en tout (la mémoire d'une instance du Worker est limitée).
  if (corps.length < 2000000) {
    memoireDonnees.delete(cle);
    memoireDonnees.set(cle, { t: maintenant, corps });
    let total = 0;
    for (const v of memoireDonnees.values()) total += v.corps.length;
    while (memoireDonnees.size > 30 || total > 8000000) {
      const plusVieille = memoireDonnees.keys().next().value;
      total -= memoireDonnees.get(plusVieille).corps.length;
      memoireDonnees.delete(plusVieille);
    }
  }
  return new Response(corps, { status: 200, headers: { ...ENTETES_API, ...cors } });
}

// ---------------- page console : CSP par empreinte (plus de 'unsafe-inline')
// Les empreintes du <script> et du <style> de console.html sont calculées une fois au démarrage du Worker :
// elles suivent donc automatiquement toute modification de console.html. Ne pas ajouter d'attribut style="..." ni de
// onclick="..." dans la page : ils seraient bloqués.
let cspConsole = null;
async function empreinteBloc(motif) {
  const m = motif.exec(PAGE_CONSOLE);
  if (!m) return "'none'";
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(m[1]));
  return "'sha256-" + btoa(String.fromCharCode(...new Uint8Array(d))) + "'";
}
async function csp() {
  if (!cspConsole) {
    const [script, style] = await Promise.all([empreinteBloc(/<script>([\s\S]*?)<\/script>/), empreinteBloc(/<style>([\s\S]*?)<\/style>/)]);
    cspConsole = `default-src 'none'; script-src ${script}; style-src ${style}; connect-src 'self'; img-src 'self' data:; ` +
      "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
  }
  return cspConsole;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const p = url.pathname;

    if (request.method === "GET" && (p === "/" || p === "/console")) {
      return new Response(PAGE_CONSOLE, {
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Cache-Control": "no-cache",
          "Content-Security-Policy": await csp(),
          "X-Content-Type-Options": "nosniff",
          "Referrer-Policy": "no-referrer",
          "X-Robots-Tag": "noindex",
          "Strict-Transport-Security": HSTS,
        },
      });
    }

    if (p === "/donnees" && (request.method === "GET" || request.method === "OPTIONS")) return donnees(request, env, url);

    // carte et console : tout passe par le Durable Object. La carte qui présente le BON jeton n'est jamais freinée par
    // la limite par adresse IP : sur eduroam, des centaines d'appareils partagent son adresse publique, et n'importe
    // qui pourrait sinon épuiser cette limite pour la couper du relais.
    if (p.startsWith("/carte/") || p.startsWith("/api/")) {
      // Console sans jeton de session bien formé (robot, page d'une très vieille version) : refusée ici, sans réveiller
      // le Durable Object (chaque requête qui l'atteint compte dans son quota quotidien). Un tel jeton ne peut de
      // toute façon jamais être valide.
      if (p.startsWith("/api/") && p !== "/api/connexion" && !/^[0-9a-f]{64}$/.test(porteur(request)))
        return json({ erreur: "session expirée : reconnectez-vous" }, 401);
      const carteAuthentifiee = p.startsWith("/carte/") && (await secretEgal(porteur(request), env.JETON_CARTE));
      if (!carteAuthentifiee && await tropDeRequetes(env.LIMITE, ipClient(request)))
        return json({ erreur: "trop de requêtes : réessayez dans une minute" }, 429, { "Retry-After": "60" });
      try {
        return await env.RELAIS.get(env.RELAIS.idFromName("carte")).fetch(request);
      } catch {
        // le plus souvent : quota quotidien des Durable Objects épuisé (offre gratuite), jusqu'à 00:00 UTC. 503 : la
        // carte espace alors ses échanges (jusqu'à 5 min), la console affiche ce message.
        return json({ erreur: "relais indisponible : quota Cloudflare du jour atteint (remise à zéro à 00:00 UTC) ou panne passagère" },
                    503, { "Retry-After": "300" });
      }
    }
    return new Response("Introuvable", { status: 404 });
  },
};

// Un seul Durable Object (« carte ») : journal, commandes en attente, état, sessions, essais refusés (SQLite).
// Toute requête SQL passe par q(), qui compte les lignes lues et écrites (facturées par Cloudflare) pour les paliers
// du quota (voir QUOTAS). Ce qui change à chaque échange (dernière visite de la carte, console ouverte) reste en
// mémoire : avant, chaque échange relisait et réécrivait 4 lignes de la table etat, et le nettoyage du journal relisait
// tout le journal (jusqu'à 5000 lignes) toutes les 2 s quand la console était ouverte.
export class Relais {
  constructor(state, env) {
    this.env = env;
    this.sql = state.storage.sql;
    this.conso = { requetes: 0, lectures: 0, ecritures: 0 };
    this.sql.exec(`CREATE TABLE IF NOT EXISTS lignes (id INTEGER PRIMARY KEY AUTOINCREMENT, t INTEGER, txt TEXT)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS commandes (id INTEGER PRIMARY KEY AUTOINCREMENT, texte TEXT, cree INTEGER)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS etat (cle TEXT PRIMARY KEY, valeur TEXT)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS echecs (ip TEXT PRIMARY KEY, n INTEGER, depuis INTEGER)`);
    // on ne garde que l'EMPREINTE du jeton de session : une fuite de la base ne donne accès à rien
    this.sql.exec(`CREATE TABLE IF NOT EXISTS sessions (empreinte TEXT PRIMARY KEY, cree INTEGER, expire INTEGER,
                   mdp TEXT, ip TEXT, appareil TEXT)`);
    // table etat relue UNE fois : version et démarrage de la carte, et « memoire » (compteurs du quota du jour,
    // dernière visite de la carte, dernier nettoyage), gardée toutes les SAUVEGARDE_MS
    this.etat = {};
    for (const r of this.q("SELECT cle, valeur FROM etat")) this.etat[r.cle] = r.valeur;
    let m = {};
    try { m = JSON.parse(this.etat.memoire || "{}") || {}; } catch {}
    this.jour = typeof m.jour === "string" ? m.jour : "";
    for (const k of Object.keys(this.conso)) this.conso[k] += Number(m[k]) || 0;
    this.vue = Number(m.vue) || Number(this.etat.vue) || 0;          // (anciennes versions : clé « vue » à part)
    this.uptime = Number(m.uptime) || Number(this.etat.uptime) || 0;
    this.purge = Number(m.purge) || 0;
    this.lignesSautees = Number(m.sautees) || 0;
    this.consoleVue = 0;         // dernière requête de la page console (mémoire seulement)
    this.derniereCommande = 0;   // dernière commande envoyée depuis la console
    this.sauve = Date.now();
    // clés des anciennes versions, réécrites à chaque échange : supprimées une fois pour toutes
    const anciennes = ["vue", "uptime", "console"].filter((k) => k in this.etat);
    if (anciennes.length) {
      this.q(`DELETE FROM etat WHERE cle IN (${anciennes.map(() => "?").join(",")})`, ...anciennes);
      for (const k of anciennes) delete this.etat[k];
    }
    this.jourCourant(Date.now());
  }

  // Toutes les requêtes SQL passent par ici : lignes lues et écrites comptées pour le quota du jour.
  q(requete, ...params) {
    const c = this.sql.exec(requete, ...params);
    const r = c.toArray();
    this.conso.lectures += Number(c.rowsRead) || 0;
    this.conso.ecritures += Number(c.rowsWritten) || 0;
    return r;
  }
  // Clé de la table etat : écrite seulement si elle change.
  ecrire(cle, valeur) {
    valeur = String(valeur);
    if (this.etat[cle] === valeur) return;
    this.etat[cle] = valeur;
    this.q("INSERT INTO etat (cle, valeur) VALUES (?, ?) ON CONFLICT(cle) DO UPDATE SET valeur = excluded.valeur", cle, valeur);
  }
  // Lignes du journal ajoutées en UNE requête (le compteur AUTOINCREMENT n'est réécrit qu'une fois, pas à chaque ligne).
  ajouterLignes(liste) {
    if (!liste.length) return;
    const donnees = JSON.stringify(liste.map(([t, txt]) => [Math.round(Number(t)) || 0, String(txt).slice(0, 500)]));
    this.q("INSERT INTO lignes (t, txt) SELECT json_extract(value, '$[0]'), json_extract(value, '$[1]') " +
           "FROM json_each(?) ORDER BY CAST(key AS INTEGER)", donnees);
  }
  ajouterLigne(t, txt) { this.ajouterLignes([[t, txt]]); }

  // ---- quota du jour (remis à zéro à 00:00 UTC, comme celui de Cloudflare)
  jourCourant(maintenant) {
    const j = new Date(maintenant).toISOString().slice(0, 10);
    if (j === this.jour) return;
    const sautees = this.jour ? this.lignesSautees : 0;
    this.jour = j;
    this.conso = { requetes: 0, lectures: 0, ecritures: 0 };
    this.lignesSautees = 0;
    if (sautees > 0) this.ajouterLigne(maintenant, `[… ${sautees} ligne(s) de la carte non gardée(s) hier : quota d'écritures Cloudflare presque atteint]`);
    this.sauver(maintenant, true);
  }
  parts() {
    return { requetes: this.conso.requetes / QUOTAS.requetes, lectures: this.conso.lectures / QUOTAS.lectures,
             ecritures: this.conso.ecritures / QUOTAS.ecritures };
  }
  // 0 : normal | 1 : économie | 2 : sobre (voir QUOTAS)
  niveau() {
    const p = this.parts();
    const max = Math.max(p.requetes, p.lectures, p.ecritures);
    return max >= SEUIL_SOBRE ? 2 : max >= SEUIL_ECONOMIE ? 1 : 0;
  }
  sauver(maintenant, force = false) {
    if (!force && maintenant - this.sauve < SAUVEGARDE_MS) return;
    this.sauve = maintenant;
    this.ecrire("memoire", JSON.stringify({ jour: this.jour, ...this.conso, vue: this.vue, uptime: this.uptime,
                                            purge: this.purge, sautees: this.lignesSautees }));
  }

  // ---- nettoyage du journal, une fois par heure. Les deux requêtes suivent l'ordre des lignes (clé primaire) : elles
  // ne lisent que les lignes effacées, pas tout le journal.
  purger(maintenant) {
    if (maintenant - this.purge < PURGE_MS) return;
    this.purge = maintenant;
    this.q("DELETE FROM lignes WHERE id <= (SELECT MAX(id) FROM lignes) - ?", LIGNES_GARDEES);
    this.q("DELETE FROM lignes WHERE id < COALESCE((SELECT id FROM lignes WHERE t >= ? ORDER BY id LIMIT 1), " +
           "(SELECT MAX(id) + 1 FROM lignes))", maintenant - DUREE_GARDEE_MS);
  }

  // ---- anti-devinette : 10 secrets faux = 15 min de blocage (compteurs séparés carte / console)
  estBloque(cle, maintenant) {
    const r = this.q("SELECT n, depuis FROM echecs WHERE ip = ?", cle);
    return r.length > 0 && maintenant - r[0].depuis < BLOCAGE_MS && r[0].n >= ESSAIS_MAX;
  }
  noterEchec(cle, maintenant, quoi) {
    const r = this.q("SELECT n, depuis FROM echecs WHERE ip = ?", cle);
    const actif = r.length > 0 && maintenant - r[0].depuis < BLOCAGE_MS;
    const n = actif ? r[0].n + 1 : 1;
    if (!actif) this.q("INSERT OR REPLACE INTO echecs (ip, n, depuis) VALUES (?, 1, ?)", cle, maintenant);
    else if (n === ESSAIS_MAX) this.q("UPDATE echecs SET n = ?, depuis = ? WHERE ip = ?", n, maintenant, cle); // 15 min à partir de maintenant
    else this.q("UPDATE echecs SET n = ? WHERE ip = ?", n, cle);
    this.q("DELETE FROM echecs WHERE depuis < ?", maintenant - BLOCAGE_MS);
    if (n === ESSAIS_MAX) this.ajouterLigne(maintenant, `[sécurité] ${ESSAIS_MAX} ${quoi} refusés depuis ${cle.split(" ")[1]} : accès bloqué 15 min`);
    // Toutes adresses confondues : beaucoup d'essais en une heure = quelqu'un essaie depuis de nombreuses adresses
    // (le blocage par adresse ne l'arrête pas). Une alerte par heure au plus dans le journal.
    if (!this.alerte || maintenant - this.alerte.debut > 3600000) this.alerte = { debut: maintenant, n: 0, dit: false };
    this.alerte.n++;
    if (this.alerte.n >= ALERTE_ESSAIS_HEURE && !this.alerte.dit) {
      this.alerte.dit = true;
      this.ajouterLigne(maintenant, `[sécurité] ${this.alerte.n} secrets refusés en moins d'1 h, toutes adresses confondues : ` +
        "attaque probable. Vérifiez que le mot de passe de la console est long et aléatoire.");
    }
  }

  // ---- sessions de la console
  async empreinteMdp() {
    return (await empreinteHex("console:" + this.env.MOT_DE_PASSE_CONSOLE)).slice(0, 32);
  }
  async session(jeton, maintenant) {
    const mdp = this.env.MOT_DE_PASSE_CONSOLE;
    if (typeof mdp !== "string" || mdp.length < 16 || !/^[0-9a-f]{64}$/.test(jeton)) return null;
    const e = await empreinteHex(jeton);
    const r = this.q("SELECT empreinte, expire, mdp FROM sessions WHERE empreinte = ?", e);
    // mot de passe changé depuis l'ouverture de la session : elle ne vaut plus rien
    if (!r.length || r[0].expire < maintenant || r[0].mdp !== await this.empreinteMdp()) return null;
    return r[0];
  }

  async fetch(request) {
    const maintenant = Date.now();
    this.jourCourant(maintenant);
    this.conso.requetes++;
    try {
      return await this.traiter(request, maintenant);
    } finally {
      this.sauver(Date.now());
    }
  }

  async traiter(request, maintenant) {
    const url = new URL(request.url);
    const p = url.pathname;
    const ip = ipClient(request);

    // ================= la carte
    if (p.startsWith("/carte/")) {
      // Le jeton de la carte est testé AVANT le blocage : quelqu'un qui partage l'adresse IP de la carte (eduroam)
      // et envoie 10 faux jetons bloque son adresse, mais pas la vraie carte. (Jeton de 16 caractères aléatoires ou
      // plus : impossible à deviner, même sans blocage ; le blocage ne sert qu'à freiner les essais.)
      const cle = "carte " + ip;
      if (!(await secretEgal(porteur(request), this.env.JETON_CARTE))) {
        if (this.estBloque(cle, maintenant)) return json({ erreur: "trop d'essais : réessayez dans 15 min" }, 429);
        this.noterEchec(cle, maintenant, "jetons de carte");
        return json({ erreur: "jeton refusé" }, 401);
      }
      if (p === "/carte/sync" && request.method === "POST") return this.sync(request, maintenant);
      if (p === "/carte/manifeste" && request.method === "GET") {
        let m;
        try { m = await manifeste(this.env); } catch { return json({ erreur: "GitHub injoignable" }, 502); }
        return m ? json(m) : json({ erreur: "pas de release avec firmware.json" }, 404);
      }
      if (p === "/carte/firmware" && request.method === "GET") {
        const tag = url.searchParams.get("tag") || "";
        if (!/^v\d{1,12}$/.test(tag)) return json({ erreur: "tag invalide" }, 400);
        let r;
        try { r = await telechargerRelease(this.env, tag, "firmware.bin"); } catch { return json({ erreur: "GitHub injoignable" }, 502); }
        if (!r.ok) return json({ erreur: "firmware.bin introuvable" }, 404);
        // relayé tel quel (la carte vérifie l'empreinte, la signature et la version) ; la taille aide la carte
        const h = new Headers({ "Content-Type": "application/octet-stream", "Cache-Control": "no-store" });
        const n = r.headers.get("Content-Length");
        if (n) h.set("Content-Length", n);
        return new Response(r.body, { status: 200, headers: h });
      }
      return json({ erreur: "inconnu" }, 404);
    }

    // ================= la page console
    if (p.startsWith("/api/")) {
      const cle = "console " + ip;

      // ---- connexion : le mot de passe ne sert qu'ici, la page reçoit un jeton de session
      if (p === "/api/connexion" && request.method === "POST") {
        if (this.estBloque(cle, maintenant)) return json({ erreur: "trop d'essais : réessayez dans 15 min" }, 429);
        // L'essai est compté AVANT toute attente (lecture du corps, calcul d'empreinte) : pendant ces attentes, d'autres
        // requêtes peuvent passer ; comptées après, une rafale dépasserait les 10 essais. Effacé si c'est le bon.
        this.noterEchec(cle, maintenant, "mots de passe");
        let corps = {};
        try { corps = await request.json(); } catch {}
        if (!(await secretEgal(corps && corps.mdp, this.env.MOT_DE_PASSE_CONSOLE))) {
          return json({ erreur: "mot de passe refusé" }, 401);
        }
        this.q("DELETE FROM echecs WHERE ip = ?", cle);
        const jeton = [...crypto.getRandomValues(new Uint8Array(32))].map((o) => o.toString(16).padStart(2, "0")).join("");
        const duree = corps.souvenir === true ? SESSION_LONGUE_MS : SESSION_COURTE_MS;
        const app = appareil(request.headers.get("User-Agent") || "");
        this.q("INSERT INTO sessions (empreinte, cree, expire, mdp, ip, appareil) VALUES (?, ?, ?, ?, ?, ?)",
          await empreinteHex(jeton), maintenant, maintenant + duree, await this.empreinteMdp(), ip, app);
        this.q("DELETE FROM sessions WHERE expire < ?", maintenant);
        this.q("DELETE FROM sessions WHERE empreinte NOT IN (SELECT empreinte FROM sessions ORDER BY cree DESC LIMIT ?)", SESSIONS_MAX);
        this.ajouterLigne(maintenant, `» connexion à la console (${ip}, ${app})`);
        return json({ jeton, expire: maintenant + duree });
      }

      // Session : jeton aléatoire de 256 bits, impossible à deviner. Une session VALIDE passe toujours, même depuis une
      // adresse bloquée (adresse partagée avec quelqu'un qui essaie des mots de passe) ; les jetons faux sont comptés.
      const jeton = porteur(request);
      const s = await this.session(jeton, maintenant);
      if (!s) {
        if (this.estBloque(cle, maintenant)) return json({ erreur: "trop d'essais : réessayez dans 15 min" }, 429);
        this.noterEchec(cle, maintenant, "jetons de session");
        return json({ erreur: "session expirée : reconnectez-vous" }, 401);
      }
      const qui = `${ip}, ${appareil(request.headers.get("User-Agent") || "")}`;

      if (p === "/api/deconnexion" && request.method === "POST") {
        let corps = {};
        try { corps = await request.json(); } catch {}
        if (corps.partout === true) {
          this.q("DELETE FROM sessions");
          this.ajouterLigne(maintenant, `» tous les appareils déconnectés de la console (${qui})`);
        } else {
          this.q("DELETE FROM sessions WHERE empreinte = ?", s.empreinte);
        }
        return json({ ok: true });
      }

      if (p === "/api/manifeste" && request.method === "GET") {
        let m;
        try { m = await manifeste(this.env); } catch { return json({ erreur: "GitHub injoignable" }, 502); }
        return m ? json({ version: m.version, tag: m.tag, taille: m.taille }) : json({ erreur: "aucune release avec firmware.json" }, 404);
      }

      // ---- la page console lit le journal (et signale qu'elle est ouverte)
      if (p === "/api/journal" && request.method === "GET") {
        this.consoleVue = maintenant;
        const apres = Number(url.searchParams.get("apres")) || 0;
        let lignes;
        if (apres > 0) lignes = this.q("SELECT id, t, txt FROM lignes WHERE id > ? ORDER BY id LIMIT 1000", apres);
        else lignes = this.q("SELECT id, t, txt FROM lignes ORDER BY id DESC LIMIT 400").reverse();
        const attente = this.q("SELECT COUNT(*) AS n FROM commandes")[0].n;
        const parts = this.parts(), niveau = this.niveau();
        return json({
          lignes,
          carte: { vue: this.vue || null, version: this.etat.version || null, uptime: this.uptime },
          commandesEnAttente: attente,
          maintenant,
          // la page règle son rythme sur celui-ci, et affiche le quota du jour (en %)
          rythme: RYTHME_CONSOLE_MS[niveau],
          quota: { niveau, requetes: Math.round(parts.requetes * 100), lectures: Math.round(parts.lectures * 100),
                   ecritures: Math.round(parts.ecritures * 100) },
        });
      }

      // ---- une commande tapée dans la console
      if (p === "/api/commande" && request.method === "POST") {
        let corps;
        try { corps = await request.json(); } catch { return json({ erreur: "requête illisible" }, 400); }
        const texte = normaliser((corps && corps.texte) || "");
        if (!texte || texte.length > 100) return json({ erreur: "commande vide ou trop longue" }, 400);
        if (!COMMANDES_AUTORISEES.includes(texte)) {
          return json({ erreur: "Réservée au câble USB (ou inconnue). À distance : status, verbose on/off, calib, reboot, wifi scan, wifi connect, maj, maj installer, diag tls, page (suivante, precedente, fixe, auto, co2, pm1, pm2.5, pm10, atmo, courbe, etat…), aide (calib oui : câble USB)." }, 403);
        }
        const n = this.q("SELECT COUNT(*) AS n FROM commandes")[0].n;
        if (n >= MAX_COMMANDES_ATTENTE) return json({ erreur: "déjà trop de commandes en attente" }, 429);
        this.q("INSERT INTO commandes (texte, cree) VALUES (?, ?)", texte, maintenant);
        this.ajouterLigne(maintenant, `» ${texte}   (console : ${qui})`);
        this.consoleVue = this.derniereCommande = maintenant;
        return json({ ok: true });
      }

      // ---- vider le journal affiché (une trace reste : qui l'a vidé, quand)
      if (p === "/api/effacer" && request.method === "POST") {
        this.q("DELETE FROM lignes");
        this.ajouterLigne(maintenant, `── journal vidé depuis la console (${qui}) ──`);
        return json({ ok: true });
      }
      return json({ erreur: "inconnu" }, 404);
    }
    return json({ erreur: "inconnu" }, 404);
  }

  // ---- la carte envoie un paquet du journal
  async sync(request, maintenant) {
    if (Number(request.headers.get("Content-Length")) > MAX_OCTETS_PAR_ENVOI) return json({ erreur: "paquet trop gros" }, 413);
    const millis = Number(request.headers.get("X-Millis")) || 0;
    const boot = (request.headers.get("X-Boot") || "").slice(0, 32);
    const version = (request.headers.get("X-Version") || "").slice(0, 32);
    const perdues = Number(request.headers.get("X-Perdues")) || 0;
    const texte = (await request.text()).slice(-MAX_OCTETS_PAR_ENVOI);
    const aGarder = [];
    if (boot && boot !== this.etat.boot) {
      aGarder.push([maintenant - millis, `── démarrage de la carte (version ${version}) ──`]);
      this.ecrire("boot", boot);
    }
    if (perdues > 0) aGarder.push([maintenant, `[… ${perdues} ligne(s) perdue(s) : journal plein sur la carte]`]);
    const lignes = texte.split("\n").filter((l) => l.length > 0).slice(-MAX_LIGNES_PAR_ENVOI);
    // Quota d'écritures presque atteint : le journal de la carte n'est plus gardé jusqu'au lendemain (une ligne le
    // dit), sauf la minute qui suit une commande de la console, pour qu'on en voie la réponse.
    const garder = this.parts().ecritures < SEUIL_SOBRE || maintenant - this.derniereCommande < GARDE_APRES_COMMANDE_MS;
    if (garder) {
      for (const l of lignes) {
        const tab = l.indexOf("\t");
        const ms = tab > 0 ? Number(l.slice(0, tab)) : NaN;
        const t = Number.isFinite(ms) && ms >= 0 && ms <= millis ? maintenant - (millis - ms) : maintenant;
        aGarder.push([t, ligneCarte(tab > 0 ? l.slice(tab + 1) : l)]);
      }
    } else if (lignes.length) {
      if (this.lignesSautees === 0)
        aGarder.push([maintenant, "[… journal de la carte suspendu jusqu'à 00:00 UTC : quota d'écritures Cloudflare du jour " +
                                  "presque atteint (les commandes et leurs réponses passent toujours)]"]);
      this.lignesSautees += lignes.length;
    }
    this.ajouterLignes(aGarder);
    this.purger(maintenant);
    this.vue = maintenant;
    this.uptime = Math.round(millis / 1000);
    if (version) this.ecrire("version", version);
    // commandes en attente : remises UNE seule fois (si la réponse se perd, la commande est perdue, jamais doublée)
    const cmds = this.q("SELECT id, texte FROM commandes ORDER BY id");
    if (cmds.length) this.q("DELETE FROM commandes WHERE id <= ?", cmds[cmds.length - 1].id);
    // « presque direct » (un échange toutes les 2 s) seulement hors mode économie
    const live = this.niveau() === 0 && maintenant - this.consoleVue < CONSOLE_LIVE_MS;
    return json({ live, cmds: cmds.map((c) => c.texte) });
  }
}
