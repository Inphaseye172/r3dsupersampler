// Built when no RED R3D SDK was found at configure time.
#include "r3dss/codec_r3dsdk/r3dsdk.hpp"

namespace r3dss::codec::r3dsdk {

bool builtWithSdk() { return false; }
bool initialize() { return false; }
std::string statusText() {
    return "RED R3D SDK not built in: download the SDK from RED, set R3DSDK_ROOT and reconfigure (docs/FEASIBILITY.md)";
}
std::string runtimeDir() { return {}; }
std::unique_ptr<ICodecReader> makeSdkReader() { return nullptr; }

} // namespace r3dss::codec::r3dsdk
