#include "r3dss/codec_red/red_container.hpp"
#include <fstream>

namespace r3dss::codec::red {

namespace {
uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool isTagChar(unsigned char c) { return c >= 0x20 && c < 0x7f; }
bool parseHeader(const unsigned char* h, Chunk& c) {
    c.size = be32(h);
    if (c.size < 8) return false;
    for (int i = 4; i < 7; ++i)
        if (!isTagChar(h[i])) return false;
    c.tag.assign(reinterpret_cast<const char*>(h + 4), 3);
    c.version = h[7];
    return true;
}
uint64_t alignUp(uint64_t v) { return (v + kAlignment - 1) / kAlignment * kAlignment; }
} // namespace

bool looksLikeRedContainer(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char hdr[8];
    in.read(reinterpret_cast<char*>(hdr), 8);
    if (in.gcount() != 8) return false;
    Chunk c;
    return parseHeader(hdr, c) && c.tag == kTagHeader;
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
        bool allZero = true;
        for (unsigned char b : hdr) allZero = allZero && b == 0;
        if (allZero) { // padding: skip to the next 4 KiB boundary
            const uint64_t next = alignUp(off + 1);
            if (next <= off) break;
            off = next;
            continue;
        }
        Chunk c;
        c.offset = off;
        if (!parseHeader(hdr, c) || off + c.size > fileSize) break;
        out.push_back(c);
        off = alignUp(off + c.size);
    }
    if (out.empty() || out.front().tag != kTagHeader)
        return makeError(ErrorCode::Format, "no RED container header in " + path.string());
    return out;
}

std::map<std::string, ChunkStats> chunkHistogram(const std::vector<Chunk>& chunks) {
    std::map<std::string, ChunkStats> h;
    for (const auto& c : chunks) {
        ChunkStats& s = h[c.tag];
        ++s.count;
        s.totalBytes += c.size;
        s.minSize = std::min(s.minSize, c.size);
        s.maxSize = std::max(s.maxSize, c.size);
        s.versions.insert(c.version);
    }
    return h;
}

Expected<std::vector<std::string>> readHeaderStrings(const std::filesystem::path& path, size_t minLen) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + path.string());
    unsigned char hdr[8];
    in.read(reinterpret_cast<char*>(hdr), 8);
    Chunk c;
    if (in.gcount() != 8 || !parseHeader(hdr, c) || c.tag != kTagHeader)
        return makeError(ErrorCode::Format, "no RED container header in " + path.string());
    std::vector<unsigned char> body(static_cast<size_t>(std::min<uint64_t>(c.size, 1u << 20)));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()));
    std::vector<std::string> out;
    std::string run;
    for (unsigned char b : body) {
        if (isTagChar(b)) {
            run.push_back(static_cast<char>(b));
        } else {
            if (run.size() >= minLen) out.push_back(run);
            run.clear();
        }
    }
    if (run.size() >= minLen) out.push_back(run);
    return out;
}

} // namespace r3dss::codec::red
