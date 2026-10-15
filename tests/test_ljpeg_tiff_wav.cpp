#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include "r3dss/codec_cdng/ljpeg.hpp"
#include "r3dss/codec_cdng/tiff.hpp"
#include "r3dss/codec_cdng/wav.hpp"
#include "r3dss/synth/synth.hpp"

using namespace r3dss;
using namespace r3dss::codec;
namespace fs = std::filesystem;

namespace {
fs::path tmpDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("r3dss-test-" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
} // namespace

TEST_CASE("Lossless JPEG round trip, 16-bit container, 1 to 4 components, all predictors") {
    std::mt19937 rng(7);
    for (uint16_t comps : {uint16_t(1), uint16_t(2), uint16_t(4)}) {
        for (uint8_t pred = 1; pred <= 7; ++pred) {
            const uint32_t w = 64, h = 17;
            std::vector<uint16_t> s(w * h);
            for (auto& v : s) v = static_cast<uint16_t>(rng() & 0xFFFF);
            ljpeg::EncodeParams p;
            p.components = comps;
            p.predictor = pred;
            auto enc = ljpeg::encode(s.data(), w, h, p);
            REQUIRE(enc);
            auto dec = ljpeg::decode(enc->data(), enc->size());
            REQUIRE(dec);
            REQUIRE(dec->width == w);
            REQUIRE(dec->height == h);
            REQUIRE(dec->components == comps);
            REQUIRE(dec->samples == s);
        }
    }
}

TEST_CASE("Lossless JPEG 12-bit precision and extreme values") {
    const uint32_t w = 48, h = 8;
    std::vector<uint16_t> s(w * h);
    for (size_t i = 0; i < s.size(); ++i) s[i] = (i % 3 == 0) ? 4095 : (i % 3 == 1 ? 0 : 2048);
    ljpeg::EncodeParams p;
    p.precision = 12;
    p.components = 2;
    auto enc = ljpeg::encode(s.data(), w, h, p);
    REQUIRE(enc);
    auto dec = ljpeg::decode(enc->data(), enc->size());
    REQUIRE(dec);
    REQUIRE(dec->samples == s);
    // 16-bit extremes incl. the +32768 special category
    std::vector<uint16_t> e(w * h);
    for (size_t i = 0; i < e.size(); ++i) e[i] = (i & 1) ? 65535 : 0;
    e[5] = 32768;
    ljpeg::EncodeParams p16;
    p16.components = 1;
    auto enc16 = ljpeg::encode(e.data(), w, h, p16);
    REQUIRE(enc16);
    auto dec16 = ljpeg::decode(enc16->data(), enc16->size());
    REQUIRE(dec16);
    REQUIRE(dec16->samples == e);
}

TEST_CASE("Lossless JPEG round trip with restart intervals (row aligned and mid-row), all predictors") {
    std::mt19937 rng(11);
    for (uint16_t comps : {uint16_t(1), uint16_t(2), uint16_t(4)}) {
        for (uint8_t pred = 1; pred <= 7; ++pred) {
            for (uint32_t ri : {1u, 3u, 8u, 16u, 37u}) {
                const uint32_t w = 32, h = 9;
                std::vector<uint16_t> s(w * h);
                for (auto& v : s) v = static_cast<uint16_t>(rng() & 0x3FFF);
                ljpeg::EncodeParams p;
                p.components = comps;
                p.predictor = pred;
                p.precision = 14;
                p.restartInterval = ri;
                auto enc = ljpeg::encode(s.data(), w, h, p);
                REQUIRE(enc);
                // DRI segment present and RSTn markers in the entropy data
                bool dri = false, rst = false;
                for (size_t i = 0; i + 1 < enc->size(); ++i) {
                    if ((*enc)[i] != 0xFF) continue;
                    dri |= (*enc)[i + 1] == 0xDD;
                    rst |= (*enc)[i + 1] >= 0xD0 && (*enc)[i + 1] <= 0xD7;
                }
                REQUIRE(dri);
                REQUIRE(rst);
                auto dec = ljpeg::decode(enc->data(), enc->size());
                INFO("comps " << comps << " predictor " << int(pred) << " interval " << ri);
                REQUIRE(dec);
                REQUIRE(dec->samples == s);
            }
        }
    }
}

namespace {
// Minimal independent LJ92 stream builder for decoder tests: 1 component, fixed Huffman table in
// which category k (0..8) has the 4-bit code k. The caller supplies the differences, i.e. the
// predictions are spelled out by the test, not computed by the codec under test.
struct TinyStream {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int nbits = 0;
    void bits(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1u);
            if (++nbits == 8) {
                out.push_back(static_cast<uint8_t>(acc));
                if (acc == 0xFF) out.push_back(0);
                acc = 0;
                nbits = 0;
            }
        }
    }
    void flush() {
        while (nbits) bits(1, 1);
    }
    void diff(int d) {
        int cat = 0;
        for (int a = d < 0 ? -d : d; a; a >>= 1) ++cat;
        bits(static_cast<uint32_t>(cat), 4);
        if (cat) bits(static_cast<uint32_t>(d < 0 ? d - 1 : d) & ((1u << cat) - 1), cat);
    }
    void header(int precision, uint32_t w, uint32_t h, uint32_t restart, int predictor) {
        out = {0xFF, 0xD8, 0xFF, 0xC3, 0, 11, static_cast<uint8_t>(precision), static_cast<uint8_t>(h >> 8),
               static_cast<uint8_t>(h), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w), 1, 1, 0x11, 0};
        // DHT: 9 codes of length 4 for symbols 0..8
        const uint8_t dht[] = {0xFF, 0xC4, 0, 2 + 1 + 16 + 9, 0x00, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 1, 2, 3, 4, 5, 6, 7, 8};
        out.insert(out.end(), std::begin(dht), std::end(dht));
        if (restart) {
            const uint8_t dri[] = {0xFF, 0xDD, 0, 4, static_cast<uint8_t>(restart >> 8), static_cast<uint8_t>(restart)};
            out.insert(out.end(), std::begin(dri), std::end(dri));
        }
        const uint8_t sos[] = {0xFF, 0xDA, 0, 8, 1, 1, 0x00, static_cast<uint8_t>(predictor), 0, 0};
        out.insert(out.end(), std::begin(sos), std::end(sos));
    }
    void marker(uint8_t m) {
        flush();
        out.push_back(0xFF);
        out.push_back(m);
    }
};
} // namespace

TEST_CASE("Lossless JPEG restart: first sample of an interval uses 2^(P-1), rest of that line uses Ra (T.81 H.1.2.1)") {
    // 3x2 image, 8-bit, restart every 2 MCUs, so intervals start at (0,0), (2,0) and (1,1).
    //   row0: 10 20 30     row1: 40 50 60
    TinyStream t;
    t.header(8, 3, 2, 2, 1);
    t.diff(10 - 128); // (0,0) first sample of the scan: 2^(P-1)
    t.diff(20 - 10);  // (1,0) first line: Ra
    t.marker(0xD0);
    t.diff(30 - 128); // (2,0) first sample of an interval: 2^(P-1), although mid-row
    t.diff(40 - 10);  // (0,1) first sample of a line: Rb
    t.marker(0xD1);
    t.diff(50 - 128); // (1,1) first sample of an interval
    t.diff(60 - 50);  // (2,1) rest of the interval's first line: Ra
    t.marker(0xD9);
    auto dec = ljpeg::decode(t.out.data(), t.out.size());
    REQUIRE(dec);
    REQUIRE(dec->samples == std::vector<uint16_t>{10, 20, 30, 40, 50, 60});

    // Predictor 6 must not be used on the first line of an interval that starts at a line start.
    //   4x3 image, restart every 4 MCUs (one line): every line is a "first line".
    TinyStream u;
    u.header(8, 4, 3, 4, 6);
    const int rows[3][4] = {{100, 104, 108, 112}, {50, 60, 70, 80}, {200, 190, 180, 170}};
    for (int y = 0; y < 3; ++y) {
        if (y) u.marker(static_cast<uint8_t>(0xD0 + y - 1));
        u.diff(rows[y][0] - 128);
        for (int x = 1; x < 4; ++x) u.diff(rows[y][x] - rows[y][x - 1]);
    }
    u.marker(0xD9);
    auto dec2 = ljpeg::decode(u.out.data(), u.out.size());
    REQUIRE(dec2);
    REQUIRE(dec2->samples == std::vector<uint16_t>{100, 104, 108, 112, 50, 60, 70, 80, 200, 190, 180, 170});
}

TEST_CASE("Lossless JPEG decoder is strict: truncated, surplus, garbage and out-of-sequence data are errors") {
    std::mt19937 rng(5);
    const uint32_t w = 64, h = 16;
    std::vector<uint16_t> s(w * h);
    for (auto& v : s) v = static_cast<uint16_t>(rng() & 0x0FFF);
    ljpeg::EncodeParams p;
    p.precision = 12;
    auto enc = ljpeg::encode(s.data(), w, h, p);
    REQUIRE(enc);
    const std::vector<uint8_t> good = *enc;
    REQUIRE(good[good.size() - 2] == 0xFF);
    REQUIRE(good[good.size() - 1] == 0xD9);
    REQUIRE(ljpeg::decode(good.data(), good.size()));

    // Ending exactly at the end of the byte count (no EOI) is accepted, as is padding after EOI.
    REQUIRE(ljpeg::decode(good.data(), good.size() - 2));
    std::vector<uint8_t> padded = good;
    padded.push_back(0);
    REQUIRE(ljpeg::decode(padded.data(), padded.size()));

    // Truncated entropy data (with or without EOI)
    for (size_t cut : {3u, 10u, 100u}) {
        std::vector<uint8_t> t(good.begin(), good.end() - static_cast<std::ptrdiff_t>(cut));
        INFO("cut " << cut);
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
        t.push_back(0xFF);
        t.push_back(0xD9);
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
    }
    // Surplus data between the last sample and EOI
    {
        std::vector<uint8_t> t(good.begin(), good.end() - 2);
        for (int i = 0; i < 4; ++i) t.push_back(0x5A);
        t.push_back(0xFF);
        t.push_back(0xD9);
        auto r = ljpeg::decode(t.data(), t.size());
        REQUIRE_FALSE(r);
    }
    // Garbage instead of EOI, and a marker other than EOI
    {
        std::vector<uint8_t> t = good;
        t[t.size() - 1] = 0xC4;
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
        t[t.size() - 2] = 0x12;
        t[t.size() - 1] = 0x34;
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
    }
    // Overwriting the second half of the entropy data with garbage is detected.
    {
        std::vector<uint8_t> t = good;
        std::mt19937 g(9);
        for (size_t i = t.size() / 2; i < t.size() - 2; ++i) {
            t[i] = static_cast<uint8_t>(g());
            if (t[i] == 0xFF) t[i] = 0x7F;
        }
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
    }
    // 12-bit stream whose reconstruction leaves the 12-bit range
    {
        TinyStream t;
        t.header(12, 2, 1, 0, 1);
        t.diff(0);   // 2048
        t.diff(255); // 2303
        t.marker(0xD9);
        REQUIRE(ljpeg::decode(t.out.data(), t.out.size()));
        TinyStream bad;
        bad.header(8, 2, 1, 0, 1);
        bad.diff(127); // 255
        bad.diff(1);   // 256 > 8-bit range
        bad.marker(0xD9);
        REQUIRE_FALSE(ljpeg::decode(bad.out.data(), bad.out.size()));
    }
    // Restart markers: missing or out of sequence
    {
        TinyStream t;
        t.header(8, 2, 2, 2, 1);
        t.diff(1);
        t.diff(1);
        t.marker(0xD1); // expected RST0
        t.diff(1);
        t.diff(1);
        t.marker(0xD9);
        REQUIRE_FALSE(ljpeg::decode(t.out.data(), t.out.size()));
        TinyStream m;
        m.header(8, 2, 2, 2, 1);
        for (int i = 0; i < 4; ++i) m.diff(1); // no marker after 2 MCUs
        m.marker(0xD9);
        REQUIRE_FALSE(ljpeg::decode(m.out.data(), m.out.size()));
    }
    // Lossy (SOF0) streams are rejected, not decoded as lossless
    {
        std::vector<uint8_t> t = good;
        t[3] = 0xC0;
        REQUIRE_FALSE(ljpeg::decode(t.data(), t.size()));
    }
}

TEST_CASE("Lossless JPEG encoder codes differences without wrapping below 16-bit precision") {
    // A jump from 0 to 4095 must be coded as +4095 (category 12), which a standard decoder
    // reconstructs modulo 2^16; a modulo-2^12 shortcut (-1) would reconstruct 65535.
    std::vector<uint16_t> s = {0, 4095, 4095, 0, 0, 4095, 2048, 1};
    ljpeg::EncodeParams p;
    p.precision = 12;
    p.components = 1;
    auto enc = ljpeg::encode(s.data(), 8, 1, p);
    REQUIRE(enc);
    auto dec = ljpeg::decode(enc->data(), enc->size());
    REQUIRE(dec);
    REQUIRE(dec->samples == s);
    // Samples above the precision are rejected.
    std::vector<uint16_t> big = {0, 4096};
    REQUIRE_FALSE(ljpeg::encode(big.data(), 2, 1, p));
}

TEST_CASE("Lossless JPEG compresses a smooth synthetic frame") {
    synth::SynthParams sp;
    sp.width = 512; sp.height = 288; sp.pattern = synth::Pattern::Scene;
    BayerFrame f = synth::makeFrame(sp);
    ljpeg::EncodeParams p;
    p.components = 2;
    auto enc = ljpeg::encode(f.data.data(), f.width, f.height, p);
    REQUIRE(enc);
    // 12-bit content with mild noise codes at ~9 bits/sample in the 16-bit container.
    REQUIRE(enc->size() < f.byteSize() * 0.65);
}

TEST_CASE("TIFF writer output is readable by the TIFF reader") {
    const fs::path dir = tmpDir("tiff");
    tiff::Writer w;
    std::vector<uint8_t> payload(1000, 0xAB);
    const uint64_t off = w.appendData(payload);
    tiff::Writer::IfdBuilder exif, main;
    exif.setU16(tiff::tag::ISOSpeedRatings, {800});
    const uint64_t exifOff = w.writeIfd(exif);
    main.setU32(tiff::tag::ImageWidth, {640});
    main.setU32(tiff::tag::ImageLength, {480});
    main.setAscii(tiff::tag::Make, "Test");
    main.setRational(tiff::tag::ExposureTime, {{1, 50}});
    main.setSRational(tiff::tag::ColorMatrix1, {{-5, 10}});
    main.setU32(tiff::tag::StripOffsets, {static_cast<uint32_t>(off)});
    main.setU32(tiff::tag::ExifIFD, {static_cast<uint32_t>(exifOff)});
    w.setFirstIfd(w.writeIfd(main));
    REQUIRE(w.save(dir / "t.tif"));
    tiff::Reader r;
    REQUIRE(r.open(dir / "t.tif"));
    auto ifd = r.readIfd(r.firstIfdOffset());
    REQUIRE(ifd);
    REQUIRE(*ifd->u32(tiff::tag::ImageWidth) == 640);
    REQUIRE(ifd->ascii(tiff::tag::Make) == "Test");
    REQUIRE(*ifd->real(tiff::tag::ExposureTime) == 0.02);
    REQUIRE(*ifd->real(tiff::tag::ColorMatrix1) == -0.5);
    auto exifR = r.readIfd(*ifd->u32(tiff::tag::ExifIFD));
    REQUIRE(exifR);
    REQUIRE(*exifR->u32(tiff::tag::ISOSpeedRatings) == 800);
    REQUIRE(r.bytes()[off] == 0xAB);
}

TEST_CASE("WAV stream writer and reader round trip float32 and pcm24") {
    const fs::path dir = tmpDir("wav");
    for (auto fmt : {SampleFormat::Float32, SampleFormat::PCM24, SampleFormat::PCM16}) {
        AudioInfo info;
        info.channels = fmt == SampleFormat::PCM16 ? 2 : 4;
        info.sampleRate = 48000;
        info.format = fmt;
        AudioBlock b;
        b.info = info;
        b.sampleCount = 4801;
        b.bytes = synth::makeAudio(info, b.sampleCount, 3);
        const fs::path p = dir / (std::string(sampleFormatName(fmt)) + ".wav");
        wav::StreamWriter w;
        REQUIRE(w.begin(p, info));
        REQUIRE(w.write(b));
        REQUIRE(w.finish());
        auto wi = wav::probe(p);
        REQUIRE(wi);
        REQUIRE(wi->audio.channels == info.channels);
        REQUIRE(wi->audio.format == fmt);
        REQUIRE(wi->audio.sampleCount == b.sampleCount);
        auto rd = wav::read(p, *wi, 0, 100000);
        REQUIRE(rd);
        REQUIRE(rd->sampleCount == b.sampleCount);
        REQUIRE(rd->bytes == b.bytes);
        auto part = wav::read(p, *wi, 4800, 10);
        REQUIRE(part->sampleCount == 1);
    }
}
