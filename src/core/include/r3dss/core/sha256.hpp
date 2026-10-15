// SHA-256 (FIPS 180-4) used for archive-copy verification and output manifests.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include "r3dss/core/expected.hpp"

namespace r3dss {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> finish();
    static std::string hex(const std::array<uint8_t, 32>& digest);
    static std::string hashBytes(const void* data, size_t len);
    static Expected<std::string> hashFile(const std::filesystem::path& p);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

} // namespace r3dss
