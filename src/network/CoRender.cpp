// CircuitDrift network: co-render harness and replay (see CoRender.h).
#include "network/CoRender.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace cd::network {

namespace {

void putU64(community::Sha256& h, std::uint64_t v) {
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
    h.update(b, 8);
}
void putF64(community::Sha256& h, double v) { putU64(h, std::bit_cast<std::uint64_t>(v)); }
void putDoubles(community::Sha256& h, const double* p, std::size_t n) {
    std::uint8_t buf[8 * 64];
    while (n) {
        const std::size_t m = std::min<std::size_t>(n, 64);
        for (std::size_t i = 0; i < m; ++i) {
            const std::uint64_t v = std::bit_cast<std::uint64_t>(p[i]);
            for (int k = 0; k < 8; ++k) buf[8 * i + k] = static_cast<std::uint8_t>(v >> (8 * k));
        }
        h.update(buf, 8 * m);
        p += m;
        n -= m;
    }
}
void putUuid(community::Sha256& h, const Uuid128& u) { putU64(h, u.hi); putU64(h, u.lo); }

Digest automationDigest(const std::vector<double>& a) {
    community::Sha256 h;
    h.update("cd.network.automation/1");
    putU64(h, a.size());
    putDoubles(h, a.data(), a.size());
    return h.finish();
}

Digest combineContent(std::uint32_t channels, std::uint64_t length, std::vector<community::Sha256>& perChannel) {
    community::Sha256 h;
    h.update("cd.network.content/1");
    putU64(h, channels);
    putU64(h, length);
    for (auto& c : perChannel) {
        const auto d = c.finish();
        h.update(d.data(), d.size());
    }
    return h.finish();
}

std::vector<std::size_t> memberOrder(const CoRenderSpec& s) {
    std::vector<std::size_t> o(s.members.size());
    for (std::size_t i = 0; i < o.size(); ++i) o[i] = i;
    std::sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return s.members[a].member < s.members[b].member; });
    return o;
}

double activityAt(const std::vector<double>& a, std::uint64_t q) {
    if (a.empty()) return 1.0;
    const double v = q < a.size() ? a[q] : a.back();
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

std::string hex(const Digest& d) { return community::toHex(d).substr(0, 16); }

}  // namespace

const char* toString(CoRenderError e) noexcept {
    switch (e) {
        case CoRenderError::None: return "ok";
        case CoRenderError::InvalidSpec: return "invalid co-render spec";
        case CoRenderError::DuplicateMember: return "duplicate member UUID";
        case CoRenderError::InvalidBleed: return "invalid bleed configuration";
        case CoRenderError::InvalidPsu: return "invalid PSU manifest";
        case CoRenderError::BadFeed: return "feed outside the member/length";
        case CoRenderError::IncompleteFeed: return "not every member delivered the full length";
        case CoRenderError::Unconverged: return "rail/demand fixed point did not converge";
    }
    return "unknown";
}

const char* toString(ReplayError e) noexcept {
    switch (e) {
        case ReplayError::None: return "ok";
        case ReplayError::FormatVersion: return "influence bundle format version differs";
        case ReplayError::SampleRateMismatch: return "sample rate differs";
        case ReplayError::StartMismatch: return "render start differs";
        case ReplayError::LengthMismatch: return "render length differs";
        case ReplayError::RoutingMismatch: return "group topology/routing differs";
        case ReplayError::ModelMismatch: return "model version or parameters differ";
        case ReplayError::SeedMismatch: return "bleed seed differs";
        case ReplayError::ContentHashMismatch: return "input content hash differs";
        case ReplayError::AutomationHashMismatch: return "input automation hash differs";
        case ReplayError::UnknownMember: return "member is not part of the bundle";
        case ReplayError::RenderFailed: return "re-render failed";
        case ReplayError::StreamMismatch: return "replayed streams exceed 1e-6 peak error";
    }
    return "unknown";
}

void CoRenderSession::fail(CoRenderError e, std::string why) {
    if (error_ != CoRenderError::None) return;
    error_ = e;
    reason_ = std::string(toString(e)) + (why.empty() ? "" : ": " + why);
}

CoRenderSession::CoRenderSession(const CoRenderSpec& spec) : spec_(spec) {
    for (auto& m : spec_.members) m.audio.clear();   // the session only uses fed data
    const std::size_t n = spec.members.size();
    if (n == 0 || n > kMaxMembersPerGroup || spec.lengthSamples == 0 || !(spec.sampleRate >= 8000 && spec.sampleRate <= 768000) ||
        spec.maxIterations == 0 || spec.maxIterations > kMaxFixedPointIterations || !(spec.tolerance > 0)) {
        fail(CoRenderError::InvalidSpec, "members 1..32, length > 0, Fs, iterations 1..8, tolerance > 0");
        return;
    }
    if (!validPsuSpec(spec.psu)) { fail(CoRenderError::InvalidPsu, ""); return; }
    if (const BleedError be = validateBleed(spec.bleed); be != BleedError::None) { fail(CoRenderError::InvalidBleed, toString(be)); return; }
    for (const auto& m : spec.members)
        if ((m.channels != 1 && m.channels != 2) || m.member.isNil() || !std::isfinite(m.quiescentA) || !std::isfinite(m.signalA)) {
            fail(CoRenderError::InvalidSpec, "member channels 1|2, non-nil UUID, finite demand parameters");
            return;
        }
    order_ = memberOrder(spec);
    for (std::size_t i = 1; i < n; ++i)
        if (spec.members[order_[i]].member == spec.members[order_[i - 1]].member) { fail(CoRenderError::DuplicateMember, ""); return; }
    orderOf_.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) orderOf_[order_[i]] = i;
    quanta_ = (spec.lengthSamples + kQuantumFrames - 1) / kQuantumFrames;

    BundleManifest& mf = bundle_.manifest;
    mf.sampleRate = spec.sampleRate;
    mf.startSample = spec.startSample;
    mf.lengthSamples = spec.lengthSamples;
    mf.bleedSeed = spec.bleed.seed;
    std::vector<Uuid128> ids;
    for (std::size_t o : order_) {
        ids.push_back(spec.members[o].member);
        mf.channels.push_back(spec.members[o].channels);
    }
    mf.memberOrder = ids;
    {
        community::Sha256 h;
        h.update("cd.network.topology/1");
        for (std::size_t i = 0; i < n; ++i) {
            putUuid(h, ids[i]);
            putU64(h, mf.channels[i]);
            putU64(h, spec.members[order_[i]].bleedOptIn ? 1 : 0);
        }
        putU64(h, spec.bleed.off ? 1 : 0);
        putF64(h, spec.bleed.levelDb);
        putU64(h, spec.bleed.topology ? 1 : 0);
        if (spec.bleed.topology) {
            for (const auto& u : spec.bleed.topology->members) putUuid(h, u);
            putDoubles(h, spec.bleed.topology->weights.data(), spec.bleed.topology->weights.size());
        }
        mf.topology = h.finish();
    }
    {
        community::Sha256 h;
        h.update("cd.network.model-parameters/1");
        const PsuSpec& p = spec.psu;
        putU64(h, p.manifestId);
        putU64(h, p.railModelId);
        putF64(h, p.nominalRailV);
        putF64(h, p.sourceV);
        putF64(h, p.sourceResistanceOhm);
        putF64(h, p.capacitanceF);
        for (std::size_t o : order_) { putF64(h, spec.members[o].quiescentA); putF64(h, spec.members[o].signalA); }
        putU64(h, spec.maxIterations);
        putF64(h, spec.tolerance);
        mf.modelParameters = h.finish();
    }
    mf.inputs.resize(n);
    staged_.resize(n);
    hashers_.resize(n);
    delivered_.assign(n, 0);
    bundle_.bleed.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = spec.members[order_[i]];
        mf.inputs[i].member = m.member;
        mf.inputs[i].channels = m.channels;
        mf.inputs[i].automation = automationDigest(m.activity);
        staged_[i].assign(m.channels, std::vector<double>(quanta_ * kQuantumFrames, 0.0));
        hashers_[i].resize(m.channels);
        bundle_.bleed[i].assign(m.channels, std::vector<double>(spec.lengthSamples, 0.0));
    }
    bundle_.railFraction.reserve(quanta_);
    bundle_.iterations.reserve(quanta_);
    (void)buildBleedWeights(spec.bleed, ids, W_);
    sources_.assign(n, BleedSource{});
    sinks_.assign(n, BleedSink{});
    psu_ = ReservoirPsu(spec.psu, spec.sampleRate);
}

bool CoRenderSession::feed(std::size_t memberIndex, const double* const* data, std::size_t frames) {
    if (error_ != CoRenderError::None) return false;
    if (memberIndex >= orderOf_.size() || !data) { fail(CoRenderError::BadFeed, "unknown member"); return false; }
    const std::size_t o = orderOf_[memberIndex];
    if (delivered_[o] + frames > spec_.lengthSamples) { fail(CoRenderError::BadFeed, "beyond render length"); return false; }
    for (std::size_t c = 0; c < staged_[o].size(); ++c) {
        if (!data[c]) { fail(CoRenderError::BadFeed, "null channel"); return false; }
        std::copy(data[c], data[c] + frames, staged_[o][c].begin() + static_cast<std::ptrdiff_t>(delivered_[o]));
        putDoubles(hashers_[o][c], data[c], frames);
    }
    delivered_[o] += frames;
    processReady();
    return error_ == CoRenderError::None;
}

void CoRenderSession::processReady() {
    std::uint64_t minDelivered = *std::min_element(delivered_.begin(), delivered_.end());
    while (error_ == CoRenderError::None && nextQuantum_ < quanta_) {
        const std::uint64_t end = std::min<std::uint64_t>((nextQuantum_ + 1) * kQuantumFrames, spec_.lengthSamples);
        if (minDelivered < end) break;
        if (!processQuantum(nextQuantum_)) break;
        ++nextQuantum_;
    }
}

bool CoRenderSession::processQuantum(std::uint64_t q) {
    const std::size_t n = order_.size();
    const std::uint64_t off = q * kQuantumFrames;
    const std::size_t frames = static_cast<std::size_t>(std::min<std::uint64_t>(kQuantumFrames, spec_.lengthSamples - off));
    // Rail-independent part of each member's demand, fixed member-UUID order.
    double base = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = spec_.members[order_[i]];
        double e = 0;
        for (const auto& ch : staged_[i])
            for (std::size_t s = 0; s < frames; ++s) e += ch[off + s] * ch[off + s];
        const double rms = std::sqrt(e / double(frames * staged_[i].size()));
        base += m.quiescentA + m.signalA * activityAt(m.activity, q) * rms;
    }
    // One shared PSU solve per evaluation; bounded secant-accelerated fixed point on r = F(r).
    const double v0 = psu_.volts();
    auto F = [&](double rr, double& vOut) {
        vOut = psu_.trial(v0, base * rr);
        return psu_.fractionOf(vOut);
    };
    double v = v0;
    double r0 = psu_.railFraction();
    double f0 = F(r0, v);
    std::uint32_t it = 1;
    bool converged = std::fabs(f0 - r0) <= spec_.tolerance;
    double r = f0;
    double r1 = f0, f1 = f0;
    while (!converged && it < spec_.maxIterations) {
        if (it == 1) {
            r1 = f0;                                     // plain fixed-point step
        } else {
            const double d = (f1 - r1) - (f0 - r0);
            const double next = d != 0.0 ? r1 - (f1 - r1) * (r1 - r0) / d : f1;
            r0 = r1;
            f0 = f1;
            r1 = std::isfinite(next) ? std::clamp(next, 0.0, 4.0) : f1;
        }
        f1 = F(r1, v);
        ++it;
        r = f1;
        converged = std::fabs(f1 - r1) <= spec_.tolerance;
    }
    if (!converged || !std::isfinite(r)) {
        fail(CoRenderError::Unconverged, "quantum " + std::to_string(q) + " after " + std::to_string(it) + " iterations");
        return false;
    }
    psu_.setVolts(v);
    maxIterUsed_ = std::max(maxIterUsed_, it);
    bundle_.railFraction.push_back(r);
    bundle_.iterations.push_back(static_cast<std::uint8_t>(it));
    // Bleed from the pre-circuit taps (zero latency in the deterministic path).
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = spec_.members[order_[i]];
        sources_[i] = BleedSource{};
        sources_[i].channels = m.channels;
        sources_[i].gain = m.bleedOptIn ? 1.0 : 0.0;
        sinks_[i] = BleedSink{};
        sinks_[i].channels = m.channels;
        sinks_[i].enabled = m.bleedOptIn;
        for (std::uint32_t c = 0; c < m.channels; ++c) {
            sources_[i].ch[c] = staged_[i][c].data() + off;
            sinks_[i].ch[c] = bundle_.bleed[i][c].data() + off;
        }
    }
    mixBleed(W_, n, sources_, sinks_, frames);
    return true;
}

CoRenderResult CoRenderSession::finish() {
    CoRenderResult r;
    if (error_ == CoRenderError::None)
        for (std::size_t i = 0; i < delivered_.size(); ++i)
            if (delivered_[i] != spec_.lengthSamples) { fail(CoRenderError::IncompleteFeed, ""); break; }
    if (error_ == CoRenderError::None && nextQuantum_ != quanta_) fail(CoRenderError::IncompleteFeed, "quanta pending");
    r.error = error_;
    r.reason = reason_;
    if (error_ != CoRenderError::None) return r;
    for (std::size_t i = 0; i < staged_.size(); ++i)
        bundle_.manifest.inputs[i].content = combineContent(static_cast<std::uint32_t>(staged_[i].size()), spec_.lengthSamples, hashers_[i]);
    r.maxIterationsUsed = maxIterUsed_;
    r.bundle = std::move(bundle_);
    bundle_ = InfluenceBundle{};
    return r;
}

CoRenderResult coRender(const CoRenderSpec& spec, std::span<const FeedStep> plan) {
    CoRenderSession s(spec);
    if (s.error() != CoRenderError::None) return s.finish();
    std::vector<std::uint64_t> fed(spec.members.size(), 0);
    auto deliver = [&](std::size_t i, std::size_t frames) {
        const auto& m = spec.members[i];
        frames = static_cast<std::size_t>(std::min<std::uint64_t>(frames, spec.lengthSamples - fed[i]));
        if (frames == 0) return;
        const double* ptr[kMaxChannels] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < m.channels && c < kMaxChannels; ++c) {
            if (c >= m.audio.size() || m.audio[c].size() < spec.lengthSamples) { ptr[c] = nullptr; continue; }
            ptr[c] = m.audio[c].data() + fed[i];
        }
        (void)s.feed(i, ptr, frames);
        fed[i] += frames;
    };
    for (const FeedStep& st : plan)
        if (st.member < spec.members.size()) deliver(st.member, st.frames);
    for (std::size_t i = 0; i < spec.members.size(); ++i) deliver(i, static_cast<std::size_t>(spec.lengthSamples));
    return s.finish();
}

InputDigest digestInput(const CoRenderMemberInput& in, std::uint64_t lengthSamples) {
    InputDigest d;
    d.member = in.member;
    d.channels = in.channels;
    std::vector<community::Sha256> hs(in.channels);
    for (std::uint32_t c = 0; c < in.channels; ++c)
        if (c < in.audio.size()) putDoubles(hs[c], in.audio[c].data(), std::min<std::uint64_t>(lengthSamples, in.audio[c].size()));
    d.content = combineContent(in.channels, lengthSamples, hs);
    d.automation = automationDigest(in.activity);
    return d;
}

ReplayResult compareManifests(const BundleManifest& a, const BundleManifest& b) {
    ReplayResult r;
    auto bad = [&](ReplayError e, std::string why = {}) {
        r.error = e;
        r.reason = std::string(toString(e)) + (why.empty() ? "" : ": " + why);
        return r;
    };
    if (a.formatVersion != b.formatVersion) return bad(ReplayError::FormatVersion);
    if (!(a.sampleRate == b.sampleRate)) return bad(ReplayError::SampleRateMismatch);
    if (a.startSample != b.startSample)
        return bad(ReplayError::StartMismatch, std::to_string(a.startSample) + " vs " + std::to_string(b.startSample));
    if (a.lengthSamples != b.lengthSamples || a.quantum != b.quantum) return bad(ReplayError::LengthMismatch);
    if (a.memberOrder != b.memberOrder || a.channels != b.channels || a.topology != b.topology)
        return bad(ReplayError::RoutingMismatch);
    if (a.psuModelVersion != b.psuModelVersion || a.bleedModelVersion != b.bleedModelVersion ||
        a.demandModelVersion != b.demandModelVersion || a.modelParameters != b.modelParameters)
        return bad(ReplayError::ModelMismatch);
    if (a.bleedSeed != b.bleedSeed) return bad(ReplayError::SeedMismatch);
    if (a.inputs.size() != b.inputs.size()) return bad(ReplayError::RoutingMismatch);
    for (std::size_t i = 0; i < a.inputs.size(); ++i) {
        if (a.inputs[i].content != b.inputs[i].content)
            return bad(ReplayError::ContentHashMismatch, "member order #" + std::to_string(i) + " " + hex(a.inputs[i].content) +
                                                             " vs " + hex(b.inputs[i].content));
        if (a.inputs[i].automation != b.inputs[i].automation)
            return bad(ReplayError::AutomationHashMismatch, "member order #" + std::to_string(i));
    }
    return r;
}

ReplayResult replay(const InfluenceBundle& ref, const CoRenderSpec& spec, std::span<const FeedStep> plan) {
    const CoRenderResult again = coRender(spec, plan);
    ReplayResult r;
    if (!again.ok()) {
        r.error = ReplayError::RenderFailed;
        r.reason = std::string(toString(r.error)) + ": " + again.reason;
        return r;
    }
    r = compareManifests(ref.manifest, again.bundle.manifest);
    if (!r.valid()) return r;
    double peak = 0;
    bool shape = ref.railFraction.size() == again.bundle.railFraction.size() && ref.bleed.size() == again.bundle.bleed.size();
    for (std::size_t q = 0; shape && q < ref.railFraction.size(); ++q)
        peak = std::max(peak, std::fabs(ref.railFraction[q] - again.bundle.railFraction[q]));
    for (std::size_t i = 0; shape && i < ref.bleed.size(); ++i) {
        if (ref.bleed[i].size() != again.bundle.bleed[i].size()) { shape = false; break; }
        for (std::size_t c = 0; c < ref.bleed[i].size(); ++c) {
            if (ref.bleed[i][c].size() != again.bundle.bleed[i][c].size()) { shape = false; break; }
            for (std::size_t s = 0; s < ref.bleed[i][c].size(); ++s)
                peak = std::max(peak, std::fabs(ref.bleed[i][c][s] - again.bundle.bleed[i][c][s]));
        }
    }
    r.peakError = shape ? peak : INFINITY;
    if (!shape || !(peak <= kReplayPeakTolerance)) {
        r.error = ReplayError::StreamMismatch;
        r.reason = toString(r.error);
    }
    return r;
}

ReplayResult BundleReader::open(const InfluenceBundle& b, const InputDigest& own, double sampleRate, std::uint64_t startSample) {
    bundle_ = nullptr;
    ReplayResult r;
    auto bad = [&](ReplayError e) { r.error = e; r.reason = toString(e); return r; };
    const auto& mf = b.manifest;
    if (mf.formatVersion != kInfluenceBundleVersion) return bad(ReplayError::FormatVersion);
    if (!(mf.sampleRate == sampleRate)) return bad(ReplayError::SampleRateMismatch);
    if (mf.startSample != startSample) return bad(ReplayError::StartMismatch);
    std::size_t idx = mf.memberOrder.size();
    for (std::size_t i = 0; i < mf.memberOrder.size(); ++i) if (mf.memberOrder[i] == own.member) idx = i;
    if (idx == mf.memberOrder.size() || idx >= mf.inputs.size() || idx >= b.bleed.size()) return bad(ReplayError::UnknownMember);
    if (mf.inputs[idx].channels != own.channels) return bad(ReplayError::RoutingMismatch);
    if (mf.inputs[idx].content != own.content) return bad(ReplayError::ContentHashMismatch);
    if (mf.inputs[idx].automation != own.automation) return bad(ReplayError::AutomationHashMismatch);
    bundle_ = &b;
    index_ = idx;
    return r;
}

double BundleReader::read(std::uint64_t absoluteSample, std::uint32_t frames, double* const* out, std::uint32_t channels) const noexcept {
    if (!bundle_) {
        for (std::uint32_t c = 0; c < channels; ++c) if (out && out[c]) std::memset(out[c], 0, frames * sizeof(double));
        return 1.0;
    }
    const auto& mf = bundle_->manifest;
    const auto& streams = bundle_->bleed[index_];
    for (std::uint32_t c = 0; c < channels; ++c) {
        if (!out || !out[c]) continue;
        const std::vector<double>* src = c < streams.size() ? &streams[c] : (streams.empty() ? nullptr : &streams[0]);
        for (std::uint32_t s = 0; s < frames; ++s) {
            const std::uint64_t t = absoluteSample + s;
            out[c][s] = (src && t >= mf.startSample && t - mf.startSample < mf.lengthSamples) ? (*src)[t - mf.startSample] : 0.0;
        }
    }
    if (absoluteSample < mf.startSample || absoluteSample - mf.startSample >= mf.lengthSamples) return 1.0;
    const std::uint64_t q = (absoluteSample - mf.startSample) / kQuantumFrames;
    return q < bundle_->railFraction.size() ? bundle_->railFraction[q] : 1.0;
}

}  // namespace cd::network
