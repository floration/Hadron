#!/usr/bin/env python3
"""Build modules/bios/data/settings.db (v2) from the legacy settings.db and tools/bios/kb.py.

    python3 tools/bios/genkb.py <legacy settings.db> <out settings.db>

Python 3 standard library only (a build tool; rule 3 allows it). See tools/bios/README.md.

The S lines (data extracted from the BIOS image, which is not in the repo) are copied verbatim from the legacy
file except field 16 (KB index), which is re-matched against the KB here in legacy gen.py's order:
tier-1 regex on the prompt -> NAME regex on the stripped prompt -> MENU regex on the joined menu paths -> FALLBACK.

The run fails (exit 1, naming the entry and the text) when rule 7 is broken:
  * a tier-1 entry has no basis, or any entry has an unknown basis;
  * an owner-measured basis has no conditions, or a published basis has no https:// URL;
  * a number in user-facing text is not wrapped in [[text|kind|ref]] with kind owner-measured or published;
  * a [[...]] marker has another kind, or a published ref that is not an https:// URL.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kb  # noqa: E402

BOARD = 'MS-7E06'
BIOS_VERSION = '7E06vAF'
BASES = ('owner-measured', 'published', 'reasoning', 'folklore')
NUMBER_KINDS = ('owner-measured', 'published')
US, RS = '\x1f', '\x1e'

# Number heuristic (docs/legacy/bios-explorer.md section 7, extended with mOhm and MT/s): a number, optionally
# signed or a range, followed by a unit. Spec-style identifiers carry no unit after the number and are not
# matched: DDR5-4800, Gen4, x16 lane widths, model numbers (14700K, 9900K), microcode ids (0x12B), PL1/PL2.
UNIT = r'(?:%|ms|µs|us|ns|mV|mOhm|W|V|A|MHz|GHz|KB|MB|GB|°\s?C|x|fps|cycles|MT/s)'
NUMBER_RE = re.compile(
    r'(?<![\w.])[+~±-]?\d+(?:\.\d+)?(?:\s*(?:-|–|to)\s*\d+(?:\.\d+)?)?\s?' + UNIT + r'(?![\w/])')
# "1% lows" and "0.1% lows" are metric names, not quantities ("1% and 0.1% lows" included).
METRIC_RE = re.compile(r'(?:0\.)?1%(?=\s+(?:and\s+0\.1%\s+)?lows)')
MARK_RE = re.compile(r'\[\[([^\[\]|]+)\|([^\[\]|]+)\|([^\[\]]+)\]\]')

GOALS = ('power', 'balanced', 'single', 'multi', 'latency')
FIELDS = ('title', 'expl', 'recs', 'note', 'impact')


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


class Fail(Exception):
    pass


def entry_texts(e):
    """(label, text) for every user-facing text field of an entry."""
    yield 'title', e['title']
    yield 'explanation', e['expl']
    for g in GOALS:
        yield 'rec.' + g, e['recs'][g]
    yield 'note', e['note']
    for name, t in zip(('impact.fps', 'impact.games', 'impact.apps', 'impact.claim'), e['impact']):
        yield name, t


def render(text, where, sources, errors):
    """Strip [[text|kind|ref]] markers to plain text; collect sources; flag unmarked numbers."""
    def sub(m):
        shown, kind, ref = m.group(1), m.group(2).strip(), m.group(3).strip()
        if kind not in NUMBER_KINDS:
            errors.append('%s: [[%s]] has kind %r; a number needs owner-measured or published' % (where, shown, kind))
        elif kind == 'published' and not all(u.startswith('https://') for u in ref.split()):
            errors.append('%s: [[%s]] published ref is not an https:// URL: %r' % (where, shown, ref))
        elif kind == 'owner-measured' and not ref:
            errors.append('%s: [[%s]] owner-measured with no conditions' % (where, shown))
        sources.append((shown, kind, ref))
        return '\x00' * len(shown)   # placeholder so the number check skips marked text
    masked = MARK_RE.sub(sub, text)
    if '[[' in masked or ']]' in masked:
        errors.append('%s: malformed [[...]] marker in %r' % (where, text))
    for m in NUMBER_RE.finditer(METRIC_RE.sub(lambda m: '\x01' * len(m.group(0)), masked)):
        errors.append('%s: unsourced number %r in: %s' % (where, m.group(0), MARK_RE.sub(r'\1', text)))
    return MARK_RE.sub(lambda m: m.group(1), text)


def check_basis(e, where, tier1, errors):
    b, note = e.get('basis', ''), e.get('basis_note', '')
    if not b:
        if tier1:
            errors.append('%s: tier-1 entry has no basis' % where)
        return
    if b not in BASES:
        errors.append('%s: unknown basis %r' % (where, b))
    elif b == 'owner-measured' and not note.strip():
        errors.append('%s: owner-measured basis without conditions' % where)
    elif b == 'published' and not re.search(r'https://\S+', note):
        errors.append('%s: published basis without an https:// URL in basis_note' % where)
    if re.search(r'http://', note):
        errors.append('%s: basis_note has a non-https URL' % where)


def k_line(i, e, where, tier1, errors):
    check_basis(e, where, tier1, errors)
    sources = []
    r = {}
    for label, text in entry_texts(e):
        r[label] = render(text, '%s %s' % (where, label), sources, errors)
    f = ['K', str(i), esc(r['title']), esc(r['explanation'])]
    f += [esc(r['rec.' + g]) for g in GOALS]
    f.append(esc(r['note']))
    f += [esc(r[k]) for k in ('impact.fps', 'impact.games', 'impact.apps', 'impact.claim')]
    f += [e.get('basis', ''), esc(e.get('basis_note', ''))]
    f.append(RS.join(esc(n) + US + k + US + esc(ref) for n, k, ref in sources))
    return '\t'.join(f)


def validate_entry(e, where):
    missing = [k for k in ('rx', 'title', 'expl', 'recs', 'note', 'impact') if k not in e]
    if missing or set(e['recs']) != set(GOALS) or len(e['impact']) != 4:
        raise Fail('%s: malformed entry (missing %s)' % (where, missing or 'recs/impact shape'))


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    src, dst = argv[1], argv[2]
    with open(src, encoding='utf-8', newline='') as fh:
        legacy = fh.read().split('\n')
    v_line = next((ln for ln in legacy if ln.startswith('V\t')), None)
    s_lines = [ln for ln in legacy if ln.startswith('S\t')]
    if v_line is None or not s_lines:
        print('error: %s has no V line or no S lines' % src, file=sys.stderr)
        return 1

    tiers = [('T1', kb.T1, True), ('NAME', kb.NAME, False), ('MENU', kb.MENU, False), ('FALLBACK', [kb.FALLBACK], False)]
    errors, out, idx = [], [v_line, 'B\t%s\t%s' % (BOARD, BIOS_VERSION)], 0
    ranges = {}
    for tname, entries, tier1 in tiers:
        if tname == 'NAME':
            out.append('T\t%d' % len(kb.T1))
        ranges[tname] = (idx, idx + len(entries))
        for j, e in enumerate(entries):
            where = 'K %d (%s #%d "%s")' % (idx, tname, j, e.get('title', '?'))
            try:
                validate_entry(e, where)
            except Fail as ex:
                errors.append(str(ex))
                idx += 1
                continue
            out.append(k_line(idx, e, where, tier1, errors))
            idx += 1

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
    nk = idx
    print('K entries: %d (tier 1 %d, NAME %d, MENU %d, FALLBACK 1)' % (nk, len(kb.T1), len(kb.NAME), len(kb.MENU)))
    print('settings: %d; matched tier 1 %d, NAME %d, MENU %d, FALLBACK %d' % (
        len(s_lines), stats['tier1'], stats['name'], stats['menu'], stats['fallback']))
    print('KB index differs from the legacy file on %d settings' % changed)
    nsrc = sum(1 for ln in out if ln.startswith('K\t') and ln.split('\t')[16])
    print('entries with sourced numbers: %d; wrote %s (%d bytes)' % (nsrc, dst, len(data.encode('utf-8'))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
