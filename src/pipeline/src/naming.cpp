#include "r3dss/pipeline/job.hpp"
#include <limits>
#include "r3dss/core/paths.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {
std::string sizeToken(const JobOptions& o, uint32_t alignment) {
    if (o.size != SizePreset::Custom || !o.customSize) return sizePresetName(o.size);
    // The name states the size actually written: the custom size after the codec's alignment.
    constexpr uint32_t big = std::numeric_limits<uint32_t>::max();
    auto planned = resample::planOutputSize(big, big, SizePreset::Custom, o.customSize, alignment);
    const resample::OutputSize s = planned ? *planned : *o.customSize;
    return std::to_string(s.width) + "x" + std::to_string(s.height);
}
} // namespace

std::string expandName(const std::string& tmpl, const std::string& stem, const JobOptions& o, codec::Format outFmt) {
    const auto factory = codec::CodecRegistry::instance().byFormat(outFmt);
    const codec::Capabilities caps = factory ? factory->caps() : codec::Capabilities{};
    const std::string profile = factory && !caps.rateControl ? "lossless" : qualityProfileName(o.profile);
    std::string out = tmpl;
    auto replaceAll = [&](const std::string& key, const std::string& val) {
        for (size_t pos = out.find(key); pos != std::string::npos; pos = out.find(key, pos + val.size()))
            out.replace(pos, key.size(), val);
    };
    replaceAll("{stem}", stem);
    replaceAll("{size}", sizeToken(o, factory ? caps.dimAlignment : 2));
    replaceAll("{profile}", profile);
    replaceAll("{format}", codec::formatName(outFmt));
    replaceAll("{kernel}", kernelName(o.kernel));
    return out;
}

fs::path normaliseSourcePath(const fs::path& p) {
    if (toUtf8Generic(p).rfind("mem://", 0) == 0) return p;
    fs::path q = p;
    while (q.has_relative_path() && (!q.has_filename() || q.filename() == ".")) {
        fs::path parent = q.parent_path();
        if (parent.empty() || parent == q) break;
        q = parent;
    }
    return q;
}

std::string clipStem(const fs::path& source) {
    fs::path p = normaliseSourcePath(source);
    if (toUtf8Generic(p).rfind("mem://", 0) == 0) return toUtf8(p.filename());
    std::error_code ec;
    auto lowerExt = [](const fs::path& f) {
        std::string e = toUtf8(f.extension());
        for (char& c : e)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return e;
    };
    const bool dngFrame = !fs::is_directory(p, ec) && lowerExt(p) == ".dng";
    if (dngFrame) p = normaliseSourcePath(p.parent_path().empty() ? fs::path(".") : p.parent_path());
    const fs::path name = p.filename();
    if (name.empty() || name == "." || name == "..") {
        // "." or "..": name the clip after the folder it really is.
        const fs::path abs = normaliseSourcePath(fs::absolute(p, ec).lexically_normal());
        return toUtf8(abs.filename());
    }
    if (dngFrame || fs::is_directory(p, ec)) return toUtf8(name);
    return toUtf8(p.stem());
}

} // namespace r3dss::pipeline
