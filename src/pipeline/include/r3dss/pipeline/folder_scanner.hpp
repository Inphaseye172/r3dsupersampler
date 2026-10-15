// Finds raw clips (NEV / R3D NE files, CinemaDNG directories) and non-raw files under a root.
#pragma once
#include <filesystem>
#include <vector>
#include "r3dss/pipeline/job.hpp"

namespace r3dss::pipeline {

struct ScanOptions {
    bool recursive = true;
    bool preserveStructure = true;
    bool archiveMode = false; // also list non-raw files for copying
};

struct ScanResult {
    std::vector<ClipJob> clips;
    std::vector<CopyJob> copies;
    std::vector<std::filesystem::path> skipped; // raw-looking files no codec recognised
};

// `outputRoot` is where outputs (and archive copies) go, mirroring the structure under `root`.
Expected<ScanResult> scanFolder(const std::filesystem::path& root, const std::filesystem::path& outputRoot,
                                const ScanOptions& opts);

} // namespace r3dss::pipeline
