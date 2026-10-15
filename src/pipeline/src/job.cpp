#include "r3dss/pipeline/job.hpp"
#include "r3dss/core/paths.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace r3dss::pipeline {

using json = nlohmann::json;

namespace {
json optionsToJson(const JobOptions& o) {
    json j;
    j["size"] = sizePresetName(o.size);
    if (o.customSize) j["customSize"] = {{"width", o.customSize->width}, {"height", o.customSize->height}};
    j["outputFormat"] = o.outputFormat ? codec::formatName(*o.outputFormat) : "keep";
    j["profile"] = qualityProfileName(o.profile);
    j["kernel"] = kernelName(o.kernel);
    j["clipAware"] = o.clipAware;
    j["verify"] = o.verify;
    j["useGpu"] = o.useGpu;
    j["lossless"] = o.lossless;
    j["overwrite"] = o.overwrite;
    j["namingTemplate"] = o.namingTemplate;
    j["maxParallelClips"] = o.maxParallelClips;
    j["maxThreads"] = o.maxThreads;
    return j;
}

Expected<JobOptions> optionsFromJson(const json& j) {
    JobOptions o;
    if (j.contains("size")) {
        auto s = sizePresetFromName(j["size"].get<std::string>());
        if (!s) return makeError(ErrorCode::InvalidArgument, "unknown size preset");
        o.size = *s;
    }
    if (j.contains("customSize")) {
        const int64_t w = j["customSize"].value("width", int64_t{0}), h = j["customSize"].value("height", int64_t{0});
        if (const std::string err = checkCustomSize(w, h); !err.empty())
            return makeError(ErrorCode::InvalidArgument, "invalid customSize " + std::to_string(w) + "x" + std::to_string(h) + ": " + err);
        o.customSize = resample::OutputSize{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    if (o.size == SizePreset::Custom && !o.customSize)
        return makeError(ErrorCode::InvalidArgument, "size \"custom\" needs customSize {width, height}");
    if (j.contains("outputFormat")) {
        const std::string f = j["outputFormat"].get<std::string>();
        if (f != "keep") {
            auto fm = codec::formatFromName(f);
            if (!fm) return makeError(ErrorCode::InvalidArgument, "unknown output format: " + f);
            o.outputFormat = fm;
        }
    }
    if (j.contains("profile")) {
        auto p = qualityProfileFromName(j["profile"].get<std::string>());
        if (!p) return makeError(ErrorCode::InvalidArgument, "unknown profile");
        o.profile = *p;
    }
    if (j.contains("kernel")) {
        auto k = kernelFromName(j["kernel"].get<std::string>());
        if (!k) return makeError(ErrorCode::InvalidArgument, "unknown kernel");
        o.kernel = *k;
    }
    o.clipAware = j.value("clipAware", o.clipAware);
    o.verify = j.value("verify", o.verify);
    o.useGpu = j.value("useGpu", o.useGpu);
    o.lossless = j.value("lossless", o.lossless);
    o.overwrite = j.value("overwrite", o.overwrite);
    o.namingTemplate = j.value("namingTemplate", o.namingTemplate);
    o.maxParallelClips = j.value("maxParallelClips", o.maxParallelClips);
    o.maxThreads = j.value("maxThreads", o.maxThreads);
    return o;
}
} // namespace

std::string batchJobToJson(const BatchJob& b) {
    json j;
    j["name"] = b.name;
    j["outputRoot"] = toUtf8Generic(b.outputRoot);
    j["archiveMode"] = b.archiveMode;
    j["options"] = optionsToJson(b.options);
    j["clips"] = json::array();
    for (const auto& c : b.clips) {
        json cj = {{"source", toUtf8Generic(c.source)},
                   {"outputDir", toUtf8Generic(c.outputDir)},
                   {"outputName", c.outputName},
                   {"relativeDir", toUtf8Generic(c.relativeDir)}};
        if (!c.sidecars.empty()) {
            cj["sidecars"] = json::array();
            for (const auto& s : c.sidecars) cj["sidecars"].push_back(toUtf8Generic(s));
        }
        j["clips"].push_back(cj);
    }
    j["copies"] = json::array();
    for (const auto& c : b.copies) j["copies"].push_back({{"source", toUtf8Generic(c.source)}, {"dest", toUtf8Generic(c.dest)}});
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

Expected<BatchJob> batchJobFromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Format, std::string("invalid job JSON: ") + e.what());
    }
    try {
        BatchJob b;
        b.name = j.value("name", "batch");
        b.outputRoot = fromUtf8(j.value("outputRoot", ""));
        b.archiveMode = j.value("archiveMode", false);
        if (j.contains("options")) {
            R3DSS_TRY_ASSIGN(o, optionsFromJson(j["options"]));
            b.options = o;
        }
        for (const auto& c : j.value("clips", json::array())) {
            ClipJob cj;
            cj.source = normaliseSourcePath(fromUtf8(c.value("source", "")));
            cj.outputDir = fromUtf8(c.value("outputDir", ""));
            cj.outputName = c.value("outputName", "");
            cj.relativeDir = fromUtf8(c.value("relativeDir", ""));
            for (const auto& sc : c.value("sidecars", json::array())) cj.sidecars.push_back(fromUtf8(sc.get<std::string>()));
            b.clips.push_back(cj);
        }
        for (const auto& c : j.value("copies", json::array())) b.copies.push_back({fromUtf8(c.value("source", "")), fromUtf8(c.value("dest", ""))});
        return b;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Format, std::string("invalid job JSON: ") + e.what());
    }
}

Expected<BatchJob> loadBatchJob(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + toUtf8(p));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return batchJobFromJson(text);
}

Expected<void> saveBatchJob(const BatchJob& b, const std::filesystem::path& p) {
    std::ofstream out(p);
    if (!out) return makeError(ErrorCode::Io, "cannot create " + toUtf8(p));
    out << batchJobToJson(b);
    return {};
}

} // namespace r3dss::pipeline
