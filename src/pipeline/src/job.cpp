#include "r3dss/pipeline/job.hpp"
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
    if (j.contains("customSize")) o.customSize = resample::OutputSize{j["customSize"]["width"], j["customSize"]["height"]};
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
    j["outputRoot"] = b.outputRoot.generic_string();
    j["archiveMode"] = b.archiveMode;
    j["options"] = optionsToJson(b.options);
    j["clips"] = json::array();
    for (const auto& c : b.clips)
        j["clips"].push_back({{"source", c.source.generic_string()},
                              {"outputDir", c.outputDir.generic_string()},
                              {"outputName", c.outputName},
                              {"relativeDir", c.relativeDir.generic_string()}});
    j["copies"] = json::array();
    for (const auto& c : b.copies) j["copies"].push_back({{"source", c.source.generic_string()}, {"dest", c.dest.generic_string()}});
    return j.dump(2);
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
        b.outputRoot = j.value("outputRoot", "");
        b.archiveMode = j.value("archiveMode", false);
        if (j.contains("options")) {
            R3DSS_TRY_ASSIGN(o, optionsFromJson(j["options"]));
            b.options = o;
        }
        for (const auto& c : j.value("clips", json::array())) {
            ClipJob cj;
            cj.source = c.value("source", "");
            cj.outputDir = c.value("outputDir", "");
            cj.outputName = c.value("outputName", "");
            cj.relativeDir = c.value("relativeDir", "");
            b.clips.push_back(cj);
        }
        for (const auto& c : j.value("copies", json::array())) b.copies.push_back({c.value("source", ""), c.value("dest", "")});
        return b;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Format, std::string("invalid job JSON: ") + e.what());
    }
}

Expected<BatchJob> loadBatchJob(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + p.string());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return batchJobFromJson(text);
}

Expected<void> saveBatchJob(const BatchJob& b, const std::filesystem::path& p) {
    std::ofstream out(p);
    if (!out) return makeError(ErrorCode::Io, "cannot create " + p.string());
    out << batchJobToJson(b);
    return {};
}

} // namespace r3dss::pipeline
