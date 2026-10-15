#include "r3dss/codec_cdng/cdng_codec.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "r3dss/codec_cdng/ljpeg.hpp"
#include "r3dss/codec_cdng/tiff.hpp"
#include "r3dss/codec_cdng/wav.hpp"
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/logger.hpp"

namespace r3dss::codec {

using namespace tiff;
namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

constexpr uint16_t kPhotometricCfa = 32803;

std::optional<CfaPattern> cfaFromColors(const std::array<int, 4>& c) {
    for (int p = 0; p < 4; ++p) {
        const auto pat = static_cast<CfaPattern>(p);
        if (cfaColors(pat) == c) return pat;
    }
    return std::nullopt;
}

uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
int fromBcd(uint8_t b) { return ((b >> 4) & 0xF) * 10 + (b & 0xF); }

std::pair<uint32_t, uint32_t> toRational(double v, uint32_t den = 10000) {
    return {static_cast<uint32_t>(std::llround(v * den)), den};
}
std::pair<int32_t, int32_t> toSRational(double v, int32_t den = 10000) {
    return {static_cast<int32_t>(std::llround(v * den)), den};
}

} // namespace

std::array<uint8_t, 8> encodeSmpteTimecode(const Timecode& tc) {
    std::array<uint8_t, 8> b{};
    b[0] = static_cast<uint8_t>(toBcd(tc.frames) | (tc.dropFrame ? 0x40 : 0));
    b[1] = toBcd(tc.seconds);
    b[2] = toBcd(tc.minutes);
    b[3] = toBcd(tc.hours);
    return b;
}

Timecode decodeSmpteTimecode(const uint8_t* b, Rational fps) {
    Timecode tc;
    tc.fps = fps;
    tc.frames = fromBcd(b[0] & 0x3F);
    tc.dropFrame = (b[0] & 0x40) != 0;
    tc.seconds = fromBcd(b[1] & 0x7F);
    tc.minutes = fromBcd(b[2] & 0x7F);
    tc.hours = fromBcd(b[3] & 0x3F);
    return tc;
}

std::vector<fs::path> listDngFrames(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && lowerExt(e.path()) == ".dng") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct FrameGeometry {
    uint32_t width = 0, height = 0, bits = 16, compression = 1;
    std::vector<uint64_t> blockOffsets, blockBytes;
    uint32_t blockW = 0, blockH = 0; // tile size, or (width, rowsPerStrip) for strips
    bool tiled = false;
};

Expected<Ifd> findRawIfd(const Reader& r) {
    R3DSS_TRY_ASSIGN(ifd0, r.readIfd(r.firstIfdOffset()));
    auto isRaw = [](const Ifd& i) {
        return i.u32(tag::Photometric).value_or(0) == kPhotometricCfa && i.u32(tag::NewSubFileType).value_or(0) == 0;
    };
    if (isRaw(ifd0)) return ifd0;
    if (const Entry* sub = ifd0.find(tag::SubIFDs)) {
        for (uint32_t i = 0; i < sub->count; ++i) {
            auto off = sub->u32(i);
            if (!off) continue;
            auto s = r.readIfd(*off);
            if (s && isRaw(*s)) return *s;
        }
    }
    if (ifd0.u32(tag::Photometric).value_or(0) == kPhotometricCfa) return ifd0;
    return makeError(ErrorCode::Format, "no CFA raw image IFD found");
}

Expected<FrameGeometry> geometryOf(const Ifd& ifd) {
    FrameGeometry g;
    g.width = ifd.u32(tag::ImageWidth).value_or(0);
    g.height = ifd.u32(tag::ImageLength).value_or(0);
    g.bits = ifd.u32(tag::BitsPerSample).value_or(16);
    g.compression = ifd.u32(tag::Compression).value_or(1);
    if (ifd.u32(tag::SamplesPerPixel).value_or(1) != 1) return makeError(ErrorCode::Unsupported, "SamplesPerPixel != 1");
    if (!g.width || !g.height) return makeError(ErrorCode::Format, "missing image dimensions");
    if (const Entry* to = ifd.find(tag::TileOffsets)) {
        g.tiled = true;
        g.blockW = ifd.u32(tag::TileWidth).value_or(0);
        g.blockH = ifd.u32(tag::TileLength).value_or(0);
        const Entry* tb = ifd.find(tag::TileByteCounts);
        if (!tb || tb->count != to->count || !g.blockW || !g.blockH) return makeError(ErrorCode::Format, "bad tile tags");
        for (uint32_t i = 0; i < to->count; ++i) { g.blockOffsets.push_back(*to->u32(i)); g.blockBytes.push_back(*tb->u32(i)); }
    } else if (const Entry* so = ifd.find(tag::StripOffsets)) {
        g.blockW = g.width;
        g.blockH = ifd.u32(tag::RowsPerStrip).value_or(g.height);
        if (g.blockH == 0 || g.blockH > g.height) g.blockH = g.height;
        const Entry* sb = ifd.find(tag::StripByteCounts);
        if (!sb || sb->count != so->count) return makeError(ErrorCode::Format, "bad strip tags");
        for (uint32_t i = 0; i < so->count; ++i) { g.blockOffsets.push_back(*so->u32(i)); g.blockBytes.push_back(*sb->u32(i)); }
    } else {
        return makeError(ErrorCode::Format, "no strips or tiles");
    }
    return g;
}

// Unpacks `count` big-endian MSB-first packed samples of `bits` each (TIFF uncompressed < 16).
void unpackBits(const uint8_t* src, size_t srcLen, uint32_t bits, uint32_t count, bool bigEndian16, uint16_t* dst) {
    if (bits == 16) {
        for (uint32_t i = 0; i < count && 2 * i + 1 < srcLen; ++i)
            dst[i] = bigEndian16 ? static_cast<uint16_t>((src[2 * i] << 8) | src[2 * i + 1])
                                 : static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
        return;
    }
    if (bits == 8) {
        for (uint32_t i = 0; i < count && i < srcLen; ++i) dst[i] = src[i];
        return;
    }
    uint64_t acc = 0;
    int nbits = 0;
    size_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        while (nbits < static_cast<int>(bits) && pos < srcLen) { acc = (acc << 8) | src[pos++]; nbits += 8; }
        if (nbits < static_cast<int>(bits)) { dst[i] = 0; continue; }
        dst[i] = static_cast<uint16_t>((acc >> (nbits - bits)) & ((1u << bits) - 1));
        nbits -= static_cast<int>(bits);
        acc &= (nbits ? (1ull << nbits) - 1 : 0);
    }
}

Expected<void> decodeFrameData(const Reader& r, const Ifd& ifd, const FrameGeometry& g, BayerFrame& out) {
    (void)ifd; // geometry already extracted; kept for symmetry with the strip/tile readers
    out.data.resize(static_cast<size_t>(g.width) * g.height);
    const uint32_t blocksX = g.tiled ? (g.width + g.blockW - 1) / g.blockW : 1;
    const auto& bytes = r.bytes();
    for (size_t b = 0; b < g.blockOffsets.size(); ++b) {
        const uint64_t off = g.blockOffsets[b], len = g.blockBytes[b];
        if (off + len > bytes.size()) return makeError(ErrorCode::Format, "block data out of range");
        const uint32_t bx = static_cast<uint32_t>(b % blocksX), by = static_cast<uint32_t>(b / blocksX);
        const uint32_t x0 = bx * g.blockW, y0 = by * g.blockH;
        if (y0 >= g.height) break;
        const uint32_t rows = std::min(g.blockH, g.height - y0);
        const uint32_t cols = std::min(g.blockW, g.width - x0);
        if (g.compression == 7) {
            R3DSS_TRY_ASSIGN(img, ljpeg::decode(bytes.data() + off, static_cast<size_t>(len)));
            // The LJPEG stream covers blockW x blockH samples (components folded into width).
            if (static_cast<uint64_t>(img.width) * img.height < static_cast<uint64_t>(g.blockW) * g.blockH)
                return makeError(ErrorCode::Format, "LJPEG tile smaller than tile geometry");
            // Streams may be laid out as blockW*blockH samples with any component folding; treat
            // the decoded samples as a flat blockW x blockH raster.
            for (uint32_t y = 0; y < rows; ++y)
                std::memcpy(out.data.data() + static_cast<size_t>(y0 + y) * g.width + x0,
                            img.samples.data() + static_cast<size_t>(y) * g.blockW, cols * sizeof(uint16_t));
        } else if (g.compression == 1) {
            std::vector<uint16_t> tmp(static_cast<size_t>(g.blockW) * rows);
            const uint64_t need = (static_cast<uint64_t>(g.blockW) * rows * g.bits + 7) / 8;
            unpackBits(bytes.data() + off, static_cast<size_t>(std::min<uint64_t>(len, need)), g.bits, g.blockW * rows,
                       r.bigEndian(), tmp.data());
            for (uint32_t y = 0; y < rows; ++y)
                std::memcpy(out.data.data() + static_cast<size_t>(y0 + y) * g.width + x0,
                            tmp.data() + static_cast<size_t>(y) * g.blockW, cols * sizeof(uint16_t));
        } else {
            return makeError(ErrorCode::Unsupported, "DNG compression " + std::to_string(g.compression) + " not supported");
        }
    }
    return {};
}

Expected<void> readClipInfoFromIfd(const Reader& r, const Ifd& ifd, ClipInfo& ci, Rational* fpsOut) {
    R3DSS_TRY_ASSIGN(g, geometryOf(ifd));
    ci.width = g.width;
    ci.height = g.height;
    if (ci.width & 1u || ci.height & 1u) return makeError(ErrorCode::Unsupported, "odd DNG dimensions");
    // Bit depth: prefer WhiteLevel to infer the real sensor depth, fall back to BitsPerSample.
    ci.white = ifd.u32(tag::WhiteLevel).value_or((1u << std::min<uint32_t>(16, g.bits)) - 1);
    uint16_t bits = 16;
    while (bits > 8 && (1u << (bits - 1)) > ci.white) --bits;
    ci.bits = bits;
    if (const Entry* bl = ifd.find(tag::BlackLevel)) {
        const uint32_t n = bl->count;
        for (uint32_t i = 0; i < 4; ++i) {
            auto v = bl->real(n == 4 ? i : 0);
            ci.black[i] = static_cast<uint32_t>(std::lround(v.value_or(0.0)));
        }
    }
    // CFA
    if (const Entry* cp = ifd.find(tag::CFAPattern)) {
        if (cp->count == 4) {
            std::array<int, 4> cols{};
            for (uint32_t i = 0; i < 4; ++i) cols[i] = static_cast<int>(cp->u32(i).value_or(1));
            auto pat = cfaFromColors(cols);
            if (!pat) return makeError(ErrorCode::Unsupported, "non-Bayer CFA pattern");
            ci.cfa = *pat;
        } else {
            return makeError(ErrorCode::Unsupported, "CFA repeat pattern is not 2x2");
        }
    }
    ci.meta.cameraMake = ifd.ascii(tag::Make);
    ci.meta.cameraModel = ifd.ascii(tag::Model);
    if (ci.meta.cameraModel.empty()) ci.meta.cameraModel = ifd.ascii(tag::UniqueCameraModel);
    if (const Entry* cm = ifd.find(tag::ColorMatrix1); cm && cm->count == 9) {
        ColorInfo c;
        for (uint32_t i = 0; i < 9; ++i) c.colorMatrix1[i] = cm->real(i).value_or(0.0);
        c.calibrationIlluminant1 = static_cast<uint16_t>(ifd.u32(tag::CalibrationIlluminant1).value_or(21));
        if (const Entry* cm2 = ifd.find(tag::ColorMatrix2); cm2 && cm2->count == 9) {
            std::array<double, 9> m{};
            for (uint32_t i = 0; i < 9; ++i) m[i] = cm2->real(i).value_or(0.0);
            c.colorMatrix2 = m;
            c.calibrationIlluminant2 = static_cast<uint16_t>(ifd.u32(tag::CalibrationIlluminant2).value_or(17));
        }
        ci.meta.color = c;
    }
    if (const Entry* wb = ifd.find(tag::AsShotNeutral); wb && wb->count == 3) {
        WhiteBalance w;
        for (uint32_t i = 0; i < 3; ++i) w.asShotNeutral[i] = wb->real(i).value_or(1.0);
        ci.meta.whiteBalance = w;
    }
    if (const Entry* pd = ifd.find(tag::DNGPrivateData); pd && pd->count)
        ci.meta.opaque.push_back({"DNGPrivateData", pd->data});
    if (const Entry* fr = ifd.find(tag::FrameRate); fr && fr->count >= 1) {
        const uint8_t* p = fr->data.data();
        auto rd = [&](const uint8_t* q) {
            return fr->bigEndian ? (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3]
                                 : uint32_t(q[0]) | (uint32_t(q[1]) << 8) | (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24);
        };
        const int32_t n = static_cast<int32_t>(rd(p)), d = static_cast<int32_t>(rd(p + 4));
        if (n > 0 && d > 0) *fpsOut = Rational{n, d};
    }
    if (const Entry* tc = ifd.find(tag::TimeCodes); tc && tc->count >= 8)
        ci.startTimecode = decodeSmpteTimecode(tc->data.data(), *fpsOut);
    if (const Entry* rn = ifd.find(tag::ReelName)) ci.meta.vendor["ReelName"] = rn->ascii();
    // EXIF
    if (auto exifOff = ifd.u32(tag::ExifIFD)) {
        auto exif = r.readIfd(*exifOff);
        if (exif) {
            if (auto iso = exif->u32(tag::ISOSpeedRatings)) ci.meta.iso = *iso;
            if (auto et = exif->real(tag::ExposureTime)) ci.meta.exposureTimeSec = *et;
            LensInfo lens;
            bool haveLens = false;
            if (const Entry* lm = exif->find(tag::LensModel)) { lens.model = lm->ascii(); haveLens = true; }
            if (auto fl = exif->real(tag::FocalLength)) { lens.focalLengthMm = *fl; haveLens = true; }
            if (auto fn = exif->real(tag::FNumber)) { lens.apertureF = *fn; haveLens = true; }
            if (haveLens) ci.meta.lens = lens;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
class CdngReader final : public ICodecReader {
public:
    Expected<ClipInfo> open(const fs::path& path) override {
        std::error_code ec;
        dir_ = fs::is_directory(path, ec) ? path : path.parent_path();
        frames_ = listDngFrames(dir_);
        if (frames_.empty()) return makeError(ErrorCode::Format, "no .dng frames in " + dir_.string());
        info_ = ClipInfo{};
        info_.format = Format::CinemaDNG;
        info_.path = dir_;
        info_.frameCount = static_cast<int64_t>(frames_.size());
        Reader r;
        R3DSS_TRY(r.open(frames_.front()));
        R3DSS_TRY_ASSIGN(ifd, findRawIfd(r));
        Rational fps{24, 1};
        R3DSS_TRY(readClipInfoFromIfd(r, ifd, info_, &fps));
        info_.fps = fps;
        if (info_.startTimecode) info_.startTimecode->fps = fps;
        for (const auto& f : frames_) info_.sourceBytes += fs::file_size(f, ec);
        // Audio sidecar: <dirname>.wav, else the first .wav in the directory.
        fs::path wav = dir_ / (dir_.filename().string() + ".wav");
        if (!fs::exists(wav, ec)) {
            wav.clear();
            for (const auto& e : fs::directory_iterator(dir_, ec))
                if (e.is_regular_file(ec) && lowerExt(e.path()) == ".wav") { wav = e.path(); break; }
        }
        if (!wav.empty()) {
            auto wi = wav::probe(wav);
            if (wi) {
                wav_ = *wi;
                wavPath_ = wav;
                info_.audio = wi->audio;
                info_.sourceBytes += fs::file_size(wav, ec);
            } else {
                R3DSS_LOG_WARN("cdng", "ignoring unreadable audio sidecar " + wav.string() + ": " + wi.error().toString());
            }
        }
        return info_;
    }
    const ClipInfo& info() const override { return info_; }

    Expected<void> readFrame(int64_t index, BayerFrame& out) override {
        if (index < 0 || index >= static_cast<int64_t>(frames_.size()))
            return makeError(ErrorCode::InvalidArgument, "frame index out of range");
        Reader r;
        R3DSS_TRY(r.open(frames_[static_cast<size_t>(index)]));
        R3DSS_TRY_ASSIGN(ifd, findRawIfd(r));
        R3DSS_TRY_ASSIGN(g, geometryOf(ifd));
        if (g.width != info_.width || g.height != info_.height)
            return makeError(ErrorCode::Format, "frame " + std::to_string(index) + " has a different size");
        out.width = g.width;
        out.height = g.height;
        out.bits = info_.bits;
        out.cfa = info_.cfa;
        out.black = info_.black;
        out.white = info_.white;
        out.index = index;
        out.meta = FrameMetadata{};
        R3DSS_TRY(decodeFrameData(r, ifd, g, out));
        if (const Entry* tc = ifd.find(tag::TimeCodes); tc && tc->count >= 8)
            out.meta.timecode = decodeSmpteTimecode(tc->data.data(), info_.fps);
        else if (info_.startTimecode)
            out.meta.timecode = info_.startTimecode->plusFrames(index);
        return {};
    }

    Expected<AudioBlock> readAudio(int64_t first, int64_t count) override {
        if (wavPath_.empty()) {
            AudioBlock b;
            b.info = info_.audio;
            return b;
        }
        return wav::read(wavPath_, wav_, first, count);
    }

    Expected<Timecode> frameTimecode(int64_t index) override {
        if (!info_.startTimecode) return makeError(ErrorCode::Unsupported, "clip has no timecode");
        return info_.startTimecode->plusFrames(index);
    }
    void close() override { frames_.clear(); }

private:
    fs::path dir_, wavPath_;
    std::vector<fs::path> frames_;
    ClipInfo info_;
    wav::WavInfo wav_;
};

// ---------------------------------------------------------------------------------------------
class CdngWriter final : public ICodecWriter {
public:
    explicit CdngWriter(CdngWriterOptions o) : opts_(std::move(o)) {}

    Expected<void> begin(const WriterConfig& cfg) override {
        cfg_ = cfg;
        if (cfg.width & 1u || cfg.height & 1u) return makeError(ErrorCode::InvalidArgument, "odd output size");
        std::error_code ec;
        fs::create_directories(cfg.dest, ec);
        if (!fs::is_directory(cfg.dest, ec)) return makeError(ErrorCode::Io, "cannot create " + cfg.dest.string());
        clipName_ = cfg.clipName.empty() ? cfg.dest.filename().string() : cfg.clipName;
        if (cfg.audio.present()) {
            wavPath_ = cfg.dest / (clipName_ + ".wav");
            R3DSS_TRY(wav_.begin(wavPath_, cfg.audio));
            summary_.files.push_back(wavPath_);
        }
        return {};
    }

    Expected<EncodedStats> writeFrame(const BayerFrame& f, FrameBudget) override {
        if (f.width != cfg_.width || f.height != cfg_.height)
            return makeError(ErrorCode::InvalidArgument, "frame size does not match writer config");
        char name[64];
        std::snprintf(name, sizeof name, "_%06lld.dng", static_cast<long long>(frameNo_));
        const fs::path p = cfg_.dest / (clipName_ + name);
        Writer w;
        Writer::IfdBuilder raw, exif;
        std::vector<uint32_t> offsets, counts;
        const bool lj = opts_.losslessJpeg;
        if (lj) {
            const uint32_t tileH = std::max<uint32_t>(16, (opts_.tileRows + 15) / 16 * 16);
            const uint32_t tileW = (f.width + 15) / 16 * 16;
            const uint32_t nTiles = (f.height + tileH - 1) / tileH;
            std::vector<std::vector<uint8_t>> encoded(nTiles);
            std::vector<std::string> errors(nTiles);
            auto encodeTile = [&](size_t t) {
                const uint32_t y0 = static_cast<uint32_t>(t) * tileH;
                const uint32_t rows = std::min(tileH, f.height - y0);
                std::vector<uint16_t> tile(static_cast<size_t>(tileW) * tileH);
                for (uint32_t y = 0; y < tileH; ++y) {
                    const uint32_t sy = y0 + std::min(y, rows - 1);
                    const uint16_t* src = f.data.data() + static_cast<size_t>(sy) * f.width;
                    uint16_t* dst = tile.data() + static_cast<size_t>(y) * tileW;
                    std::memcpy(dst, src, f.width * sizeof(uint16_t));
                    for (uint32_t x = f.width; x < tileW; ++x) dst[x] = src[f.width - 1];
                }
                ljpeg::EncodeParams ep;
                ep.precision = 16;
                ep.components = 2;
                ep.predictor = 1;
                auto enc = ljpeg::encode(tile.data(), tileW, tileH, ep);
                if (enc) encoded[t] = std::move(*enc);
                else errors[t] = enc.error().toString();
            };
            auto pool = sharedThreadPool();
            if (pool && pool->size() > 1 && nTiles > 1) pool->parallelFor(nTiles, encodeTile);
            else for (size_t t = 0; t < nTiles; ++t) encodeTile(t);
            for (size_t t = 0; t < nTiles; ++t) {
                if (!errors[t].empty()) return makeError(ErrorCode::Internal, "tile encode failed: " + errors[t]);
                offsets.push_back(static_cast<uint32_t>(w.appendData(encoded[t])));
                counts.push_back(static_cast<uint32_t>(encoded[t].size()));
            }
            raw.setU32(tag::TileWidth, {tileW});
            raw.setU32(tag::TileLength, {tileH});
            raw.setU32(tag::TileOffsets, offsets);
            raw.setU32(tag::TileByteCounts, counts);
            raw.setU16(tag::Compression, {7});
        } else {
            std::vector<uint8_t> bytes(f.byteSize());
            for (size_t i = 0; i < f.data.size(); ++i) {
                bytes[2 * i] = static_cast<uint8_t>(f.data[i]);
                bytes[2 * i + 1] = static_cast<uint8_t>(f.data[i] >> 8);
            }
            offsets.push_back(static_cast<uint32_t>(w.appendData(bytes)));
            counts.push_back(static_cast<uint32_t>(bytes.size()));
            raw.setU32(tag::StripOffsets, offsets);
            raw.setU32(tag::RowsPerStrip, {f.height});
            raw.setU32(tag::StripByteCounts, counts);
            raw.setU16(tag::Compression, {1});
        }
        raw.setU32(tag::NewSubFileType, {0});
        raw.setU32(tag::ImageWidth, {f.width});
        raw.setU32(tag::ImageLength, {f.height});
        raw.setU16(tag::BitsPerSample, {16});
        raw.setU16(tag::Photometric, {kPhotometricCfa});
        raw.setU16(tag::Orientation, {1});
        raw.setU16(tag::SamplesPerPixel, {1});
        raw.setU16(tag::PlanarConfiguration, {1});
        const auto& m = cfg_.meta;
        raw.setAscii(tag::Make, m.cameraMake.empty() ? "Unknown" : m.cameraMake);
        raw.setAscii(tag::Model, m.cameraModel.empty() ? "Unknown" : m.cameraModel);
        raw.setAscii(tag::Software, opts_.software);
        raw.setU16(tag::CFARepeatPatternDim, {2, 2});
        {
            const auto cols = cfaColors(f.cfa);
            raw.setU8(tag::CFAPattern, {uint8_t(cols[0]), uint8_t(cols[1]), uint8_t(cols[2]), uint8_t(cols[3])});
        }
        raw.setU8(tag::DNGVersion, {1, 4, 0, 0});
        raw.setU8(tag::DNGBackwardVersion, {1, 1, 0, 0});
        raw.setAscii(tag::UniqueCameraModel, m.cameraModel.empty() ? "R3D Supersampler" : m.cameraModel);
        raw.setU8(tag::CFAPlaneColor, {0, 1, 2});
        raw.setU16(tag::CFALayout, {1});
        raw.setU16(tag::BlackLevelRepeatDim, {2, 2});
        raw.setU32(tag::BlackLevel, {f.black[0], f.black[1], f.black[2], f.black[3]});
        raw.setU32(tag::WhiteLevel, {f.white});
        if (m.color) {
            std::vector<std::pair<int32_t, int32_t>> cm;
            for (double v : m.color->colorMatrix1) cm.push_back(toSRational(v));
            raw.setSRational(tag::ColorMatrix1, cm);
            raw.setU16(tag::CalibrationIlluminant1, {m.color->calibrationIlluminant1});
            if (m.color->colorMatrix2) {
                std::vector<std::pair<int32_t, int32_t>> cm2;
                for (double v : *m.color->colorMatrix2) cm2.push_back(toSRational(v));
                raw.setSRational(tag::ColorMatrix2, cm2);
                raw.setU16(tag::CalibrationIlluminant2, {m.color->calibrationIlluminant2.value_or(17)});
            }
        } else {
            raw.setSRational(tag::ColorMatrix1, {{1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}});
            raw.setU16(tag::CalibrationIlluminant1, {21});
        }
        if (m.whiteBalance) {
            std::vector<std::pair<uint32_t, uint32_t>> wb;
            for (double v : m.whiteBalance->asShotNeutral) wb.push_back(toRational(std::max(1e-6, v), 1000000));
            raw.setRational(tag::AsShotNeutral, wb);
        }
        for (const auto& ob : m.opaque)
            if (ob.tag == "DNGPrivateData" && !ob.bytes.empty()) raw.setU8(tag::DNGPrivateData, ob.bytes);
        const Timecode tc = f.meta.timecode ? *f.meta.timecode
                                            : (cfg_.startTimecode ? cfg_.startTimecode->plusFrames(frameNo_)
                                                                  : Timecode::fromFrames(frameNo_, cfg_.fps, false));
        {
            const auto b = encodeSmpteTimecode(tc);
            raw.setU8(tag::TimeCodes, std::vector<uint8_t>(b.begin(), b.end()));
        }
        raw.setSRational(tag::FrameRate, {{static_cast<int32_t>(cfg_.fps.num), static_cast<int32_t>(cfg_.fps.den)}});
        if (auto it = m.vendor.find("ReelName"); it != m.vendor.end())
            if (auto s = std::get_if<std::string>(&it->second)) raw.setAscii(tag::ReelName, *s);
        // EXIF IFD
        exif.setU8(tag::ExifVersion, {'0', '2', '3', '0'}, Type::Undefined);
        const auto iso = f.meta.iso ? f.meta.iso : m.iso;
        if (iso) exif.setU16(tag::ISOSpeedRatings, {static_cast<uint16_t>(std::min<uint32_t>(*iso, 65535))});
        const auto et = f.meta.exposureTimeSec ? f.meta.exposureTimeSec : m.exposureTimeSec;
        if (et) exif.setRational(tag::ExposureTime, {toRational(*et, 1000000)});
        if (m.lens) {
            if (!m.lens->model.empty()) exif.setAscii(tag::LensModel, m.lens->model);
            if (m.lens->focalLengthMm) exif.setRational(tag::FocalLength, {toRational(*m.lens->focalLengthMm, 100)});
            if (m.lens->apertureF) exif.setRational(tag::FNumber, {toRational(*m.lens->apertureF, 100)});
        }
        const uint64_t exifOff = w.writeIfd(exif);
        raw.setU32(tag::ExifIFD, {static_cast<uint32_t>(exifOff)});
        const uint64_t ifd0 = w.writeIfd(raw);
        w.setFirstIfd(ifd0);
        R3DSS_TRY(w.save(p));
        summary_.files.push_back(p);
        ++frameNo_;
        ++summary_.framesWritten;
        summary_.totalBytes += w.bytes().size();
        EncodedStats st;
        st.bytes = w.bytes().size();
        st.bitsPerPixel = static_cast<double>(st.bytes) * 8.0 / static_cast<double>(f.sampleCount());
        return st;
    }

    Expected<void> writeAudio(const AudioBlock& b) override {
        if (wavPath_.empty()) return {};
        R3DSS_TRY(wav_.write(b));
        summary_.audioSamplesWritten += b.sampleCount;
        return {};
    }

    Expected<WriteSummary> finish() override {
        if (!wavPath_.empty()) {
            R3DSS_TRY(wav_.finish());
            std::error_code ec;
            summary_.totalBytes += fs::file_size(wavPath_, ec);
        }
        return summary_;
    }

    void abort() override {
        if (!wavPath_.empty()) wav_.abort();
        std::error_code ec;
        for (const auto& f : summary_.files) fs::remove(f, ec);
    }

private:
    CdngWriterOptions opts_;
    WriterConfig cfg_;
    std::string clipName_;
    fs::path wavPath_;
    wav::StreamWriter wav_;
    int64_t frameNo_ = 0;
    WriteSummary summary_;
};

class CdngFactory final : public ICodecFactory {
public:
    explicit CdngFactory(CdngWriterOptions o) : opts_(std::move(o)) {}
    Format format() const override { return Format::CinemaDNG; }
    std::string displayName() const override { return "CinemaDNG (reference codec)"; }
    Capabilities caps() const override {
        Capabilities c;
        c.read = c.write = c.lossless = true;
        c.rateControl = false;
        c.dimAlignment = 2;
        c.bitDepths = {8, 10, 12, 14, 16};
        return c;
    }
    ModuleStatus status() const override { return {ModuleState::Available, "built in (open format, lossless only)"}; }
    bool probe(const fs::path& p) const override {
        std::error_code ec;
        if (fs::is_directory(p, ec)) return !listDngFrames(p).empty();
        return fs::is_regular_file(p, ec) && lowerExt(p) == ".dng";
    }
    std::unique_ptr<ICodecReader> makeReader() const override { return std::make_unique<CdngReader>(); }
    std::unique_ptr<ICodecWriter> makeWriter() const override { return std::make_unique<CdngWriter>(opts_); }

private:
    CdngWriterOptions opts_;
};

} // namespace

std::shared_ptr<ICodecFactory> makeCinemaDngCodecFactory(CdngWriterOptions opts) {
    return std::make_shared<CdngFactory>(std::move(opts));
}

} // namespace r3dss::codec
