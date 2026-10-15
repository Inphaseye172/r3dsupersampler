#include "r3dss/codec_nev/nev_codec.hpp"
#include <algorithm>
#include <cctype>
#include "r3dss/codec_nev/red_container.hpp"

namespace r3dss::codec {

std::unique_ptr<ITicoRawCodec> makeTicoRawCodec() {
#if defined(R3DSS_WITH_TICORAW)
    // Task 11: construct the intoPIX adapter here.
    return nullptr;
#else
    return nullptr;
#endif
}

namespace {

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

ModuleStatus stubStatus(const char* name) {
    ModuleStatus s;
    if (makeTicoRawCodec()) {
        s.state = ModuleState::Available;
        s.text = std::string(name) + ": intoPIX TicoRAW SDK linked";
    } else {
        s.state = ModuleState::MissingSdk;
        s.text = std::string(name) +
                 ": requires the intoPIX FastTicoRAW SDK (commercial). Build with -DR3DSS_WITH_TICORAW=ON and "
                 "TICORAW_ROOT set. Until then files are recognised but not decoded. See docs/FEASIBILITY.md.";
    }
    return s;
}

class StubReader final : public ICodecReader {
public:
    explicit StubReader(Format f) : fmt_(f) {}
    Expected<ClipInfo> open(const std::filesystem::path& path) override {
        info_ = ClipInfo{};
        info_.format = fmt_;
        info_.path = path;
        std::error_code ec;
        info_.sourceBytes = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
        auto chunks = red::listChunks(path, 64);
        if (!chunks) return Unexpected{chunks.error()};
        info_.meta.vendor["container.firstChunk"] = chunks->front().tag;
        info_.meta.vendor["container.chunksSeen"] = static_cast<int64_t>(chunks->size());
        return info_;
    }
    const ClipInfo& info() const override { return info_; }
    Expected<void> readFrame(int64_t, BayerFrame&) override { return unavailable(); }
    Expected<AudioBlock> readAudio(int64_t, int64_t) override { return unavailable(); }
    Expected<Timecode> frameTimecode(int64_t) override { return unavailable(); }
    void close() override {}

private:
    Unexpected unavailable() const {
        return makeError(ErrorCode::CodecUnavailable,
                         std::string(formatName(fmt_)) + " decoding needs the intoPIX TicoRAW SDK (not built in)");
    }
    Format fmt_;
    ClipInfo info_;
};

class StubWriter final : public ICodecWriter {
public:
    explicit StubWriter(Format f) : fmt_(f) {}
    Expected<void> begin(const WriterConfig&) override { return unavailable(); }
    Expected<EncodedStats> writeFrame(const BayerFrame&, FrameBudget) override { return unavailable(); }
    Expected<void> writeAudio(const AudioBlock&) override { return unavailable(); }
    Expected<WriteSummary> finish() override { return unavailable(); }
    void abort() override {}

private:
    Unexpected unavailable() const {
        if (fmt_ == Format::R3DNE)
            return makeError(ErrorCode::Unsupported,
                             "R3D NE output is not available: no public REDCODE encoder exists (see docs/FEASIBILITY.md)");
        return makeError(ErrorCode::CodecUnavailable, "NEV encoding needs the intoPIX TicoRAW SDK (not built in)");
    }
    Format fmt_;
};

class RedFamilyFactory final : public ICodecFactory {
public:
    RedFamilyFactory(Format f, std::string name, std::string ext) : fmt_(f), name_(std::move(name)), ext_(std::move(ext)) {}
    Format format() const override { return fmt_; }
    std::string displayName() const override { return name_; }
    Capabilities caps() const override {
        Capabilities c;
        const bool sdk = makeTicoRawCodec() != nullptr;
        c.read = sdk;
        c.write = sdk && fmt_ == Format::NEV;
        c.cuda = sdk;
        c.rateControl = true;
        c.lossless = sdk;
        c.dimAlignment = 8; // TicoRAW codes 8-sample groups; unconfirmed for Nikon streams
        c.bitDepths = {12};
        return c;
    }
    ModuleStatus status() const override {
        ModuleStatus s = stubStatus(name_.c_str());
        if (fmt_ == Format::R3DNE && s.state == ModuleState::Available) {
            s.text += " (read only: no R3D NE encoder exists)";
        }
        return s;
    }
    bool probe(const std::filesystem::path& p) const override {
        return lowerExt(p) == ext_ && red::looksLikeRedContainer(p);
    }
    std::unique_ptr<ICodecReader> makeReader() const override { return std::make_unique<StubReader>(fmt_); }
    std::unique_ptr<ICodecWriter> makeWriter() const override { return std::make_unique<StubWriter>(fmt_); }

private:
    Format fmt_;
    std::string name_, ext_;
};

} // namespace

std::shared_ptr<ICodecFactory> makeNevCodecFactory() {
    return std::make_shared<RedFamilyFactory>(Format::NEV, "Nikon N-RAW (NEV)", ".nev");
}
std::shared_ptr<ICodecFactory> makeR3dNeCodecFactory() {
    return std::make_shared<RedFamilyFactory>(Format::R3DNE, "R3D NE", ".r3d");
}

} // namespace r3dss::codec
