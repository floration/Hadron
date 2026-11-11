"""Find HII string + form packages in a module and parse the IFR into a list of setup questions."""
import struct, uuid

def guid_str(b): return str(uuid.UUID(bytes_le=bytes(b)))

# ---------------- string packages ----------------
def find_string_packages(blob):
    out = []
    pos = 0
    while True:
        i = blob.find(b'en-US\x00', pos)
        if i < 0: break
        pos = i + 1
        start = i - 46          # header(4) HdrSize(4) StringInfoOffset(4) LanguageWindow(32) LanguageName(2)
        if start < 0: continue
        hdr = struct.unpack_from('<I', blob, start)[0]
        plen, ptype = hdr & 0xFFFFFF, hdr >> 24
        if ptype != 0x04: continue
        hsize, sinfo = struct.unpack_from('<II', blob, start + 4)
        if hsize != sinfo or hsize < 46 or plen < hsize or start + plen > len(blob): continue
        strs = parse_strings(blob[start + sinfo:start + plen])
        if strs: out.append((start, plen, strs))
    return out

def parse_strings(b):
    """UEFI HII string blocks (UEFI spec 33.3.6.2)."""
    strs = {}
    sid = 1; o = 0
    def ucs2(o):
        e = o
        while e + 1 < len(b) and (b[e] or b[e+1]): e += 2
        return b[o:e].decode('utf-16le', 'replace'), e + 2
    def scsu(o):
        e = b.index(0, o); return b[o:e].decode('latin-1'), e + 1
    while o < len(b):
        t = b[o]; o += 1
        if t == 0x00: break
        elif t == 0x10: strs[sid], o = scsu(o); sid += 1
        elif t == 0x11: o += 1; strs[sid], o = scsu(o); sid += 1
        elif t in (0x12, 0x13):
            if t == 0x13: o += 1
            n = int.from_bytes(b[o:o+2], 'little'); o += 2
            for _ in range(n): strs[sid], o = scsu(o); sid += 1
        elif t == 0x14: strs[sid], o = ucs2(o); sid += 1
        elif t == 0x15: o += 1; strs[sid], o = ucs2(o); sid += 1
        elif t in (0x16, 0x17):
            if t == 0x17: o += 1
            n = int.from_bytes(b[o:o+2], 'little'); o += 2
            for _ in range(n): strs[sid], o = ucs2(o); sid += 1
        elif t == 0x20:
            ref = int.from_bytes(b[o:o+2], 'little'); o += 2
            strs[sid] = strs.get(ref, ''); sid += 1
        elif t == 0x21: sid += int.from_bytes(b[o:o+2], 'little'); o += 2
        elif t == 0x22: sid += b[o]; o += 1
        elif t == 0x30: o += b[o+1] - 1
        elif t == 0x31: o += int.from_bytes(b[o+1:o+3], 'little') - 1
        elif t == 0x32: o += int.from_bytes(b[o+1:o+5], 'little') - 1
        else: break
    return strs

# ---------------- form packages ----------------
def find_form_packages(blob):
    out = []
    pos = 0
    while True:
        i = blob.find(b'\x0e', pos)
        if i < 0: break
        pos = i + 1
        start = i - 4
        if start < 0: continue
        hdr = struct.unpack_from('<I', blob, start)[0]
        plen, ptype = hdr & 0xFFFFFF, hdr >> 24
        if ptype != 0x02 or plen < 30 or start + plen > len(blob): continue
        ln = blob[i + 1] & 0x7F
        if ln < 23 or not (blob[i + 1] & 0x80): continue
        body = blob[i:start + plen]
        if body[-2:] != b'\x29\x02': continue   # must end with END (closing the formset)
        out.append((start, plen, body))
    return out

# value sizes for ONE_OF/NUMERIC flags & 0x0F
SZ = {0: 1, 1: 2, 2: 4, 3: 8}
VAL_TYPE = {0: 1, 1: 2, 2: 4, 3: 8, 4: 1, 5: 0, 6: 0, 7: 0}   # ONE_OF_OPTION / DEFAULT value types

def rd(b, o, n):
    return int.from_bytes(b[o:o + n], 'little')

def parse_form(body, strs):
    S = lambda i: strs.get(i, '')
    varstores = {}
    forms = {}            # form id -> title
    refs = []             # (from form, to form)
    questions = []
    scope = []            # stack of (opcode, info)
    cur_form = None
    cur_q = None
    formset = {}
    cond_stack = []       # active suppress/grayout conditions: list of ('suppress'|'gray'|'disable', kind) ; kind 'true' | 'expr'
    expr_capture = None   # list collecting opcodes of the expression right after SUPPRESS_IF etc.
    o = 0
    while o + 2 <= len(body):
        op = body[o]; ln = body[o + 1] & 0x7F; sc = body[o + 1] >> 7
        if ln < 2 or o + ln > len(body): break
        d = body[o + 2:o + ln]
        # an expression immediately following a condition opcode, until the first op that is not an expression op
        if op == 0x0E:     # FORM_SET
            formset = dict(guid=guid_str(d[0:16]), title=S(rd(d, 16, 2)))
        elif op == 0x01:   # FORM
            cur_form = rd(d, 0, 2); forms[cur_form] = S(rd(d, 2, 2))
        elif op == 0x24:   # VARSTORE
            g = guid_str(d[0:16]); vid = rd(d, 16, 2); size = rd(d, 18, 2)
            name = d[20:].split(b'\x00')[0].decode('latin-1')
            varstores[vid] = dict(guid=g, name=name, size=size)
        elif op == 0x26:   # VARSTORE_EFI
            g = guid_str(d[0:16]) if False else guid_str(d[2:18]) if False else None
            vid = rd(d, 0, 2); g = guid_str(d[2:18]); attr = rd(d, 18, 4)
            size = rd(d, 22, 2) if len(d) >= 24 else 0
            name = d[24:].split(b'\x00')[0].decode('latin-1') if len(d) > 24 else ''
            varstores[vid] = dict(guid=g, name=name, size=size)
        elif op == 0x0F:   # REF
            if len(d) >= 13:
                refs.append((cur_form, rd(d, 11, 2)))
        elif op in (0x05, 0x07, 0x06):   # ONE_OF, NUMERIC, CHECKBOX
            prompt, help_, qid, vsid, voff, qflags = rd(d, 0, 2), rd(d, 2, 2), rd(d, 4, 2), rd(d, 6, 2), rd(d, 8, 2), d[10]
            q = dict(kind={0x05: 'oneof', 0x07: 'numeric', 0x06: 'checkbox'}[op], prompt=S(prompt).strip(), help=S(help_).strip(),
                     qid=qid, varstore=vsid, offset=voff, form=cur_form,
                     suppress=[c for c in cond_stack if c[0] in ('suppress', 'disable')],
                     gray=[c for c in cond_stack if c[0] == 'gray'], options=[], default=None, mfg=None)
            if op == 0x06:
                f = d[11] if len(d) > 11 else 0
                q['width'] = 1; q['default'] = f & 1; q['mfg'] = (f >> 1) & 1
                q['options'] = [(0, 'Disabled'), (1, 'Enabled')]
            else:
                f = d[11]; w = SZ[f & 0x0F]; q['width'] = w
                if op == 0x07:
                    q['min'] = rd(d, 12, w); q['max'] = rd(d, 12 + w, w); q['step'] = rd(d, 12 + 2 * w, w)
                    q['hex'] = bool(f & 0x10)
            questions.append(q)
            if sc: cur_q = q
        elif op == 0x09 and cur_q is not None:   # ONE_OF_OPTION
            sid, fl, ty = rd(d, 0, 2), d[2], d[3]
            n = VAL_TYPE.get(ty, 0)
            val = rd(d, 4, n) if n else 0
            cur_q['options'].append((val, S(sid).strip()))
            if fl & 0x10: cur_q['default'] = val
            if fl & 0x20: cur_q['mfg'] = val
        elif op == 0x5B and cur_q is not None:   # DEFAULT
            did, ty = rd(d, 0, 2), d[2]; n = VAL_TYPE.get(ty, 0)
            val = rd(d, 3, n) if n else None
            if val is not None:
                if did == 0: cur_q['default'] = val
                elif did == 1: cur_q['mfg'] = val
        # ---- scope handling ----
        if op in (0x0A, 0x19, 0x5F if False else 0x1E):
            pass
        if op == 0x0A or op == 0x19 or op == 0x1E:   # SUPPRESS_IF, GRAY_OUT_IF, DISABLE_IF
            kind = {0x0A: 'suppress', 0x19: 'gray', 0x1E: 'disable'}[op]
            # look at the expression right after: TRUE (0x46) alone means unconditional
            nxt = body[o + ln] if o + ln < len(body) else 0
            nxt_len = body[o + ln + 1] & 0x7F if o + ln + 1 < len(body) else 0
            after = body[o + ln + nxt_len] if o + ln + nxt_len < len(body) else 0
            always = (nxt == 0x46 and after != 0x15 and after != 0x16)   # TRUE not combined with AND/OR
            entry = (kind, 'true' if always else 'expr')
            cond_stack.append(entry)
            scope.append(('cond', entry))
        elif sc:
            scope.append((op, None))
        elif op == 0x29:   # END
            if scope:
                kind, info = scope.pop()
                if kind == 'cond':
                    if cond_stack: cond_stack.pop()
                elif kind in (0x05, 0x07, 0x06):
                    cur_q = None
                elif kind == 0x01:
                    cur_form = None
        o += ln
    return dict(formset=formset, varstores=varstores, forms=forms, refs=refs, questions=questions)
