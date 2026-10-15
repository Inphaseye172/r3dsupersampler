// Source detection for RED-container clips: which camera / codec produced the file.
// Two inputs: the RED R3D SDK's metadata (preferred) or the header strings when no SDK is built.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "r3dss/codec/codec.hpp"

namespace r3dss::codec::red {

struct SourceInfo {
    SourceKind kind = SourceKind::Unknown;
    std::string cameraModel; // "KOMODO 6K", "NIKON ZR"
    std::string sensorName;  // "KOMODO S35", "NIKON"
    std::string redcode;     // "LQ", "HQ", "R3D NE"
    std::string label;       // "REDCODE RAW (KOMODO 6K)"
};

// SDK metadata keys as the R3D SDK names them: camera_model, sensor_name, redcode.
// Nikon R3D NE: sensor_name == "NIKON" or redcode == "R3D NE". Anything else with a RED sensor
// (KOMODO, V-RAPTOR, DSMC2 family, ...) is REDCODE RAW.
SourceInfo detectFromMetadata(const std::map<std::string, std::string>& metadata, const std::string& extension = "");

// Header-string path (no SDK): finds the model / sensor tokens among the printable strings of the
// "RED" chunk. `extension` (lower-case, with dot) distinguishes NEV from R3D NE for Nikon clips.
SourceInfo detectFromHeaderStrings(const std::vector<std::string>& strings, const std::string& extension);

std::string makeLabel(SourceKind kind, const std::string& cameraModel);

} // namespace r3dss::codec::red
