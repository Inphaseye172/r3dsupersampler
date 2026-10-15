// Container-only reader and the (always failing) writer for the RED family. The real decoder
// lives in src/codec_r3dsdk and wraps this reader's container facts.
#pragma once
#include <memory>
#include "r3dss/codec/codec.hpp"
#include "r3dss/codec_red/red_container.hpp"
#include "r3dss/codec_red/red_detect.hpp"

namespace r3dss::codec::red {

// Fills format, sourceKind/label, sourceBytes and container facts (meta.vendor "container.*") from
// the chunk list and header strings. Used by both the container-only and the SDK reader.
Expected<void> fillContainerInfo(const std::filesystem::path& path, ClipInfo& info);

// Reader that opens the container but cannot decode frames (no SDK built in).
std::unique_ptr<ICodecReader> makeContainerOnlyReader(std::string unavailableReason);
// Writer that refuses: REDCODE cannot be written.
std::unique_ptr<ICodecWriter> makeUnsupportedWriter();

bool probeRedFamily(const std::filesystem::path& path); // extension .r3d/.nev and RED header

} // namespace r3dss::codec::red
