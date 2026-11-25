# Mises à jour à distance (OTA) : script lancé par PlatformIO à chaque compilation (platformio.ini : extra_scripts).
#
#  1. NUMÉRO DE VERSION : écrit src/version_fw.h (FW_VERSION = AAMMJJhhmm, date de la dernière modification du code).
#     La carte n'installe une mise à jour que si son numéro est plus grand que le sien. Le numéro tient sur 32 bits :
#     ce format fonctionne jusqu'à fin 2042 (au-delà, changer de format et reflasher au câble USB).
#  2. SIGNATURE : après chaque compilation, si ta clé privée existe (outils/cle_privee_ota.pem), fabrique
#     ota/firmware.bin + ota/firmware.json (version, taille, empreinte SHA-256, signature). Ce sont les DEUX fichiers
#     à joindre à une release GitHub.
#  3. « pio run -t publier » : compile, signe, puis PUBLIE la release GitHub (tag v<version>, firmware.bin + firmware.json)
#     avec l'outil « gh » (GitHub CLI). La clé privée reste sur ton PC : c'est voulu, ne pas la confier à GitHub Actions.
#  4. « pio run -t cles » (une seule fois) : crée ta paire de clés. La clé PRIVÉE reste sur ton PC (jamais dans git,
#     garde-en une copie) ; la clé PUBLIQUE va dans src/cle_ota.h, compilée dans le programme de la carte.
#
# Signature : ECDSA P-256 + SHA-256, écrite ici en Python pur (aucun paquet à installer), vérifiée sur la carte par
# mbedtls. Message signé : "MAIR-OTA|<version>|<taille>|<sha256>".
import base64
import datetime
import hashlib
import hmac
import json
import os
import secrets
import shutil
import time

# ------------------------------------------------------------------ courbe P-256 (secp256r1)
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def _ajouter(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        l = (3 * x1 * x1 + A) * pow(2 * y1, P - 2, P) % P
    else:
        l = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (l * l - x1 - x2) % P
    return (x3, (l * (x1 - x3) - y1) % P)


def _multiplier(k, point):
    r = None
    while k:
        if k & 1:
            r = _ajouter(r, point)
        point = _ajouter(point, point)
        k >>= 1
    return r


def _k_rfc6979(d, h):
    """Nonce déterministe (RFC 6979) : jamais deux fois le même k pour deux messages différents."""
    x = d.to_bytes(32, "big")
    h1 = (int.from_bytes(h, "big") % N).to_bytes(32, "big")
    v, k = b"\x01" * 32, b"\x00" * 32
    k = hmac.new(k, v + b"\x00" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        t = int.from_bytes(v, "big")
        if 1 <= t < N:
            return t
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def _der_entier(v):
    b = v.to_bytes((v.bit_length() + 8) // 8, "big")   # un octet de plus : jamais lu comme négatif
    b = b.lstrip(b"\x00") or b"\x00"
    if b[0] & 0x80:
        b = b"\x00" + b
    return b"\x02" + bytes([len(b)]) + b


def signer(d, message):
    """Signature ECDSA P-256 / SHA-256 au format DER (celui qu'attend mbedtls_pk_verify)."""
    h = hashlib.sha256(message).digest()
    z = int.from_bytes(h, "big")
    k = _k_rfc6979(d, h)
    r = _multiplier(k, G)[0] % N
    s = pow(k, N - 2, N) * (z + r * d) % N
    if s > N // 2:
        s = N - s
    corps = _der_entier(r) + _der_entier(s)
    return b"\x30" + bytes([len(corps)]) + corps


def verifier(q, message, sig):
    """Vérification (pour les tests)."""
    assert sig[0] == 0x30
    i = 2
    lr = sig[i + 1]; r = int.from_bytes(sig[i + 2:i + 2 + lr], "big"); i += 2 + lr
    ls = sig[i + 1]; s = int.from_bytes(sig[i + 2:i + 2 + ls], "big")
    if not (1 <= r < N and 1 <= s < N):
        return False
    z = int.from_bytes(hashlib.sha256(message).digest(), "big")
    w = pow(s, N - 2, N)
    pt = _ajouter(_multiplier(z * w % N, G), _multiplier(r * w % N, q))
    return pt is not None and pt[0] % N == r


# ------------------------------------------------------------------ fichiers de clés (formats standard PEM)
OID_EC = bytes.fromhex("06072a8648ce3d0201")
OID_P256 = bytes.fromhex("06082a8648ce3d030107")


def _pem(etiquette, der):
    b = base64.b64encode(der).decode()
    lignes = [b[i:i + 64] for i in range(0, len(b), 64)]
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (etiquette, "\n".join(lignes), etiquette)


def pem_publique(q):
    point = b"\x04" + q[0].to_bytes(32, "big") + q[1].to_bytes(32, "big")
    algo = b"\x30\x13" + OID_EC + OID_P256
    return _pem("PUBLIC KEY", b"\x30\x59" + algo + b"\x03\x42\x00" + point)


def pem_privee(d, q):
    point = b"\x04" + q[0].to_bytes(32, "big") + q[1].to_bytes(32, "big")
    corps = (b"\x02\x01\x01" + b"\x04\x20" + d.to_bytes(32, "big") + b"\xa0\x0a" + OID_P256
             + b"\xa1\x44\x03\x42\x00" + point)
    return _pem("EC PRIVATE KEY", b"\x30\x77" + corps)


def lire_privee(chemin):
    texte = open(chemin).read()
    b64 = "".join(l for l in texte.splitlines() if l and not l.startswith("-----"))
    der = base64.b64decode(b64)
    if not (der[:2] == b"\x30\x77" and der[5:7] == b"\x04\x20"):
        raise ValueError("clé privée illisible : %s" % chemin)
    return int.from_bytes(der[7:39], "big")


def nouvelle_cle():
    d = secrets.randbelow(N - 1) + 1
    return d, _multiplier(d, G)


# ------------------------------------------------------------------ version et manifeste
def lire_version(entete):
    for ligne in open(entete):
        if ligne.startswith("#define FW_VERSION"):
            return int(ligne.split()[2].rstrip("UL"))
    return 0


def ecrire_version_si_besoin(dossier_src):
    """Nouveau numéro seulement si un fichier du programme a changé depuis le dernier (sinon : rien à recompiler)."""
    entete = os.path.join(dossier_src, "version_fw.h")
    dernier = 0.0
    for racine, _, fichiers in os.walk(dossier_src):
        for f in fichiers:
            if f != "version_fw.h":
                dernier = max(dernier, os.path.getmtime(os.path.join(racine, f)))
    if os.path.exists(entete) and os.path.getmtime(entete) >= dernier:
        return lire_version(entete)
    version = int(time.strftime("%y%m%d%H%M"))
    if os.path.exists(entete):
        precedente = lire_version(entete)
        if version <= precedente:
            # toujours plus grand que le précédent, ET toujours une vraie date (minute suivante, pas « ...1960 ») :
            # la carte se sert de cette date pour refuser les certificats HTTPS déjà expirés
            try:
                suivante = datetime.datetime.strptime("%010d" % precedente, "%y%m%d%H%M") + datetime.timedelta(minutes=1)
                version = int(suivante.strftime("%y%m%d%H%M"))
            except ValueError:
                version = precedente + 1
    with open(entete, "w") as f:
        f.write("// Fichier fabriqué par outils/ota.py à chaque modification du code : ne pas modifier, ne pas mettre dans git.\n")
        f.write("#pragma once\n#define FW_VERSION %dUL\n" % version)
    return version


def fabriquer_manifeste(bin_chemin, version, d):
    contenu = open(bin_chemin, "rb").read()
    sha = hashlib.sha256(contenu).hexdigest()
    message = "MAIR-OTA|%d|%d|%s" % (version, len(contenu), sha)
    sig = signer(d, message.encode()).hex()
    return {"version": version, "tag": "v%d" % version, "taille": len(contenu), "sha256": sha, "sig": sig}


# ------------------------------------------------------------------ publication de la release
def publier_release(dossier_ota, depot_git):
    """Crée la release GitHub avec firmware.bin + firmware.json (via « gh »). Renvoie True si elle est publiée."""
    import subprocess
    gh = shutil.which("gh")
    if gh is None:
        print("Publication : l'outil « gh » (GitHub CLI) n'est pas installé. Installe-le (https://cli.github.com), puis « gh auth login ».")
        print("              Sinon : release à la main avec Firmware/ota/firmware.bin et firmware.json.")
        return False
    binaire = os.path.join(dossier_ota, "firmware.bin")
    manifeste = os.path.join(dossier_ota, "firmware.json")
    if not (os.path.exists(binaire) and os.path.exists(manifeste)):
        print("Publication : firmware.bin / firmware.json absents (la clé privée existe-t-elle ? « pio run -t cles »).")
        return False
    infos = json.load(open(manifeste))
    tag = infos["tag"]
    # Avertit si le code publié n'est pas celui du dépôt : la release est taguée sur la branche par défaut de GitHub.
    try:
        sale = subprocess.run(["git", "status", "--porcelain", "--", "."], cwd=depot_git, capture_output=True, text=True).stdout.strip()
        avance = subprocess.run(["git", "log", "@{u}..", "--oneline"], cwd=depot_git, capture_output=True, text=True).stdout.strip()
        if sale or avance:
            print("ATTENTION : du code n'est pas encore sur GitHub (modifications non commitées ou commits non poussés).")
            print("            La release contient bien CE programme compilé, mais le dépôt ne le montre pas encore : fais « git push ».")
    except OSError:
        pass
    notes = "Programme de la carte, version %s (signé). La carte l'installe avec « maj installer »." % infos["version"]
    r = subprocess.run([gh, "release", "create", tag, binaire, manifeste, "--title", tag, "--notes", notes, "--latest"],
                       cwd=depot_git)
    if r.returncode != 0:
        print("Publication échouée (voir ci-dessus). Tag déjà publié ? Modifie le code pour obtenir une nouvelle version,")
        print("ou « gh auth status » si tu n'es pas connecté.")
        return False
    print("Release %s publiée. Sur la console : « Chercher une mise à jour », puis « Installer la mise à jour »." % tag)
    return True


# ------------------------------------------------------------------ intégration PlatformIO
try:
    Import("env")  # noqa: F821  (fourni par PlatformIO)
except NameError:
    env = None

if env is not None:
    PROJET = env.subst("$PROJECT_DIR")
    SRC = env.subst("$PROJECT_SRC_DIR")
    CLE_PRIVEE = os.path.join(PROJET, "outils", "cle_privee_ota.pem")
    CLE_PUBLIQUE_H = os.path.join(SRC, "cle_ota.h")
    SORTIE = os.path.join(PROJET, "ota")

    VERSION = ecrire_version_si_besoin(SRC)

    def apres_compilation(source, target, env):
        binaire = str(target[0])
        version = lire_version(os.path.join(SRC, "version_fw.h"))
        if not os.path.exists(CLE_PRIVEE):
            print("OTA : pas de clé privée (pio run -t cles) : programme NON signé, mise à jour à distance impossible.")
            return
        if not os.path.exists(CLE_PUBLIQUE_H):
            print("OTA : src/cle_ota.h manquant : relance « pio run -t cles » (il le recrée depuis ta clé privée).")
            return
        manifeste = fabriquer_manifeste(binaire, version, lire_privee(CLE_PRIVEE))
        os.makedirs(SORTIE, exist_ok=True)
        shutil.copyfile(binaire, os.path.join(SORTIE, "firmware.bin"))
        with open(os.path.join(SORTIE, "firmware.json"), "w") as f:
            json.dump(manifeste, f, indent=1)
        print("OTA : version %d signée -> Firmware/ota/firmware.bin + firmware.json (%d octets)." % (version, manifeste["taille"]))
        print("      Pour l'envoyer à la carte : release GitHub « %s » avec ces deux fichiers, puis « maj installer »." % manifeste["tag"])

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", apres_compilation)

    def generer_cles(*args, **kwargs):
        if os.path.exists(CLE_PRIVEE):
            d = lire_privee(CLE_PRIVEE)
            q = _multiplier(d, G)
            print("Clé privée déjà présente (%s) : on la garde (en changer obligerait à reflasher en USB)." % CLE_PRIVEE)
        else:
            d, q = nouvelle_cle()
            os.makedirs(os.path.dirname(CLE_PRIVEE), exist_ok=True)
            # Lisible par toi seul (sous Linux / macOS ; sans effet sous Windows, où le dossier personnel est déjà privé)
            fd = os.open(CLE_PRIVEE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(pem_privee(d, q))
            print("Clé privée créée : %s" % CLE_PRIVEE)
            print("  -> NE LA METS PAS dans git (.gitignore s'en charge) et garde-en une copie (clé USB, gestionnaire de")
            print("     mots de passe). Perdue : il faudra une nouvelle clé ET un flash USB.")
        with open(CLE_PUBLIQUE_H, "w") as f:
            f.write("// Clé PUBLIQUE de signature des mises à jour (créée par « pio run -t cles »). Elle n'est pas secrète :\n")
            f.write("// elle permet seulement de VÉRIFIER une signature. La clé privée correspondante reste sur ton PC.\n")
            f.write("#pragma once\nstatic const char CLE_PUBLIQUE_OTA[] =\n")
            for ligne in pem_publique(q).splitlines():
                f.write('  "%s\\n"\n' % ligne)
            f.write(";\n")
        print("Clé publique écrite : %s (à compiler, puis flasher UNE fois en USB)." % CLE_PUBLIQUE_H)
        print("")
        print("Idées de secrets pour le relais (à copier une seule fois, puis à garder dans un gestionnaire de mots de passe) :")
        print("  jeton de la carte        : %s" % secrets.token_urlsafe(24))
        print("  mot de passe de console  : %s" % secrets.token_urlsafe(18))

    def publier(*args, **kwargs):
        publier_release(SORTIE, PROJET)

    env.AddCustomTarget("publier", "$BUILD_DIR/${PROGNAME}.bin", publier, title="Publier la release",
                        description="Compile, signe et publie la release GitHub (outil gh requis)")

    env.AddCustomTarget("cles", None, generer_cles, title="Clés OTA",
                        description="Crée la clé de signature des mises à jour (une seule fois)")
