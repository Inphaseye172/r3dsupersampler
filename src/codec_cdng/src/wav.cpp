#include "r3dss/codec_cdng/wav.hpp"
#include "r3dss/core/paths.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace r3dss::codec::wav {

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
} // namespace

Expected<WavInfo> probe(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + toUtf8(p));
    uint8_t hdr[12];
    in.read(reinterpret_cast<char*>(hdr), 12);
    if (in.gcount() != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return makeError(ErrorCode::Format, "not a WAVE file: " + toUtf8(p));
    WavInfo info;
    bool haveFmt = false;
    uint64_t pos = 12;
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    while (pos + 8 <= fileSize) {
        in.seekg(static_cast<std::streamoff>(pos));
        uint8_t ch[8];
        in.read(reinterpret_cast<char*>(ch), 8);
        if (in.gcount() != 8) break;
        const uint32_t size = rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<uint8_t> f(size);
            in.read(reinterpret_cast<char*>(f.data()), size);
            if (size < 16) return makeError(ErrorCode::Format, "fmt chunk too small");
            uint16_t tag = rd16(f.data());
            info.audio.channels = rd16(f.data() + 2);
            info.audio.sampleRate = rd32(f.data() + 4);
            const uint16_t bits = rd16(f.data() + 14);
            if (tag == 0xFFFE && size >= 40) tag = rd16(f.data() + 24); // extensible: sub-format GUID first 2 bytes
            if (tag == 3 && bits == 32) info.audio.format = SampleFormat::Float32;
            else if (tag == 1 && bits == 16) info.audio.format = SampleFormat::PCM16;
            else if (tag == 1 && bits == 24) info.audio.format = SampleFormat::PCM24;
            else if (tag == 1 && bits == 32) info.audio.format = SampleFormat::PCM32;
            else return makeError(ErrorCode::Unsupported, "unsupported WAVE format tag/bits");
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            info.dataOffset = pos + 8;
            info.dataBytes = std::min<uint64_t>(size, fileSize - info.dataOffset);
        }
        pos += 8 + size + (size & 1u);
    }
    if (!haveFmt || !info.dataOffset) return makeError(ErrorCode::Format, "WAVE missing fmt or data chunk");
    info.audio.sampleCount = static_cast<int64_t>(info.dataBytes / std::max(1u, info.audio.frameBytes()));
    return info;
}

Expected<AudioBlock> read(const std::filesystem::path& p, const WavInfo& info, int64_t first, int64_t count) {
    AudioBlock b;
    b.info = info.audio;
    const uint32_t fb = info.audio.frameBytes();
    const int64_t total = info.audio.sampleCount;
    first = std::clamp<int64_t>(first, 0, total);
    count = std::clamp<int64_t>(count, 0, total - first);
    b.firstSample = first;
    b.sampleCount = count;
    b.bytes.resize(static_cast<size_t>(count * fb));
    if (count == 0) return b;
    std::ifstream in(p, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + toUtf8(p));
    in.seekg(static_cast<std::streamoff>(info.dataOffset + static_cast<uint64_t>(first) * fb));
    in.read(reinterpret_cast<char*>(b.bytes.data()), static_cast<std::streamsize>(b.bytes.size()));
    if (static_cast<size_t>(in.gcount()) != b.bytes.size()) return makeError(ErrorCode::Io, "short audio read");
    return b;
}

StreamWriter::~StreamWriter() {
    if (f_) std::fclose(f_);
}

Expected<void> StreamWriter::begin(const std::filesystem::path& p, const AudioInfo& info) {
    path_ = p;
    info_ = info;
    samples_ = 0;
#ifdef _WIN32
    f_ = _wfopen(p.c_str(), L"wb");
#else
    f_ = std::fopen(p.c_str(), "wb");
#endif
    if (!f_) return makeError(ErrorCode::Io, "cannot create " + toUtf8(p));
    const uint16_t bits = static_cast<uint16_t>(sampleFormatBytes(info.format) * 8);
    const bool isFloat = info.format == SampleFormat::Float32;
    const bool extensible = info.channels > 2 || bits > 16;
    std::vector<uint8_t> h;
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    riffSizePos_ = static_cast<long>(h.size());
    put32(h, 0);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, extensible ? 40u : 16u);
    put16(h, extensible ? 0xFFFE : (isFloat ? 3 : 1));
    put16(h, static_cast<uint16_t>(info.channels));
    put32(h, info.sampleRate);
    put32(h, info.sampleRate * info.frameBytes());
    put16(h, static_cast<uint16_t>(info.frameBytes()));
    put16(h, bits);
    if (extensible) {
        put16(h, 22);
        put16(h, bits);
        put32(h, 0); // channel mask: unspecified
        put16(h, isFloat ? 3 : 1);
        const uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        h.insert(h.end(), guidTail, guidTail + 14);
    }
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    dataSizePos_ = static_cast<long>(h.size());
    put32(h, 0);
    if (std::fwrite(h.data(), 1, h.size(), f_) != h.size()) return makeError(ErrorCode::Io, "write failed: " + toUtf8(p));
    return {};
}

Expected<void> StreamWriter::write(const AudioBlock& b) {
    if (!f_) return makeError(ErrorCode::Internal, "wav writer not started");
    if (b.bytes.empty()) return {};
    if (std::fwrite(b.bytes.data(), 1, b.bytes.size(), f_) != b.bytes.size())
        return makeError(ErrorCode::Io, "audio write failed: " + toUtf8(path_));
    samples_ += b.sampleCount;
    return {};
}

Expected<void> StreamWriter::finish() {
    if (!f_) return {};
    const uint64_t dataBytes = static_cast<uint64_t>(samples_) * info_.frameBytes();
    if (dataBytes & 1u) std::fputc(0, f_);
    const long end = std::ftell(f_);
    auto patch = [&](long pos, uint32_t v) {
        std::fseek(f_, pos, SEEK_SET);
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, f_);
    };
    patch(dataSizePos_, static_cast<uint32_t>(dataBytes));
    patch(riffSizePos_, static_cast<uint32_t>(end - 8));
    std::fclose(f_);
    f_ = nullptr;
    return {};
}

void StreamWriter::abort() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

} // namespace r3dss::codec::wav
