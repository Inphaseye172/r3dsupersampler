#include "r3dss/pipeline/job.hpp"

namespace r3dss::pipeline {

std::string expandName(const std::string& tmpl, const std::string& stem, const JobOptions& o, codec::Format outFmt) {
    std::string out = tmpl;
    auto replaceAll = [&](const std::string& key, const std::string& val) {
        for (size_t pos = out.find(key); pos != std::string::npos; pos = out.find(key, pos + val.size()))
            out.replace(pos, key.size(), val);
    };
    replaceAll("{stem}", stem);
    replaceAll("{size}", sizePresetName(o.size));
    replaceAll("{profile}", qualityProfileName(o.profile));
    replaceAll("{format}", codec::formatName(outFmt));
    replaceAll("{kernel}", kernelName(o.kernel));
    return out;
}

} // namespace r3dss::pipeline
