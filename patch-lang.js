// Usage : node patch-lang.js lang/fr.js lang/en.js lang/ja.js
// Remplace, clé par clé, les textes modifiés (texte seulement). Chaque clé doit exister une seule fois.
const fs = require("fs");
const N = "\\u00a0";
const T = {
  fr: {
    "ep.titre": "Pour une meilleure visibilité",
    "manifeste.texte": "Des capteurs mesurent en continu le CO₂, les particules fines, la température et l’humidité afin d’évaluer la qualité de l’air de votre habitat.",
    "acc.resume": "Vue d’ensemble",
    "seuil.oms": " (Atmosud)",
    "comp.seuils.p1": `Particules${N}: seuils de l'indice ATMO publiés par Atmosud (arrêté du 10 juillet 2020). Ces seuils portent sur des moyennes journalières${N}: ici ils servent de repère pour la dernière mesure, à titre indicatif. CO₂${N}: 800${N}ppm est l'objectif du HCSP${N}; 1${N}000 et 1${N}500${N}ppm sont des repères usuels pour l'aération des salles. PM1${N}: l'indice ATMO ne fixe pas de seuil, on applique ici les mêmes limites que pour les PM2.5.`,
    "comp.seuils.p2": `Sources${N}: Atmosud, <em>L'indice ATMO de la qualité de l'air</em> (atmosud.org)${N}; arrêté du 10 juillet 2020 relatif à l'indice de la qualité de l'air ambiant${N}; HCSP, recommandations sur l'aération et la ventilation des établissements.`,
    "pied.niveaux.dd": `Seuils de l'indice ATMO (Atmosud) pour les particules et du HCSP pour le CO₂, détaillés dans «${N}Comprendre les mesures${N}». Capteur à bas coût${N}: les valeurs sont indicatives et n'ont pas de valeur réglementaire.`,
  },
  en: {
    "ep.titre": "For better visibility",
    "manifeste.texte": "Sensors continuously measure CO₂, fine particles, temperature and humidity to assess the air quality of your home.",
    "acc.resume": "Overview",
    "seuil.oms": " (Atmosud)",
    "comp.seuils.p1": `Particles: thresholds of the ATMO index published by Atmosud (order of 10 July 2020). These thresholds apply to daily averages: here they serve as a guide for the latest reading, for information only. CO₂: 800${N}ppm is the HCSP target; 1,000 and 1,500${N}ppm are common reference points for ventilating rooms. PM1: the ATMO index sets no threshold, so the same limits as for PM2.5 are applied here.`,
    "comp.seuils.p2": `Sources: Atmosud, <em>The ATMO air quality index</em> (atmosud.org); order of 10 July 2020 on the ambient air quality index; HCSP, recommendations on ventilation and air renewal in public buildings.`,
    "pied.niveaux.dd": `ATMO index thresholds (Atmosud) for particles and HCSP thresholds for CO₂, detailed in “Understanding the readings”. Low-cost sensor: the values are indicative and have no regulatory value.`,
  },
  ja: {
    "ep.titre": "より見やすくするために",
    "manifeste.texte": "センサーが、CO₂、微小粒子、温度、湿度を絶えず測定し、お住まいの空気の質を評価します。",
    "acc.resume": "全体像",
    "seuil.oms": " (Atmosud)",
    "comp.seuils.p1": `粒子:Atmosudが公表しているATMO指数の基準値(2020年7月10日の省令)を用いています。これらの基準値は日平均に対するものですが、ここでは最新の測定値の目安として、参考程度に使っています。CO₂:800${N}ppmはHCSPの目標値で、1,000${N}ppmと1,500${N}ppmは部屋の換気の目安として一般的に使われる値です。PM1:ATMO指数には基準値がないため、ここではPM2.5と同じ基準を適用しています。`,
    "comp.seuils.p2": `出典:Atmosud『ATMO大気質指数』(atmosud.org)、2020年7月10日の省令(大気質指数に関するもの)、HCSP『施設の換気と空気の入れ替えに関する勧告』。`,
    "pied.niveaux.dd": `粒子にはATMO指数(Atmosud)、CO₂にはHCSPの基準値を使用しており、詳細は「測定値の見方」に記載しています。低価格センサーのため、値は参考値であり、法的な効力はありません。`,
  },
};
for (const f of process.argv.slice(2)) {
  const code = (f.match(/(fr|en|ja)\.js$/) || [])[1];
  if (!code) { console.error("langue non reconnue : " + f); process.exit(1); }
  let s = fs.readFileSync(f, "utf8");
  for (const [k, v] of Object.entries(T[code])) {
    const re = new RegExp('("' + k.replace(/\./g, "\\.") + '":\\s*)"(?:[^"\\\\]|\\\\.)*"');
    if (!re.test(s)) { console.error(f + " : clé introuvable " + k); process.exit(1); }
    s = s.replace(re, (m, p) => p + JSON.stringify(v).replace(/\\\\u00a0/g, "\\u00a0"));
  }
  fs.writeFileSync(f, s);
  console.log(f + " : " + Object.keys(T[code]).length + " clés mises à jour");
}