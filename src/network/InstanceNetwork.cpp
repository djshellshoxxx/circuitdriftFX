#include "network/InstanceNetwork.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <new>

namespace cd::network {
namespace {
inline float bitsToFloat(std::uint32_t b) noexcept { return std::bit_cast<float>(b); }
inline std::uint32_t floatToBits(float f) noexcept { return std::bit_cast<std::uint32_t>(f); }
inline float sane(float v, float lo, float hi) noexcept { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; }
inline std::uint64_t splitmix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline unsigned popcount(std::uint32_t v) noexcept { return unsigned(std::popcount(v)); }
}  // namespace

const char* joinResultText(JoinResult r) noexcept {
    switch (r) {
        case JoinResult::Ok: return "ok";
        case JoinResult::InvalidArgument: return "invalid argument";
        case JoinResult::GroupLimit: return "group limit reached (8 groups per broker)";
        case JoinResult::MemberLimit: return "member limit reached (32 members per group)";
        case JoinResult::IncompatibleSampleRate: return "sample rate differs from the group";
        case JoinResult::IncompatibleRail: return "PSU rail model or nominal voltage differs from the group";
        case JoinResult::Duplicate: return "member already joined";
    }
    return "?";
}

float bleedSign(std::uint64_t seed, const Uuid128& id) noexcept {
    return (splitmix(seed ^ splitmix(id.hi) ^ (splitmix(id.lo) << 1)) & 1u) ? 1.0f : -1.0f;
}

double bleedGainLinear(bool enabled, double dB) noexcept {
    if (!enabled || !std::isfinite(dB)) return 0.0;
    return std::pow(10.0, std::clamp(dB, kBleedMinDb, kBleedMaxDb) / 20.0);
}

double bleedRowWeights(unsigned self, const Uuid128* ids, std::uint32_t mask, std::uint64_t seed, double g,
                       const float* topology, float* w) noexcept {
    for (unsigned j = 0; j < kMaxMembers; ++j) w[j] = 0.0f;
    if (!(g > 0.0) || self >= kMaxMembers) return 0.0;
    const unsigned n = popcount(mask);
    const double base = g / double(std::max(1u, n > 0 ? n - 1 : 0));
    double sum = 0.0;
    for (unsigned j = 0; j < kMaxMembers; ++j) {
        if (j == self || !((mask >> j) & 1u)) continue;
        double v = topology ? double(topology[j]) : base * double(bleedSign(seed, ids[j]));
        if (!std::isfinite(v)) v = 0.0;
        w[j] = float(v);
        sum += std::abs(double(w[j]));  // sum of the stored float weights, so the bound is exact
    }
    if (sum > g) {  // topology rows (and float rounding) may never push the row above the configured gain
        const double k = (g / sum) * (1.0 - 1.0e-6);  // margin >> float rounding error (6e-8)
        sum = 0.0;
        for (unsigned j = 0; j < kMaxMembers; ++j) { w[j] = float(double(w[j]) * k); sum += std::abs(double(w[j])); }
    }
    return sum;
}

bool validateLayout(const void* mem, std::size_t size, std::string& error) {
    if (!mem) { error = "null region"; return false; }
    if (reinterpret_cast<std::uintptr_t>(mem) % alignof(SharedLayout) != 0) { error = "misaligned region"; return false; }
    if (size < sizeof(SharedHeader)) { error = "region smaller than header"; return false; }
    const auto& h = *static_cast<const SharedHeader*>(mem);
    if (h.magic != kMagic) { error = "bad magic"; return false; }
    if (h.major != kProtocolMajor) { error = "incompatible protocol major version"; return false; }
    if (h.byteSize != sizeof(SharedLayout)) { error = "header byte size mismatch"; return false; }
    if (size < sizeof(SharedLayout)) { error = "region smaller than declared layout"; return false; }
    if (h.slotCount != kMaxMembers || h.ringQuanta != kRingQuanta || h.quantumFrames != kQuantumFrames) { error = "unexpected layout constants"; return false; }
    if (h.channels < 1 || h.channels > kAudioChannels) { error = "invalid channel count"; return false; }
    if (!std::isfinite(h.sampleRate) || h.sampleRate < 8000.0 || h.sampleRate > 384000.0) { error = "invalid sample rate"; return false; }
    if (!std::isfinite(h.nominalRailV) || std::abs(h.nominalRailV) > 10000.0f) { error = "invalid nominal rail voltage"; return false; }
    error.clear();
    return true;
}

void fillHeader(SharedLayout& l, const GroupSpec& s) noexcept {
    SharedHeader& h = l.header;
    h.magic = kMagic; h.major = kProtocolMajor; h.minor = kProtocolMinor;
    h.byteSize = sizeof(SharedLayout);
    h.groupHi = s.groupId.hi; h.groupLo = s.groupId.lo; h.sessionHash = s.sessionKeyHash;
    h.sampleRate = s.sampleRate; h.channels = s.channels;
    h.slotCount = kMaxMembers; h.ringQuanta = kRingQuanta; h.quantumFrames = kQuantumFrames;
    h.railModelId = s.railModelId; h.nominalRailV = s.nominalRailV; h.bleedSeed = s.bleedSeed;
}

SharedLayout* initializeLayout(void* mem, std::size_t size, const GroupSpec& spec, std::string& error) {
    if (!mem || size < sizeof(SharedLayout)) { error = "region too small"; return nullptr; }
    if (reinterpret_cast<std::uintptr_t>(mem) % alignof(SharedLayout) != 0) { error = "misaligned region"; return nullptr; }
    if (spec.channels < 1 || spec.channels > kAudioChannels || !std::isfinite(spec.sampleRate) || spec.sampleRate < 8000.0 ||
        spec.sampleRate > 384000.0 || !std::isfinite(spec.nominalRailV)) {
        error = "invalid group specification"; return nullptr;
    }
    auto* l = new (mem) SharedLayout();
    fillHeader(*l, spec);
    error.clear();
    return l;
}

// ---------------------------------------------------------------- Member (audio thread)

bool Member::removed() const noexcept {
    if (removedLocal_ || !L_) return true;
    const MemberSlot& s = L_->members[slot_];
    return s.state.load(std::memory_order_acquire) != 2 || s.generation.load(std::memory_order_relaxed) != gen_;
}

void Member::setBleed(bool enabled, double dB) noexcept {
    bleedGainBits_.store(floatToBits(float(bleedGainLinear(enabled, dB))), std::memory_order_relaxed);
}
bool Member::setTopologyRow(const float* w) noexcept {
    if (!w) return false;
    for (unsigned j = 0; j < kMaxMembers; ++j) if (!std::isfinite(w[j])) return false;
    for (unsigned j = 0; j < kMaxMembers; ++j) topology_[j].store(floatToBits(w[j]), std::memory_order_relaxed);
    topologyOn_.store(true, std::memory_order_release);
    return true;
}
void Member::clearTopology() noexcept { topologyOn_.store(false, std::memory_order_release); }
void Member::setNetworkAvailable(bool a) noexcept { available_.store(a, std::memory_order_relaxed); }

bool Member::publish(std::uint64_t tick, const MemberFrameIn& in) noexcept {
    if (removedLocal_ || !L_) return false;
    MemberSlot& s = L_->members[slot_];
    if (s.state.load(std::memory_order_acquire) != 2 || s.generation.load(std::memory_order_relaxed) != gen_) {
        removedLocal_ = true;
        return false;
    }
    FrameSlot& f = s.ring[tick % kRingQuanta];
    f.stamp.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const float demand = sane(in.demandA, 0.0f, kMaxDemandA), power = sane(in.powerW, 0.0f, 1.0e7f), act = sane(in.activity, 0.0f, 1.0f);
    f.epoch.store(epoch_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    f.generation.store(gen_, std::memory_order_relaxed);
    f.demandBits.store(floatToBits(demand), std::memory_order_relaxed);
    f.powerBits.store(floatToBits(power), std::memory_order_relaxed);
    f.activityBits.store(floatToBits(act), std::memory_order_relaxed);
    if (in.left) {
        const bool mono = in.right == nullptr;
        for (unsigned n = 0; n < kQuantumFrames; ++n) {
            float l = sane(in.left[n], -kMaxAudioAbs, kMaxAudioAbs);
            float r = mono ? l : sane(in.right[n], -kMaxAudioAbs, kMaxAudioAbs);
            if (mono) { l *= duplicateGain_; r *= duplicateGain_; }  // mono duplicates at -3 dB into stereo
            f.audio[n].store(floatToBits(l), std::memory_order_relaxed);
            f.audio[kQuantumFrames + n].store(floatToBits(r), std::memory_order_relaxed);
        }
        f.hasAudio.store(1, std::memory_order_relaxed);
    } else {
        f.hasAudio.store(0, std::memory_order_relaxed);
    }
    f.stamp.store(tick + 1, std::memory_order_release);
    s.heartbeat.store(s.heartbeat.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // single writer
    ownDemand_ = demand; ownPower_ = power;
    return true;
}

void Member::consume(std::uint64_t tick, float* bleedL, float* bleedR, ConsumeOut& out) noexcept {
    out = ConsumeOut{};
    if (bleedL) std::fill(bleedL, bleedL + kQuantumFrames, 0.0f);
    if (bleedR) std::fill(bleedR, bleedR + kQuantumFrames, 0.0f);
    if (removedLocal_ || removed()) {
        removedLocal_ = true;
        coupling_ = std::max(0.0f, coupling_ - rampStep_);
        out.status = MemberStatus::Removed; out.coupling = 0.0f;
        out.sharedDemandA = ownDemand_; out.sharedPowerW = ownPower_;
        return;
    }
    const SharedLayout& L = *L_;
    const std::uint32_t epoch = epoch_.load(std::memory_order_relaxed);
    const unsigned latency = latency_.load(std::memory_order_relaxed);
    const bool avail = available_.load(std::memory_order_relaxed);
    coupling_ = avail ? std::min(1.0f, coupling_ + rampStep_) : std::max(0.0f, coupling_ - rampStep_);
    const std::uint32_t mask = L.header.activeMask.load(std::memory_order_acquire);
    out.members = popcount(mask);
    const float g = bitsToFloat(bleedGainBits_.load(std::memory_order_relaxed));

    Uuid128 ids[kMaxMembers];
    for (unsigned j = 0; j < kMaxMembers; ++j)
        if ((mask >> j) & 1u) { ids[j].hi = L.members[j].idHi.load(std::memory_order_relaxed); ids[j].lo = L.members[j].idLo.load(std::memory_order_relaxed); }
    float row[kMaxMembers], w[kMaxMembers];
    const bool topo = topologyOn_.load(std::memory_order_acquire);
    if (topo) for (unsigned j = 0; j < kMaxMembers; ++j) row[j] = bitsToFloat(topology_[j].load(std::memory_order_relaxed));
    bleedRowWeights(slot_, ids, mask, L.header.bleedSeed, double(g), topo ? row : nullptr, w);

    float demandSum = 0, powerSum = 0, rowApplied = 0;
    unsigned missing = 0;
    const bool haveHistory = tick >= latency;
    const std::uint64_t t = haveHistory ? tick - latency : 0;
    for (unsigned j = 0; j < kMaxMembers; ++j) {
        if (j == slot_) continue;
        const bool active = (mask >> j) & 1u;
        const float target = active ? 1.0f : 0.0f;
        rampW_[j] = rampW_[j] < target ? std::min(target, rampW_[j] + rampStep_) : std::max(target, rampW_[j] - rampStep_);
        if (!active && rampW_[j] <= 0.0f) { heldDemand_[j] = 0; heldPower_[j] = 0; continue; }
        bool ok = false;
        const bool wantAudio = g > 0.0f && coupling_ > 0.0f && bleedL && bleedR;
        if (active && haveHistory) {
            const MemberSlot& peer = L.members[j];
            const FrameSlot& f = peer.ring[t % kRingQuanta];
            const std::uint64_t s1 = f.stamp.load(std::memory_order_acquire);
            if (s1 == t + 1) {
                const std::uint32_t fe = f.epoch.load(std::memory_order_relaxed), fg = f.generation.load(std::memory_order_relaxed);
                const float d = bitsToFloat(f.demandBits.load(std::memory_order_relaxed));
                const float p = bitsToFloat(f.powerBits.load(std::memory_order_relaxed));
                const bool hasAudio = f.hasAudio.load(std::memory_order_relaxed) != 0;
                if (wantAudio && hasAudio)
                    for (unsigned n = 0; n < kAudioChannels * kQuantumFrames; ++n)
                        scratch_[n / kQuantumFrames][n % kQuantumFrames] = sane(bitsToFloat(f.audio[n].load(std::memory_order_relaxed)), -kMaxAudioAbs, kMaxAudioAbs);
                std::atomic_thread_fence(std::memory_order_acquire);
                const std::uint64_t s2 = f.stamp.load(std::memory_order_relaxed);  // single check, no retry
                if (s2 == s1 && fe == epoch && fg == peer.generation.load(std::memory_order_relaxed)) {
                    ok = true;
                    heldDemand_[j] = sane(d, 0.0f, kMaxDemandA); heldPower_[j] = sane(p, 0.0f, 1.0e7f);
                    if (wantAudio && hasAudio) {
                        const float c = w[j] * rampW_[j] * coupling_;
                        for (unsigned n = 0; n < kQuantumFrames; ++n) { bleedL[n] += c * scratch_[0][n]; bleedR[n] += c * scratch_[1][n]; }
                        rowApplied += std::abs(c);
                    }
                }
            }
        }
        if (!ok) {
            heldDemand_[j] *= decay_; heldPower_[j] *= decay_;  // hold with tau = 100 ms; audio contributes zero
            if (active && haveHistory) ++missing;
        }
        demandSum += rampW_[j] * heldDemand_[j];
        powerSum += rampW_[j] * heldPower_[j];
    }
    out.missingPeers = missing;
    out.coupling = coupling_;
    out.bleedRowAbsSum = rowApplied;
    out.sharedDemandA = ownDemand_ + coupling_ * demandSum;
    out.sharedPowerW = ownPower_ + coupling_ * powerSum;
    out.status = !avail ? MemberStatus::Unavailable : (missing > 0 ? MemberStatus::Degraded : MemberStatus::Coupled);
}

// ---------------------------------------------------------------- Group (worker)

Group::Group(SharedLayout* layout) : L_(layout) {}

unsigned Group::memberCount() const noexcept { return popcount(L_->header.activeMask.load(std::memory_order_acquire)); }
std::uint64_t Group::rosterGeneration() const noexcept { return L_->header.rosterGeneration.load(std::memory_order_acquire); }
void Group::bumpEpoch() noexcept { L_->header.epoch.fetch_add(1, std::memory_order_acq_rel); }

JoinResult Group::join(const Uuid128& id, double fs, std::uint32_t rail, float nominalV, std::unique_ptr<Member>& out, std::string* reason) {
    auto fail = [&](JoinResult r) { if (reason) *reason = joinResultText(r); return r; };
    if ((id.hi == 0 && id.lo == 0) || !std::isfinite(fs) || fs < 8000.0 || fs > 384000.0 || !std::isfinite(nominalV)) return fail(JoinResult::InvalidArgument);
    SharedHeader& h = L_->header;
    if (std::abs(fs - h.sampleRate) > 0.5) return fail(JoinResult::IncompatibleSampleRate);
    if (rail != h.railModelId || std::abs(nominalV - h.nominalRailV) > 1.0e-3f * std::max(1.0f, std::abs(h.nominalRailV))) return fail(JoinResult::IncompatibleRail);
    const std::uint32_t mask = h.activeMask.load(std::memory_order_acquire);
    for (unsigned j = 0; j < kMaxMembers; ++j)
        if (((mask >> j) & 1u) && L_->members[j].idHi.load(std::memory_order_relaxed) == id.hi && L_->members[j].idLo.load(std::memory_order_relaxed) == id.lo)
            return fail(JoinResult::Duplicate);
    unsigned slot = kMaxMembers;
    for (unsigned j = 0; j < kMaxMembers; ++j) {
        std::uint32_t expect = 0;
        if (L_->members[j].state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) { slot = j; break; }
    }
    if (slot == kMaxMembers) return fail(JoinResult::MemberLimit);
    MemberSlot& s = L_->members[slot];
    const std::uint32_t gen = s.generation.load(std::memory_order_relaxed) + 1;
    s.generation.store(gen, std::memory_order_relaxed);
    s.idHi.store(id.hi, std::memory_order_relaxed); s.idLo.store(id.lo, std::memory_order_relaxed);
    s.heartbeat.store(0, std::memory_order_relaxed);
    s.railModelId.store(rail, std::memory_order_relaxed);
    for (auto& f : s.ring) f.stamp.store(0, std::memory_order_relaxed);
    s.state.store(2, std::memory_order_release);
    h.activeMask.fetch_or(1u << slot, std::memory_order_acq_rel);
    h.rosterGeneration.fetch_add(1, std::memory_order_acq_rel);
    lastGen_[slot] = 0; lastBeat_[slot] = 0; lastChangeNs_[slot] = 0;

    auto m = std::make_unique<Member>();
    m->L_ = L_; m->slot_ = slot; m->gen_ = gen; m->id_ = id; m->fs_ = h.sampleRate;
    m->decay_ = float(std::exp(-double(kQuantumFrames) / (h.sampleRate * kDecayTauMs / 1000.0)));
    m->rampStep_ = float(double(kQuantumFrames) / (h.sampleRate * kRampMs / 1000.0));
    m->epoch_.store(h.epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m->duplicateGain_ = float(std::pow(10.0, -3.0 / 20.0));
    out = std::move(m);
    if (reason) reason->clear();
    return JoinResult::Ok;
}

void Group::forceRemove(unsigned slot) {
    MemberSlot& s = L_->members[slot];
    L_->header.activeMask.fetch_and(~(1u << slot), std::memory_order_acq_rel);
    s.generation.fetch_add(1, std::memory_order_acq_rel);  // stale frames/handles are rejected
    s.state.store(0, std::memory_order_release);
    L_->header.rosterGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void Group::leave(Member& m) {
    if (m.L_ != L_) return;
    MemberSlot& s = L_->members[m.slot_];
    if (s.state.load(std::memory_order_acquire) == 2 && s.generation.load(std::memory_order_acquire) == m.gen_) forceRemove(m.slot_);
}

std::size_t Group::reapStale(std::int64_t nowNs, std::int64_t timeoutNs) {
    std::size_t removed = 0;
    const std::uint32_t mask = L_->header.activeMask.load(std::memory_order_acquire);
    for (unsigned j = 0; j < kMaxMembers; ++j) {
        if (!((mask >> j) & 1u)) continue;
        MemberSlot& s = L_->members[j];
        const std::uint64_t beat = s.heartbeat.load(std::memory_order_relaxed);
        const std::uint32_t gen = s.generation.load(std::memory_order_relaxed);
        if (gen != lastGen_[j] || beat != lastBeat_[j]) { lastGen_[j] = gen; lastBeat_[j] = beat; lastChangeNs_[j] = nowNs; continue; }
        if (nowNs - lastChangeNs_[j] >= timeoutNs) { forceRemove(j); ++removed; }
    }
    return removed;
}

// ---------------------------------------------------------------- Registry

Registry& Registry::instance() { static Registry r; return r; }

Registry::Entry* Registry::find(const GroupSpec& spec) const {
    for (const auto& e : groups_)
        if (e->spec.groupId == spec.groupId && e->spec.sessionKeyHash == spec.sessionKeyHash) return e.get();
    return nullptr;
}

JoinResult Registry::join(const GroupSpec& spec, const Uuid128& memberId, std::unique_ptr<Member>& out, std::string* reason) {
    std::lock_guard<std::mutex> lock(m_);
    auto fail = [&](JoinResult r) { if (reason) *reason = joinResultText(r); return r; };
    if ((spec.groupId.hi == 0 && spec.groupId.lo == 0) || spec.channels < 1 || spec.channels > kAudioChannels) return fail(JoinResult::InvalidArgument);
    Entry* e = find(spec);
    bool fresh = false;
    if (!e) {
        // Recycle an empty group if the broker is at its limit (members pin layouts, so this is safe).
        if (groups_.size() >= kMaxGroups) {
            auto it = std::find_if(groups_.begin(), groups_.end(), [](const auto& g) { return g->group->memberCount() == 0; });
            if (it == groups_.end()) return fail(JoinResult::GroupLimit);
            groups_.erase(it);
        }
        auto entry = std::make_unique<Entry>();
        entry->spec = spec;
        entry->layout = std::make_shared<SharedLayout>();
        fillHeader(*entry->layout, spec);
        entry->group = std::make_unique<Group>(entry->layout.get());
        groups_.push_back(std::move(entry));
        e = groups_.back().get();
        fresh = true;
    }
    const JoinResult r = e->group->join(memberId, spec.sampleRate, spec.railModelId, spec.nominalRailV, out, reason);
    if (r == JoinResult::Ok) out->keepAlive(e->layout);
    else if (fresh) groups_.pop_back();
    return r;
}

void Registry::leave(Member& m) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& e : groups_) if (e->layout.get() == m.L_) { e->group->leave(m); return; }
}

std::size_t Registry::reapStale(std::int64_t nowNs, std::int64_t timeoutNs) {
    std::lock_guard<std::mutex> lock(m_);
    std::size_t n = 0;
    for (auto& e : groups_) n += e->group->reapStale(nowNs, timeoutNs);
    return n;
}

std::size_t Registry::groupCount() const { std::lock_guard<std::mutex> lock(m_); return groups_.size(); }
std::size_t Registry::memberCount(const GroupSpec& spec) const {
    std::lock_guard<std::mutex> lock(m_);
    const Entry* e = find(spec);
    return e ? e->group->memberCount() : 0;
}
void Registry::clear() {
    std::lock_guard<std::mutex> lock(m_);
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const auto& g) { return g->group->memberCount() == 0; }), groups_.end());
}

}  // namespace cd::network
