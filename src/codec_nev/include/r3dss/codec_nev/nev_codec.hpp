// NEV (N-RAW) and R3D NE codec factories.
//
// Without the intoPIX TicoRAW SDK these are honest stubs: probe() recognises the files,
// makeReader()->open() returns container-level facts it can establish (chunk count, size) and
// every frame/audio call fails with ErrorCode::CodecUnavailable. The writer always fails.
// With R3DSS_WITH_TICORAW the adapter in ticoraw_adapter.hpp is used (task 11/22).
#pragma once
#include <memory>
#include "r3dss/codec/codec.hpp"

namespace r3dss::codec {

std::shared_ptr<ICodecFactory> makeNevCodecFactory();
std::shared_ptr<ICodecFactory> makeR3dNeCodecFactory();

// Abstract TicoRAW essence codec. The intoPIX implementation lives behind R3DSS_WITH_TICORAW.
class ITicoRawCodec {
public:
    virtual ~ITicoRawCodec() = default;
    virtual Expected<void> decode(const uint8_t* codestream, size_t bytes, BayerFrame& out) = 0;
    virtual Expected<std::vector<uint8_t>> encode(const BayerFrame& in, uint64_t budgetBits, bool lossless) = 0;
};
// Returns nullptr when the SDK is not compiled in.
std::unique_ptr<ITicoRawCodec> makeTicoRawCodec();

} // namespace r3dss::codec
