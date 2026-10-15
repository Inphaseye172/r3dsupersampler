// Tests on the samples-v1 corpus (scripts/fetch_samples.py). Skipped when R3DSS_SAMPLES is unset
// or the clip is missing, so CI stays green without the download.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include "r3dss/codec/codec.hpp"
#include "r3dss/codec_red/red_container.hpp"
#include "r3dss/core/sha256.hpp"

using namespace r3dss;
namespace fs = std::filesystem;

namespace {
const char* kKomodoClip = "A060_A027_0912VZ_001.R3D";
const char* kKomodoSha = "b03dfab13093fd9bb1878a3af8bddbb81672fbc8256823b7141df42d41ae9a6e";
fs::path samplePath() {
    const char* dir = std::getenv("R3DSS_SAMPLES");
    if (!dir) return {};
    fs::path p = fs::path(dir) / kKomodoClip;
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path();
}
struct Init {
    Init() { codec::registerBuiltinCodecs(); }
} init;
} // namespace

TEST_CASE("samples-v1 Komodo clip: hash, 98 chunks, tag histogram, source label", "[samples]") {
    const fs::path clip = samplePath();
    if (clip.empty()) SKIP("R3DSS_SAMPLES unset or clip missing (run scripts/fetch_samples.py)");
    auto sha = Sha256::hashFile(clip);
    REQUIRE(sha);
    REQUIRE(*sha == kKomodoSha);
    auto chunks = codec::red::listChunks(clip);
    REQUIRE(chunks);
    REQUIRE(chunks->size() == 98);
    auto h = codec::red::chunkHistogram(*chunks);
    REQUIRE(h["RED"].count == 1);
    REQUIRE(h["RDI"].count == 84);
    REQUIRE(h["RDA"].count == 7);
    REQUIRE(h["RDX"].count == 1);
    REQUIRE(h["RII"].count == 1);
    REQUIRE(h["RIA"].count == 1);
    REQUIRE(h["RIX"].count == 2);
    REQUIRE(h["REO"].count == 1);
    REQUIRE(chunks->front().version == 50);
    REQUIRE(chunks->back().tag == "REO");
    REQUIRE(chunks->back().version == 66);
    REQUIRE(h["RDA"].minSize == 192128);
    auto f = codec::CodecRegistry::instance().probe(clip);
    REQUIRE(f);
    auto reader = f->makeReader();
    auto info = reader->open(clip);
    REQUIRE(info);
    REQUIRE(info->sourceLabel == "REDCODE RAW (KOMODO 6K)");
    REQUIRE(info->sourceKind == codec::SourceKind::RedcodeRaw);
    REQUIRE(std::get<int64_t>(info->meta.vendor.at("container.videoFrames")) == 84);
    REQUIRE(std::get<std::string>(info->meta.vendor.at("header.timecode0")) == "01:19:31:07");
    REQUIRE(std::get<std::string>(info->meta.vendor.at("header.timecode1")) == "09:02:10:20");
    REQUIRE(info->sourceBytes == 379416576);
}
