# Extraction : python3 extraire.py ../src/main.cpp extrait.inc  (produit extrait.inc et extrait_carte.inc)
# Extrait du VRAI main.cpp les déclarations et fonctions du stockage hors ligne et de l'envoi (sans les modifier).
import re, sys
src = open(sys.argv[1], encoding='utf-8').read().split('\n')
def bloc_lignes(debut_re, fin_re):
    i = next(k for k, l in enumerate(src) if re.match(debut_re, l))
    j = next(k for k in range(i, len(src)) if re.match(fin_re, src[k]))
    return src[i:j + 1]
def fonction(nom):
    i = next(k for k, l in enumerate(src) if re.match(r'^[A-Za-z_][\w\s\*&<>:]*\b' + nom + r'\(', l) and l.rstrip().endswith('{'))
    prof, out = 0, []
    for k in range(i, len(src)):
        out.append(src[k]); prof += src[k].count('{') - src[k].count('}')
        if prof == 0: return out
out = []
out += bloc_lignes(r'^static const char\* URL_SERVEUR', r'^static const char\* URL_SERVEUR')
out += bloc_lignes(r'^static const unsigned long INTERVALLE_LECTURE_MS', r'^static const unsigned long ATTENTE_HEURE_MAX_MS')
out += bloc_lignes(r'^bool verbose', r'^volatile uint32_t nbLignesAttente')
for f in ['heureValide', 'reseauOK', 'arrondi1', 'dansBornes', 'periodeMesureMs', 'cleOK', 'lireCle', 'agregatAjouter', 'agregatRaz',
          'reinitialiserBacklog', 'initialiserBacklog', 'compacterBacklog', 'purgerAnciennes', 'stockerRecord', 'lireProchaineLigne',
          'validerLigne', 'calculerAge', 'posterAuServeur', 'envoyerLigne', 'construireRecord', 'deposerMesure', 'sortirPendante',
          'finaliserPeriode', 'rejouerUneLigne', 'ecartCourantMs', 'gererEnvois']:
    out += fonction(f) + ['']
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(out))
print(len(out), 'lignes extraites')

# Version « carte » : tout le code extrait devient une structure (une instance = un démarrage, RAM remise à neuf),
# avec les types de l'ESP32 (long et unsigned long sur 32 bits).
txt = '\n'.join(out)
txt = re.sub(r'static const char\*', 'static constexpr const char*', txt)
txt = re.sub(r'static const (?!char)', 'static constexpr ', txt)
txt = re.sub(r'\bunsigned long\b', 'uint32_t', txt)
txt = re.sub(r'\blong\b', 'int32_t', txt)
open(sys.argv[2].replace('.inc', '_carte.inc'), 'w', encoding='utf-8').write(txt)
