#!/usr/bin/env python3
"""tools/services/genkb.py — builds modules/services/data/services.db from tools/services/kb.py.

    python3 tools/services/genkb.py                 # regenerate modules/services/data/services.db
    python3 tools/services/genkb.py <out.db>        # write elsewhere (CI compares it byte-for-byte)
    python3 tools/services/genkb.py --selftest      # the gate's own tests

The rule-7 checks reuse tools/bios/genkb.py (unsourced-number heuristic, owner-specific text, basis/basis_note
validation), loaded by path so the bios generator is not changed. On top of those, the services gate fails on
(spec docs/pipeline/svcdrv/2-research.md §7): a missing basis or pbasis; a target outside {3, 4}; `never` with
levels; a driver in a level without `sysok`; `stock` on a storage driver or without an owner-measured/published
stock basis; a `bloat` entry that is contested; a later level with a softer target than an earlier one; a contested
note that does not name sources on both sides; duplicate names; a seed outside 60-100 entries.

Format (UTF-8, LF, TAB-separated, bios escaping): see tools/services/README.md.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_OUT = os.path.join(ROOT, 'modules', 'services', 'data', 'services.db')


def load_bios_gen():
    spec = importlib.util.spec_from_file_location('hadron_bios_genkb', os.path.join(ROOT, 'tools', 'bios', 'genkb.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_kb(path=None):
    spec = importlib.util.spec_from_file_location('hadron_services_kb', path or os.path.join(HERE, 'kb.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


B = load_bios_gen()
LEVELS = ('bloat', 'stripped', 'minimum')
FIELDS = 26
SEED_MIN, SEED_MAX = 60, 100
# Storage drivers never get a Windows-stock value: Windows sets those per PC (spec §0.5). Mirrors plan.cpp.
STORAGE = {'storport', 'stornvme', 'storahci', 'iastorac', 'iastoravc', 'iastorvd', 'iastorv', 'iavroc', 'atapi', 'pciide',
           'pciidex', 'scsiport', 'classpnp', 'disk', 'partmgr', 'volmgr', 'volmgrx', 'volsnap', 'mountmgr', 'fvevol',
           'rdyboost', 'iorate', 'ehstorclass', 'spaceport', 'volume'}
HW_RE = re.compile(r'^(?:class:[A-Za-z0-9]+|svc:[A-Za-z0-9_.-]+|self|vmguest|hvhost)$')
NAME_RE = re.compile(r'^[A-Za-z0-9_.-]+$')
CFG_RE = re.compile(r'^[0-4]/[-01]$')


def expand(text, kb, with_url):
    """@KEY -> the source's label (and its URL in basis and contested notes)."""
    def sub(m):
        k = m.group(1)
        if k not in kb.SOURCES:
            return m.group(0)
        label = kb.LABELS.get(k, k)
        return '%s (%s)' % (label, kb.SOURCES[k]) if with_url else label
    return B.KEY_RE.sub(sub, text)


def unknown_keys(text, kb):
    return [k for k in B.KEY_RE.findall(text) if k not in kb.SOURCES]


def check_entry(e, kb, errors):
    """Validates one entry; returns the K line (without LF) or None."""
    name = e.get('name', '')
    where = 'entry %s' % (name or '?')
    if not NAME_RE.match(name):
        errors.append('%s: name must be ASCII letters, digits, _ . -' % where)
    if e.get('kind') not in ('svc', 'drv'):
        errors.append('%s: kind must be svc or drv' % where)
    for k in kb.SOURCES:
        if k not in kb.LABELS:
            errors.append('source @%s has no label' % k)
    # Text fields: numbers, owner text, unknown sources.
    texts = {}
    for f in ('title', 'expl', 'if_off', 'test'):
        t = e.get(f, '')
        for k in unknown_keys(t, kb):
            errors.append('%s %s: unknown source @%s' % (where, f, k))
        texts[f] = B.render(t, '%s %s' % (where, f), kb.SOURCES, [], errors)
    if not e.get('title') or not e.get('expl'):
        errors.append('%s: title and expl are required' % where)
    # Explanation basis (every entry is tier 1).
    B.render_basis({'basis': e.get('basis', ''), 'basis_note': e.get('basis_note', '')}, where, True, kb.SOURCES, errors)
    if not e.get('basis'):
        errors.append('%s: no basis' % where)
    # Levels.
    levels = e.get('levels', {})
    lv, why = ['', '', ''], ['', '', '']
    last = None
    for i, L in enumerate(LEVELS):
        if L not in levels:
            continue
        t, w = levels[L]
        if t not in (3, 4):
            errors.append('%s: %s target %r is outside {3, 4}' % (where, L, t))
        if last is not None and t < last:
            errors.append('%s: %s target %r is softer than an earlier level (%r): membership is cumulative' % (where, L, t, last))
        last = t
        lv[i] = str(t)
        for k in unknown_keys(w, kb):
            errors.append('%s %s why: unknown source @%s' % (where, L, k))
        why[i] = B.render(w, '%s %s why' % (where, L), kb.SOURCES, [], errors)
    for L in levels:
        if L not in LEVELS:
            errors.append('%s: unknown level %r' % (where, L))
    if levels:
        if not e.get('pbasis'):
            errors.append('%s: preset membership without a pbasis' % where)
        B.render_basis({'basis': e.get('pbasis', ''), 'basis_note': e.get('pbasis_note', '')}, '%s pbasis' % where, True,
                       kb.SOURCES, errors)
        if e.get('never'):
            errors.append('%s: a never entry has levels' % where)
        if e.get('kind') == 'drv' and not e.get('sysok'):
            errors.append('%s: a driver in a level needs sysok' % where)
        if 'bloat' in levels and e.get('contested'):
            errors.append('%s: a bloat entry is contested (Pure bloat removal is only what no source says to keep)' % where)
    elif e.get('pbasis') or e.get('pbasis_note'):
        errors.append('%s: pbasis without any level' % where)
    # Contested: sources on both sides.
    con = e.get('contested', '')
    if con:
        keys = set(B.KEY_RE.findall(con))
        if len(keys) < 2:
            errors.append('%s: contested must name at least two sources (@KEY) on the two sides' % where)
        for k in unknown_keys(con, kb):
            errors.append('%s contested: unknown source @%s' % (where, k))
        B.render(con, '%s contested' % where, kb.SOURCES, [], errors)
    # Hardware hint.
    hw = e.get('hw', '')
    if hw and not HW_RE.match(hw):
        errors.append('%s: bad hw hint %r' % (where, hw))
    if hw and not levels:
        errors.append('%s: hw hint without a level' % where)
    # Windows stock value.
    stock = e.get('stock')
    stock_cfg = ''
    if stock is not None:
        stock_cfg = stock if isinstance(stock, str) else '%d/-' % stock
        if not CFG_RE.match(stock_cfg):
            errors.append('%s: stock %r must be like "2/1" or "3/-"' % (where, stock))
        if name.lower() in STORAGE:
            errors.append('%s: stock on a storage driver (Windows sets those per PC)' % where)
        if e.get('stock_basis') not in B.NUMBER_KINDS:
            errors.append('%s: stock needs an owner-measured or published stock_basis' % where)
        B.render_basis({'basis': e.get('stock_basis', ''), 'basis_note': e.get('stock_note', '')}, '%s stock' % where, True,
                       kb.SOURCES, errors)
    elif e.get('stock_basis') or e.get('stock_note'):
        errors.append('%s: stock_basis without a stock value' % where)
    builds = e.get('builds') or []
    if not builds or not all(re.fullmatch(r'\d{5}', b) for b in builds):
        errors.append('%s: builds must be five-digit build numbers' % where)
    for f in ('sources',):
        if e.get(f):
            B.render(e[f], '%s %s' % (where, f), kb.SOURCES, [], errors)

    fields = ['K', name, e.get('kind', ''), ','.join(builds),
              B.esc(expand(texts['title'], kb, False)), B.esc(expand(texts['expl'], kb, False)),
              B.esc(expand(texts['if_off'], kb, False)), B.esc(expand(texts['test'], kb, False))]
    fields += lv
    fields += [B.esc(expand(w, kb, False)) for w in why]
    fields += ['1' if e.get('sysok') else '0', '1' if e.get('never') else '0',
               e.get('basis', ''), B.esc(expand(e.get('basis_note', ''), kb, True)),
               e.get('pbasis', ''), B.esc(expand(e.get('pbasis_note', ''), kb, True)),
               stock_cfg, e.get('stock_basis', ''), B.esc(expand(e.get('stock_note', ''), kb, True)),
               hw, B.esc(expand(con, kb, True)), B.esc(e.get('sources', ''))]
    if len(fields) != FIELDS:
        errors.append('%s: internal: %d fields' % (where, len(fields)))
    return '\t'.join(fields)


def generate(kb):
    """Returns (db text, errors)."""
    errors = []
    lines = ['V\t1\tservices', 'B\t%s' % kb.BUILD]
    seen = set()
    for e in kb.ENTRIES:
        n = e.get('name', '').lower()
        if n in seen:
            errors.append('duplicate entry %s' % e.get('name'))
        seen.add(n)
        lines.append(check_entry(e, kb, errors))
    if not SEED_MIN <= len(kb.ENTRIES) <= SEED_MAX:
        errors.append('the seed has %d entries; the spec asks for %d-%d' % (len(kb.ENTRIES), SEED_MIN, SEED_MAX))
    for lvl in LEVELS:
        if not any(lvl in e.get('levels', {}) and e.get('kind') == 'svc' for e in kb.ENTRIES):
            errors.append('no Services entry at level %s' % lvl)
    if not any(e.get('levels') and e.get('kind') == 'drv' for e in kb.ENTRIES):
        errors.append('no Drivers entry with a level')
    return '\n'.join(lines) + '\n', errors


def build(dst):
    kb = load_kb()
    text, errors = generate(kb)
    if errors:
        for e in errors:
            print('genkb: ' + e, file=sys.stderr)
        print('genkb: %d error(s); %s not written' % (len(errors), dst), file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    lv = {L: sum(1 for e in kb.ENTRIES if L in e.get('levels', {})) for L in LEVELS}
    print('genkb: wrote %s (%d entries; bloat %d, stripped %d, minimum %d; %d explanation-only)' %
          (dst, len(kb.ENTRIES), lv['bloat'], lv['stripped'], lv['minimum'],
           sum(1 for e in kb.ENTRIES if e.get('never'))))
    return 0


# ---- selftest ---------------------------------------------------------------------------------------------------
class FakeKb:
    def __init__(self, entries, base):
        self.BUILD = base.BUILD
        self.SOURCES = dict(base.SOURCES)
        self.LABELS = dict(base.LABELS)
        self.ENTRIES = entries


def good(**kw):
    e = dict(name='GoodSvc', kind='svc', builds=['19045'], title='Good', expl='Does a thing.', if_off='The thing stops.',
             test='', basis='published', basis_note='@IOT describes it.', levels={'stripped': (4, 'not needed')},
             pbasis='reasoning', pbasis_note='How it works.', sysok=False, never=False, hw='', contested='', stock=None,
             stock_basis='', stock_note='', sources='')
    e.update(kw)
    return e


def errors_for(e, base):
    errs = []
    check_entry(e, FakeKb([e], base), errs)
    return errs


def selftest():
    base = load_kb()
    fails = 0

    def expect(cond, what):
        nonlocal fails
        print('  [%s] %s' % ('PASS' if cond else 'FAIL', what))
        if not cond:
            fails += 1

    print('services genkb selftest')
    expect(errors_for(good(), base) == [], 'a well-formed entry passes')
    bad = [
        ('missing basis', good(basis='', basis_note='')),
        ('missing pbasis', good(pbasis='', pbasis_note='')),
        ('published basis without a URL source', good(basis='published', basis_note='a forum post')),
        ('unknown basis', good(basis='hearsay')),
        ('unmarked number (seconds)', good(expl='Boots 2 seconds faster.')),
        ('unmarked number (percent)', good(if_off='Saves 5% CPU.')),
        ('unmarked number (MB)', good(expl='Frees 120 MB of RAM.')),
        ('owner-specific text', good(expl='On your PC this is idle.')),
        ('owner-specific CPU', good(expl='The 14700KF does not need it.')),
        ('target outside {3,4}', good(levels={'stripped': (2, 'x')})),
        ('target 0', good(levels={'minimum': (0, 'x')})),
        ('never with levels', good(never=True)),
        ('driver in a level without sysok', good(kind='drv')),
        ('stock on a storage driver', good(name='stornvme', kind='drv', levels={}, pbasis='', pbasis_note='', stock=0,
                                           stock_basis='published', stock_note='@IOT')),
        ('stock without a measured or published basis', good(stock='3/-', stock_basis='reasoning', stock_note='x')),
        ('bloat entry contested', good(levels={'bloat': (4, 'x')}, contested='@IOT says keep; @VDOT disables')),
        ('softer later level', good(levels={'bloat': (4, 'x'), 'minimum': (3, 'y')})),
        ('contested with one source', good(contested='@IOT says keep')),
        ('bad hw hint', good(hw='usb')),
        ('unknown source key', good(basis_note='@NOPE says so')),
        ('pbasis without a level', good(levels={})),
        ('bad build', good(builds=['22H2'])),
    ]
    for what, e in bad:
        expect(errors_for(e, base) != [], 'fails: ' + what)
    ok_texts = ['Wired port authentication (dot1x) on campus networks.', 'Windows 10/11 IoT Enterprise.',
                'Windows Update fails with error 0x80070422.', 'Stop 0x7B after a filter is removed.',
                'Microsoft\'s Windows 10 2004 VDI guide.', 'Windows Server 2016 with Desktop Experience.']
    for t in ok_texts:
        expect(errors_for(good(expl=t), base) == [], 'passes: ' + t)
    # The committed KB passes and is in the seed range.
    text, errs = generate(base)
    for e in errs:
        print('    ' + e)
    expect(errs == [], 'tools/services/kb.py passes every check (%d entries)' % len(base.ENTRIES))
    expect(all(len(l.split('\t')) == FIELDS for l in text.splitlines() if l.startswith('K\t')), 'every K line has %d fields' % FIELDS)
    expect('@' not in ''.join(l.split('\t')[17] for l in text.splitlines() if l.startswith('K\t')),
           'every @KEY in basis notes is expanded')
    print('services genkb selftest: %s (%d failed)' % ('FAIL' if fails else 'PASS', fails))
    return 1 if fails else 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--selftest':
        return selftest()
    return build(argv[1] if len(argv) > 1 else DEFAULT_OUT)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
