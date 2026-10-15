#include "r3dss/codec_cdng/tiff.hpp"
#include <cstring>
#include <fstream>

namespace r3dss::codec::tiff {

uint32_t typeSize(Type t) {
    switch (t) {
    case Type::Byte: case Type::Ascii: case Type::SByte: case Type::Undefined: return 1;
    case Type::Short: case Type::SShort: return 2;
    case Type::Long: case Type::SLong: case Type::Float: return 4;
    case Type::Rational: case Type::SRational: case Type::Double: return 8;
    }
    return 1;
}

namespace {
uint16_t rd16(const uint8_t* p, bool big) { return big ? uint16_t((p[0] << 8) | p[1]) : uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p, bool big) {
    return big ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]
               : uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void wr16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void wr32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i)); }
} // namespace

std::optional<uint32_t> Entry::u32(uint32_t i) const {
    if (i >= count) return std::nullopt;
    const uint8_t* p = data.data() + static_cast<size_t>(i) * typeSize(type);
    switch (type) {
    case Type::Byte: case Type::Undefined: case Type::SByte: return *p;
    case Type::Short: case Type::SShort: return rd16(p, bigEndian);
    case Type::Long: case Type::SLong: return rd32(p, bigEndian);
    default: return std::nullopt;
    }
}

std::optional<int32_t> Entry::s32(uint32_t i) const {
    if (i >= count) return std::nullopt;
    const uint8_t* p = data.data() + static_cast<size_t>(i) * typeSize(type);
    switch (type) {
    case Type::SByte: return static_cast<int8_t>(*p);
    case Type::SShort: return static_cast<int16_t>(rd16(p, bigEndian));
    case Type::SLong: return static_cast<int32_t>(rd32(p, bigEndian));
    default: {
        auto u = u32(i);
        return u ? std::optional<int32_t>(static_cast<int32_t>(*u)) : std::nullopt;
    }
    }
}

std::optional<double> Entry::real(uint32_t i) const {
    if (i >= count) return std::nullopt;
    const uint8_t* p = data.data() + static_cast<size_t>(i) * typeSize(type);
    switch (type) {
    case Type::Rational: {
        const uint32_t n = rd32(p, bigEndian), d = rd32(p + 4, bigEndian);
        return d ? static_cast<double>(n) / d : 0.0;
    }
    case Type::SRational: {
        const int32_t n = static_cast<int32_t>(rd32(p, bigEndian)), d = static_cast<int32_t>(rd32(p + 4, bigEndian));
        return d ? static_cast<double>(n) / d : 0.0;
    }
    case Type::Float: {
        uint32_t u = rd32(p, bigEndian);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    case Type::Double: {
        uint8_t tmp[8];
        for (int k = 0; k < 8; ++k) tmp[k] = bigEndian ? p[7 - k] : p[k];
        double d;
        std::memcpy(&d, tmp, 8);
        return d;
    }
    default: {
        auto s = s32(i);
        return s ? std::optional<double>(*s) : std::nullopt;
    }
    }
}

std::string Entry::ascii() const {
    std::string s(data.begin(), data.end());
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

Expected<void> Reader::open(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + p.string());
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0);
    buf_.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(buf_.data()), size);
    if (buf_.size() < 8) return makeError(ErrorCode::Format, "file too small for TIFF: " + p.string());
    if (buf_[0] == 'I' && buf_[1] == 'I') big_ = false;
    else if (buf_[0] == 'M' && buf_[1] == 'M') big_ = true;
    else return makeError(ErrorCode::Format, "not a TIFF/DNG file: " + p.string());
    if (u16At(2) != 42) return makeError(ErrorCode::Format, "bad TIFF magic: " + p.string());
    first_ = u32At(4);
    return {};
}

uint16_t Reader::u16At(uint64_t off) const { return off + 2 <= buf_.size() ? rd16(buf_.data() + off, big_) : 0; }
uint32_t Reader::u32At(uint64_t off) const { return off + 4 <= buf_.size() ? rd32(buf_.data() + off, big_) : 0; }

Expected<Ifd> Reader::readIfd(uint64_t off) const {
    if (off + 2 > buf_.size()) return makeError(ErrorCode::Format, "IFD offset out of range");
    const uint16_t n = u16At(off);
    if (off + 2 + 12ull * n + 4 > buf_.size()) return makeError(ErrorCode::Format, "IFD truncated");
    Ifd ifd;
    for (uint16_t i = 0; i < n; ++i) {
        const uint64_t e = off + 2 + 12ull * i;
        Entry en;
        en.tag = u16At(e);
        en.type = static_cast<Type>(u16At(e + 2));
        en.count = u32At(e + 4);
        en.bigEndian = big_;
        const uint64_t bytes = static_cast<uint64_t>(en.count) * typeSize(en.type);
        if (bytes > 64ull * 1024 * 1024) return makeError(ErrorCode::Format, "IFD entry too large");
        const uint64_t src = bytes <= 4 ? e + 8 : u32At(e + 8);
        if (src + bytes > buf_.size()) return makeError(ErrorCode::Format, "IFD entry data out of range");
        en.data.assign(buf_.begin() + static_cast<std::ptrdiff_t>(src), buf_.begin() + static_cast<std::ptrdiff_t>(src + bytes));
        ifd.entries[en.tag] = std::move(en);
    }
    ifd.nextIfdOffset = u32At(off + 2 + 12ull * n);
    return ifd;
}

Writer::Writer() {
    buf_ = {'I', 'I', 42, 0, 0, 0, 0, 0}; // IFD0 offset patched by setFirstIfd
}

void Writer::align() { while (buf_.size() & 1u) buf_.push_back(0); }

uint64_t Writer::appendData(const std::vector<uint8_t>& bytes) { return appendData(bytes.data(), bytes.size()); }
uint64_t Writer::appendData(const uint8_t* bytes, size_t len) {
    align();
    const uint64_t off = buf_.size();
    buf_.insert(buf_.end(), bytes, bytes + len);
    return off;
}

void Writer::IfdBuilder::setU8(uint16_t tag, const std::vector<uint8_t>& v, Type t) {
    Entry e; e.tag = tag; e.type = t; e.count = static_cast<uint32_t>(v.size()); e.data = v; entries[tag] = e;
}
void Writer::IfdBuilder::setU16(uint16_t tag, const std::vector<uint16_t>& v) {
    Entry e; e.tag = tag; e.type = Type::Short; e.count = static_cast<uint32_t>(v.size());
    for (uint16_t x : v) wr16(e.data, x);
    entries[tag] = e;
}
void Writer::IfdBuilder::setU32(uint16_t tag, const std::vector<uint32_t>& v) {
    Entry e; e.tag = tag; e.type = Type::Long; e.count = static_cast<uint32_t>(v.size());
    for (uint32_t x : v) wr32(e.data, x);
    entries[tag] = e;
}
void Writer::IfdBuilder::setAscii(uint16_t tag, const std::string& s) {
    Entry e; e.tag = tag; e.type = Type::Ascii; e.data.assign(s.begin(), s.end()); e.data.push_back(0);
    e.count = static_cast<uint32_t>(e.data.size());
    entries[tag] = e;
}
void Writer::IfdBuilder::setRational(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>>& v) {
    Entry e; e.tag = tag; e.type = Type::Rational; e.count = static_cast<uint32_t>(v.size());
    for (auto& [n, d] : v) { wr32(e.data, n); wr32(e.data, d); }
    entries[tag] = e;
}
void Writer::IfdBuilder::setSRational(uint16_t tag, const std::vector<std::pair<int32_t, int32_t>>& v) {
    Entry e; e.tag = tag; e.type = Type::SRational; e.count = static_cast<uint32_t>(v.size());
    for (auto& [n, d] : v) { wr32(e.data, static_cast<uint32_t>(n)); wr32(e.data, static_cast<uint32_t>(d)); }
    entries[tag] = e;
}

uint64_t Writer::writeIfd(const IfdBuilder& ifd) {
    // Out-of-line data first, then the IFD itself.
    std::map<uint16_t, uint32_t> dataOff;
    for (const auto& [t, e] : ifd.entries)
        if (e.data.size() > 4) dataOff[t] = static_cast<uint32_t>(appendData(e.data));
    align();
    const uint64_t off = buf_.size();
    wr16(buf_, static_cast<uint16_t>(ifd.entries.size()));
    for (const auto& [t, e] : ifd.entries) {
        wr16(buf_, e.tag);
        wr16(buf_, static_cast<uint16_t>(e.type));
        wr32(buf_, e.count);
        if (e.data.size() > 4) wr32(buf_, dataOff[t]);
        else {
            for (size_t i = 0; i < 4; ++i) buf_.push_back(i < e.data.size() ? e.data[i] : 0);
        }
    }
    wr32(buf_, 0); // next IFD
    return off;
}

void Writer::setFirstIfd(uint64_t off) { put32(buf_, 4, static_cast<uint32_t>(off)); }

Expected<void> Writer::save(const std::filesystem::path& p) const {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::Io, "cannot create " + p.string());
    out.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(buf_.size()));
    if (!out) return makeError(ErrorCode::Io, "write failed: " + p.string());
    return {};
}

} // namespace r3dss::codec::tiff
