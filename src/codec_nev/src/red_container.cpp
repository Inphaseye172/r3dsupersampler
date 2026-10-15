#include "r3dss/codec_nev/red_container.hpp"
#include <fstream>

namespace r3dss::codec::red {

namespace {
uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool isTagChar(unsigned char c) { return c >= 0x20 && c < 0x7f; }
} // namespace

bool looksLikeRedContainer(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char hdr[8];
    in.read(reinterpret_cast<char*>(hdr), 8);
    if (in.gcount() != 8) return false;
    const uint32_t size = be32(hdr);
    if (size < 8) return false;
    return hdr[4] == 'R' && hdr[5] == 'E' && hdr[6] == 'D' && isTagChar(hdr[7]);
}

Expected<std::vector<Chunk>> listChunks(const std::filesystem::path& path, size_t maxChunks) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    std::vector<Chunk> out;
    uint64_t off = 0;
    while (off + 8 <= fileSize && out.size() < maxChunks) {
        in.seekg(static_cast<std::streamoff>(off));
        unsigned char hdr[8];
        in.read(reinterpret_cast<char*>(hdr), 8);
        if (in.gcount() != 8) break;
        Chunk c;
        c.offset = off;
        c.size = be32(hdr);
        c.tag.assign(reinterpret_cast<const char*>(hdr + 4), 4);
        bool tagOk = true;
        for (int i = 4; i < 8; ++i) tagOk = tagOk && isTagChar(hdr[i]);
        if (c.size < 8 || !tagOk || off + c.size > fileSize) break;
        out.push_back(c);
        off += c.size;
    }
    if (out.empty()) return makeError(ErrorCode::Format, "no RED chunks found in " + path.string());
    return out;
}

} // namespace r3dss::codec::red
