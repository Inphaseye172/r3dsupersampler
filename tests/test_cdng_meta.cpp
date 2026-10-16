// CinemaDNG metadata and audio carried from source to output (camera, colour, framing, opcodes,
// timecode, sidecar WAVs), including the preview-IFD0 + raw-SubIFD layout.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/codec_cdng/tiff.hpp"
#include "r3dss/pipeline/clip_converter.hpp"

using namespace r3dss;
using namespace r3dss::codec;
using namespace r3dss::pipeline;
namespace fs = std::filesystem;

namespace {

struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} const init;

fs::path tmpDir(const std::string& name) {
    static const std::string run = std::to_string(std::random_device{}());
    fs::path p = fs::temp_directory_path() / ("r3dss-meta-" + name + "-" + run);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::vector<uint8_t> be32s(std::initializer_list<uint32_t> v) {
    std::vector<uint8_t> b;
    for (uint32_t x : v) {
        b.push_back(uint8_t(x >> 24)); b.push_back(uint8_t(x >> 16)); b.push_back(uint8_t(x >> 8)); b.push_back(uint8_t(x));
    }
    return b;
}

// One opcode list holding a single opcode.
std::vector<uint8_t> opcodeList(uint32_t id, const std::vector<uint8_t>& params, uint32_t flags = 0) {
    std::vector<uint8_t> b = be32s({1, id, 0x01030000u, flags, static_cast<uint32_t>(params.size())});
    b.insert(b.end(), params.begin(), params.end());
    return b;
}

constexpr uint32_t W = 64, H = 32;

// A frame in the layout Adobe DNG Converter and many cameras use: IFD0 is a small preview
// (NewSubFileType 1) holding the camera tags, the CFA raw image is a SubIFD.
void writeSubIfdFrame(const fs::path& p, int frame, uint16_t value, uint32_t badRow, uint32_t badCol) {
    tiff::Writer w;
    std::vector<uint8_t> raw(W * H * 2);
    for (uint32_t i = 0; i < W * H; ++i) {
        const uint16_t v = (i / W == badRow && i % W == badCol) ? uint16_t(60000) : value;
        raw[2 * i] = uint8_t(v);
        raw[2 * i + 1] = uint8_t(v >> 8);
    }
    const uint64_t rawOff = w.appendData(raw);
    const std::vector<uint8_t> preview(8 * 4 * 3, 128);
    const uint64_t prevOff = w.appendData(preview);

    tiff::Writer::IfdBuilder sub;
    sub.setU32(tiff::tag::NewSubFileType, {0});
    sub.setU32(tiff::tag::ImageWidth, {W});
    sub.setU32(tiff::tag::ImageLength, {H});
    sub.setU16(tiff::tag::BitsPerSample, {16});
    sub.setU16(tiff::tag::Compression, {1});
    sub.setU16(tiff::tag::Photometric, {32803});
    sub.setU16(tiff::tag::SamplesPerPixel, {1});
    sub.setU32(tiff::tag::StripOffsets, {static_cast<uint32_t>(rawOff)});
    sub.setU32(tiff::tag::RowsPerStrip, {H});
    sub.setU32(tiff::tag::StripByteCounts, {static_cast<uint32_t>(raw.size())});
    sub.setU16(tiff::tag::CFARepeatPatternDim, {2, 2});
    sub.setU8(tiff::tag::CFAPattern, {0, 1, 1, 2});
    sub.setU32(tiff::tag::WhiteLevel, {65535});
    sub.setRational(tiff::tag::DefaultCropOrigin, {{4, 1}, {2, 1}});
    sub.setRational(tiff::tag::DefaultCropSize, {{56, 1}, {28, 1}});
    sub.setRational(tiff::tag::DefaultScale, {{2, 1}, {1, 1}});
    sub.setU8(tiff::tag::OpcodeList1, opcodeList(5, be32s({0, 1, 0, badRow, badCol})), tiff::Type::Undefined);
    // GainMap on CFA phase (0,1) over the whole frame, and a normalised vignette correction.
    std::vector<uint8_t> gm = be32s({0, 1, H, W, 0, 1, 2, 2, 1, 1});
    gm.resize(gm.size() + 32, 0); // spacing / origin doubles
    const std::vector<uint8_t> mp = be32s({1, 0x3F800000u});
    gm.insert(gm.end(), mp.begin(), mp.end());
    sub.setU8(tiff::tag::OpcodeList2, opcodeList(9, gm), tiff::Type::Undefined);
    std::vector<uint8_t> vig = be32s({1});
    vig.resize(vig.size() + 2 * 8 + 2 * 8, 0);
    sub.setU8(tiff::tag::OpcodeList3, opcodeList(3, vig), tiff::Type::Undefined);
    const uint64_t subOff = w.writeIfd(sub);

    tiff::Writer::IfdBuilder ifd0;
    ifd0.setU32(tiff::tag::NewSubFileType, {1});
    ifd0.setU32(tiff::tag::ImageWidth, {8});
    ifd0.setU32(tiff::tag::ImageLength, {4});
    ifd0.setU16(tiff::tag::BitsPerSample, {8, 8, 8});
    ifd0.setU16(tiff::tag::Compression, {1});
    ifd0.setU16(tiff::tag::Photometric, {2});
    ifd0.setU16(tiff::tag::SamplesPerPixel, {3});
    ifd0.setU32(tiff::tag::StripOffsets, {static_cast<uint32_t>(prevOff)});
    ifd0.setU32(tiff::tag::RowsPerStrip, {4});
    ifd0.setU32(tiff::tag::StripByteCounts, {static_cast<uint32_t>(preview.size())});
    ifd0.setU32(tiff::tag::SubIFDs, {static_cast<uint32_t>(subOff)});
    ifd0.setAscii(tiff::tag::Make, "TestCam");
    ifd0.setAscii(tiff::tag::Model, "Model X");
    ifd0.setAscii(tiff::tag::UniqueCameraModel, "TestCam Model X v2");
    ifd0.setU16(tiff::tag::Orientation, {3});
    ifd0.setU8(tiff::tag::DNGVersion, {1, 4, 0, 0});
    ifd0.setSRational(tiff::tag::ColorMatrix1, {{7, 10}, {-2, 10}, {0, 1}, {-4, 10}, {12, 10}, {2, 10}, {0, 1}, {1, 10}, {6, 10}});
    ifd0.setU16(tiff::tag::CalibrationIlluminant1, {21});
    ifd0.setSRational(50964 /*ForwardMatrix1*/, {{6, 10}, {25, 100}, {1, 10}, {25, 100}, {8, 10}, {-5, 100}, {0, 1}, {5, 100}, {9, 10}});
    ifd0.setSRational(tiff::tag::BaselineExposure, {{-3, 4}});
    ifd0.setRational(tiff::tag::AsShotNeutral, {{5, 10}, {1, 1}, {7 + frame, 10}}); // white balance changes per frame
    ifd0.setSRational(tiff::tag::FrameRate, {{25, 1}});
    ifd0.setU8(tiff::tag::TimeCodes, {uint8_t(0x10 + frame), 0x03, 0x02, 0x01, 0xAA, 0xBB, 0, 0}); // user bits too
    ifd0.setAscii(tiff::tag::ReelName, "A001");
    w.setFirstIfd(w.writeIfd(ifd0));
    REQUIRE(w.save(p));
}

void writeWav8(const fs::path& p, uint32_t samples) {
    std::vector<uint8_t> b = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                              1, 0, 1, 0, 0x80, 0xBB, 0, 0, 0x80, 0xBB, 0, 0, 1, 0, 8, 0, 'd', 'a', 't', 'a'};
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(samples >> (8 * i)));
    for (uint32_t i = 0; i < samples; ++i) b.push_back(uint8_t(128 + (i % 64)));
    const uint32_t riff = static_cast<uint32_t>(b.size() - 8);
    for (int i = 0; i < 4; ++i) b[4 + i] = uint8_t(riff >> (8 * i));
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::vector<uint8_t> fileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

const tiff::Entry& need(const tiff::Ifd& ifd, uint16_t tag) {
    const tiff::Entry* e = ifd.find(tag);
    INFO("tag " << tag);
    REQUIRE(e);
    return *e;
}

} // namespace

TEST_CASE("Synthesised SMPTE timecode above 30 fps uses frame pairs, never the drop-frame bit") {
    for (Rational fps : {Rational{50, 1}, Rational{60000, 1001}, Rational{60, 1}}) {
        for (int f = 0; f < (fps.toDouble() > 55 ? 60 : 50); ++f) {
            Timecode tc;
            tc.fps = fps;
            tc.hours = 1;
            tc.seconds = 7;
            tc.frames = f;
            const auto b = encodeSmpteTimecode(tc);
            REQUIRE((b[0] & 0x40) == 0);
            REQUIRE(decodeSmpteTimecode(b.data(), fps) == tc);
        }
    }
}

TEST_CASE("CinemaDNG metadata: IFD0 tags of a SubIFD layout, per-frame tags, framing and opcodes reach the output") {
    const fs::path dir = tmpDir("subifd");
    const fs::path clip = dir / "C001";
    fs::create_directories(clip);
    for (int f = 0; f < 3; ++f) writeSubIfdFrame(clip / ("C001_00000" + std::to_string(f) + ".dng"), f, 1000, 10, 11);

    JobOptions o;
    o.size = SizePreset::Custom;
    o.customSize = resample::OutputSize{32, 16};
    o.kernel = ResampleKernel::Area;
    const ClipResult r = convertClip(ClipJob(clip, dir / "out"), o, {});
    INFO(r.message);
    REQUIRE(r.status == ClipStatus::Done);
    REQUIRE(r.frames == 3);
    std::vector<fs::path> frames;
    for (const auto& f : r.files)
        if (f.extension() == ".dng") frames.push_back(f);
    std::sort(frames.begin(), frames.end());
    REQUIRE(frames.size() == 3);

    for (int f = 0; f < 3; ++f) {
        tiff::Reader rd;
        REQUIRE(rd.open(frames[static_cast<size_t>(f)]));
        auto ifd = rd.readIfd(rd.firstIfdOffset());
        REQUIRE(ifd);
        REQUIRE(ifd->ascii(tiff::tag::Make) == "TestCam");
        REQUIRE(ifd->ascii(tiff::tag::UniqueCameraModel) == "TestCam Model X v2");
        REQUIRE(ifd->ascii(tiff::tag::ReelName) == "A001");
        REQUIRE(ifd->u32(tiff::tag::Orientation) == 3u);
        REQUIRE(need(*ifd, 50964).count == 9);
        REQUIRE(need(*ifd, 50964).real(1).value() == 0.25);
        REQUIRE(need(*ifd, tiff::tag::BaselineExposure).real(0).value() == -0.75);
        REQUIRE(need(*ifd, tiff::tag::ColorMatrix1).real(4).value() == 1.2);
        REQUIRE(need(*ifd, tiff::tag::AsShotNeutral).real(2).value() == (7 + f) / 10.0); // this frame's own WB
        REQUIRE(need(*ifd, tiff::tag::FrameRate).real(0).value() == 25.0);
        const auto& tc = need(*ifd, tiff::tag::TimeCodes); // byte for byte, user bits included
        REQUIRE(tc.data == std::vector<uint8_t>{uint8_t(0x10 + f), 0x03, 0x02, 0x01, 0xAA, 0xBB, 0, 0});
        // Framing scaled by 1/2 per axis; the 2:1 pixel aspect is kept.
        REQUIRE(need(*ifd, tiff::tag::DefaultCropOrigin).real(0).value() == 2.0);
        REQUIRE(need(*ifd, tiff::tag::DefaultCropOrigin).real(1).value() == 1.0);
        REQUIRE(need(*ifd, tiff::tag::DefaultCropSize).real(0).value() == 28.0);
        REQUIRE(need(*ifd, tiff::tag::DefaultCropSize).real(1).value() == 14.0);
        REQUIRE(need(*ifd, tiff::tag::DefaultScale).real(0).value() == 2.0);
        REQUIRE(need(*ifd, tiff::tag::DefaultScale).real(1).value() == 1.0);
        // Bad-pixel list applied to the mosaic, not carried; GainMap rescaled keeping its CFA phase.
        REQUIRE(ifd->find(tiff::tag::OpcodeList1) == nullptr);
        const auto& gm = need(*ifd, tiff::tag::OpcodeList2).data;
        REQUIRE(gm.size() > 36);
        const auto u = [&](size_t at) { return (uint32_t(gm[at]) << 24) | (uint32_t(gm[at + 1]) << 16) | (uint32_t(gm[at + 2]) << 8) | gm[at + 3]; };
        REQUIRE(u(4) == 9);                                                 // GainMap
        REQUIRE((u(20) == 0 && u(24) == 1 && u(28) == 16 && u(32) == 32)); // top, left (phase 1), bottom, right
        REQUIRE(ifd->find(tiff::tag::OpcodeList3) != nullptr);              // normalised: copied
    }
    // The hot pixel at (10, 11) was repaired before resampling: the output is flat.
    auto reader = makeCinemaDngCodecFactory()->makeReader();
    REQUIRE(reader->open(r.output));
    BayerFrame fr;
    REQUIRE(reader->readFrame(0, fr));
    for (uint16_t v : fr.data) REQUIRE(v == 1000);
}

TEST_CASE("CinemaDNG audio sidecars are copied byte for byte; AppleDouble stubs are ignored") {
    const fs::path dir = tmpDir("wav");
    const fs::path clip = dir / "C002";
    fs::create_directories(clip);
    for (int f = 0; f < 2; ++f) writeSubIfdFrame(clip / ("C002_00000" + std::to_string(f) + ".dng"), f, 2000, 0, 0);
    writeWav8(clip / "C002.wav", 3840);      // 8-bit PCM: not decodable by the stream path
    writeWav8(clip / "C002_boom.wav", 1000); // a second recorder track
    { std::ofstream(clip / "._C002.wav") << "Mac OS X        "; }

    JobOptions o;
    o.size = SizePreset::Same;
    const ClipResult r = convertClip(ClipJob(clip, dir / "out"), o, {});
    INFO(r.message);
    REQUIRE(r.status == ClipStatus::Done);
    const std::string name = r.output.filename().string();
    REQUIRE(fileBytes(r.output / (name + ".wav")) == fileBytes(clip / "C002.wav"));
    REQUIRE(fileBytes(r.output / (name + "_C002_boom.wav")) == fileBytes(clip / "C002_boom.wav"));
    REQUIRE_FALSE(fs::exists(r.output / (name + "_._C002.wav")));
    bool warned = false;
    for (const auto& w : r.warnings) warned |= w.find("cannot be decoded") != std::string::npos;
    REQUIRE(warned);
}
