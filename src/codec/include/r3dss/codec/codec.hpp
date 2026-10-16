// The abstract reader / writer interface every codec module implements.
// See docs/CODEC_INTERFACE.md. Modules never expose SDK types through this header.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "r3dss/core/expected.hpp"
#include "r3dss/core/types.hpp"

namespace r3dss::codec {

// RedFamily = any clip in RED's chunked container: REDCODE RAW (KOMODO, V-RAPTOR, ...), Nikon R3D NE
// and Nikon NEV. They are read through the RED R3D SDK and can never be written.
enum class Format : uint8_t { CinemaDNG, RedFamily, Memory, Unknown };
const char* formatName(Format f);
std::optional<Format> formatFromName(std::string_view s);
const char* formatExtension(Format f); // ".dng" / ".R3D"

struct Capabilities {
    bool read = false;
    bool readsMosaic = false; // readFrame() returns the undebayered sensor mosaic (required for RAW-to-RAW)
    bool readsRgb = false;    // readRgbFrame() returns a developed RGB image
    bool write = false;
    bool cuda = false;        // codec can decode/encode on CUDA itself
    bool rateControl = false; // writeFrame honours FrameBudget
    bool lossless = false;    // writer can be mathematically lossless
    uint32_t dimAlignment = 2; // output width/height must be multiples of this
    std::vector<uint16_t> bitDepths{12, 14, 16};
};

// What camera / codec produced a RedFamily clip, detected from content (never from the extension).
enum class SourceKind : uint8_t { Unknown, CinemaDng, RedcodeRaw, NikonR3dNe, NikonNev };
const char* sourceKindName(SourceKind k);

// A rectangle of a stored raw image in raw pixel coordinates (DNG ActiveArea order).
struct PixelRect {
    uint32_t top = 0, left = 0, bottom = 0, right = 0;
    uint32_t width() const { return right - left; }
    uint32_t height() const { return bottom - top; }
    bool operator==(const PixelRect&) const = default;
};

struct ClipInfo {
    Format format = Format::Unknown;
    SourceKind sourceKind = SourceKind::Unknown;
    std::string sourceLabel; // e.g. "REDCODE RAW (KOMODO 6K)"; shown as the GUI Format column
    std::filesystem::path path;
    uint32_t width = 0, height = 0; // size of the frames readFrame() returns (even)
    // Where those frames come from in the stored raw image (CinemaDNG; 0 when a reader does not
    // report it). declaredActiveArea is the source's ActiveArea (the whole image when absent);
    // activeArea is the rectangle actually returned: LibRaw's visible area (origin rounded up to
    // even) with an odd trailing row/column dropped, so activeArea.width() == width.
    uint32_t rawWidth = 0, rawHeight = 0;
    PixelRect declaredActiveArea, activeArea;
    uint16_t bits = 12;
    CfaPattern cfa = CfaPattern::RGGB;
    std::array<uint32_t, 4> black{0, 0, 0, 0};
    uint32_t white = 4095;
    Rational fps{24, 1};
    int64_t frameCount = 0;
    AudioInfo audio;
    std::optional<Timecode> startTimecode;
    ClipMetadata meta;
    // CinemaDNG: every audio sidecar of the clip, the main one first (copied byte for byte when the
    // output is CinemaDNG too), and anything the reader could not carry over faithfully.
    std::vector<std::filesystem::path> audioSidecars;
    std::vector<std::string> warnings;
    uint64_t sourceBytes = 0; // total bytes on disk (for bitrate reporting)
    double durationSeconds() const { return fps.toDouble() > 0 ? static_cast<double>(frameCount) / fps.toDouble() : 0.0; }
};

class ICodecReader {
public:
    virtual ~ICodecReader() = default;
    virtual Expected<ClipInfo> open(const std::filesystem::path& path) = 0;
    virtual const ClipInfo& info() const = 0;
    // Reads a frame into `out` (reused across calls to avoid allocations).
    virtual Expected<void> readFrame(int64_t index, BayerFrame& out) = 0;
    // Reads `count` samples (per channel) starting at `firstSample`; fewer at end of clip.
    virtual Expected<AudioBlock> readAudio(int64_t firstSample, int64_t count) = 0;
    virtual Expected<Timecode> frameTimecode(int64_t index) = 0;
    virtual void close() = 0;

    // Developed RGB decode, for decoders whose SDK never exposes the sensor mosaic.
    struct RgbOptions {
        uint32_t downscale = 1;          // 1 = full resolution, 2 = half, 4, 8, 16
        bool primaryDevelopment = true;  // true: scene-referred (e.g. Log3G10 / REDWideGamutRGB), no look
    };
    virtual Expected<void> readRgbFrame(int64_t index, RgbFrame& out, const RgbOptions& opts) {
        (void)index; (void)out; (void)opts;
        return makeError(ErrorCode::Unsupported, "this codec does not provide developed RGB frames");
    }
};

struct WriterConfig {
    std::filesystem::path dest; // file (MOV/MXF) or directory (CinemaDNG)
    std::string clipName;       // base name for frame files / clip header
    uint32_t width = 0, height = 0;
    uint16_t bits = 12;
    CfaPattern cfa = CfaPattern::RGGB;
    std::array<uint32_t, 4> black{0, 0, 0, 0};
    uint32_t white = 4095;
    Rational fps{24, 1};
    QualityProfile profile = QualityProfile::Q5_Normal;
    AudioInfo audio;
    std::optional<Timecode> startTimecode;
    ClipMetadata meta;
    bool lossless = false; // request lossless where the codec supports it
    // Size of the source frames (for rescaling crop / aspect / opcode geometry); 0 = unknown.
    uint32_t sourceWidth = 0, sourceHeight = 0;
    // Audio files to copy byte for byte into the output instead of a streamed track (main one first).
    std::vector<std::filesystem::path> copyAudioFiles;
};

struct FrameBudget {
    uint64_t bits = 0; // target size of this frame in bits (0 = codec default)
    bool lossless = false;
};

struct EncodedStats {
    uint64_t bytes = 0;
    double bitsPerPixel = 0.0;
};

struct WriteSummary {
    int64_t framesWritten = 0;
    int64_t audioSamplesWritten = 0;
    uint64_t totalBytes = 0;
    std::vector<std::filesystem::path> files; // every file produced (for verification/manifest)
};

class ICodecWriter {
public:
    virtual ~ICodecWriter() = default;
    virtual Expected<void> begin(const WriterConfig& cfg) = 0;
    virtual Expected<EncodedStats> writeFrame(const BayerFrame& frame, FrameBudget budget) = 0;
    virtual Expected<void> writeAudio(const AudioBlock& block) = 0;
    virtual Expected<WriteSummary> finish() = 0;
    virtual void abort() = 0; // remove partial output
};

enum class ModuleState : uint8_t { Available, MissingSdk, MissingLicense, Stub };
struct ModuleStatus {
    ModuleState state = ModuleState::Stub;
    std::string text; // human readable, shown in the GUI
    bool usable() const { return state == ModuleState::Available; }
};

class ICodecFactory {
public:
    virtual ~ICodecFactory() = default;
    virtual Format format() const = 0;
    virtual std::string displayName() const = 0;
    virtual Capabilities caps() const = 0;
    virtual ModuleStatus status() const = 0;
    // True if `path` looks like this format (extension + magic bytes); never decodes frames.
    virtual bool probe(const std::filesystem::path& path) const = 0;
    virtual std::unique_ptr<ICodecReader> makeReader() const = 0;
    virtual std::unique_ptr<ICodecWriter> makeWriter() const = 0;
};

class CodecRegistry {
public:
    static CodecRegistry& instance();
    void registerFactory(std::shared_ptr<ICodecFactory> f);
    std::shared_ptr<ICodecFactory> byFormat(Format f) const;
    // First factory whose probe() accepts the path, or nullptr.
    std::shared_ptr<ICodecFactory> probe(const std::filesystem::path& path) const;
    std::vector<std::shared_ptr<ICodecFactory>> all() const;

private:
    std::vector<std::shared_ptr<ICodecFactory>> factories_;
};

// Registers the built-in codecs (CinemaDNG, RED family via the R3D SDK, memory). Idempotent.
void registerBuiltinCodecs();

} // namespace r3dss::codec
