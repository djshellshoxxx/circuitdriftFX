// CircuitDrift uimodel: T08 bundled model tooltip catalogue + 450 ms hover/focus timer state
// machine. Owner: cd_uimodel. Message thread only. Text is compiled in (offline, no network).
// Every entry describes a SYNTHETIC FIXTURE model; none claims a real device or measurement
// (G01 open). Short text <= 60 words, long text <= 250 words, with a "what changes in audio"
// section, per 06-interface "Inspector data and education".
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "blueprint/Compiler.h"

namespace cd::uimodel {

struct TooltipEntry {
    const char* modelId;
    int revision;
    const char* role;          // circuit role (position-dependent roles such as pull-up are roles, not materials)
    const char* material;      // composition statement (honest "not modelled" for fixtures)
    const char* units;         // "name [unit]; ..." exactly as the manifest declares
    const char* provenance;    // always states 'synthetic fixture'
    const char* shortText;
    const char* longText;
    const char* whatChangesInAudio;
    const char* sourceLink;    // bundled local reference, never a URL requiring network
};

const std::vector<TooltipEntry>& tooltipCatalogue();
const TooltipEntry* findTooltip(const std::string& modelId, int revision = 1) noexcept;
// Units string derived from a registry model (devices: params; sources: paramName [V]).
std::string unitsFor(const dsp::DeviceManifest& m);
std::string unitsFor(const blueprint::SourceModel& s);
int wordCount(const char* text) noexcept;
// Empty when complete (all fields non-empty, word limits, 'synthetic fixture' provenance, audio section).
std::string validateEntry(const TooltipEntry& e);

// 450 ms delay timer driven by an injected monotonic microsecond clock; frame callers poll tick().
// Opens at the first tick with elapsed >= 450 ms (never earlier), i.e. within one UI frame of 450 ms
// when ticks run every frame. Escape dismisses until the pointer leaves or focus moves.
class TooltipTimer {
public:
    enum class State : std::uint8_t { Idle, Pending, Open, Dismissed };
    static constexpr std::int64_t kDelayUs = 450000;
    using ClockUs = std::function<std::int64_t()>;
    explicit TooltipTimer(ClockUs clock) : clock_(std::move(clock)) {}
    void start(const std::string& targetId);   // hover enter or keyboard focus
    void leave();                               // pointer leave / focus lost (pinned tooltips stay)
    void escape();
    void pin();                                 // touch: keep open until unpin()/escape()
    void unpin();
    State tick();                               // call every UI frame
    State state() const noexcept { return state_; }
    bool pinned() const noexcept { return pinned_; }
    const std::string& target() const noexcept { return target_; }
    std::int64_t deadlineUs() const noexcept { return startUs_ + kDelayUs; }
private:
    ClockUs clock_;
    State state_ = State::Idle;
    std::string target_;
    std::int64_t startUs_ = 0;
    bool pinned_ = false;
};

}  // namespace cd::uimodel
