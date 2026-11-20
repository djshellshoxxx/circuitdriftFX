#include "network/InfluenceBundle.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace cd::network {
namespace {
struct PayloadShape {
    std::size_t ticks = 0, bleedFloats = 0, totalFloats = 0;
};

// One validation contract for wire input, in-memory replay, serialization and co-render.
// Check the cap by division before multiplying any user-supplied length.
bool validateManifest(const BundleManifest& m, PayloadShape& shape, std::string& error) {
    const auto members = m.memberIds.size();
    if (m.version != kBundleVersion) { error = "unsupported bundle version"; return false; }
    if (members == 0 || members > kMaxMembers || m.inputHashes.size() != members) {
        error = "invalid member/hash count"; return false;
    }
    for (std::size_t i = 1; i < members; ++i) {
        if (!(m.memberIds[i - 1] < m.memberIds[i])) { error = "member IDs must be unique and sorted"; return false; }
    }
    if (!std::isfinite(m.sampleRate) || m.sampleRate < 8000 || m.sampleRate > 384000 ||
        m.lengthSamples == 0 || m.startSample > std::numeric_limits<std::uint64_t>::max() - m.lengthSamples ||
        !std::isfinite(m.bleedGainLinear) || m.bleedGainLinear < 0 || m.bleedGainLinear > bleedGainLinear(true, kBleedMaxDb) ||
        !std::isfinite(m.nominalRailV) || !std::isfinite(m.sourceOhms) || m.sourceOhms < 0 ||
        m.modelVersion.empty() || m.modelVersion.size() > 256 || m.modelVersion.find('\0') != std::string::npos) {
        error = "invalid manifest values"; return false;
    }
    const std::uint64_t ticks = m.lengthSamples / kQuantumFrames + (m.lengthSamples % kQuantumFrames != 0);
    const std::size_t perTick = 2 + members * kAudioChannels * kQuantumFrames;
    if (ticks > (kBundleMaxBytes / sizeof(float)) / perTick) {
        error = "influence bundle would exceed size cap"; return false;
    }
    shape.ticks = std::size_t(ticks);
    shape.totalFloats = shape.ticks * perTick;
    shape.bleedFloats = shape.totalFloats - 2 * shape.ticks;
    return true;
}

bool validatePayload(const InfluenceBundle& b, const PayloadShape& shape, std::string& error) {
    if (b.railVolts.size() != shape.ticks || b.railDemandA.size() != shape.ticks || b.bleed.size() != shape.bleedFloats) {
        error = "bundle payload inconsistent"; return false;
    }
    const auto finite = [](float x) { return std::isfinite(x); };
    if (!std::all_of(b.railVolts.begin(), b.railVolts.end(), finite) ||
        !std::all_of(b.railDemandA.begin(), b.railDemandA.end(), finite) ||
        !std::all_of(b.bleed.begin(), b.bleed.end(), finite)) {
        error = "bundle contains non-finite samples"; return false;
    }
    return true;
}

bool validateBundle(const InfluenceBundle& b, PayloadShape& shape, std::string& error) {
    return validateManifest(b.manifest, shape, error) && validatePayload(b, shape, error);
}

bool fitsFloat(double x) noexcept {
    return std::isfinite(x) && std::abs(x) <= std::numeric_limits<float>::max();
}
}  // namespace

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t h) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
std::uint64_t hashSamples(const float* x, std::size_t n) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t b = std::bit_cast<std::uint32_t>(x[i]);
        const std::uint8_t le[4] = {std::uint8_t(b), std::uint8_t(b >> 8), std::uint8_t(b >> 16), std::uint8_t(b >> 24)};
        h = fnv1a64(le, 4, h);
    }
    return h;
}

bool coRender(const CoRenderConfig& cfg, std::vector<MemberStem> stems, const DemandFn& demand, InfluenceBundle& out, std::string& error) {
    const std::size_t n = stems.size();
    if (n == 0 || n > kMaxMembers) { error = "member count must be 1..32"; return false; }
    if (!demand) { error = "no demand function"; return false; }
    if (!std::isfinite(cfg.sampleRate) || cfg.sampleRate < 8000 || cfg.sampleRate > 384000) { error = "invalid sample rate"; return false; }
    if (cfg.lengthSamples == 0 || cfg.maxIterations < 1 || cfg.maxIterations > 8) { error = "invalid length or iteration bound"; return false; }
    if (!std::isfinite(cfg.nominalRailV) || !(cfg.sourceOhms >= 0) || !std::isfinite(cfg.sourceOhms) ||
        !(cfg.tolerance > 0) || !std::isfinite(cfg.tolerance)) { error = "invalid PSU parameters"; return false; }
    std::sort(stems.begin(), stems.end(), [](const MemberStem& a, const MemberStem& b) { return a.id < b.id; });
    for (std::size_t i = 1; i < n; ++i) if (stems[i].id == stems[i - 1].id) { error = "duplicate member id"; return false; }
    for (auto& s : stems) if (s.left.size() < cfg.lengthSamples || s.right.size() < cfg.lengthSamples) { error = "stem shorter than render length"; return false; }
    if (!cfg.topology.empty()) {
        if (cfg.topology.size() != n) { error = "topology must be NxN"; return false; }
        for (const auto& r : cfg.topology) {
            if (r.size() != n) { error = "topology must be NxN"; return false; }
            if (!std::all_of(r.begin(), r.end(), [](float x) { return std::isfinite(x); })) {
                error = "topology contains non-finite weights"; return false;
            }
        }
    }
    InfluenceBundle b;
    BundleManifest& m = b.manifest;
    m.sampleRate = cfg.sampleRate; m.startSample = cfg.startSample; m.lengthSamples = cfg.lengthSamples;
    m.latencyQuanta = cfg.latencyQuanta; m.bleedSeed = cfg.bleedSeed;
    m.bleedGainLinear = bleedGainLinear(cfg.bleedEnabled, cfg.bleedDb);
    m.nominalRailV = cfg.nominalRailV; m.sourceOhms = cfg.sourceOhms; m.modelVersion = cfg.modelVersion;
    for (auto& s : stems) { m.memberIds.push_back(s.id); m.inputHashes.push_back(s.contentHash); }
    PayloadShape shape;
    if (!validateManifest(m, shape, error)) return false;
    const std::uint64_t ticks = shape.ticks;
    for (const auto& s : stems) {
        for (std::uint64_t i = 0; i < cfg.lengthSamples; ++i) {
            if (!std::isfinite(s.left[std::size_t(i)]) || !std::isfinite(s.right[std::size_t(i)])) {
                error = "stem contains non-finite samples"; return false;
            }
        }
    }
    b.railVolts.assign(shape.ticks, 0.f); b.railDemandA.assign(shape.ticks, 0.f);
    b.bleed.assign(shape.bleedFloats, 0.f);

    std::vector<Uuid128> ids(m.memberIds.begin(), m.memberIds.end());
    ids.resize(kMaxMembers);
    const std::uint32_t mask = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
    std::vector<std::vector<float>> rows(n, std::vector<float>(kMaxMembers, 0.f));
    for (std::size_t i = 0; i < n && !cfg.topology.empty(); ++i) for (std::size_t j = 0; j < n; ++j) rows[i][j] = cfg.topology[i][j];

    for (std::uint64_t k = 0; k < ticks; ++k) {
        // Joint rail solve in fixed member order: V = Vnom - R * sum_i I_i(V).
        double v = cfg.nominalRailV, sumI = 0;
        bool converged = false;
        for (int it = 0; it < cfg.maxIterations; ++it) {
            sumI = 0;
            for (std::size_t i = 0; i < n; ++i) sumI += demand(i, k, v);
            const double next = cfg.nominalRailV - cfg.sourceOhms * sumI;
            if (!std::isfinite(next)) { error = "rail solve produced a non-finite voltage"; return false; }
            const bool done = std::abs(next - v) <= cfg.tolerance;
            v = next;
            if (done) { converged = true; break; }
        }
        if (!converged) { error = "rail solve did not converge within the iteration bound"; return false; }
        if (!fitsFloat(v) || !fitsFloat(sumI)) { error = "rail solve exceeds sample representation"; return false; }
        b.railVolts[std::size_t(k)] = float(v); b.railDemandA[std::size_t(k)] = float(sumI);
        if (k < cfg.latencyQuanta || m.bleedGainLinear <= 0) continue;
        const std::uint64_t src = (k - cfg.latencyQuanta) * kQuantumFrames;
        for (std::size_t i = 0; i < n; ++i) {
            float w[kMaxMembers];
            bleedRowWeights(unsigned(i), ids.data(), mask, cfg.bleedSeed, m.bleedGainLinear, cfg.topology.empty() ? nullptr : rows[i].data(), w);
            for (std::size_t j = 0; j < n; ++j) {
                if (w[j] == 0.f) continue;
                for (unsigned s = 0; s < kQuantumFrames; ++s) {
                    const std::uint64_t at = src + s;
                    if (at >= cfg.lengthSamples) break;
                    b.bleed[b.bleedOffset(i, k, 0) + s] += w[j] * stems[j].left[std::size_t(at)];
                    b.bleed[b.bleedOffset(i, k, 1) + s] += w[j] * stems[j].right[std::size_t(at)];
                }
            }
        }
    }
    if (!validatePayload(b, shape, error)) return false;
    out = std::move(b);
    error.clear();
    return true;
}

// ---- serialization (little-endian, bounds-checked) ----
namespace {
struct Writer {
    std::vector<std::uint8_t> v;
    void u32(std::uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i))); }
    void u64(std::uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(std::uint8_t(x >> (8 * i))); }
    void f64(double d) { u64(std::bit_cast<std::uint64_t>(d)); }
    void f32(float f) { u32(std::bit_cast<std::uint32_t>(f)); }
};
struct Reader {
    const std::uint8_t* d; std::size_t n, p = 0; bool ok = true;
    bool need(std::size_t k) { if (!ok || p > n || k > n - p) { ok = false; return false; } return true; }
    std::uint32_t u32() { if (!need(4)) return 0; std::uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= std::uint32_t(d[p + std::size_t(i)]) << (8 * i); p += 4; return x; }
    std::uint64_t u64() { if (!need(8)) return 0; std::uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= std::uint64_t(d[p + std::size_t(i)]) << (8 * i); p += 8; return x; }
    double f64() { return std::bit_cast<double>(u64()); }
    float f32() { return std::bit_cast<float>(u32()); }
};
constexpr std::uint32_t kBundleMagic = 0x42494443u;  // "CDIB"
}  // namespace

std::vector<std::uint8_t> serializeBundle(const InfluenceBundle& b) {
    PayloadShape shape;
    std::string error;
    if (!validateBundle(b, shape, error)) return {};
    Writer w;
    const auto& m = b.manifest;
    w.v.reserve(84 + m.modelVersion.size() + 24 * m.memberIds.size() + shape.totalFloats * sizeof(float));
    w.u32(kBundleMagic); w.u32(m.version);
    w.f64(m.sampleRate); w.u64(m.startSample); w.u64(m.lengthSamples); w.u32(m.latencyQuanta); w.u64(m.bleedSeed);
    w.f64(m.bleedGainLinear); w.f64(m.nominalRailV); w.f64(m.sourceOhms);
    w.u32(std::uint32_t(m.modelVersion.size()));
    w.v.insert(w.v.end(), m.modelVersion.begin(), m.modelVersion.end());
    w.u32(std::uint32_t(m.memberIds.size()));
    for (std::size_t i = 0; i < m.memberIds.size(); ++i) { w.u64(m.memberIds[i].hi); w.u64(m.memberIds[i].lo); w.u64(m.inputHashes[i]); }
    for (float f : b.railVolts) w.f32(f);
    for (float f : b.railDemandA) w.f32(f);
    for (float f : b.bleed) w.f32(f);
    w.u64(fnv1a64(w.v.data(), w.v.size()));
    return std::move(w.v);
}

bool parseBundle(const std::uint8_t* data, std::size_t n, InfluenceBundle& out, std::string& error) {
    if (!data || n < 16 || n > kBundleMaxBytes + 4096) { error = "bundle size out of range"; return false; }
    if (fnv1a64(data, n - 8) != [&] { Reader t{data, n, n - 8}; return t.u64(); }()) { error = "bundle integrity hash mismatch"; return false; }
    Reader r{data, n - 8};
    InfluenceBundle b;
    auto& m = b.manifest;
    if (r.u32() != kBundleMagic) { error = "bad bundle magic"; return false; }
    m.version = r.u32();
    if (m.version != kBundleVersion) { error = "unsupported bundle version"; return false; }
    m.sampleRate = r.f64(); m.startSample = r.u64(); m.lengthSamples = r.u64(); m.latencyQuanta = r.u32(); m.bleedSeed = r.u64();
    m.bleedGainLinear = r.f64(); m.nominalRailV = r.f64(); m.sourceOhms = r.f64();
    const std::uint32_t sl = r.u32();
    if (!r.ok || sl > 256 || !r.need(sl)) { error = "truncated or oversize model string"; return false; }
    m.modelVersion.assign(reinterpret_cast<const char*>(data + r.p), sl); r.p += sl;
    const std::uint32_t cnt = r.u32();
    if (!r.ok || cnt == 0 || cnt > kMaxMembers) { error = "invalid member count"; return false; }
    for (std::uint32_t i = 0; i < cnt; ++i) { Uuid128 id; id.hi = r.u64(); id.lo = r.u64(); m.memberIds.push_back(id); m.inputHashes.push_back(r.u64()); }
    if (!r.ok) { error = "truncated manifest"; return false; }
    PayloadShape shape;
    if (!validateManifest(m, shape, error)) return false;
    if (shape.totalFloats * sizeof(float) != (n - 8 - r.p)) { error = "payload size does not match manifest"; return false; }
    b.railVolts.resize(shape.ticks); b.railDemandA.resize(shape.ticks);
    b.bleed.resize(shape.bleedFloats);
    for (auto& f : b.railVolts) f = r.f32();
    for (auto& f : b.railDemandA) f = r.f32();
    for (auto& f : b.bleed) f = r.f32();
    if (!r.ok) { error = "truncated payload"; return false; }
    if (!validatePayload(b, shape, error)) return false;
    out = std::move(b);
    error.clear();
    return true;
}

bool BundleReplayer::open(const InfluenceBundle& bundle, const BundleManifest& expected, std::string& error) {
    b_ = nullptr;
    if (!(bundle.manifest == expected)) {
        const auto& a = bundle.manifest;
        error = a.sampleRate != expected.sampleRate ? "sample rate mismatch"
              : a.startSample != expected.startSample || a.lengthSamples != expected.lengthSamples ? "start/length mismatch"
              : a.memberIds != expected.memberIds ? "group topology mismatch"
              : a.inputHashes != expected.inputHashes ? "input content/automation hash mismatch"
              : a.modelVersion != expected.modelVersion ? "model version mismatch"
              : "manifest mismatch";
        return false;
    }
    PayloadShape shape;
    if (!validateBundle(bundle, shape, error)) return false;
    b_ = &bundle;
    error.clear();
    return true;
}

bool BundleReplayer::readBleed(std::size_t member, std::uint64_t pos, std::size_t n, float* l, float* r) const {
    if (!b_ || member >= b_->manifest.memberIds.size() || !l || !r) return false;
    const auto& m = b_->manifest;
    if (pos < m.startSample || pos - m.startSample > m.lengthSamples || n > m.lengthSamples - (pos - m.startSample)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t rel = pos - m.startSample + i;
        const std::uint64_t tick = rel / kQuantumFrames;
        const std::size_t off = std::size_t(rel % kQuantumFrames);
        l[i] = b_->bleed[b_->bleedOffset(member, tick, 0) + off];
        r[i] = b_->bleed[b_->bleedOffset(member, tick, 1) + off];
    }
    return true;
}

bool BundleReplayer::readRail(std::uint64_t pos, std::size_t n, float* volts, float* amps) const {
    if (!b_ || !volts || !amps) return false;
    const auto& m = b_->manifest;
    if (pos < m.startSample || pos - m.startSample > m.lengthSamples || n > m.lengthSamples - (pos - m.startSample)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t tick = (pos - m.startSample + i) / kQuantumFrames;
        volts[i] = b_->railVolts[std::size_t(tick)]; amps[i] = b_->railDemandA[std::size_t(tick)];
    }
    return true;
}

}  // namespace cd::network
