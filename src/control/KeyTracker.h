// cd_control: reference-note tracking for key/scale lock (CD-F12 sources Manual/MIDI/Audio).
// MIDI: last-held note wins; sustain (CC64>=64) retains released notes until the pedal lifts;
// all-notes-off (CC123, also CC120) clears every note and the pedal immediately and returns to the
// manual reference without the hold; with no active note the last reference is held 500 ms, then the
// reference weight ramps 1->0 over 100 ms (manual reference). Timers are sample counters (block
// independent). Audio: the processor feeds the detected note; invalid/no pitch -> immediate 100 ms return
// (the detector already applied its own 200 ms hold).
//
// Threading: noexcept, no allocation (fixed note table), single owner thread (audio).
#pragma once
#include <cstdint>
#include <span>
#include "control/ControlTypes.h"

namespace cd::control {

class KeyTracker {
public:
    static constexpr double kHoldS = 0.5;
    static constexpr double kReturnS = 0.1;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // MIDI source: process events (ordered by frame) across a block of `frames` samples.
    void advanceMidi(std::span<const MidiEvent> events, int frames) noexcept;
    // Audio source: pitch is a MIDI float (valid) or no pitch (valid=false); then time advances.
    void advanceAudio(bool valid, double midiPitch, int frames) noexcept;
    // Manual source / disabled: no reference note; time advances through any return ramp.
    void advanceManual(int frames) noexcept;

    // 1 while a note (or its hold) is the reference, ramping to 0 on return, 0 when manual.
    double weight() const noexcept { return weight_; }
    double notePitch() const noexcept { return note_; }   // last reference pitch (MIDI float)
    KeyState state() const noexcept { return state_; }
    bool pedalDown() const noexcept { return pedal_; }
    int heldCount() const noexcept { return count_; }

private:
    struct Slot { uint8_t note; bool held; bool sustained; };
    static constexpr int kMax = 32;
    void handle(const MidiEvent& e) noexcept;
    void removeAt(int i) noexcept;
    void touch(uint8_t note) noexcept;     // note-on: move/add to the end (most recent)
    void evaluate(bool allowHold) noexcept;  // recompute active note / state after an event
    void tickTime(int frames) noexcept;
    void releaseAll() noexcept;

    double fs_ = 48000.0;
    Slot slots_[kMax]{};
    int count_ = 0;
    bool pedal_ = false;
    KeyState state_ = KeyState::ManualReference;
    double note_ = 69.0;
    double weight_ = 0.0;
    double holdLeft_ = 0.0, returnLeft_ = 0.0;  // samples
};

}  // namespace cd::control
