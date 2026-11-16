// ScaleQuantizer + MidiNoteTracker (CD-F12): scale-locked resonance targets.
// Owner module: cd_control. Threading: audio-thread safe (noexcept, fixed storage, double state).
// Quantizes creative resonance/cutoff movement only; not a polyphonic pitch corrector.
#pragma once
#include <cstddef>
#include <cstdint>

namespace cd::control {

enum class Scale { Chromatic, Major, NaturalMinor, PentatonicMajor, PentatonicMinor, Custom };

// 12-bit interval mask (bit i = i semitones above the root). Custom must be nonzero; returns 0 if invalid.
std::uint16_t scaleMask(Scale s, std::uint16_t customMask) noexcept;

class ScaleQuantizer {
public:
    static constexpr double kHysteresisSemitones = 0.15;
    static constexpr double kGlideSeconds = 0.050;  // 95% settling time in log-frequency

    // Rejects: Custom mask 0 or >0xFFF, root outside 0..11, bad A4/sample rate. Keeps old config on failure.
    bool configure(Scale s, std::uint16_t customMask, int root, double a4Hz, double sampleRate) noexcept;
    bool configured() const noexcept { return configured_; }

    // Pure nearest allowed MIDI note (ties go lower), no hysteresis. Returns false for non-finite p.
    bool nearestNote(double midiPitch, int& note) const noexcept;
    double pitchOf(double hz) const noexcept;  // 69 + 12 log2(f/A4); NaN if hz invalid
    double hzOf(double midiPitch) const noexcept;

    // Set new target frequency. Invalid (NaN/Inf/<=0) is rejected (returns false, state unchanged).
    // The first accepted target snaps; later ones glide.
    bool setTarget(double hz) noexcept;
    void step() noexcept;  // per-sample glide
    void advance(std::size_t n) noexcept;
    int note() const noexcept { return note_; }
    double outputPitch() const noexcept { return glidePitch_; }  // MIDI units, glided
    double outputHz() const noexcept { return hzOf(glidePitch_); }

private:
    bool allowed(int note) const noexcept;
    bool configured_ = false;
    std::uint16_t mask_ = 0xFFF;
    int root_ = 0;
    double a4_ = 440.0, fs_ = 48000.0, glideCoef_ = 0.0;
    bool haveNote_ = false;
    int note_ = 69;
    double glidePitch_ = 69.0;
};

// MIDI reference note for key tracking. Last-held wins, sustain retains, all-notes-off immediate
// (clears held and sustained notes), then 500 ms hold and 100 ms return to manual (weight 1 -> 0).
class MidiNoteTracker {
public:
    static constexpr double kHoldSeconds = 0.5;
    static constexpr double kReturnSeconds = 0.1;

    void reset() noexcept;
    void noteOn(int note) noexcept;
    void noteOff(int note) noexcept;
    void sustain(bool down) noexcept;
    void allNotesOff() noexcept;
    void advance(double seconds) noexcept;

    bool hasActiveNote() const noexcept { return active_ >= 0; }
    int activeNote() const noexcept { return active_; }
    int referenceNote() const noexcept { return lastNote_; }  // last note, meaningful while weight()>0
    double weight() const noexcept;                            // 1 active/hold, ramps to 0 over 100 ms

private:
    void recompute() noexcept;
    bool held_[128] = {};
    bool sustained_[128] = {};
    std::uint64_t order_[128] = {};
    std::uint64_t counter_ = 0;
    bool pedal_ = false;
    int active_ = -1;
    int lastNote_ = -1;
    double idle_ = 1e9;
};

}  // namespace cd::control
