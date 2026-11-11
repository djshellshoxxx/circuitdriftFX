// CircuitDrift network: deterministic co-render + influence bundle replay (see CoRender.h).
// Owner: cd_network. Threading: coRender/serialize/parse non-audio; ReplaySource reads audio-safe.
#include "network/CoRender.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace cd::network {

namespace {
constexpr std::int64_t kMaxBundleSamples = std::int64_t(1) << 26;   // ~23 min at 48 kHz per member

class Writer {
public:
    void bytes(const void* p, std::size_t n) { const auto* s = static_cast<const std::uint8_t*>(p); v.insert(v.end(), s, s + n); }
    void u8(std::uint8_t x) { v.push_back(x); }
    void u32(std::uint32_t x) { for (int k = 0; k < 4; ++k) v.push_back(std::uint8_t(x >> (8 * k))); }
    void u64(std::uint64_t x) { for (int k = 0; k < 8; ++k) v.push_back(std::uint8_t(x >> (8 * k))); }
    void f64(double d) { std::uint64_t u; std::memcpy(&u, &d, 8); u64(u); }
    std::vector<std::uint8_t> v;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}
    bool bytes(void* p, std::size_t n) { if (n > b_.size() - pos_) return fail(); std::memcpy(p, b_.data() + pos_, n); pos_ += n; return true; }
    bool u8(std::uint8_t& x) { return bytes(&x, 1); }
    bool u32(std::uint32_t& x) {
        std::uint8_t t[4];
        if (!bytes(t, 4)) return false;
        x = 0;
        for (int k = 0; k < 4; ++k) x |= std::uint32_t(t[k]) << (8 * k);
        return true;
    }
    bool u64(std::uint64_t& x) {
        std::uint8_t t[8];
        if (!bytes(t, 8)) return false;
        x = 0;
        for (int k = 0; k < 8; ++k) x |= std::uint64_t(t[k]) << (8 * k);
        return true;
    }
    bool f64(double& d) { std::uint64_t u; if (!u64(u)) return false; std::memcpy(&d, &u, 8); return true; }
    std::size_t remaining() const { return b_.size() - pos_; }
    std::size_t pos() const { return pos_; }
private:
    bool fail() { pos_ = b_.size(); return false; }
    std::span<const std::uint8_t> b_;
    std::size_t pos_ = 0;
};

void hashStem(Hash64& h, const Stem& s) noexcept {
    h.u64(s.size());
    for (const auto& c : s) {
        h.u64(c.size());
        h.update(c.data(), c.size() * sizeof(double));   // exact bit patterns
    }
}

double quantumRms(const Stem& stem, std::int64_t from, std::uint32_t n) noexcept {
    double e = 0;
    std::size_t count = 0;
    for (const auto& c : stem)
        for (std::uint32_t f = 0; f < n; ++f) {
            const std::size_t i = std::size_t(from) + f;
            const double x = i < c.size() ? c[i] : 0.0;
            e += x * x;
            ++count;
        }
    return count ? std::sqrt(e / double(count)) : 0.0;
}
}  // namespace

std::uint64_t stemContentHash(const Stem& stem) noexcept {
    Hash64 h;
    h.update("cd-stem", 7);
    hashStem(h, stem);
    return h.finish();
}

std::uint64_t automationHash() noexcept { return hash64("cd-automation:none", 18); }

std::uint64_t engineIdentityHash(const runtime::Engine& e) noexcept {
    const auto id = e.snapshotIdentity();
    Hash64 h;
    h.update("cd-engine-identity", 18);
    h.u64(id.layout.layoutId);
    h.u64(id.layout.version);
    h.u64(id.layout.byteSize);
    h.f64(id.sampleRate);
    h.u64(id.oversampling);
    h.u64(id.modelId);
    h.u64(std::uint64_t(e.latencySamples()));
    return h.finish();
}

std::uint64_t InfluenceBundle::computeHash() const noexcept {
    Hash64 h;
    h.update("CDIB", 4);
    h.u64(kVersion);
    h.update(kModelVersion, std::strlen(kModelVersion));
    h.f64(sampleRate);
    h.u64(std::uint64_t(start));
    h.u64(std::uint64_t(length));
    h.f64(gain);
    h.f64(nominalV);
    h.u64(bleedSeed);
    h.u64(psuHash);
    h.u64(topologyHash);
    h.u64(sharedDemand ? 1 : 0);
    h.u64(rowClamped ? 1 : 0);
    h.u64(members.size());
    for (const auto& m : members) {
        h.update(m.id.b.data(), 16);
        h.u64(m.channels);
        h.u64(m.contentHash);
        h.u64(m.automationHash);
        h.u64(m.engineIdentityHash);
        h.f64(m.powerW);
        hashStem(h, m.bleed);
    }
    h.u64(rail.size());
    h.update(rail.data(), rail.size() * sizeof(double));
    h.update(railIterations.data(), railIterations.size());
    return h.finish();
}

std::vector<std::uint8_t> InfluenceBundle::serialize() const {
    Writer w;
    w.bytes("CDIB", 4);
    w.u32(kVersion);
    w.f64(sampleRate);
    w.u64(std::uint64_t(start));
    w.u64(std::uint64_t(length));
    w.f64(gain);
    w.f64(nominalV);
    w.u64(bleedSeed);
    w.u64(psuHash);
    w.u64(topologyHash);
    w.u8(sharedDemand ? 1 : 0);
    w.u8(rowClamped ? 1 : 0);
    w.u32(std::uint32_t(members.size()));
    for (const auto& m : members) {
        w.bytes(m.id.b.data(), 16);
        w.u32(m.channels);
        w.u64(m.contentHash);
        w.u64(m.automationHash);
        w.u64(m.engineIdentityHash);
        w.f64(m.powerW);
        for (const auto& c : m.bleed)
            for (double x : c) w.f64(x);
    }
    for (double v : rail) w.f64(v);
    for (auto it : railIterations) w.u8(it);
    w.u64(integrityHash);
    return std::move(w.v);
}

bool InfluenceBundle::parse(std::span<const std::uint8_t> bytes, InfluenceBundle& out, std::string& error) {
    Reader r(bytes);
    char magic[4];
    std::uint32_t version = 0, nMembers = 0;
    InfluenceBundle b;
    std::uint64_t start = 0, length = 0;
    std::uint8_t shared = 0, clamped = 0;
    if (!r.bytes(magic, 4) || std::memcmp(magic, "CDIB", 4) != 0) { error = "bundle: bad magic"; return false; }
    if (!r.u32(version) || version != kVersion) { error = "bundle: unsupported version"; return false; }
    if (!r.f64(b.sampleRate) || !r.u64(start) || !r.u64(length) || !r.f64(b.gain) || !r.f64(b.nominalV) ||
        !r.u64(b.bleedSeed) || !r.u64(b.psuHash) || !r.u64(b.topologyHash) || !r.u8(shared) || !r.u8(clamped) ||
        !r.u32(nMembers)) {
        error = "bundle: truncated manifest"; return false;
    }
    if (!std::isfinite(b.sampleRate) || b.sampleRate < 8000 || b.sampleRate > 768000) { error = "bundle: sample rate"; return false; }
    if (start > std::uint64_t(kMaxBundleSamples) * 1024 || length == 0 || length > std::uint64_t(kMaxBundleSamples)) {
        error = "bundle: start/length out of range"; return false;
    }
    if (!std::isfinite(b.gain) || b.gain < 0 || b.gain > 1 || !std::isfinite(b.nominalV) || b.nominalV <= 0) {
        error = "bundle: gain/rail"; return false;
    }
    if (nMembers < 1 || nMembers > kMaxMembers || shared > 1 || clamped > 1) { error = "bundle: member count/flags"; return false; }
    b.start = std::int64_t(start);
    b.length = std::int64_t(length);
    b.sharedDemand = shared != 0;
    b.rowClamped = clamped != 0;
    const std::uint64_t quanta = (length + kQuantum - 1) / kQuantum;
    b.members.resize(nMembers);
    for (auto& m : b.members) {
        if (!r.bytes(m.id.b.data(), 16) || !r.u32(m.channels) || !r.u64(m.contentHash) || !r.u64(m.automationHash) ||
            !r.u64(m.engineIdentityHash) || !r.f64(m.powerW)) {
            error = "bundle: truncated member"; return false;
        }
        if (m.channels < 1 || m.channels > kMaxChannels) { error = "bundle: member channels"; return false; }
        if (r.remaining() / 8 / m.channels < length) { error = "bundle: truncated bleed stream"; return false; }
        m.bleed.assign(m.channels, std::vector<double>(std::size_t(length)));
        for (auto& c : m.bleed)
            for (auto& x : c)
                if (!r.f64(x) || !std::isfinite(x)) { error = "bundle: invalid bleed sample"; return false; }
    }
    if (r.remaining() != quanta * 8 + quanta + 8) { error = "bundle: size mismatch"; return false; }
    b.rail.resize(std::size_t(quanta));
    for (auto& v : b.rail)
        if (!r.f64(v) || !std::isfinite(v)) { error = "bundle: invalid rail"; return false; }
    b.railIterations.resize(std::size_t(quanta));
    for (auto& it : b.railIterations)
        if (!r.u8(it) || it < 1 || it > 8) { error = "bundle: invalid iteration count"; return false; }
    if (!r.u64(b.integrityHash) || r.remaining() != 0) { error = "bundle: trailing data"; return false; }
    if (b.computeHash() != b.integrityHash) { error = "bundle: integrity hash mismatch"; return false; }
    out = std::move(b);
    return true;
}

bool coRender(const std::vector<CoRenderMember>& membersIn, const CoRenderConfig& cfg, CoRenderResult& out,
              std::string& error) {
    if (membersIn.empty() || membersIn.size() > kMaxMembers) { error = "1..32 members required"; return false; }
    if (cfg.length <= 0 || cfg.length > kMaxBundleSamples || cfg.start < 0) { error = "invalid start/length"; return false; }
    if (cfg.maxIterations < 1 || cfg.maxIterations > 8) { error = "fixed-point iterations must be 1..8"; return false; }
    SharedReservoir psu;
    if (!psu.prepare(cfg.psu, cfg.sampleRate)) { error = "invalid PSU manifest/sample rate"; return false; }

    // Fixed member-ID order.
    std::vector<std::size_t> order(membersIn.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return uuidLess(membersIn[a].id, membersIn[b].id); });
    for (std::size_t k = 1; k < order.size(); ++k)
        if (membersIn[order[k]].id == membersIn[order[k - 1]].id) { error = "duplicate member id"; return false; }
    const std::uint32_t n = std::uint32_t(order.size());

    InfluenceBundle& b = out.bundle;
    b = InfluenceBundle{};
    b.sampleRate = cfg.sampleRate;
    b.start = cfg.start;
    b.length = cfg.length;
    b.gain = bleedGain(cfg.bleed);
    b.nominalV = cfg.psu.nominalV;
    b.bleedSeed = cfg.bleedSeed;
    b.psuHash = cfg.psu.hash();
    b.sharedDemand = cfg.sharedDemand;

    TopologyMatrix matrix{};
    Hash64 th;
    th.u64(n);
    if (cfg.useTopology) {
        for (std::size_t i = 0; i < cfg.topology.size() && i < n; ++i)
            for (std::size_t j = 0; j < cfg.topology[i].size() && j < n; ++j) matrix[i][j] = cfg.topology[i][j];
        b.rowClamped = enforceRowSums(matrix, n, b.gain);
        for (std::uint32_t i = 0; i < n; ++i) for (std::uint32_t j = 0; j < n; ++j) th.f64(matrix[i][j]);
    } else {
        th.update("default", 7);
    }
    b.topologyHash = th.finish();

    std::vector<std::unique_ptr<runtime::Engine>> engines;
    out.reference.assign(n, Stem{});
    b.members.resize(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        const CoRenderMember& m = membersIn[order[k]];
        const std::size_t ch = m.stem.size();
        if (ch < 1 || ch > kMaxChannels || int(ch) != m.engine.channels) { error = "stem/engine channel mismatch"; return false; }
        for (const auto& c : m.stem)
            if (std::int64_t(c.size()) != cfg.length) { error = "stem length must equal render length"; return false; }
        if (!(m.engine.sampleRate == cfg.sampleRate)) { error = "member sample rate mismatch"; return false; }
        if (!std::isfinite(m.powerW) || m.powerW < 0 || m.powerW > 1e6) { error = "member power"; return false; }
        auto e = std::make_unique<runtime::Engine>();
        if (!e->prepare(m.engine, error)) { error = "engine prepare: " + error; return false; }
        auto& bm = b.members[k];
        bm.id = m.id;
        bm.channels = std::uint32_t(ch);
        bm.contentHash = stemContentHash(m.stem);
        bm.automationHash = automationHash();
        bm.engineIdentityHash = engineIdentityHash(*e);
        bm.powerW = m.powerW;
        bm.bleed.assign(ch, std::vector<double>(std::size_t(cfg.length), 0.0));
        out.reference[k].assign(ch, std::vector<double>(std::size_t(cfg.length), 0.0));
        engines.push_back(std::move(e));
    }

    const std::int64_t quanta = (cfg.length + kQuantum - 1) / kQuantum;
    b.rail.assign(std::size_t(quanta), 0.0);
    b.railIterations.assign(std::size_t(quanta), 0);
    std::vector<double> inBuf(std::size_t(kMaxChannels) * kQuantum), tapBuf(std::size_t(n) * kMaxChannels * kQuantum, 0.0);
    double qb[kMaxChannels][kQuantum];
    out.maxIterationsUsed = 0;
    for (std::int64_t q = 0; q < quanta; ++q) {
        const std::int64_t from = q * kQuantum;
        const std::uint32_t frames = std::uint32_t(std::min<std::int64_t>(kQuantum, cfg.length - from));
        // 1) demands (fixed member order) + one PSU solve
        double totalP = 0;
        BleedSource src[kMaxMembers];
        for (std::uint32_t k = 0; k < n; ++k) {
            const CoRenderMember& m = membersIn[order[k]];
            totalP += m.powerW * (0.2 + 0.8 * std::min(1.0, quantumRms(m.stem, from, frames)));
            for (std::uint32_t c = 0; c < b.members[k].channels; ++c) {
                double* t = tapBuf.data() + (std::size_t(k) * kMaxChannels + c) * kQuantum;
                std::fill(t, t + kQuantum, 0.0);
                for (std::uint32_t f = 0; f < frames; ++f) {
                    double x = m.stem[c][std::size_t(from) + f];
                    t[f] = std::isfinite(x) ? std::clamp(x, -kTapLimit, kTapLimit) : 0.0;
                }
                src[k].ch[c] = t;
            }
            src[k].channels = b.members[k].channels;
            src[k].sign = memberSign(cfg.bleedSeed, b.members[k].id);
            src[k].present = true;
        }
        if (cfg.sharedDemand) {
            const RailSolve rs = solveRailFixedPoint(psu, totalP, cfg.maxIterations);
            if (!rs.converged) {
                error = "unconverged rail fixed-point solve at quantum " + std::to_string(q) + " (rejected)";
                return false;
            }
            psu.setVoltage(rs.v);
            b.railIterations[std::size_t(q)] = std::uint8_t(rs.iterations);
            out.maxIterationsUsed = std::max(out.maxIterationsUsed, rs.iterations);
        } else {
            b.railIterations[std::size_t(q)] = 1;
        }
        b.rail[std::size_t(q)] = psu.voltage();
        // 2) bleed per receiver, 3) render each member on stem + bleed (fixed order)
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t ch = b.members[i].channels;
            double* o[kMaxChannels] = {qb[0], qb[1]};
            if (b.gain > 0 && n > 1) mixBleed(src, n, n, i, ch, b.gain, cfg.useTopology ? &matrix : nullptr, o, kQuantum);
            else for (auto& row : qb) std::fill(row, row + kQuantum, 0.0);
            const CoRenderMember& m = membersIn[order[i]];
            const double* inPtr[kMaxChannels];
            double* outPtr[kMaxChannels];
            for (std::uint32_t c = 0; c < ch; ++c) {
                double* x = inBuf.data() + std::size_t(c) * kQuantum;
                for (std::uint32_t f = 0; f < frames; ++f) {
                    b.members[i].bleed[c][std::size_t(from) + f] = qb[c][f];
                    x[f] = m.stem[c][std::size_t(from) + f] + qb[c][f];
                }
                inPtr[c] = x;
                outPtr[c] = out.reference[i][c].data() + from;
            }
            runtime::ProcessContext ctx;
            ctx.sampleRate = cfg.sampleRate;
            ctx.sampleStart = cfg.start + from;
            ctx.frames = frames;
            ctx.offline = true;
            runtime::ProcessBlock blk;
            blk.ctx = ctx;
            blk.buses = runtime::AudioBuses{int(ch), inPtr, outPtr};
            engines[i]->process(blk);
        }
    }
    b.seal();
    return true;
}

bool ReplaySource::bind(const InfluenceBundle& bundle, const Uuid& member, std::uint64_t contentHash,
                        std::uint64_t automation, std::uint64_t engineIdentity, double sampleRate, std::string& error) {
    b_ = nullptr;
    m_ = nullptr;
    if (bundle.computeHash() != bundle.integrityHash) { error = "replay invalid: bundle integrity hash"; return false; }
    if (!(bundle.sampleRate == sampleRate)) { error = "replay invalid: sample rate mismatch"; return false; }
    const InfluenceBundle::Member* found = nullptr;
    for (const auto& m : bundle.members) if (m.id == member) found = &m;
    if (!found) { error = "replay invalid: member not in bundle routing"; return false; }
    if (found->contentHash != contentHash) { error = "replay invalid: input content hash mismatch"; return false; }
    if (found->automationHash != automation) { error = "replay invalid: automation hash mismatch"; return false; }
    if (found->engineIdentityHash != engineIdentity) { error = "replay invalid: engine/model identity mismatch"; return false; }
    b_ = &bundle;
    m_ = found;
    nominal_ = bundle.nominalV;
    return true;
}

std::uint32_t ReplaySource::bleedFor(std::int64_t sampleStart, std::uint32_t frames, double* const* out) const noexcept {
    std::uint32_t flags = 0;
    if (!m_) {
        return kOutOfRange;
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        const std::int64_t i = sampleStart + std::int64_t(f) - b_->start;
        const bool in = i >= 0 && i < b_->length;
        if (!in) flags |= kOutOfRange;
        for (std::uint32_t c = 0; c < m_->channels; ++c) out[c][f] = in ? m_->bleed[c][std::size_t(i)] : 0.0;
    }
    return flags;
}

double ReplaySource::railAt(std::int64_t sample) const noexcept {
    if (!b_) return 0;
    const std::int64_t i = sample - b_->start;
    if (i < 0 || i >= b_->length) return nominal_;
    return b_->rail[std::size_t(i / std::int64_t(kQuantum))];
}

}  // namespace cd::network
