#include "r3dss/codec/codec.hpp"
#include <algorithm>
#include <cctype>

namespace r3dss::codec {

const char* formatName(Format f) {
    switch (f) {
    case Format::CinemaDNG: return "CinemaDNG";
    case Format::NEV: return "NEV";
    case Format::R3DNE: return "R3D NE";
    case Format::Memory: return "memory";
    case Format::Unknown: return "unknown";
    }
    return "?";
}

std::optional<Format> formatFromName(std::string_view s) {
    std::string l(s);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (l == "cinemadng" || l == "cdng" || l == "dng") return Format::CinemaDNG;
    if (l == "nev" || l == "n-raw" || l == "nraw") return Format::NEV;
    if (l == "r3dne" || l == "r3d ne" || l == "r3d-ne" || l == "r3d") return Format::R3DNE;
    if (l == "memory") return Format::Memory;
    if (l == "keep" || l == "same") return std::nullopt;
    return std::nullopt;
}

const char* formatExtension(Format f) {
    switch (f) {
    case Format::CinemaDNG: return ".dng";
    case Format::NEV: return ".NEV";
    case Format::R3DNE: return ".R3D";
    case Format::Memory: return ".mem";
    case Format::Unknown: return "";
    }
    return "";
}

CodecRegistry& CodecRegistry::instance() {
    static CodecRegistry r;
    return r;
}

void CodecRegistry::registerFactory(std::shared_ptr<ICodecFactory> f) {
    for (auto& e : factories_)
        if (e->format() == f->format()) {
            e = std::move(f);
            return;
        }
    factories_.push_back(std::move(f));
}

std::shared_ptr<ICodecFactory> CodecRegistry::byFormat(Format f) const {
    for (const auto& e : factories_)
        if (e->format() == f) return e;
    return nullptr;
}

std::shared_ptr<ICodecFactory> CodecRegistry::probe(const std::filesystem::path& path) const {
    for (const auto& e : factories_)
        if (e->probe(path)) return e;
    return nullptr;
}

std::vector<std::shared_ptr<ICodecFactory>> CodecRegistry::all() const { return factories_; }

} // namespace r3dss::codec
