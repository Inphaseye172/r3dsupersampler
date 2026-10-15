// Fundamental value types shared by every module: CFA pattern, rationals, timecode,
// the Bayer frame container, audio blocks and the clip metadata passthrough model.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace r3dss {

// ---- CFA ------------------------------------------------------------------------------------
// Colour of a mosaic site: 0 = R, 1 = G, 2 = B.
enum class CfaPattern : uint8_t { RGGB = 0, BGGR = 1, GRBG = 2, GBRG = 3 };

const char* cfaName(CfaPattern p);
std::optional<CfaPattern> cfaFromName(std::string_view s);
// Colour (0/1/2) of mosaic site (x, y) for a pattern.
int cfaColorAt(CfaPattern p, uint32_t x, uint32_t y);
// The 2x2 pattern as colours in row-major order: [ (0,0), (1,0), (0,1), (1,1) ].
std::array<int, 4> cfaColors(CfaPattern p);

// Plane index p in {0,1,2,3} corresponds to offset (ox, oy) = (p & 1, p >> 1) in the mosaic.
constexpr uint32_t planeOffsetX(int p) { return static_cast<uint32_t>(p & 1); }
constexpr uint32_t planeOffsetY(int p) { return static_cast<uint32_t>(p >> 1); }

// ---- Rational -------------------------------------------------------------------------------
struct Rational {
    int64_t num = 0;
    int64_t den = 1;
    double toDouble() const { return den ? static_cast<double>(num) / static_cast<double>(den) : 0.0; }
    bool operator==(const Rational&) const = default;
};

// ---- Timecode -------------------------------------------------------------------------------
struct Timecode {
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    bool dropFrame = false;
    // Nominal frame rate used for frame<->timecode arithmetic (rounded up for 23.976 etc.)
    Rational fps{24, 1};

    std::string toString() const; // "HH:MM:SS:FF" or "HH:MM:SS;FF" for drop frame
    int64_t toFrames() const;     // counts frames since 00:00:00:00 (drop-frame aware)
    static Timecode fromFrames(int64_t frameNumber, Rational fps, bool dropFrame);
    static std::optional<Timecode> parse(std::string_view text, Rational fps);
    Timecode plusFrames(int64_t n) const { return fromFrames(toFrames() + n, fps, dropFrame); }
    bool operator==(const Timecode& o) const {
        return hours == o.hours && minutes == o.minutes && seconds == o.seconds && frames == o.frames &&
               dropFrame == o.dropFrame;
    }
};

// ---- Frames ---------------------------------------------------------------------------------
struct FrameMetadata {
    std::optional<Timecode> timecode;
    std::optional<double> exposureTimeSec;
    std::optional<uint32_t> iso;
    std::map<std::string, std::string> extra; // per-frame key/values carried through verbatim
};

// A raw Bayer mosaic. `data` is row-major, one uint16_t per site, values in [0, (1<<bits)-1].
struct BayerFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bits = 12;
    CfaPattern cfa = CfaPattern::RGGB;
    std::array<uint32_t, 4> black{0, 0, 0, 0}; // per plane, plane order (0,0),(1,0),(0,1),(1,1)
    uint32_t white = 4095;
    std::vector<uint16_t> data;
    FrameMetadata meta;
    int64_t index = 0;

    static BayerFrame create(uint32_t w, uint32_t h, uint16_t bits, CfaPattern cfa);
    size_t sampleCount() const { return static_cast<size_t>(width) * height; }
    size_t byteSize() const { return sampleCount() * sizeof(uint16_t); }
    uint16_t& at(uint32_t x, uint32_t y) { return data[static_cast<size_t>(y) * width + x]; }
    uint16_t at(uint32_t x, uint32_t y) const { return data[static_cast<size_t>(y) * width + x]; }
    uint32_t maxCode() const { return (1u << bits) - 1u; }
};

// ---- Audio ----------------------------------------------------------------------------------
enum class SampleFormat : uint8_t { PCM16, PCM24, PCM32, Float32 };
const char* sampleFormatName(SampleFormat f);
uint32_t sampleFormatBytes(SampleFormat f);

struct AudioInfo {
    uint32_t channels = 0;
    uint32_t sampleRate = 48000;
    SampleFormat format = SampleFormat::PCM24;
    int64_t sampleCount = 0; // per channel; 0 when unknown
    bool present() const { return channels > 0; }
    uint32_t frameBytes() const { return channels * sampleFormatBytes(format); }
};

// Interleaved audio, `bytes.size() == sampleCount * info.frameBytes()`.
struct AudioBlock {
    AudioInfo info;
    int64_t firstSample = 0;
    int64_t sampleCount = 0;
    std::vector<uint8_t> bytes;
};

// ---- Clip metadata passthrough model -------------------------------------------------------
struct WhiteBalance {
    std::optional<double> kelvin;
    std::optional<double> tint;
    std::array<double, 3> asShotNeutral{1.0, 1.0, 1.0}; // DNG-style camera-neutral (R,G,B)
};

struct ColorInfo {
    std::array<double, 9> colorMatrix1{1, 0, 0, 0, 1, 0, 0, 0, 1}; // XYZ -> camera, row-major
    uint16_t calibrationIlluminant1 = 21;                         // D65 in EXIF terms
    std::optional<std::array<double, 9>> colorMatrix2;
    std::optional<uint16_t> calibrationIlluminant2;
    std::string colorSpaceName;                                   // e.g. "NikonLog", "RWG"
};

struct LensInfo {
    std::string model;
    std::optional<double> focalLengthMm;
    std::optional<double> apertureF;
    std::vector<uint8_t> correctionBlob; // vendor distortion/vignetting data, opaque passthrough
};

using MetaValue = std::variant<int64_t, double, std::string>;

struct OpaqueBlob {
    std::string tag;
    std::vector<uint8_t> bytes;
};

struct ClipMetadata {
    std::string cameraMake, cameraModel, cameraSerial, firmware;
    std::optional<uint32_t> iso;
    std::optional<double> exposureTimeSec;
    std::optional<WhiteBalance> whiteBalance;
    std::optional<ColorInfo> color;
    std::optional<LensInfo> lens;
    std::map<std::string, MetaValue> vendor; // vendor key/values we understand by name
    std::vector<OpaqueBlob> opaque;          // byte blobs we carry through untouched
};

// ---- Quality / sizing -----------------------------------------------------------------------
enum class QualityProfile : uint8_t { Q1_Maximum = 1, Q3_High = 3, Q5_Normal = 5, Q7_Proxy = 7 };
const char* qualityProfileName(QualityProfile q);
std::optional<QualityProfile> qualityProfileFromName(std::string_view s);

enum class SizePreset : uint8_t { Same, SixK, UHD, FHD, Custom };
const char* sizePresetName(SizePreset s);
std::optional<SizePreset> sizePresetFromName(std::string_view s);

enum class ResampleKernel : uint8_t { Lanczos3, Area };
const char* kernelName(ResampleKernel k);
std::optional<ResampleKernel> kernelFromName(std::string_view s);

} // namespace r3dss
