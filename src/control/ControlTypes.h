// cd_control: shared plain types for the per-block control interface.
// Threading: POD / views only; safe on the audio thread. No ownership of audio or MIDI memory.
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace cd::control {

struct MidiEvent {
    uint32_t frame = 0;   // sample offset inside the block; events must be ordered by frame
    uint8_t status = 0;   // raw MIDI status byte (0x9n note on, 0x8n note off, 0xBn controller)
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

struct AudioView {
    const double* const* channels = nullptr;  // channels[c][0..frames)
    int numChannels = 0;
};

struct ControlContext {
    uint64_t sampleStart = 0;          // host sample position of the block (informational)
    int frames = 0;                    // 0..maxBlock
    std::optional<double> bpm;         // absent => explicit 120 BPM fallback + status flag
    std::optional<double> ppq;         // quarter-note position at block start; absent => free-run
    int timeSigNum = 4;
    int timeSigDen = 4;
    bool playing = true;
    std::span<const MidiEvent> midi;   // ordered by frame
};

enum class KeySource : int { Manual = 0, Midi = 1, Audio = 2 };
enum class KeyState : uint8_t { Off, ManualReference, NoteActive, NoteHold, Returning };
enum class PitchState : uint8_t { None, Low, Acquired };

struct ControlStatus {
    bool tempoActive = false;
    bool tempoFallback = false;        // tempo.enabled and host BPM unavailable (120 used)
    double tempoPhase = 0.0;           // cycles, [0,1)
    KeyState keyState = KeyState::Off;
    bool scaleLockActive = false;      // key.enabled: creative cutoff quantized
    bool pitchDriftHeld = false;       // "Resonance lock; pitch drift held."
    double referenceNote = 69.0;       // MIDI float currently used as resonance reference
    PitchState pitchState = PitchState::None;
    double pitchHz = 0.0;
    double pitchConfidence = 0.0;
    bool harmonicAvailable[2] = {false, false};  // false => targeting disabled (no calibration map)
    bool sidechainConnected = false;
    bool sidechainMissing = false;     // External/Blend selected but no bus
    bool blockClamped = false;         // frames exceeded prepared maxBlock
};

}  // namespace cd::control
