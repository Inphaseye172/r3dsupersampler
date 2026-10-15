// Minimal TIFF 6 / DNG IFD reader and writer (little-endian output, either endianness on input).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "r3dss/core/expected.hpp"

namespace r3dss::codec::tiff {

enum class Type : uint16_t {
    Byte = 1, Ascii = 2, Short = 3, Long = 4, Rational = 5, SByte = 6, Undefined = 7,
    SShort = 8, SLong = 9, SRational = 10, Float = 11, Double = 12,
    Ifd = 13 // TIFF Technical Note 1: a LONG holding an IFD offset (SubIFDs / ExifIFD may use it)
};
uint32_t typeSize(Type t);

struct Entry {
    uint16_t tag = 0;
    Type type = Type::Byte;
    uint32_t count = 0;
    std::vector<uint8_t> data; // raw bytes in FILE byte order for reads; little-endian for writes
    bool bigEndian = false;    // byte order of `data` (reads only)

    // Typed accessors (index i); return nullopt when out of range or type mismatch.
    std::optional<uint32_t> u32(uint32_t i = 0) const;
    std::optional<int32_t> s32(uint32_t i = 0) const;
    std::optional<double> real(uint32_t i = 0) const; // rational / srational / float / double
    std::string ascii() const;
};

struct Ifd {
    std::map<uint16_t, Entry> entries;
    uint64_t nextIfdOffset = 0;
    const Entry* find(uint16_t tag) const {
        auto it = entries.find(tag);
        return it == entries.end() ? nullptr : &it->second;
    }
    std::optional<uint32_t> u32(uint16_t tag, uint32_t i = 0) const {
        const Entry* e = find(tag);
        return e ? e->u32(i) : std::nullopt;
    }
    std::optional<double> real(uint16_t tag, uint32_t i = 0) const {
        const Entry* e = find(tag);
        return e ? e->real(i) : std::nullopt;
    }
    std::string ascii(uint16_t tag) const {
        const Entry* e = find(tag);
        return e ? e->ascii() : std::string();
    }
};

class Reader {
public:
    Expected<void> open(const std::filesystem::path& p);
    bool bigEndian() const { return big_; }
    // Parses the IFD at `offset` (entries only; large data is loaded into Entry::data).
    Expected<Ifd> readIfd(uint64_t offset) const;
    uint64_t firstIfdOffset() const { return first_; }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    uint16_t u16At(uint64_t off) const;
    uint32_t u32At(uint64_t off) const;

private:
    std::vector<uint8_t> buf_;
    bool big_ = false;
    uint64_t first_ = 0;
};

// Builds a little-endian TIFF: image data blocks are appended first, IFDs last.
class Writer {
public:
    Writer();
    // Appends raw bytes (tile / strip data) and returns their file offset (word aligned).
    uint64_t appendData(const std::vector<uint8_t>& bytes);
    uint64_t appendData(const uint8_t* bytes, size_t len);

    struct IfdBuilder {
        std::map<uint16_t, Entry> entries;
        void setU8(uint16_t tag, const std::vector<uint8_t>& v, Type t = Type::Byte);
        void setU16(uint16_t tag, const std::vector<uint16_t>& v);
        void setU32(uint16_t tag, const std::vector<uint32_t>& v);
        void setAscii(uint16_t tag, const std::string& s);
        void setRational(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>>& v);
        void setSRational(uint16_t tag, const std::vector<std::pair<int32_t, int32_t>>& v);
        void setRaw(const Entry& e) { entries[e.tag] = e; }
    };
    // Writes an IFD and returns its offset. Call for sub-IFDs (EXIF) before the main IFD so the
    // main IFD can reference them.
    uint64_t writeIfd(const IfdBuilder& ifd);
    void setFirstIfd(uint64_t off);
    Expected<void> save(const std::filesystem::path& p) const;
    const std::vector<uint8_t>& bytes() const { return buf_; }

private:
    void align();
    std::vector<uint8_t> buf_;
};

// Tag numbers used by the CinemaDNG codec.
namespace tag {
inline constexpr uint16_t NewSubFileType = 254, ImageWidth = 256, ImageLength = 257, BitsPerSample = 258,
                          Compression = 259, Photometric = 262, Make = 271, Model = 272, StripOffsets = 273,
                          Orientation = 274, SamplesPerPixel = 277, RowsPerStrip = 278, StripByteCounts = 279,
                          PlanarConfiguration = 284, Software = 305, SampleFormat = 339, DateTime = 306, TileWidth = 322, TileLength = 323,
                          TileOffsets = 324, TileByteCounts = 325, SubIFDs = 330, CFARepeatPatternDim = 33421,
                          CFAPattern = 33422, ExposureTime = 33434, FNumber = 33437, ExifIFD = 34665,
                          ISOSpeedRatings = 34855, ExifVersion = 36864, FocalLength = 37386, LensModel = 42036,
                          DNGVersion = 50706, DNGBackwardVersion = 50707, UniqueCameraModel = 50708,
                          CFAPlaneColor = 50710, CFALayout = 50711, BlackLevelRepeatDim = 50713, BlackLevel = 50714,
                          LinearizationTable = 50712, BlackLevelDeltaH = 50715, BlackLevelDeltaV = 50716,
                          WhiteLevel = 50717, DefaultCropOrigin = 50719, DefaultCropSize = 50720, ColorMatrix1 = 50721, ColorMatrix2 = 50722, AsShotNeutral = 50728,
                          BaselineExposure = 50730, DNGPrivateData = 50740, ActiveArea = 50829, CalibrationIlluminant1 = 50778, CalibrationIlluminant2 = 50779,
                          OriginalRawFileName = 50827, TimeCodes = 51043, FrameRate = 51044, TStop = 51058,
                          ReelName = 51081, CameraLabel = 51105;
}

} // namespace r3dss::codec::tiff
