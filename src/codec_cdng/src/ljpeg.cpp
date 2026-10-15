#include "r3dss/codec_cdng/ljpeg.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace r3dss::codec::ljpeg {

namespace {

struct HuffTable {
    // Canonical decoding tables (T.81 F.2.2.3)
    std::array<int32_t, 18> maxcode{};
    std::array<int32_t, 17> valptr{};
    std::array<int32_t, 17> mincode{};
    std::vector<uint8_t> vals;
    bool present = false;
    // 9-bit fast lookup: (length << 8) | value, 0 = not resolvable in 9 bits
    std::array<uint16_t, 512> fast{};
};

struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t acc = 0;
    int bits = 0;
    bool hitMarker = false;

    void fill() {
        while (bits <= 24) {
            uint32_t byte = 0;
            if (p < end && !hitMarker) {
                byte = *p;
                if (byte == 0xFF) {
                    const uint8_t next = p + 1 < end ? p[1] : 0;
                    if (next == 0x00) p += 2;            // stuffed byte
                    else { hitMarker = true; byte = 0; } // marker: stop consuming
                } else {
                    ++p;
                }
            }
            acc |= byte << (24 - bits);
            bits += 8;
        }
    }
    uint32_t peek(int n) { if (bits < n) fill(); return acc >> (32 - n); }
    void skip(int n) { acc <<= n; bits -= n; }
    uint32_t get(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        skip(n);
        return v;
    }
    void alignAndResync() { acc = 0; bits = 0; hitMarker = false; }
};

bool buildTable(HuffTable& t, const uint8_t* bitsCount /*[1..16]*/, const uint8_t* vals, size_t nvals) {
    t.vals.assign(vals, vals + nvals);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
        const int n = bitsCount[l];
        if (n) {
            t.valptr[l] = k;
            t.mincode[l] = code;
            code += n;
            k += n;
            t.maxcode[l] = code - 1;
        } else {
            t.maxcode[l] = -1;
        }
        code <<= 1;
    }
    t.maxcode[17] = 0x7fffffff;
    t.fast.fill(0);
    // populate fast lookup
    code = 0;
    k = 0;
    for (int l = 1; l <= 9; ++l) {
        for (int i = 0; i < bitsCount[l]; ++i, ++k) {
            const int shift = 9 - l;
            const int base = code << shift;
            for (int j = 0; j < (1 << shift); ++j)
                if (base + j < 512) t.fast[static_cast<size_t>(base + j)] = static_cast<uint16_t>((l << 8) | t.vals[static_cast<size_t>(k)]);
            ++code;
        }
        code <<= 1;
    }
    t.present = k <= static_cast<int>(nvals);
    return t.present;
}

int decodeSymbol(BitReader& br, const HuffTable& t) {
    const uint32_t look = br.peek(9);
    const uint16_t f = t.fast[look];
    if (f) {
        br.skip(f >> 8);
        return f & 0xff;
    }
    int code = static_cast<int>(br.get(9));
    int l = 9;
    while (l <= 16 && code > t.maxcode[l]) {
        code = (code << 1) | static_cast<int>(br.get(1));
        ++l;
    }
    if (l > 16) return -1;
    const int idx = t.valptr[l] + code - t.mincode[l];
    if (idx < 0 || idx >= static_cast<int>(t.vals.size())) return -1;
    return t.vals[static_cast<size_t>(idx)];
}

inline int extend(uint32_t v, int ssss) {
    if (ssss == 0) return 0;
    if (ssss == 16) return 32768;
    const int half = 1 << (ssss - 1);
    return static_cast<int>(v) < half ? static_cast<int>(v) - (1 << ssss) + 1 : static_cast<int>(v);
}

inline int predict(int predictor, int ra, int rb, int rc) {
    switch (predictor) {
    case 1: return ra;
    case 2: return rb;
    case 3: return rc;
    case 4: return ra + rb - rc;
    case 5: return ra + ((rb - rc) >> 1);
    case 6: return rb + ((ra - rc) >> 1);
    case 7: return (ra + rb) >> 1;
    default: return ra;
    }
}

} // namespace

Expected<Image> decode(const uint8_t* data, size_t len) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return makeError(ErrorCode::Format, "LJPEG: missing SOI");
    Image img;
    std::array<HuffTable, 4> tables;
    struct Comp { uint8_t id, h, v, tq, td; };
    std::vector<Comp> comps;
    int predictor = 1, pt = 0;
    uint32_t restartInterval = 0;
    size_t pos = 2;
    bool sosFound = false;
    while (pos + 4 <= len && !sosFound) {
        if (data[pos] != 0xFF) return makeError(ErrorCode::Format, "LJPEG: marker expected");
        const uint8_t m = data[pos + 1];
        if (m == 0xFF) { ++pos; continue; }
        pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (pos + 2 > len) return makeError(ErrorCode::Format, "LJPEG: truncated segment");
        const size_t segLen = (size_t(data[pos]) << 8) | data[pos + 1];
        if (segLen < 2 || pos + segLen > len) return makeError(ErrorCode::Format, "LJPEG: bad segment length");
        const uint8_t* s = data + pos + 2;
        const size_t sl = segLen - 2;
        switch (m) {
        case 0xC3: { // SOF3
            if (sl < 6) return makeError(ErrorCode::Format, "LJPEG: short SOF3");
            img.precision = s[0];
            img.height = (uint32_t(s[1]) << 8) | s[2];
            const uint32_t columns = (uint32_t(s[3]) << 8) | s[4];
            const uint8_t nf = s[5];
            if (nf < 1 || nf > 4 || sl < 6u + 3u * nf) return makeError(ErrorCode::Format, "LJPEG: bad component count");
            for (int i = 0; i < nf; ++i) {
                Comp c{};
                c.id = s[6 + i * 3];
                c.h = s[7 + i * 3] >> 4;
                c.v = s[7 + i * 3] & 15;
                c.tq = s[8 + i * 3];
                comps.push_back(c);
            }
            img.components = nf;
            img.width = columns * nf;
            break;
        }
        case 0xC4: { // DHT
            size_t o = 0;
            while (o + 17 <= sl) {
                const uint8_t tc = s[o] >> 4, th = s[o] & 15;
                (void)tc;
                if (th > 3) return makeError(ErrorCode::Format, "LJPEG: bad DHT id");
                uint8_t counts[17] = {0};
                size_t total = 0;
                for (int i = 1; i <= 16; ++i) { counts[i] = s[o + static_cast<size_t>(i)]; total += counts[i]; }
                if (o + 17 + total > sl) return makeError(ErrorCode::Format, "LJPEG: DHT truncated");
                buildTable(tables[th], counts, s + o + 17, total);
                o += 17 + total;
            }
            break;
        }
        case 0xDD: // DRI
            if (sl >= 2) restartInterval = (uint32_t(s[0]) << 8) | s[1];
            break;
        case 0xDA: { // SOS
            if (sl < 1) return makeError(ErrorCode::Format, "LJPEG: short SOS");
            const uint8_t ns = s[0];
            if (ns != comps.size() || sl < 1u + 2u * ns + 3u) return makeError(ErrorCode::Format, "LJPEG: SOS/SOF mismatch");
            for (int i = 0; i < ns; ++i) {
                const uint8_t cs = s[1 + i * 2];
                const uint8_t td = s[2 + i * 2] >> 4;
                bool found = false;
                for (auto& c : comps) if (c.id == cs) { c.td = td; found = true; }
                if (!found) return makeError(ErrorCode::Format, "LJPEG: unknown scan component");
            }
            predictor = s[1 + ns * 2];
            pt = s[3 + ns * 2] & 15;
            sosFound = true;
            break;
        }
        default: break; // APPn, COM, etc.
        }
        pos += segLen;
    }
    if (!sosFound || img.width == 0 || img.height == 0) return makeError(ErrorCode::Format, "LJPEG: no scan");
    if (img.precision < 2 || img.precision > 16) return makeError(ErrorCode::Format, "LJPEG: bad precision");
    for (auto& c : comps)
        if (!tables[c.td].present) return makeError(ErrorCode::Format, "LJPEG: missing Huffman table");
    if (predictor < 1 || predictor > 7) return makeError(ErrorCode::Format, "LJPEG: bad predictor");

    const uint32_t nc = img.components;
    const uint32_t cols = img.width / nc;
    img.samples.resize(static_cast<size_t>(img.width) * img.height);
    BitReader br{data + pos, data + len};
    const int mask = (1 << img.precision) - 1;
    const int initial = 1 << (img.precision - pt - 1);
    uint32_t mcusToRestart = restartInterval;
    bool rowResetPending = true; // first row (or first after restart) uses predictor 1 / initial

    for (uint32_t y = 0; y < img.height; ++y) {
        uint16_t* row = img.samples.data() + static_cast<size_t>(y) * img.width;
        const uint16_t* prev = y ? row - img.width : nullptr;
        const bool firstRow = rowResetPending;
        rowResetPending = false;
        for (uint32_t x = 0; x < cols; ++x) {
            if (restartInterval && mcusToRestart == 0) {
                // Expect RSTn marker: realign, skip marker bytes, reset predictor state.
                br.alignAndResync();
                while (br.p + 1 < br.end && !(br.p[0] == 0xFF && br.p[1] >= 0xD0 && br.p[1] <= 0xD7)) ++br.p;
                if (br.p + 1 < br.end) br.p += 2;
                mcusToRestart = restartInterval;
                // After a restart the current row behaves like a first row.
                prev = nullptr;
            }
            for (uint32_t c = 0; c < nc; ++c) {
                const int ssss = decodeSymbol(br, tables[comps[c].td]);
                if (ssss < 0) return makeError(ErrorCode::Format, "LJPEG: bad Huffman code");
                const int diff = extend(br.get(ssss == 16 ? 0 : ssss), ssss);
                int pred;
                const size_t i = static_cast<size_t>(x) * nc + c;
                if (x == 0) {
                    pred = (firstRow || !prev) ? initial : prev[i];
                } else if (firstRow || !prev) {
                    pred = row[i - nc];
                } else {
                    const int ra = row[i - nc], rb = prev[i], rc = prev[i - nc];
                    pred = predict(predictor, ra, rb, rc);
                }
                row[i] = static_cast<uint16_t>((pred + diff) & mask);
            }
            if (restartInterval) --mcusToRestart;
        }
    }
    return img;
}

// ---------------------------------------------------------------------------------------------
namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int bits = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t v, int n) {
        while (n > 0) {
            const int take = std::min(n, 24 - bits);
            acc = (acc << take) | ((v >> (n - take)) & ((1u << take) - 1));
            bits += take;
            n -= take;
            while (bits >= 8) {
                const uint8_t b = static_cast<uint8_t>(acc >> (bits - 8));
                out.push_back(b);
                if (b == 0xFF) out.push_back(0x00);
                bits -= 8;
                acc &= (1u << bits) - 1;
            }
        }
    }
    void flush() {
        if (bits > 0) put((1u << (8 - bits)) - 1, 8 - bits); // pad with 1s
    }
};

// Annex K.2: derive code lengths (max 16) from symbol frequencies.
void buildCodeLengths(const std::array<uint32_t, 18>& freqIn, std::array<uint8_t, 17>& bitsOut,
                      std::vector<uint8_t>& valsOut) {
    std::array<int64_t, 257> freq{};
    std::array<int, 257> codesize{}, others{};
    for (int i = 0; i < 17; ++i) freq[static_cast<size_t>(i)] = freqIn[static_cast<size_t>(i)];
    freq[256] = 1; // reserved symbol guarantees no all-ones code
    others.fill(-1);
    for (;;) {
        int v1 = -1, v2 = -1;
        for (int i = 0; i < 257; ++i)
            if (freq[static_cast<size_t>(i)] > 0 && (v1 < 0 || freq[static_cast<size_t>(i)] <= freq[static_cast<size_t>(v1)])) v1 = i;
        for (int i = 0; i < 257; ++i)
            if (freq[static_cast<size_t>(i)] > 0 && i != v1 && (v2 < 0 || freq[static_cast<size_t>(i)] <= freq[static_cast<size_t>(v2)])) v2 = i;
        if (v2 < 0) break;
        freq[static_cast<size_t>(v1)] += freq[static_cast<size_t>(v2)];
        freq[static_cast<size_t>(v2)] = 0;
        for (int i = v1;; i = others[static_cast<size_t>(i)]) {
            ++codesize[static_cast<size_t>(i)];
            if (others[static_cast<size_t>(i)] < 0) { others[static_cast<size_t>(i)] = v2; break; }
        }
        for (int i = v2;; i = others[static_cast<size_t>(i)]) {
            ++codesize[static_cast<size_t>(i)];
            if (others[static_cast<size_t>(i)] < 0) break;
        }
    }
    std::array<int, 33> bits{};
    for (int i = 0; i < 257; ++i)
        if (codesize[static_cast<size_t>(i)]) ++bits[static_cast<size_t>(codesize[static_cast<size_t>(i)])];
    // Limit to 16 bits (K.3)
    for (int i = 32; i > 16; --i) {
        while (bits[static_cast<size_t>(i)] > 0) {
            int j = i - 2;
            while (bits[static_cast<size_t>(j)] == 0) --j;
            bits[static_cast<size_t>(i)] -= 2;
            bits[static_cast<size_t>(i - 1)] += 1;
            bits[static_cast<size_t>(j + 1)] += 2;
            bits[static_cast<size_t>(j)] -= 1;
        }
    }
    int i = 16;
    while (bits[static_cast<size_t>(i)] == 0) --i;
    bits[static_cast<size_t>(i)] -= 1; // remove reserved symbol
    bitsOut.fill(0);
    for (int l = 1; l <= 16; ++l) bitsOut[static_cast<size_t>(l)] = static_cast<uint8_t>(bits[static_cast<size_t>(l)]);
    valsOut.clear();
    for (int len = 1; len <= 32; ++len)
        for (int s = 0; s < 256; ++s)
            if (codesize[static_cast<size_t>(s)] == len) valsOut.push_back(static_cast<uint8_t>(s));
}

inline int category(int diff) {
    if (diff == 32768) return 16;
    int a = diff < 0 ? -diff : diff;
    int n = 0;
    while (a) { ++n; a >>= 1; }
    return n;
}

} // namespace

Expected<std::vector<uint8_t>> encode(const uint16_t* samples, uint32_t width, uint32_t height, const EncodeParams& p) {
    if (p.components < 1 || p.components > 4 || width % p.components)
        return makeError(ErrorCode::InvalidArgument, "LJPEG: width must be a multiple of the component count");
    if (p.precision < 2 || p.precision > 16) return makeError(ErrorCode::InvalidArgument, "LJPEG: bad precision");
    if (width == 0 || height == 0 || width / p.components > 65535 || height > 65535)
        return makeError(ErrorCode::InvalidArgument, "LJPEG: bad dimensions");
    const uint32_t nc = p.components;
    const uint32_t cols = width / nc;
    const int initial = 1 << (p.precision - 1);
    const int wrapMask = (1 << p.precision) - 1;

    // Pass 1: diffs and statistics.
    std::vector<int32_t> diffs(static_cast<size_t>(width) * height);
    std::array<uint32_t, 18> freq{};
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t* row = samples + static_cast<size_t>(y) * width;
        const uint16_t* prev = y ? row - width : nullptr;
        int32_t* drow = diffs.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < cols; ++x) {
            for (uint32_t c = 0; c < nc; ++c) {
                const size_t i = static_cast<size_t>(x) * nc + c;
                int pred;
                if (x == 0) pred = prev ? prev[i] : initial;
                else if (!prev) pred = row[i - nc];
                else pred = predict(p.predictor, row[i - nc], prev[i], prev[i - nc]);
                int d = (static_cast<int>(row[i]) - pred) & wrapMask;
                if (p.precision == 16) {
                    if (d >= 32768 && d != 32768) d -= 65536;
                } else if (d > (wrapMask >> 1)) {
                    d -= (wrapMask + 1);
                }
                drow[i] = d;
                ++freq[static_cast<size_t>(category(d))];
            }
        }
    }
    std::array<uint8_t, 17> bits{};
    std::vector<uint8_t> vals;
    buildCodeLengths(freq, bits, vals);
    // Canonical codes
    std::array<uint16_t, 17> code{}, codeLen{};
    {
        int c = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            for (int n = 0; n < bits[static_cast<size_t>(l)]; ++n, ++k) {
                code[vals[static_cast<size_t>(k)]] = static_cast<uint16_t>(c++);
                codeLen[vals[static_cast<size_t>(k)]] = static_cast<uint16_t>(l);
            }
            c <<= 1;
        }
    }

    std::vector<uint8_t> out;
    out.reserve(diffs.size() + 1024);
    auto marker = [&](uint8_t m) { out.push_back(0xFF); out.push_back(m); };
    auto be16 = [&](uint32_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); };
    marker(0xD8);
    marker(0xC3);
    be16(8 + 3 * nc);
    out.push_back(static_cast<uint8_t>(p.precision));
    be16(height);
    be16(cols);
    out.push_back(static_cast<uint8_t>(nc));
    for (uint32_t c = 0; c < nc; ++c) { out.push_back(static_cast<uint8_t>(c + 1)); out.push_back(0x11); out.push_back(0); }
    marker(0xC4);
    be16(static_cast<uint32_t>(2 + 1 + 16 + vals.size()));
    out.push_back(0x00);
    for (int l = 1; l <= 16; ++l) out.push_back(bits[static_cast<size_t>(l)]);
    out.insert(out.end(), vals.begin(), vals.end());
    marker(0xDA);
    be16(6 + 2 * nc);
    out.push_back(static_cast<uint8_t>(nc));
    for (uint32_t c = 0; c < nc; ++c) { out.push_back(static_cast<uint8_t>(c + 1)); out.push_back(0x00); }
    out.push_back(p.predictor);
    out.push_back(0);
    out.push_back(0);

    BitWriter bw(out);
    for (int32_t d : diffs) {
        const int cat = category(d);
        bw.put(code[static_cast<size_t>(cat)], codeLen[static_cast<size_t>(cat)]);
        if (cat > 0 && cat < 16) {
            uint32_t v = d < 0 ? static_cast<uint32_t>(d - 1) : static_cast<uint32_t>(d);
            bw.put(v & ((1u << cat) - 1), cat);
        }
    }
    bw.flush();
    marker(0xD9);
    return out;
}

} // namespace r3dss::codec::ljpeg
