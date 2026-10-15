// Chunk-level walker for the RED-style container used by NEV (N-RAW) and R3D NE files.
//
// What is implemented here is strictly the publicly documented top level: every chunk is a
// 32-bit big-endian size (including the 8-byte header) followed by a 4-character tag, and the
// first chunk of a file starts with a "RED" tag ("RED1" / "RED2"). Nothing below chunk level is
// decoded: payloads are exposed as opaque byte ranges. Parsing the header fields, the frame
// index and the per-frame metadata (task 10 in docs/IMPLEMENTATION_PLAN.md) needs sample
// files and a legal review first, and is deliberately not guessed here.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "r3dss/core/expected.hpp"

namespace r3dss::codec::red {

struct Chunk {
    uint64_t offset = 0; // file offset of the 8-byte chunk header
    uint64_t size = 0;   // total chunk size including the header
    std::string tag;     // 4 characters
};

// True when the first 8 bytes look like a RED chunk header ("RED1", "RED2", ...).
bool looksLikeRedContainer(const std::filesystem::path& path);

// Lists top-level chunks (bounded by `maxChunks`). Stops at the first malformed chunk.
Expected<std::vector<Chunk>> listChunks(const std::filesystem::path& path, size_t maxChunks = 1u << 20);

// FourCC strings observed in the wild for the essence codec id (unconfirmed, see FEASIBILITY.md).
inline constexpr const char* kFourccNraw = "iNRAW";
inline constexpr const char* kFourccR3dNe = "iNR3D";

} // namespace r3dss::codec::red
