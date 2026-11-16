/* i18n.js : petit moteur de traduction, sans dépendance.

   Les textes vivent dans lang/fr.js, lang/en.js, lang/ja.js (un fichier par langue, chargés AVANT ce fichier).
   Pour ajouter une langue :
     1. copier lang/fr.js en lang/xx.js et traduire les valeurs (jamais les clés) ;
     2. ajouter <script src="lang/xx.js"></script> dans index.html, avant js/i18n.js ;
     3. ajouter la langue dans LANGUES ci-dessous ;
     4. ajouter un bouton <button data-lang="xx"> dans les deux sélecteurs de langue d'index.html.

   Dans le HTML :
     data-i18n="clé"                    remplace le texte de l'élément
     data-i18n-html="clé"               idem, mais le texte peut contenir du HTML (<em>, <mark>…)
     data-i18n-attr="attribut:clé;…"    traduit des attributs (aria-label, title, content, data-titre…)
   Dans le JavaScript :
     I18N.t("clé", { n: 3 })            texte avec des variables {n}
     I18N.tp("clé", 3, { n: 3 })        texte avec pluriel (clé.one / clé.other)
*/
(function () {
  "use strict";

  // Langues proposées : code → nom écrit dans la langue elle-même, et réglage régional (nombres, dates).
  const LANGUES = {
    fr: { nom: "Français", locale: "fr-FR" },
    en: { nom: "English",  locale: "en-GB" },
    ja: { nom: "日本語",    locale: "ja-JP" },
  };
  const DEFAUT = "fr";                 // langue d'origine du site, et langue de repli
  const CLE_STOCKAGE = "langue-site";  // mémorise le dernier choix de la personne (localStorage)

  const DICO = window.I18N_DICT = window.I18N_DICT || {};
  const ecouteurs = [];
  let langue = DEFAUT;

  const disponible = code => !!(code && LANGUES[code] && DICO[code]);

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
      const code = String(p).slice(0, 2).toLowerCase();
      if (disponible(code)) return code;
    }
    return DEFAUT;
  }
  langue = langueInitiale();

  // Texte d'une clé dans la langue courante, sinon en français, sinon la clé elle-même (visible : on repère l'oubli).
  function t(cle, vars) {
    let s = DICO[langue] && DICO[langue][cle];
    if (s === undefined) s = DICO[DEFAUT] && DICO[DEFAUT][cle];
    if (s === undefined) return cle;
    if (vars) s = s.replace(/\{(\w+)\}/g, (m, k) => (vars[k] !== undefined ? vars[k] : m));
    return s;
  }

  // Pluriel : "clé.one" ou "clé.other" selon la règle de la langue (en français, 0 et 1 sont au singulier).
  function tp(cle, n, vars) {
    let categorie = "other";
    try { categorie = new Intl.PluralRules(LANGUES[langue].locale).select(n); } catch (e) { /* ancien navigateur */ }
    const dico = DICO[langue] || {};
    const k = (dico[cle + "." + categorie] !== undefined) ? cle + "." + categorie : cle + ".other";
    return t(k, vars);
  }

  // Applique les traductions à tout le document (ou à une partie : appliquer(element)).
  function appliquer(racine) {
    const r = racine || document;
    r.querySelectorAll("[data-i18n]").forEach(e => { e.textContent = t(e.getAttribute("data-i18n")); });
    r.querySelectorAll("[data-i18n-html]").forEach(e => { e.innerHTML = t(e.getAttribute("data-i18n-html")); });
    r.querySelectorAll("[data-i18n-attr]").forEach(e => {
      e.getAttribute("data-i18n-attr").split(";").forEach(paire => {
        const i = paire.indexOf(":");
        if (i > 0) e.setAttribute(paire.slice(0, i).trim(), t(paire.slice(i + 1).trim()));
      });
    });
  }

  // Les caractères japonais ne sont dans aucune des polices du site : on charge Noto Sans JP, seulement si besoin.
  function chargerPoliceJaponaise() {
    if (document.getElementById("police-japonaise")) return;
    const lien = document.createElement("link");
    lien.id = "police-japonaise";
    lien.rel = "stylesheet";
    lien.href = "https://fonts.googleapis.com/css2?family=Noto+Sans+JP:wght@400;500;700;900&display=swap";
    document.head.appendChild(lien);
  }

  // Applique la langue : <html lang>, textes du HTML, titre de l'onglet, état des boutons.
  function appliquerLangue() {
    const racine = document.documentElement;
    racine.lang = langue;
    if (langue === "ja") chargerPoliceJaponaise();
    appliquer();
    document.title = t("meta.titre");   // le script de la page l'ajuste ensuite selon le verdict
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
    ecouteurs.forEach(f => { try { f(code); } catch (e) { console.error(e); } });
  }

  // À appeler une fois, à la fin de la page, avant de démarrer le script principal.
  function initialiser() {
    appliquerLangue();
    document.addEventListener("click", e => {
      const b = e.target.closest && e.target.closest("[data-lang]");
      if (b) definir(b.getAttribute("data-lang"));
    });
  }

  window.I18N = {
    LANGUES,
    t,
    tp,
    appliquer,
    definir,
    initialiser,
    langue: () => langue,
    locale: () => LANGUES[langue].locale,
    surChangement: f => { ecouteurs.push(f); },
  };
})();
