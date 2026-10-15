#include "r3dss/pipeline/archive_copier.hpp"
#include "r3dss/core/paths.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include "r3dss/core/logger.hpp"
#include "r3dss/core/sha256.hpp"
#include "r3dss/pipeline/output_claim.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

namespace {

Expected<std::string> hashFresh(const fs::path& p, const std::shared_ptr<CancellationToken>& cancel) {
    flushAndDropCache(p, false);
    return Sha256::hashFile(p, cancel.get());
}

} // namespace

Expected<CopyResult> copyAndVerify(const fs::path& source, const fs::path& dest,
                                   const std::shared_ptr<CancellationToken>& cancel,
                                   const std::function<void(uint64_t, uint64_t)>& progress) {
    CopyResult r;
    r.source = source;
    r.dest = dest;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return makeError(ErrorCode::Io, "not a readable file: " + toUtf8(source));
    if (pathsOverlap(source, dest))
        return makeError(ErrorCode::InvalidArgument, "source and destination are the same file: " + toUtf8(source));
    const uint64_t total = fs::file_size(source, ec);

    if (fs::exists(fs::symlink_status(dest, ec))) {
        // Never replace an existing file: identical content counts as already archived.
        R3DSS_TRY_ASSIGN(srcHash, hashFresh(source, cancel));
        R3DSS_TRY_ASSIGN(dstHash, hashFresh(dest, cancel));
        if (srcHash != dstHash)
            return makeError(ErrorCode::Verification,
                             "destination exists with different content (not replaced): " + toUtf8(dest));
        r.sha256 = r.sourceSha256 = r.destSha256 = srcHash;
        r.bytes = total;
        r.verified = true;
        r.alreadyPresent = true;
        return r;
    }

    fs::create_directories(dest.parent_path(), ec);
    if (ec) return makeError(ErrorCode::Io, "cannot create " + toUtf8(dest.parent_path()) + ": " + ec.message());
    std::ifstream in(source, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + toUtf8(source));
    fs::path tmp = dest;
    tmp += ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::Io, "cannot create " + toUtf8(tmp));
    auto failTmp = [&](ErrorCode code, const std::string& msg) {
        if (out.is_open()) out.close();
        std::error_code e2;
        fs::remove(tmp, e2);
        return makeError(code, msg);
    };
    Sha256 h;
    std::vector<char> buf(4u << 20);
    uint64_t done = 0;
    for (;;) {
        if (cancel && cancel->isCancelled()) return failTmp(ErrorCode::Cancelled, "copy cancelled");
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (in.bad()) return failTmp(ErrorCode::Io, "read failed: " + toUtf8(source));
        if (n <= 0) break;
        h.update(buf.data(), static_cast<size_t>(n));
        out.write(buf.data(), n);
        if (!out) return failTmp(ErrorCode::Io, "write failed: " + toUtf8(tmp));
        done += static_cast<uint64_t>(n);
        if (progress) progress(done, total);
        if (in.eof()) break;
    }
    out.close();
    if (!out) return failTmp(ErrorCode::Io, "write failed: " + toUtf8(tmp));
    const std::string streamHash = Sha256::hex(h.finish());

    // Independent verification: the copy read back from the device, and a second read of the source.
    flushAndDropCache(tmp, true);
    auto dstHash = hashFresh(tmp, cancel);
    if (!dstHash) return failTmp(dstHash.error().code, dstHash.error().message);
    auto srcHash = hashFresh(source, cancel);
    if (!srcHash) return failTmp(srcHash.error().code, srcHash.error().message);
    r.bytes = done;
    r.sourceSha256 = *srcHash;
    r.destSha256 = *dstHash;
    r.sha256 = *dstHash;
    r.verified = streamHash == *dstHash && *srcHash == *dstHash;
    if (!r.verified)
        return failTmp(ErrorCode::Verification, "copy of " + toUtf8(source) + " does not verify: copied data " +
                                                    streamHash.substr(0, 16) + ", copy on disk " + dstHash->substr(0, 16) +
                                                    ", source re-read " + srcHash->substr(0, 16));
    if (auto mv = renameNoReplace(tmp, dest); !mv) return failTmp(ErrorCode::Io, mv.error().message);
    // Preserve timestamps where possible.
    fs::last_write_time(dest, fs::last_write_time(source, ec), ec);
    return r;
}

Expected<void> writeManifest(const fs::path& path, const std::vector<CopyResult>& copies, const std::vector<fs::path>& produced,
                             bool complete) {
    nlohmann::json j;
    j["complete"] = complete;
    j["copies"] = nlohmann::json::array();
    std::error_code ec;
    for (const auto& c : copies) {
        if (!c.verified || !fs::is_regular_file(c.dest, ec)) {
            R3DSS_LOG_WARN("manifest", "left out (not present): " + toUtf8(c.dest));
            continue;
        }
        j["copies"].push_back({{"source", toUtf8Generic(c.source)}, {"dest", toUtf8Generic(c.dest)},
                               {"sha256", c.sha256}, {"sourceSha256", c.sourceSha256}, {"destSha256", c.destSha256},
                               {"bytes", c.bytes}, {"verified", c.verified}, {"alreadyArchived", c.alreadyPresent}});
    }
    j["produced"] = nlohmann::json::array();
    for (const auto& p : produced) {
        if (!fs::is_regular_file(p, ec)) {
            R3DSS_LOG_WARN("manifest", "left out (not present): " + toUtf8(p));
            continue;
        }
        auto h = Sha256::hashFile(p);
        if (!h) {
            R3DSS_LOG_WARN("manifest", "left out (unreadable): " + toUtf8(p) + ": " + h.error().toString());
            continue;
        }
        j["produced"].push_back({{"path", toUtf8Generic(p)}, {"sha256", *h}, {"bytes", fs::file_size(p, ec)}});
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::Io, "cannot create manifest " + toUtf8(path));
    out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    out.close();
    if (!out) return makeError(ErrorCode::Io, "cannot write manifest " + toUtf8(path));
    return {};
}

} // namespace r3dss::pipeline
