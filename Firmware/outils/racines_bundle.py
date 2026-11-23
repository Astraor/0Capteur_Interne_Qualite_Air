# Racines HTTPS au format « bundle » : script lancé par PlatformIO AVANT chaque compilation (platformio.ini :
# extra_scripts = pre:...). Peut aussi se lancer seul : python outils/racines_bundle.py
#
# Pourquoi : avec une liste de certificats en texte (setCACert), la carte décode TOUTES les racines en mémoire vive
# pendant la connexion (~6 Ko pour les 4 d'AirCarto, ~18 Ko pour les 10 du relais). Il n'en restait plus assez pour
# le calcul RSA 4096 bits de la chaîne d'AirCarto (« diag tls » : BIGNUM - Memory allocation failed).
# Le « bundle » (setCACertBundle) reste en mémoire flash : pour chaque racine, seulement son nom et sa clé publique ;
# la carte ne lit que la racine dont elle a besoin, au moment de vérifier la signature.
#
# Source : src/racines_tls.h (RACINES_AIRCARTO, RACINES_TLS), qui reste la seule liste à modifier.
# Résultat : src/racines_bundle.h (BUNDLE_AIRCARTO, BUNDLE_TLS), réécrit seulement s'il change.
# Format attendu par la bibliothèque (esp_crt_bundle.c d'Arduino-ESP32 2.x) : nombre de racines sur 2 octets, puis
# pour chaque racine, triées par nom (ordre des octets DER, recherche dichotomique sur la carte) :
#   longueur du nom (2 octets), longueur de la clé (2 octets), nom du sujet (DER), clé publique SubjectPublicKeyInfo (DER).
# Aucun paquet à installer (bibliothèque standard de Python seulement).
import base64
import hashlib
import os
import re
import sys


def _tlv(der, i):
    """(étiquette, début du contenu, fin) de l'élément DER qui commence en i."""
    tag = der[i]
    n = der[i + 1]
    j = i + 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 3:
            raise ValueError("longueur DER non gérée")
        n = int.from_bytes(der[j:j + k], "big")
        j += k
    if j + n > len(der):
        raise ValueError("DER tronqué")
    return tag, j, j + n


def sujet_et_cle(der):
    """Nom du sujet et clé publique (SubjectPublicKeyInfo), en DER brut, d'un certificat X.509."""
    tag, d, f = _tlv(der, 0)                       # Certificate
    if tag != 0x30 or f != len(der):
        raise ValueError("certificat invalide")
    tag, d, f = _tlv(der, d)                       # TBSCertificate
    if tag != 0x30:
        raise ValueError("TBSCertificate invalide")
    champs = []
    i = d
    while i < f:
        t, cd, cf = _tlv(der, i)
        champs.append((t, i, cf))
        i = cf
    if champs and champs[0][0] == 0xA0:            # [0] version, facultatif
        champs = champs[1:]
    # serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo
    if len(champs) < 6 or champs[0][0] != 0x02 or any(c[0] != 0x30 for c in champs[1:6]):
        raise ValueError("champs du TBSCertificate inattendus")
    _, sd, sf = champs[4]
    _, kd, kf = champs[5]
    return bytes(der[sd:sf]), bytes(der[kd:kf])


def _chaines_c(texte):
    """Concatène les chaînes C "..." d'un morceau de code (échappements \\n et \\" seulement)."""
    morceaux = re.findall(r'"((?:[^"\\]|\\.)*)"', texte)
    return "".join(m.replace("\\n", "\n").replace('\\"', '"') for m in morceaux)


def lire_listes(chemin):
    """{nom : texte PEM} pour RACINES_AIRCARTO et RACINES_TLS, macros CERT_... développées."""
    src = open(chemin, encoding="utf-8").read()
    # commentaires // retirés, sauf à l'intérieur des chaînes (le base64 peut contenir « // »)
    src = re.sub(r'"(?:[^"\\\n]|\\.)*"|//[^\n]*', lambda m: m.group(0) if m.group(0)[0] == '"' else "", src)
    macros = {}
    for m in re.finditer(r"#define\s+(\w+)\s*\\\n((?:[^\n]*\\\n)*[^\n]*)", src):
        macros[m.group(1)] = m.group(2)
    listes = {}
    for m in re.finditer(r"static\s+const\s+char\s+(\w+)\[\]\s*=(.*?);", src, re.S):
        corps = m.group(2)
        corps = re.sub(r"\b(CERT_\w+)\b", lambda x: macros[x.group(1)], corps)
        listes[m.group(1)] = _chaines_c(corps)
    return listes


def certificats(pem):
    ders = []
    for b64 in re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S):
        ders.append(base64.b64decode("".join(b64.split()), validate=True))
    return ders


def bundle(ders):
    entrees = {}
    for der in ders:
        sujet, cle = sujet_et_cle(der)
        if sujet in entrees and entrees[sujet][1] != cle:
            raise ValueError("deux racines de même nom et de clés différentes")
        entrees[sujet] = (der, cle)
    out = bytearray(len(entrees).to_bytes(2, "big"))
    resume = []
    for sujet in sorted(entrees):
        der, cle = entrees[sujet]
        out += len(sujet).to_bytes(2, "big") + len(cle).to_bytes(2, "big") + sujet + cle
        resume.append(hashlib.sha256(der).hexdigest().upper())
    return bytes(out), resume


def _nom_lisible(der):
    """CN du certificat, pour les commentaires (lecture simple : premier commonName du sujet)."""
    sujet, _ = sujet_et_cle(der)
    i = sujet.find(b"\x06\x03\x55\x04\x03")       # OID 2.5.4.3 (commonName)
    if i < 0:
        return "?"
    _, d, f = _tlv(sujet, i + 5)
    return sujet[d:f].decode("utf-8", "replace")


def generer(src_dir):
    entree = os.path.join(src_dir, "racines_tls.h")
    sortie = os.path.join(src_dir, "racines_bundle.h")
    listes = lire_listes(entree)
    lignes = ["#pragma once",
              "// FICHIER GÉNÉRÉ par outils/racines_bundle.py à partir de racines_tls.h : ne pas modifier à la main.",
              "// Racines HTTPS au format « bundle » (nom du sujet + clé publique, en mémoire flash) : la carte n'en lit",
              "// qu'une, au moment de vérifier la signature, au lieu de les décoder toutes en mémoire vive.",
              "#include <stdint.h>", ""]
    for nom_c, nom_b in (("RACINES_AIRCARTO", "BUNDLE_AIRCARTO"), ("RACINES_TLS", "BUNDLE_TLS")):
        if nom_c not in listes:
            raise ValueError("%s introuvable dans racines_tls.h" % nom_c)
        ders = certificats(listes[nom_c])
        if not ders:
            raise ValueError("%s : aucun certificat" % nom_c)
        donnees, _ = bundle(ders)
        lignes.append("// %s : %d racines (empreinte SHA-256 du certificat complet, 8 premiers octets)" % (nom_b, len(ders)))
        for der in sorted(ders, key=lambda d: sujet_et_cle(d)[0]):
            lignes.append("//   %-48s %s" % (_nom_lisible(der), hashlib.sha256(der).hexdigest().upper()[:16]))
        lignes.append("static const uint8_t %s[%d] = {" % (nom_b, len(donnees)))
        for i in range(0, len(donnees), 20):
            lignes.append("  " + ", ".join("0x%02X" % b for b in donnees[i:i + 20]) + ",")
        lignes.append("};")
        lignes.append("")
    texte = "\n".join(lignes)
    ancien = open(sortie, encoding="utf-8").read() if os.path.exists(sortie) else None
    if ancien != texte:
        with open(sortie, "w", encoding="utf-8", newline="\n") as f:
            f.write(texte)
        print("Racines HTTPS : %s régénéré." % os.path.relpath(sortie))


if "Import" in globals():                       # lancé par PlatformIO (SCons)
    Import("env")  # noqa: F821
    generer(env.subst("$PROJECT_SRC_DIR"))  # noqa: F821
elif __name__ == "__main__":
    generer(os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "..", "src"))
