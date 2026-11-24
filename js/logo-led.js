/* =====================================================================================================================
   Logo LED de La Bonne M'Air : le logo de démarrage de la carte, sur le site.
   Même dessin, pixel par pixel et aux mêmes couleurs, que l'animation de démarrage de la carte (dessinerLogo dans
   Firmware/src/main.cpp) et que la page de connexion de la console (relais/src/console.html) : la basilique qui monte,
   « LA BONNE M'AIR » écrit lettre à lettre, la barre des 6 niveaux qui se remplit, la statue qui scintille.

   Deux formats :
   - panneau : l'écran de la carte, 64 x 32 LED (introduction, écran de chargement, attente de la dernière mesure) ;
   - carré   : 32 x 32 LED pour les icônes (onglet, téléphone, tablette), l'écran d'entrée et le pied de page. Même
               basilique, même police, mêmes couleurs ;
               « M' » en grand, « AIR » en petit, et la barre des niveaux en bas à droite.

   Onglet : le logo carré s'anime au chargement de la page (comme la carte au démarrage), puis la statue scintille de
   temps en temps. Dès qu'une mesure arrive, la barre montre le niveau ATMO du verdict, comme la jauge de la carte : le
   niveau atteint est allumé, les autres restent en sourdine.
   Les icônes fixes (img/favicon.svg, favicon.ico, apple-touch-icon.png, icone-192.png, icone-512.png) sont des images de
   ce même logo carré, fabriquées avec outils/icones.html.
   ===================================================================================================================== */
(function () {
  "use strict";

  // ---------- Le dessin (recopié de main.cpp : LOGO_BASILIQUE, couleurs de dessinerLogo, police 5 x 7 d'Adafruit GFX)
  const BASILIQUE = [
    ".......G.......", "......GGG......", ".....GGGG......", "......GGG......", "......GGG......", ".....GGGGG.....",
    "......SSS......", ".....SSSSS.....", ".....S.S.S.....", ".....S.S.S.....", ".....SSSSS.....", "....SSSSSSS....",
    "....SVVVVVS....", "....S..S..S....", "....S..S..S....", "....S..S..S....", "....SSSSSSS....", "....SVVVVVS....",
    "....SSSSSSS....", "....SVVVVVS....", "..SSSSSSSSSSS..", "..SVVVVVVVVVS..", "..SS.SSSSS.SS..", "..SV.VVVVV.VS..",
    "..SSSSSSSSSSS..", ".SSSSSSSSSSSSS.", ".HHHHHHHHHHHHH.", "HHHHHHHHHHHHHHH", "hhhhhhhhhhhhhhh", "hhhhhhhhhhhhhhh",
    "hhhhhhhhhhhhhhh", "hhhhhhhhhhhhhhh"];
  const TEINTE = { G: "#ffaa00", S: "#5a96af", V: "#00965a", H: "#008c28", h: "#004614" };
  const TITRE = "#00ffe6", VERT = "#00c800", ECLAT = "#fff078";
  const NIVEAUX = ["#00c800", "#ffdc00", "#ff6400", "#ff0000", "#ff0078", "#aa00ff"];          // barre : niveau allumé
  const SOURDINE = ["#004300", "#554900", "#552100", "#550000", "#550028", "#390055"];         // ... et en sourdine
  // police 5 x 7 de la carte : une colonne par octet, bit 0 en haut
  const POLICE = {
    L: [0x7f, 0x40, 0x40, 0x40, 0x40], A: [0x7c, 0x12, 0x11, 0x12, 0x7c], B: [0x7f, 0x49, 0x49, 0x49, 0x36],
    O: [0x3e, 0x41, 0x41, 0x41, 0x3e], N: [0x7f, 0x04, 0x08, 0x10, 0x7f], E: [0x7f, 0x49, 0x49, 0x49, 0x41],
    M: [0x7f, 0x02, 0x1c, 0x02, 0x7f], I: [0x00, 0x41, 0x7f, 0x41, 0x00], R: [0x7f, 0x09, 0x19, 0x29, 0x46] };
  const FOND_PANNEAU = "#050b0d";   // le noir du panneau de LED
  const LED_ETEINTE = "#0f2128";    // une LED éteinte, à peine devinée

  // Une image de LED : px[y * l + x] = couleur, ou null pour une LED éteinte.
  function grille(l, h) {
    const g = { l, h, px: new Array(l * h).fill(null) };
    g.point = (x, y, c) => { if (x >= 0 && x < l && y >= 0 && y < h) g.px[y * l + x] = c; };
    g.texte = (t, x0, y0, k, c) => {
      for (const ch of t) {
        const glyphe = POLICE[ch];
        if (glyphe) for (let col = 0; col < 5; col++) for (let li = 0; li < 7; li++)
          if ((glyphe[col] >> li) & 1) for (let a = 0; a < k; a++) for (let b = 0; b < k; b++) g.point(x0 + col * k + a, y0 + li * k + b, c);
        x0 += 6 * k;
      }
    };
    return g;
  }

  // Basilique révélée du bas vers le haut (rangs : 0..32), décalée de dx colonnes ; eclat : étoile près de la statue.
  function basilique(g, rangs, dx, eclat) {
    for (let y = 32 - rangs; y < 32; y++) for (let x = 0; x < 15; x++) {
      const c = TEINTE[BASILIQUE[y][x]];
      if (c) g.point(x + dx, y, c);
    }
    if (eclat) {
      g.point(11 + dx, 1, ECLAT);
      g.point(10 + dx, 1, TEINTE.G); g.point(12 + dx, 1, TEINTE.G); g.point(11 + dx, 0, TEINTE.G); g.point(11 + dx, 2, TEINTE.G);
    }
  }

  // Couleur du segment i de la barre : les « niveaux » premiers allumés (démarrage), ou seulement le niveau « seul »
  // (verdict connu : comme la jauge de la carte).
  function couleurSegment(i, niveaux, seul) {
    if (seul >= 0) return i === seul ? NIVEAUX[i] : SOURDINE[i];
    return i < niveaux ? NIVEAUX[i] : SOURDINE[i];
  }

  // L'écran de la carte (64 x 32), exactement comme dessinerLogo : lettres 0..13 (« LA BONNE » puis « M'AIR »).
  function panneau(e) {
    const g = grille(64, 32);
    basilique(g, e.rangs, 0, e.eclat);
    g.texte("LA BONNE".slice(0, Math.max(0, Math.min(8, e.lettres))), 16, 1, 1, TITRE);
    const n2 = e.lettres - 8;
    if (n2 >= 1) g.texte("M", 16, 10, 2, TITRE);
    if (n2 >= 2) { for (let x = 27; x <= 28; x++) for (let y = 10; y <= 12; y++) g.point(x, y, TITRE); g.point(27, 13, TITRE); }
    if (n2 >= 3) g.texte("A", 30, 10, 2, VERT);
    if (n2 >= 4) g.texte("I", 42, 10, 2, VERT);
    if (n2 >= 5) g.texte("R", 54, 10, 2, VERT);
    for (let i = 0; i < 6; i++) {
      const xs = 16 + Math.floor((i * 49) / 6), xe = 16 + Math.floor(((i + 1) * 49) / 6) - 2;
      for (let x = xs; x <= xe; x++) for (let y = 27; y <= 29; y++) g.point(x, y, couleurSegment(i, e.niveaux, e.seul));
    }
    return g;
  }

  // Le logo carré (32 x 32) : basilique contre le bord gauche (la colline continue hors du cadre), « M' » (taille 2),
  // « AIR » (taille 1, en vert comme sur la carte), barre de 6 segments de 2 LED. Lettres 0..5 : M, ', A, I, R.
  function carre(e) {
    const g = grille(32, 32);
    basilique(g, e.rangs, -1, e.eclat);
    if (e.lettres >= 1) g.texte("M", 17, 2, 2, TITRE);                                     // x 17..26, y 2..15
    if (e.lettres >= 2) { for (let x = 28; x <= 29; x++) for (let y = 2; y <= 4; y++) g.point(x, y, TITRE); g.point(28, 5, TITRE); }
    if (e.lettres >= 3) g.texte("A", 16, 18, 1, VERT);                                     // x 16..20, y 18..24
    if (e.lettres >= 4) g.texte("I", 21, 18, 1, VERT);                                     // x 22..24 (glyphe étroit)
    if (e.lettres >= 5) g.texte("R", 26, 18, 1, VERT);                                     // x 26..30
    for (let i = 0; i < 6; i++) {                                                          // x 15..31, y 27..29
      const xs = 15 + 3 * i;
      for (let x = xs; x <= xs + 1; x++) for (let y = 27; y <= 29; y++) g.point(x, y, couleurSegment(i, e.niveaux, e.seul));
    }
    return g;
  }

  // Étapes de l'animation de démarrage : [instant (ms), rangs, lettres, niveaux, éclat]. Comme afficherSplash, en plus court.
  function etapes(nbLettres, dureeLettre) {
    const e = []; let t = 0;
    for (let r = 2; r <= 32; r += 2) { e.push([t, r, 0, 0, false]); t += 40; }
    for (let l = 1; l <= nbLettres; l++) { e.push([t, 32, l, 0, false]); t += dureeLettre(l); }
    for (let n = 1; n <= 6; n++) { e.push([t, 32, nbLettres, n, false]); t += 110; }
    for (let k = 0; k < 4; k++) { e.push([t, 32, nbLettres, 6, k % 2 === 0]); t += 140; }
    e.push([t, 32, nbLettres, 6, false]);
    return e;
  }
  const ETAPES_PANNEAU = etapes(13, (l) => (l <= 8 ? 55 : 95));
  const ETAPES_CARRE = etapes(5, () => 110);

  // ---------- Peinture
  function arrondi(ctx, x, y, l, h, r) {
    ctx.beginPath();
    if (ctx.roundRect) { ctx.roundRect(x, y, l, h, r); return; }
    ctx.moveTo(x + r, y); ctx.arcTo(x + l, y, x + l, y + h, r); ctx.arcTo(x + l, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + l, y, r); ctx.closePath();
  }
  // Peint la grille g à partir de (x0, y0), « pas » pixels par LED.
  //  style "led"   : LED allumée = halo léger + point rond, LED éteinte devinée (comme la console) ;
  //  style "pixel" : carrés pleins, nets aux petites tailles (onglet).
  function peindre(ctx, g, x0, y0, pas, style) {
    for (let y = 0; y < g.h; y++) for (let x = 0; x < g.l; x++) {
      const c = g.px[y * g.l + x];
      if (style === "pixel") {
        if (c) { ctx.fillStyle = c; ctx.fillRect(x0 + x * pas, y0 + y * pas, pas, pas); }
        continue;
      }
      const cx = x0 + (x + 0.5) * pas, cy = y0 + (y + 0.5) * pas;
      if (c) {
        ctx.globalAlpha = 0.22; ctx.fillStyle = c;
        ctx.beginPath(); ctx.arc(cx, cy, pas * 0.72, 0, 7); ctx.fill();
        ctx.globalAlpha = 1;
        ctx.beginPath(); ctx.arc(cx, cy, pas * 0.4, 0, 7); ctx.fill();
      } else {
        ctx.fillStyle = LED_ETEINTE;
        ctx.beginPath(); ctx.arc(cx, cy, pas * 0.3, 0, 7); ctx.fill();
      }
    }
  }
  // Remplit un canvas affiché (taille CSS × densité de l'écran) avec la grille, style « led ».
  function peindreCanvas(cv, g) {
    const r = Math.min(window.devicePixelRatio || 1, 3);
    const w = Math.round(cv.clientWidth * r), h = Math.round(cv.clientHeight * r);
    if (!w || !h) return;
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    const ctx = cv.getContext("2d");
    // logo carré affiché petit (moins de 5 pixels par LED : écran d'entrée, pied de page) : dessiné comme l'icône
    // d'onglet (carrés nets sur fond arrondi), les points ronds y seraient flous
    if (g.l === g.h && w === h && w / g.l < 5) { peindreIcone(ctx, g, w, true); return; }
    ctx.clearRect(0, 0, w, h);
    peindre(ctx, g, 0, 0, w / g.l, "led");
  }
  // Icône carrée de « taille » pixels. onglet : fond arrondi, carrés pleins sur tout le cadre (16 à 64 px) ;
  // sinon (téléphone, tablette) : fond plein, panneau de LED rondes centré avec une marge.
  function peindreIcone(ctx, g, taille, onglet) {
    ctx.clearRect(0, 0, taille, taille);
    if (onglet) {
      ctx.save();
      arrondi(ctx, 0, 0, taille, taille, taille * 0.14);
      ctx.clip();
      ctx.fillStyle = FOND_PANNEAU; ctx.fillRect(0, 0, taille, taille);
      peindre(ctx, g, 0, 0, taille / 32, "pixel");
      ctx.restore();
      return;
    }
    ctx.fillStyle = "#0e1b24"; ctx.fillRect(0, 0, taille, taille);
    const m = Math.round(taille * 0.09), cote = taille - 2 * m;
    ctx.fillStyle = FOND_PANNEAU;
    arrondi(ctx, m * 0.55, m * 0.55, taille - m * 1.1, taille - m * 1.1, taille * 0.06); ctx.fill();
    peindre(ctx, g, m, m, cote / 32, "led");
  }

  // ---------- Animation pas à pas : rejoue « liste » (étapes) et appelle dessiner(étape) à chaque changement.
  // Minuteries plutôt que requestAnimationFrame : l'onglet doit s'animer même quand la page est en arrière-plan.
  function jouer(liste, dessiner, fin) {
    let i = 0, minuterie = 0, arrete = false;
    const debut = performance.now();
    const suivant = () => {
      if (arrete) return;
      const t = performance.now() - debut;
      while (i + 1 < liste.length && liste[i + 1][0] <= t) i++;   // en retard (onglet ralenti) : on saute à l'étape due
      dessiner(liste[i]);
      if (i >= liste.length - 1) { if (fin) fin(); return; }
      minuterie = setTimeout(suivant, Math.max(0, liste[i + 1][0] - (performance.now() - debut)));
      i++;
    };
    suivant();
    return () => { arrete = true; clearTimeout(minuterie); };
  }
  const animationsReduites = () => document.documentElement.classList.contains("mouvement-reduit");

  // ---------- Onglet animé
  const onglet = (() => {
    let lien = null, ctx = null, seul = -1, arreter = null, scintille = 0, derniere = "";
    const TAILLE = 64;   // 2 pixels par LED : net en 16 et 32 px (écrans standard et haute densité)
    function poser(e) {
      if (!ctx) return;
      peindreIcone(ctx, carre(e), TAILLE, true);
      const url = ctx.canvas.toDataURL("image/png");
      if (url === derniere) return;
      derniere = url;
      lien.type = "image/png";
      lien.href = url;
    }
    const repos = (eclat) => poser({ rangs: 32, lettres: 5, niveaux: 6, eclat: !!eclat, seul });
    // Au repos : la statue scintille toutes les 9 s (deux éclats), comme à la fin du démarrage de la carte.
    function programmerScintillement() {
      clearTimeout(scintille);
      if (animationsReduites()) return;
      scintille = setTimeout(() => {
        if (arreter || animationsReduites()) { programmerScintillement(); return; }
        arreter = jouer([[0, 1], [160, 0], [320, 1], [480, 0]], (e) => repos(e[1] === 1), () => { arreter = null; programmerScintillement(); });
      }, 9000);
    }
    function demarrer() {
      lien = document.getElementById("favicon");
      const cv = document.createElement("canvas");
      cv.width = cv.height = TAILLE;
      ctx = cv.getContext && cv.getContext("2d");
      if (!lien || !ctx) return;
      // Le navigateur choisit son icône d'onglet parmi toutes les balises « icon » : on ne garde que celle qu'on anime
      // (64 x 64) et les grandes (192, 512 : écran d'accueil des téléphones), sinon favicon.ico (16, 32 px) passerait devant.
      for (const l of document.querySelectorAll('link[rel~="icon"]'))
        if (l !== lien && !/192|512/.test(l.getAttribute("sizes") || "")) l.remove();
      lien.setAttribute("sizes", TAILLE + "x" + TAILLE);
      if (animationsReduites()) { repos(false); return; }
      arreter = jouer(ETAPES_CARRE, ([, rangs, lettres, niveaux, eclat]) => poser({ rangs, lettres, niveaux, eclat, seul: -1 }),
        () => { arreter = null; repos(false); programmerScintillement(); });
    }
    // Niveau ATMO du verdict (0 à 5), ou -1 sans verdict : la barre se remplit puis ne garde que ce niveau allumé.
    function niveau(n) {
      n = Number.isInteger(n) && n >= 0 && n < 6 ? n : -1;
      if (n === seul) return;
      seul = n;
      if (!ctx || arreter) return;   // démarrage en cours : il finira sur ce niveau
      if (animationsReduites()) { repos(false); return; }
      clearTimeout(scintille);
      const barre = [];
      for (let k = 0; k <= 6; k++) barre.push([k * 70, k]);
      arreter = jouer(barre, ([, k]) => poser({ rangs: 32, lettres: 5, niveaux: k, eclat: false, seul: k < 6 ? -1 : seul }),
        () => { arreter = null; repos(false); programmerScintillement(); });
    }
    return { demarrer, niveau };
  })();

  // ---------- Logos vivants dans la page (introduction, chargement, écran d'entrée, pied de page)
  // creer(canvas, options) pilote un logo dans un canvas affiché :
  //   forme      : "panneau" (l'écran de la carte, 64 x 32) ou "carre" (32 x 32, comme l'icône d'onglet) ;
  //   scintiller : logo complet, la statue scintille de temps en temps (seulement s'il est à l'écran) ;
  //   boucle     : le démarrage se rejoue sans fin (attente de données), seulement s'il est à l'écran ;
  //   aVue       : le démarrage se joue la première fois que le logo arrive à l'écran.
  // Méthodes : eteindre() (toutes les LED éteintes), demarrer(fin) (rejoue le démarrage de la carte ; fin() quand le
  // logo est complet), figer() (logo complet, plus d'animation). Le canvas est repeint à chaque changement de taille.
  // Animations réduites (bouton du pied de page) : logo complet tout de suite, sans éclat ni boucle.
  function creer(cv, opts) {
    opts = opts || {};
    const estCarre = opts.forme === "carre";
    const dessin = estCarre ? carre : panneau;
    const etapesLogo = estCarre ? ETAPES_CARRE : ETAPES_PANNEAU;
    const complet = { rangs: 32, lettres: estCarre ? 5 : 13, niveaux: 6, eclat: false, seul: -1 };
    let image = dessin(complet), arreter = null, minuterie = 0, visible = false, enAttente = false, vu = false;
    let etat = "complet";
    const repeindre = () => peindreCanvas(cv, image);
    const poser = (e) => { image = dessin(e); repeindre(); };
    function stop() { if (arreter) { arreter(); arreter = null; } clearTimeout(minuterie); enAttente = false; }
    // après le démarrage : éclats (scintiller) ou nouveau démarrage (boucle), seulement si le logo est vu
    function suite() {
      if (animationsReduites()) return;
      if (opts.boucle) { minuterie = setTimeout(() => (visible && !document.hidden ? ctl.demarrer() : (enAttente = true)), 1300); return; }
      if (!opts.scintiller) return;
      minuterie = setTimeout(() => {
        if (!visible || document.hidden) { enAttente = true; return; }
        arreter = jouer([[0, 1], [140, 0], [280, 1], [420, 0]],
          (e) => poser(Object.assign({}, complet, { eclat: e[1] === 1 })), () => { arreter = null; suite(); });
      }, 5000 + Math.random() * 4000);
    }
    const ctl = {
      eteindre() { stop(); etat = "eteint"; poser({ rangs: 0, lettres: 0, niveaux: 0, eclat: false, seul: -1 }); },
      demarrer(fin) {
        stop();
        if (animationsReduites()) { etat = "complet"; poser(complet); if (fin) fin(); return; }
        etat = "anime";
        arreter = jouer(etapesLogo, ([, rangs, lettres, niveaux, eclat]) => poser({ rangs, lettres, niveaux, eclat, seul: -1 }),
          () => { arreter = null; etat = "complet"; if (fin) fin(); suite(); });
      },
      figer() { stop(); etat = "complet"; poser(complet); },
      get etat() { return etat; },
    };
    // Taille : repeint dès que le canvas change de taille (fenêtre, plein écran, élément qui réapparaît).
    if ("ResizeObserver" in window) new ResizeObserver(repeindre).observe(cv);
    else window.addEventListener("resize", repeindre);
    // À l'écran ou non : les éclats et la boucle attendent que le logo soit vu.
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(([e]) => {
        visible = e.isIntersecting;
        if (!visible) return;
        if (opts.aVue && !vu) { vu = true; ctl.demarrer(); return; }
        if (enAttente) { enAttente = false; suite(); }
      }).observe(cv);
    } else {
      visible = true;
      if (opts.aVue) ctl.demarrer();
    }
    document.addEventListener("visibilitychange", () => { if (!document.hidden && visible && enAttente) { enAttente = false; suite(); } });
    if (opts.aVue) ctl.eteindre(); else repeindre();
    return ctl;
  }

  window.LogoLED = {
    panneau, carre, peindreIcone, peindreCanvas, creer,
    onglet, ETAPES_PANNEAU, ETAPES_CARRE, FOND_PANNEAU, TITRE,
    DUREE_PANNEAU_MS: ETAPES_PANNEAU[ETAPES_PANNEAU.length - 1][0],
  };
  // L'onglet s'anime tout de suite (la balise <link id="favicon"> est dans le <head>) ; page d'outil sans onglet animé :
  // rien à faire.
  if (document.getElementById("favicon")) onglet.demarrer();
})();
