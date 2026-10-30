// Synthetic boundary/streaming regressions; no hardware or realtime qualification claim.
#include "dsp/Halfband2x.h"
#include "dsp/Oversampling.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (false)
using namespace cd::dsp;

void stageBoundaries() {
    // Both public FIR types must remain available without an ODR/name collision.
    Halfband2x fixed;
    fixed.prepare();
    CHECK(fixed.coefficients().size() == 77);

    PolyphaseHalfband2x fir;
    CHECK(fir.design(0.225, 100));
    CHECK(!fir.design(std::nextafter(0.25, 0.0), 100)); // Previously overflowed double -> int.
    CHECK(fir.taps().empty());
    CHECK(fir.center() == 0 && fir.magnitude(0) == 0);
    double pair[2] = {123, 456};
    fir.up(1, pair);
    CHECK(pair[0] == 0 && pair[1] == 0 && fir.down(pair) == 0);
    CHECK(!fir.design(0.2499, 100)); // Finite but exceeds the work/storage budget.
    CHECK(fir.design(0.225, 100));
    CHECK(fir.taps().size() <= PolyphaseHalfband2x::kMaxTaps);
    for (int i = 0; i < 512; ++i) {
        fir.up(i == 0 ? 1 : 0, pair);
        CHECK(std::isfinite(pair[0]) && std::isfinite(pair[1]));
        CHECK(std::isfinite(fir.down(pair)));
    }

    HalfbandIir2x iir;
    CHECK(iir.design(0.225, 100));
    CHECK(!iir.design(5e-21, 100)); // Previously accepted NaN coefficients as a perfect stopband.
    CHECK(iir.coefCount() == 0 && iir.magnitude(0) == 0 && iir.groupDelayDc() == 0);
    iir.up(1, pair);
    CHECK(pair[0] == 0 && pair[1] == 0 && iir.down(pair) == 0);
    CHECK(iir.design(0.225, 100));
    CHECK(std::isfinite(iir.groupDelayDc()));
    for (int i = 0; i < 512; ++i) {
        iir.up(i == 0 ? 1 : 0, pair);
        CHECK(std::isfinite(pair[0]) && std::isfinite(pair[1]));
        CHECK(std::isfinite(iir.down(pair)));
    }
}

void cascadeBoundaries() {
    Oversampler os;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::array<Oversampler::Spec, 9> rejected{{
        {3, OsFamily::Fir}, {128, OsFamily::Fir}, {4, OsFamily::Fir, 0},
        {4, OsFamily::Fir, 0.5}, {4, OsFamily::Fir, nan},
        {4, OsFamily::Iir, 1e-20, 100},
        {4, OsFamily::Fir, std::nextafter(0.5, 0.0), 100},
        {1, OsFamily::Fir, 0.45, inf}, {1, static_cast<OsFamily>(255)}
    }};
    for (const auto& spec : rejected) {
        CHECK(os.prepare({}, 1));
        CHECK(!os.prepare(spec, 1));
        CHECK(os.factor() == 0 && os.stages() == 0 && os.latency() == 0);
        std::array<double, 4> sentinel{11, 22, 33, 44};
        const auto before = sentinel;
        os.up(nullptr, sentinel.data(), 1);
        os.down(nullptr, sentinel.data(), 1);
        CHECK(sentinel == before);
    }
    CHECK(!os.prepare({}, 0));
    CHECK(!os.prepare({}, Oversampler::kMaxBlockSize + 1));

    for (const auto family : {OsFamily::Fir, OsFamily::Iir}) {
        Oversampler fresh;
        CHECK(os.prepare({4, family}, 1));
        CHECK(fresh.prepare({4, family}, 1));
        std::array<double, 4> sentinel{11, 22, 33, 44};
        const auto before = sentinel;
        for (int n : {-1, 0, 2, std::numeric_limits<int>::max()}) {
            // No valid source storage supplied: rejection must happen before pointer access.
            os.up(nullptr, sentinel.data(), n);
            os.down(nullptr, sentinel.data(), n);
        }
        CHECK(sentinel == before);
        for (int i = 0; i < 256; ++i) {
            const double in = i == 0 ? 1 : 0;
            double high[4], refHigh[4], out, ref;
            os.up(&in, high, 1); fresh.up(&in, refHigh, 1);
            os.down(high, &out, 1); fresh.down(refHigh, &ref, 1);
            CHECK(out == ref && std::isfinite(out)); // Rejected calls must not advance clocks.
        }
    }
}

std::vector<double> render(Oversampler& os, const std::vector<double>& in, int block) {
    std::vector<double> high(std::size_t(block) * os.factor()), out(in.size());
    for (std::size_t at = 0; at < in.size(); at += block) {
        const int n = static_cast<int>(std::min<std::size_t>(block, in.size() - at));
        os.up(in.data() + at, high.data(), n);
        os.down(high.data(), out.data() + at, n);
    }
    return out;
}

void streaming() {
    std::vector<double> in(1024);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = 0.5 * std::sin(0.13 * i);
    for (const auto family : {OsFamily::Fir, OsFamily::Iir}) {
        Oversampler os;
        CHECK(os.prepare({4, family}, 64));
        const auto reference = render(os, in, 1);
        for (int block : {17, 64}) {
            os.reset();
            CHECK(reference == render(os, in, block));
        }
        CHECK(os.prepare({4, family}, 64));
        CHECK(reference == render(os, in, 64));
    }
}
} // namespace

int main() {
    stageBoundaries();
    cascadeBoundaries();
    streaming();
    std::printf("oversampling safety: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
