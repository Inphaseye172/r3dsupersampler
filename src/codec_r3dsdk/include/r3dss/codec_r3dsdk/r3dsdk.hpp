// Public surface of the RED R3D SDK module. No SDK types leak through this header.
#pragma once
#include <memory>
#include <string>
#include "r3dss/codec/codec.hpp"

namespace r3dss::codec::r3dsdk {

// True when this build was compiled against the RED R3D SDK.
bool builtWithSdk();
// Initialises the SDK once (thread-safe). Returns the status text ("RED R3D SDK 9.2.1 ..." or why not).
// Safe to call without the SDK: returns false.
bool initialize();
std::string statusText();
// Directory the runtime libraries (REDR3D-x64.*, REDDecoder-x64.*) were loaded from, or empty.
std::string runtimeDir();

// Reader backed by the SDK (milestone 2). Returns nullptr when the SDK is unavailable.
std::unique_ptr<ICodecReader> makeSdkReader();

// Factory for the whole RED family (REDCODE RAW, R3D NE, NEV); always available, reads only with the SDK.
std::shared_ptr<ICodecFactory> makeRedFamilyFactory();

} // namespace r3dss::codec::r3dsdk
