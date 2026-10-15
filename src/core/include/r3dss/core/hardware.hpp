// Hardware probe and the thread-count policy from docs/ARCHITECTURE.md.
#pragma once
#include <cstdint>
#include <string>

namespace r3dss {

struct HardwareInfo {
    unsigned logicalCores = 1;
    uint64_t totalRamBytes = 0;
    bool cudaAvailable = false; // filled in by the CUDA module when linked
    std::string gpuName;
    uint64_t gpuMemoryBytes = 0;
};

HardwareInfo probeHardware();

struct ThreadPlan {
    unsigned readerThreads = 1;   // per clip
    unsigned decodeWorkers = 1;   // per clip (CPU codecs)
    unsigned resampleWorkers = 1; // per clip (CPU path) or CUDA streams (GPU path)
    unsigned encodeWorkers = 1;   // shared across clips
    unsigned parallelClips = 1;
    size_t queueDepth = 4;
};

// Derives the plan from the hardware and a frame size (bytes) so RAM bounds parallelism.
// `maxParallelClipsOverride` 0 = automatic.
ThreadPlan planThreads(const HardwareInfo& hw, size_t frameBytes, unsigned maxParallelClipsOverride = 0,
                       unsigned maxThreadsOverride = 0);

} // namespace r3dss
