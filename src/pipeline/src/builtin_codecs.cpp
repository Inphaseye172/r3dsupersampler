#include <mutex>
#include "r3dss/codec/codec.hpp"
#include "r3dss/codec/memory_codec.hpp"
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/codec_r3dsdk/r3dsdk.hpp"

namespace r3dss::codec {

void registerBuiltinCodecs() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& r = CodecRegistry::instance();
        r.registerFactory(makeCinemaDngCodecFactory());
        r.registerFactory(r3dsdk::makeRedFamilyFactory());
        r.registerFactory(makeMemoryCodecFactory());
    });
}

} // namespace r3dss::codec
