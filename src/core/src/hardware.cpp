#include "r3dss/core/hardware.hpp"
#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace r3dss {

HardwareInfo probeHardware() {
    HardwareInfo hw;
    hw.logicalCores = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) hw.totalRamBytes = ms.ullTotalPhys;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0) hw.totalRamBytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(psize);
#endif
    return hw;
}

ThreadPlan planThreads(const HardwareInfo& hw, size_t frameBytes, unsigned maxClipsOverride, unsigned maxThreads) {
    ThreadPlan p;
    const unsigned C = maxThreads ? std::min(hw.logicalCores, maxThreads) : hw.logicalCores;
    p.readerThreads = C >= 4 ? 2 : 1;
    p.decodeWorkers = std::max(1u, C / 2);
    p.resampleWorkers = hw.cudaAvailable ? 2 : std::max(1u, C / 2);
    p.encodeWorkers = std::max(1u, C >= 3 ? C - 2 : 1);
    p.queueDepth = 4;
    unsigned clips = std::clamp(C / 8, 1u, 4u);
    if (maxClipsOverride) clips = std::max(1u, maxClipsOverride);
    // Each clip holds about 3 stages x queueDepth frames in flight.
    if (frameBytes && hw.totalRamBytes) {
        const uint64_t perClip = static_cast<uint64_t>(frameBytes) * 3 * p.queueDepth;
        const uint64_t budget = hw.totalRamBytes / 2;
        const unsigned byRam = static_cast<unsigned>(std::max<uint64_t>(1, budget / std::max<uint64_t>(1, perClip)));
        clips = std::min(clips, byRam);
    }
    p.parallelClips = std::max(1u, clips);
    return p;
}

} // namespace r3dss
