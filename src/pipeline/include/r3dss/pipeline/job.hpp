// Job model shared by the CLI and the GUI, serialisable to JSON (docs/ARCHITECTURE.md §jobs).
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/expected.hpp"
#include "r3dss/core/types.hpp"
#include "r3dss/resample/size_planner.hpp"

namespace r3dss::pipeline {

struct JobOptions {
    SizePreset size = SizePreset::UHD;
    std::optional<resample::OutputSize> customSize;
    std::optional<codec::Format> outputFormat; // nullopt = keep the source container
    QualityProfile profile = QualityProfile::Q5_Normal;
    ResampleKernel kernel = ResampleKernel::Lanczos3;
    bool clipAware = true;
    bool verify = true;
    bool useGpu = true;
    bool lossless = false;
    bool overwrite = false;        // replace existing outputs instead of skipping
    std::string namingTemplate = "{stem}_{size}_{profile}";
    unsigned maxParallelClips = 0; // 0 = automatic
    unsigned maxThreads = 0;       // 0 = all cores
};

struct ClipJob {
    std::filesystem::path source;      // file or CinemaDNG directory
    std::filesystem::path outputDir;   // directory the output is placed in
    std::string outputName;            // without extension (filled by naming if empty)
    std::filesystem::path relativeDir; // for preserved folder structure (informational)
};

struct CopyJob {
    std::filesystem::path source;
    std::filesystem::path dest;
};

struct BatchJob {
    std::string name;
    std::filesystem::path outputRoot;
    JobOptions options;
    std::vector<ClipJob> clips;
    std::vector<CopyJob> copies; // archive-mode copies of non-raw files
    bool archiveMode = false;
};

Expected<BatchJob> loadBatchJob(const std::filesystem::path& jsonPath);
Expected<void> saveBatchJob(const BatchJob& job, const std::filesystem::path& jsonPath);
std::string batchJobToJson(const BatchJob& job);
Expected<BatchJob> batchJobFromJson(const std::string& text);

// Expands the naming template: {stem} {size} {profile} {format} {kernel}.
std::string expandName(const std::string& tmpl, const std::string& stem, const JobOptions& o, codec::Format outFmt);

} // namespace r3dss::pipeline
