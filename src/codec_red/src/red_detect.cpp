#include "r3dss/codec_red/red_detect.hpp"
#include <algorithm>
#include <cctype>

namespace r3dss::codec::red {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
bool contains(const std::string& hay, const char* needle) { return upper(hay).find(needle) != std::string::npos; }

// Camera family tokens seen in RED camera_model strings.
const char* kRedTokens[] = {"KOMODO", "V-RAPTOR", "RAPTOR", "DSMC2", "DSMC", "MONSTRO", "HELIUM", "GEMINI",
                            "DRAGON", "SCARLET", "EPIC", "WEAPON", "RANGER", "RED ONE", "MYSTERIUM"};
bool hasRedToken(const std::string& s) {
    for (const char* t : kRedTokens)
        if (contains(s, t)) return true;
    return false;
}
bool isNikon(const std::string& s) { return contains(s, "NIKON"); }
// Sensor strings carry a format suffix: "KOMODO S35", "V-RAPTOR VV", "MONSTRO 8K VV", "HELIUM 8K S35".
bool looksLikeSensor(const std::string& s) {
    const std::string u = upper(s);
    return u.find(" S35") != std::string::npos || u.find(" VV") != std::string::npos || u == "NIKON";
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}
} // namespace

std::string makeLabel(SourceKind kind, const std::string& cameraModel) {
    std::string base = sourceKindName(kind);
    if (kind == SourceKind::Unknown) base = "RED container";
    return cameraModel.empty() ? base : base + " (" + cameraModel + ")";
}

SourceInfo detectFromMetadata(const std::map<std::string, std::string>& md, const std::string& extension) {
    SourceInfo s;
    auto get = [&](const char* k) {
        auto it = md.find(k);
        return it == md.end() ? std::string() : trim(it->second);
    };
    s.cameraModel = get("camera_model");
    s.sensorName = get("sensor_name");
    s.redcode = get("redcode");
    if (upper(s.sensorName) == "NIKON" || upper(s.redcode) == "R3D NE" || isNikon(s.cameraModel))
        s.kind = upper(extension) == ".NEV" ? SourceKind::NikonNev : SourceKind::NikonR3dNe;
    else if (hasRedToken(s.sensorName) || hasRedToken(s.cameraModel) || !s.redcode.empty())
        s.kind = SourceKind::RedcodeRaw;
    s.label = makeLabel(s.kind, s.cameraModel);
    return s;
}

SourceInfo detectFromHeaderStrings(const std::vector<std::string>& strings, const std::string& extension) {
    SourceInfo s;
    for (const auto& raw : strings) {
        const std::string str = trim(raw);
        if (str.size() > 40) continue;
        if (upper(str) == "R3D NE") { s.redcode = str; continue; }
        if (!(hasRedToken(str) || isNikon(str))) continue;
        if (looksLikeSensor(str)) {
            if (s.sensorName.empty()) s.sensorName = str;
        } else if (s.cameraModel.empty()) {
            s.cameraModel = str;
        }
    }
    const bool nikon = isNikon(s.cameraModel) || isNikon(s.sensorName) || upper(s.redcode) == "R3D NE";
    if (nikon) s.kind = upper(extension) == ".NEV" ? SourceKind::NikonNev : SourceKind::NikonR3dNe;
    else if (!s.cameraModel.empty() || !s.sensorName.empty()) s.kind = SourceKind::RedcodeRaw;
    if (s.cameraModel.empty() && !s.sensorName.empty()) s.cameraModel = s.sensorName;
    s.label = makeLabel(s.kind, s.cameraModel);
    return s;
}

} // namespace r3dss::codec::red
