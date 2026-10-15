#include "r3dss/pipeline/archive_copier.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include "r3dss/core/sha256.hpp"

namespace r3dss::pipeline {

namespace fs = std::filesystem;

Expected<CopyResult> copyAndVerify(const fs::path& source, const fs::path& dest,
                                   const std::shared_ptr<CancellationToken>& cancel,
                                   const std::function<void(uint64_t, uint64_t)>& progress) {
    CopyResult r;
    r.source = source;
    r.dest = dest;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ifstream in(source, std::ios::binary);
    if (!in) return makeError(ErrorCode::Io, "cannot open " + source.string());
    const fs::path tmp = dest.string() + ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::Io, "cannot create " + tmp.string());
    const uint64_t total = fs::file_size(source, ec);
    Sha256 h;
    std::vector<char> buf(4u << 20);
    uint64_t done = 0;
    while (in) {
        if (cancel && cancel->isCancelled()) {
            out.close();
            fs::remove(tmp, ec);
            return makeError(ErrorCode::Cancelled, "copy cancelled");
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        h.update(buf.data(), static_cast<size_t>(n));
        out.write(buf.data(), n);
        if (!out) {
            fs::remove(tmp, ec);
            return makeError(ErrorCode::Io, "write failed: " + tmp.string());
        }
        done += static_cast<uint64_t>(n);
        if (progress) progress(done, total);
    }
    out.close();
    r.bytes = done;
    r.sha256 = Sha256::hex(h.finish());
    fs::rename(tmp, dest, ec);
    if (ec) return makeError(ErrorCode::Io, "rename failed: " + dest.string());
    R3DSS_TRY_ASSIGN(check, Sha256::hashFile(dest));
    r.verified = check == r.sha256;
    if (!r.verified) {
        r.error = "hash mismatch after copy";
        return makeError(ErrorCode::Verification, "copied file hash mismatch: " + dest.string());
    }
    // Preserve timestamps where possible.
    fs::last_write_time(dest, fs::last_write_time(source, ec), ec);
    return r;
}

Expected<void> writeManifest(const fs::path& path, const std::vector<CopyResult>& copies,
                             const std::vector<fs::path>& produced) {
    nlohmann::json j;
    j["copies"] = nlohmann::json::array();
    for (const auto& c : copies)
        j["copies"].push_back({{"source", c.source.generic_string()}, {"dest", c.dest.generic_string()},
                               {"sha256", c.sha256}, {"bytes", c.bytes}, {"verified", c.verified}});
    j["produced"] = nlohmann::json::array();
    for (const auto& p : produced) {
        auto h = Sha256::hashFile(p);
        std::error_code ec;
        j["produced"].push_back({{"path", p.generic_string()}, {"sha256", h ? *h : ""},
                                 {"bytes", std::filesystem::exists(p, ec) ? std::filesystem::file_size(p, ec) : 0}});
    }
    std::ofstream out(path);
    if (!out) return makeError(ErrorCode::Io, "cannot create manifest " + path.string());
    out << j.dump(2);
    return {};
}

} // namespace r3dss::pipeline
