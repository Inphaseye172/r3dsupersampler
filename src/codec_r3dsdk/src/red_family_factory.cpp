#include "r3dss/codec_r3dsdk/r3dsdk.hpp"
#include "r3dss/codec_red/red_codec.hpp"

namespace r3dss::codec::r3dsdk {

namespace {
class RedFamilyFactory final : public ICodecFactory {
public:
    Format format() const override { return Format::RedFamily; }
    std::string displayName() const override { return "REDCODE RAW / R3D NE / NEV (RED R3D SDK)"; }
    Capabilities caps() const override {
        Capabilities c;
        c.read = builtWithSdk() && initialize() && makeSdkReader() != nullptr;
        c.write = false;
        c.cuda = false;
        c.rateControl = false;
        c.lossless = false;
        c.dimAlignment = 2;
        c.bitDepths = {16};
        return c;
    }
    ModuleStatus status() const override {
        ModuleStatus s;
        if (!builtWithSdk()) {
            s.state = ModuleState::MissingSdk;
            s.text = statusText();
        } else if (!initialize()) {
            s.state = ModuleState::MissingSdk;
            s.text = statusText();
        } else if (!makeSdkReader()) {
            s.state = ModuleState::Stub;
            s.text = statusText() + "; decoder arrives in milestone 2 (container probing only)";
        } else {
            s.state = ModuleState::Available;
            s.text = statusText();
        }
        return s;
    }
    bool probe(const std::filesystem::path& p) const override { return red::probeRedFamily(p); }
    std::unique_ptr<ICodecReader> makeReader() const override {
        if (auto r = makeSdkReader()) return r;
        return red::makeContainerOnlyReader("cannot decode REDCODE frames: " + status().text);
    }
    std::unique_ptr<ICodecWriter> makeWriter() const override { return red::makeUnsupportedWriter(); }
};
} // namespace

std::shared_ptr<ICodecFactory> makeRedFamilyFactory() { return std::make_shared<RedFamilyFactory>(); }

} // namespace r3dss::codec::r3dsdk
