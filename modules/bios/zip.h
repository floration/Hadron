// modules/bios/zip.h — a minimal read-only zip reader with its own inflate (RFC 1951), so a snapshot
// .zip can be opened without extracting it first. Hard rule 3: no zlib, no third-party code.
//
// Supports stored (0) and deflated (8) entries, the UTF-8 name flag (bit 11, else CP437), and checks
// each entry's CRC-32. Not supported (reported as errors, never guessed): zip64, encryption, other
// compression methods, multi-disk archives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hx::bios {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0);

// Raw deflate stream (no zlib header). Appends to *out and stops with an error as soon as the output
// would exceed maxOut bytes, so a corrupt or hostile stream can't grow without limit. Returns false
// with a reason in *err.
bool Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>* out, size_t maxOut, std::wstring* err);

struct ZipEntry {
    std::wstring name;        // '/'-separated (a '\' written by some archivers is normalised to '/')
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc = 0, csize = 0, usize = 0, localOffset = 0;
};

class ZipReader {
public:
    // Takes the whole archive in memory (snapshots are a few MB). Parses the central directory.
    bool Open(std::vector<uint8_t> data, std::wstring* err);
    const std::vector<ZipEntry>& Entries() const { return entries_; }
    const ZipEntry* Find(const std::wstring& name) const;   // exact, case-insensitive ASCII
    // Refuses (before allocating) an entry whose declared size exceeds cap; inflate stops at that size.
    bool Extract(const ZipEntry& e, std::vector<uint8_t>* out, size_t cap, std::wstring* err) const;

private:
    std::vector<uint8_t> data_;
    std::vector<ZipEntry> entries_;
};

}  // namespace hx::bios
