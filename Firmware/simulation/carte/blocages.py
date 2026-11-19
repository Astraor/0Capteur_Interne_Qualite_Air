#!/usr/bin/env python3
"""Plusieurs jours de carte allumée sans interruption, avec une panne qui peut bloquer les envois sans faire planter
la carte (donc sans redémarrage salvateur). Vérifie que les envois reprennent seuls et qu'aucune mesure n'est perdue.

Usage : python3 blocages.py [binaire de la carte] [dossier de sortie]   (après sh compiler.sh)
"""
import concurrent.futures as cf, datetime as dt, json, os, shutil, subprocess, sys
from zoneinfo import ZoneInfo

ICI = os.path.dirname(os.path.abspath(__file__))
CARTE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ICI, "build", "carte")
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "sortie", "blocages")
P = ZoneInfo("Europe/Paris")
CLE = "0123456789abcdef0123456789abcdef"

def ep(j, h, m=0):
    return int(dt.datetime(2026, 11, j, h, m, tzinfo=P).timestamp())

DEBUT, FIN, INCIDENT = ep(19, 7), ep(21, 7), ep(20, 10)   # 2 jours ; la panne arrive le 2e jour à 10 h
BASE = [f"cle {CLE}", "ap MaBox -58 motdepasse123",
        f"cmd {ep(19, 7, 1)} wifi add MaBox motdepasse123", f"cmd {ep(19, 7, 2)} set cle {CLE}"]
CAS = {
    "reference": [],
    # 2 min de refus 401 (maintenance du serveur…) : avant le correctif, plus aucun envoi jusqu'au débranchement
    "401_passager": [f"panne_serveur {INCIDENT} {INCIDENT + 120} 401"],
    # Wi-Fi « connecté » mais plus aucun paquet ne passe ; une nouvelle connexion répare la liaison
    "wifi_fantome": [f"wifi_zombie {INCIDENT} {FIN}"],
    # le serveur répond une fois next_push_in_s = 2 jours
    "delai_geant": [f"attente_serveur {INCIDENT} {INCIDENT + 60} 172800"],
}

def lancer(nom, lignes):
    d = os.path.join(SORTIE, nom)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    open(os.path.join(d, "scenario.txt"), "w").write("\n".join(BASE + lignes) + "\n")
    env = dict(os.environ, SIM_DOSSIER=os.path.join(d, "etat"), SIM_EPOCH_BOOT=str(DEBUT), SIM_DUREE_S=str(FIN - DEBUT),
               SIM_SCENARIO=os.path.join(d, "scenario.txt"), SIM_GRAINE="7")
    with open(os.path.join(d, "console.txt"), "w") as console:
        subprocess.run([CARTE], env=env, stdout=console, stderr=subprocess.STDOUT, check=False)
    recu = [json.loads(l) for l in open(os.path.join(d, "etat", "serveur.jsonl"))]
    t_recu = lambda p: dt.datetime.fromisoformat(p["time"].replace("Z", "+00:00")).timestamp()
    apres = sorted(t_recu(p) for p in recu if t_recu(p) > INCIDENT + 30)
    reprise = f"{(apres[0] - INCIDENT) / 60:5.1f} min" if apres else "JAMAIS"
    # minutes (heure réelle de la mesure) sans aucune mesure, de 1 h avant la panne à la fin
    reelles = {int((t_recu(p) - p.get("x_age_s", 0)) // 60) for p in recu}
    minutes = range((INCIDENT - 3600) // 60, (FIN - 300) // 60)
    manquantes = sum(1 for m in minutes if m not in reelles)
    return f"{nom:13s} {len(recu):6d} mesures reçues | reprise des envois : {reprise:>9s} | minutes sans mesure : {manquantes} / {len(minutes)}"

if __name__ == "__main__":
    os.makedirs(SORTIE, exist_ok=True)
    with cf.ThreadPoolExecutor(4) as ex:
        for ligne in ex.map(lambda kv: lancer(*kv), CAS.items()):
            print(ligne)
