// CircuitDrift tests: T11 over POSIX shared memory and hostile shared-memory input —
// owner-only 0600 objects named from the random session token, permission/size/name refusal,
// a member in a SEPARATE PROCESS (fork) coupled through the broker, a broker PROCESS killed with
// SIGKILL mid-run (members never wait: RtTrap alloc/lock/IO counters), and 2000+2000 header/live
// mutations (fuzz) rejected or contained without crash (run under ASan/UBSan in CI).
// fork() on one Linux box is NOT DAW multi-process qualification: per-host process/sandbox/
// prefetch behaviour stays open under G05. Signals/PSU are SYNTHETIC fixtures. Owner: network agent.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "network/Broker.h"
#include "network/Member.h"
#include "network/SharedRegion.h"
#include "support/RtTrap.h"
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace cd::network;
using cd::test::RtTrapCounts;
using cd::test::RtTrapScope;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

constexpr double kFs = 48000;

struct Lcg {
    std::uint64_t s;
    std::uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return std::uint32_t(s >> 33); }
    std::uint64_t u64() { return (std::uint64_t(u()) << 32) | u(); }
};

double signal(std::uint32_t member, std::uint32_t ch, std::int64_t s) {
    return 0.5 * std::sin(2.0 * M_PI * (90.0 + 37.0 * member) * double(s) / kFs + 0.3 * ch);
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

GroupConfig groupConfig(std::uint32_t maxMembers) {
    GroupConfig c;
    c.sessionKey = "shm-key";
    c.name = "shm";
    c.maxMembers = maxMembers;
    c.bleed = BleedConfig{true, -24.0};
    c.group = Uuid::random();
    c.session = Uuid::random();
    return c;
}

JoinRequest joinFor(const GroupConfig& g, std::uint32_t idx) {
    JoinRequest r;
    r.member = Uuid::fromSeed(500 + idx);
    r.expectedGroup = g.group;
    r.sessionKey = g.sessionKey;
    r.sampleRate = g.sampleRate;
    r.channels = 2;
    r.psu = g.psu;
    return r;
}

// A member driven by a virtual transport derived from a shared monotonic clock (1 quantum/ms), so
// separate processes share one timeline like tracks of one host.
struct Driver {
    std::unique_ptr<MemberClient> client;
    std::uint32_t idx = 0;
    std::int64_t pos = -1, first = -1;
    std::vector<double> bleedL;   // channel 0, absolute sample index
    RtTrapCounts trap{};
    double maxCallMs = 0;
    std::uint32_t statusOr = 0;
    void runTo(std::int64_t target) {
        if (pos < 0) first = pos = target - target % 128;
        while (pos < target) {
            double tb[2][128], ob[2][128];
            for (int c = 0; c < 2; ++c) for (int f = 0; f < 128; ++f) tb[c][f] = signal(idx, std::uint32_t(c), pos + f);
            const double* tap[2] = {tb[0], tb[1]};
            double* out[2] = {ob[0], ob[1]};
            NetContext ctx;
            ctx.sampleRate = kFs;
            ctx.sampleStart = pos;
            ctx.frames = 128;
            ctx.powerW = 5;
            const double t0 = nowMs();
            NetResult r;
            {
                RtTrapScope s;
                r = client->endpoint().process(ctx, tap, out);
                const auto c = s.counts();
                trap.allocations += c.allocations; trap.deallocations += c.deallocations; trap.mutexLocks += c.mutexLocks;
                trap.condWaits += c.condWaits; trap.ioCalls += c.ioCalls;
            }
            maxCallMs = std::max(maxCallMs, nowMs() - t0);
            statusOr |= r.status;
            if (bleedL.size() < std::size_t(pos + 128)) bleedL.resize(std::size_t(pos + 128) + 65536, 0.0);
            std::copy(ob[0], ob[0] + 128, bleedL.begin() + pos);
            pos += 128;
        }
    }
};

#if defined(__linux__)
void posixObjectSecurity() {
    std::string err;
    const Uuid session = Uuid::random();
    const std::string name = PosixShmRegion::nameForSession(session);
    CHECK(name.size() == 30 && name.rfind("/cdip-", 0) == 0, "name %s", name.c_str());
    auto r = PosixShmRegion::create(name, 1 << 20, err);
    CHECK(r != nullptr, "create: %s", err.c_str());
    if (!r) return;
    struct stat st {};
    const std::string path = "/dev/shm/" + name.substr(1);
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == ::geteuid(), "mode %o", unsigned(st.st_mode & 0777));
    CHECK(!PosixShmRegion::create(name, 1 << 20, err), "O_EXCL create succeeded twice");
    auto o = PosixShmRegion::open(name, err);
    CHECK(o && o->size() == std::size_t(1 << 20), "open: %s", err.c_str());
    if (o) { r->data()[777] = std::byte{0x5a}; CHECK(o->data()[777] == std::byte{0x5a}, "mappings not shared"); }
    for (const char* bad : {"noslash", "/../etc", "/UPPER", "/cdip-0123456789abcdef0123456789abcdef"})
        CHECK(!PosixShmRegion::open(bad, err) && !PosixShmRegion::create(bad, 4096, err), "bad name %s accepted", bad);
    // Foreign permissions / bad size are refused by open().
    const std::string loose = PosixShmRegion::nameForSession(Uuid::random());
    int fd = ::shm_open(loose.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0, "shm_open loose");
    if (fd >= 0) {
        CHECK(::fchmod(fd, 0644) == 0 && ::ftruncate(fd, 1 << 20) == 0, "loose setup");
        CHECK(!PosixShmRegion::open(loose, err) && err.find("permissions") != std::string::npos, "0644 object accepted: %s", err.c_str());
        CHECK(::fchmod(fd, 0600) == 0 && ::ftruncate(fd, 16) == 0, "shrink");
        CHECK(!PosixShmRegion::open(loose, err) && err.find("size") != std::string::npos, "16-byte object accepted: %s", err.c_str());
        ::close(fd);
        ::shm_unlink(loose.c_str());
    }
    const std::string n2 = name;
    o.reset();
    r.reset();   // owner unlinks
    CHECK(!PosixShmRegion::open(n2, err), "name not unlinked by owner");
    std::printf("T11 shm security: /cdip-<96-bit token>, mode 0600 owner-only, O_EXCL, invalid names, 0644 and undersized "
                "objects refused, owner unlinks\n");
}

void crossProcessMember() {
    std::string err;
    GroupConfig cfg = groupConfig(2);
    Layout l;
    computeLayout(2, l);
    auto region = PosixShmRegion::create(PosixShmRegion::nameForSession(cfg.session), l.totalBytes, err);
    CHECK(region != nullptr, "create: %s", err.c_str());
    if (!region) return;
    const std::string name = region->name();
    auto broker = GroupBroker::create(cfg, std::move(region), err);
    CHECK(broker != nullptr, "broker: %s", err.c_str());
    if (!broker) return;
    const double t0 = nowMs();
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {   // ---- child process: member 1 ----
        std::string e;
        auto c = MemberClient::attach(PosixShmRegion::open(name, e), joinFor(cfg, 1), e);
        if (!c) ::_exit(10);
        const double deadline = nowMs() + 3000;
        while (c->poll() == MemberClient::JoinState::Pending && nowMs() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (c->poll() != MemberClient::JoinState::Active) ::_exit(11);
        Driver d;
        d.client = std::move(c);
        d.idx = 1;
        const double end = nowMs() + 600;
        while (nowMs() < end) {
            d.runTo(std::int64_t((nowMs() - t0) * 128.0));
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        // received bleed must be exact (peer 0's tap delayed L quanta) or zero (missing/late)
        const std::int64_t start = d.first;
        std::int64_t exact = 0, zero = 0, bad = 0;
        const double k = broker->gain() * memberSign(cfg.bleedSeed, Uuid::fromSeed(500));
        for (std::int64_t s = start + 60 * 128; s < d.pos; ++s) {
            const double v = d.bleedL[std::size_t(s)], e2 = k * 1.0 * signal(0, 0, s - 8 * 128);
            if (v == e2) ++exact; else if (v == 0.0) ++zero; else ++bad;
        }
        const double frac = double(exact) / double(std::max<std::int64_t>(1, exact + zero + bad));
        std::printf("T11 cross-process member (child pid): exact %.1f%% zero %.1f%% other %lld, RT violations %ld, max call %.3f ms\n",
                    100 * frac, 100.0 * double(zero) / double(std::max<std::int64_t>(1, exact + zero + bad)), (long long)bad,
                    d.trap.violations(), d.maxCallMs);
        std::fflush(stdout);
        ::_exit(bad == 0 && frac >= 0.5 && d.trap.violations() == 0 ? 0 : 12);
    }
    CHECK(pid > 0, "fork failed");
    if (pid < 0) return;
    // ---- parent process: broker + member 0 ----
    auto c0 = MemberClient::attach(PosixShmRegion::open(name, err), joinFor(cfg, 0), err);
    CHECK(c0 != nullptr, "parent attach: %s", err.c_str());
    Driver d;
    if (c0) {
        broker->service(nowMs());
        CHECK(c0->poll() == MemberClient::JoinState::Active, "parent member not active");
        d.client = std::move(c0);
    }
    int status = 0;
    pid_t done = 0;
    const double end = nowMs() + 6000;
    while (done == 0 && nowMs() < end) {
        if (d.client) d.runTo(std::int64_t((nowMs() - t0) * 128.0));
        broker->service(nowMs());
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        done = ::waitpid(pid, &status, WNOHANG);
    }
    if (done == 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); }
    CHECK(done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child member failed (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    const auto bd = broker->diagnostics();
    CHECK(bd.joins == 2 && bd.ticks > 100, "broker joins %lld ticks %lld", (long long)bd.joins, (long long)bd.ticks);
    CHECK(d.trap.violations() == 0, "parent RT violations %ld", d.trap.violations());
    std::printf("T11 cross-process: broker+member in parent, member in forked child over POSIX shm: %lld ticks (%lld degraded), "
                "child exit %d, parent RT violations %ld, max parent call %.3f ms\n",
                (long long)bd.ticks, (long long)bd.degradedTicks, WIFEXITED(status) ? WEXITSTATUS(status) : -1, d.trap.violations(),
                d.maxCallMs);
}

void brokerProcessKilled() {
    std::string err;
    GroupConfig cfg = groupConfig(2);
    Layout l;
    computeLayout(2, l);
    auto region = PosixShmRegion::create(PosixShmRegion::nameForSession(cfg.session), l.totalBytes, err);
    CHECK(region != nullptr, "create: %s", err.c_str());
    if (!region) return;
    const std::string name = region->name();
    auto broker = GroupBroker::create(cfg, std::move(region), err);
    if (!broker) { CHECK(false, "broker: %s", err.c_str()); return; }
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {   // ---- child: the broker process ----
        for (;;) { broker->service(nowMs()); std::this_thread::sleep_for(std::chrono::microseconds(300)); }
    }
    CHECK(pid > 0, "fork failed");
    if (pid < 0) return;
    Driver d[2];
    bool ok = true;
    for (std::uint32_t i = 0; i < 2; ++i) {
        d[i].idx = i;
        d[i].client = MemberClient::attach(PosixShmRegion::open(name, err), joinFor(cfg, i), err);
        if (!d[i].client) { ok = false; break; }
        const double deadline = nowMs() + 3000;
        while (d[i].client->poll() == MemberClient::JoinState::Pending && nowMs() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ok = ok && d[i].client->poll() == MemberClient::JoinState::Active;
    }
    CHECK(ok, "members did not join the broker process: %s", err.c_str());
    if (!ok) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); return; }
    const double t0 = nowMs();
    auto runFor = [&](double ms) {
        const double end = nowMs() + ms;
        while (nowMs() < end) {
            for (auto& m : d) m.runTo(std::int64_t((nowMs() - t0) * 128.0));
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    };
    runFor(300);
    const auto before = d[0].client->endpoint().diagnostics();
    ::kill(pid, SIGKILL);   // broker process dies without any shutdown
    ::waitpid(pid, nullptr, 0);
    const std::int64_t killPos = d[0].pos;
    runFor(900);
    const auto after = d[0].client->endpoint().diagnostics();
    bool tailZero = true;
    for (auto& m : d)
        for (std::int64_t s = m.pos - 100 * 128; s < m.pos; ++s) tailZero = tailZero && m.bleedL[std::size_t(s)] == 0.0;
    const long viol = d[0].trap.violations() + d[1].trap.violations();
    CHECK(before.resultsUsed > 50 && !before.brokerLost, "no coupling before the kill (%lld results)", (long long)before.resultsUsed);
    CHECK(after.brokerLost && after.influence == 0.0 && tailZero, "fallback after broker kill incomplete");
    CHECK((d[1].statusOr & netstatus::kBrokerLost) != 0, "member 1 status 0x%x", d[1].statusOr);
    CHECK(viol == 0, "RT violations %ld", viol);
    std::printf("T11 broker process SIGKILL mid-run: %lld results used before, members kept processing %lld samples after the "
                "kill with %ld RT violations (alloc/lock/IO), max callback %.3f ms, broker lost -> local PSU, zero bleed\n",
                (long long)before.resultsUsed, (long long)(d[0].pos - killPos), viol, std::max(d[0].maxCallMs, d[1].maxCallMs));
}
#endif

// ---------------------------------------------------------------------------------------------
// Fuzz A: hostile header bytes / fields on attach.
void fuzzHeader() {
    ArenaDirectory dir;
    std::string err;
    GroupConfig cfg = groupConfig(2);
    Layout l;
    computeLayout(2, l);
    auto arena = dir.create("pristine", l.totalBytes, err);
    auto broker = GroupBroker::create(cfg, std::make_unique<ArenaView>(arena), err);
    CHECK(broker != nullptr, "broker %s", err.c_str());
    if (!broker) return;
    std::vector<std::byte> pristine(arena->data(), arena->data() + arena->size());
    Lcg r{31337};
    int rejected = 0, accepted = 0, mustRejectMissed = 0;
    auto scratch = ArenaRegion::create("scratch", l.totalBytes);
    for (int it = 0; it < 2000; ++it) {
        std::memcpy(scratch->data(), pristine.data(), pristine.size());
        auto* h = reinterpret_cast<RegionHeader*>(scratch->data());
        bool mustReject = false;
        const int kind = int(r.u() % 3);
        if (kind == 0) {   // random byte flips in the header
            const int k = 1 + int(r.u() % 8);
            for (int q = 0; q < k; ++q) {
                const std::size_t off = r.u() % sizeof(RegionHeader);
                const std::byte nb = std::byte(r.u() & 0xff);
                if (off < offsetof(RegionHeader, pad0) && scratch->data()[off] != nb) mustReject = true;
                scratch->data()[off] = nb;
            }
        } else if (kind == 1) {   // random 64-bit word
            const std::size_t off = 8 * (r.u() % (sizeof(RegionHeader) / 8));
            std::uint64_t v = r.u64();
            std::uint64_t old;
            std::memcpy(&old, scratch->data() + off, 8);
            if (off < offsetof(RegionHeader, pad0) && v != old) mustReject = true;
            std::memcpy(scratch->data() + off, &v, 8);
        } else {   // attacker that recomputes the hash: structural fields must still be bounds-checked
            switch (r.u() % 14) {
                case 0: h->totalBytes = ~0ull; break;
                case 1: h->totalBytes = l.totalBytes + 64; break;
                case 2: h->entryOffset = l.totalBytes; break;
                case 3: h->ringsOffset = 1ull << 40; break;
                case 4: h->memberStride = 0; break;
                case 5: h->maxMembers = (r.u() & 1) ? 33u : 1u; break;
                case 6: h->ringSlots = 63; break;
                case 7: h->quantum = 0; break;
                case 8: h->channels = 3; break;
                case 9: h->sampleRate = std::nan(""); break;
                case 10: h->latencyQuanta = 0; break;
                case 11: std::memset(h->groupName, 'A', sizeof h->groupName); break;
                case 12: h->upFrameBytes = 1u << 30; break;
                default: h->major = 2; break;
            }
            h->immutableHash = hash64(h, offsetof(RegionHeader, immutableHash));
            mustReject = true;
        }
        auto c = MemberClient::attach(std::make_unique<ArenaView>(scratch), joinFor(cfg, 0), err);
        if (c) ++accepted; else ++rejected;
        if (mustReject && c) ++mustRejectMissed;
    }
    // truncated mapping
    Layout tmp;
    CHECK(validateRegion(arena->data(), arena->size() - 64, tmp) == RegionError::TotalBytes, "truncated mapping accepted");
    CHECK(validateRegion(arena->data(), 100, tmp) == RegionError::TooSmall, "tiny mapping accepted");
    CHECK(mustRejectMissed == 0, "%d corrupted headers accepted", mustRejectMissed);
    std::printf("T11 fuzz header: 2000 mutations (byte flips, random words, hash-recomputing hostile fields) -> %d rejected, %d "
                "accepted (mutable-word changes only), 0 corrupted immutable headers accepted, no crash\n", rejected, accepted);
}

// Fuzz B: hostile writes into a live region (entries, ring indices, frames, mutable words).
struct Live {
    std::shared_ptr<ArenaRegion> arena;
    std::unique_ptr<GroupBroker> broker;
    std::unique_ptr<MemberClient> m[2];
    std::int64_t pos = 0;
    double now = 0;
};

bool buildLive(Live& L, const GroupConfig& cfg) {
    std::string err;
    Layout l;
    computeLayout(2, l);
    L = Live{};
    L.arena = ArenaRegion::create("live", l.totalBytes);
    L.broker = GroupBroker::create(cfg, std::make_unique<ArenaView>(L.arena), err);
    if (!L.broker) return false;
    for (std::uint32_t i = 0; i < 2; ++i) {
        L.m[i] = MemberClient::attach(std::make_unique<ArenaView>(L.arena), joinFor(cfg, i), err);
        if (!L.m[i]) return false;
        L.broker->service(L.now);
        if (L.m[i]->poll() != MemberClient::JoinState::Active) return false;
    }
    return true;
}

void fuzzLive() {
    GroupConfig cfg = groupConfig(2);
    Live L;
    CHECK(buildLive(L, cfg), "live setup");
    Layout l;
    computeLayout(2, l);
    Lcg r{4242};
    RtTrapCounts trap{};
    int rebuilds = 0;
    double maxAbs = 0;
    bool finite = true;
    std::int64_t invalidSeen = 0;
    auto step = [&]() {
        for (std::uint32_t i = 0; i < 2; ++i) {
            double tb[2][128], ob[2][128];
            for (int c = 0; c < 2; ++c) for (int f = 0; f < 128; ++f) tb[c][f] = signal(i, std::uint32_t(c), L.pos + f);
            const double* tap[2] = {tb[0], tb[1]};
            double* out[2] = {ob[0], ob[1]};
            NetContext ctx;
            ctx.sampleRate = kFs;
            ctx.sampleStart = L.pos;
            ctx.frames = 128;
            {
                RtTrapScope s;
                L.m[i]->endpoint().process(ctx, tap, out);
                const auto c = s.counts();
                trap.allocations += c.allocations; trap.deallocations += c.deallocations; trap.mutexLocks += c.mutexLocks;
                trap.condWaits += c.condWaits; trap.ioCalls += c.ioCalls;
            }
            for (int c = 0; c < 2; ++c)
                for (int f = 0; f < 128; ++f) { finite = finite && std::isfinite(ob[c][f]); maxAbs = std::max(maxAbs, std::fabs(ob[c][f])); }
        }
        L.pos += 128;
        L.now += 1000.0 * 128 / kFs;
        L.broker->service(L.now);
    };
    for (int k = 0; k < 20; ++k) step();
    for (int it = 0; it < 2000; ++it) {
        std::byte* base = L.arena->data();
        const int kind = int(r.u() % 6);
        if (kind == 0) {   // member entry words / request fields
            const std::size_t off = l.entryOffset + (r.u() % 2) * l.entryStride + 8 * (r.u() % 8);
            const std::uint64_t v = (r.u() & 1) ? r.u64() : std::uint64_t(r.u() % 8);
            std::memcpy(base + off, &v, 8);
        } else if (kind == 1) {   // ring indices (tail/head of up or down ring)
            const std::size_t blk = l.ringsOffset + (r.u() % 2) * l.memberStride + ((r.u() & 1) ? l.downRingOffset : l.upRingOffset);
            const std::size_t off = blk + ((r.u() & 1) ? offsetof(RingCtl, head) : offsetof(RingCtl, tail));
            std::uint64_t v;
            std::memcpy(&v, base + off, 8);
            v = (r.u() & 1) ? r.u64() : v + (r.u() % 3) - 1;   // garbage or off-by-one
            std::memcpy(base + off, &v, 8);
        } else if (kind == 2 || kind == 3) {   // frame bytes (envelopes and payloads) in any slot
            const std::size_t blk = l.ringsOffset + (r.u() % 2) * l.memberStride;
            const bool down = r.u() & 1;
            const std::size_t slots = blk + (down ? l.downRingOffset : l.upRingOffset) + sizeof(RingCtl);
            const std::size_t frame = slots + (r.u() % kRingSlots) * sizeof(UpFrame);
            const std::size_t off = frame + ((r.u() & 1) ? (r.u() % sizeof(Envelope)) : (r.u() % sizeof(UpFrame)));
            if (kind == 2) {
                base[off] = std::byte(r.u() & 0xff);
            } else {   // hostile doubles: NaN, Inf, huge, plausible
                const double vals[] = {std::nan(""), INFINITY, -1e300, 0.9, -0.05, 1e-3};
                const double v = vals[r.u() % 6];
                std::memcpy(base + (off & ~std::size_t(7)), &v, 8);
            }
        } else if (kind == 4) {   // mutable header words
            const std::size_t offs[] = {offsetof(RegionHeader, rosterGeneration), offsetof(RegionHeader, brokerHeartbeat),
                                        offsetof(RegionHeader, brokerState)};
            const std::uint64_t v = (r.u() & 1) ? r.u64() : std::uint64_t(r.u() % 4);
            std::memcpy(base + offs[r.u() % 3], &v, 8);
        } else {   // random bytes anywhere in the region
            for (int q = 0; q < 16; ++q) base[r.u() % L.arena->size()] = std::byte(r.u() & 0xff);
        }
        for (int k = 0; k < 2; ++k) step();
        for (auto& m : L.m) invalidSeen += m->endpoint().diagnostics().invalidFrames;
        bool broken = false;
        for (auto& m : L.m) {
            const auto d = m->endpoint().diagnostics();
            broken = broken || d.brokerLost || d.evicted || d.ringCorrupt;
        }
        if (broken || it % 97 == 96) {
            ++rebuilds;
            if (!buildLive(L, cfg)) { CHECK(false, "rebuild failed"); return; }
            for (int k = 0; k < 12; ++k) step();
        }
    }
    const double lim = kMaxBleedGain * kTapLimit;
    CHECK(finite && maxAbs <= lim, "hostile frames leaked: max|bleed| %.3e (limit %.3e)", maxAbs, lim);
    CHECK(trap.violations() == 0, "RT violations %ld under fuzz", trap.violations());
    std::printf("T11 fuzz live: 2000 hostile writes (entries, ring indices, envelopes/payloads incl. NaN/Inf/huge, mutable words, "
                "random bytes) -> no crash, %d rebuilds after detected corruption/eviction, outputs finite, max|bleed| %.3e <= %.3e, "
                "RT violations %ld\n", rebuilds, maxAbs, lim, trap.violations());
    (void)invalidSeen;
}

}  // namespace

int main() {
#if defined(__linux__)
    posixObjectSecurity();
    crossProcessMember();
    brokerProcessKilled();
#else
    std::printf("T11 POSIX shm/fork tests: SKIPPED on this platform (not Linux)\n");
#endif
    fuzzHeader();
    fuzzLive();
    if (g_fail) { std::printf("network_shm: %d failure(s)\n", g_fail); return 1; }
    std::printf("network_shm: all checks passed\n");
    return 0;
}
