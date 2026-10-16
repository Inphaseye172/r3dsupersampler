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
    std::string sha256;       // hash of the archived file (== destSha256 when verified)
    std::string sourceSha256; // hash of a separate, second read of the source
    std::string destSha256;   // hash of the file read back from the destination
    uint64_t bytes = 0;
    bool verified = false;
    bool alreadyPresent = false; // destination already held an identical file; nothing was written
    std::string error;
};

// Copies source -> dest (creating directories) through a temporary file. Verification hashes the
// data as it is copied, then re-reads the source and the written copy independently; all three must
// match before the copy is renamed into place. An existing destination is never replaced: if it is
// identical to the source the result is `alreadyPresent`, otherwise the copy fails.
Expected<CopyResult> copyAndVerify(const std::filesystem::path& source, const std::filesystem::path& dest,
                                   const std::shared_ptr<CancellationToken>& cancel = nullptr,
                                   const std::function<void(uint64_t done, uint64_t total)>& progress = nullptr);

// Writes a JSON manifest listing every archived copy (with source and destination hashes) and every
// produced output file with its hash. Entries whose file no longer exists are left out.
// `complete` = false marks a manifest written for a cancelled batch.
Expected<void> writeManifest(const std::filesystem::path& path, const std::vector<CopyResult>& copies,
                             const std::vector<std::filesystem::path>& produced, bool complete = true);

} // namespace r3dss::pipeline
