// T24 (part) A/B slot tests: identical-slot unity, 20 ms toggle continuity, swap invariance,
// copy A->B continuation (direct and through a matching snapshot), block partitions and
// zero-frame blocks. The slot engines are SYNTHETIC stateful processors (seeded one-pole
// filter with xorshift noise), not circuit models; no physical validation is implied.
#include "state/AbSlots.h"
#include "state/SnapshotExchange.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace {
using namespace cd::state;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
constexpr double kFs = 48000;

// Synthetic stateful slot engine: configuration (coef, noise depth) + dynamic state (y, rng).
struct SynthEngine {
    double coef = 0.1, noiseDepth = 1e-3, y = 0;
    std::uint64_t rng = 1;
    void process(const double* in, double* out, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            const double noise = (double(rng >> 11) * 0x1.0p-53 - 0.5) * noiseDepth;
            y += coef * (in[i] + noise - y);
            out[i] = y;
        }
    }
};
static_assert(std::is_trivially_copyable_v<SynthEngine>);

double input(std::uint64_t n) { return 0.5 * std::sin(2 * std::numbers::pi * 220.0 * double(n) / kFs) + 0.2 * std::sin(2 * std::numbers::pi * 3100.0 * double(n) / kFs); }

// Runs two engines through a mixer for `blocks` (block sizes cycling) with a per-block hook.
struct Rig {
    SynthEngine eng[2];
    AbMixer mixer;
    std::uint64_t clock = 0;
    std::vector<double> in, b0, b1, out;
    Rig() : in(8192), b0(8192), b1(8192), out(8192) { CHECK(mixer.prepare(kFs)); }
    // Processes one block, returns the actions applied at its start.
    AbBlockActions block(std::uint32_t n, std::vector<double>& sink) {
        const AbBlockActions act = mixer.beginBlock();
        if (act.copyAToB) eng[mixer.physicalB()] = eng[mixer.physicalA()];  // config + dynamic state + seeds
        for (std::uint32_t i = 0; i < n; ++i) in[i] = input(clock + i);
        eng[0].process(in.data(), b0.data(), n);
        eng[1].process(in.data(), b1.data(), n);
        const double* a = mixer.physicalA() == 0 ? b0.data() : b1.data();
        const double* b = mixer.physicalA() == 0 ? b1.data() : b0.data();
        mixer.mix(a, b, out.data(), n);
        sink.insert(sink.end(), out.begin(), out.begin() + n);
        clock += n;
        return act;
    }
};

void identicalSlotUnity() {
    Rig r;
    r.eng[0].rng = r.eng[1].rng = 99;
    SynthEngine ref = r.eng[0];
    std::vector<double> got, want(8192);
    std::vector<double> refIn(8192);
    double maxDb = 0;
    bool exact = true;
    const double positions[] = {0, .1, .25, .37, .5, .63, .75, .9, 1, .5, 0};
    for (std::size_t k = 0; k < std::size(positions); ++k) {
        CHECK(r.mixer.setPosition(positions[k]));
        if (k % 3 == 2) CHECK(r.mixer.toggle());
        got.clear();
        const std::uint64_t start = r.clock;
        r.block(1500, got);  // includes the 20 ms ramp and static positions
        for (std::uint32_t i = 0; i < 1500; ++i) refIn[i] = input(start + i);
        ref.process(refIn.data(), want.data(), 1500);
        for (std::uint32_t i = 0; i < 1500; ++i) {
            if (got[i] != want[i]) exact = false;
            if (want[i] != 0) maxDb = std::max(maxDb, std::fabs(20 * std::log10(std::fabs(got[i] / want[i]))));
        }
    }
    CHECK(exact);
    CHECK(maxDb < 0.01);
    std::printf("T24 identical-slot unity: bit-exact=%d, max deviation %.3g dB (limit 0.01 dB)\n", int(exact), maxDb);
}

// DC +1 on A and -1 on B: the toggle ramp is a straight 20 ms line, identical for every partition.
void toggleContinuityAndPartitions() {
    constexpr std::uint32_t kTotal = 4096;
    std::vector<double> reference;
    for (std::uint32_t block : {1u, 17u, 64u, 127u, 512u, 8192u}) {
        AbMixer m;
        CHECK(m.prepare(kFs));
        std::vector<double> a(8192, 1.0), b(8192, -1.0), out(8192), all;
        CHECK(m.toggle());
        for (std::uint32_t start = 0; start < kTotal;) {
            m.beginBlock();
            m.mix(a.data(), b.data(), out.data(), 0);  // interleaved zero-frame call changes nothing
            m.beginBlock();
            const std::uint32_t n = std::min(block, kTotal - start);
            m.mix(a.data(), b.data(), out.data(), n);
            all.insert(all.end(), out.begin(), out.begin() + n);
            start += n;
        }
        if (reference.empty()) {
            reference = all;
            const double step = 1.0 / (AbMixer::kFadeSeconds * kFs);
            double maxStep = std::fabs(all[0] - 1.0);
            for (std::size_t i = 1; i < all.size(); ++i) maxStep = std::max(maxStep, std::fabs(all[i] - all[i - 1]));
            std::size_t reach = 0;
            while (reach < all.size() && all[reach] != -1.0) ++reach;
            CHECK(maxStep <= 2.0 * step * (1 + 1e-9));       // |a-b| * (1/(0.02 Fs)) per sample
            CHECK(reach + 1 == std::size_t(AbMixer::kFadeSeconds * kFs));  // reaches B at sample 960
            CHECK(all.back() == -1.0);
            std::printf("T24 20 ms continuity: endpoint reached after %zu samples (%.3f ms), max output step %.6g (limit %.6g)\n",
                        reach + 1, 1e3 * double(reach + 1) / kFs, maxStep, 2.0 * step);
        } else {
            CHECK(all == reference);
        }
    }
    // Reversing mid-ramp stays continuous.
    AbMixer m;
    CHECK(m.prepare(kFs));
    std::vector<double> a(64, 1.0), b(64, -1.0), out(64);
    double prev = 1.0, maxStep = 0;
    CHECK(m.toggle());
    for (int k = 0; k < 40; ++k) {
        if (k == 7) CHECK(m.toggle());
        m.beginBlock();
        m.mix(a.data(), b.data(), out.data(), 64);
        for (double v : out) { maxStep = std::max(maxStep, std::fabs(v - prev)); prev = v; }
    }
    CHECK(maxStep <= 2.0 / (AbMixer::kFadeSeconds * kFs) * (1 + 1e-9));
    CHECK(prev == 1.0);
}

double maxAbsDiff(const std::vector<double>& x, const std::vector<double>& y) {
    double d = 0;
    for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i) d = std::max(d, std::fabs(x[i] - y[i]));
    return x.size() == y.size() ? d : std::numeric_limits<double>::infinity();
}

void swapInvariance() {
    Rig ref, swp;
    for (Rig* r : {&ref, &swp}) {
        r->eng[0].coef = 0.1; r->eng[0].rng = 11;
        r->eng[1].coef = 0.35; r->eng[1].noiseDepth = 4e-3; r->eng[1].rng = 22;
    }
    std::vector<double> outRef, outSwp;
    const std::uint32_t sizes[] = {64, 17, 127, 1, 512, 64};
    for (int k = 0; k < 60; ++k) {
        if (k == 3) { ref.mixer.setPosition(0.8); swp.mixer.setPosition(0.8); }
        if (k == 20) { ref.mixer.toggle(); swp.mixer.toggle(); }
        if (k == 5 || k == 9 || k == 21 || k == 40) CHECK(swp.mixer.swap());  // mid-ramp and static
        if (k == 30) { ref.mixer.setPosition(0.25); swp.mixer.setPosition(0.75); }  // B-relative after odd swaps
        const std::uint32_t n = sizes[k % 6];
        ref.block(n, outRef);
        const AbBlockActions act = swp.block(n, outSwp);
        if (k == 5) CHECK(act.swapped && swp.mixer.physicalA() == 1);
    }
    CHECK(swp.mixer.physicalA() == 0);  // four swaps
    const double d = maxAbsDiff(outRef, outSwp);
    CHECK(d <= 1e-6);
    // Zero-frame block applies a swap (state only, no clock).
    std::vector<double> none;
    const double before = swp.mixer.position();
    CHECK(swp.mixer.swap());
    const std::uint64_t clock = swp.clock;
    const AbBlockActions act = swp.block(0, none);
    CHECK(act.swapped && none.empty() && swp.clock == clock && swp.mixer.position() == 1.0 - before);
    std::printf("T24 swap invariance: max |diff| %.3g over %zu samples (limit 1e-6)\n", d, outRef.size());
}

void copyContinuation() {
    Rig r;
    r.eng[0].coef = 0.12; r.eng[0].rng = 5;
    r.eng[1].coef = 0.4; r.eng[1].rng = 77;
    std::vector<double> sink;
    CHECK(r.mixer.setPosition(1.0));  // listen to B
    for (int k = 0; k < 20; ++k) r.block(127, sink);
    // Reference: slot A continuing without interruption.
    SynthEngine refA = r.eng[r.mixer.physicalA()];
    CHECK(r.mixer.copyAToB());
    CHECK(r.mixer.swap());           // queued after the copy: applied at the NEXT block boundary
    sink.clear();
    AbBlockActions act = r.block(300, sink);
    CHECK(act.copyAToB && !act.swapped);
    std::vector<double> in(300), want(300);
    for (std::uint32_t i = 0; i < 300; ++i) in[i] = input(r.clock - 300 + i);
    refA.process(in.data(), want.data(), 300);
    const double directErr = maxAbsDiff(sink, want);  // position 1 => output is slot B == copy of A
    CHECK(directErr <= 1e-6);
    std::vector<double> more;
    act = r.block(10, more);
    CHECK(act.swapped);

    // Through a coherent snapshot: capture slot A at a block boundary, restore into a fresh engine.
    SnapshotIdentity id;
    id.layout = {0x41425331u /* synthetic A/B engine layout */, 1, sizeof(SynthEngine)};
    id.sampleRate = kFs; id.oversampling = 1; id.modelId = 0x5EED;
    SnapshotExchange x(id);
    const SynthEngine& liveA = r.eng[r.mixer.physicalA()];
    std::memcpy(x.writePayload().data(), &liveA, sizeof liveA);
    x.publish(r.clock, 0.0, 1);
    SynthEngine continued = liveA;
    HostSaveResult saved = hostSave(x, "{}");
    CHECK(saved.decision == RestoreDecision::Restore && saved.snapshot.header.sampleIndex == r.clock);
    SynthEngine restored;  // warm-initialised, different state
    const SynthEngine warm = restored;
    std::string reason;
    SnapshotIdentity other = id; other.sampleRate = 96000;
    CHECK(restoreSnapshot(saved.snapshot, other, std::as_writable_bytes(std::span(&restored, 1)), reason) == RestoreDecision::ResetWarm);
    CHECK(std::memcmp(&restored, &warm, sizeof warm) == 0);  // incompatible Fs: live state unchanged
    CHECK(restoreSnapshot(saved.snapshot, id, std::as_writable_bytes(std::span(&restored, 1)), reason) == RestoreDecision::Restore);
    std::vector<double> in2(4096), o1(4096), o2(4096);
    for (std::uint32_t i = 0; i < 4096; ++i) in2[i] = input(r.clock + i);
    continued.process(in2.data(), o1.data(), 4096);
    restored.process(in2.data(), o2.data(), 4096);
    const double snapErr = maxAbsDiff(o1, o2);
    CHECK(snapErr <= 1e-6);
    std::printf("T24 copy A->B continuation: direct max err %.3g, via snapshot max err %.3g (limit 1e-6; synthetic engine)\n",
                directErr, snapErr);
}

void invalidInputs() {
    AbMixer m(4);
    CHECK(!m.prepare(0) && !m.prepare(-48000) && !m.prepare(std::numeric_limits<double>::quiet_NaN()) && !m.prepare(1e9));
    CHECK(m.prepare(44100));
    CHECK(!m.setPosition(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!m.setPosition(std::numeric_limits<double>::infinity()));
    CHECK(!m.setPosition(-0.1) && !m.setPosition(1.5));
    for (int i = 0; i < 4; ++i) CHECK(m.toggle());
    CHECK(!m.toggle());  // bounded command queue: full -> false, never blocks
    m.beginBlock();
    CHECK(m.target() == 0.0);  // four toggles
    m.mix(nullptr, nullptr, nullptr, 16);  // ignored
    CHECK(m.position() == 0.0);
}

}  // namespace

int main() {
    identicalSlotUnity();
    toggleContinuityAndPartitions();
    swapInvariance();
    copyContinuation();
    invalidInputs();
    if (failures) { std::printf("state_ab: %d failure(s)\n", failures); return 1; }
    std::puts("state_ab: all checks passed (synthetic engines)");
    return 0;
}
