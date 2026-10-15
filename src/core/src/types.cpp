#include "r3dss/core/types.hpp"
#include <algorithm>
#include <cctype>

namespace r3dss {

namespace {
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

const char* cfaName(CfaPattern p) {
    switch (p) {
    case CfaPattern::RGGB: return "RGGB";
    case CfaPattern::BGGR: return "BGGR";
    case CfaPattern::GRBG: return "GRBG";
    case CfaPattern::GBRG: return "GBRG";
    }
    return "?";
}

std::optional<CfaPattern> cfaFromName(std::string_view s) {
    const std::string l = lower(s);
    if (l == "rggb") return CfaPattern::RGGB;
    if (l == "bggr") return CfaPattern::BGGR;
    if (l == "grbg") return CfaPattern::GRBG;
    if (l == "gbrg") return CfaPattern::GBRG;
    return std::nullopt;
}

std::array<int, 4> cfaColors(CfaPattern p) {
    switch (p) {
    case CfaPattern::RGGB: return {0, 1, 1, 2};
    case CfaPattern::BGGR: return {2, 1, 1, 0};
    case CfaPattern::GRBG: return {1, 0, 2, 1};
    case CfaPattern::GBRG: return {1, 2, 0, 1};
    }
    return {0, 1, 1, 2};
}

int cfaColorAt(CfaPattern p, uint32_t x, uint32_t y) { return cfaColors(p)[(y & 1u) * 2u + (x & 1u)]; }

BayerFrame BayerFrame::create(uint32_t w, uint32_t h, uint16_t bits, CfaPattern cfa) {
    BayerFrame f;
    f.width = w;
    f.height = h;
    f.bits = bits;
    f.cfa = cfa;
    f.white = (1u << bits) - 1u;
    f.data.assign(static_cast<size_t>(w) * h, 0);
    return f;
}

const char* sampleFormatName(SampleFormat f) {
    switch (f) {
    case SampleFormat::PCM16: return "pcm16";
    case SampleFormat::PCM24: return "pcm24";
    case SampleFormat::PCM32: return "pcm32";
    case SampleFormat::Float32: return "float32";
    }
    return "?";
}

uint32_t sampleFormatBytes(SampleFormat f) {
    switch (f) {
    case SampleFormat::PCM16: return 2;
    case SampleFormat::PCM24: return 3;
    case SampleFormat::PCM32: return 4;
    case SampleFormat::Float32: return 4;
    }
    return 0;
}

const char* qualityProfileName(QualityProfile q) {
    switch (q) {
    case QualityProfile::Q1_Maximum: return "Q1";
    case QualityProfile::Q3_High: return "Q3";
    case QualityProfile::Q5_Normal: return "Q5";
    case QualityProfile::Q7_Proxy: return "Q7";
    }
    return "?";
}

std::optional<QualityProfile> qualityProfileFromName(std::string_view s) {
    const std::string l = lower(s);
    if (l == "q1" || l == "maximum" || l == "max") return QualityProfile::Q1_Maximum;
    if (l == "q3" || l == "high") return QualityProfile::Q3_High;
    if (l == "q5" || l == "normal") return QualityProfile::Q5_Normal;
    if (l == "q7" || l == "proxy") return QualityProfile::Q7_Proxy;
    return std::nullopt;
}

const char* sizePresetName(SizePreset s) {
    switch (s) {
    case SizePreset::Same: return "same";
    case SizePreset::SixK: return "6K";
    case SizePreset::UHD: return "UHD";
    case SizePreset::TwoK: return "2K";
    case SizePreset::FHD: return "FHD";
    case SizePreset::Custom: return "custom";
    }
    return "?";
}

std::optional<SizePreset> sizePresetFromName(std::string_view s) {
    const std::string l = lower(s);
    if (l == "same" || l == "source") return SizePreset::Same;
    if (l == "6k") return SizePreset::SixK;
    if (l == "uhd" || l == "4k") return SizePreset::UHD;
    if (l == "2k" || l == "dci2k") return SizePreset::TwoK;
    if (l == "fhd" || l == "1080p" || l == "hd") return SizePreset::FHD;
    if (l == "custom") return SizePreset::Custom;
    return std::nullopt;
}

const char* kernelName(ResampleKernel k) {
    switch (k) {
    case ResampleKernel::Lanczos3: return "lanczos3";
    case ResampleKernel::Area: return "area";
    }
    return "?";
}

std::optional<ResampleKernel> kernelFromName(std::string_view s) {
    const std::string l = lower(s);
    if (l == "lanczos3" || l == "lanczos") return ResampleKernel::Lanczos3;
    if (l == "area" || l == "box" || l == "bin") return ResampleKernel::Area;
    return std::nullopt;
}

} // namespace r3dss
