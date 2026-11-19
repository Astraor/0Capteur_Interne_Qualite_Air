#!/usr/bin/env python3
"""Simule plusieurs mises sous tension successives de la carte (coupures de courant, redémarrages, watchdog)
avec un scénario d'une journée, puis analyse ce que le serveur a reçu et rend les images de l'écran LED.

Usage : python3 journee.py [binaire de la carte] [dossier de sortie]   (après sh compiler.sh)
Résultats : bilan à l'écran, journal série dans console.txt, planches de l'écran LED dans ecran/*.png
"""
import datetime as dt, json, os, struct, subprocess, sys, shutil
from zoneinfo import ZoneInfo

ICI = os.path.dirname(os.path.abspath(__file__))
CARTE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ICI, "build", "carte")
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "sortie", "journee")
PARIS = ZoneInfo("Europe/Paris")
CLE = "0123456789abcdef0123456789abcdef"

def ep(jour, h, m=0, s=0):
    return int(dt.datetime(2026, 11, jour, h, m, s, tzinfo=PARIS).timestamp())

# ---------------------------------------------------------------- scénario
SCENARIO = [
    f"cle {CLE}",
    "ap MaBox -58 motdepasse123",
    "ap eduroam -71 - ent",
    "ap Voisin -83 autremdp99",
    # carte neuve : on se trompe de mot de passe, puis on corrige, puis la clé
    f"cmd {ep(19, 7, 0, 20)} wifi add MaBox mauvaismdp1",
    f"cmd {ep(19, 7, 1, 30)} wifi add MaBox motdepasse123",
    f"cmd {ep(19, 7, 2, 0)} set cle {CLE}",
    f"cmd {ep(19, 7, 2, 30)} status",
    f"cmd {ep(19, 7, 3, 0)} wifi list",
    f"cmd {ep(19, 13, 30)} status",             # pendant la coupure Wi-Fi
    f"cmd {ep(19, 15, 5)} status",              # pendant le rattrapage
    f"cmd {ep(19, 21, 20)} status",
    f"cmd {ep(20, 8, 30)} status",
    # incidents
    f"coupure_wifi {ep(19, 13, 0)} {ep(19, 15, 0)}",       # 2 h sans Wi-Fi : stockage puis rattrapage
    f"panne_serveur {ep(19, 16, 0)} {ep(19, 16, 10)} 503",  # serveur en maintenance 10 min
    f"batterie {ep(19, 16, 30)} {ep(19, 16, 45)}",          # coupure secteur (sur batterie)
    f"pollution {ep(19, 18, 0)} {ep(19, 18, 50)} 95",       # épisode de particules (cuisine, travaux...)
    f"coupure_wifi {ep(19, 20, 5)} {ep(19, 21, 15)}",      # Wi-Fi absent au redémarrage après la coupure de courant
    f"fenetre {ep(19, 11, 40)} {ep(19, 11, 50)}",
    "proba429 0.01",
    f"coupure_wifi {ep(20, 0, 30)} {ep(20, 6, 30)}",        # borne éteinte la nuit (vu sur le vrai serveur)
    f"derive_co2 {ep(20, 1, 0)} {ep(20, 6, 0)} 420",          # le CO2 dérive jusqu'à des valeurs impossibles (vu : 37 ppm)
    f"images {ep(20, 3, 25)} {ep(20, 3, 27)}",
    # fenêtres où l'écran est enregistré
    f"images {ep(19, 7, 0)} {ep(19, 7, 6)}",
    f"images {ep(19, 9, 55)} {ep(19, 10, 32)}",
    f"images {ep(19, 13, 0)} {ep(19, 13, 4)}",
    f"images {ep(19, 18, 10)} {ep(19, 18, 30)}",
    f"images {ep(19, 20, 10)} {ep(19, 20, 14)}",
    f"images {ep(20, 2, 0)} {ep(20, 2, 2)}",
]
# Mises sous tension : (début, fin). Entre deux : courant coupé.
ALIMENTATIONS = [
    (ep(19, 7, 0), ep(19, 20, 0)),     # journée
    (ep(19, 20, 10), ep(19, 20, 55)),  # retour du courant... Wi-Fi absent, nouvelle coupure avant l'heure NTP
    (ep(19, 21, 0), ep(20, 9, 0)),     # nuit et matin suivant
]

if os.path.exists(SORTIE):
    shutil.rmtree(SORTIE)
os.makedirs(SORTIE)
etat = os.path.join(SORTIE, "etat")
os.makedirs(etat)
open(os.path.join(SORTIE, "scenario.txt"), "w").write("\n".join(SCENARIO) + "\n")

reboots = []
for debut, fin in ALIMENTATIONS:
    t, raison, ntp = debut, 1, "0"
    while t < fin - 5:
        env = dict(os.environ, SIM_DOSSIER=etat, SIM_EPOCH_BOOT=str(t), SIM_DUREE_S=str(fin - t),
                   SIM_RESET=str(raison), SIM_SCENARIO=os.path.join(SORTIE, "scenario.txt"), SIM_GRAINE=str(t % 9973), SIM_NTP=ntp)
        with open(os.path.join(SORTIE, "console.txt"), "a") as console:
            code = subprocess.run([CARTE], env=env, stdout=console, stderr=subprocess.STDOUT, timeout=1800).returncode
        arret = open(os.path.join(etat, "arret.txt")).read().split()
        t_arret = int(arret[1])
        ntp = arret[2] if len(arret) > 2 else "0"
        reboots.append((t, t_arret, code))
        if code == 0:
            break
        raison = {42: 3, 43: 6}.get(code, 4)   # 42 ESP.restart -> SW | 43 watchdog -> TASK_WDT | autre -> PANIC
        t = t_arret + 1

print("Mises sous tension :")
for a, b, c in reboots:
    print(f"  {dt.datetime.fromtimestamp(a, PARIS):%d/%m %H:%M} -> {dt.datetime.fromtimestamp(b, PARIS):%d/%m %H:%M}  code {c}")

# ---------------------------------------------------------------- ce que le serveur a reçu
recu = [json.loads(l) for l in open(os.path.join(etat, "serveur.jsonl"))] if os.path.exists(os.path.join(etat, "serveur.jsonl")) else []
print(f"\nServeur : {len(recu)} mesures reçues")
def t_recu(p): return dt.datetime.fromisoformat(p["time"].replace("Z", "+00:00")).timestamp()
reelles = sorted(t_recu(p) - p.get("x_age_s", 0) for p in recu)
rejouees = [p for p in recu if p.get("x_age_s", 0) > 120]
print(f"  dont {len(rejouees)} rejouées (x_age_s > 120 s), âge max {max((p.get('x_age_s', 0) for p in recu), default=0)} s")
champs = {}
for p in recu:
    for k in p:
        champs[k] = champs.get(k, 0) + 1
print("  champs :", ", ".join(f"{k}={v}" for k, v in sorted(champs.items())))
# couverture : minutes (heure réelle de mesure) avec au moins une mesure, par heure, pendant que la carte est alimentée
print("\nCouverture par heure (heure réelle des mesures ; minutes avec mesure / minutes alimentées) :")
# une minute est « couverte » s'il y a une mesure à moins de 61 s de son milieu (période de 30 s, ou de 60 s pendant
# un rattrapage : une minute d'horloge peut alors tomber entre deux mesures sans qu'il manque rien)
import bisect
def couverte(m, temps):
    c = m * 60 + 30
    i = bisect.bisect_left(temps, c - 61)
    return i < len(temps) and temps[i] <= c + 61
minutes = {m for m in range(int(reelles[0] // 60), int(reelles[-1] // 60) + 1) if couverte(m, reelles)} if reelles else set()
alim = set()
for a, b, _ in reboots:
    for m in range(a // 60 + 4, b // 60):   # 3 min de préchauffage du CO2 + 1 min de marge
        alim.add(m)
heures = {}
for m in alim:
    h = dt.datetime.fromtimestamp(m * 60, PARIS).strftime("%d/%m %Hh")
    tot, ok = heures.get(h, (0, 0))
    heures[h] = (tot + 1, ok + (m in minutes))
# mesures encore dans la file d'attente (flash) quand la simulation s'arrête : pas perdues, juste pas encore envoyées
en_attente = set()
nvs = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(etat, "nvs.txt")) if "\t" in l) if os.path.exists(os.path.join(etat, "nvs.txt")) else {}
fichier = os.path.join(etat, "fs", "backlog.jsonl")
if os.path.exists(fichier):
    with open(fichier, "rb") as f:
        f.seek(int(nvs.get("off", "0")))
        for l in f:
            try:
                e = json.loads(l).get("_e", 0)
                if e: en_attente.update((int(e // 60) - 1, int(e // 60), int(e // 60) + 1))
            except ValueError:
                pass
print(f"  ({len(en_attente)} minutes de mesures encore en file d'attente à la fin de la simulation : pas perdues)")
trous = []
for h in sorted(heures):
    tot, ok = heures[h]
    attente = sum(1 for m in alim if m not in minutes and m in en_attente and dt.datetime.fromtimestamp(m * 60, PARIS).strftime("%d/%m %Hh") == h)
    marque = ""
    if ok + attente < tot - 1:
        marque = "  <-- TROU (mesures perdues)"
        trous.append(h)
    elif ok < tot - 1:
        marque = f"  (+{attente} en file d'attente)"
    print(f"  {h} : {ok:2d}/{tot:2d}{marque}")
json.dump({"reboots": reboots, "nb_recues": len(recu), "rejouees": len(rejouees), "trous": trous},
          open(os.path.join(SORTIE, "bilan.json"), "w"), indent=1)

# ---------------------------------------------------------------- images de l'écran
try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit(0)
fich = os.path.join(etat, "images.bin")
if not os.path.exists(fich):
    sys.exit(0)
data = open(fich, "rb").read()
TAILLE = 12 + 64 * 32 * 2
images = []
for i in range(0, len(data) - TAILLE + 1, TAILLE):
    e, ms, pl = struct.unpack_from("<III", data, i)
    px = struct.unpack_from("<2048H", data, i + 12)
    images.append((e + ms / 1000, pl & 0xFF, pl >> 8, px))
print(f"\nÉcran : {len(images)} images différentes enregistrées")
NOMS = ["CO2", "AERER", "PM1", "PM2.5", "PM10", "INDICE", "COURBE", "WIFI"]

def rendu(px, lum=255, echelle=6):
    im = Image.new("RGB", (64 * echelle, 32 * echelle), (12, 12, 14))
    d = ImageDraw.Draw(im)
    k = max(lum, 30) / 255
    for y in range(32):
        for x in range(64):
            c = px[y * 64 + x]
            r, g, b = ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
            if c == 0:
                d.ellipse([x * echelle + 1, y * echelle + 1, x * echelle + echelle - 2, y * echelle + echelle - 2], fill=(28, 28, 32))
            else:
                d.ellipse([x * echelle, y * echelle, x * echelle + echelle - 1, y * echelle + echelle - 1], fill=(int(r * k), int(g * k), int(b * k)))
    return im

os.makedirs(os.path.join(SORTIE, "ecran"), exist_ok=True)
# planches : pour chaque fenêtre d'images, une vignette par changement de page (+ éclairs d'alerte)
fenetres = [l.split()[1:] for l in SCENARIO if l.startswith("images ")]
for a, b in fenetres:
    a, b = int(a), int(b)
    sel, page_prec = [], None
    for (t, page, lum, px) in images:
        if not (a <= t < b):
            continue
        cle = (page, sum(1 for v in px if v) > 1900)   # page + plein écran (éclair)
        if cle != page_prec or (sel and t - sel[-1][0] > 20):
            sel.append((t, page, lum, px))
            page_prec = cle
    sel = sel[:16]
    if not sel:
        continue
    col = 4
    lignes = (len(sel) + col - 1) // col
    planche = Image.new("RGB", (col * 400, lignes * 232), (245, 245, 240))
    d = ImageDraw.Draw(planche)
    for i, (t, page, lum, px) in enumerate(sel):
        x, y = (i % col) * 400 + 8, (i // col) * 232 + 8
        planche.paste(rendu(px, lum), (x, y))
        d.text((x, y + 196), f"{dt.datetime.fromtimestamp(t, PARIS):%H:%M:%S}  page {NOMS[page] if page < 8 else page}  lum {lum}", fill=(20, 20, 20))
    nom = os.path.join(SORTIE, "ecran", f"planche_{dt.datetime.fromtimestamp(a, PARIS):%d_%Hh%M}.png")
    planche.save(nom)
    print("  ", nom, len(sel), "vignettes")
