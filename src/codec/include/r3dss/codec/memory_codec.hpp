// In-memory codec used by unit tests and the benchmark: clips live in a process-wide store
// keyed by a pseudo path "mem://name". Exercises the full pipeline without disk I/O.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include "r3dss/codec/codec.hpp"

namespace r3dss::codec {

struct MemoryClip {
    ClipInfo info;
    std::vector<BayerFrame> frames;
    std::vector<uint8_t> audioBytes; // interleaved, info.audio describes the layout
};

class MemoryStore {
public:
    static MemoryStore& instance();
    void put(const std::string& name, std::shared_ptr<MemoryClip> clip);
    std::shared_ptr<MemoryClip> get(const std::string& name) const;
    void erase(const std::string& name);
    static bool isMemPath(const std::filesystem::path& p);
    static std::string nameOf(const std::filesystem::path& p);

private:
    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<MemoryClip>> clips_;
};

std::shared_ptr<ICodecFactory> makeMemoryCodecFactory();

} // namespace r3dss::codec
