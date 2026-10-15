// CinemaDNG reference codec: a clip is a directory of numbered .dng frames plus an optional
// .wav sidecar. Reads uncompressed (8..16-bit packed) and lossless-JPEG DNGs; writes 16-bit
// containers (uncompressed or lossless JPEG) with CFA, levels, colour, ISO, timecode, frame
// rate and lens tags, and streams audio to <clip>.wav.
#pragma once
#include <memory>
#include "r3dss/codec/codec.hpp"

namespace r3dss::codec {

struct CdngWriterOptions {
    bool losslessJpeg = true;   // Compression 7 (true) or 1 (false)
    uint32_t tileRows = 256;    // rows per tile when compressed
    std::string software = "Supersampler";
};

std::shared_ptr<ICodecFactory> makeCinemaDngCodecFactory(CdngWriterOptions opts = {});

// Helpers shared with tests and the verifier.
std::vector<std::filesystem::path> listDngFrames(const std::filesystem::path& dir);
std::array<uint8_t, 8> encodeSmpteTimecode(const Timecode& tc);
Timecode decodeSmpteTimecode(const uint8_t* b, Rational fps);

} // namespace r3dss::codec
