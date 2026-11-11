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
note that does not name sources on both sides; duplicate names; fewer than 60 entries.

The bios number check only knows units ("2 s", "120 MB"). This module is about counts and sizes, so the services gate
also fails on (kb-audit F7): a count of processes, services, drivers, threads, handles or instances ("40 processes",
"30 services"); a size or a duration written in words ("200 megabytes", "two seconds", "ten percent"); and a fraction
("half", "a third", "a quarter"). A number marked [[shown|published|@KEY]] passes, as in the bios KB. The bios
soft-quantity check ("twice", "tens of", ...) is reported as warnings, which never fail the build.

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
SEED_MIN = 60
# Storage drivers never get a Windows-stock value: Windows sets those per PC (spec §0.5). Mirrors plan.cpp.
STORAGE = {'storport', 'stornvme', 'storahci', 'iastorac', 'iastoravc', 'iastorvd', 'iastorv', 'iavroc', 'atapi', 'pciide',
           'pciidex', 'scsiport', 'classpnp', 'disk', 'partmgr', 'volmgr', 'volmgrx', 'volsnap', 'mountmgr', 'fvevol',
           'rdyboost', 'iorate', 'ehstorclass', 'spaceport', 'volume'}
HW_RE = re.compile(r'^(?:class:[A-Za-z0-9]+|svc:[A-Za-z0-9_.-]+|self|vmguest|hvhost)$')
NAME_RE = re.compile(r'^[A-Za-z0-9_.-]+$')
CFG_RE = re.compile(r'^[0-4]/[-01]$')

# Counts, sizes in words and fractions (kb-audit F7). Years and builds before a noun ("Windows Server 2016 services")
# and version numbers ("USB Audio 2.0 driver") are names, not counts.
# "one" alone is left out: "one instance per user" is a description, not a quantity.
WORD_NUM = (r'(?:zero|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|fifteen|'
            r'sixteen|seventeen|eighteen|nineteen|twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety|hundreds?|'
            r'thousands?|millions?|dozens?|a\s+dozen)(?:[\s-]+(?:one|two|three|four|five|six|seven|eight|nine|'
            r'hundred|thousand|million))*')
COUNT_NOUN = (r'(?:processe?s?|services?|drivers?|threads?|handles?|instances?|tasks?|svchosts?|'
              r'(?:kilo|mega|giga|tera|kibi|mebi|gibi|tebi)bytes?)')
COUNT_RE = re.compile(
    r'(?<![\w./])(?<!Server )(?<!Windows )(?<!build )(?<!version )'
    r'(?:\d+(?:,\d{3})*(?![.\d])|' + WORD_NUM + r')(?:\s+(?:of\s+)?(?:or\s+so\s+)?)?[\s-]*'
    r'(?:more\s+|fewer\s+|less\s+|extra\s+|additional\s+|background\s+|running\s+)?' + COUNT_NOUN + r'(?!\w)', re.I)
# Units spelled out after a number word. "three times" is left to the bios soft check (it warns on "N times faster").
WORD_UNIT = (r'(?:(?:milli|micro|nano)?seconds?|secs?|minutes?|mins?|hours?|hrs?|days?|weeks?|months?|years?|'
             r'per\s*cent|percent(?:age\s+points?)?|ms|KB|MB|GB|TB|KiB|MiB|GiB|watts?|volts?|degrees?|frames?|fps|'
             r'hertz|[kMG]?Hz)')
WORD_UNIT_RE = re.compile(r'(?<![\w-])' + WORD_NUM + r'[\s-]+(?:more\s+|fewer\s+|less\s+)?' + WORD_UNIT + r'(?!\w)',
                          re.I)
FRACTION_RE = re.compile(r'\b(?:half|halves|halved|halving|one[\s-]+third|a\s+third|two[\s-]+thirds|'
                         r'one[\s-]+quarter|a\s+quarter|three[\s-]+quarters|quarter\s+of)\b(?![\s-]*part(?:y|ies))', re.I)


def check_quantities(text, where, errors, warnings):
    """The services additions to the bios number check. Marked [[...]] numbers and @KEY names are skipped."""
    masked = B.MARK_RE.sub(lambda m: '\x00' * len(m.group(0)), text)
    masked = B.KEY_RE.sub(lambda m: '\x00' * len(m.group(0)), masked)
    plain = B.MARK_RE.sub(lambda m: m.group(1), text)
    for rx, what in ((COUNT_RE, 'unsourced count'), (WORD_UNIT_RE, 'unsourced quantity in words'),
                     (FRACTION_RE, 'unsourced fraction')):
        for m in rx.finditer(masked):
            errors.append('%s: %s %r in: %s' % (where, what, m.group(0).strip(), plain))
    if warnings is not None:
        for w in B.SOFT_WARN.finditer(masked):
            if FRACTION_RE.fullmatch(w.group(0)):
                continue   # already an error above
            warnings.append('%s: vague quantity %r (check it, or give it a basis)' % (where, w.group(0)))


def render(text, where, kb, errors, warnings):
    """The bios render (unit numbers, owner text, markers) plus the services quantity checks."""
    check_quantities(text, where, errors, warnings)
    return B.render(text, where, kb.SOURCES, [], errors)


def render_basis(basis, note, where, kb, errors, warnings):
    check_quantities(note, '%s note' % where, errors, warnings)
    return B.render_basis({'basis': basis, 'basis_note': note}, where, True, kb.SOURCES, errors)


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


def check_entry(e, kb, errors, warnings=None):
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
        texts[f] = render(t, '%s %s' % (where, f), kb, errors, warnings)
    if not e.get('title') or not e.get('expl'):
        errors.append('%s: title and expl are required' % where)
    # Explanation basis (every entry is tier 1).
    render_basis(e.get('basis', ''), e.get('basis_note', ''), where, kb, errors, warnings)
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
        why[i] = render(w, '%s %s why' % (where, L), kb, errors, warnings)
    for L in levels:
        if L not in LEVELS:
            errors.append('%s: unknown level %r' % (where, L))
    if levels:
        if not e.get('pbasis'):
            errors.append('%s: preset membership without a pbasis' % where)
        render_basis(e.get('pbasis', ''), e.get('pbasis_note', ''), '%s pbasis' % where, kb, errors, warnings)
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
        render(con, '%s contested' % where, kb, errors, warnings)
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
        render_basis(e.get('stock_basis', ''), e.get('stock_note', ''), '%s stock' % where, kb, errors, warnings)
    elif e.get('stock_basis') or e.get('stock_note'):
        errors.append('%s: stock_basis without a stock value' % where)
    builds = e.get('builds') or []
    if not builds or not all(re.fullmatch(r'\d{5}', b) for b in builds):
        errors.append('%s: builds must be five-digit build numbers' % where)
    for f in ('sources',):
        if e.get(f):
            render(e[f], '%s %s' % (where, f), kb, errors, warnings)

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


def generate(kb, warnings=None):
    """Returns (db text, errors); soft warnings go to `warnings` when given."""
    errors = []
    lines = ['V\t1\tservices', 'B\t%s' % kb.BUILD]
    seen = set()
    for e in kb.ENTRIES:
        n = e.get('name', '').lower()
        if n in seen:
            errors.append('duplicate entry %s' % e.get('name'))
        seen.add(n)
        lines.append(check_entry(e, kb, errors, warnings))
    if len(kb.ENTRIES) < SEED_MIN:
        errors.append('the KB has %d entries; the spec asks for at least %d' % (len(kb.ENTRIES), SEED_MIN))
    for lvl in LEVELS:
        if not any(lvl in e.get('levels', {}) and e.get('kind') == 'svc' for e in kb.ENTRIES):
            errors.append('no Services entry at level %s' % lvl)
    if not any(e.get('levels') and e.get('kind') == 'drv' for e in kb.ENTRIES):
        errors.append('no Drivers entry with a level')
    return '\n'.join(lines) + '\n', errors


def build(dst):
    kb = load_kb()
    warnings = []
    text, errors = generate(kb, warnings)
    for w in warnings:
        print('genkb: warning: ' + w, file=sys.stderr)
    if errors:
        for e in errors:
            print('genkb: ' + e, file=sys.stderr)
        print('genkb: %d error(s); %s not written' % (len(errors), dst), file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    lv = {L: sum(1 for e in kb.ENTRIES if L in e.get('levels', {})) for L in LEVELS}
    print('genkb: wrote %s (%d entries; bloat %d, stripped %d, minimum %d; %d explanation-only; %d warning(s))' %
          (dst, len(kb.ENTRIES), lv['bloat'], lv['stripped'], lv['minimum'],
           sum(1 for e in kb.ENTRIES if not e.get('levels')), len(warnings)))
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


def errors_for(e, base, warnings=None):
    errs = []
    check_entry(e, FakeKb([e], base), errs, warnings)
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
        # kb-audit F7: counts, sizes and durations in words, fractions; in every text field and note.
        ('count of processes', good(expl='Starts 40 processes fewer.')),
        ('count of services', good(if_off='Removes 30 services.')),
        ('count of threads', good(expl='Uses 12 threads.')),
        ('count in words', good(expl='Starts forty fewer processes.')),
        ('count in words, hyphenated', good(test='About twenty-five background services stop.')),
        ('count of svchost instances', good(expl='Runs in 3 svchost instances.')),
        ('size in words', good(if_off='Frees 200 megabytes.')),
        ('size in words, no digits', good(expl='Frees two hundred megabytes.')),
        ('fraction (half)', good(expl='Saves half the RAM.')),
        ('fraction (a third)', good(if_off='Boot takes a third less time.')),
        ('duration in words', good(expl='Two seconds faster.')),
        ('percent in words', good(expl='Ten percent fewer stalls.')),
        ('count in a why', good(levels={'stripped': (4, 'stops 5 services')})),
        ('count in a basis note', good(basis_note='@IOT; frees 50 megabytes.')),
        ('count in a pbasis note', good(pbasis_note='Removes 30 services.')),
        ('count in a contested note', good(contested='@IOT says keep; @VDOT disables 20 services.')),
    ]
    for what, e in bad:
        expect(errors_for(e, base) != [], 'fails: ' + what)
    ok_texts = ['Wired port authentication (dot1x) on campus networks.', 'Windows 10/11 IoT Enterprise.',
                'Windows Update fails with error 0x80070422.', 'Stop 0x7B after a filter is removed.',
                'Microsoft\'s Windows 10 2004 VDI guide.', 'Windows Server 2016 with Desktop Experience.']
    ok_texts += ['Windows Server 2016 services and Windows 10 20H2 drivers.', 'One of the Xbox services.', 'Windows 10/11 services.',
                 'The USB Audio 2.0 driver.',
                 'Restart three times each way.', 'Behalf of the user.', 'Detects a third-party antivirus.', 'A per-user service: one instance per user.',
                 'Frees [[200 MB|published|@IOT]] of RAM.']
    for t in ok_texts:
        expect(errors_for(good(expl=t), base) == [], 'passes: ' + t)
    w = []
    expect(errors_for(good(expl='Starts twice as fast.'), base, w) == [] and len(w) == 1,
           'warns, does not fail: a vague quantity ("twice")')
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
