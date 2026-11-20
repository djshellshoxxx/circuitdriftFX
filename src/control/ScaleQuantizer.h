// cd_control: scale masks and resonance quantizer (CD-F12). Quantizes only CREATIVE cutoff/resonance
// movement, never kelvin, rail voltage or other electrical state. Not a polyphonic pitch corrector.
//
// Scale mask: bit i set => interval i semitones above the root is allowed (12 bits). key.mask custom
// scale uses the same convention; a zero mask is treated as chromatic. Pitch p = 69+12*log2(f/A4).
// Nearest allowed integer MIDI note, ties -> lower note. Hysteresis 0.15 st around boundaries (the
// target must pass the midpoint by 0.15 st before the note changes). Glide 50 ms linear in log-frequency.
//
// Threading: noexcept, no allocation; single owner thread (audio thread in the processor).
#pragma once
#include <cmath>
#include <cstdint>

namespace cd::control {

namespace scale {
inline constexpr uint16_t kChromatic = 0x0FFF;
// key.scale enum: 0 Chromatic, 1 Major, 2 Natural Minor, 3 Pentatonic Major, 4 Pentatonic Minor, 5 Custom.
uint16_t maskFor(int scaleIndex, uint16_t customMask) noexcept;
bool isAllowed(uint16_t mask, int root, int midiNote) noexcept;
// Stateless nearest allowed note (ties -> lower). Bounded local search; mask 0 treated as chromatic.
// Nonfinite pitch uses A4; finite pitches outside the int domain saturate with search headroom.
int nearestAllowed(double pitch, uint16_t mask, int root) noexcept;
inline double midiToHz(double midi, double a4Hz = 440.0) noexcept { return a4Hz * std::exp2((midi - 69.0) / 12.0); }
inline double hzToMidi(double hz, double a4Hz = 440.0) noexcept { return 69.0 + 12.0 * std::log2(hz / a4Hz); }
}  // namespace scale

class ResonanceQuantizer {
public:
    static constexpr double kHysteresisSt = 0.15;
    static constexpr double kGlideS = 0.05;

    void configure(uint16_t mask, int root) noexcept;
    void reset() noexcept { haveNote_ = haveOutput_ = false; glideRemaining_ = 0; }
    // Update with target pitch p (MIDI float) and elapsed time; returns glided pitch (MIDI float).
    // dtSeconds==0 still re-evaluates the note decision but advances no glide time.
    double update(double pitch, double dtSeconds) noexcept;
    int note() const noexcept { return note_; }
    double glided() const noexcept { return current_; }
    // Hysteresis decision only (no glide); exposed for deterministic tie/hysteresis tests.
    int decide(double pitch) noexcept;

private:
    uint16_t mask_ = scale::kChromatic;
    int root_ = 0;
    bool haveNote_ = false;
    bool haveOutput_ = false;  // scale changes invalidate decisions, never an initialized glide
    int note_ = 69;
    int glideNote_ = 69;
    double current_ = 69.0, start_ = 69.0, glideRemaining_ = 0.0;
};

}  // namespace cd::control

