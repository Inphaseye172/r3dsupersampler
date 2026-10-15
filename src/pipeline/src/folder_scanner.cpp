#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/core/paths.hpp"
#include <algorithm>
#include <set>
#include "r3dss/codec_cdng/cdng_codec.hpp"
#include "r3dss/pipeline/output_claim.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = toUtf8(p.extension());
    for (char& c : e)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

bool isDotName(const fs::path& p) {
    const std::string n = toUtf8(p.filename());
    return !n.empty() && n[0] == '.';
}

class Scanner {
public:
    Scanner(const fs::path& root, const fs::path& outRoot, const ScanOptions& o, ScanResult& res)
        : root_(root), outRoot_(outRoot), outKey_(outRoot.empty() ? std::string() : pathKey(outRoot)), o_(o), res_(res) {}

    void scanDir(const fs::path& dir) {
        if (!firstVisit(dir)) return;
        std::error_code ec;
        const fs::path outDir = outDirFor(dir);
        const fs::path rel = relFor(dir);
        auto& reg = codec::CodecRegistry::instance();
        for (const auto& p : sortedEntries(dir)) {
            if (fs::is_directory(p, ec)) {
                if (excluded(p)) continue;
                if (!codec::listDngFrames(p).empty()) addClipDir(p, outDir, rel); // CinemaDNG clip directory
                else if (o_.recursive) scanDir(p);
                continue;
            }
            if (!fs::is_regular_file(p, ec)) continue;
            auto f = reg.probe(p);
            if (f && f->format() != codec::Format::CinemaDNG) {
                ClipJob cj;
                cj.source = p;
                cj.outputDir = outDir;
                cj.relativeDir = rel;
                res_.clips.push_back(cj);
            } else if (o_.archiveMode) {
                res_.copies.push_back({p, outDir / p.filename()});
            }
        }
    }

    // A folder holding CinemaDNG frames. Its other files become sidecars (archive mode) and folders
    // inside it that hold frames of their own are clips too (when scanning recursively).
    void addClipDir(const fs::path& clipDir, const fs::path& outDir, const fs::path& rel) {
        if (!firstVisit(clipDir)) return;
        ClipJob cj;
        cj.source = clipDir;
        cj.outputDir = outDir;
        cj.relativeDir = rel;
        std::vector<fs::path> nested;
        collect(clipDir, clipDir, o_.archiveMode ? &cj.sidecars : nullptr, nested);
        res_.clips.push_back(cj);
        if (!o_.recursive) return;
        for (const auto& n : nested) addClipDir(n, outDirFor(n.parent_path()), relFor(n.parent_path()));
    }

private:
    void collect(const fs::path& clipDir, const fs::path& dir, std::vector<fs::path>* sidecars, std::vector<fs::path>& nested) {
        std::error_code ec;
        for (const auto& p : sortedEntries(dir)) {
            if (isDotName(p)) continue; // .DS_Store, AppleDouble "._*" files, .Trashes ...
            if (fs::is_directory(p, ec)) {
                if (excluded(p)) continue;
                if (!codec::listDngFrames(p).empty()) nested.push_back(p);
                else if (firstVisit(p)) collect(clipDir, p, sidecars, nested);
                continue;
            }
            if (!sidecars || !fs::is_regular_file(p, ec)) continue;
            const std::string ext = lowerExt(p);
            if (ext == ".dng") continue;                   // frames
            if (ext == ".wav" && dir == clipDir) continue; // the clip's audio: carried by the converter
            sidecars->push_back(p.lexically_relative(clipDir));
        }
    }

    static std::vector<fs::path> sortedEntries(const fs::path& dir) {
        std::error_code ec;
        std::vector<fs::path> entries;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            entries.push_back(it->path());
        std::sort(entries.begin(), entries.end());
        return entries;
    }

    // Symlinked folders can form loops; every real folder is visited once.
    bool firstVisit(const fs::path& dir) { return visited_.insert(pathKey(dir)).second; }

    // The output root (and anything inside it) is never scanned, so earlier outputs are not re-read.
    bool excluded(const fs::path& dir) const {
        if (outKey_.empty()) return false;
        const std::string k = pathKey(dir);
        return k == outKey_ || (k.size() > outKey_.size() && k.compare(0, outKey_.size(), outKey_) == 0 &&
                                (outKey_.back() == '/' || k[outKey_.size()] == '/'));
    }

    fs::path relFor(const fs::path& dir) const {
        std::error_code ec;
        fs::path rel = fs::relative(dir, root_, ec);
        if (ec || rel == "." || (!rel.empty() && *rel.begin() == "..")) rel.clear();
        return rel;
    }
    fs::path outDirFor(const fs::path& dir) const {
        const fs::path rel = relFor(dir);
        return o_.preserveStructure && !rel.empty() ? outRoot_ / rel : outRoot_;
    }

    fs::path root_, outRoot_;
    std::string outKey_;
    const ScanOptions& o_;
    ScanResult& res_;
    std::set<std::string> visited_;
};

} // namespace

Expected<ScanResult> scanFolder(const fs::path& rootIn, const fs::path& outputRoot, const ScanOptions& opts) {
    const fs::path root = normaliseSourcePath(rootIn);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return makeError(ErrorCode::InvalidArgument, "not a directory: " + toUtf8(root));
    if (!outputRoot.empty() && pathKey(root) == pathKey(outputRoot))
        return makeError(ErrorCode::InvalidArgument,
                         "the output folder must differ from the scanned folder: " + toUtf8(root));
    ScanResult res;
    Scanner s(root, normaliseSourcePath(outputRoot), opts, res);
    if (!codec::listDngFrames(root).empty()) s.addClipDir(root, normaliseSourcePath(outputRoot), {});
    else s.scanDir(root);
    return res;
}

} // namespace r3dss::pipeline
