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

## Stub behaviour (today)

`NEV` and `R3D NE` factories (`codec_nev`): `probe()` accepts `.nev` / `.r3d` whose first chunk
tag starts with `RED`; `makeReader()->open()` lists top-level chunks (count, first tag) into
`meta.vendor["container.*"]`; `readFrame`, `readAudio`, `frameTimecode` return
`ErrorCode::CodecUnavailable` with a sentence naming the missing SDK; the writer returns the
same for NEV and `ErrorCode::Unsupported` ("no public REDCODE encoder exists") for R3D NE.
`status()` is `MissingSdk` with the text shown in Settings → Codec modules.

## Adding the TicoRAW module (tasks 10–12, 22)

1. Implement `ITicoRawCodec` (`codec_nev/nev_codec.hpp`) against the intoPIX SDK behind
   `R3DSS_WITH_TICORAW`; `makeTicoRawCodec()` returns it.
2. Replace the chunk walker's opaque payloads with real header/index/frame parsing learned from
   samples (task 10).
3. Map container metadata into `ClipMetadata` (task 12) and keep unknown blobs in `opaque`.
4. The factory's `caps()` already flips `read/write/cuda/lossless` when the adapter exists.

The in-memory codec (`codec/memory_codec.hpp`, paths `mem://name`) implements the same interface
and is what the pipeline tests and the benchmark use.
