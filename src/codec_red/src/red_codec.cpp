#include "r3dss/codec_red/red_codec.hpp"
#include <algorithm>
#include <cctype>

namespace r3dss::codec::red {

namespace {
std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}
} // namespace

bool probeRedFamily(const std::filesystem::path& p) {
    const std::string e = lowerExt(p);
    return (e == ".r3d" || e == ".nev") && looksLikeRedContainer(p);
}

Expected<void> fillContainerInfo(const std::filesystem::path& path, ClipInfo& info) {
    info.format = Format::RedFamily;
    info.path = path;
    std::error_code ec;
    info.sourceBytes = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    R3DSS_TRY_ASSIGN(chunks, listChunks(path));
    const auto hist = chunkHistogram(chunks);
    info.meta.vendor["container.chunks"] = static_cast<int64_t>(chunks.size());
    for (const auto& [tag, st] : hist) info.meta.vendor["container.chunks." + tag] = static_cast<int64_t>(st.count);
    if (auto it = hist.find(kTagVideo); it != hist.end())
        info.meta.vendor["container.videoFrames"] = static_cast<int64_t>(it->second.count);
    R3DSS_TRY_ASSIGN(strings, readHeaderStrings(path));
    const SourceInfo src = detectFromHeaderStrings(strings, lowerExt(path));
    info.sourceKind = src.kind;
    info.sourceLabel = src.label;
    if (!src.cameraModel.empty()) info.meta.cameraModel = src.cameraModel;
    if (!src.sensorName.empty()) info.meta.vendor["sensor_name"] = src.sensorName;
    if (!src.redcode.empty()) info.meta.vendor["redcode"] = src.redcode;
    info.meta.cameraMake = src.kind == SourceKind::NikonR3dNe || src.kind == SourceKind::NikonNev ? "Nikon" : "RED";
    // Timecode-looking strings in the header: edge then absolute on the surveyed clip.
    std::vector<std::string> tcs;
    for (const auto& s : strings)
        if (s.size() == 11 && s[2] == ':' && s[5] == ':' && (s[8] == ':' || s[8] == ';')) tcs.push_back(s);
    for (size_t i = 0; i < tcs.size(); ++i) info.meta.vendor["header.timecode" + std::to_string(i)] = tcs[i];
    return {};
}

namespace {

class ContainerOnlyReader final : public ICodecReader {
public:
    explicit ContainerOnlyReader(std::string why) : why_(std::move(why)) {}
    Expected<ClipInfo> open(const std::filesystem::path& path) override {
        info_ = ClipInfo{};
        R3DSS_TRY(fillContainerInfo(path, info_));
        return info_;
    }
    const ClipInfo& info() const override { return info_; }
    Expected<void> readFrame(int64_t, BayerFrame&) override { return unavailable(); }
    Expected<AudioBlock> readAudio(int64_t, int64_t) override { return unavailable(); }
    Expected<Timecode> frameTimecode(int64_t) override { return unavailable(); }
    void close() override {}

private:
    Unexpected unavailable() const { return makeError(ErrorCode::CodecUnavailable, why_); }
    std::string why_;
    ClipInfo info_;
};

class UnsupportedWriter final : public ICodecWriter {
public:
    Expected<void> begin(const WriterConfig&) override { return refuse(); }
    Expected<EncodedStats> writeFrame(const BayerFrame&, FrameBudget) override { return refuse(); }
    Expected<void> writeAudio(const AudioBlock&) override { return refuse(); }
    Expected<WriteSummary> finish() override { return refuse(); }
    void abort() override {}

private:
    static Unexpected refuse() {
        return makeError(ErrorCode::Unsupported,
                         "REDCODE cannot be written (no public encoder exists); choose CinemaDNG, ProRes or DNxHR output");
    }
};

} // namespace

std::unique_ptr<ICodecReader> makeContainerOnlyReader(std::string why) {
    return std::make_unique<ContainerOnlyReader>(std::move(why));
}
std::unique_ptr<ICodecWriter> makeUnsupportedWriter() { return std::make_unique<UnsupportedWriter>(); }

} // namespace r3dss::codec::red
