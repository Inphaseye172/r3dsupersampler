#include "r3dss/codec_cdng/ljpeg.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace r3dss::codec::ljpeg {

namespace {

Error formatError(const char* what) { return Error(ErrorCode::Format, std::string("LJPEG: ") + what); }

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

// Reads one entropy-coded segment. A marker (or the end of the data) ends the segment; reads past
// it return zero bits, and consuming any of those marks the segment as truncated.
struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t acc = 0;
    int bits = 0;  // bits held in acc
    int fake = 0;  // of those, the zero bits appended after the end of the segment (always the lowest)
    bool hitMarker = false;
    bool overrun = false;

    void fill() {
        while (bits <= 24) {
            uint32_t byte = 0;
            bool real = false;
            if (p < end && !hitMarker) {
                if (*p != 0xFF) {
                    byte = *p++;
                    real = true;
                } else if (p + 1 < end && p[1] == 0x00) { // stuffed 0xFF
                    byte = 0xFF;
                    p += 2;
                    real = true;
                } else {
                    hitMarker = true; // a marker (or a lone 0xFF): stop consuming, p stays on it
                }
            }
            acc |= byte << (24 - bits);
            bits += 8;
            if (!real) fake += 8;
        }
    }
    uint32_t peek(int n) {
        if (bits < n) fill();
        return acc >> (32 - n);
    }
    void skip(int n) {
        acc <<= n;
        bits -= n;
        if (bits < fake) {
            overrun = true;
            fake = bits;
        }
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        skip(n);
        return v;
    }
    // Unconsumed bits that came from the data. At the end of a segment only the padding bits of
    // the last byte (fewer than 8) may remain.
    int pendingRealBits() const { return bits - fake; }
    void restartAt(const uint8_t* q) {
        p = q;
        acc = 0;
        bits = 0;
        fake = 0;
        hitMarker = false;
    }
};

// Returns the marker code at q (skipping 0xFF fill bytes; q is left on the marker's 0xFF), or -1
// when q does not point at a marker.
int markerAt(const uint8_t*& q, const uint8_t* end) {
    if (q >= end || *q != 0xFF) return -1;
    while (q + 1 < end && q[1] == 0xFF) ++q;
    if (q + 1 >= end || q[1] == 0x00) return -1;
    return q[1];
}

bool buildTable(HuffTable& t, const uint8_t* bitsCount /*[1..16]*/, const uint8_t* vals, size_t nvals) {
    t = HuffTable{};
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
        if (code > (1 << l)) return false; // over-subscribed: not a prefix code
        code <<= 1;
    }
    if (k != static_cast<int>(nvals)) return false;
    t.maxcode[17] = 0x7fffffff;
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
    t.present = true;
    return true;
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

// Prediction of T.81 H.1.2.1, shared by the decoder and the encoder. The first MCU of the scan and
// of every restart interval is predicted from 2^(P-Pt-1); the rest of the line in which the scan or
// interval starts uses Ra; the first sample of every other line uses Rb; all other samples use the
// selected predictor.
struct Predictor {
    int selection = 1;
    int initial = 0;
    bool intervalStart = true;
    uint32_t intervalLine = 0;

    int at(const uint16_t* row, const uint16_t* prev, uint32_t x, uint32_t y, size_t i, uint32_t nc) const {
        if (intervalStart) return initial;
        if (y == intervalLine) return row[i - nc];
        if (x == 0) return prev[i];
        return predict(selection, row[i - nc], prev[i], prev[i - nc]);
    }
};

} // namespace

Expected<Image> decode(const uint8_t* data, size_t len) {
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return formatError("missing SOI");
    Image img;
    std::array<HuffTable, 4> tables;
    struct Comp { uint8_t id = 0, td = 0; };
    std::vector<Comp> comps;
    int predictor = 1, pt = 0;
    uint32_t restartInterval = 0;
    size_t pos = 2;
    bool sofFound = false, sosFound = false;
    while (!sosFound) {
        if (pos + 2 > len) return formatError("truncated header");
        if (data[pos] != 0xFF) return formatError("marker expected");
        const uint8_t m = data[pos + 1];
        if (m == 0xFF) { ++pos; continue; } // fill byte
        pos += 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue; // TEM / stray RST: no segment
        if (m == 0xD8) return formatError("SOI inside the stream");
        if (m == 0xD9) return formatError("EOI before the scan");
        if (pos + 2 > len) return formatError("truncated segment");
        const size_t segLen = (size_t(data[pos]) << 8) | data[pos + 1];
        if (segLen < 2 || pos + segLen > len) return formatError("bad segment length");
        const uint8_t* s = data + pos + 2;
        const size_t sl = segLen - 2;
        switch (m) {
        case 0xC3: { // SOF3
            if (sofFound) return formatError("more than one frame header");
            if (sl < 6) return formatError("short SOF3");
            img.precision = s[0];
            img.height = (uint32_t(s[1]) << 8) | s[2];
            const uint32_t columns = (uint32_t(s[3]) << 8) | s[4];
            const uint8_t nf = s[5];
            if (nf < 1 || nf > 4 || sl < 6u + 3u * nf) return formatError("bad component count");
            for (int i = 0; i < nf; ++i) {
                Comp c;
                c.id = s[6 + i * 3];
                if (s[7 + i * 3] != 0x11) return formatError("subsampled components are not supported");
                comps.push_back(c);
            }
            if (columns == 0 || img.height == 0) return formatError("image size missing from the frame header");
            img.components = nf;
            img.width = columns * nf;
            sofFound = true;
            break;
        }
        case 0xC0: case 0xC1: case 0xC2: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return formatError("not a lossless Huffman-coded JPEG (SOF3)");
        case 0xC4: { // DHT
            size_t o = 0;
            while (o < sl) {
                if (o + 17 > sl) return formatError("DHT truncated");
                const uint8_t th = s[o] & 15;
                if (th > 3) return formatError("bad DHT id");
                uint8_t counts[17] = {0};
                size_t total = 0;
                for (int i = 1; i <= 16; ++i) { counts[i] = s[o + static_cast<size_t>(i)]; total += counts[i]; }
                if (o + 17 + total > sl) return formatError("DHT truncated");
                if (!buildTable(tables[th], counts, s + o + 17, total)) return formatError("invalid Huffman table");
                o += 17 + total;
            }
            break;
        }
        case 0xDD: // DRI
            if (sl < 2) return formatError("short DRI");
            restartInterval = (uint32_t(s[0]) << 8) | s[1];
            break;
        case 0xDA: { // SOS
            if (!sofFound) return formatError("scan before the frame header");
            if (sl < 1) return formatError("short SOS");
            const uint8_t ns = s[0];
            if (ns != comps.size() || sl < 1u + 2u * ns + 3u) return formatError("SOS/SOF mismatch");
            for (size_t i = 0; i < ns; ++i) {
                if (s[1 + i * 2] != comps[i].id) return formatError("scan components differ from the frame");
                comps[i].td = s[2 + i * 2] >> 4;
                if (comps[i].td > 3) return formatError("bad Huffman table id");
            }
            predictor = s[1 + ns * 2];
            const uint8_t se = s[2 + ns * 2], ahal = s[3 + ns * 2];
            if (se != 0 || (ahal >> 4) != 0) return formatError("bad scan parameters");
            pt = ahal & 15;
            sosFound = true;
            break;
        }
        default: break; // APPn, COM, DNL-free tables we do not need
        }
        pos += segLen;
    }
    if (img.precision < 2 || img.precision > 16) return formatError("bad precision");
    if (pt >= img.precision) return formatError("bad point transform");
    for (const auto& c : comps)
        if (!tables[c.td].present) return formatError("missing Huffman table");
    if (predictor < 1 || predictor > 7) return formatError("bad predictor");

    // Every sample costs at least one bit (the shortest Huffman code), so a header claiming more
    // samples than the data has bits is corrupt; checked before allocating the image.
    if (static_cast<uint64_t>(img.width) * img.height > 8ull * (len - pos))
        return formatError("frame header is larger than the entropy data (truncated or corrupt)");
    const uint32_t nc = img.components;
    const uint32_t cols = img.width / nc;
    img.samples.resize(static_cast<size_t>(img.width) * img.height);
    BitReader br{data + pos, data + len};
    const int vbits = img.precision - pt;
    const uint32_t vmax = (1u << vbits) - 1u;
    Predictor pr;
    pr.selection = predictor;
    pr.initial = 1 << (vbits - 1);
    uint32_t mcusLeft = restartInterval;
    uint32_t nextRst = 0;

    for (uint32_t y = 0; y < img.height; ++y) {
        uint16_t* row = img.samples.data() + static_cast<size_t>(y) * img.width;
        const uint16_t* prev = y ? row - img.width : nullptr;
        for (uint32_t x = 0; x < cols; ++x) {
            if (restartInterval && mcusLeft == 0) {
                // End of a restart interval: only padding may remain, then RSTn in sequence.
                if (br.overrun) return formatError("entropy data truncated before a restart marker");
                if (br.pendingRealBits() >= 8) return formatError("extra entropy data before a restart marker");
                const uint8_t* q = br.p;
                if (markerAt(q, br.end) != 0xD0 + static_cast<int>(nextRst & 7))
                    return formatError("restart marker missing or out of sequence");
                br.restartAt(q + 2);
                ++nextRst;
                mcusLeft = restartInterval;
                pr.intervalStart = true;
                pr.intervalLine = y;
            }
            for (uint32_t c = 0; c < nc; ++c) {
                const int ssss = decodeSymbol(br, tables[comps[c].td]);
                if (ssss < 0 || ssss > 16) return formatError("bad Huffman code");
                const int diff = extend(br.get(ssss == 16 ? 0 : ssss), ssss);
                const size_t i = static_cast<size_t>(x) * nc + c;
                const uint32_t v = static_cast<uint32_t>(pr.at(row, prev, x, y, i, nc) + diff) & 0xFFFFu;
                if (v > vmax) return formatError("sample out of range (corrupt entropy data)");
                row[i] = static_cast<uint16_t>(v);
            }
            pr.intervalStart = false;
            if (restartInterval) --mcusLeft;
        }
        if (br.overrun) return formatError("entropy data truncated");
    }
    if (br.overrun) return formatError("entropy data truncated");
    if (br.pendingRealBits() >= 8) return formatError("extra entropy data after the last sample");
    const uint8_t* q = br.p;
    if (q < br.end) {
        const int mk = markerAt(q, br.end);
        if (mk != 0xD9) return formatError(mk < 0 ? "garbage after the scan" : "marker other than EOI after the scan");
    }
    if (pt)
        for (auto& v : img.samples) v = static_cast<uint16_t>(v << pt);
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
    if (p.predictor < 1 || p.predictor > 7) return makeError(ErrorCode::InvalidArgument, "LJPEG: bad predictor");
    if (width == 0 || height == 0 || width / p.components > 65535 || height > 65535)
        return makeError(ErrorCode::InvalidArgument, "LJPEG: bad dimensions");
    if (p.restartInterval > 65535) return makeError(ErrorCode::InvalidArgument, "LJPEG: restart interval too large");
    const uint32_t nc = p.components;
    const uint32_t cols = width / nc;
    const uint32_t vmax = (1u << p.precision) - 1u;
    const uint32_t restart = p.restartInterval;

    // Pass 1: differences (prediction per T.81 H.1.2.1, modulo 2^16 per H.1.2) and statistics.
    std::vector<int32_t> diffs(static_cast<size_t>(width) * height);
    std::array<uint32_t, 18> freq{};
    Predictor pr;
    pr.selection = p.predictor;
    pr.initial = 1 << (p.precision - 1);
    uint32_t mcusLeft = restart;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t* row = samples + static_cast<size_t>(y) * width;
        const uint16_t* prev = y ? row - width : nullptr;
        int32_t* drow = diffs.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < cols; ++x) {
            if (restart && mcusLeft == 0) {
                pr.intervalStart = true;
                pr.intervalLine = y;
                mcusLeft = restart;
            }
            for (uint32_t c = 0; c < nc; ++c) {
                const size_t i = static_cast<size_t>(x) * nc + c;
                if (row[i] > vmax) return makeError(ErrorCode::InvalidArgument, "LJPEG: sample exceeds the precision");
                int d = static_cast<int>(row[i]) - pr.at(row, prev, x, y, i, nc);
                if (p.precision == 16) {
                    d &= 0xFFFF;
                    if (d > 32768) d -= 65536; // [-32767, 32768]; 32768 is category 16
                }
                drow[i] = d;
                ++freq[static_cast<size_t>(category(d))];
            }
            pr.intervalStart = false;
            if (restart) --mcusLeft;
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
    if (restart) {
        marker(0xDD);
        be16(4);
        be16(restart);
    }
    marker(0xDA);
    be16(6 + 2 * nc);
    out.push_back(static_cast<uint8_t>(nc));
    for (uint32_t c = 0; c < nc; ++c) { out.push_back(static_cast<uint8_t>(c + 1)); out.push_back(0x00); }
    out.push_back(p.predictor);
    out.push_back(0);
    out.push_back(0);

    BitWriter bw(out);
    const size_t mcuCount = static_cast<size_t>(cols) * height;
    uint32_t rst = 0;
    mcusLeft = restart;
    for (size_t m = 0; m < mcuCount; ++m) {
        if (restart && mcusLeft == 0) {
            bw.flush();
            marker(static_cast<uint8_t>(0xD0 + (rst++ & 7)));
            mcusLeft = restart;
        }
        for (uint32_t c = 0; c < nc; ++c) {
            const int32_t d = diffs[m * nc + c];
            const int cat = category(d);
            bw.put(code[static_cast<size_t>(cat)], codeLen[static_cast<size_t>(cat)]);
            if (cat > 0 && cat < 16) {
                const uint32_t v = d < 0 ? static_cast<uint32_t>(d - 1) : static_cast<uint32_t>(d);
                bw.put(v & ((1u << cat) - 1), cat);
            }
        }
        if (restart) --mcusLeft;
    }
    bw.flush();
    marker(0xD9);
    return out;
}

} // namespace r3dss::codec::ljpeg
