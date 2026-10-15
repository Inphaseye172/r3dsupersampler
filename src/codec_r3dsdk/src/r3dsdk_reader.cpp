// RED R3D SDK reader (compiled only with R3DSS_HAVE_R3DSDK). Opens REDCODE RAW, Nikon R3D NE and
// NEV clips, reports clip facts and metadata, decodes developed 16-bit RGB frames and audio.
//
// The SDK never exposes the sensor mosaic (VideoPixelType has only RGB/BGR formats), so this reader
// advertises readsMosaic = false: it serves identification, verification and previews; it is not a
// RAW-to-RAW path (docs/FEASIBILITY.md).
//
// API used: InitializeSdk / Clip(LoadFrom, Status, Width, Height, VideoFrameCount, AudioChannelCount,
// AudioSampleCount, DecodeAudio, TimecodeFramerate, AbsoluteTimecode, EdgeTimecode, Metadata*,
// GetClipImageProcessingSettings, DecodeVideoFrame). Paths passed to the SDK are UTF-8 (per the
// SDK header). Metadata keys are the documented string values of the RMD_* constants.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "R3DSDK.h"
#include "R3DSDKMetadata.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "r3dss/codec_r3dsdk/r3dsdk.hpp"
#include "r3dss/codec_red/red_codec.hpp"
#include "r3dss/codec_red/red_detect.hpp"
#include "r3dss/core/logger.hpp"
#include "r3dss/core/paths.hpp"

namespace r3dss::codec::r3dsdk {

namespace {

const char* loadStatusText(R3DSDK::LoadStatus s) {
    switch (s) {
    case R3DSDK::LSClipLoaded: return "loaded";
    case R3DSDK::LSPathNotFound: return "path not found";
    case R3DSDK::LSFailedToOpenFile: return "failed to open file";
    case R3DSDK::LSNotAnR3DFile: return "not a (valid) R3D file";
    case R3DSDK::LSClipIsEmpty: return "clip has no video frames";
    case R3DSDK::LSOutOfMemory: return "out of memory";
    case R3DSDK::LSUnknownError: return "unknown error";
    case R3DSDK::LSNoClipOpen: return "no clip open";
    case R3DSDK::LSNotInitialized: return "SDK not initialised";
    default: return "unrecognised load status";
    }
}

const char* decodeStatusText(R3DSDK::DecodeStatus s) {
    switch (s) {
    case R3DSDK::DSDecodeOK: return "ok";
    case R3DSDK::DSOutputBufferInvalid: return "output buffer invalid (null, too small or not aligned)";
    case R3DSDK::DSBytesPerRowInvalid: return "bytes per row invalid";
    case R3DSDK::DSRequestOutOfRange: return "request out of range";
    case R3DSDK::DSInvalidParameter: return "invalid parameter";
    case R3DSDK::DSIsDroppedFrame: return "dropped frame";
    case R3DSDK::DSDecodeFailed: return "decode failed (corrupt frame)";
    case R3DSDK::DSOutOfMemory: return "out of memory";
    case R3DSDK::DSUnknownError: return "unknown error";
    case R3DSDK::DSNoClipOpen: return "no clip open";
    case R3DSDK::DSCannotReadFromFile: return "I/O error reading the frame";
    case R3DSDK::DSInvalidPixelType: return "invalid pixel type";
    default: return "unrecognised decode status";
    }
}

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = toUtf8(p.extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// Byte buffer with a guaranteed alignment (the SDK requires 16 bytes for video, 512 for audio).
class AlignedBuffer {
public:
    uint8_t* get(size_t bytes, size_t alignment) {
        if (storage_.size() < bytes + alignment) storage_.assign(bytes + alignment, 0);
        uintptr_t p = reinterpret_cast<uintptr_t>(storage_.data());
        p = (p + alignment - 1) / alignment * alignment;
        return reinterpret_cast<uint8_t*>(p);
    }

private:
    std::vector<uint8_t> storage_;
};

Rational rationalFromFps(double fps) {
    static const Rational common[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48000, 1001}, {48, 1},
                                      {50, 1}, {60000, 1001}, {60, 1}, {96, 1}, {100, 1}, {120000, 1001}, {120, 1}};
    for (const Rational& r : common)
        if (std::fabs(r.toDouble() - fps) < 1e-3) return r;
    return Rational{static_cast<int64_t>(std::llround(fps * 1000.0)), 1000};
}

class R3dSdkReader final : public ICodecReader {
public:
    Expected<ClipInfo> open(const std::filesystem::path& path) override {
        if (!initialize()) return makeError(ErrorCode::CodecUnavailable, statusText());
        info_ = ClipInfo{};
        // Container facts first (chunk counts, header strings); SDK facts override them below.
        if (auto c = red::fillContainerInfo(path, info_); !c)
            R3DSS_LOG_WARN("r3dsdk", "container walk failed, continuing with the SDK: " + c.error().toString());
        info_.format = Format::RedFamily;
        info_.path = path;
        clip_ = std::make_unique<R3DSDK::Clip>();
        const std::string utf8 = toUtf8(path);
        const R3DSDK::LoadStatus ls = clip_->LoadFrom(utf8.c_str());
        if (ls != R3DSDK::LSClipLoaded) {
            clip_.reset();
            return makeError(ErrorCode::Format, "RED SDK could not open " + utf8 + ": " + loadStatusText(ls));
        }
        info_.width = static_cast<uint32_t>(clip_->Width());
        info_.height = static_cast<uint32_t>(clip_->Height());
        info_.frameCount = static_cast<int64_t>(clip_->VideoFrameCount());
        info_.bits = 16; // developed RGB output depth; there is no mosaic to describe

        // Every metadata item, typed as the SDK reports it.
        std::map<std::string, std::string> asText;
        const size_t n = clip_->MetadataCount();
        for (size_t i = 0; i < n; ++i) {
            const std::string key = clip_->MetadataItemKey(i);
            const std::string text = clip_->MetadataItemAsString(i);
            asText[key] = text;
            switch (clip_->MetadataItemType(i)) {
            case R3DSDK::MetadataTypeInt: info_.meta.vendor["rmd." + key] = static_cast<int64_t>(clip_->MetadataItemAsInt(i)); break;
            case R3DSDK::MetadataTypeFloat: info_.meta.vendor["rmd." + key] = static_cast<double>(clip_->MetadataItemAsFloat(i)); break;
            default: info_.meta.vendor["rmd." + key] = text; break;
            }
        }
        auto has = [&](const char* k) { return clip_->MetadataExists(k); };
        auto num = [&](const char* k) { return has(k) ? static_cast<int64_t>(clip_->MetadataItemAsInt(k)) : int64_t(0); };
        auto flt = [&](const char* k) { return has(k) ? static_cast<double>(clip_->MetadataItemAsFloat(k)) : 0.0; };
        auto str = [&](const char* k) { return has(k) ? clip_->MetadataItemAsString(k) : std::string(); };

        // Frame rate: exact rational from numerator/denominator when present.
        if (num("framerate_numerator") > 0 && num("framerate_denominator") > 0)
            info_.fps = Rational{num("framerate_numerator"), num("framerate_denominator")};
        else
            info_.fps = rationalFromFps(clip_->VideoAudioFramerate());
        if (has("record_framerate")) info_.meta.vendor["record_framerate"] = flt("record_framerate");

        // Timecode: absolute (time-of-day) as the clip's start; edge kept alongside.
        tcFps_ = rationalFromFps(clip_->TimecodeFramerate() > 0 ? clip_->TimecodeFramerate() : clip_->VideoAudioFramerate());
        if (const char* tc = clip_->AbsoluteTimecode(0)) {
            info_.startTimecode = Timecode::parse(tc, tcFps_);
            info_.meta.vendor["absolute_timecode"] = std::string(tc);
        }
        if (const char* tc = clip_->EdgeTimecode(0)) info_.meta.vendor["edge_timecode"] = std::string(tc);

        // Audio: the SDK delivers 32-bit big-endian words (24-bit integer, MSB aligned; R3D NE may
        // record 32-bit float, flagged by audio_format).
        info_.audio = AudioInfo{};
        info_.audio.channels = static_cast<uint32_t>(clip_->AudioChannelCount());
        if (info_.audio.channels) {
            info_.audio.sampleRate = num("samplerate") > 0 ? static_cast<uint32_t>(num("samplerate")) : 48000u;
            floatAudio_ = has("audio_format") && num("audio_format") == 1;
            info_.audio.format = floatAudio_ ? SampleFormat::Float32 : SampleFormat::PCM32;
            info_.audio.sampleCount = static_cast<int64_t>(clip_->AudioSampleCount());
        }

        // Camera / exposure / colour metadata.
        const red::SourceInfo src = red::detectFromMetadata(asText, lowerExt(path));
        info_.sourceKind = src.kind;
        info_.sourceLabel = src.label;
        const bool nikon = src.kind == SourceKind::NikonR3dNe || src.kind == SourceKind::NikonNev;
        info_.meta.cameraMake = nikon ? "Nikon" : "RED";
        info_.meta.cameraModel = str("camera_model");
        info_.meta.cameraSerial = str("camera_pin");
        info_.meta.firmware = str("camera_firmware_version");
        if (has("iso")) info_.meta.iso = static_cast<uint32_t>(num("iso"));
        if (has("exposure_time") && num("exposure_time") > 0) info_.meta.exposureTimeSec = static_cast<double>(num("exposure_time")) / 1e6;
        WhiteBalance wb;
        if (has("white_balance_kelvin")) wb.kelvin = flt("white_balance_kelvin");
        if (has("white_balance_tint")) wb.tint = flt("white_balance_tint");
        if (wb.kelvin || wb.tint) info_.meta.whiteBalance = wb;
        if (has("lens_name") || has("lens_focal_length")) {
            LensInfo lens;
            lens.model = str("lens_name");
            if (has("lens_focal_length")) lens.focalLengthMm = flt("lens_focal_length");
            if (has("lens_aperture") && num("lens_aperture") > 0 && num("lens_aperture") != 0xFFFF)
                lens.apertureF = static_cast<double>(num("lens_aperture")) / 10.0;
            info_.meta.lens = lens;
        }
        info_.meta.vendor["sdk.colorVersionMin"] = static_cast<int64_t>(clip_->MinimumColorVersion());
        info_.meta.vendor["sdk.colorVersionDefault"] = static_cast<int64_t>(clip_->DefaultColorVersion());
        info_.meta.vendor["sdk.version"] = std::string(R3DSDK::GetSdkVersion());
        return info_;
    }

    const ClipInfo& info() const override { return info_; }

    Expected<void> readFrame(int64_t, BayerFrame&) override {
        return makeError(ErrorCode::Unsupported,
                         "REDCODE: the RED R3D SDK returns developed RGB only, never the sensor mosaic (docs/FEASIBILITY.md)");
    }

    Expected<void> readRgbFrame(int64_t index, RgbFrame& out, const RgbOptions& opts) override {
        if (!clip_) return makeError(ErrorCode::InvalidArgument, "no clip open");
        if (index < 0 || index >= info_.frameCount) return makeError(ErrorCode::InvalidArgument, "frame index out of range");
        R3DSDK::VideoDecodeMode mode;
        switch (opts.downscale) {
        case 1: mode = R3DSDK::DECODE_FULL_RES_PREMIUM; break;
        case 2: mode = R3DSDK::DECODE_HALF_RES_PREMIUM; break;
        case 4: mode = R3DSDK::DECODE_QUARTER_RES_GOOD; break;
        case 8: mode = R3DSDK::DECODE_EIGHT_RES_GOOD; break;
        case 16: mode = R3DSDK::DECODE_SIXTEENTH_RES_GOOD; break;
        default: return makeError(ErrorCode::InvalidArgument, "downscale must be 1, 2, 4, 8 or 16");
        }
        const size_t w = clip_->Width() / opts.downscale, h = clip_->Height() / opts.downscale;
        const size_t rowBytes = (w * 3 * sizeof(uint16_t) + 15) / 16 * 16;
        const size_t bytes = rowBytes * h;
        uint8_t* buf = video_.get(bytes, 16);

        R3DSDK::ImageProcessingSettings ips;
        R3DSDK::VideoDecodeJob job;
        job.Mode = mode;
        job.PixelType = R3DSDK::PixelType_16Bit_RGB_Interleaved;
        job.OutputBuffer = buf;
        job.BytesPerRow = rowBytes;
        job.OutputBufferSize = bytes;
        job.ImageProcessing = nullptr;
        job.HdrProcessing = nullptr;
        job.OutputFrameMetadata = nullptr;
        if (opts.primaryDevelopment) {
            clip_->GetClipImageProcessingSettings(ips);
            ips.ImagePipelineMode = R3DSDK::Primary_Development_Only; // REDWideGamutRGB / Log3G10, no look
            job.ImageProcessing = &ips;
        }
        const R3DSDK::DecodeStatus ds = clip_->DecodeVideoFrame(static_cast<size_t>(index), job);
        if (ds != R3DSDK::DSDecodeOK)
            return makeError(ErrorCode::Format, "RED SDK decode of frame " + std::to_string(index) + " failed: " + decodeStatusText(ds));
        out.width = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.index = index;
        out.colorSpace = opts.primaryDevelopment ? "REDWideGamutRGB" : "clip default";
        out.transfer = opts.primaryDevelopment ? "Log3G10" : "clip default";
        out.data.resize(w * h * 3);
        for (size_t y = 0; y < h; ++y)
            std::memcpy(out.data.data() + y * w * 3, buf + y * rowBytes, w * 3 * sizeof(uint16_t));
        out.meta = FrameMetadata{};
        if (auto tc = frameTimecode(index)) out.meta.timecode = *tc;
        return {};
    }

    Expected<AudioBlock> readAudio(int64_t first, int64_t count) override {
        AudioBlock b;
        b.info = info_.audio;
        if (!clip_ || !info_.audio.present()) return b;
        const int64_t total = info_.audio.sampleCount;
        first = std::clamp<int64_t>(first, 0, total);
        count = std::clamp<int64_t>(count, 0, total - first);
        b.firstSample = first;
        if (count == 0) return b;
        const size_t ch = info_.audio.channels;
        const size_t need = static_cast<size_t>(count) * ch * 4;
        const size_t bufSize = (need + 511) / 512 * 512;
        uint8_t* buf = audio_.get(bufSize, 512);
        size_t got = static_cast<size_t>(count);
        const R3DSDK::DecodeStatus ds = clip_->DecodeAudio(static_cast<unsigned long long>(first), &got, buf, bufSize);
        if (ds != R3DSDK::DSDecodeOK) return makeError(ErrorCode::Format, std::string("RED SDK audio decode failed: ") + decodeStatusText(ds));
        b.sampleCount = static_cast<int64_t>(got);
        b.bytes.resize(got * ch * 4);
        // Big-endian 32-bit words -> little-endian PCM32 / float32 for WAV.
        for (size_t i = 0; i < got * ch; ++i) {
            const uint8_t* s = buf + i * 4;
            uint8_t* d = b.bytes.data() + i * 4;
            d[0] = s[3];
            d[1] = s[2];
            d[2] = s[1];
            d[3] = s[0];
        }
        return b;
    }

    Expected<Timecode> frameTimecode(int64_t index) override {
        if (!clip_) return makeError(ErrorCode::InvalidArgument, "no clip open");
        const char* tc = clip_->AbsoluteTimecode(static_cast<size_t>(index));
        if (!tc) return makeError(ErrorCode::Unsupported, "clip has no absolute timecode");
        auto t = Timecode::parse(tc, tcFps_);
        if (!t) return makeError(ErrorCode::Format, std::string("unparseable timecode ") + tc);
        return *t;
    }

    void close() override { clip_.reset(); }

private:
    std::unique_ptr<R3DSDK::Clip> clip_;
    ClipInfo info_;
    Rational tcFps_{24, 1};
    bool floatAudio_ = false;
    AlignedBuffer video_, audio_;
};

} // namespace

std::unique_ptr<ICodecReader> makeSdkReader() {
    if (!initialize()) return nullptr;
    return std::make_unique<R3dSdkReader>();
}

} // namespace r3dss::codec::r3dsdk
