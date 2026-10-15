// Writes a synthetic RED-style container (docs/CODESTREAM.md §1 layout) for unit tests.
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace r3dss::testfx {

struct ChunkSpec {
    std::string tag; // 3 chars
    uint8_t version;
    uint64_t size;   // including the 8-byte header
    std::string embeddedStrings; // NUL-separated printable strings placed after the header
};

// Writes chunks at 4096-aligned offsets with zero gaps and returns the expected offsets.
inline std::vector<uint64_t> writeRedContainer(const std::filesystem::path& p, const std::vector<ChunkSpec>& chunks) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::vector<uint64_t> offsets;
    uint64_t off = 0;
    for (const auto& c : chunks) {
        offsets.push_back(off);
        std::vector<char> buf(static_cast<size_t>(c.size), 0);
        const uint32_t sz = static_cast<uint32_t>(c.size);
        buf[0] = static_cast<char>(sz >> 24);
        buf[1] = static_cast<char>(sz >> 16);
        buf[2] = static_cast<char>(sz >> 8);
        buf[3] = static_cast<char>(sz);
        buf[4] = c.tag[0];
        buf[5] = c.tag[1];
        buf[6] = c.tag[2];
        buf[7] = static_cast<char>(c.version);
        size_t pos = 16;
        for (char ch : c.embeddedStrings)
            if (pos < buf.size()) buf[pos++] = ch;
        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        const uint64_t padded = (off + c.size + 4095) / 4096 * 4096;
        std::vector<char> zeros(static_cast<size_t>(padded - off - c.size), 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        off = padded;
    }
    return offsets;
}

} // namespace r3dss::testfx
