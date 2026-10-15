// Chunk-level walker for RED's container, used by .R3D (REDCODE RAW), Nikon R3D NE and NEV.
//
// Layout verified on a RED KOMODO 6K clip (docs/CODESTREAM.md §1):
//   * every chunk starts with a 4-byte big-endian size (including the 8-byte header), then
//     3 ASCII tag bytes and 1 version byte, e.g. "RED" v50, "RDI" v1, "RDA" v1, "REO" v66;
//   * chunks start on 4096-byte boundaries, the gap after a chunk is zero-filled;
//   * the first chunk is the "RED" file header, the last is the "REO" trailer (a header copy).
// Nothing below chunk level is decoded here; the frame payloads are REDCODE RAW, readable only
// through the RED R3D SDK (src/codec_r3dsdk).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "r3dss/core/expected.hpp"

namespace r3dss::codec::red {

inline constexpr uint64_t kAlignment = 4096;

struct Chunk {
    uint64_t offset = 0; // file offset of the 8-byte chunk header
    uint64_t size = 0;   // chunk size including the header (not padded)
    std::string tag;     // exactly 3 characters
    uint8_t version = 0;
};

struct ChunkStats {
    size_t count = 0;
    uint64_t totalBytes = 0, minSize = UINT64_MAX, maxSize = 0;
    std::set<uint8_t> versions;
};

// True when the first 8 bytes are a chunk header whose tag is "RED".
bool looksLikeRedContainer(const std::filesystem::path& path);

// Lists every top-level chunk. Stops at EOF or the first unparseable header.
Expected<std::vector<Chunk>> listChunks(const std::filesystem::path& path, size_t maxChunks = 1u << 22);

std::map<std::string, ChunkStats> chunkHistogram(const std::vector<Chunk>& chunks);

// Printable ASCII runs (>= minLen chars) inside the "RED" header chunk, in file order. Observed
// contents include the camera model ("KOMODO 6K"), sensor ("KOMODO S35"), serial, firmware,
// clip file name, format name ("6K 17:9") and the edge / absolute start timecodes.
Expected<std::vector<std::string>> readHeaderStrings(const std::filesystem::path& path, size_t minLen = 4);

// Known tags: RED header, RDI video frame, RDA audio, RDX, RII, RIA, RIX indexes, REO trailer.
inline constexpr const char* kTagHeader = "RED";
inline constexpr const char* kTagVideo = "RDI";
inline constexpr const char* kTagAudio = "RDA";
inline constexpr const char* kTagTrailer = "REO";

} // namespace r3dss::codec::red
