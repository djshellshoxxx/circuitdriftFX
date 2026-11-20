// T12 (detector part): monophonic autocorrelation pitch detector on SYNTHETIC signals.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
#include "TestHarness.h"
#include "control/PitchDetector.h"

using namespace cd::control;

static PitchResult feedAll(PitchDetector& d, const std::vector<double>& x, int block = 480) {
    for (std::size_t i = 0; i < x.size(); i += std::size_t(block)) {
        d.push(x.data() + i, int(std::min<std::size_t>(std::size_t(block), x.size() - i)));
        d.run();
    }
    return d.latest();
}

static void cleanTones() {
    const double freqs[] = {80, 98, 110, 165, 196, 261.63, 329.63, 440, 523.25, 659.26, 784, 880, 1000};
    for (double fs : {48000.0, 44100.0, 96000.0}) {
        for (double f : freqs) {
            PitchDetector d;
            d.prepare(fs);
            // Sine plus a weaker 2nd harmonic (still a clean monophonic tone).
            auto x = th::sine(f, 0.3, fs, std::size_t(fs * 0.6));
            const auto h2 = th::sine(2 * f, 0.1, fs, x.size(), 0.7);
            for (std::size_t i = 0; i < x.size(); ++i) x[i] += h2[i];
            const PitchResult r = feedAll(d, x);
            CHECK(r.state == PitchState::Acquired, "fs %g f %g state %d", fs, f, int(r.state));
            CHECK(std::fabs(th::cents(r.frequencyHz, f)) <= 15.0, "fs %g f %g got %g (%.2f cents)", fs, f, r.frequencyHz,
                  th::cents(r.frequencyHz, f));
            CHECK(r.confidence > 0.8, "confidence %g", r.confidence);
        }
    }
    // Pure sines: report the worst-case error for the record.
    double worst = 0;
    for (double f : freqs) {
        PitchDetector d;
        d.prepare(48000.0);
        const PitchResult r = feedAll(d, th::sine(f, 0.3, 48000.0, 28800));
        worst = std::max(worst, std::fabs(th::cents(r.frequencyHz, f)));
    }
    std::printf("pitch: worst pure-sine error %.2f cents\n", worst);
    CHECK(worst <= 15.0, "worst %g", worst);
}

static void lowConfidence() {
    constexpr double fs = 48000.0;
    {   // Silence below -60 dBFS gate: no pitch.
        PitchDetector d;
        d.prepare(fs);
        const PitchResult r = feedAll(d, th::sine(440, 4e-4, fs, 24000));  // RMS ~ -71 dBFS
        CHECK(r.state == PitchState::None, "quiet tone rejected: state %d", int(r.state));
    }
    {   // White noise: never Acquired.
        PitchDetector d;
        d.prepare(fs);
        th::Lcg rng;
        std::vector<double> x(48000);
        for (auto& v : x) v = 0.3 * rng.bipolar();
        int acquired = 0;
        for (std::size_t i = 0; i < x.size(); i += 480) {
            d.push(x.data() + i, 480);
            d.run();
            acquired += d.latest().state == PitchState::Acquired ? 1 : 0;
        }
        CHECK(acquired == 0, "noise acquired %d times", acquired);
        CHECK(d.latest().state == PitchState::Low, "noise reports Low, got %d", int(d.latest().state));
    }
    {   // Dense inharmonic cluster (chord-like, no common period inside the lag range).
        PitchDetector d;
        d.prepare(fs);
        const double notes[] = {261.63, 277.18, 370.0, 415.3};  // C4 C#4 F#4 G#4
        std::vector<double> x(48000, 0.0);
        for (double f : notes) {
            const auto s = th::sine(f, 0.1, fs, x.size());
            for (std::size_t i = 0; i < x.size(); ++i) x[i] += s[i];
        }
        int acquired = 0;
        for (std::size_t i = 0; i < x.size(); i += 480) {
            d.push(x.data() + i, 480);
            d.run();
            acquired += d.latest().state == PitchState::Acquired ? 1 : 0;
        }
        CHECK(acquired == 0, "chord cluster acquired %d times", acquired);
    }
}

static void hopStateMachine() {
    PitchHopTracker t;
    PitchResult r = t.update(true, true, 220.0, 0.95);
    CHECK(r.state == PitchState::Low, "one valid hop is not acquired");
    r = t.update(true, true, 220.5, 0.95);
    CHECK(r.state == PitchState::Acquired && std::fabs(r.frequencyHz - 220.5) < 1e-9, "two valid hops acquire");
    // Octave jump: rejected until stable for 3 hops.
    r = t.update(true, true, 441.0, 0.95);
    CHECK(r.state == PitchState::Acquired && std::fabs(r.frequencyHz - 220.5) < 1e-9, "octave jump hop 1 rejected");
    r = t.update(true, true, 441.0, 0.95);
    CHECK(std::fabs(r.frequencyHz - 220.5) < 1e-9, "octave jump hop 2 rejected");
    r = t.update(true, true, 441.0, 0.95);
    CHECK(std::fabs(r.frequencyHz - 441.0) < 1e-9, "octave jump accepted after 3 stable hops");
    // An interrupted octave candidate restarts the count.
    t.update(true, true, 220.5, 0.9);  // back down an octave: hop 1
    t.update(true, true, 441.0, 0.9);  // flip
    r = t.update(true, true, 220.5, 0.9);
    CHECK(std::fabs(r.frequencyHz - 441.0) < 1e-9, "unstable octave candidate not accepted");
    // Non-octave change tracks immediately.
    r = t.update(true, true, 330.0, 0.9);
    CHECK(std::fabs(r.frequencyHz - 330.0) < 1e-9, "non-octave change tracked");
    // Invalid hold: 200 ms = 2400 samples = 18.75 hops -> held through 18 hops, reverted at 19.
    for (int i = 1; i <= 18; ++i) {
        r = t.update(true, false, 0, 0);
        CHECK(r.held && r.state == PitchState::Low && std::fabs(r.frequencyHz - 330.0) < 1e-9, "held hop %d", i);
    }
    r = t.update(true, false, 0, 0);
    CHECK(r.state == PitchState::None && !r.held && r.frequencyHz == 0, "reverted after 200 ms");
    // Silence never advertises a pitch; present-but-invalid is Low.
    t.reset();
    CHECK(t.update(false, false, 0, 0).state == PitchState::None, "silent -> None");
    CHECK(t.update(true, false, 0, 0).state == PitchState::Low, "noise -> Low");
}

static void stalenessAndOverload() {
    constexpr double fs = 48000.0;
    {
        PitchDetector d;
        d.prepare(fs);
        const auto x = th::sine(330, 0.3, fs, 24000);
        const PitchResult r = feedAll(d, x);
        CHECK(r.state == PitchState::Acquired && d.isFresh(r), "fresh result");
        CHECK(r.timestamp > 0 && r.timestamp <= d.pushedSamples(), "timestamped");
        // 300 ms more audio pushed but the worker never ran: the stored result is stale.
        const auto more = th::sine(330, 0.3, fs, 14400);
        for (std::size_t i = 0; i < more.size(); i += 480) d.push(more.data() + i, 480);
        CHECK(!d.isFresh(d.latest()), "stale after >250 ms");
        d.run();
        CHECK(d.isFresh(d.latest()), "fresh again after the worker catches up");
    }
    {   // Overload: 4 s backlog, worker drops oldest windows and still locks onto the newest audio.
        PitchDetector d;
        d.prepare(fs, 1u << 18);
        const auto x = th::sine(247, 0.3, fs, 192512);
        for (std::size_t i = 0; i < x.size(); i += 1024) d.push(x.data() + i, 1024);
        d.run();
        CHECK(d.droppedWindows() > 0, "backlog dropped");
        PitchResult r = d.latest();
        CHECK(r.state == PitchState::Acquired || r.state == PitchState::Low, "state after overload");
        const auto y = th::sine(247, 0.3, fs, 24000);
        r = feedAll(d, y);
        CHECK(r.state == PitchState::Acquired && std::fabs(th::cents(r.frequencyHz, 247)) < 15, "recovers");
    }
    {   // Ring full on the producer side: block is dropped, counted, never blocks.
        PitchDetector d;
        d.prepare(fs, 4096);
        std::vector<double> blk(3000, 0.1);
        d.push(blk.data(), 3000);
        d.push(blk.data(), 3000);
        CHECK(d.droppedSamples() == 3000, "overflow counted");
        d.run();
    }
}

static void seekDiscontinuity() {
    constexpr double fs = 48000.0;
    PitchDetector d;
    d.prepare(fs);
    const auto before = th::sine(220.0, 0.3, fs, 24000);
    const auto acquired = feedAll(d, before);
    CHECK(acquired.state == PitchState::Acquired && d.isFresh(acquired), "pre-seek tone acquired");

    // The old result and even a retained copy must be invalid immediately,
    // without touching worker-owned history from the audio producer.
    const auto oldQueued = th::sine(220.0, 0.3, fs, 6000);
    d.push(oldQueued.data(), int(oldQueued.size()));
    d.markDiscontinuity();
    CHECK(d.latest().state == PitchState::None, "seek immediately hides prior generation");
    CHECK(!d.isFresh(acquired), "saved pre-seek result is invalid");
    const auto after = th::sine(523.25, 0.3, fs, 12000);
    const auto changed = feedAll(d, after);
    CHECK(changed.state == PitchState::Acquired && d.isFresh(changed), "post-seek tone acquired");
    CHECK(std::fabs(th::cents(changed.frequencyHz, 523.25)) < 15,
          "post-seek worker discards queued pre-seek audio: %g", changed.frequencyHz);
}

static void droppedInputInvalidatesPitch() {
    constexpr double fs = 48000.0;
    PitchDetector d;
    d.prepare(fs, 8192);
    const auto tone = th::sine(220.0, 0.3, fs, 24000);
    const auto acquired = feedAll(d, tone);
    CHECK(acquired.state == PitchState::Acquired && d.isFresh(acquired), "tone acquired before input gap");
    // Queue less than the freshness interval, then drop one ordinary host block.
    d.push(tone.data(), 8192);
    d.push(tone.data(), 480);
    CHECK(d.droppedSamples() == 480, "one dropped input block counted");
    CHECK(!d.isFresh(acquired) && d.latest().state == PitchState::None,
          "a dropped input block immediately invalidates pre-gap pitch");
    d.run();
    CHECK(d.latest().state == PitchState::None,
          "worker does not reacquire from queued audio that predates the gap");
    const auto changed = feedAll(d, th::sine(523.25, 0.3, fs, 12000));
    CHECK(changed.state == PitchState::Acquired && d.isFresh(changed) &&
          std::fabs(th::cents(changed.frequencyHz, 523.25)) < 15,
          "detector recovers using post-gap audio");
}

static void threadedSpsc() {
    constexpr double fs = 48000.0;
    PitchDetector d;
    d.prepare(fs, 1u << 17);
    const auto x = th::sine(196, 0.3, fs, std::size_t(fs * 3));
    std::atomic<bool> done{false};
    std::thread worker([&] {
        while (!done.load(std::memory_order_acquire)) { d.run(); std::this_thread::yield(); }
        d.run();
    });
    PitchResult seen{};
    for (std::size_t i = 0; i < x.size(); i += 256) {
        d.push(x.data() + i, int(std::min<std::size_t>(256, x.size() - i)));
        seen = d.latest();  // concurrent seqlock reads
        std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    worker.join();
    const PitchResult r = d.latest();
    CHECK(r.state == PitchState::Acquired && std::fabs(th::cents(r.frequencyHz, 196)) < 15, "threaded: %g state %d dropped %llu",
          r.frequencyHz, int(r.state), (unsigned long long)d.droppedSamples());
    (void)seen;
}

int main() {
    cleanTones();
    lowConfidence();
    hopStateMachine();
    stalenessAndOverload();
    seekDiscontinuity();
    droppedInputInvalidatesPitch();
    threadedSpsc();
    if (g_fail) { std::printf("pitch: %d failure(s)\n", g_fail); return 1; }
    std::printf("pitch: all passed\n");
    return 0;
}
