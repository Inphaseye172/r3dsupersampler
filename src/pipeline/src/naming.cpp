#include "r3dss/pipeline/job.hpp"
#include <cstdint>
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

std::string checkCustomSize(int64_t width, int64_t height) {
    for (const int64_t v : {width, height}) {
        const int64_t aligned = (v + 1) / 2 * 2; // odd values round up to even, as the size planner does
        if (v < 0 || aligned < 16) return "width and height must be at least 16";
        if (aligned > 65535) return "width and height must be at most 65534";
    }
    return {};
}

std::string parseSizeOption(const std::string& text, JobOptions& o) {
    if (auto sp = sizePresetFromName(text); sp && *sp != SizePreset::Custom) {
        o.size = *sp;
        o.customSize.reset();
        return {};
    }
    const std::string usage = "bad --size '" + text + "': use 6K, UHD, 2K, FHD, same or <width>x<height> (e.g. 1920x1080)";
    const size_t x = text.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= text.size()) return usage;
    uint32_t dims[2] = {0, 0};
    const std::string parts[2] = {text.substr(0, x), text.substr(x + 1)};
    for (int i = 0; i < 2; ++i) {
        for (char c : parts[i])
            if (c < '0' || c > '9') return usage;
        if (parts[i].size() > 6) return "bad --size '" + text + "': width and height must be at most 65534";
        uint64_t v = 0;
        for (char c : parts[i]) v = v * 10 + static_cast<uint64_t>(c - '0');
        dims[i] = static_cast<uint32_t>(v);
    }
    if (const std::string err = checkCustomSize(dims[0], dims[1]); !err.empty()) return "bad --size '" + text + "': " + err;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{dims[0], dims[1]};
    return {};
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
