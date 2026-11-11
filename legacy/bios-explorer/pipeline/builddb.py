"""Build the deduplicated settings database from the BIOS image, and decode a UEFI dump against it."""
import pickle, sys, json, os, csv, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ifr

def build(mods_pkl):
    mods = pickle.load(open(mods_pkl, 'rb'))
    b = [s for n, g, t, secs in mods if g.startswith('899407d7') for st, s in secs if st == 0x10][0]
    strs = ifr.find_string_packages(b)[0][2]
    r = ifr.parse_form(ifr.find_form_packages(b)[0][2], strs)
    vs = r['varstores']; forms = r['forms']
    # form tree: parent of each form = first form that references it
    parent = {}
    for a, t in r['refs']:
        if a is not None and t not in parent and t != a: parent[t] = a
    root = next(iter(forms))
    reach = {root}; frontier = [root]
    kids = collections.defaultdict(list)
    for a, t in r['refs']: kids[a].append(t)
    while frontier:
        f = frontier.pop()
        for t in kids.get(f, []):
            if t not in reach: reach.add(t); frontier.append(t)
    def path(fid):
        p = []; seen = set()
        while fid is not None and fid not in seen:
            seen.add(fid); t = forms.get(fid, '').strip()
            if t: p.append(t)
            fid = parent.get(fid)
        return ' > '.join(reversed(p))
    groups = collections.OrderedDict()
    for q in r['questions']:
        v = vs.get(q['varstore'])
        if not v or not q['prompt']: continue
        key = (v['name'], q['offset'], q['width'])
        always_hidden = any(k == 'true' for _, k in q['suppress']) or (q['form'] not in reach)
        g = groups.get(key)
        if g is None:
            g = groups[key] = dict(var=v['name'], guid=v['guid'], offset=q['offset'], width=q['width'], kind=q['kind'],
                                   prompt=q['prompt'], help=q['help'], options=list(q['options']), default=q['default'], mfg=q['mfg'],
                                   min=q.get('min'), max=q.get('max'), step=q.get('step'), hex=q.get('hex', False),
                                   paths=[], visible_somewhere=False, conditional=True, aliases=[])
        if q['prompt'] != g['prompt'] and q['prompt'] not in g['aliases']: g['aliases'].append(q['prompt'])
        if not g['help'] and q['help']: g['help'] = q['help']
        for val, txt in q['options']:
            if all(val != v0 for v0, _ in g['options']): g['options'].append((val, txt))
        if g['default'] is None and q['default'] is not None: g['default'] = q['default']
        p = path(q['form'])
        if p and p not in g['paths']: g['paths'].append(p)
        if not always_hidden:
            g['visible_somewhere'] = True
            if not q['suppress']: g['conditional'] = False
    out = []
    for i, g in enumerate(groups.values()):
        g['hidden'] = not g['visible_somewhere']
        if g['hidden']: g['conditional'] = False
        del g['visible_somewhere']
        g['id'] = i
        out.append(g)
    return out

def decode(db, dump_dir):
    idx = list(csv.DictReader(open(os.path.join(dump_dir, 'index.csv'), encoding='utf-8-sig')))
    files = {}
    for row in idx:
        if row['File']:
            p = os.path.join(dump_dir, 'vars', row['File'])
            if os.path.exists(p): files[(row['Name'], row['Guid'].strip('{}').lower())] = open(p, 'rb').read()
    # real defaults from AMI's StdDefaults NVRAM store (what "Load Optimized Defaults" restores)
    std = {}
    sd = next((v for (n, g), v in files.items() if n == 'StdDefaults'), None)
    if sd:
        o = 0
        while True:
            i = sd.find(b'NVAR', o)
            if i < 0: break
            size = int.from_bytes(sd[i+4:i+6], 'little'); attr = sd[i+9]; p = i + 10
            p += 16 if attr & 0x04 else 1
            if attr & 0x02:
                e = sd.index(0, p); name = sd[p:e].decode('latin-1'); p = e + 1
            else:
                e = p
                while sd[e] or sd[e+1]: e += 2
                name = sd[p:e].decode('utf-16le'); p = e + 2
            std[(name, i + size - p)] = sd[p:i + size]
            o = i + 4
    sizes = {k[0]: len(v) for k, v in files.items()}
    for s in db:
        data = files.get((s['var'], s['guid'].lower()))
        dd = std.get((s['var'], len(data))) if data else None
        s['ifr_default'] = s['default']
        if dd is not None and s['offset'] + s['width'] <= len(dd):
            s['default'] = int.from_bytes(dd[s['offset']:s['offset'] + s['width']], 'little')
        s['current'] = int.from_bytes(data[s['offset']:s['offset'] + s['width']], 'little') if data and s['offset'] + s['width'] <= len(data) else None
    return db

if __name__ == '__main__':
    db = build(sys.argv[1])
    if len(sys.argv) > 3: decode(db, sys.argv[3])
    json.dump(db, open(sys.argv[2], 'w'), ensure_ascii=False)
    c = collections.Counter(s['var'] for s in db)
    print(len(db), 'unique settings;', sum(s['hidden'] for s in db), 'always hidden;', sum(s['conditional'] for s in db), 'conditional')
    print(c.most_common(12))
    if len(sys.argv) > 3:
        known = [s for s in db if s['current'] is not None]
        changed = [s for s in known if s['default'] is not None and s['current'] != s['default']]
        print(len(known), 'decoded;', len(changed), 'differ from default')
        for s in changed[:60]:
            opt = dict(s['options'])
            print(f"  [{s['var']}+{s['offset']}] {s['prompt'][:42]:42s} cur={opt.get(s['current'], s['current'])!s:18.18} def={opt.get(s['default'], s['default'])!s:14.14} {'HIDDEN' if s['hidden'] else ''}")
