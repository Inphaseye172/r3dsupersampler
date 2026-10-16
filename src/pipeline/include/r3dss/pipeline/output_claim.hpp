// Safe output publication (docs/ARCHITECTURE.md §outputs): a clip is written into "<dest>.partial",
// claimed atomically, and renamed to "<dest>" only once it is complete and verified.
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "r3dss/core/expected.hpp"

namespace r3dss::pipeline {

// "<dest>.partial": where an output lives until it is complete.
std::filesystem::path partialPathFor(const std::filesystem::path& dest);

// Absolute, lexically normal form with symlinks resolved for the parts that exist. Case-folded on
// Windows and macOS (case-insensitive file systems) so it can be used as a comparison key.
std::string pathKey(const std::filesystem::path& p);

// True when `a` and `b` name the same location or one of them lies inside the other.
bool pathsOverlap(const std::filesystem::path& a, const std::filesystem::path& b);

// The same test on two pathKey() results.
bool pathKeysOverlap(const std::string& a, const std::string& b);

// A pathKey() followed by the keys of all its parent folders, up to and including the root.
std::vector<std::string> pathKeyAndParents(const std::string& key);

// Optionally flushes a written file to the device, then (where the OS allows) drops its cached pages
// so the next read comes from storage rather than memory. Best effort.
void flushAndDropCache(const std::filesystem::path& p, bool flush);

// Renames `from` to `to`, failing (never replacing) if `to` already exists. Works for files and
// directories on the same volume.
Expected<void> renameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to);

// Exclusive claim on one output. claim() creates "<dest>.partial" with a non-recursive mkdir (an
// exclusive create for single-file outputs), so two writers can never share a destination. A
// ".partial" folder left behind by a process that no longer runs on this machine is removed and
// claimed again; one that belongs to a live process (or another machine) fails with
// "destination in use". The destructor removes the partial output unless commit() succeeded.
class OutputClaim {
public:
    OutputClaim() = default;
    OutputClaim(OutputClaim&& o) noexcept;
    OutputClaim& operator=(OutputClaim&& o) noexcept;
    OutputClaim(const OutputClaim&) = delete;
    OutputClaim& operator=(const OutputClaim&) = delete;
    ~OutputClaim();

    static Expected<OutputClaim> claim(const std::filesystem::path& dest, bool directory);

    const std::filesystem::path& dest() const { return dest_; }
    const std::filesystem::path& partial() const { return partial_; }
    bool active() const { return active_; }

    // Publishes the partial output as `dest`. When `dest` exists and `replaceExisting` is set, the old
    // output is moved aside, the new one renamed in, and only then the old one deleted, so a failure
    // keeps the previous output. Refuses to replace anything that overlaps one of `protect` (sources).
    Expected<void> commit(bool replaceExisting, const std::vector<std::filesystem::path>& protect = {});

    // Removes the partial output (failure or cancellation). Idempotent.
    void discard();

    // Name of the ownership marker inside a claimed ".partial" folder (removed before commit).
    static const char* ownerFileName();

private:
    std::filesystem::path dest_, partial_;
    bool directory_ = true;
    bool active_ = false;
};

} // namespace r3dss::pipeline
