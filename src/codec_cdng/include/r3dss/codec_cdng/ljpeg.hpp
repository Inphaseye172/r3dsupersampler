// Lossless JPEG (ITU-T T.81 process 14, SOF3) decoder and encoder for 2..16-bit samples,
// as used by DNG Compression = 7. The encoder writes 2 interleaved components with predictor 1
// so each sample is predicted from the same-colour neighbour two sites to the left.
#pragma once
#include <cstdint>
#include <vector>
#include "r3dss/core/expected.hpp"

namespace r3dss::codec::ljpeg {

struct Image {
    uint32_t width = 0;     // samples per row (components * columns)
    uint32_t height = 0;
    uint16_t precision = 16;
    uint16_t components = 1;
    std::vector<uint16_t> samples; // row-major, interleaved components
};

Expected<Image> decode(const uint8_t* data, size_t len);

struct EncodeParams {
    uint16_t precision = 16;
    uint16_t components = 2; // 1..4; width must be divisible by components
    uint8_t predictor = 1;   // 1..7
};
// `samples` is row-major with `width` samples per row and `height` rows.
Expected<std::vector<uint8_t>> encode(const uint16_t* samples, uint32_t width, uint32_t height, const EncodeParams& p);

} // namespace r3dss::codec::ljpeg
