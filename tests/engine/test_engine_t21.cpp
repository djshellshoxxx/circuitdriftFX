// T21 alias measurement RE-RUN ON THE INTEGRATED CIRCUIT (P03), published method from
// docs/TRACEABILITY.md: Fs 48 kHz, N = 16384 rectangular FFT of steady state (samples 2N..3N-1),
// five equal tones on bins 7*{37,101,233,419,677}; unwanted = in-band (20 Hz-20 kHz) power on bins
// not divisible by 7, relative to the in-band output power of a 64x FIR (140 dB stages) reference.
// The memoryless tanh(D*x) of the primitive test is replaced by the engine's synthetic triode
// stage (CircuitPath: C_in, Koren triode, R_load, R_k/C_k, C_out, saturating transformer) driven
// at drive = 20*log10(D) dB inside the engine's oversampler cascade; cases nominal (A=0.1, D=4,
// gated at the spec limits Studio -70 dB / Render -85 dB) and hot (A=0.3, D=8, recorded).
// With the engine's input staging (0.25 V/FS for the triode) the published nominal mapping puts
// only 0.5 V peak on the grid (nearly linear), so two harder recorded cases are added at +12.04 dB
// more drive (2 V and ~7 V peak at the grid; published window only, circuit lane only).
// The supply rail is held constant in the circuit-lane harness (ripple is not aliasing); the full
// engine (both slots, supply, thermal; HPF, soft clip, gain match and noise off) is measured too,
// normalized by its own in-band power (its output is in FS units, the lane reference in volts).
// FINDING (method): unlike a memoryless fixture the circuit is not yet periodic at 2N (0.68 s):
// the local B+ decoupling (R_dec*C_dec = 0.22 s) re-settles under drive and the resulting leakage
// around the tones is counted as "unwanted". The published window (2N..3N-1) is therefore
// conservative and is the gated one; an extended settled window (4N..5N-1) is recorded alongside.
// SYNTHETIC FIXTURE circuit (G01): this is software antialiasing evidence, not hardware accuracy.
#include <cmath>
#include <complex>
#include <cstdio>
#include <future>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "dsp/Oversampling.h"
#include "engine/Circuit.h"
#include "engine/CircuitEngine.h"

namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace cd;

constexpr int N = 16384;
constexpr double kFs = 48000.0;
const int kBins[5] = {7 * 37, 7 * 101, 7 * 233, 7 * 419, 7 * 677};

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wk = 1;
            for (size_t k = 0; k < len / 2; ++k, wk *= w) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}

double tone(int i, double amp) {
    double x = 0;
    for (int b : kBins) x += amp * std::sin(2 * M_PI * double(b) * double(i) / N);
    return x;
}

struct Spectra { std::vector<double> published, settled; };

std::vector<double> spectrum(const std::vector<double>& all, int from) {
    std::vector<std::complex<double>> X(all.begin() + from, all.begin() + from + N);
    fft(X);
    std::vector<double> p(N / 2);
    for (int k = 0; k < N / 2; ++k) p[k] = std::norm(X[k]);
    return p;
}

// Circuit-lane harness: the engine's oversampler spec around the engine's triode CircuitPath.
Spectra laneSpectrum(int factor, dsp::OsFamily fam, double stopDb, int cap, double amp, double driveDb, int periods) {
    const auto f = engine::Family::Triode12AX7;
    const auto& d = engine::familyDesign(f);
    engine::CircuitPath p;
    std::string err;
    if (!p.build(f, engine::nominalValues(f, 1), 1.0 / (kFs * factor), cap, 0.97 * d.railDesignV, err)) return {};
    dsp::Oversampler os;
    if (!os.prepare({factor, fam, 0.45, stopDb}, 256)) return {};
    const double gin = std::pow(10.0, driveDb / 20.0) * d.inVoltsPerFs, vc = engine::complianceVolts(d);
    const int B = 256;
    std::vector<double> in(B), hi(std::size_t(B) * factor), out(B), capture;
    long fails = 0;
    for (int start = 0; start < periods * N; start += B) {
        for (int i = 0; i < B; ++i) in[i] = tone(start + i, amp);
        os.up(in.data(), hi.data(), B);
        for (double& v : hi) {
            p.setDrive(engine::sourceCompliance(v * gin, vc));
            if (!p.step()) ++fails;
            v = p.outputVolts();
        }
        os.down(hi.data(), out.data(), B);
        capture.insert(capture.end(), out.begin(), out.end());
    }
    if (fails) std::printf("T21 harness %dx: %ld solver failures\n", factor, fails);
    return {spectrum(capture, 2 * N), periods >= 5 ? spectrum(capture, 4 * N) : std::vector<double>{}};
}

// Full engine (mono): Studio or Render, mix 1, HPF/soft clip off, both slots at the same drive.
Spectra engineSpectrum(int mode, double amp, double driveDb) {
    engine::CircuitEngine e;
    auto p = core::defaultValues();
    p[core::indexOf("quality.mode")] = mode;
    p[core::indexOf("hpf.enabled")] = 0;
    p[core::indexOf("output.softClip")] = 0;
    p[core::indexOf("slot.a.stage.drive")] = driveDb;
    p[core::indexOf("slot.b.stage.drive")] = driveDb;
    engine::EngineConfig cfg;
    cfg.channels = 1;
    cfg.sampleRate = kFs;
    std::string err;
    if (!e.prepare(cfg, p, err)) return {};
    const int B = 256;
    std::vector<double> in(B), out(B), capture;
    core::ModulationBlock mod{};
    engine::ProcessContext ctx{};
    for (int start = 0; start < 5 * N; start += B) {
        for (int i = 0; i < B; ++i) in[i] = tone(start + i, amp);
        const double* ip[1] = {in.data()};
        double* op[1] = {out.data()};
        e.process(ctx, ip, op, 1, nullptr, 0, B, p, mod);
        capture.insert(capture.end(), out.begin(), out.end());
    }
    return {spectrum(capture, 2 * N), spectrum(capture, 4 * N)};
}
}  // namespace

int main() {
    const double df = kFs / N;
    const int k0 = int(std::ceil(20.0 / df)), k1 = int(20000.0 / df);
    auto inband = [&](const std::vector<double>& p) { double e = 0; for (int k = k0; k <= k1; ++k) e += p[k]; return e; };
    auto unwanted = [&](const std::vector<double>& p) { double e = 0; for (int k = k0; k <= k1; ++k) if (k % 7) e += p[k]; return e; };
    struct Stim { const char* name; double amp, drive; bool gated, full; };
    const Stim stims[4] = {{"nominal", 0.1, 20 * std::log10(4.0), true, true}, {"hot", 0.3, 20 * std::log10(8.0), false, true},
                           {"grid2V", 0.1, 20 * std::log10(16.0), false, false}, {"grid7V", 0.3, 20 * std::log10(32.0), false, false}};
    struct Mode { const char* name; int factor; dsp::OsFamily fam; int cap; double limit; };
    const Mode modes[] = {{"Studio 4x FIR", 4, dsp::OsFamily::Fir, 8, -70.0}, {"Render 16x FIR", 16, dsp::OsFamily::Fir, 12, -85.0},
                          {"4x IIR", 4, dsp::OsFamily::Iir, 8, 0.0}, {"2x FIR Draft", 2, dsp::OsFamily::Fir, 8, 0.0}};
    // All runs in parallel (independent objects; the engine itself is single-threaded).
    std::future<Spectra> ref[4], lane[4][4], eng[4][2];
    for (int s = 0; s < 4; ++s) {
        const int periods = stims[s].full ? 5 : 3;
        ref[s] = std::async(std::launch::async, laneSpectrum, 64, dsp::OsFamily::Fir, 140.0, 12, stims[s].amp, stims[s].drive, periods);
        for (int m = 0; m < 4; ++m)
            lane[s][m] = std::async(std::launch::async, laneSpectrum, modes[m].factor, modes[m].fam, 100.0, modes[m].cap, stims[s].amp,
                                    stims[s].drive, periods);
        if (stims[s].full)
            for (int m = 0; m < 2; ++m) eng[s][m] = std::async(std::launch::async, engineSpectrum, m + 1, stims[s].amp, stims[s].drive);
    }
    for (int s = 0; s < 4; ++s) {
        const Stim& st = stims[s];
        const Spectra pr = ref[s].get();
        if (pr.published.empty()) { CHECK(false, "%s reference failed", st.name); continue; }
        const bool hasSettled = !pr.settled.empty();
        const double eRefP = inband(pr.published), eRefS = hasSettled ? inband(pr.settled) : 1.0;
        auto fmt = [&](double v, char* buf) { if (hasSettled) std::snprintf(buf, 32, "%8.2f dB", v); else std::snprintf(buf, 32, "     n/a"); return buf; };
        char sb[32];
        std::printf("T21 integrated %-7s (drive %.2f dB, A=%.1f) 64x reference floor: published window %8.2f dB, settled window %s\n",
                    st.name, st.drive, st.amp, 10 * std::log10(unwanted(pr.published) / eRefP + 1e-300),
                    fmt(hasSettled ? 10 * std::log10(unwanted(pr.settled) / eRefS + 1e-300) : 0.0, sb));
        for (int m = 0; m < 4; ++m) {
            const Spectra p = lane[s][m].get();
            if (p.published.empty()) { CHECK(false, "%s %s harness failed", st.name, modes[m].name); continue; }
            const double a = 10 * std::log10(unwanted(p.published) / eRefP + 1e-300);
            const double b2 = hasSettled ? 10 * std::log10(unwanted(p.settled) / eRefS + 1e-300) : 0.0;
            const bool gate = st.gated && modes[m].limit < 0;
            std::printf("T21 integrated %-7s circuit lane %-15s alias %8.2f dB published, %s settled (%s %.0f dB)%s\n", st.name,
                        modes[m].name, a, fmt(b2, sb), gate ? "limit" : (modes[m].limit < 0 ? "ungated, spec" : "no spec, info"), modes[m].limit,
                        (modes[m].limit < 0 && a > modes[m].limit) ? " FINDING: exceeds spec" : "");
            if (gate) CHECK(a <= modes[m].limit, "%s %s alias %g dB", st.name, modes[m].name, a);
        }
        for (int m = 0; m < 2 && st.full; ++m) {
            const Spectra p = eng[s][m].get();
            if (p.published.empty()) { CHECK(false, "%s engine run failed", st.name); continue; }
            const double a = 10 * std::log10(unwanted(p.published) / inband(p.published) + 1e-300);
            const double b2 = 10 * std::log10(unwanted(p.settled) / inband(p.settled) + 1e-300);
            const double lim = modes[m].limit;
            std::printf("T21 integrated %-7s full engine  %-15s alias %8.2f dB published, %8.2f dB settled (%s %.0f dB)%s\n", st.name,
                        modes[m].name, a, b2, st.gated ? "limit" : "ungated, spec", lim, a > lim ? " FINDING: exceeds spec" : "");
            if (st.gated) CHECK(a <= lim, "%s engine %s alias %g dB", st.name, modes[m].name, a);
        }
    }
    std::printf(g_fail ? "ENGINE T21 FAILED (%d)\n" : "ENGINE T21 PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
