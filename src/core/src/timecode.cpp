#include "r3dss/core/types.hpp"
#include <charconv>
#include <cmath>
#include <cstdio>

namespace r3dss {

namespace {
// Nominal integer frame rate for timecode counting (23.976 -> 24, 29.97 -> 30, 59.94 -> 60).
int nominalFps(Rational fps) { return static_cast<int>(std::lround(std::ceil(fps.toDouble() - 1e-6))); }
} // namespace

std::string Timecode::toString() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d%c%02d", hours, minutes, seconds, dropFrame ? ';' : ':', frames);
    return buf;
}

int64_t Timecode::toFrames() const {
    const int64_t nf = nominalFps(fps);
    const int64_t totalMinutes = hours * 60 + minutes;
    int64_t f = ((totalMinutes * 60 + seconds) * nf) + frames;
    if (dropFrame && (nf == 30 || nf == 60)) {
        const int64_t drop = nf == 30 ? 2 : 4;
        f -= drop * (totalMinutes - totalMinutes / 10);
    }
    return f;
}

Timecode Timecode::fromFrames(int64_t n, Rational fps, bool df) {
    Timecode tc;
    tc.fps = fps;
    tc.dropFrame = df;
    const int64_t nf = nominalFps(fps);
    if (nf <= 0) return tc;
    if (df && (nf == 30 || nf == 60)) {
        const int64_t drop = nf == 30 ? 2 : 4;
        const int64_t framesPerMinute = nf * 60 - drop;
        const int64_t framesPer10Min = framesPerMinute * 10 + drop;
        const int64_t d = n / framesPer10Min;
        int64_t m = n % framesPer10Min;
        if (m < drop) m += drop; // avoid negative adjust in the first frames of a 10-minute block
        n += drop * 9 * d + drop * ((m - drop) / framesPerMinute);
    }
    tc.frames = static_cast<int>(n % nf);
    n /= nf;
    tc.seconds = static_cast<int>(n % 60);
    n /= 60;
    tc.minutes = static_cast<int>(n % 60);
    n /= 60;
    tc.hours = static_cast<int>(n % 24);
    return tc;
}

std::optional<Timecode> Timecode::parse(std::string_view s, Rational fps) {
    if (s.size() != 11) return std::nullopt;
    auto num = [&](size_t off, int& out) {
        const auto r = std::from_chars(s.data() + off, s.data() + off + 2, out);
        return r.ec == std::errc{};
    };
    Timecode tc;
    tc.fps = fps;
    if (!num(0, tc.hours) || !num(3, tc.minutes) || !num(6, tc.seconds) || !num(9, tc.frames)) return std::nullopt;
    tc.dropFrame = s[8] == ';';
    return tc;
}

} // namespace r3dss
