# Codec interface

Header: `src/codec/include/r3dss/codec/codec.hpp`. Every container/codec module implements three
classes and registers a factory. Modules never expose SDK types; the pipeline only sees
`BayerFrame`, `AudioBlock`, `ClipInfo`, `ClipMetadata` and `Expected<T>`.

## Types

```cpp
enum class Format { CinemaDNG, NEV, R3DNE, Memory, Unknown };

struct Capabilities {
    bool read, write;
    bool cuda;          // the codec itself can run on CUDA
    bool rateControl;   // writeFrame() honours FrameBudget
    bool lossless;      // writer can be mathematically lossless
    uint32_t dimAlignment;         // output W/H must be multiples of this (CinemaDNG 2, TicoRAW 8 unconfirmed)
    std::vector<uint16_t> bitDepths;
};

struct ClipInfo {
    Format format; std::filesystem::path path;
    uint32_t width, height; uint16_t bits; CfaPattern cfa; uint32_t black[4], white;
    Rational fps; int64_t frameCount; AudioInfo audio; std::optional<Timecode> startTimecode;
    ClipMetadata meta; uint64_t sourceBytes;
};

struct ClipMetadata {                 // passthrough model
    std::string cameraMake, cameraModel, cameraSerial, firmware;
    std::optional<uint32_t> iso; std::optional<double> exposureTimeSec;
    std::optional<WhiteBalance> whiteBalance;   // kelvin/tint and/or asShotNeutral[3]
    std::optional<ColorInfo> color;             // colorMatrix1/2, illuminants, colour-space name
    std::optional<LensInfo> lens;               // model, focal length, aperture, opaque correction blob
    std::map<std::string, MetaValue> vendor;    // named vendor values we understand (int64/double/string)
    std::vector<OpaqueBlob> opaque;             // byte blobs carried through untouched
};
```

## Reader

```cpp
class ICodecReader {
    Expected<ClipInfo> open(const std::filesystem::path&);
    const ClipInfo& info() const;
    Expected<void> readFrame(int64_t index, BayerFrame& out);   // `out` is reused by the caller
    Expected<AudioBlock> readAudio(int64_t firstSample, int64_t count); // fewer samples at the end
    Expected<Timecode> frameTimecode(int64_t index);
    void close();
};
```

Rules: `readFrame` may be called from one thread at a time per reader instance (the pipeline
uses one reader per clip on its reader thread). Frames carry their index and, when the container
has it, a per-frame timecode in `out.meta.timecode`.

## Writer

```cpp
struct WriterConfig { path dest; std::string clipName; uint32_t width, height; uint16_t bits; CfaPattern cfa;
                      uint32_t black[4], white; Rational fps; QualityProfile profile; AudioInfo audio;
                      std::optional<Timecode> startTimecode; ClipMetadata meta; bool lossless; };
struct FrameBudget  { uint64_t bits; bool lossless; };     // 0 bits = codec default
struct EncodedStats { uint64_t bytes; double bitsPerPixel; };
struct WriteSummary { int64_t framesWritten, audioSamplesWritten; uint64_t totalBytes; std::vector<path> files; };

class ICodecWriter {
    Expected<void> begin(const WriterConfig&);
    Expected<EncodedStats> writeFrame(const BayerFrame&, FrameBudget);   // called in frame order, one thread
    Expected<void> writeAudio(const AudioBlock&);                       // after or interleaved with frames
    Expected<WriteSummary> finish();
    void abort();                                                       // remove partial output
};
```

`dest` is a file for single-file containers (NEV) and a directory for CinemaDNG. `files` in the
summary lists every file produced so the verifier and the archive manifest can hash them.

## Factory and registry

```cpp
enum class ModuleState { Available, MissingSdk, MissingLicense, Stub };
struct ModuleStatus { ModuleState state; std::string text; bool usable() const; };

class ICodecFactory {
    Format format() const; std::string displayName() const;
    Capabilities caps() const; ModuleStatus status() const;
    bool probe(const std::filesystem::path&) const;   // extension + magic, never decodes
    std::unique_ptr<ICodecReader> makeReader() const;
    std::unique_ptr<ICodecWriter> makeWriter() const;
};
CodecRegistry::instance().registerFactory(f); byFormat(Format); probe(path); all();
void registerBuiltinCodecs();   // CinemaDNG, NEV, R3D NE, memory
```

## RED family module (today)

`Format::RedFamily` covers REDCODE RAW (`.R3D`), Nikon R3D NE (`.R3D`) and NEV. `ClipInfo` carries
`sourceKind` / `sourceLabel` detected from content (`codec_red/red_detect.hpp`), e.g.
`REDCODE RAW (KOMODO 6K)`. The factory lives in `codec_r3dsdk` and always registers:

- `probe()` = extension `.r3d` / `.nev` **and** a RED chunk header (`codec_red/red_container.hpp`).
- `makeReader()` returns the SDK-backed reader when the build has the RED R3D SDK and its runtime
  libraries were found (milestone 2); otherwise a container-only reader whose `open()` fills the
  container facts (`meta.vendor["container.*"]`, `header.timecode*`, camera model) and whose frame
  and audio calls fail with `ErrorCode::CodecUnavailable` and the SDK status text.
- `makeWriter()` always fails with `ErrorCode::Unsupported`: REDCODE cannot be written.
- `status()`: `MissingSdk` (not built / runtime not found) or `Available`.

The in-memory codec (`codec/memory_codec.hpp`, paths `mem://name`) implements the same interface
and is what the pipeline tests and the benchmark use.
