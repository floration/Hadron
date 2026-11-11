#!/usr/bin/env python3
"""Build modules/bios/data/settings.db (v2) from the legacy settings.db and tools/bios/kb.py.

    python3 tools/bios/genkb.py <legacy settings.db> <out settings.db>
    python3 tools/bios/genkb.py --selftest

Python 3 standard library only (a build tool; rule 3 allows it). See tools/bios/README.md.

The S lines (data extracted from the BIOS image, which is not in the repo) are copied verbatim from the legacy
file except field 16 (KB index), which is re-matched against the KB here in legacy gen.py's order:
tier-1 regex on the prompt -> NAME regex on the stripped prompt -> MENU regex on the joined menu paths -> FALLBACK.

The run fails (exit 1, naming the entry, the field and the text) when rule 7 is broken:
  * a tier-1 entry has no basis, or any entry has an unknown basis;
  * an owner-measured basis does not name an @OWNER* source, or a published basis names no https source;
  * a number in user-facing text or in a basis_note is not inside a [[text|kind|ref]] marker;
  * a marker has a kind other than owner-measured/published, a ref that is not @KEY(s) from kb.SOURCES of the
    right type (OWNER* for owner-measured, https URL for published), or a text with no digit or over 40 chars;
  * text contains an owner-specific phrase (OWNER_DENY).
"""
import os
import re
import sys

BOARD = 'MS-7E06'
BIOS_VERSION = '7E06vAF'
BASES = ('owner-measured', 'published', 'reasoning', 'folklore')
NUMBER_KINDS = ('owner-measured', 'published')
US, RS = '\x1f', '\x1e'
GOALS = ('power', 'balanced', 'single', 'multi', 'latency')
MARK_MAX = 40

# ---- number heuristic ------------------------------------------------------------------------------------------
# A number (optionally signed, "~", or a range "a-b" / "a to b"), then an optional space or hyphen, then a unit.
# Case-insensitive. Based on docs/legacy/bios-explorer.md section 7, extended (kb-auditor round 1).
# Not matched, because no unit follows the number or the number is glued to a word: DDR5-4800, Gen4, x16/x8,
# 14700K, 9900K, 0x12B, PL1, TPM 2.0, Windows 11, M.2, "Gear 2", "4G" (the name of the 4 GB address boundary).
UNIT = ('(?:'
        r'GB/s|MB/s|GT/s|MT/s|Gbps|GHz|MHz|mOhm|mΩ|mV|ms|µs|μs|us|ns|minutes?|min|hours?|seconds?|s|'
        r'bytes?|KB|MB|GB|watts?|W|volts?|V|amps?|A|percent|%|°\s?C|C|Ω|×|x|fps|cycles'
        ')')
NUMBER_RE = re.compile(
    r'(?<![\w.])[+~±-]?\d+(?:\.\d+)?(?:\s*(?:-|–|to)\s*\d+(?:\.\d+)?)?[\s-]?' + UNIT + r'(?!\w)'
    r'|(?<![\w.])\d+\.\d+G(?!\w)',          # decimal G link speeds, e.g. 2.5G Ethernet
    re.I)
# "1% lows" and "0.1% lows" are metric names, not quantities ("1% and 0.1% lows" included).
METRIC_RE = re.compile(r'(?<![\d.])(?:0\.)?1%(?=\s+(?:and\s+0\.1%\s+)?lows)')
MARK_RE = re.compile(r'\[\[([^\[\]|]+)\|([^\[\]|]+)\|([^\[\]]+)\]\]')
KEY_RE = re.compile(r'@([A-Z0-9_]+)')

# Owner-specific text never ships in the KB (rule 7 / legacy section 7 #27). Case-insensitive.
OWNER_DENY = re.compile(r'14700KF|your PC\b|your 14700|\b53x\b|7E06|his current', re.I)
# Vague quantities worth a second look; reported as warnings, never failures.
SOFT_WARN = re.compile(r'a few percent|several watts|\bhalves\b|\bdoubles\b|tens of|an hour', re.I)


def esc(s):
    """Same as legacy gen.py esc(): backslash doubled, TAB -> space, CR dropped, LF -> \\n."""
    return str(s).replace('\\', '\\\\').replace('\t', ' ').replace('\r', '').replace('\n', '\\n')


def unesc(s):
    """Inverse of esc() for the characters esc() produces (legacy bsx.cpp Unesc)."""
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            i += 1
            out.append('\n' if s[i] == 'n' else s[i])
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def unmarked_numbers(text):
    """Numbers the heuristic flags in plain text (metric names masked)."""
    masked = METRIC_RE.sub(lambda m: '\x01' * len(m.group(0)), text)
    return [m.group(0) for m in NUMBER_RE.finditer(masked)]


def resolve_ref(ref, kind, sources):
    """Expand '@KEY @KEY' to values; returns (value, error or None)."""
    keys = ref.split()
    if not keys or not all(KEY_RE.fullmatch(k) for k in keys):
        return '', 'ref %r must be @KEY name(s) from kb.SOURCES' % ref
    vals = []
    for k in keys:
        name = k[1:]
        if name not in sources:
            return '', 'unknown source @%s' % name
        v = sources[name]
        if kind == 'owner-measured':
            if not name.startswith('OWNER'):
                return '', 'owner-measured ref @%s is not an OWNER* source' % name
        elif name.startswith('OWNER') or not re.fullmatch(r'https://\S+', v):
            return '', 'published ref @%s is not an https:// URL (%r)' % (name, v)
        vals.append(v)
    return ' '.join(vals), None


def render(text, where, sources, out_sources, errors, warnings=None):
    """Strip markers to plain text; collect (text, kind, resolved ref); flag unmarked numbers and denied text."""
    def sub(m):
        shown, kind, ref = m.group(1), m.group(2).strip(), m.group(3).strip()
        if kind not in NUMBER_KINDS:
            errors.append('%s: [[%s]] has kind %r; a number needs owner-measured or published' % (where, shown, kind))
        if not re.search(r'\d', shown) or len(shown) > MARK_MAX:
            errors.append('%s: [[%s]] marker text must contain a digit and be at most %d chars' % (where, shown, MARK_MAX))
        val, err = resolve_ref(ref, kind, sources)
        if err and kind in NUMBER_KINDS:
            errors.append('%s: [[%s]] %s' % (where, shown, err))
        out_sources.append((shown, kind, val))
        return '\x00' * len(shown)   # placeholder so the number check skips marked text
    masked = MARK_RE.sub(sub, text)
    plain = MARK_RE.sub(lambda m: m.group(1), text)
    if '[[' in masked or ']]' in masked:
        errors.append('%s: malformed [[...]] marker in %r' % (where, text))
    for n in unmarked_numbers(masked):
        errors.append('%s: unsourced number %r in: %s' % (where, n, plain))
    m = OWNER_DENY.search(plain)
    if m:
        errors.append('%s: owner-specific text %r in: %s' % (where, m.group(0), plain))
    if warnings is not None:
        for w in SOFT_WARN.finditer(plain):
            warnings.append('%s: vague quantity %r' % (where, w.group(0)))
    return plain


def render_basis(e, where, tier1, sources, errors):
    """Check basis/basis_note; return the basis_note with @KEYs expanded."""
    b, note = e.get('basis', ''), e.get('basis_note', '')
    if not b:
        if tier1:
            errors.append('%s: tier-1 entry has no basis' % where)
        return note
    if b not in BASES:
        errors.append('%s: unknown basis %r' % (where, b))
    for n in unmarked_numbers(MARK_RE.sub('', KEY_RE.sub('', note))):
        errors.append('%s basis_note: unsourced number %r in: %s' % (where, n, note))
    keys = KEY_RE.findall(note)
    for k in keys:
        if k not in sources:
            errors.append('%s basis_note: unknown source @%s' % (where, k))
    if b == 'owner-measured' and not any(k.startswith('OWNER') for k in keys):
        errors.append('%s: owner-measured basis must name an @OWNER* source in basis_note' % where)
    if b == 'published' and not any(not k.startswith('OWNER') and sources.get(k, '').startswith('https://')
                                    for k in keys):
        errors.append('%s: published basis must name an https source (@KEY) in basis_note' % where)
    if 'http://' in note:
        errors.append('%s: basis_note has a non-https URL' % where)
    expanded = KEY_RE.sub(lambda m: sources.get(m.group(1), m.group(0)), note)
    dm = OWNER_DENY.search(expanded)
    if dm:
        errors.append('%s basis_note: owner-specific text %r' % (where, dm.group(0)))
    return expanded


def entry_texts(e):
    yield 'title', e['title']
    yield 'explanation', e['expl']
    for g in GOALS:
        yield 'rec.' + g, e['recs'][g]
    yield 'note', e['note']
    for name, t in zip(('impact.fps', 'impact.games', 'impact.apps', 'impact.claim'), e['impact']):
        yield name, t


def k_line(i, e, where, tier1, sources, errors, warnings):
    bnote = render_basis(e, where, tier1, sources, errors)
    srcs, r = [], {}
    for label, text in entry_texts(e):
        r[label] = render(text, '%s %s' % (where, label), sources, srcs, errors, warnings)
    f = ['K', str(i), esc(r['title']), esc(r['explanation'])]
    f += [esc(r['rec.' + g]) for g in GOALS]
    f.append(esc(r['note']))
    f += [esc(r[k]) for k in ('impact.fps', 'impact.games', 'impact.apps', 'impact.claim')]
    f += [e.get('basis', ''), esc(bnote)]
    f.append(RS.join(esc(n) + US + k + US + esc(ref) for n, k, ref in srcs))
    return '\t'.join(f)


def malformed(e):
    missing = [k for k in ('rx', 'title', 'expl', 'recs', 'note', 'impact') if k not in e]
    if missing:
        return 'missing %s' % missing
    if set(e['recs']) != set(GOALS) or len(e['impact']) != 4:
        return 'recs/impact shape'
    return None


def build(src, dst):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import kb
    sources = kb.SOURCES
    with open(src, encoding='utf-8', newline='') as fh:
        legacy = fh.read().split('\n')
    v_line = next((ln for ln in legacy if ln.startswith('V\t')), None)
    s_lines = [ln for ln in legacy if ln.startswith('S\t')]
    if v_line is None or not s_lines:
        print('error: %s has no V line or no S lines' % src, file=sys.stderr)
        return 1

    bad_src = [k for k, v in sources.items() if not k.startswith('OWNER') and not re.fullmatch(r'https://\S+', v)]
    errors = ['SOURCES[%r] is not an https:// URL' % k for k in bad_src]
    warnings = []
    tiers = [('T1', kb.T1, True), ('NAME', kb.NAME, False), ('MENU', kb.MENU, False), ('FALLBACK', [kb.FALLBACK], False)]
    out, idx, ranges = [v_line, 'B\t%s\t%s' % (BOARD, BIOS_VERSION)], 0, {}
    for tname, entries, tier1 in tiers:
        if tname == 'NAME':
            out.append('T\t%d' % len(kb.T1))
        ranges[tname] = (idx, idx + len(entries))
        for j, e in enumerate(entries):
            where = 'K %d (%s #%d "%s")' % (idx, tname, j, e.get('title', '?'))
            bad = malformed(e)
            if bad:
                errors.append('%s: malformed entry (%s)' % (where, bad))
            else:
                out.append(k_line(idx, e, where, tier1, sources, errors, warnings))
            idx += 1

    for w in warnings:
        print('warning: ' + w, file=sys.stderr)
    if errors:
        print('genkb: FAILED, %d problem(s):' % len(errors), file=sys.stderr)
        for e in errors:
            print('  ' + e, file=sys.stderr)
        return 1

    def compiled(tname):
        base = ranges[tname][0]
        return [(re.compile(e['rx'], re.I), base + j) for j, e in enumerate(getattr(kb, tname))]
    kre, nre, mre = compiled('T1'), compiled('NAME'), compiled('MENU')
    fb = ranges['FALLBACK'][0]
    stats = dict(tier1=0, name=0, menu=0, fallback=0)
    changed = 0
    for ln in s_lines:
        f = ln.split('\t')
        if len(f) != 17:
            print('error: S line with %d fields: %r' % (len(f), ln[:80]), file=sys.stderr)
            return 1
        prompt, paths = unesc(f[12]), unesc(f[14])
        k = next((i for r, i in kre if r.search(prompt)), -1)
        if k >= 0:
            stats['tier1'] += 1
        else:
            k = next((i for r, i in nre if r.search(prompt.strip())), -1)
            if k >= 0:
                stats['name'] += 1
            else:
                k = next((i for r, i in mre if r.search(paths)), -1)
                if k >= 0:
                    stats['menu'] += 1
                else:
                    k = fb
                    stats['fallback'] += 1
        if str(k) != f[16]:
            changed += 1
        f[16] = str(k)
        out.append('\t'.join(f))

    data = '\n'.join(out) + '\n'
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(data)
    print('K entries: %d (tier 1 %d, NAME %d, MENU %d, FALLBACK 1)' % (idx, len(kb.T1), len(kb.NAME), len(kb.MENU)))
    print('settings: %d; matched tier 1 %d, NAME %d, MENU %d, FALLBACK %d' % (
        len(s_lines), stats['tier1'], stats['name'], stats['menu'], stats['fallback']))
    print('KB index differs from the legacy file on %d settings' % changed)
    nsrc = sum(1 for ln in out if ln.startswith('K\t') and ln.split('\t')[16])
    print('entries with sourced numbers: %d; warnings: %d; wrote %s (%d bytes)' % (
        nsrc, len(warnings), dst, len(data.encode('utf-8'))))
    return 0


# ---- selftest ---------------------------------------------------------------------------------------------------
FLAG = [  # must be flagged as unsourced numbers
    '10-30% throughput', 'about +20%', '~30 ms', '1 ms', '5 µs', '5 μs', '200 ns', '1.55 V', '1.1 mOhm', '1.1 mΩ',
    '307 A', '253 W', '125 watts', '1.4 volts', '100 amps', '5.6 GHz', '100 MHz', '256 KB', '2 MB', '16 GB',
    '100 °C', '100°C', '100 C', '53x', '55-56x', '4×', '144 fps', '50 cycles', 'DDR5 at 6400 MT/s', '16 GT/s',
    '5 GB/s', '500 MB/s', '10 Gbps', '64-byte line', '64 bytes', '2.5G Ethernet', '10 percent', '30 seconds',
    '5 min', '2 hours', '3 s', '21% lows', '1 to 3%', '0.2 MS', '4 GB of address space',
]
PASS = [  # must not be flagged
    '1% lows', '0.1% lows', 'compare 1% and 0.1% lows', 'DDR5-4800', 'Gen4', 'x16 vs x8', 'x8+x8', 'i7-14700K',
    'i9-9900K', '0x12B', '0x125 and 0x129', 'PL1 = PL2', 'TPM 2.0', 'Windows 11', 'M.2 SATA', 'Gear 2 for DDR5',
    'Above 4G Decoding', 'the 32-bit (4G) boundary', '3+ runs', '0 = default', 'entered here as 1228', 'PS1-PS4',
    'S4/S5', '12th-14th gen', 'a 4-point fan curve', '2-DIMM-per-channel', 'USB 2 speed mode', 'Turbo Boost Max 3.0',
    '1N is fastest', 'Gear 4 costs latency', 'C0/C1', 'Package C-State', 'tRRD_S', 'DDR5 on 14th gen',
]


def selftest():
    fails = []
    for t in FLAG:
        if not unmarked_numbers(t):
            fails.append('not flagged: %r' % t)
    for t in PASS:
        hits = unmarked_numbers(t)
        if hits:
            fails.append('false positive %r in %r' % (hits, t))
    src = {'URL': 'https://example.org/a', 'BAD': 'http://example.org', 'OWNERX': "author's CPU, game, metric"}

    def run(text):
        errs, s = [], []
        plain = render(text, 'T', src, s, errs)
        return plain, s, errs
    cases = [  # (text, expect error substring or None)
        ('rated [[5.6 GHz|published|@URL]] turbo', None),
        ('[[about +20%|owner-measured|@OWNERX]] better', None),
        ('[[5.6 GHz|published|https://example.org/a]]', 'must be @KEY'),
        ('[[5.6 GHz|published|@BAD]]', 'not an https'),
        ('[[5.6 GHz|published|@OWNERX]]', 'not an https'),
        ('[[5.6 GHz|published|@NOPE]]', 'unknown source'),
        ('[[+20%|owner-measured|@URL]]', 'not an OWNER'),
        ('[[5.6 GHz|reasoning|@URL]]', 'has kind'),
        ('[[fast|published|@URL]]', 'contain a digit'),
        ('[[' + 'x' * 41 + ' 1|published|@URL]]', 'at most'),
        ('[[5.6 GHz|published|@URL]] and 5.5 GHz', 'unsourced number'),
        ('Your PC is set to Override', 'owner-specific'),
        ('on the 14700KF', 'owner-specific'),
        ('his current CPU', 'owner-specific'),
    ]
    for text, want in cases:
        plain, s, errs = run(text)
        got = '; '.join(errs)
        if want is None and errs:
            fails.append('unexpected error for %r: %s' % (text, got))
        elif want is not None and want not in got:
            fails.append('expected %r for %r, got %r' % (want, text, got))
    plain, s, errs = run('rated [[5.6 GHz|published|@URL]] turbo')
    if plain != 'rated 5.6 GHz turbo' or s != [('5.6 GHz', 'published', 'https://example.org/a')]:
        fails.append('render/sources wrong: %r %r' % (plain, s))
    for note, basis, want in [('@URL', 'published', None), ('@OWNERX. Different CPU.', 'owner-measured', None),
                              ('no url', 'published', 'https source'), ('@URL', 'owner-measured', 'OWNER'),
                              ('Measured 20% faster.', 'reasoning', 'unsourced number'), ('', '', 'no basis'),
                              ('x', 'guess', 'unknown basis')]:
        errs = []
        render_basis({'basis': basis, 'basis_note': note}, 'B', True, src, errs)
        got = '; '.join(errs)
        if (want is None and errs) or (want is not None and want not in got):
            fails.append('basis %r/%r: expected %r, got %r' % (basis, note, want, got))
    if esc('a\\b\tc\r\nd') != 'a\\\\b c\\nd' or unesc(esc('a\\b\nc')) != 'a\\b\nc':
        fails.append('esc/unesc round trip')
    total = len(FLAG) + len(PASS) + len(cases) + 1 + 7 + 1
    for f in fails:
        print('FAIL ' + f)
    print('selftest: %d checks (%d flag, %d pass, %d marker, 7 basis, 2 render/escape), %d failed'
          % (total, len(FLAG), len(PASS), len(cases), len(fails)))
    return 1 if fails else 0


def main(argv):
    if len(argv) == 2 and argv[1] == '--selftest':
        return selftest()
    if len(argv) != 3:
        print('usage: genkb.py <legacy settings.db> <out settings.db> | --selftest', file=sys.stderr)
        return 2
    return build(argv[1], argv[2])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
