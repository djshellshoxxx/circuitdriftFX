// Acceptance tests: T20 (thermal), T22 (HPF), T23 (soft clip / gain match), registry invariants (P01).
// Plain-assert harness: no third-party dependency, deterministic, no I/O.
#include <cmath>
#include <complex>
#include <string>
#include <cstdio>
#include <set>
#include <vector>
#include "core/ParameterRegistry.h"
#include "dsp/OutputProtection.h"
#include "dsp/ThermalNetwork.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace cd;

static void registry() {
    std::set<uint32_t> ids; std::set<std::string> names;
    for (const auto& p : core::kParams) {
        CHECK(p.hostId >= 1000 && p.hostId <= 1999, "%s id %u", p.id, p.hostId);
        CHECK(ids.insert(p.hostId).second, "dup hostId %u", p.hostId);
        CHECK(names.insert(p.id).second, "dup id %s", p.id);
        CHECK(p.min <= p.def && p.def <= p.max, "%s default range", p.id);
        const double u = core::toNormalized(p, p.def);
        CHECK(std::fabs(core::toPlain(p, u) - p.def) < 1e-9 * (1 + std::fabs(p.def)), "%s roundtrip", p.id);
        CHECK(std::fabs(core::toPlain(p, 0.0) - p.min) < 1e-9 && std::fabs(core::toPlain(p, 1.0) - p.max) < 1e-9, "%s endpoints", p.id);
        CHECK(core::toPlain(p, NAN) == p.def, "%s NaN", p.id);
    }
    CHECK(core::kParamCount == 74, "count %zu", core::kParamCount);
    const auto* h = core::findById("hpf.frequency");
    CHECK(h && h->def == 15.0 && h->mapping == core::ParamMapping::Log, "hpf spec");
    CHECK(core::findByHostId(1011) == h, "lookup");
    CHECK(core::smoothingCoeff(48000, 0) == 0.0, "tau0");
    CHECK(std::fabs(core::smoothingCoeff(48000, 0.01) - std::exp(-1.0 / 480.0)) < 1e-15, "coeff");
}

static void t20_thermal() {
    dsp::ThermalNetwork::Config c; c.n = 1; c.capacity[0] = 2.0; c.toAmbient[0] = 0.2;
    dsp::ThermalNetwork net;
    CHECK(net.prepare(c, 1e-3, 298.15), "prepare");
    double p = 1.0;
    for (int i = 0; i < 10000; ++i) net.step(&p);
    const double exact = 298.15 + 5.0 * (1.0 - std::exp(-1.0));  // tau=C/G=10 s
    CHECK(std::fabs(net.temperature(0) - exact) <= 0.002, "err %g", net.temperature(0) - exact);
    // Coupled 2-node steady state vs hand solution: node0 heated 1 W, G01=0.5, G1amb=0.25, G0amb=0.
    dsp::ThermalNetwork::Config d; d.n = 2; d.capacity[0] = d.capacity[1] = 1.0;
    d.toAmbient[1] = 0.25; d.coupling[0][1] = d.coupling[1][0] = 0.5;
    dsp::ThermalNetwork n2; CHECK(n2.prepare(d, 1e-2, 300.0), "prepare2");
    double pw[2] = {1.0, 0.0};
    for (int i = 0; i < 200000; ++i) n2.step(pw);
    CHECK(std::fabs(n2.temperature(1) - 304.0) < 1e-6 && std::fabs(n2.temperature(0) - 306.0) < 1e-6,
          "coupled %g %g", n2.temperature(0), n2.temperature(1));
    // Rejection: isolated island (no ambient path), non-positive C, asymmetric coupling.
    dsp::ThermalNetwork::Config bad = d; bad.toAmbient[1] = 0.0;
    CHECK(!n2.prepare(bad, 1e-2, 300.0), "island");
    bad = d; bad.capacity[0] = 0.0; CHECK(!n2.prepare(bad, 1e-2, 300.0), "C=0");
    bad = d; bad.coupling[0][1] = 0.4; CHECK(!n2.prepare(bad, 1e-2, 300.0), "asym");
}

static void t22_hpf() {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        dsp::ButterworthHpf f; CHECK(f.design(fs, 15.0), "design");
        // Magnitude from coefficients at fc.
        const double w = 2 * M_PI * 15.0 / fs;
        auto mag = [&](double ww) {
            std::complex<double> z1 = std::polar(1.0, -ww), z2 = std::polar(1.0, -2 * ww);
            return std::abs((f.b0 + f.b1 * z1 + f.b2 * z2) / (1.0 + f.a1 * z1 + f.a2 * z2));
        };
        CHECK(std::fabs(20 * std::log10(mag(w)) + 3.0103) <= 0.05, "fs %g corner %g dB", fs, 20 * std::log10(mag(w)));
        // DC step residual after 2 s.
        double y = 0; for (int i = 0; i < int(2 * fs); ++i) y = f.process(1.0);
        CHECK(20 * std::log10(std::fabs(y) + 1e-300) < -100.0, "fs %g residual %g", fs, y);
    }
    dsp::ButterworthHpf f; CHECK(!f.design(48000, 24000) && !f.design(48000, 0) && !f.design(0, 15), "reject");
}

static void t23_clip_match() {
    double prev = 0;
    for (double x = 0; x < 50; x += 1e-3) {
        const double y = dsp::softClip(x);
        CHECK(y <= 1.0 && (x > 2.5 || y < 1.0) && y >= prev - 1e-15, "monotone/bounded at %g", x);
        if (x <= 0.9) CHECK(y == x, "identity");
        prev = y;
    }
    CHECK(dsp::softClip(-0.95) == -dsp::softClip(0.95), "odd");
    const double h = 1e-6;  // C1 at knee
    CHECK(std::fabs((dsp::softClip(0.9 + h) - dsp::softClip(0.9 - h)) / (2 * h) - 1.0) < 1e-4, "derivative");
    // Gain match: wet is 2x (+6 dB) of a -12 dBFS-RMS sine; settle 5 s, residual within +-0.5 dB.
    const double fs = 48000; dsp::GainMatcher gm; gm.prepare(fs);
    double ed = 0, ew = 0; const int N = int(8 * fs); const int tail = int(fs);
    for (int i = 0; i < N; ++i) {
        const double d = 0.25 * std::sin(2 * M_PI * 220.0 * i / fs), w = 2.0 * d;
        const double g = gm.update(d, w);
        if (i >= N - tail) { ed += d * d; ew += (g * w) * (g * w); }
    }
    CHECK(std::fabs(10 * std::log10(ew / ed)) <= 0.5, "match %g dB", 10 * std::log10(ew / ed));
    // Freeze below -60 dBFS: gain stays unity for quiet input.
    gm.reset(); for (int i = 0; i < 48000; ++i) gm.update(1e-4, 2e-4);
    CHECK(gm.gain() == 1.0, "freeze");
    // Clamp: huge mismatch bounded to [0.25,4] eventually.
    gm.reset(); for (int i = 0; i < int(30 * fs); ++i) gm.update(0.5 * std::sin(i * 0.05), 0.01 * std::sin(i * 0.05));
    CHECK(gm.gain() <= 4.0 + 1e-9 && gm.gain() > 3.5, "clamp %g", gm.gain());
}

int main() {
    registry(); t20_thermal(); t22_hpf(); t23_clip_match();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
