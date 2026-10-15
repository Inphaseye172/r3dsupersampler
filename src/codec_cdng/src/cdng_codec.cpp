#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/core/paths.hpp"
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
    std::string e = toUtf8(p.extension());
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

bool naturalLess(std::string_view a, std::string_view b) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && digit(a[ie])) ++ie;
            while (je < b.size() && digit(b[je])) ++je;
            size_t is = i, js = j; // skip leading zeros, keep one digit
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return ie - is < je - js;
            if (const int c = a.substr(is, ie - is).compare(b.substr(js, je - js)); c != 0) return c < 0;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
            ++i;
            ++j;
        }
    }
    if (i == a.size() && j == b.size()) return a < b; // equal in natural order (e.g. "01" vs "1")
    return i == a.size();
}

std::vector<fs::path> listDngFrames(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || lowerExt(e.path()) != ".dng") continue;
        // Hidden files, notably macOS AppleDouble "._<frame>.dng" companions on exFAT/SMB volumes,
        // are not frames.
        const std::string name = toUtf8(e.path().filename());
        if (name.empty() || name[0] == '.') continue;
        out.push_back(e.path());
    }
    // Natural order, so unpadded frame numbers (clip_2 before clip_10) play in sequence.
    std::sort(out.begin(), out.end(),
              [](const fs::path& x, const fs::path& y) { return naturalLess(toUtf8(x.filename()), toUtf8(y.filename())); });
    return out;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct FrameGeometry {
    uint32_t width = 0, height = 0, bits = 16, compression = 1;
    std::vector<uint64_t> blockOffsets, blockBytes;
    uint32_t blockW = 0, blockH = 0; // tile size, or (width, rowsPerStrip) for strips
    uint32_t blocksAcross = 1, blocksDown = 1;
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
    if (ifd.u32(tag::SampleFormat).value_or(1) != 1)
        return makeError(ErrorCode::Unsupported, "floating-point or signed DNG samples are not supported");
    if (!g.width || !g.height) return makeError(ErrorCode::Format, "missing image dimensions");
    if (g.bits < 1 || g.bits > 16)
        return makeError(ErrorCode::Unsupported, "BitsPerSample " + std::to_string(g.bits) + " is not supported");
    if (const Entry* to = ifd.find(tag::TileOffsets)) {
        g.tiled = true;
        g.blockW = ifd.u32(tag::TileWidth).value_or(0);
        g.blockH = ifd.u32(tag::TileLength).value_or(0);
        const Entry* tb = ifd.find(tag::TileByteCounts);
        if (!tb || tb->count != to->count || !g.blockW || !g.blockH) return makeError(ErrorCode::Format, "bad tile tags");
        g.blocksAcross = (g.width + g.blockW - 1) / g.blockW;
        g.blocksDown = (g.height + g.blockH - 1) / g.blockH;
        for (uint32_t i = 0; i < to->count; ++i) { g.blockOffsets.push_back(*to->u32(i)); g.blockBytes.push_back(*tb->u32(i)); }
    } else if (const Entry* so = ifd.find(tag::StripOffsets)) {
        g.blockW = g.width;
        g.blockH = ifd.u32(tag::RowsPerStrip).value_or(g.height);
        if (g.blockH == 0 || g.blockH > g.height) g.blockH = g.height;
        const Entry* sb = ifd.find(tag::StripByteCounts);
        if (!sb || sb->count != so->count) return makeError(ErrorCode::Format, "bad strip tags");
        g.blocksAcross = 1;
        g.blocksDown = (g.height + g.blockH - 1) / g.blockH;
        for (uint32_t i = 0; i < so->count; ++i) { g.blockOffsets.push_back(*so->u32(i)); g.blockBytes.push_back(*sb->u32(i)); }
    } else {
        return makeError(ErrorCode::Format, "no strips or tiles");
    }
    if (g.blockOffsets.size() < static_cast<size_t>(g.blocksAcross) * g.blocksDown)
        return makeError(ErrorCode::Format, std::string("too few ") + (g.tiled ? "tiles" : "strips") + " for the image size");
    return g;
}

// The rectangle of the raw image a frame is read from. `declared` is the ActiveArea (whole image
// when absent). `crop` follows LibRaw, the reference decoder the output must match: the visible
// image starts at the ActiveArea origin rounded up to an even row/column (CFAPattern stays anchored
// at the declared origin), and an odd trailing row/column is dropped so the mosaic consists of
// whole 2x2 cells.
struct RawRegion {
    PixelRect declared, crop;
};

Expected<RawRegion> regionOf(const Ifd& ifd, const FrameGeometry& g) {
    RawRegion rg;
    rg.declared = {0, 0, g.height, g.width};
    if (const Entry* aa = ifd.find(tag::ActiveArea)) {
        if (aa->count != 4) return makeError(ErrorCode::Format, "ActiveArea must have 4 values");
        PixelRect d{aa->u32(0).value_or(0), aa->u32(1).value_or(0), aa->u32(2).value_or(0), aa->u32(3).value_or(0)};
        if (!(d.top < d.bottom && d.left < d.right && d.bottom <= g.height && d.right <= g.width))
            return makeError(ErrorCode::Format, "ActiveArea lies outside the image");
        rg.declared = d;
    }
    const PixelRect& d = rg.declared;
    PixelRect& c = rg.crop;
    c.top = d.top + (d.top & 1u);
    c.left = d.left + (d.left & 1u);
    if (c.top >= d.bottom || c.left >= d.right) return makeError(ErrorCode::Unsupported, "active area smaller than 2x2");
    const uint32_t h = (d.bottom - c.top) & ~1u, w = (d.right - c.left) & ~1u;
    if (w < 2 || h < 2) return makeError(ErrorCode::Unsupported, "active area smaller than 2x2");
    c.bottom = c.top + h;
    c.right = c.left + w;
    return rg;
}

// CFA pattern of the cropped mosaic. CFAPattern is relative to the declared ActiveArea origin; the
// crop starts (crop - declared) rows/columns later.
Expected<CfaPattern> cfaOf(const Ifd& ifd, const RawRegion& rg) {
    std::array<int, 4> cols = cfaColors(CfaPattern::RGGB);
    if (const Entry* dim = ifd.find(tag::CFARepeatPatternDim))
        if (dim->count != 2 || dim->u32(0).value_or(0) != 2 || dim->u32(1).value_or(0) != 2)
            return makeError(ErrorCode::Unsupported, "CFA repeat pattern is not 2x2");
    if (const Entry* cp = ifd.find(tag::CFAPattern)) {
        if (cp->count != 4) return makeError(ErrorCode::Unsupported, "CFA repeat pattern is not 2x2");
        // CFAPattern values index CFAPlaneColor (default R, G, B).
        std::array<int, 3> plane{0, 1, 2};
        if (const Entry* pc = ifd.find(tag::CFAPlaneColor); pc && pc->count == 3)
            for (uint32_t i = 0; i < 3; ++i) plane[i] = static_cast<int>(pc->u32(i).value_or(i));
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t v = cp->u32(i).value_or(9);
            if (v > 2) return makeError(ErrorCode::Unsupported, "non-RGB CFA pattern");
            cols[i] = plane[v];
        }
    }
    const uint32_t dx = (rg.crop.left - rg.declared.left) & 1u, dy = (rg.crop.top - rg.declared.top) & 1u;
    std::array<int, 4> shifted{};
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x) shifted[y * 2 + x] = cols[((y + dy) & 1u) * 2 + ((x + dx) & 1u)];
    auto pat = cfaFromColors(shifted);
    if (!pat) return makeError(ErrorCode::Unsupported, "non-Bayer CFA pattern");
    return *pat;
}

// Smallest bit depth (8..16) that holds `v`.
uint16_t bitsFor(uint32_t v) {
    uint16_t bits = 8;
    while (bits < 16 && v > (1u << bits) - 1u) ++bits;
    return bits;
}

// Sample levels of one frame, in the linear (post-LinearizationTable) domain.
struct Levels {
    std::vector<uint16_t> lut; // LinearizationTable; empty = codes are linear
    bool lutMonotonic = true;
    uint32_t white = 0;
    uint16_t bits = 16;              // bits of the linear values
    std::array<uint32_t, 4> black{}; // per site of the cropped mosaic (plane order)
    // Black that varies inside a CFA site (BlackLevelDeltaH/V, BlackLevel repeat patterns that are
    // not 1x1/1x2/2x1/2x2) cannot be written as a 2x2 BlackLevel, so it is folded into the
    // samples: value' = value - (black(x, y) - black[site]).
    bool fold = false;
    uint32_t repRows = 1, repCols = 1;
    std::vector<double> rep;    // BlackLevel repeat pattern, anchored at the crop origin
    std::vector<double> dh, dv; // BlackLevelDeltaH/V per cropped column / row (0 if absent)
    double blackAt(uint32_t x, uint32_t y) const { return rep[(y % repRows) * repCols + x % repCols] + dh[x] + dv[y]; }
};

Expected<Levels> levelsOf(const Ifd& ifd, const FrameGeometry& g, const RawRegion& rg) {
    Levels lv;
    if (const Entry* lt = ifd.find(tag::LinearizationTable); lt && lt->count) {
        const uint32_t n = std::min<uint32_t>(lt->count, 65536);
        lv.lut.resize(n);
        for (uint32_t i = 0; i < n; ++i) lv.lut[i] = static_cast<uint16_t>(std::min<uint32_t>(lt->u32(i).value_or(0), 65535));
        lv.lutMonotonic = std::is_sorted(lv.lut.begin(), lv.lut.end());
    }
    const uint32_t lutMax = lv.lut.empty() ? 0 : *std::max_element(lv.lut.begin(), lv.lut.end());
    // WhiteLevel is a linear-domain value; without it LibRaw uses the table maximum, or 2^bits-1.
    const uint32_t defWhite = lv.lut.empty() ? (1u << g.bits) - 1u : lutMax;
    lv.white = std::clamp<uint32_t>(ifd.u32(tag::WhiteLevel).value_or(defWhite), 1, 65535);
    lv.bits = bitsFor(lv.lut.empty() ? lv.white : lutMax);

    // BlackLevel repeat pattern. LibRaw anchors it at its visible origin, which is the crop
    // origin (rounded up to even); for the usual 2x2 pattern that is the same as the raw origin.
    const Entry* bl = ifd.find(tag::BlackLevel);
    if (const Entry* rd = ifd.find(tag::BlackLevelRepeatDim); rd && rd->count >= 2) {
        lv.repRows = rd->u32(0).value_or(1);
        lv.repCols = rd->u32(1).value_or(1);
    } else if (bl && bl->count == 4) {
        lv.repRows = lv.repCols = 2; // four values without BlackLevelRepeatDim: per CFA site
    }
    if (lv.repRows < 1 || lv.repCols < 1 || lv.repRows > 64 || lv.repCols > 64)
        return makeError(ErrorCode::Unsupported, "BlackLevelRepeatDim out of range");
    lv.rep.assign(static_cast<size_t>(lv.repRows) * lv.repCols, 0.0);
    if (bl && bl->count) {
        const bool full = bl->count >= lv.rep.size();
        for (size_t i = 0; i < lv.rep.size(); ++i) lv.rep[i] = bl->real(full ? static_cast<uint32_t>(i) : 0).value_or(0.0);
    }
    // BlackLevelDeltaH/V are indexed from the declared ActiveArea origin.
    const PixelRect& c = rg.crop;
    lv.dh.assign(c.width(), 0.0);
    lv.dv.assign(c.height(), 0.0);
    if (const Entry* e = ifd.find(tag::BlackLevelDeltaH))
        for (uint32_t x = 0; x < c.width(); ++x)
            if (const uint32_t i = x + c.left - rg.declared.left; i < e->count) lv.dh[x] = e->real(i).value_or(0.0);
    if (const Entry* e = ifd.find(tag::BlackLevelDeltaV))
        for (uint32_t y = 0; y < c.height(); ++y)
            if (const uint32_t i = y + c.top - rg.declared.top; i < e->count) lv.dv[y] = e->real(i).value_or(0.0);

    // Per-site black: the mean of black(x, y) over the site. Deltas contribute their overall mean
    // (as LibRaw adds them); anything that varies inside a site is folded into the samples.
    const auto mean = [](const std::vector<double>& v) {
        double s = 0.0;
        for (double x : v) s += x;
        return v.empty() ? 0.0 : s / static_cast<double>(v.size());
    };
    const double mh = mean(lv.dh), mv = mean(lv.dv);
    bool varies = false;
    for (double v : lv.dh) varies |= v != mh;
    for (double v : lv.dv) varies |= v != mv;
    for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t sx = s & 1u, sy = s >> 1;
        double sum = 0.0, first = 0.0;
        uint64_t n = 0;
        for (uint32_t a = 0; a < lv.repRows; ++a) {
            // rows y of parity sy with y % repRows == a, within the cropped height
            uint64_t ny = 0;
            for (uint32_t y = a; y < std::min<uint32_t>(c.height(), 2 * lv.repRows); y += lv.repRows) ny += (y & 1u) == sy;
            for (uint32_t b = 0; b < lv.repCols; ++b) {
                uint64_t nx = 0;
                for (uint32_t x = b; x < std::min<uint32_t>(c.width(), 2 * lv.repCols); x += lv.repCols) nx += (x & 1u) == sx;
                if (!ny || !nx) continue;
                const double v = lv.rep[static_cast<size_t>(a) * lv.repCols + b];
                if (n == 0) first = v;
                varies |= v != first;
                sum += v * static_cast<double>(ny * nx);
                n += ny * nx;
            }
        }
        const double siteMean = (n ? sum / static_cast<double>(n) : 0.0) + mh + mv;
        lv.black[s] = static_cast<uint32_t>(std::lround(std::clamp(siteMean, 0.0, 65535.0)));
    }
    lv.fold = varies;
    return lv;
}

// Unpacks `count` samples of `bits` each from one row (TIFF uncompressed): 8/16-bit as bytes or
// words in the file's byte order, other depths MSB-first packed (FillOrder 1, as DNG requires).
void unpackRow(const uint8_t* src, uint32_t bits, uint32_t count, bool bigEndian16, uint16_t* dst) {
    if (bits == 16) {
        for (uint32_t i = 0; i < count; ++i)
            dst[i] = bigEndian16 ? static_cast<uint16_t>((src[2 * i] << 8) | src[2 * i + 1])
                                 : static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
        return;
    }
    if (bits == 8) {
        for (uint32_t i = 0; i < count; ++i) dst[i] = src[i];
        return;
    }
    uint64_t acc = 0;
    uint32_t nbits = 0;
    size_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        while (nbits < bits) { acc = (acc << 8) | src[pos++]; nbits += 8; }
        dst[i] = static_cast<uint16_t>((acc >> (nbits - bits)) & ((1u << bits) - 1u));
        nbits -= bits;
        acc &= (1ull << nbits) - 1ull;
    }
}

// Decodes the strips/tiles that overlap `crop` straight into `out` (crop.width() x crop.height()).
Expected<void> decodeFrameData(const Reader& r, const FrameGeometry& g, const PixelRect& crop, std::vector<uint16_t>& out) {
    if (g.compression != 1 && g.compression != 7)
        return makeError(ErrorCode::Unsupported, "DNG compression " + std::to_string(g.compression) + " not supported");
    const uint32_t ow = crop.width();
    out.assign(static_cast<size_t>(ow) * crop.height(), 0);
    const auto& bytes = r.bytes();
    const char* kind = g.tiled ? "tile" : "strip";

    std::vector<size_t> todo; // blocks that overlap the crop
    for (uint32_t by = 0; by < g.blocksDown; ++by) {
        const uint32_t y0 = by * g.blockH, rows = std::min(g.blockH, g.height - y0);
        if (y0 + rows <= crop.top || y0 >= crop.bottom) continue;
        for (uint32_t bx = 0; bx < g.blocksAcross; ++bx) {
            const uint32_t x0 = bx * g.blockW, cols = std::min(g.blockW, g.width - x0);
            if (x0 + cols <= crop.left || x0 >= crop.right) continue;
            todo.push_back(static_cast<size_t>(by) * g.blocksAcross + bx);
        }
    }
    std::vector<std::string> errors(todo.size());
    auto decodeBlock = [&](size_t k) {
        const size_t b = todo[k];
        const uint32_t bx = static_cast<uint32_t>(b % g.blocksAcross), by = static_cast<uint32_t>(b / g.blocksAcross);
        const uint32_t x0 = bx * g.blockW, y0 = by * g.blockH;
        const uint32_t rows = std::min(g.blockH, g.height - y0), cols = std::min(g.blockW, g.width - x0);
        const uint64_t off = g.blockOffsets[b], len = g.blockBytes[b];
        const std::string where = std::string(kind) + " " + std::to_string(b);
        if (off > bytes.size() || len > bytes.size() - off) { errors[k] = where + ": data lies beyond the end of the file"; return; }
        std::vector<uint16_t> tmp;
        ljpeg::Image img;
        const uint16_t* src = nullptr;
        size_t stride = 0;
        if (g.compression == 7) {
            auto dec = ljpeg::decode(bytes.data() + off, static_cast<size_t>(len));
            if (!dec) { errors[k] = where + ": " + dec.error().message; return; }
            img = std::move(*dec);
            // The stream normally is blockW samples wide (components folded into the width). Other
            // encoders fold rows differently (e.g. 2*blockW x blockH/2); the samples are then in
            // raster order of the block, as LibRaw and the DNG SDK read them. Anything else is an
            // error, never a copy with the wrong stride.
            const uint64_t n = static_cast<uint64_t>(img.width) * img.height;
            if (img.width == g.blockW && img.height >= rows) {
                stride = img.width;
            } else if (n == static_cast<uint64_t>(g.blockW) * rows || n == static_cast<uint64_t>(g.blockW) * g.blockH) {
                stride = g.blockW;
            } else {
                errors[k] = where + ": lossless JPEG stream is " + std::to_string(img.width) + "x" + std::to_string(img.height) +
                            " samples, which does not fit the " + std::to_string(g.blockW) + "x" +
                            std::to_string(g.tiled ? g.blockH : rows) + " " + kind;
                return;
            }
            src = img.samples.data();
        } else {
            // TIFF: every row of a strip or tile starts on a byte boundary.
            const uint64_t rowBytes = (static_cast<uint64_t>(g.blockW) * g.bits + 7) / 8;
            if (len < rowBytes * rows) { errors[k] = where + ": data truncated"; return; }
            tmp.resize(static_cast<size_t>(g.blockW) * rows);
            for (uint32_t y = 0; y < rows; ++y)
                unpackRow(bytes.data() + off + y * rowBytes, g.bits, g.blockW, r.bigEndian(), tmp.data() + static_cast<size_t>(y) * g.blockW);
            src = tmp.data();
            stride = g.blockW;
        }
        const uint32_t ys = std::max(y0, crop.top), ye = std::min(y0 + rows, crop.bottom);
        const uint32_t xs = std::max(x0, crop.left), xe = std::min(x0 + cols, crop.right);
        for (uint32_t y = ys; y < ye; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y - crop.top) * ow + (xs - crop.left),
                        src + static_cast<size_t>(y - y0) * stride + (xs - x0), (xe - xs) * sizeof(uint16_t));
    };
    auto pool = sharedThreadPool();
    if (pool && pool->size() > 1 && todo.size() > 1) pool->parallelFor(todo.size(), decodeBlock);
    else for (size_t k = 0; k < todo.size(); ++k) decodeBlock(k);
    for (const auto& e : errors)
        if (!e.empty()) return makeError(ErrorCode::Format, e);
    return {};
}

// Maps codes through the LinearizationTable and folds intra-site black variation into the samples.
void applyLevels(const Levels& lv, uint32_t width, uint32_t height, std::vector<uint16_t>& data) {
    if (!lv.lut.empty()) {
        const size_t last = lv.lut.size() - 1;
        for (auto& v : data) v = lv.lut[std::min<size_t>(v, last)];
    }
    if (lv.fold) {
        const double maxv = static_cast<double>((1u << lv.bits) - 1u);
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t* row = data.data() + static_cast<size_t>(y) * width;
            for (uint32_t x = 0; x < width; ++x) {
                const double site = static_cast<double>(lv.black[(x & 1u) + 2 * (y & 1u)]);
                const double v = static_cast<double>(row[x]) - (lv.blackAt(x, y) - site);
                row[x] = static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, maxv)));
            }
        }
    }
}

// Everything readFrame needs to know about one frame's raw IFD.
struct FrameLayout {
    FrameGeometry g;
    RawRegion region;
    CfaPattern cfa = CfaPattern::RGGB;
    Levels levels;
};

Expected<FrameLayout> layoutOf(const Ifd& ifd) {
    FrameLayout fl;
    R3DSS_TRY_ASSIGN(g, geometryOf(ifd));
    fl.g = std::move(g);
    R3DSS_TRY_ASSIGN(rg, regionOf(ifd, fl.g));
    fl.region = rg;
    R3DSS_TRY_ASSIGN(cfa, cfaOf(ifd, fl.region));
    fl.cfa = cfa;
    R3DSS_TRY_ASSIGN(lv, levelsOf(ifd, fl.g, fl.region));
    fl.levels = std::move(lv);
    return fl;
}

Expected<void> readClipInfoFromIfd(const Reader& r, const Ifd& ifd, ClipInfo& ci, Rational* fpsOut) {
    R3DSS_TRY_ASSIGN(layout, layoutOf(ifd));
    const PixelRect& declared = layout.region.declared;
    const PixelRect& crop = layout.region.crop;
    ci.rawWidth = layout.g.width;
    ci.rawHeight = layout.g.height;
    ci.declaredActiveArea = declared;
    ci.activeArea = crop;
    ci.width = crop.width();
    ci.height = crop.height();
    if (!(crop == declared))
        R3DSS_LOG_INFO("cdng", "reading the " + std::to_string(ci.width) + "x" + std::to_string(ci.height) +
                                   " mosaic at row " + std::to_string(crop.top) + ", column " + std::to_string(crop.left) +
                                   " of the " + std::to_string(layout.g.width) + "x" + std::to_string(layout.g.height) +
                                   " raw image (ActiveArea [" + std::to_string(declared.top) + ", " + std::to_string(declared.left) + ", " +
                                   std::to_string(declared.bottom) + ", " + std::to_string(declared.right) +
                                   "], moved to an even origin and an even size)");
    const Levels& lv = layout.levels;
    ci.white = lv.white;
    ci.bits = lv.bits;
    ci.black = lv.black;
    ci.cfa = layout.cfa;
    ci.meta.sampleEncoding = {};
    if (!lv.lut.empty() && lv.lutMonotonic) {
        ci.meta.sampleEncoding.bitsPerSample = static_cast<uint16_t>(layout.g.bits);
        ci.meta.sampleEncoding.linearizationTable = lv.lut;
    } else if (!lv.lut.empty()) {
        // An output cannot re-encode to a table that is not monotonic: it gets linear values.
        ci.meta.sampleEncoding.bitsPerSample = bitsFor(lv.white);
        R3DSS_LOG_INFO("cdng", "LinearizationTable is not monotonic; the output stores linear values without a table");
    } else {
        ci.meta.sampleEncoding.bitsPerSample = static_cast<uint16_t>(layout.g.bits);
    }
    if (lv.fold)
        R3DSS_LOG_INFO("cdng", "black level varies inside a CFA site (BlackLevelDeltaH/V or a large BlackLevel repeat); "
                               "the variation is subtracted from the samples and a per-site black is written");
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
        if (frames_.empty()) return makeError(ErrorCode::Format, "no .dng frames in " + toUtf8(dir_));
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
        fs::path wav = dir_ / dir_.filename();
        wav += ".wav";
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
                R3DSS_LOG_WARN("cdng", "ignoring unreadable audio sidecar " + toUtf8(wav) + ": " + wi.error().toString());
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
        R3DSS_TRY_ASSIGN(fl, layoutOf(ifd));
        const PixelRect& c = fl.region.crop;
        if (c.width() != info_.width || c.height() != info_.height)
            return makeError(ErrorCode::Format, "frame " + std::to_string(index) + " has a different size");
        // Levels, bit depth and CFA phase come from this frame's own tags.
        out.width = c.width();
        out.height = c.height();
        out.bits = std::max(info_.bits, fl.levels.bits);
        out.cfa = fl.cfa;
        out.black = fl.levels.black;
        out.white = fl.levels.white;
        out.index = index;
        out.meta = FrameMetadata{};
        R3DSS_TRY(decodeFrameData(r, fl.g, c, out.data));
        applyLevels(fl.levels, out.width, out.height, out.data);
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
// LJ92 tile layout (I10). A tile is never wider than the image: LibRaw-family decoders wrap the
// rows of the first tile column at min(TileWidth, ImageWidth), so a single padded column wider than
// the image shears every row after the first. Tiles are at most 256 wide (a multiple of 16 that
// fits the image) and at most `maxRows` (a multiple of 16) tall; only the last column/row is padded.
// Images narrower than 16 are written as one strip, whose stream width is the image width.
// A frame never has exactly four tiles: LibRaw (after dcraw) takes four TileOffsets for a Sinar
// 4-shot file and misreads such DNGs (rawpy 0.27.1: every 2x2 grid whose width is two tiles, e.g.
// 512x512 or 512x288 with 256x256 tiles), so the tile length, else the width, is halved instead.
struct TileLayout {
    uint32_t tileW = 0, tileH = 0, across = 1, down = 1;
    bool strip = false;
};

TileLayout tileLayoutFor(uint32_t width, uint32_t height, uint32_t maxRows) {
    TileLayout t;
    if (width < 16) {
        t.strip = true;
        t.tileW = width;
        t.tileH = height;
        return t;
    }
    const auto layout = [&](uint32_t tw, uint32_t th) {
        TileLayout l;
        l.tileW = tw;
        l.tileH = th;
        l.across = (width + tw - 1) / tw;
        l.down = (height + th - 1) / th;
        return l;
    };
    const auto half = [](uint32_t v) { return std::max<uint32_t>(16, (v / 2 + 15) / 16 * 16); };
    const uint32_t tw = std::min<uint32_t>(256, width / 16 * 16);
    const uint32_t th = std::min<uint32_t>(std::max<uint32_t>(16, (maxRows + 15) / 16 * 16), (height + 15) / 16 * 16);
    for (const auto& [w, h] : {std::pair{tw, th}, std::pair{tw, half(th)}, std::pair{half(tw), th}, std::pair{half(tw), half(th)}}) {
        t = layout(w, h);
        if (t.across * t.down != 4) return t;
    }
    return layout(tw, th); // not reached for any image of 16x16 or more
}

// Nearest code of a non-decreasing LinearizationTable for every 16-bit linear value (ties go to
// the lower code; values above the table map to its last code).
std::vector<uint16_t> inverseTable(const std::vector<uint16_t>& t) {
    std::vector<uint16_t> inv(65536);
    const size_t n = t.size();
    size_t i = 0; // first code whose value is >= v
    for (uint32_t v = 0; v < 65536; ++v) {
        while (i < n && t[i] < v) ++i;
        size_t best;
        if (i == n) best = n - 1;
        else if (i == 0 || t[i] == v) best = i;
        else best = (v - t[i - 1] <= t[i] - v) ? i - 1 : i;
        inv[v] = static_cast<uint16_t>(best);
    }
    return inv;
}

class CdngWriter final : public ICodecWriter {
public:
    explicit CdngWriter(CdngWriterOptions o) : opts_(std::move(o)) {}

    Expected<void> begin(const WriterConfig& cfg) override {
        cfg_ = cfg;
        if (cfg.width & 1u || cfg.height & 1u) return makeError(ErrorCode::InvalidArgument, "odd output size");
        std::error_code ec;
        fs::create_directories(cfg.dest, ec);
        if (!fs::is_directory(cfg.dest, ec)) return makeError(ErrorCode::Io, "cannot create " + toUtf8(cfg.dest));
        clipName_ = cfg.clipName.empty() ? toUtf8(cfg.dest.filename()) : cfg.clipName;
        // Keep the source's LinearizationTable (I9): frames arrive linear and are re-encoded to the
        // nearest table code, so the output keeps the source's code depth and tone design.
        table_.clear();
        inverse_.clear();
        if (const auto& t = cfg.meta.sampleEncoding.linearizationTable; !t.empty()) {
            if (std::is_sorted(t.begin(), t.end())) {
                table_ = t;
                inverse_ = inverseTable(t);
            } else {
                R3DSS_LOG_INFO("cdng", "LinearizationTable is not monotonic; writing linear values without a table");
            }
        }
        if (cfg.audio.present()) {
            wavPath_ = cfg.dest / fromUtf8(clipName_ + ".wav");
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
        const fs::path p = cfg_.dest / fromUtf8(clipName_ + name);
        Writer w;
        Writer::IfdBuilder raw, exif;
        std::vector<uint32_t> offsets, counts;
        const bool lj = opts_.losslessJpeg;
        // Codes to store: the linear values, or their nearest LinearizationTable codes.
        const uint16_t* data = f.data.data();
        std::vector<uint16_t> codes;
        if (!inverse_.empty()) {
            codes.resize(f.data.size());
            for (size_t i = 0; i < codes.size(); ++i) codes[i] = inverse_[f.data[i]];
            data = codes.data();
        }
        // Stored depth: the source's BitsPerSample when known (else 16), never fewer bits than the
        // data needs.
        uint16_t bps = cfg_.meta.sampleEncoding.bitsPerSample ? cfg_.meta.sampleEncoding.bitsPerSample : uint16_t(16);
        {
            const uint16_t maxv = f.data.empty() ? uint16_t(0) : *std::max_element(data, data + f.data.size());
            bps = std::clamp<uint16_t>(bps, 2, 16);
            while (bps < 16 && maxv > (1u << bps) - 1u) ++bps;
        }
        if (lj) {
            const TileLayout tl = tileLayoutFor(f.width, f.height, opts_.tileRows);
            const size_t nTiles = static_cast<size_t>(tl.across) * tl.down;
            std::vector<std::vector<uint8_t>> encoded(nTiles);
            std::vector<std::string> errors(nTiles);
            auto encodeTile = [&](size_t t) {
                const uint32_t x0 = static_cast<uint32_t>(t % tl.across) * tl.tileW;
                const uint32_t y0 = static_cast<uint32_t>(t / tl.across) * tl.tileH;
                const uint32_t cols = std::min(tl.tileW, f.width - x0), rows = std::min(tl.tileH, f.height - y0);
                std::vector<uint16_t> tile(static_cast<size_t>(tl.tileW) * tl.tileH);
                for (uint32_t y = 0; y < tl.tileH; ++y) { // edge-replicate the padding of the last column/row
                    const uint16_t* src = data + static_cast<size_t>(y0 + std::min(y, rows - 1)) * f.width + x0;
                    uint16_t* dst = tile.data() + static_cast<size_t>(y) * tl.tileW;
                    std::memcpy(dst, src, cols * sizeof(uint16_t));
                    for (uint32_t x = cols; x < tl.tileW; ++x) dst[x] = src[cols - 1];
                }
                ljpeg::EncodeParams ep;
                ep.precision = bps;
                ep.components = 2;
                ep.predictor = 1;
                auto enc = ljpeg::encode(tile.data(), tl.tileW, tl.tileH, ep);
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
            if (tl.strip) {
                raw.setU32(tag::StripOffsets, offsets);
                raw.setU32(tag::RowsPerStrip, {f.height});
                raw.setU32(tag::StripByteCounts, counts);
            } else {
                raw.setU32(tag::TileWidth, {tl.tileW});
                raw.setU32(tag::TileLength, {tl.tileH});
                raw.setU32(tag::TileOffsets, offsets);
                raw.setU32(tag::TileByteCounts, counts);
            }
            raw.setU16(tag::Compression, {7});
        } else {
            std::vector<uint8_t> bytes(f.byteSize());
            for (size_t i = 0; i < f.data.size(); ++i) {
                bytes[2 * i] = static_cast<uint8_t>(data[i]);
                bytes[2 * i + 1] = static_cast<uint8_t>(data[i] >> 8);
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
        raw.setU16(tag::BitsPerSample, {lj ? bps : uint16_t(16)});
        if (!table_.empty()) raw.setU16(tag::LinearizationTable, table_);
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
        raw.setAscii(tag::UniqueCameraModel, m.cameraModel.empty() ? "r3dss" : m.cameraModel);
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
    std::vector<uint16_t> table_, inverse_; // LinearizationTable to write and its nearest-code inverse
};

class CdngFactory final : public ICodecFactory {
public:
    explicit CdngFactory(CdngWriterOptions o) : opts_(std::move(o)) {}
    Format format() const override { return Format::CinemaDNG; }
    std::string displayName() const override { return "CinemaDNG (reference codec)"; }
    Capabilities caps() const override {
        Capabilities c;
        c.read = c.write = c.lossless = true;
        c.readsMosaic = true;
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
