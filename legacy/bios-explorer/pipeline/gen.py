"""Pack settings DB + knowledge base into a tab-separated UTF-8 file the C++ app embeds."""
import json, re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kb import KB, G
from kb_impact import IMPACT, EXPL, REC, NOTE
from kb2 import NAME, MENU, FALLBACK

def esc(s):
    return str(s).replace('\\', '\\\\').replace('\t', ' ').replace('\r', '').replace('\n', '\\n')

db = json.load(open(sys.argv[1]))
out = []
title = sys.argv[3] if len(sys.argv) > 3 else 'BIOS'
out.append('V\t' + esc(title))
kre = [(re.compile(rx, re.I), i) for i, (rx, *_rest) in enumerate(KB)]
miss = []
for i, (rx, t, expl, recs, note) in enumerate(KB):
    note = re.sub(r'Your PC:[^.]*\.\s*', '', note).strip()
    expl = EXPL.get(t, expl); note = NOTE.get(t, note); recs = dict(recs, **REC.get(t, {}))
    imp = IMPACT.get(t)
    if imp is None: miss.append(t); imp = ('', '', '', '')
    out.append('K\t%d\t%s\t%s\t%s\t%s\t%s' % (i, esc(t), esc(expl), '\t'.join(esc(recs[g]) for g in G), esc(note), '\t'.join(esc(x) for x in imp)))
print('missing impact:', miss)
print('unused keys:', [k for k in list(IMPACT)+list(EXPL)+list(REC)+list(NOTE) if k not in {e[1] for e in KB}])
base = len(KB)
out.append('T\t%d' % base)
def addk(i, t, expl, recs, note, imp):
    out.append('K\t%d\t%s\t%s\t%s\t%s\t%s' % (i, esc(t), esc(expl), '\t'.join(esc(recs[g]) for g in G), esc(note), '\t'.join(esc(x) for x in imp)))
nre = []
for j, (rx, t, expl, recs, note, imp) in enumerate(NAME):
    addk(base + j, t, expl, recs, note, imp); nre.append((re.compile(rx, re.I), base + j))
mbase = base + len(NAME); mre = []
for j, (rx, t, expl, recs, note, imp) in enumerate(MENU):
    addk(mbase + j, t, expl, recs, note, imp); mre.append((re.compile(rx, re.I), mbase + j))
fb = mbase + len(MENU); addk(fb, *FALLBACK)
stats = {'kb': 0, 'name': 0, 'menu': 0, 'fallback': 0}
nk = 0
for s in db:
    k = next((i for r, i in kre if r.search(s['prompt'])), -1)
    if k >= 0: stats['kb'] += 1
    if k < 0:
        k = next((i for r, i in nre if r.search(s['prompt'].strip())), -1)
        if k >= 0: stats['name'] += 1
    if k < 0:
        p = ' | '.join(s['paths'])
        k = next((i for r, i in mre if r.search(p)), -1)
        if k >= 0: stats['menu'] += 1
    if k < 0: k = fb; stats['fallback'] += 1
    if k >= 0: nk += 1
    opts = '\x1e'.join('%d\x1f%s' % (v, esc(t)) for v, t in s['options'])
    flags = ('H' if s['hidden'] else '') + ('C' if s['conditional'] else '')
    def n(x): return '' if x is None else str(x)
    out.append('\t'.join(['S', str(s['id']), esc(s['var']), s['guid'], str(s['offset']), str(s['width']), s['kind'], flags,
                          n(s['default']), n(s.get('min')), n(s.get('max')), n(s.get('step')),
                          esc(s['prompt']), esc(s['help']), esc(' | '.join(s['paths'][:3])), opts, str(k)]))
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print(stats); print(len(db), 'settings,', nk, 'with explanations;', os.path.getsize(sys.argv[2]), 'bytes')
