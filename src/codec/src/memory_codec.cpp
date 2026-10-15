#include "r3dss/codec/memory_codec.hpp"
#include <algorithm>
#include <cstring>

namespace r3dss::codec {

MemoryStore& MemoryStore::instance() {
    static MemoryStore s;
    return s;
}
void MemoryStore::put(const std::string& name, std::shared_ptr<MemoryClip> clip) {
    std::lock_guard<std::mutex> lk(m_);
    clips_[name] = std::move(clip);
}
std::shared_ptr<MemoryClip> MemoryStore::get(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = clips_.find(name);
    return it == clips_.end() ? nullptr : it->second;
}
void MemoryStore::erase(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_);
    clips_.erase(name);
}
// std::filesystem::path collapses "mem://x" to "mem:/x", so match on the "mem:" prefix.
bool MemoryStore::isMemPath(const std::filesystem::path& p) { return p.generic_string().rfind("mem:", 0) == 0; }
std::string MemoryStore::nameOf(const std::filesystem::path& p) {
    std::string s = p.generic_string().substr(4);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

namespace {

class MemoryReader final : public ICodecReader {
public:
    Expected<ClipInfo> open(const std::filesystem::path& path) override {
        clip_ = MemoryStore::instance().get(MemoryStore::nameOf(path));
        if (!clip_) return makeError(ErrorCode::Io, "memory clip not found: " + path.string());
        return clip_->info;
    }
    const ClipInfo& info() const override { return clip_->info; }
    Expected<void> readFrame(int64_t index, BayerFrame& out) override {
        if (index < 0 || index >= static_cast<int64_t>(clip_->frames.size()))
            return makeError(ErrorCode::InvalidArgument, "frame index out of range");
        out = clip_->frames[static_cast<size_t>(index)];
        out.index = index;
        return {};
    }
    Expected<AudioBlock> readAudio(int64_t first, int64_t count) override {
        AudioBlock b;
        b.info = clip_->info.audio;
        const uint32_t fb = b.info.frameBytes();
        if (!b.info.present() || fb == 0) return b;
        const int64_t total = static_cast<int64_t>(clip_->audioBytes.size() / fb);
        first = std::clamp<int64_t>(first, 0, total);
        count = std::clamp<int64_t>(count, 0, total - first);
        b.firstSample = first;
        b.sampleCount = count;
        b.bytes.assign(clip_->audioBytes.begin() + static_cast<std::ptrdiff_t>(first * fb),
                       clip_->audioBytes.begin() + static_cast<std::ptrdiff_t>((first + count) * fb));
        return b;
    }
    Expected<Timecode> frameTimecode(int64_t index) override {
        if (!clip_->info.startTimecode) return makeError(ErrorCode::Unsupported, "clip has no timecode");
        return clip_->info.startTimecode->plusFrames(index);
    }
    void close() override { clip_.reset(); }

private:
    std::shared_ptr<MemoryClip> clip_;
};

class MemoryWriter final : public ICodecWriter {
public:
    Expected<void> begin(const WriterConfig& cfg) override {
        cfg_ = cfg;
        clip_ = std::make_shared<MemoryClip>();
        clip_->info.format = Format::Memory;
        clip_->info.path = cfg.dest;
        clip_->info.width = cfg.width;
        clip_->info.height = cfg.height;
        clip_->info.bits = cfg.bits;
        clip_->info.cfa = cfg.cfa;
        clip_->info.black = cfg.black;
        clip_->info.white = cfg.white;
        clip_->info.fps = cfg.fps;
        clip_->info.audio = cfg.audio;
        clip_->info.audio.sampleCount = 0;
        clip_->info.startTimecode = cfg.startTimecode;
        clip_->info.meta = cfg.meta;
        return {};
    }
    Expected<EncodedStats> writeFrame(const BayerFrame& f, FrameBudget) override {
        if (f.width != cfg_.width || f.height != cfg_.height)
            return makeError(ErrorCode::InvalidArgument, "frame size does not match writer config");
        clip_->frames.push_back(f);
        EncodedStats st;
        st.bytes = f.byteSize();
        st.bitsPerPixel = 16.0;
        return st;
    }
    Expected<void> writeAudio(const AudioBlock& b) override {
        clip_->audioBytes.insert(clip_->audioBytes.end(), b.bytes.begin(), b.bytes.end());
        clip_->info.audio.sampleCount += b.sampleCount;
        return {};
    }
    Expected<WriteSummary> finish() override {
        WriteSummary s;
        s.framesWritten = static_cast<int64_t>(clip_->frames.size());
        s.audioSamplesWritten = clip_->info.audio.sampleCount;
        for (auto& f : clip_->frames) s.totalBytes += f.byteSize();
        clip_->info.frameCount = s.framesWritten;
        clip_->info.sourceBytes = s.totalBytes;
        s.files.push_back(cfg_.dest);
        MemoryStore::instance().put(MemoryStore::nameOf(cfg_.dest), clip_);
        return s;
    }
    void abort() override { clip_.reset(); }

private:
    WriterConfig cfg_;
    std::shared_ptr<MemoryClip> clip_;
};

class MemoryFactory final : public ICodecFactory {
public:
    Format format() const override { return Format::Memory; }
    std::string displayName() const override { return "In-memory (tests)"; }
    Capabilities caps() const override {
        Capabilities c;
        c.read = c.write = c.lossless = true;
        c.dimAlignment = 2;
        return c;
    }
    ModuleStatus status() const override { return {ModuleState::Available, "built in"}; }
    bool probe(const std::filesystem::path& p) const override { return MemoryStore::isMemPath(p); }
    std::unique_ptr<ICodecReader> makeReader() const override { return std::make_unique<MemoryReader>(); }
    std::unique_ptr<ICodecWriter> makeWriter() const override { return std::make_unique<MemoryWriter>(); }
};

} // namespace

std::shared_ptr<ICodecFactory> makeMemoryCodecFactory() { return std::make_shared<MemoryFactory>(); }

} // namespace r3dss::codec
