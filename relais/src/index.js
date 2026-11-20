// Relais Cloudflare de La Bonne M'Air : boîte aux lettres entre la carte et ta page console.
//
//   carte  --HTTPS-->  /carte/sync       (journal)      <-- commandes en attente, « console ouverte ? »
//   carte  --HTTPS-->  /carte/manifeste  (mise à jour : firmware.json de la dernière release GitHub)
//   carte  --HTTPS-->  /carte/firmware   (mise à jour : firmware.bin de cette release, relayé tel quel)
//   toi    --HTTPS-->  /                 (page console)  /api/journal  /api/commande  /api/manifeste
//   site   --HTTPS-->  /donnees          (mesures AirCarto : le relais ajoute la clé du groupe, que le site ne voit plus)
//
// Secrets (wrangler secret put ...) :
//   JETON_CARTE           la carte se présente avec lui (entré au câble USB : set jeton ...)
//   MOT_DE_PASSE_CONSOLE  toi, dans la page console
//   AIRCARTO_CLE          clé du groupe AirCarto : n'est plus écrite dans le site (elle permettait aussi d'ENVOYER
//                         de fausses mesures au nom du groupe)
// La SIGNATURE des mises à jour est vérifiée par la CARTE (clé publique compilée dedans) : même un relais piraté ne
// peut pas lui faire installer un programme qui ne vient pas de ta clé privée.
// Le relais refuse aussi les commandes sensibles (Wi-Fi, clé...) ; la carte les refuse de toute façon.
import PAGE_CONSOLE from "./console.html";

// Commandes acceptées à distance : même liste que sur la carte (commandeAutoriseeADistance dans main.cpp).
export const COMMANDES_AUTORISEES = [
  "aide", "help", "status", "verbose on", "verbose off", "calib", "calib oui", "reboot",
  "wifi scan", "wifi connect", "maj", "maj installer",
];
export function normaliser(cmd) {
  return String(cmd).toLowerCase().trim().replace(/\s+/g, " ");
}

const LIGNES_GARDEES = 5000;          // le journal garde les 5000 dernières lignes...
const DUREE_GARDEE_MS = 7 * 86400000; // ... et 7 jours au plus
const CONSOLE_LIVE_MS = 20000;        // console vue il y a moins de 20 s : la carte passe en « presque direct »
const ESSAIS_MAX = 10;                // mots de passe faux avant blocage...
const BLOCAGE_MS = 15 * 60000;        // ... de 15 min (par adresse IP)
const MAX_LIGNES_PAR_ENVOI = 400;
const MAX_COMMANDES_ATTENTE = 10;

const ENTETES_API = {
  "Content-Type": "application/json; charset=utf-8",
  "Cache-Control": "no-store",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "X-Robots-Tag": "noindex",
};
const json = (obj, statut = 200) => new Response(JSON.stringify(obj), { status: statut, headers: ENTETES_API });

// Comparaison à temps constant (on compare les empreintes : même longueur, et rien ne fuit du secret).
async function secretEgal(recu, attendu) {
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

function porteur(request) {
  const h = request.headers.get("Authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7) : "";
}

// Le fichier de la release GitHub (dépôt public ; GITHUB_TOKEN facultatif pour un dépôt privé).
async function telechargerRelease(env, chemin) {
  const entetes = { "User-Agent": "mair-relais" };
  if (env.GITHUB_TOKEN) entetes.Authorization = "Bearer " + env.GITHUB_TOKEN;
  return fetch(`https://github.com/${env.DEPOT_GITHUB}/releases/${chemin}`, { headers: entetes, redirect: "follow" });
}

// ---------------- mesures pour le site (/donnees)
// Le site public demandait les mesures avec la clé du groupe écrite dans sa page : n'importe qui pouvait la lire et
// s'en servir pour ENVOYER des mesures. Il passe maintenant par ici : seuls les paramètres de lecture sont acceptés,
// la clé est ajoutée par le relais. Bonus : Cloudflare compresse la réponse (l'API AirCarto ne le fait pas).
const URL_AIRCARTO = "https://api.aircarto.fr/iut-amu/data";
const PARAMETRES = {
  start: /^(-\d{1,6}[mhd]|\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)$/,
  end: /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/,
  freq: /^\d{1,3}[mhd]$/,
  device_id: /^[0-9A-Fa-f:]{12,17}$/,
};
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
  if (!env.AIRCARTO_CLE) return new Response(JSON.stringify({ ok: false, message: "clé AirCarto absente du relais (wrangler secret put AIRCARTO_CLE)" }),
    { status: 500, headers: { ...ENTETES_API, ...cors } });
  const params = new URLSearchParams({ key: env.AIRCARTO_CLE });
  for (const [nom, motif] of Object.entries(PARAMETRES)) {
    const v = url.searchParams.get(nom);
    if (v === null || v === "") continue;
    if (!motif.test(v)) return new Response(JSON.stringify({ ok: false, message: "paramètre invalide : " + nom }),
      { status: 400, headers: { ...ENTETES_API, ...cors } });
    params.set(nom, v);
  }
  if (!params.has("start")) return new Response(JSON.stringify({ ok: false, message: "paramètre start manquant" }),
    { status: 400, headers: { ...ENTETES_API, ...cors } });
  let r;
  try {
    r = await fetch(URL_AIRCARTO + "?" + params, { headers: { "User-Agent": "mair-relais" } });
  } catch {
    return new Response(JSON.stringify({ ok: false, message: "serveur AirCarto injoignable" }), { status: 502, headers: { ...ENTETES_API, ...cors } });
  }
  // réponse relayée telle quelle (la clé n'y figure pas)
  return new Response(r.body, { status: r.status, headers: { ...ENTETES_API, ...cors } });
}

async function manifeste(env) {
  const r = await telechargerRelease(env, "latest/download/firmware.json");
  if (!r.ok) return null;
  try {
    const m = await r.json();
    if (!/^v\d{1,12}$/.test(m.tag) || !Number.isInteger(m.version) || !Number.isInteger(m.taille)) return null;
    return m;
  } catch {
    return null;
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const relais = env.RELAIS.get(env.RELAIS.idFromName("carte"));
    const p = url.pathname;

    // ---------------- page console
    if (request.method === "GET" && (p === "/" || p === "/console")) {
      return new Response(PAGE_CONSOLE, {
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Cache-Control": "no-cache",
          "Content-Security-Policy": "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " +
            "connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
          "X-Content-Type-Options": "nosniff",
          "Referrer-Policy": "no-referrer",
          "X-Robots-Tag": "noindex",
        },
      });
    }

    // ---------------- le site (mesures)
    if (p === "/donnees" && (request.method === "GET" || request.method === "OPTIONS")) return donnees(request, env, url);

    // ---------------- la carte
    if (p.startsWith("/carte/")) {
      if (!(await secretEgal(porteur(request), env.JETON_CARTE))) return json({ erreur: "jeton refusé" }, 401);
      if (p === "/carte/sync" && request.method === "POST") return relais.fetch(request);
      if (p === "/carte/manifeste" && request.method === "GET") {
        const m = await manifeste(env);
        return m ? json(m) : json({ erreur: "pas de release avec firmware.json" }, 404);
      }
      if (p === "/carte/firmware" && request.method === "GET") {
        const tag = url.searchParams.get("tag") || "";
        if (!/^v\d{1,12}$/.test(tag)) return json({ erreur: "tag invalide" }, 400);
        const r = await telechargerRelease(env, `download/${tag}/firmware.bin`);
        if (!r.ok) return json({ erreur: "firmware.bin introuvable" }, 404);
        // relayé tel quel (la carte vérifie l'empreinte et la signature) ; la taille aide la carte
        const h = new Headers({ "Content-Type": "application/octet-stream", "Cache-Control": "no-store" });
        const n = r.headers.get("Content-Length");
        if (n) h.set("Content-Length", n);
        return new Response(r.body, { status: 200, headers: h });
      }
      return json({ erreur: "inconnu" }, 404);
    }

    // ---------------- toi (page console) : mot de passe et anti-devinette vérifiés dans le Durable Object,
    // en une seule requête (le blocage par adresse IP doit passer AVANT toute vérification du mot de passe)
    if (p.startsWith("/api/")) return relais.fetch(request);
    return new Response("Introuvable", { status: 404 });
  },
};

// Un seul Durable Object (« carte ») : il garde le journal, les commandes en attente et l'état, dans sa base SQLite.
export class Relais {
  constructor(state, env) {
    this.env = env;
    this.sql = state.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS lignes (id INTEGER PRIMARY KEY AUTOINCREMENT, t INTEGER, txt TEXT)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS commandes (id INTEGER PRIMARY KEY AUTOINCREMENT, texte TEXT, cree INTEGER)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS etat (cle TEXT PRIMARY KEY, valeur TEXT)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS echecs (ip TEXT PRIMARY KEY, n INTEGER, depuis INTEGER)`);
  }

  lire(cle, defaut = null) {
    const r = this.sql.exec("SELECT valeur FROM etat WHERE cle = ?", cle).toArray();
    return r.length ? r[0].valeur : defaut;
  }
  ecrire(cle, valeur) {
    this.sql.exec("INSERT INTO etat (cle, valeur) VALUES (?, ?) ON CONFLICT(cle) DO UPDATE SET valeur = excluded.valeur", cle, String(valeur));
  }
  ajouterLigne(t, txt) {
    this.sql.exec("INSERT INTO lignes (t, txt) VALUES (?, ?)", t, txt.slice(0, 500));
  }

  async fetch(request) {
    const url = new URL(request.url);
    const maintenant = Date.now();

    // ---- page console : anti-devinette (10 mots de passe faux = 15 min de blocage pour cette adresse), mot de passe
    if (url.pathname.startsWith("/api/")) {
      const ip = request.headers.get("CF-Connecting-IP") || "inconnue";
      const r = this.sql.exec("SELECT n, depuis FROM echecs WHERE ip = ?", ip).toArray();
      const actif = r.length > 0 && maintenant - r[0].depuis < BLOCAGE_MS;
      if (actif && r[0].n >= ESSAIS_MAX) return json({ erreur: "trop d'essais : réessaie dans 15 min" }, 429);
      if (!(await secretEgal(porteur(request), this.env.MOT_DE_PASSE_CONSOLE))) {
        if (actif) this.sql.exec("UPDATE echecs SET n = n + 1 WHERE ip = ?", ip);
        else this.sql.exec("INSERT OR REPLACE INTO echecs (ip, n, depuis) VALUES (?, 1, ?)", ip, maintenant);
        this.sql.exec("DELETE FROM echecs WHERE depuis < ?", maintenant - BLOCAGE_MS);
        return json({ erreur: "mot de passe refusé" }, 401);
      }
      if (url.pathname === "/api/manifeste" && request.method === "GET") {
        const m = await manifeste(this.env);
        return m ? json({ version: m.version, tag: m.tag, taille: m.taille }) : json({ erreur: "aucune release avec firmware.json" }, 404);
      }
    }

    // ---- la carte envoie un paquet du journal
    if (url.pathname === "/carte/sync") {
      const millis = Number(request.headers.get("X-Millis")) || 0;
      const boot = request.headers.get("X-Boot") || "";
      const version = request.headers.get("X-Version") || "";
      const perdues = Number(request.headers.get("X-Perdues")) || 0;
      if (boot && boot !== this.lire("boot")) {
        this.ajouterLigne(maintenant - millis, `── démarrage de la carte (version ${version}) ──`);
        this.ecrire("boot", boot);
      }
      if (perdues > 0) this.ajouterLigne(maintenant, `[… ${perdues} ligne(s) perdue(s) : journal plein sur la carte]`);
      const texte = await request.text();
      const lignes = texte.split("\n").filter((l) => l.length > 0).slice(-MAX_LIGNES_PAR_ENVOI);
      for (const l of lignes) {
        const tab = l.indexOf("\t");
        const ms = tab > 0 ? Number(l.slice(0, tab)) : NaN;
        const t = Number.isFinite(ms) && ms <= millis ? maintenant - (millis - ms) : maintenant;
        this.ajouterLigne(t, tab > 0 ? l.slice(tab + 1) : l);
      }
      this.sql.exec("DELETE FROM lignes WHERE id <= (SELECT MAX(id) FROM lignes) - ? OR t < ?", LIGNES_GARDEES, maintenant - DUREE_GARDEE_MS);
      this.ecrire("vue", maintenant);
      this.ecrire("version", version);
      this.ecrire("uptime", Math.round(millis / 1000));
      // commandes en attente : remises UNE seule fois (si la réponse se perd, la commande est perdue, jamais doublée)
      const cmds = this.sql.exec("SELECT id, texte FROM commandes ORDER BY id").toArray();
      this.sql.exec("DELETE FROM commandes");
      const live = maintenant - Number(this.lire("console", 0)) < CONSOLE_LIVE_MS;
      return json({ live, cmds: cmds.map((c) => c.texte) });
    }

    // ---- la page console lit le journal (et signale qu'elle est ouverte)
    if (url.pathname === "/api/journal" && request.method === "GET") {
      this.ecrire("console", maintenant);
      const apres = Number(url.searchParams.get("apres")) || 0;
      let lignes;
      if (apres > 0) lignes = this.sql.exec("SELECT id, t, txt FROM lignes WHERE id > ? ORDER BY id LIMIT 1000", apres).toArray();
      else lignes = this.sql.exec("SELECT id, t, txt FROM lignes ORDER BY id DESC LIMIT 400").toArray().reverse();
      const attente = this.sql.exec("SELECT COUNT(*) AS n FROM commandes").one().n;
      return json({
        lignes,
        carte: {
          vue: Number(this.lire("vue", 0)) || null,
          version: this.lire("version"),
          uptime: Number(this.lire("uptime", 0)),
        },
        commandesEnAttente: attente,
        maintenant,
      });
    }

    // ---- une commande tapée dans la console
    if (url.pathname === "/api/commande" && request.method === "POST") {
      let corps;
      try { corps = await request.json(); } catch { return json({ erreur: "requête illisible" }, 400); }
      const texte = normaliser(corps.texte || "");
      if (!texte || texte.length > 100) return json({ erreur: "commande vide ou trop longue" }, 400);
      if (!COMMANDES_AUTORISEES.includes(texte)) {
        return json({ erreur: "Réservée au câble USB (ou inconnue). À distance : status, verbose on/off, calib, reboot, wifi scan, wifi connect, maj, aide." }, 403);
      }
      const n = this.sql.exec("SELECT COUNT(*) AS n FROM commandes").one().n;
      if (n >= MAX_COMMANDES_ATTENTE) return json({ erreur: "déjà trop de commandes en attente" }, 429);
      this.sql.exec("INSERT INTO commandes (texte, cree) VALUES (?, ?)", texte, maintenant);
      this.ajouterLigne(maintenant, `» ${texte}   (envoyée depuis la console)`);
      this.ecrire("console", maintenant);
      return json({ ok: true });
    }

    // ---- vider le journal affiché
    if (url.pathname === "/api/effacer" && request.method === "POST") {
      this.sql.exec("DELETE FROM lignes");
      return json({ ok: true });
    }
    return json({ erreur: "inconnu" }, 404);
  }
}
