#include "core/sys/png.h"
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hx::sys {

static uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    PutBE32(out, (uint32_t)data.size());
    size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = Crc32(out.data() + crcStart, out.size() - crcStart);
    PutBE32(out, crc ^ 0xFFFFFFFFu);
}

bool SavePng(const std::wstring& path, int width, int height, const void* bgraTopDown, DWORD* err) {
    if (err) *err = ERROR_SUCCESS;
    const uint8_t* px = static_cast<const uint8_t*>(bgraTopDown);

    // Raw image stream: one filter byte (0 = none) then RGB per scanline.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * (1 + (size_t)width * 3));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        const uint8_t* row = px + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            raw.push_back(row[x * 4 + 2]);   // R (BGRA -> RGB)
            raw.push_back(row[x * 4 + 1]);   // G
            raw.push_back(row[x * 4 + 0]);   // B
        }
    }

    // Adler-32 over the raw stream (zlib trailer).
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;

    // zlib stream: 2-byte header + stored DEFLATE blocks (<=65535 each) + adler.
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size() || raw.empty()) {
        size_t n = raw.size() - off;
        if (n > 65535) n = 65535;
        bool final = (off + n >= raw.size());
        z.push_back(final ? 1 : 0);              // BFINAL in bit 0, BTYPE=00 (stored)
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));          // LEN
        uint16_t nlen = (uint16_t)~n;
        z.push_back((uint8_t)(nlen & 0xFF)); z.push_back((uint8_t)(nlen >> 8));    // NLEN
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (final) break;
    }
    PutBE32(z, adler);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    PutBE32(ihdr, (uint32_t)width); PutBE32(ihdr, (uint32_t)height);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // colour type: truecolour RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // deflate / filter / no interlace
    Chunk(out, "IHDR", ihdr);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { if (err) *err = GetLastError(); return false; }
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (!ok && err) *err = GetLastError();
    fclose(f);
    return ok;
}

}  // namespace hx::sys
