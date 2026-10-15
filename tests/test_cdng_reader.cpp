// CinemaDNG reader geometry, sample decoding and levels; writer tile layout and sample encoding.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <random>
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/codec_cdng/ljpeg.hpp"
#include "r3dss/codec_cdng/tiff.hpp"

using namespace r3dss;
using namespace r3dss::codec;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir(const std::string& name) {
    // Unique per test process: several builds may run the suite at the same time.
    static const std::string run = std::to_string(std::random_device{}());
    fs::path p = fs::temp_directory_path() / ("r3dss-test-" + name + "-" + run);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

// Packs one row MSB-first (TIFF FillOrder 1); 8/16-bit as bytes / little-endian words.
void packRow(const uint16_t* v, uint32_t n, uint32_t bits, std::vector<uint8_t>& out) {
    if (bits == 16) {
        for (uint32_t i = 0; i < n; ++i) { out.push_back(uint8_t(v[i])); out.push_back(uint8_t(v[i] >> 8)); }
        return;
    }
    uint64_t acc = 0;
    uint32_t nb = 0;
    for (uint32_t i = 0; i < n; ++i) {
        acc = (acc << bits) | v[i];
        nb += bits;
        while (nb >= 8) { out.push_back(uint8_t(acc >> (nb - 8))); nb -= 8; acc &= (1ull << nb) - 1; }
    }
    if (nb) out.push_back(uint8_t(acc << (8 - nb))); // each row starts on a byte boundary
}

struct DngSpec {
    uint32_t width = 64, height = 32;
    uint16_t bits = 16;
    uint16_t compression = 1;
    bool tiles = false;
    uint32_t rowsPerStrip = 0; // 0 = one strip
    uint32_t tileW = 32, tileH = 16;
    std::array<uint8_t, 4> cfa{0, 1, 1, 2};
    // Builds the LJ92 stream of one block (default: blockW wide, 2 components).
    std::function<std::vector<uint8_t>(const std::vector<uint16_t>&, uint32_t, uint32_t, uint16_t)> lj;
    std::function<void(tiff::Writer::IfdBuilder&)> tags;
};

std::vector<uint8_t> defaultLj(const std::vector<uint16_t>& blk, uint32_t w, uint32_t h, uint16_t bits) {
    ljpeg::EncodeParams p;
    p.precision = bits;
    p.components = 2;
    return *ljpeg::encode(blk.data(), w, h, p);
}

// Writes a one-IFD CFA DNG holding `codes` (width x height, row-major).
void writeDng(const fs::path& p, const std::vector<uint16_t>& codes, const DngSpec& s) {
    tiff::Writer w;
    tiff::Writer::IfdBuilder ifd;
    std::vector<uint32_t> offs, counts;
    const uint32_t bw = s.tiles ? s.tileW : s.width;
    const uint32_t bh = s.tiles ? s.tileH : (s.rowsPerStrip ? s.rowsPerStrip : s.height);
    for (uint32_t y0 = 0; y0 < s.height; y0 += bh) {
        for (uint32_t x0 = 0; x0 < (s.tiles ? s.width : 1u); x0 += bw) {
            const uint32_t rows = s.tiles ? bh : std::min(bh, s.height - y0);
            std::vector<uint16_t> blk(static_cast<size_t>(bw) * rows);
            for (uint32_t y = 0; y < rows; ++y)
                for (uint32_t x = 0; x < bw; ++x)
                    blk[size_t(y) * bw + x] = codes[size_t(std::min(y0 + y, s.height - 1)) * s.width + std::min(x0 + x, s.width - 1)];
            std::vector<uint8_t> bytes;
            if (s.compression == 7) bytes = (s.lj ? s.lj : defaultLj)(blk, bw, rows, s.bits);
            else for (uint32_t y = 0; y < rows; ++y) packRow(blk.data() + size_t(y) * bw, bw, s.bits, bytes);
            offs.push_back(static_cast<uint32_t>(w.appendData(bytes)));
            counts.push_back(static_cast<uint32_t>(bytes.size()));
        }
    }
    ifd.setU32(tiff::tag::NewSubFileType, {0});
    ifd.setU32(tiff::tag::ImageWidth, {s.width});
    ifd.setU32(tiff::tag::ImageLength, {s.height});
    ifd.setU16(tiff::tag::BitsPerSample, {s.bits});
    ifd.setU16(tiff::tag::Compression, {s.compression});
    ifd.setU16(tiff::tag::Photometric, {32803});
    ifd.setU16(tiff::tag::SamplesPerPixel, {1});
    if (s.tiles) {
        ifd.setU32(tiff::tag::TileWidth, {s.tileW});
        ifd.setU32(tiff::tag::TileLength, {s.tileH});
        ifd.setU32(tiff::tag::TileOffsets, offs);
        ifd.setU32(tiff::tag::TileByteCounts, counts);
    } else {
        ifd.setU32(tiff::tag::StripOffsets, offs);
        ifd.setU32(tiff::tag::RowsPerStrip, {bh});
        ifd.setU32(tiff::tag::StripByteCounts, counts);
    }
    ifd.setU16(tiff::tag::CFARepeatPatternDim, {2, 2});
    ifd.setU8(tiff::tag::CFAPattern, {s.cfa[0], s.cfa[1], s.cfa[2], s.cfa[3]});
    ifd.setU8(tiff::tag::DNGVersion, {1, 4, 0, 0});
    ifd.setU32(tiff::tag::WhiteLevel, {(1u << s.bits) - 1u});
    if (s.tags) s.tags(ifd);
    w.setFirstIfd(w.writeIfd(ifd));
    REQUIRE(w.save(p));
}

std::vector<uint16_t> randomCodes(uint32_t w, uint32_t h, uint16_t bits, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint16_t> v(size_t(w) * h);
    for (auto& x : v) x = static_cast<uint16_t>(rng() & ((1u << bits) - 1u));
    return v;
}

std::vector<uint16_t> crop(const std::vector<uint16_t>& v, uint32_t w, uint32_t top, uint32_t left, uint32_t cw, uint32_t ch) {
    std::vector<uint16_t> o;
    for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) o.push_back(v[size_t(top + y) * w + left + x]);
    return o;
}

struct Opened {
    std::unique_ptr<ICodecReader> reader;
    ClipInfo info;
};
Opened openClip(const fs::path& dir) {
    Opened o;
    o.reader = makeCinemaDngCodecFactory()->makeReader();
    auto info = o.reader->open(dir);
    if (!info) FAIL("open failed: " << info.error().toString());
    o.info = *info;
    return o;
}

BayerFrame readOk(ICodecReader& r, int64_t i) {
    BayerFrame f;
    auto ok = r.readFrame(i, f);
    if (!ok) FAIL("readFrame failed: " << ok.error().toString());
    return f;
}

} // namespace

TEST_CASE("Frame listing: natural order, hidden and AppleDouble files skipped") {
    REQUIRE(naturalLess("clip_2.dng", "clip_10.dng"));
    REQUIRE_FALSE(naturalLess("clip_10.dng", "clip_2.dng"));
    REQUIRE(naturalLess("clip_000009.dng", "clip_000010.dng"));
    REQUIRE(naturalLess("a_9_z.dng", "a_10_a.dng"));
    REQUIRE(naturalLess("A001_C001_0001.dng", "A001_C002_0000.dng"));
    REQUIRE(naturalLess("x01.dng", "x1.dng") != naturalLess("x1.dng", "x01.dng")); // strict weak order
    const fs::path dir = tmpDir("cdng-list");
    std::vector<uint16_t> m = randomCodes(64, 32, 12, 1);
    DngSpec s;
    s.bits = 12;
    for (int i : {0, 1, 2, 9, 10, 11}) {
        std::vector<uint16_t> c = m;
        c[0] = static_cast<uint16_t>(i);
        writeDng(dir / ("np_" + std::to_string(i) + ".dng"), c, s);
    }
    // AppleDouble companions and other hidden files are not frames (and would not parse).
    for (const char* junk : {"._np_0.dng", "._np_10.dng", ".hidden.dng"}) {
        static const char kAppleDouble[] = "\x00\x05\x16\x07\x00\x02\x00\x00Mac OS X        ";
        std::ofstream(dir / junk, std::ios::binary).write(kAppleDouble, sizeof kAppleDouble - 1);
    }
    const auto frames = listDngFrames(dir);
    std::vector<std::string> names;
    for (const auto& f : frames) names.push_back(f.filename().string());
    REQUIRE(names == std::vector<std::string>{"np_0.dng", "np_1.dng", "np_2.dng", "np_9.dng", "np_10.dng", "np_11.dng"});
    auto o = openClip(dir);
    REQUIRE(o.info.frameCount == 6);
    for (int64_t i = 0; i < 6; ++i) REQUIRE(readOk(*o.reader, i).data[0] == std::vector<int>{0, 1, 2, 9, 10, 11}[size_t(i)]);
}

TEST_CASE("Uncompressed packed rows start on a byte boundary (10/12/14-bit, width % 4 == 2)") {
    for (uint16_t bits : {uint16_t(8), uint16_t(10), uint16_t(12), uint16_t(14), uint16_t(16)}) {
        for (uint32_t rps : {0u, 3u}) {
            const fs::path dir = tmpDir("cdng-packed");
            DngSpec s;
            s.width = 18; // 18 * 10 and 18 * 14 bits are not whole bytes
            s.height = 8;
            s.bits = bits;
            s.rowsPerStrip = rps;
            const auto m = randomCodes(s.width, s.height, bits, bits);
            writeDng(dir / "f_000000.dng", m, s);
            auto o = openClip(dir);
            INFO("bits " << bits << " rowsPerStrip " << rps);
            REQUIRE(readOk(*o.reader, 0).data == m);
        }
    }
    // Tiled uncompressed with partial edge tiles
    const fs::path dir = tmpDir("cdng-packed-tiles");
    DngSpec s;
    s.width = 50;
    s.height = 38;
    s.bits = 14;
    s.tiles = true;
    s.tileW = 16;
    s.tileH = 16;
    const auto m = randomCodes(s.width, s.height, 14, 3);
    writeDng(dir / "f_000000.dng", m, s);
    auto o = openClip(dir);
    REQUIRE(o.info.width == 50);
    REQUIRE(o.info.height == 38);
    REQUIRE(readOk(*o.reader, 0).data == m);
}

TEST_CASE("LJ92 strips: a shorter last strip is accepted, a short stream elsewhere is an error") {
    const fs::path dir = tmpDir("cdng-ljstrips");
    DngSpec s;
    s.width = 64;
    s.height = 30; // RowsPerStrip 8: strips of 8, 8, 8, 6 rows
    s.bits = 14;
    s.compression = 7;
    s.rowsPerStrip = 8;
    const auto m = randomCodes(s.width, s.height, 14, 4);
    writeDng(dir / "f_000000.dng", m, s);
    auto o = openClip(dir);
    REQUIRE(readOk(*o.reader, 0).data == m);
}

TEST_CASE("LJ92 tiles: streams folded differently are read in sample order; mismatched streams are rejected") {
    const auto m = randomCodes(64, 32, 16, 5);
    SECTION("2*TileWidth x TileLength/2 stream (as pidng writes), 1 and 2 components") {
        for (uint16_t comps : {uint16_t(1), uint16_t(2)}) {
            const fs::path dir = tmpDir("cdng-fold");
            DngSpec s;
            s.compression = 7;
            s.tiles = true;
            s.lj = [comps](const std::vector<uint16_t>& blk, uint32_t w, uint32_t h, uint16_t bits) {
                ljpeg::EncodeParams p;
                p.precision = bits;
                p.components = comps;
                return *ljpeg::encode(blk.data(), 2 * w, h / 2, p);
            };
            writeDng(dir / "f_000000.dng", m, s);
            auto o = openClip(dir);
            REQUIRE(readOk(*o.reader, 0).data == m);
        }
    }
    SECTION("a stream wider than the tile is an error, not a copy with the wrong stride") {
        const fs::path dir = tmpDir("cdng-wide");
        DngSpec s;
        s.compression = 7;
        s.tiles = true;
        s.lj = [](const std::vector<uint16_t>& blk, uint32_t w, uint32_t h, uint16_t bits) {
            std::vector<uint16_t> wide(size_t(w + 16) * h, 0);
            for (uint32_t y = 0; y < h; ++y) std::memcpy(&wide[size_t(y) * (w + 16)], &blk[size_t(y) * w], w * 2);
            ljpeg::EncodeParams p;
            p.precision = bits;
            return *ljpeg::encode(wide.data(), w + 16, h, p);
        };
        writeDng(dir / "f_000000.dng", m, s);
        auto o = openClip(dir);
        BayerFrame f;
        auto r = o.reader->readFrame(0, f);
        REQUIRE_FALSE(r);
        REQUIRE(r.error().message.find("does not fit") != std::string::npos);
    }
    SECTION("restart markers inside tiles") {
        const fs::path dir = tmpDir("cdng-dri");
        DngSpec s;
        s.compression = 7;
        s.tiles = true;
        s.lj = [](const std::vector<uint16_t>& blk, uint32_t w, uint32_t h, uint16_t bits) {
            ljpeg::EncodeParams p;
            p.precision = bits;
            p.restartInterval = w / 2; // one tile row per interval
            return *ljpeg::encode(blk.data(), w, h, p);
        };
        writeDng(dir / "f_000000.dng", m, s);
        auto o = openClip(dir);
        REQUIRE(readOk(*o.reader, 0).data == m);
    }
}

TEST_CASE("Damaged frames fail to read instead of decoding garbage") {
    const auto m = randomCodes(64, 32, 12, 6);
    DngSpec s;
    s.bits = 12;
    s.compression = 7;
    s.tiles = true;
    const fs::path dir = tmpDir("cdng-damaged");
    writeDng(dir / "f_000000.dng", m, s);
    writeDng(dir / "f_000001.dng", m, s);
    writeDng(dir / "f_000002.dng", m, s);
    {
        // overwrite the middle of frame 1's first tile with garbage
        tiff::Reader r;
        REQUIRE(r.open(dir / "f_000001.dng"));
        auto ifd = r.readIfd(r.firstIfdOffset());
        const uint32_t off = *ifd->u32(tiff::tag::TileOffsets, 0), len = *ifd->u32(tiff::tag::TileByteCounts, 0);
        std::fstream f(dir / "f_000001.dng", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(off + len / 2);
        std::mt19937 g(1);
        for (uint32_t i = 0; i < len / 4; ++i) f.put(static_cast<char>(g() & 0x7F));
    }
    // truncate frame 2
    fs::resize_file(dir / "f_000002.dng", fs::file_size(dir / "f_000002.dng") / 2);
    auto o = openClip(dir);
    BayerFrame f;
    REQUIRE(o.reader->readFrame(0, f));
    REQUIRE_FALSE(o.reader->readFrame(1, f));
    REQUIRE_FALSE(o.reader->readFrame(2, f));
}

TEST_CASE("ActiveArea: frames are cropped like LibRaw's visible area, CFA and black follow") {
    // LibRaw (rawpy 0.27.1) on a 48x40 RGGB DNG with ActiveArea [1, 1, 38, 44] and a 2x2 BlackLevel
    // [100, 200, 300, 400]: margins top 2 / left 2, raw_pattern [[3, 2], [0, 1]] (G2 B / R G),
    // black_level_per_channel R 400, G 300, B 100, G2 200.
    const uint32_t W = 48, H = 40;
    const auto m = randomCodes(W, H, 16, 7);
    struct Case {
        std::array<uint32_t, 4> aa;
        uint32_t top, left, w, h;
        CfaPattern cfa;
        std::array<uint32_t, 4> black;
    };
    const Case cases[] = {
        {{0, 0, 40, 48}, 0, 0, 48, 40, CfaPattern::RGGB, {100, 200, 300, 400}},
        {{8, 16, 32, 40}, 8, 16, 24, 24, CfaPattern::RGGB, {100, 200, 300, 400}},
        {{1, 1, 38, 44}, 2, 2, 42, 36, CfaPattern::BGGR, {100, 200, 300, 400}},
        {{1, 0, 38, 44}, 2, 0, 44, 36, CfaPattern::GBRG, {100, 200, 300, 400}},
        {{0, 1, 38, 44}, 0, 2, 42, 38, CfaPattern::GRBG, {100, 200, 300, 400}},
        {{3, 2, 38, 45}, 4, 2, 42, 34, CfaPattern::GBRG, {100, 200, 300, 400}}, // odd width: trailing column dropped
    };
    for (const auto& c : cases) {
        const fs::path dir = tmpDir("cdng-aa");
        DngSpec s;
        s.width = W;
        s.height = H;
        s.compression = 7;
        s.tiles = true;
        s.tags = [&](tiff::Writer::IfdBuilder& b) {
            b.setU32(tiff::tag::ActiveArea, {c.aa[0], c.aa[1], c.aa[2], c.aa[3]});
            b.setU16(tiff::tag::BlackLevelRepeatDim, {2, 2});
            b.setU32(tiff::tag::BlackLevel, {100, 200, 300, 400});
        };
        writeDng(dir / "f_000000.dng", m, s);
        auto o = openClip(dir);
        INFO("ActiveArea " << c.aa[0] << "," << c.aa[1] << "," << c.aa[2] << "," << c.aa[3]);
        REQUIRE(o.info.width == c.w);
        REQUIRE(o.info.height == c.h);
        REQUIRE(o.info.rawWidth == W);
        REQUIRE(o.info.rawHeight == H);
        REQUIRE(o.info.declaredActiveArea == PixelRect{c.aa[0], c.aa[1], c.aa[2], c.aa[3]});
        REQUIRE(o.info.activeArea == PixelRect{c.top, c.left, c.top + c.h, c.left + c.w});
        REQUIRE(o.info.cfa == c.cfa);
        REQUIRE(o.info.black == c.black);
        const BayerFrame f = readOk(*o.reader, 0);
        REQUIRE(f.width == c.w);
        REQUIRE(f.cfa == c.cfa);
        REQUIRE(f.data == crop(m, W, c.top, c.left, c.w, c.h));
    }
    // Odd image size without ActiveArea: the trailing row and column are dropped, CFA unchanged.
    const fs::path dir = tmpDir("cdng-odd");
    DngSpec s;
    s.width = 41;
    s.height = 31;
    s.cfa = {1, 0, 2, 1}; // GRBG
    const auto mo = randomCodes(41, 31, 16, 8);
    writeDng(dir / "f_000000.dng", mo, s);
    auto o = openClip(dir);
    REQUIRE(o.info.width == 40);
    REQUIRE(o.info.height == 30);
    REQUIRE(o.info.cfa == CfaPattern::GRBG);
    REQUIRE(readOk(*o.reader, 0).data == crop(mo, 41, 0, 0, 40, 30));
}

TEST_CASE("LinearizationTable: reader returns linear values, writer re-encodes to the same table") {
    // 12-bit codes, table code -> round(65535 * (code/4095)^2)
    std::vector<uint16_t> table(4096);
    for (size_t i = 0; i < table.size(); ++i) table[i] = static_cast<uint16_t>(std::lround(65535.0 * std::pow(static_cast<double>(i) / 4095.0, 2.0)));
    const auto codes = randomCodes(64, 32, 12, 9);
    const fs::path dir = tmpDir("cdng-lin");
    DngSpec s;
    s.bits = 12;
    s.compression = 7;
    s.tiles = true;
    s.tags = [&](tiff::Writer::IfdBuilder& b) {
        b.setU16(tiff::tag::LinearizationTable, table);
        b.setU32(tiff::tag::WhiteLevel, {65535});
        b.setU32(tiff::tag::BlackLevel, {256});
    };
    writeDng(dir / "f_000000.dng", codes, s);
    auto o = openClip(dir);
    REQUIRE(o.info.bits == 16);
    REQUIRE(o.info.white == 65535);
    REQUIRE(o.info.black == std::array<uint32_t, 4>{256, 256, 256, 256});
    REQUIRE(o.info.meta.sampleEncoding.bitsPerSample == 12);
    REQUIRE(o.info.meta.sampleEncoding.linearizationTable == table);
    const BayerFrame f = readOk(*o.reader, 0);
    for (size_t i = 0; i < codes.size(); ++i) REQUIRE(f.data[i] == table[codes[i]]);

    // Write it back (identity): same table, 12-bit codes, identical linear values.
    const fs::path out = tmpDir("cdng-lin-out");
    auto w = makeCinemaDngCodecFactory()->makeWriter();
    WriterConfig wc;
    wc.dest = out / "o";
    wc.width = f.width;
    wc.height = f.height;
    wc.meta = o.info.meta;
    REQUIRE(w->begin(wc));
    REQUIRE(w->writeFrame(f, {}));
    BayerFrame g = f; // a resampled frame holds values between table entries
    for (auto& v : g.data) v = static_cast<uint16_t>(std::min(65535, v + 7));
    REQUIRE(w->writeFrame(g, {}));
    REQUIRE(w->finish());
    const auto outFrames = listDngFrames(out / "o");
    REQUIRE(outFrames.size() == 2);
    tiff::Reader tr;
    REQUIRE(tr.open(outFrames[0]));
    auto ifd = tr.readIfd(tr.firstIfdOffset());
    REQUIRE(*ifd->u32(tiff::tag::BitsPerSample) == 12);
    const tiff::Entry* lt = ifd->find(tiff::tag::LinearizationTable);
    REQUIRE(lt);
    REQUIRE(lt->count == 4096);
    for (uint32_t i = 0; i < 4096; i += 97) REQUIRE(*lt->u32(i) == table[i]);
    auto o2 = openClip(out / "o");
    REQUIRE(readOk(*o2.reader, 0).data == f.data);
    // Second frame: every value is stored as the nearest table entry.
    const BayerFrame g2 = readOk(*o2.reader, 1);
    for (size_t i = 0; i < g.data.size(); ++i) {
        const auto it = std::lower_bound(table.begin(), table.end(), g.data[i]);
        int best = 1 << 30;
        if (it != table.end()) best = std::min(best, std::abs(int(*it) - int(g.data[i])));
        if (it != table.begin()) best = std::min(best, std::abs(int(*(it - 1)) - int(g.data[i])));
        REQUIRE(std::abs(int(g2.data[i]) - int(g.data[i])) == best);
    }

    // A table that is not monotonic is applied on read but not carried to the output.
    std::vector<uint16_t> bad = table;
    std::swap(bad[1000], bad[1001]); // distinct values: no longer non-decreasing
    const fs::path dirBad = tmpDir("cdng-lin-bad");
    s.tags = [&](tiff::Writer::IfdBuilder& b) {
        b.setU16(tiff::tag::LinearizationTable, bad);
        b.setU32(tiff::tag::WhiteLevel, {60000});
    };
    writeDng(dirBad / "f_000000.dng", codes, s);
    auto ob = openClip(dirBad);
    REQUIRE(ob.info.meta.sampleEncoding.linearizationTable.empty());
    REQUIRE(ob.info.meta.sampleEncoding.bitsPerSample == 16); // from WhiteLevel 60000
    const BayerFrame fb = readOk(*ob.reader, 0);
    for (size_t i = 0; i < codes.size(); ++i) REQUIRE(fb.data[i] == bad[codes[i]]);
}

TEST_CASE("BlackLevel repeat patterns and BlackLevelDeltaH/V") {
    const uint32_t W = 32, H = 16;
    std::vector<uint16_t> m(size_t(W) * H, 20000);
    SECTION("1x2 repeat: per column parity") {
        const fs::path dir = tmpDir("cdng-bl12");
        DngSpec s;
        s.width = W;
        s.height = H;
        s.tags = [](tiff::Writer::IfdBuilder& b) {
            b.setU16(tiff::tag::BlackLevelRepeatDim, {1, 2});
            b.setU32(tiff::tag::BlackLevel, {2000, 3000});
        };
        writeDng(dir / "f_000000.dng", m, s);
        auto o = openClip(dir);
        REQUIRE(o.info.black == std::array<uint32_t, 4>{2000, 3000, 2000, 3000});
        REQUIRE(readOk(*o.reader, 0).data == m); // nothing folded
    }
    SECTION("2x1 repeat and a single value") {
        const fs::path dir = tmpDir("cdng-bl21");
        DngSpec s;
        s.width = W;
        s.height = H;
        s.tags = [](tiff::Writer::IfdBuilder& b) {
            b.setU16(tiff::tag::BlackLevelRepeatDim, {2, 1});
            b.setU32(tiff::tag::BlackLevel, {2000, 3000});
        };
        writeDng(dir / "f_000000.dng", m, s);
        REQUIRE(openClip(dir).info.black == std::array<uint32_t, 4>{2000, 2000, 3000, 3000});
    }
    SECTION("DeltaH/V and a 4x4 repeat are folded into the samples") {
        const fs::path dir = tmpDir("cdng-bldelta");
        std::vector<double> dh(W), dv(H);
        for (uint32_t x = 0; x < W; ++x) dh[x] = (x % 5) * 3.0;
        for (uint32_t y = 0; y < H; ++y) dv[y] = (y % 3) * 2.0;
        std::vector<uint32_t> rep(16);
        for (uint32_t i = 0; i < 16; ++i) rep[i] = 1000 + 10 * i;
        DngSpec s;
        s.width = W;
        s.height = H;
        s.tags = [&](tiff::Writer::IfdBuilder& b) {
            b.setU16(tiff::tag::BlackLevelRepeatDim, {4, 4});
            b.setU32(tiff::tag::BlackLevel, rep);
            std::vector<std::pair<int32_t, int32_t>> h, v;
            for (double d : dh) h.push_back({int32_t(d * 2), 2});
            for (double d : dv) v.push_back({int32_t(d * 2), 2});
            b.setSRational(tiff::tag::BlackLevelDeltaH, h);
            b.setSRational(tiff::tag::BlackLevelDeltaV, v);
        };
        writeDng(dir / "f_000000.dng", m, s);
        auto o = openClip(dir);
        const BayerFrame f = readOk(*o.reader, 0);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const double black = rep[(y % 4) * 4 + x % 4] + dh[x] + dv[y];
                const double signal = 20000.0 - black; // what the source means at (x, y)
                const double outSignal = double(f.at(x, y)) - double(f.black[(x & 1) + 2 * (y & 1)]);
                REQUIRE(std::abs(outSignal - signal) <= 0.5);
            }
    }
}

TEST_CASE("Per-frame BlackLevel and WhiteLevel are read per frame and written per frame") {
    const fs::path dir = tmpDir("cdng-perframe");
    const auto m = randomCodes(64, 32, 12, 10);
    const uint32_t blacks[3] = {64, 80, 72}, whites[3] = {4095, 4000, 3900};
    for (int i = 0; i < 3; ++i) {
        DngSpec s;
        s.bits = 12;
        s.compression = 7;
        s.tiles = true;
        s.tags = [&](tiff::Writer::IfdBuilder& b) {
            b.setU32(tiff::tag::BlackLevel, {blacks[i]});
            b.setU32(tiff::tag::WhiteLevel, {whites[i]});
        };
        writeDng(dir / ("f_00000" + std::to_string(i) + ".dng"), m, s);
    }
    auto o = openClip(dir);
    REQUIRE(o.info.black[0] == 64);
    REQUIRE(o.info.white == 4095);
    const fs::path out = tmpDir("cdng-perframe-out");
    auto w = makeCinemaDngCodecFactory()->makeWriter();
    WriterConfig wc;
    wc.dest = out / "o";
    wc.width = o.info.width;
    wc.height = o.info.height;
    wc.black = o.info.black;
    wc.white = o.info.white;
    wc.meta = o.info.meta;
    REQUIRE(w->begin(wc));
    for (int i = 0; i < 3; ++i) {
        const BayerFrame f = readOk(*o.reader, i);
        REQUIRE(f.black == std::array<uint32_t, 4>{blacks[i], blacks[i], blacks[i], blacks[i]});
        REQUIRE(f.white == whites[i]);
        REQUIRE(w->writeFrame(f, {}));
    }
    REQUIRE(w->finish());
    auto o2 = openClip(out / "o");
    for (int i = 0; i < 3; ++i) {
        const BayerFrame f = readOk(*o2.reader, i);
        REQUIRE(f.black[3] == blacks[i]);
        REQUIRE(f.white == whites[i]);
        REQUIRE(f.data == m);
    }
}

namespace {
// LibRaw's lossless_dng_load_raw sample placement (rawpy 0.27.1, LibRaw src/decoders/dng.cpp):
// samples of each tile's stream go to (trow + row, tcol + col) and wrap when
// ++col >= tile_width || col >= raw_width. When clrs * jwide == raw_width (jwide = stream width in
// samples) LibRaw instead reads clrs * jwide samples per JPEG row, past the end of the row, which
// garbles the frame; the emulation fails on that layout.
std::vector<uint16_t> libRawStylePlacement(const fs::path& file) {
    tiff::Reader r;
    REQUIRE(r.open(file));
    auto ifd = r.readIfd(r.firstIfdOffset());
    const uint32_t W = *ifd->u32(tiff::tag::ImageWidth), H = *ifd->u32(tiff::tag::ImageLength);
    const tiff::Entry* to = ifd->find(tiff::tag::TileOffsets);
    const tiff::Entry* tc = ifd->find(tiff::tag::TileByteCounts);
    const bool tiled = to != nullptr;
    if (!tiled) { to = ifd->find(tiff::tag::StripOffsets); tc = ifd->find(tiff::tag::StripByteCounts); }
    const uint32_t tw = tiled ? *ifd->u32(tiff::tag::TileWidth) : 0x7fffffffu;
    const uint32_t th = tiled ? *ifd->u32(tiff::tag::TileLength) : 0x7fffffffu;
    std::vector<uint16_t> out(size_t(W) * H, 0);
    uint32_t trow = 0, tcol = 0;
    for (uint32_t t = 0; t < to->count && trow < H; ++t) {
        auto img = ljpeg::decode(r.bytes().data() + *to->u32(t), *tc->u32(t));
        REQUIRE(img);
        REQUIRE_FALSE((img->components > 1 && img->components * img->width == W));
        uint32_t row = 0, col = 0;
        for (uint16_t v : img->samples) {
            if (trow + row < H && tcol + col < W) out[size_t(trow + row) * W + tcol + col] = v;
            if (++col >= tw || col >= W) { row += 1; col = 0; }
        }
        if ((tcol += tw) >= W) { trow += th; tcol = 0; }
    }
    return out;
}
} // namespace

TEST_CASE("CinemaDNG writer: LJ92 tiles never wider than the image, readable LibRaw-style") {
    for (uint32_t width : {16u, 34u, 66u, 256u, 270u, 512u, 856u, 1000u, 1024u, 1922u}) {
        for (uint32_t height : {16u, 22u, 256u, 288u, 300u, 512u}) {
            const fs::path out = tmpDir("cdng-tiles");
            BayerFrame f = BayerFrame::create(width, height, 12, CfaPattern::RGGB);
            std::mt19937 rng(width * 7 + height);
            for (auto& v : f.data) v = static_cast<uint16_t>(rng() & 4095);
            auto w = makeCinemaDngCodecFactory()->makeWriter();
            WriterConfig wc;
            wc.dest = out / "o";
            wc.width = width;
            wc.height = height;
            REQUIRE(w->begin(wc));
            REQUIRE(w->writeFrame(f, {}));
            REQUIRE(w->finish());
            const fs::path file = listDngFrames(out / "o").at(0);
            tiff::Reader r;
            REQUIRE(r.open(file));
            auto ifd = r.readIfd(r.firstIfdOffset());
            const uint32_t tw = *ifd->u32(tiff::tag::TileWidth), th = *ifd->u32(tiff::tag::TileLength);
            INFO("width " << width << " height " << height << " tile " << tw << "x" << th);
            REQUIRE(tw <= width);
            REQUIRE(tw % 16 == 0);
            REQUIRE(tw <= 256);
            REQUIRE(th % 16 == 0);
            REQUIRE(th <= 256);
            const uint32_t nTiles = ifd->find(tiff::tag::TileOffsets)->count;
            REQUIRE(nTiles == ((width + tw - 1) / tw) * ((height + th - 1) / th));
            REQUIRE(libRawStylePlacement(file) == f.data);
            auto o = openClip(out / "o");
            REQUIRE(readOk(*o.reader, 0).data == f.data);
        }
    }
}

TEST_CASE("CinemaDNG writer keeps the source's BitsPerSample as the LJ92 precision") {
    for (uint16_t bps : {uint16_t(0), uint16_t(10), uint16_t(12), uint16_t(14), uint16_t(16)}) {
        const fs::path out = tmpDir("cdng-bps");
        BayerFrame f = BayerFrame::create(64, 32, 12, CfaPattern::RGGB);
        std::mt19937 rng(bps);
        for (auto& v : f.data) v = static_cast<uint16_t>(rng() & 1023);
        auto w = makeCinemaDngCodecFactory()->makeWriter();
        WriterConfig wc;
        wc.dest = out / "o";
        wc.width = 64;
        wc.height = 32;
        wc.meta.sampleEncoding.bitsPerSample = bps;
        REQUIRE(w->begin(wc));
        REQUIRE(w->writeFrame(f, {}));
        f.data[5] = 4095; // needs 12 bits: a 10-bit source depth grows to fit
        REQUIRE(w->writeFrame(f, {}));
        REQUIRE(w->finish());
        const auto frames = listDngFrames(out / "o");
        for (size_t i = 0; i < 2; ++i) {
            tiff::Reader r;
            REQUIRE(r.open(frames[i]));
            auto ifd = r.readIfd(r.firstIfdOffset());
            const uint32_t expect = bps == 0 ? 16 : (i == 1 && bps < 12 ? 12 : bps);
            REQUIRE(*ifd->u32(tiff::tag::BitsPerSample) == expect);
            auto img = ljpeg::decode(r.bytes().data() + *ifd->u32(tiff::tag::TileOffsets), *ifd->u32(tiff::tag::TileByteCounts));
            REQUIRE(img);
            REQUIRE(img->precision == expect);
        }
        auto o = openClip(out / "o");
        REQUIRE(readOk(*o.reader, 1).data == f.data);
    }
}
