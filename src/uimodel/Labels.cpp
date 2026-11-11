// CircuitDrift uimodel: UX-08 labels. See Labels.h. Message thread only.
#include "uimodel/Labels.h"
#include <cmath>
#include <cstdio>

namespace cd::uimodel {

namespace {
std::string fmt(const char* f, double a) {
    char b[96];
    std::snprintf(b, sizeof b, f, a);
    return b;
}
std::string oversamplingName(int factor) { return factor == 1 ? "Off (1x)" : std::to_string(factor) + "x"; }
std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (const auto& x : v) if (!x.empty()) { if (!s.empty()) s += sep; s += x; }
    return s;
}
}  // namespace

std::vector<std::string> zeroLatencyUnavailableFeatures() {
    return {"Pitch Drift Delay", "Oversampling above Off (1x)", "Linear-phase FIR lookahead", "Cross-talk delay", "HQ latency padding"};
}

QualityLabel describeQuality(const runtime::QualityInfo& q, double fs) {
    QualityLabel l;
    l.title = runtime::qualityLabel(q);
    l.oversampling = oversamplingName(q.factor);
    l.family = q.family == dsp::OsFamily::Iir ? "IIR" : "FIR";
    l.draft = q.draft || q.zeroLatency || q.factor < 4;
    if (l.draft) {
        l.draftBadge = "Draft";
        l.draftNote = "Draft antialiasing (below 4x FIR)";
    }
    if (q.familyException) l.familyNote = "IIR low-latency exception";
    const double ms = fs > 0 ? 1000.0 * double(q.latencySamples) / fs : 0.0;
    l.latency = "Latency " + std::to_string(q.latencySamples) + " samples (" + fmt("%.2f", ms) + " ms)";
    if (q.zeroLatency) {
        l.unavailable = zeroLatencyUnavailableFeatures();
    }
    if (q.pitchDelayUnavailable) l.pitchNote = "Pitch Drift Delay unavailable in Zero Latency";
    std::vector<std::string> parts{l.title, l.draftBadge, l.familyNote, l.latency};
    if (!l.unavailable.empty()) parts.push_back("Unavailable: " + join(l.unavailable, ", "));
    parts.push_back(l.pitchNote);
    l.full = join(parts, " | ");
    return l;
}

QualityStatusLabel describeQualityStatus(const runtime::QualityStatus& s, double fs) {
    QualityStatusLabel r;
    r.active = describeQuality(s.active, fs);
    r.activeLine = "Active: " + r.active.full;
    if (s.transitioning && s.incoming.factor > 0) {
        r.hasIncoming = true;
        r.incoming = describeQuality(s.incoming, fs);
        r.incomingLine = "Switching to: " + r.incoming.full;
    }
    if (s.latencyChangePending && s.pending.factor > 0) {
        r.hasPending = true;
        r.pending = describeQuality(s.pending, fs);
        r.pendingLine = "Pending (applies at the next host latency change): " + r.pending.full;
    }
    r.summary = join({r.activeLine, r.incomingLine, r.pendingLine}, " | ");
    return r;
}

std::string hpfLabel(bool enabled, double hz) {
    if (!enabled) return "HPF off - DC passes";
    return "HPF " + fmt("%g", hz) + " Hz";
}

UtilityRangeLabel utilityRange(const std::string& id) {
    UtilityRangeLabel u;
    const core::ParamSpec* s = core::findById(id.c_str());
    if (!s) return u;
    u.id = id; u.min = s->min; u.max = s->max;
    if (id == "input.gain" || id == "output.gain") {
        if (s->min == -s->max) u.text = "\xC2\xB1" + fmt("%g", s->max) + " dB";
        else u.text = fmt("%g", s->min) + " to " + fmt("%g", s->max) + " dB";
    } else if (id == "mix") {
        u.text = fmt("%g", s->min * 100.0) + "\xE2\x80\x93" + fmt("%g", s->max * 100.0) + " %";
    } else if (id == "hpf.frequency") {
        u.text = fmt("%g", s->min) + "\xE2\x80\x93" + fmt("%g", s->max) + " Hz";
    } else {
        u.id.clear();
    }
    return u;
}

std::vector<UtilityRangeLabel> utilityRanges() {
    std::vector<UtilityRangeLabel> v;
    for (const char* id : {"input.gain", "output.gain", "mix", "hpf.frequency"}) v.push_back(utilityRange(id));
    return v;
}

}  // namespace cd::uimodel
