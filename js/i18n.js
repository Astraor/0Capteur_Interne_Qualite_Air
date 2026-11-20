/* i18n.js : petit moteur de traduction, sans dépendance.

   Les textes vivent dans lang/fr.js, lang/en.js, lang/ja.js (un fichier par langue, chargés AVANT ce fichier).
   Pour ajouter une langue :
     1. copier lang/fr.js en lang/xx.js et traduire les valeurs (jamais les clés) ;
     2. ajouter <script src="lang/xx.js"></script> dans index.html, avant js/i18n.js ;
     3. ajouter la langue dans LANGUES ci-dessous ;
     4. ajouter un bouton <button data-lang="xx"> dans chacun des sélecteurs de langue d'index.html.

   Dans le HTML :
     data-i18n="clé"                    remplace le texte de l'élément
     data-i18n-html="clé"               idem, mais le texte peut contenir du HTML (<em>, <mark>…)
     data-i18n-attr="attribut:clé;…"    traduit des attributs (aria-label, title, content, data-titre…)
   Dans le JavaScript :
     I18N.t("clé", { n: 3 })            texte avec des variables {n}
     I18N.tp("clé", 3, { n: 3 })        texte avec pluriel (clé.one / clé.other)
     I18N.nombre(1234.5, 1)             nombre formaté dans la langue courante (formateur mis en cache)
     I18N.date(timestamp, options)      date / heure formatée dans la langue courante (formateur mis en cache)
     I18N.surChangement(code => {…})    appelé après chaque changement de langue
     I18N.verifier()                    (à lancer dans la console) liste les clés manquantes / en trop par langue

   ORDRE DE CHARGEMENT, à la fin de <body> :
     <script src="lang/fr.js"></script> <script src="lang/en.js"></script> <script src="lang/ja.js"></script>
     <script src="js/i18n.js"></script>
     <script> I18N.initialiser(); … script principal … </script>
*/
(function () {
  "use strict";

  // Langues proposées : code → nom écrit dans la langue elle-même, réglage régional (nombres, dates)
  // et, si besoin, dir: "rtl" (arabe, hébreu…).
  const LANGUES = {
    fr: { nom: "Français", locale: "fr-FR" },
    en: { nom: "English",  locale: "en-GB" },
    ja: { nom: "日本語",    locale: "ja-JP" },
  };
  const DEFAUT = "fr";                 // langue d'origine du site, et langue de repli
  const CLE_STOCKAGE = "langue-site";  // mémorise le dernier choix de la personne (localStorage)
  const SELECTEUR = "[data-i18n],[data-i18n-html],[data-i18n-attr]";
  const POLICE_JAPONAISE = "https://fonts.googleapis.com/css2?family=Noto+Sans+JP:wght@400;500;700;900&display=swap";

  const DICO = window.I18N_DICT = window.I18N_DICT || {};
  const ecouteurs = [];
  const avertis = new Set();                 // un avertissement par clé et par langue, pas un par appel
  const formateurs = { nombres: {}, dates: new WeakMap(), pluriels: {} };   // Intl.* coûte cher à créer : un seul par réglage
  const attributs = new WeakMap();           // "attr:clé;attr:clé" déjà découpé, par élément
  let langue = DEFAUT;
  let initialise = false;

  const disponible = code => !!(code && LANGUES[code] && DICO[code]);

  function avertir(id, message) {
    if (avertis.has(id)) return;
    avertis.add(id);
    if (window.console && console.warn) console.warn("[i18n] " + message);
  }

  // Ordre de priorité : ?lang=ja dans l'adresse, puis le dernier choix, puis la langue du navigateur, puis le français.
  function langueInitiale() {
    try {
      const q = new URLSearchParams(window.location.search).get("lang");
      if (disponible(q)) return q;
    } catch (e) { /* adresse illisible : on passe au critère suivant */ }
    try {
      const s = window.localStorage.getItem(CLE_STOCKAGE);
      if (disponible(s)) return s;
    } catch (e) { /* stockage bloqué (navigation privée…) */ }
    const prefs = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language || ""];
    for (const p of prefs) {
      const code = String(p).split("-")[0].toLowerCase();
      if (disponible(code)) return code;
    }
    return DEFAUT;
  }

  if (!DICO[DEFAUT]) avertir("defaut", "lang/" + DEFAUT + ".js n'est pas chargé : il doit l'être avant js/i18n.js.");
  langue = langueInitiale();
  document.documentElement.lang = langue;   // tout de suite : le navigateur choisit les bons guillemets, césures et polices

  // ---------- Texte d'une clé ----------
  // Langue courante, sinon français (avec un avertissement dans la console), sinon la clé elle-même (visible : on repère l'oubli).
  function chercher(cle) {
    const d = DICO[langue];
    if (d && d[cle] !== undefined) return d[cle];
    const r = DICO[DEFAUT];
    if (r && r[cle] !== undefined) {
      if (langue !== DEFAUT) avertir(langue + ":" + cle, "clé « " + cle + " » absente de lang/" + langue + ".js : texte " + DEFAUT + " utilisé.");
      return r[cle];
    }
    avertir("?:" + cle, "clé « " + cle + " » introuvable.");
    return undefined;
  }

  const existe = cle => !!((DICO[langue] && DICO[langue][cle] !== undefined) || (DICO[DEFAUT] && DICO[DEFAUT][cle] !== undefined));

  function t(cle, vars) {
    let s = chercher(cle);
    if (s === undefined) return cle;
    if (vars && s.indexOf("{") !== -1) {
      s = s.replace(/\{(\w+)\}/g, (m, k) => {
        if (vars[k] !== undefined) return vars[k];
        avertir("v:" + cle + ":" + k, "variable {" + k + "} non fournie pour « " + cle + " ».");
        return m;
      });
    }
    return s;
  }

  // Pluriel : "clé.one" ou "clé.other" selon la règle de la langue (en français, 0 et 1 sont au singulier).
  // Une langue sans forme "one" (le japonais) n'a besoin que de "clé.other".
  function tp(cle, n, vars) {
    const locale = LANGUES[langue].locale;
    let regles = formateurs.pluriels[locale];
    if (!regles) {
      try { regles = new Intl.PluralRules(locale); } catch (e) { regles = { select: () => "other" }; }
      formateurs.pluriels[locale] = regles;
    }
    const categorie = regles.select(n);
    const d = DICO[langue] || {};
    const r = DICO[DEFAUT] || {};
    let k = cle + ".other";
    if (d[cle + "." + categorie] !== undefined) k = cle + "." + categorie;
    else if (d[k] === undefined && r[cle + "." + categorie] !== undefined) k = cle + "." + categorie;
    return t(k, vars);
  }

  // ---------- Nombres et dates (formateurs mis en cache) ----------
  // Sans "decimales" : jusqu'à 1 décimale si besoin. Appelé à chaque image pendant le décompte des chiffres :
  // créer un formateur à chaque fois (toLocaleString) était coûteux.
  function nombre(valeur, decimales) {
    const locale = LANGUES[langue].locale;
    const cle = locale + "|" + (decimales === undefined ? "auto" : decimales);
    let f = formateurs.nombres[cle];
    if (!f) {
      const options = (decimales === undefined)
        ? { maximumFractionDigits: 1 }
        : { minimumFractionDigits: decimales, maximumFractionDigits: decimales };
      try { f = new Intl.NumberFormat(locale, options); } catch (e) { f = { format: v => String(v) }; }
      formateurs.nombres[cle] = f;
    }
    return f.format(valeur);
  }

  // Un formateur par objet d'options ET par langue. Les graphes appellent date() pour chacun de leurs points avec
  // le même objet d'options : on le retrouve par son identité (WeakMap), sans le sérialiser à chaque appel.
  const OPTIONS_VIDES = {};
  function date(valeur, options) {
    const locale = LANGUES[langue].locale;
    const opts = options || OPTIONS_VIDES;
    let parLocale = formateurs.dates.get(opts);
    if (!parLocale) { parLocale = {}; formateurs.dates.set(opts, parLocale); }
    let f = parLocale[locale];
    if (!f) {
      try { f = new Intl.DateTimeFormat(locale, options); } catch (e) { f = { format: v => String(v) }; }
      parLocale[locale] = f;
    }
    try { return f.format(valeur instanceof Date ? valeur : new Date(valeur)); }
    catch (e) { return "—"; }   // date invalide
  }

  // ---------- Application au document ----------
  // Applique les traductions à tout le document (ou à une partie : appliquer(element)).
  // Une seule recherche dans le DOM ; une écriture seulement si le texte change (pas de repeinture inutile).
  const originaux = new WeakMap();   // contenu HTML d'origine de chaque élément traduit (secours)
  function appliquer(racine) {
    const r = racine || document;
    const noeuds = Array.from(r.querySelectorAll(SELECTEUR));
    if (r.matches && r.matches(SELECTEUR)) noeuds.unshift(r);
    for (let i = 0; i < noeuds.length; i++) {
      const e = noeuds[i];
      // Clé absente du dictionnaire (fichier de langue encore en cache, ancienne version) : on garde le texte français
      // écrit dans le HTML au lieu d'afficher le nom de la clé (« pied.devise »…).
      if (!originaux.has(e)) originaux.set(e, e.innerHTML);
      let cle = e.getAttribute("data-i18n");
      if (cle !== null) {
        if (existe(cle)) {
          const v = t(cle);
          if (e.textContent !== v) e.textContent = v;
        } else if (e.innerHTML !== originaux.get(e)) e.innerHTML = originaux.get(e);
      }
      cle = e.getAttribute("data-i18n-html");
      if (cle !== null) e.innerHTML = existe(cle) ? t(cle) : originaux.get(e);
      const brut = e.getAttribute("data-i18n-attr");
      if (brut !== null) {
        let paires = attributs.get(e);
        if (!paires || paires.brut !== brut) {
          paires = { brut, liste: [] };
          brut.split(";").forEach(paire => {
            const k = paire.indexOf(":");
            if (k > 0) paires.liste.push([paire.slice(0, k).trim(), paire.slice(k + 1).trim()]);
          });
          attributs.set(e, paires);
        }
        paires.liste.forEach(([attr, c]) => {
          const v = t(c);
          if (e.getAttribute(attr) !== v) e.setAttribute(attr, v);
        });
      }
    }
  }

  // Les caractères japonais ne sont dans aucune des polices du site : on charge Noto Sans JP, seulement si besoin.
  function chargerPoliceJaponaise() {
    if (document.getElementById("police-japonaise")) return;
    const lien = document.createElement("link");
    lien.id = "police-japonaise";
    lien.rel = "stylesheet";
    lien.href = POLICE_JAPONAISE;
    document.head.appendChild(lien);
  }

  // Titre de l'onglet et description : traduits sans avoir besoin d'attribut dans le HTML.
  function appliquerMeta() {
    if (existe("meta.titre")) document.title = t("meta.titre");   // le script de la page l'ajuste ensuite selon le verdict
    const d = document.querySelector('meta[name="description"]');
    if (d && existe("meta.description")) d.setAttribute("content", t("meta.description"));
  }

  // Applique la langue : <html lang dir>, textes du HTML, titre de l'onglet, état des boutons.
  function appliquerLangue() {
    const racine = document.documentElement;
    racine.lang = langue;
    racine.dir = LANGUES[langue].dir || "ltr";
    if (langue === "ja") chargerPoliceJaponaise();
    appliquer();
    appliquerMeta();
    document.querySelectorAll("[data-lang]").forEach(b => {
      b.setAttribute("aria-pressed", String(b.getAttribute("data-lang") === langue));
    });
  }

  // Changement de langue demandé par la personne (bouton) : mémorisé, puis les écouteurs de la page sont prévenus.
  function definir(code) {
    if (!disponible(code) || code === langue) return;
    langue = code;
    try { window.localStorage.setItem(CLE_STOCKAGE, code); } catch (e) { /* tant pis : le choix ne sera pas mémorisé */ }
    try {   // l'adresse garde la langue : un lien copié s'ouvre dans la même langue
      const url = new URL(window.location.href);
      url.searchParams.set("lang", code);
      window.history.replaceState(null, "", url);
    } catch (e) { /* adresse non modifiable (ouverture depuis un fichier, par exemple) */ }
    appliquerLangue();
    ecouteurs.slice().forEach(f => { try { f(code); } catch (e) { console.error(e); } });
  }

  // À appeler une fois, à la fin de la page, avant de démarrer le script principal.
  // (Appelée deux fois, la deuxième est ignorée : les clics ne sont pas écoutés en double.)
  function initialiser() {
    if (initialise) return;
    initialise = true;
    appliquerLangue();
    document.addEventListener("click", e => {
      const b = e.target.closest && e.target.closest("[data-lang]");
      if (b) definir(b.getAttribute("data-lang"));
    });
  }

  // Outil de mise au point, à lancer dans la console : I18N.verifier()
  // Pour chaque langue : clés du français absentes de la langue (manquantes), et clés qui n'existent pas en français (en trop).
  function verifier() {
    const ref = DICO[DEFAUT] || {};
    const rapport = {};
    Object.keys(LANGUES).forEach(code => {
      if (code === DEFAUT) return;
      const d = DICO[code];
      if (!d) { rapport[code] = "fichier lang/" + code + ".js non chargé"; return; }
      rapport[code] = {
        manquantes: Object.keys(ref).filter(k => d[k] === undefined && !(/\.one$/.test(k) && d[k.replace(/\.one$/, ".other")] !== undefined)),
        enTrop: Object.keys(d).filter(k => ref[k] === undefined),
      };
    });
    if (window.console && console.table) console.table(rapport);
    return rapport;
  }

  window.I18N = {
    LANGUES,
    t,
    tp,
    existe,
    nombre,
    date,
    appliquer,
    definir,
    initialiser,
    verifier,
    langue: () => langue,
    locale: () => LANGUES[langue].locale,
    surChangement: f => {
      ecouteurs.push(f);
      return () => { const i = ecouteurs.indexOf(f); if (i >= 0) ecouteurs.splice(i, 1); };
    },
  };
})();