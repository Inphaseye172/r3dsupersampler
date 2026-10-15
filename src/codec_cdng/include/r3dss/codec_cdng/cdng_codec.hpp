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
    std::string software = "r3dss";
};

std::shared_ptr<ICodecFactory> makeCinemaDngCodecFactory(CdngWriterOptions opts = {});

// Helpers shared with tests and the verifier.
// The .dng frames of a clip folder in natural order (frame_2 before frame_10); hidden files
// (names starting with '.', e.g. AppleDouble "._*.dng") are skipped.
std::vector<std::filesystem::path> listDngFrames(const std::filesystem::path& dir);
// Natural filename order: digit runs compare by value, everything else by character.
bool naturalLess(std::string_view a, std::string_view b);
std::array<uint8_t, 8> encodeSmpteTimecode(const Timecode& tc);
Timecode decodeSmpteTimecode(const uint8_t* b, Rational fps);

} // namespace r3dss::codec
