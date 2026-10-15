#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/core/paths.hpp"
#include <algorithm>
#include "r3dss/codec_cdng/cdng_codec.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {
void scanDir(const fs::path& dir, const fs::path& root, const fs::path& outRoot, const ScanOptions& o, ScanResult& res) {
    std::error_code ec;
    auto& reg = codec::CodecRegistry::instance();
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    const fs::path rel = fs::relative(dir, root, ec);
    const fs::path outDir = o.preserveStructure && !rel.empty() && rel != "." ? outRoot / rel : outRoot;
    for (const auto& p : entries) {
        if (fs::is_directory(p, ec)) {
            if (!codec::listDngFrames(p).empty()) { // CinemaDNG clip directory
                ClipJob cj;
                cj.source = p;
                cj.outputDir = outDir;
                cj.relativeDir = rel;
                res.clips.push_back(cj);
            } else if (o.recursive) {
                scanDir(p, root, outRoot, o, res);
            }
            continue;
        }
        if (!fs::is_regular_file(p, ec)) continue;
        auto f = reg.probe(p);
        if (f && f->format() != codec::Format::CinemaDNG) {
            ClipJob cj;
            cj.source = p;
            cj.outputDir = outDir;
            cj.relativeDir = rel;
            res.clips.push_back(cj);
        } else if (o.archiveMode) {
            res.copies.push_back({p, outDir / p.filename()});
        }
    }
}
} // namespace

Expected<ScanResult> scanFolder(const fs::path& root, const fs::path& outputRoot, const ScanOptions& opts) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return makeError(ErrorCode::InvalidArgument, "not a directory: " + toUtf8(root));
    ScanResult res;
    if (!codec::listDngFrames(root).empty()) {
        ClipJob cj;
        cj.source = root;
        cj.outputDir = outputRoot;
        res.clips.push_back(cj);
        return res;
    }
    scanDir(root, root, outputRoot, opts, res);
    return res;
}

} // namespace r3dss::pipeline
