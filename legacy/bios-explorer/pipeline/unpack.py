"""Unpack an AMI/UEFI SPI image: walk firmware volumes, FFS files and sections, decompress LZMA
sections, and collect every module's raw sections with its UI name. Output: list of (name, guid, bytes)."""
import struct, lzma, uuid, sys, pickle

LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
LZMAF86_GUID = uuid.UUID('d42ae6bd-1352-4bfb-909a-ca72a6eae889').bytes_le

def guid_str(b): return str(uuid.UUID(bytes_le=bytes(b)))

def lzma_dec(data):
    d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
    return d.decompress(data)

class Ctx:
    def __init__(self): self.modules = []; self.seen_fv = set()

def parse_sections(ctx, data, fname, fguid, depth):
    off = 0
    out = []
    while off + 4 <= len(data):
        size = data[off] | data[off+1] << 8 | data[off+2] << 16
        typ = data[off+3]
        hdr = 4
        if size == 0xFFFFFF:
            if off + 8 > len(data): break
            size = struct.unpack_from('<I', data, off+4)[0]; hdr = 8
        if size < hdr or off + size > len(data): break
        body = data[off+hdr:off+size]
        if typ == 0x01:  # compression
            ulen, ctype = struct.unpack_from('<IB', body, 0)
            payload = body[5:]
            if ctype == 0: out += parse_sections(ctx, payload, fname, fguid, depth+1)
            # ctype 1 = EFI/Tiano compression: not used by AMI for setup modules here
        elif typ == 0x02:  # GUID defined
            g = body[:16]; doff, attr = struct.unpack_from('<HH', body, 16)
            payload = data[off+doff:off+size]
            if g in (LZMA_GUID, LZMAF86_GUID):
                try:
                    dec = lzma_dec(payload)
                    out += parse_sections(ctx, dec, fname, fguid, depth+1)
                except Exception as e:
                    pass
            else:
                out += parse_sections(ctx, payload, fname, fguid, depth+1)
        elif typ == 0x17:  # FV image
            parse_fv(ctx, body, depth+1)
        elif typ == 0x15:
            try: fname[0] = body.decode('utf-16le').rstrip('\x00')
            except Exception: pass
        else:
            out.append((typ, body))
        off += size
        off = (off + 3) & ~3
    return out

def parse_fv(ctx, fv, depth=0):
    if len(fv) < 0x40 or fv[0x28:0x2C] != b'_FVH': return
    fvlen = struct.unpack_from('<Q', fv, 0x20)[0]
    hlen = struct.unpack_from('<H', fv, 0x30)[0]
    ext = struct.unpack_from('<H', fv, 0x34)[0]
    fv = fv[:fvlen] if fvlen <= len(fv) else fv
    off = hlen
    if ext:
        esize = struct.unpack_from('<I', fv, ext + 16)[0]
        off = max(off, ext + esize)
    off = (off + 7) & ~7
    while off + 24 <= len(fv):
        name = fv[off:off+16]
        if name == b'\xff' * 16: break
        typ = fv[off+18]; attr = fv[off+19]
        size = fv[off+20] | fv[off+21] << 8 | fv[off+22] << 16
        hdr = 24
        if attr & 0x01:
            size = struct.unpack_from('<Q', fv, off+24)[0]; hdr = 32
        if size < hdr or off + size > len(fv): break
        body = fv[off+hdr:off+size]
        g = guid_str(name)
        if typ in (0xF0,):  # pad
            pass
        elif typ in (0x01, 0x02):  # raw / freeform
            if body[0x28:0x2C] == b'_FVH': parse_fv(ctx, body, depth+1)
            ctx.modules.append(('raw', g, typ, [(0x19, body)]))
        else:
            nm = ['']
            secs = parse_sections(ctx, body, nm, g, 0)
            ctx.modules.append((nm[0], g, typ, secs))
        off += size
        off = (off + 7) & ~7

def unpack(path):
    img = open(path, 'rb').read()
    ctx = Ctx()
    # find top-level FVs by scanning for _FVH signatures (offset 0x28 into the header)
    pos = 0
    starts = []
    while True:
        i = img.find(b'_FVH', pos)
        if i < 0: break
        s = i - 0x28
        if s >= 0:
            fvlen = struct.unpack_from('<Q', img, s + 0x20)[0]
            if 0x48 <= fvlen <= len(img) - s:
                starts.append((s, fvlen))
        pos = i + 4
    covered_until = -1
    for s, l in starts:
        if s < covered_until: continue  # nested FVs are handled through their parent
        parse_fv(ctx, img[s:s+l])
        covered_until = s + l
    return ctx.modules

if __name__ == '__main__':
    mods = unpack(sys.argv[1])
    print(len(mods), 'modules')
    named = [m for m in mods if m[0]]
    print(len(named), 'named')
    for m in mods:
        if any(k in m[0].lower() for k in ('setup', 'msi', 'amitse', 'tse')):
            print(m[0], m[1], [ (hex(t), len(b)) for t, b in m[3]][:6])
    pickle.dump(mods, open(sys.argv[2], 'wb'))
