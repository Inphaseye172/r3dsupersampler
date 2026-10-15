// Copies non-raw files unchanged and verifies them by SHA-256 (archive mode).
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/expected.hpp"

namespace r3dss::pipeline {

struct CopyResult {
    std::filesystem::path source, dest;
    std::string sha256;
    uint64_t bytes = 0;
    bool verified = false;
    std::string error;
};

// Copies source -> dest (creating directories), hashing on the way, then re-hashes dest.
Expected<CopyResult> copyAndVerify(const std::filesystem::path& source, const std::filesystem::path& dest,
                                   const std::shared_ptr<CancellationToken>& cancel = nullptr,
                                   const std::function<void(uint64_t done, uint64_t total)>& progress = nullptr);

// Writes a JSON manifest listing every copied / produced file with its hash.
Expected<void> writeManifest(const std::filesystem::path& path, const std::vector<CopyResult>& copies,
                             const std::vector<std::filesystem::path>& produced);

} // namespace r3dss::pipeline
