#include "control/KeyTracker.h"
#include <algorithm>

namespace cd::control {

void KeyTracker::prepare(double sampleRate) noexcept {
    fs_ = sampleRate > 0 ? sampleRate : 48000.0;
    reset();
}

void KeyTracker::reset() noexcept {
    count_ = 0;
    pedal_ = false;
    state_ = KeyState::ManualReference;
    note_ = 69.0;
    weight_ = 0.0;
    holdLeft_ = returnLeft_ = 0.0;
}

void KeyTracker::removeAt(int i) noexcept {
    for (int k = i; k + 1 < count_; ++k) slots_[k] = slots_[k + 1];
    --count_;
}

void KeyTracker::touch(uint8_t note) noexcept {
    for (int i = 0; i < count_; ++i)
        if (slots_[i].note == note) { removeAt(i); break; }
    if (count_ == kMax) removeAt(0);  // drop the oldest; bounded table
    slots_[count_++] = Slot{note, true, false};
}

void KeyTracker::releaseAll() noexcept {
    count_ = 0;
    pedal_ = false;
}

// After an event: the active note is the most recent entry that is held or sustained.
void KeyTracker::evaluate(bool allowHold) noexcept {
    int active = -1;
    for (int i = count_ - 1; i >= 0; --i)
        if (slots_[i].held || (slots_[i].sustained && pedal_)) { active = i; break; }
    if (active >= 0) {
        note_ = double(slots_[active].note);
        weight_ = 1.0;
        state_ = KeyState::NoteActive;
        holdLeft_ = returnLeft_ = 0.0;
    } else if (state_ == KeyState::NoteActive) {
        if (allowHold) {
            state_ = KeyState::NoteHold;
            holdLeft_ = kHoldS * fs_;
        } else {
            state_ = KeyState::Returning;
            returnLeft_ = kReturnS * fs_;
        }
    } else if (state_ == KeyState::NoteHold && !allowHold) {
        state_ = KeyState::Returning;
        returnLeft_ = kReturnS * fs_;
    }
}

void KeyTracker::handle(const MidiEvent& e) noexcept {
    const uint8_t type = e.status & 0xF0u;
    bool allowHold = true;
    if (type == 0x90 && e.data2 > 0) {
        touch(e.data1);
    } else if (type == 0x80 || type == 0x90) {
        for (int i = 0; i < count_; ++i)
            if (slots_[i].note == e.data1) {
                slots_[i].held = false;
                if (pedal_) slots_[i].sustained = true;
                else removeAt(i);
                break;
            }
    } else if (type == 0xB0) {
        if (e.data1 == 64) {
            const bool down = e.data2 >= 64;
            if (down && !pedal_) {
                pedal_ = true;
                for (int i = 0; i < count_; ++i) if (slots_[i].held) slots_[i].sustained = true;
            } else if (!down && pedal_) {
                pedal_ = false;
                for (int i = count_ - 1; i >= 0; --i) if (!slots_[i].held) removeAt(i);
            }
        } else if (e.data1 == 123 || e.data1 == 120) {
            releaseAll();
            allowHold = false;  // releases immediately: no 500 ms hold
        }
    } else {
        return;
    }
    evaluate(allowHold);
}

void KeyTracker::tickTime(int frames) noexcept {
    if (frames <= 0) return;
    double left = double(frames);
    if (state_ == KeyState::NoteHold) {
        const double used = std::min(left, holdLeft_);
        holdLeft_ -= used;
        left -= used;
        if (holdLeft_ <= 0.0) {
            state_ = KeyState::Returning;
            returnLeft_ = kReturnS * fs_;
        }
    }
    if (state_ == KeyState::Returning) {
        returnLeft_ -= left;
        if (returnLeft_ <= 0.0) {
            returnLeft_ = 0.0;
            weight_ = 0.0;
            state_ = KeyState::ManualReference;
        } else {
            weight_ = returnLeft_ / (kReturnS * fs_);
        }
    }
}

void KeyTracker::advanceMidi(std::span<const MidiEvent> events, int frames) noexcept {
    int cursor = 0;
    for (const MidiEvent& e : events) {
        const int f = std::clamp(int(std::min<uint32_t>(e.frame, 0x7FFFFFFFu)), cursor, std::max(frames, cursor));
        tickTime(f - cursor);
        cursor = f;
        handle(e);
    }
    tickTime(frames - cursor);
}

void KeyTracker::advanceAudio(bool valid, double midiPitch, int frames) noexcept {
    if (valid) {
        note_ = midiPitch;
        weight_ = 1.0;
        state_ = KeyState::NoteActive;
        holdLeft_ = returnLeft_ = 0.0;
    } else if (state_ == KeyState::NoteActive || state_ == KeyState::NoteHold) {
        state_ = KeyState::Returning;
        returnLeft_ = kReturnS * fs_;
    }
    tickTime(frames);
}

void KeyTracker::advanceManual(int frames) noexcept {
    if (state_ == KeyState::NoteActive || state_ == KeyState::NoteHold) {
        state_ = KeyState::Returning;
        returnLeft_ = kReturnS * fs_;
    }
    tickTime(frames);
}

}  // namespace cd::control
