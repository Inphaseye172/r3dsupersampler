// Golden check of the CinemaDNG reader against an independent decoder. The DNGs in
// tests/fixtures/cdng/ were written by encoders that share no code with r3dss (pidng, tifffile,
// a small Python LJ92 encoder with restart markers) and decoded by LibRaw (rawpy 0.27.1); see
// tests/fixtures/cdng/make_fixtures.py, which also checks that LibRaw returns the intended values.
// The r3dss reader must return exactly LibRaw's visible mosaic (cropped to even width/height),
// CFA phase, per-site black and white level.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/core/sha256.hpp"

using namespace r3dss;
using namespace r3dss::codec;
namespace fs = std::filesystem;

namespace {

struct Golden {
    const char* name;
    uint32_t width, height;
    CfaPattern cfa;
    std::array<uint32_t, 4> black; // per site, plane order (0,0), (1,0), (0,1), (1,1)
    uint32_t white;
    const char* sha256; // LibRaw raw_image_visible[:h&~1, :w&~1] as little-endian uint16
};

// Printed by make_fixtures.py.
const Golden kGolden[] = {
    // pidng: LJ92 stream folded as 2*W x H/2 (1 component), single tile
    {"pidng_lj12_rggb", 64, 32, CfaPattern::RGGB, {64, 64, 64, 64}, 4095, "86ba2e3a0e86948f30a13b1cb3c7a85b189c846ce4de652721c481cbc239767b"},
    // pidng: uncompressed packed 12-bit
    {"pidng_u12_bggr", 64, 32, CfaPattern::BGGR, {64, 64, 64, 64}, 4095, "26e63cf7832ced8e5b2f3007415b84b46c0c44aabbd7882cfcb4c0af6fbcab83"},
    // tifffile: big-endian 16-bit, 5-row strips (last strip shorter), odd 51x33 size
    {"tiff_be16_strips_grbg_odd", 50, 32, CfaPattern::GRBG, {1000, 1000, 1000, 1000}, 65535, "b39bb91cd8610f2a676e6003b7ccb8a8dcfd6d90deb5a1857d2cf75bae612a79"},
    // tifffile: packed 14-bit, 16x16 tiles with partial edge tiles, 2x2 BlackLevel
    {"tiff_le14_tiles_gbrg", 50, 38, CfaPattern::GBRG, {500, 510, 520, 530}, 16383, "89eda03ebd85a2252d482491d19ee1cc11dca0bf6979a23ce73f6593a94cbe59"},
    // tifffile: packed 10-bit, width 66 (rows end mid-byte), 7-row strips
    {"tiff_le10_w66_strips", 66, 24, CfaPattern::RGGB, {0, 0, 0, 0}, 1023, "3cf044a92e9939d076af986f94aa100bacde6c93ecffbf48ef96cd450786fdc6"},
    // tifffile: big-endian 16-bit tiles, BlackLevelRepeatDim 2x1
    {"tiff_be16_tiles_bggr", 48, 40, CfaPattern::BGGR, {3000, 3000, 3100, 3100}, 65535, "0f2a3082d18edb9a46fc6c95ba2d50b431b912da1815f81b8d849eaddb80e83a"},
    // tifffile: ActiveArea [1, 3, 31, 61] (odd origin), 12-bit codes + LinearizationTable, 2x2 black
    {"tiff_aa_odd_lin12", 56, 28, CfaPattern::BGGR, {2000, 2100, 2200, 2300}, 65535, "dcffb864aaf0652c59c8e0e86be593dd556b73f483b5ab9aa691ca1823ed0598"},
    // tifffile: ActiveArea [4, 8, 36, 64]
    {"tiff_aa_even_u16", 56, 32, CfaPattern::GBRG, {0, 0, 0, 0}, 60000, "2fb44c7dfd8780d583a5016ea9ccc68cb6ade178e755ca5f1eba6f828bf93ab2"},
    // Python LJ92 encoder: restart marker after every row (DRI), 16x16 tiles
    {"lj92_restart_tiles", 64, 32, CfaPattern::GRBG, {0, 0, 0, 0}, 16383, "20c6802daa70f38627feb57bbc15ff36b264c045db3f9daa621ec8d83ed330ce"},
};

fs::path fixtureDir() { return fs::path(R3DSS_TEST_FIXTURES_DIR) / "cdng"; }

} // namespace

TEST_CASE("CinemaDNG reader returns exactly LibRaw's mosaic for independently written DNGs") {
    for (const Golden& g : kGolden) {
        INFO("fixture " << g.name);
        const fs::path dir = fixtureDir() / g.name;
        REQUIRE(fs::is_directory(dir));
        auto reader = makeCinemaDngCodecFactory()->makeReader();
        auto info = reader->open(dir);
        if (!info) FAIL("open failed: " << info.error().toString());
        REQUIRE(info->frameCount == 1);
        REQUIRE(info->width == g.width);
        REQUIRE(info->height == g.height);
        REQUIRE(info->cfa == g.cfa);
        REQUIRE(info->black == g.black);
        REQUIRE(info->white == g.white);
        BayerFrame f;
        auto r = reader->readFrame(0, f);
        if (!r) FAIL("readFrame failed: " << r.error().toString());
        REQUIRE(f.width == g.width);
        REQUIRE(f.height == g.height);
        REQUIRE(f.cfa == g.cfa);
        REQUIRE(f.black == g.black);
        REQUIRE(f.white == g.white);
        std::vector<uint8_t> le(f.data.size() * 2);
        for (size_t i = 0; i < f.data.size(); ++i) {
            le[2 * i] = static_cast<uint8_t>(f.data[i]);
            le[2 * i + 1] = static_cast<uint8_t>(f.data[i] >> 8);
        }
        REQUIRE(Sha256::hashBytes(le.data(), le.size()) == g.sha256);
    }
}
