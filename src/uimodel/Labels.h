// CircuitDrift uimodel: UX-08 quality / latency / utility label audit (pure functions).
// Owner: cd_uimodel. Message thread (allocates). Inputs are the engine's public QualityInfo /
// QualityStatus; no engine state is touched.
// Rules (08-ux "UX-08", 01-runtime "Oversampling and quality"):
//  * Minimum normal quality is 4x FIR: 1x ("Off") and 2x are labelled Draft (antialiasing).
//  * IIR is labelled the "low-latency exception" (never presented as a production default).
//  * Active quality, the incoming (crossfading) quality and the pending (latency-change staged)
//    quality are always distinct strings; pending is never shown as if it were active.
//  * Zero Latency lists the temporarily unavailable features; it is always Draft.
//  * HPF bypassed shows "DC passes". Utility ranges come from the registry: input/output
//    +/-24 dB, mix 0-100 %, HPF 10-250 Hz (checked exactly by tests).
#pragma once
#include <string>
#include <vector>
#include "runtime/Engine.h"

namespace cd::uimodel {

struct QualityLabel {
    std::string title;          // runtime::qualityLabel(q), e.g. "Studio 4x FIR", "Manual 2x FIR (Draft)"
    std::string oversampling;   // "Off (1x)", "2x", "4x", "8x", "16x"
    std::string family;         // "FIR" | "IIR"
    bool draft = false;
    std::string draftBadge;     // "Draft" or empty
    std::string draftNote;      // explains Draft antialiasing, empty when not Draft
    std::string familyNote;     // "IIR low-latency exception" or empty
    std::string latency;        // "Latency 0 samples (0.00 ms)"
    std::vector<std::string> unavailable;   // Zero Latency: features temporarily unavailable
    std::string pitchNote;      // pitch-delay requested while Zero Latency, else empty
    std::string full;           // single-line combination of the above
};

QualityLabel describeQuality(const runtime::QualityInfo& q, double sampleRate);
std::vector<std::string> zeroLatencyUnavailableFeatures();

struct QualityStatusLabel {
    QualityLabel active;
    std::string activeLine;     // "Active: <full>"
    bool hasIncoming = false;
    QualityLabel incoming;
    std::string incomingLine;   // "Switching to: <full>" (crossfade/pre-roll), empty otherwise
    bool hasPending = false;
    QualityLabel pending;
    std::string pendingLine;    // "Pending (applies at the next host latency change): <full>"
    std::string summary;        // all non-empty lines joined by " | "
};
QualityStatusLabel describeQualityStatus(const runtime::QualityStatus& s, double sampleRate);

std::string hpfLabel(bool enabled, double cutoffHz);   // "HPF 15 Hz" | "HPF off - DC passes"

struct UtilityRangeLabel {
    std::string id;
    double min = 0, max = 0;
    std::string text;           // "\xC2\xB1""24 dB", "0\xE2\x80\x93""100 %", "10\xE2\x80\x93""250 Hz"
};
// Derived from the registry (ranges are read, not duplicated); empty id when not a utility control.
UtilityRangeLabel utilityRange(const std::string& id);
std::vector<UtilityRangeLabel> utilityRanges();   // input.gain, output.gain, mix, hpf.frequency

}  // namespace cd::uimodel
