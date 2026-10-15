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
    QualityProfile profile = QualityProfile::Q5_Normal; // only for codecs with rate control (not CinemaDNG)
    ResampleKernel kernel = ResampleKernel::Lanczos3;
    bool clipAware = true;
    bool verify = true;
    bool useGpu = true;
    bool lossless = false;         // only for codecs with rate control; CinemaDNG output is always lossless
    bool overwrite = false;        // replace existing outputs (only after the new output is complete)
    std::string namingTemplate = "{stem}_{size}_{profile}";
    unsigned maxParallelClips = 0; // 0 = automatic
    unsigned maxThreads = 0;       // 0 = all cores
};

struct ClipJob {
    ClipJob() = default;
    ClipJob(std::filesystem::path src, std::filesystem::path outDir, std::string outName = {},
            std::filesystem::path relDir = {})
        : source(std::move(src)), outputDir(std::move(outDir)), outputName(std::move(outName)), relativeDir(std::move(relDir)) {}

    std::filesystem::path source;      // file or CinemaDNG directory
    std::filesystem::path outputDir;   // directory the output is placed in
    std::string outputName;            // without extension (filled by naming if empty)
    std::filesystem::path relativeDir; // for preserved folder structure (informational)
    // Archive mode: non-raw files inside the clip folder (paths relative to it, e.g. "C001_meta.xml" or
    // "proxy/C001.mov"), copied and verified into the output clip folder.
    std::vector<std::filesystem::path> sidecars;
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
// {size}: preset name ("UHD", "2K", "same", ...), or "<W>x<H>" (aligned as written) for a custom size.
// {profile}: "lossless" when the output codec has no rate control (CinemaDNG), else Q1/Q3/Q5/Q7.
std::string expandName(const std::string& tmpl, const std::string& stem, const JobOptions& o, codec::Format outFmt);

// Removes trailing separators and trailing "." components ("C001/" -> "C001", "C001\\" -> "C001" on
// Windows) so the last component names the clip. "mem://" paths are returned unchanged.
std::filesystem::path normaliseSourcePath(const std::filesystem::path& p);

// The {stem} of a clip: the folder name for a CinemaDNG folder or for a .dng frame inside one,
// otherwise the file name without extension.
std::string clipStem(const std::filesystem::path& source);

} // namespace r3dss::pipeline
