// modules/bios/zip.cpp — see zip.h. Own CRC-32, own inflate (canonical Huffman decode by code length
// counts), own central-directory parser.
#include "core/sys/win_compat.h"
#include "modules/bios/zip.h"
#include <cwctype>

namespace hx::bios {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc) {
    struct Table {
        uint32_t t[256];
        Table() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };
    static const Table tab;   // function-local static: initialised once, thread-safe (C++11)
    const uint32_t* table = tab.t;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- inflate ------------------------------------------------------------------------------------
namespace {

struct Bits {
    const uint8_t* p; size_t n; size_t pos = 0; uint32_t buf = 0; int cnt = 0; bool over = false;
    Bits(const uint8_t* in, size_t len) : p(in), n(len) {}
    uint32_t Get(int need) {   // need <= 24
        while (cnt < need) {
            if (pos >= n) { over = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        uint32_t v = buf & ((1u << need) - 1);
        buf >>= need; cnt -= need;
        return v;
    }
    void Align() { buf = 0; cnt = 0; }
};

// Canonical Huffman table: count[len] codes of each length, symbols sorted by code.
struct Huff {
    uint16_t count[16] = {};
    uint16_t sym[320] = {};
    // Returns 0 ok, <0 over-subscribed, >0 incomplete (allowed for single-code distance trees).
    int Build(const uint8_t* lens, int n) {
        for (int i = 0; i < 16; i++) count[i] = 0;
        for (int i = 0; i < n; i++) count[lens[i]]++;
        if (count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return -1;
        }
        uint16_t offs[16]; offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + count[len]);
        for (int i = 0; i < n; i++) if (lens[i]) sym[offs[lens[i]]++] = (uint16_t)i;
        return left;
    }
    int Decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)b.Get(1);
            if (b.over) return -1;
            int c = count[len];
            if (code - c < first) return sym[index + (code - first)];
            index += c; first += c;
            first <<= 1; code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

bool Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>* out, size_t maxOut, std::wstring* err) {
    Bits b(in, n);
    const size_t start = out->size();
    const size_t limit = start + maxOut;   // hard stop, whatever the stream or the header claims
    auto fail = [&](const wchar_t* why) { if (err) *err = std::wstring(L"deflate: ") + why; return false; };
    bool last = false;
    while (!last) {
        last = b.Get(1) != 0;
        uint32_t type = b.Get(2);
        if (b.over) return fail(L"truncated block header");
        if (type == 0) {
            b.Align();
            if (b.pos + 4 > n) return fail(L"truncated stored block");
            uint32_t len = in[b.pos] | (in[b.pos + 1] << 8);
            uint32_t nlen = in[b.pos + 2] | (in[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return fail(L"stored block length check failed");
            if (b.pos + len > n) return fail(L"truncated stored data");
            if (out->size() + len > limit) return fail(L"output would exceed its size limit");
            out->insert(out->end(), in + b.pos, in + b.pos + len);
            b.pos += len;
            continue;
        }
        if (type == 3) return fail(L"invalid block type");
        Huff lit, dist;
        if (type == 1) {
            uint8_t lens[288 + 30];
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            lit.Build(lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            dist.Build(lens, 30);
        } else {
            int nlen = (int)b.Get(5) + 257, ndist = (int)b.Get(5) + 1, ncode = (int)b.Get(4) + 4;
            if (b.over) return fail(L"truncated dynamic header");
            if (nlen > 286 || ndist > 30) return fail(L"bad code counts");
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lens[320] = {};
            for (int i = 0; i < ncode; i++) lens[order[i]] = (uint8_t)b.Get(3);
            if (b.over) return fail(L"truncated code lengths");
            Huff cl;
            if (cl.Build(lens, 19) != 0) return fail(L"bad code-length code");
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = cl.Decode(b);
                if (sym < 0) return fail(L"bad code-length symbol");
                if (sym < 16) { lens[idx++] = (uint8_t)sym; continue; }
                uint8_t val = 0; int rep;
                if (sym == 16) {
                    if (idx == 0) return fail(L"repeat with no previous length");
                    val = lens[idx - 1]; rep = 3 + (int)b.Get(2);
                } else if (sym == 17) rep = 3 + (int)b.Get(3);
                else rep = 11 + (int)b.Get(7);
                if (b.over || idx + rep > nlen + ndist) return fail(L"code lengths overflow");
                while (rep--) lens[idx++] = val;
            }
            if (lens[256] == 0) return fail(L"no end-of-block code");
            int r = lit.Build(lens, nlen);
            if (r < 0 || (r > 0 && nlen - lit.count[0] != 1)) return fail(L"bad literal/length code");
            r = dist.Build(lens + nlen, ndist);
            if (r < 0 || (r > 0 && ndist - dist.count[0] != 1)) return fail(L"bad distance code");
        }
        for (;;) {
            int sym = lit.Decode(b);
            if (sym < 0) return fail(L"bad literal/length symbol");
            if (sym < 256) {
                if (out->size() + 1 > limit) return fail(L"output would exceed its size limit");
                out->push_back((uint8_t)sym);
                continue;
            }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) return fail(L"bad length symbol");
            size_t len = kLenBase[sym] + b.Get(kLenExtra[sym]);
            int ds = dist.Decode(b);
            if (ds < 0 || ds >= 30) return fail(L"bad distance symbol");
            size_t d = kDistBase[ds] + b.Get(kDistExtra[ds]);
            if (b.over) return fail(L"truncated data");
            if (d > out->size() - start) return fail(L"distance before start of output");
            if (out->size() + len > limit) return fail(L"output would exceed its size limit");
            size_t from = out->size() - d;
            for (size_t k = 0; k < len; k++) out->push_back((*out)[from + k]);
        }
    }
    return true;
}

// ---- zip ----------------------------------------------------------------------------------------
namespace {
uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

std::wstring DecodeName(const uint8_t* p, size_t n, bool utf8) {
    if (n == 0) return L"";
    UINT cp = utf8 ? CP_UTF8 : 437;   // APPNOTE: no bit 11 means IBM code page 437
    int w = MultiByteToWideChar(cp, 0, (const char*)p, (int)n, nullptr, 0);
    std::wstring s(w > 0 ? (size_t)w : 0, L'\0');
    if (w > 0) MultiByteToWideChar(cp, 0, (const char*)p, (int)n, &s[0], w);
    for (auto& c : s) if (c == L'\\') c = L'/';
    return s;
}
}  // namespace

bool ZipReader::Open(std::vector<uint8_t> data, std::wstring* err) {
    data_ = std::move(data);
    entries_.clear();
    const size_t n = data_.size();
    if (n < 22) { if (err) *err = L"not a zip file (too small)"; return false; }
    // End of central directory: scan back over at most a 64 KB comment.
    size_t eocd = SIZE_MAX;
    size_t lo = n > 22 + 0xFFFF ? n - 22 - 0xFFFF : 0;
    for (size_t i = n - 22 + 1; i-- > lo;) {
        if (Rd32(&data_[i]) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) { if (err) *err = L"not a zip file (no end-of-directory record)"; return false; }
    const uint8_t* e = &data_[eocd];
    if (Rd16(e + 4) != 0 || Rd16(e + 6) != 0) { if (err) *err = L"multi-disk zip files are not supported"; return false; }
    uint32_t count = Rd16(e + 10), cdSize = Rd32(e + 12), cdOff = Rd32(e + 16);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) { if (err) *err = L"zip64 archives are not supported"; return false; }
    if ((size_t)cdOff + cdSize > n) { if (err) *err = L"corrupt zip (central directory out of range)"; return false; }
    size_t p = cdOff;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > n || Rd32(&data_[p]) != 0x02014b50) { if (err) *err = L"corrupt zip (bad directory entry)"; return false; }
        const uint8_t* c = &data_[p];
        ZipEntry z;
        z.flags = Rd16(c + 8);
        z.method = Rd16(c + 10);
        z.crc = Rd32(c + 16);
        z.csize = Rd32(c + 20);
        z.usize = Rd32(c + 24);
        uint16_t nl = Rd16(c + 28), xl = Rd16(c + 30), cl = Rd16(c + 32);
        z.localOffset = Rd32(c + 42);
        if (p + 46 + nl + xl + cl > n) { if (err) *err = L"corrupt zip (entry name out of range)"; return false; }
        z.name = DecodeName(c + 46, nl, (z.flags & 0x0800) != 0);
        entries_.push_back(std::move(z));
        p += 46 + nl + xl + cl;
    }
    return true;
}

const ZipEntry* ZipReader::Find(const std::wstring& name) const {
    for (auto& e : entries_) {
        if (e.name.size() != name.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < name.size() && eq; i++) eq = towlower(e.name[i]) == towlower(name[i]);
        if (eq) return &e;
    }
    return nullptr;
}

bool ZipReader::Extract(const ZipEntry& z, std::vector<uint8_t>* out, size_t cap, std::wstring* err) const {
    out->clear();
    auto fail = [&](const std::wstring& why) { if (err) *err = z.name + L": " + why; return false; };
    if (z.flags & 0x0001) return fail(L"encrypted entries are not supported");
    if (z.csize == 0xFFFFFFFFu || z.usize == 0xFFFFFFFFu) return fail(L"zip64 entries are not supported");
    // Size checks before any allocation: a hostile directory can claim anything.
    if (z.usize > cap) return fail(L"entry is larger than " + std::to_wstring(cap >> 20) + L" MB (" +
                                   std::to_wstring(z.usize) + L" bytes claimed)");
    if (z.method == 8 && z.usize == 0 && z.csize > 16) return fail(L"empty entry with compressed data");
    size_t p = z.localOffset;
    if (p + 30 > data_.size() || Rd32(&data_[p]) != 0x04034b50) return fail(L"bad local header");
    size_t body = p + 30 + Rd16(&data_[p + 26]) + Rd16(&data_[p + 28]);
    if (body + z.csize > data_.size()) return fail(L"data out of range");
    const uint8_t* src = data_.data() + body;
    if (z.method == 0) {
        if (z.csize != z.usize) return fail(L"stored entry size mismatch");
        out->assign(src, src + z.csize);
    } else if (z.method == 8) {
        out->reserve(z.usize);
        std::wstring why;
        if (!Inflate(src, z.csize, out, z.usize, &why)) return fail(why);
        if (out->size() != z.usize) return fail(L"inflated size differs from the directory");
    } else {
        return fail(L"compression method " + std::to_wstring(z.method) + L" is not supported");
    }
    if (Crc32(out->data(), out->size()) != z.crc) return fail(L"CRC-32 mismatch (corrupt archive)");
    return true;
}

}  // namespace hx::bios
