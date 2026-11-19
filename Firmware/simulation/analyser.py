import json, sys, collections, datetime
DEP = None
def hParis(ms):  # heure de Paris (UTC+1 en novembre) depuis le début de la simulation
    t = datetime.datetime.fromtimestamp(DEP + ms / 1000 + 3600, datetime.timezone.utc)
    return t.strftime('%H:%M:%S')
for nom in sys.argv[1:]:
    r = json.load(open(f'res_{nom}.json'))
    DEP = r['epochDepart']
    verite = {v[0]: v for v in r['verite']}
    recus = collections.defaultdict(list)
    erreurs, ages = [], []
    for s in r['serveur']:
        c = s['corps']; i = round(c['x_temp_pm'] * 10)
        time_s = s['recu'] // 1000 * 1000            # le serveur date à la seconde
        reel = time_s - c['x_age_s'] * 1000            # ce que fait le site
        recus[i].append(reel)
        if i in verite: erreurs.append((reel - verite[i][1]) / 1000); ages.append(c['x_age_s'])
    perdues = sorted(set(verite) - set(recus))
    doublons = {i: v for i, v in recus.items() if len(v) > 1}
    rejouees = [s for s in r['serveur'] if s['corps']['x_age_s'] > 120]
    print(f'== {nom}')
    print(f'   mesures prises {len(verite)} | reçues (distinctes) {len(recus)} | perdues {len(perdues)} | en double {len(doublons)}')
    if erreurs:
        e = sorted(abs(x) for x in erreurs)
        print(f'   écart heure reconstruite - heure réelle : max {max(erreurs):+.1f} s / min {min(erreurs):+.1f} s ; 99 % sous {e[int(len(e)*.99)-1]:.1f} s')
    if rejouees:
        print(f'   rejouées (âge > 2 min) : {len(rejouees)} ; âge max {max(a for a in ages)//60} min ; rattrapage terminé à {hParis(max(s["recu"] for s in rejouees))}')
    if perdues:
        # plages perdues (heure réelle)
        plages, deb, prec = [], perdues[0], perdues[0]
        for i in perdues[1:] + [None]:
            if i is None or i != prec + 1: plages.append((deb, prec)); deb = i
            prec = i
        for a, b in plages[:6]:
            print(f'   perdues : n°{a}-{b} ({b-a+1}) prises de {hParis(verite[a][1])} à {hParis(verite[b][1])} (démarrage {verite[a][2]})')
    msgs = collections.Counter(m for _, m in r['journal'] if not m.startswith('---') and not m.startswith('==='))
    for m, n in msgs.most_common(6): print(f'   journal x{n} : {m[:110]}')
    for t, m in r['journal']:
        if m.startswith('---') or m.startswith('==='): print(f'   {hParis(t)} {m}')
