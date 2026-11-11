// CircuitDrift analysis: engine telemetry contract (T17, P09).
// Owner: cd_analysis. Spec: 06-interface-inspector-export.md "Engine telemetry -> analysis".
// TelemetryFrame is a fixed-size POD with explicitly unit-suffixed fields (K, V, A, W). The audio
// thread owns TelemetryWriter (noexcept, wait-free, never blocks, never allocates); the UI/analysis
// thread owns TelemetryReader (may allocate only at construction). A full queue drops the NEW
// frame and increments a counter; the writer-assigned sequence number advances for dropped frames
// too, so the reader marks the gap. Stale data is flagged by the reader, never extrapolated.
// Threading: exactly one writer thread and one reader thread per TelemetryChannel. The reader
// disappearing (close()) or stalling can never block or change the writer's behaviour except that
// frames are counted as dropped / discarded.
#pragma once
#include "state/RtQueue.h"
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace cd::analysis {

inline constexpr std::uint32_t kTelemetrySchemaVersion = 1;
inline constexpr int kMaxTelemetryComponents = 16;  // spec allows 64; extend via schema bump
inline constexpr int kMaxTelemetryXy = 16;
inline constexpr double kStaleAfterSeconds = 0.250;

// Validity bits for TelemetryFrame::validityFlags.
enum TelemetryValidity : std::uint32_t {
    kValidRail = 1u << 0,
    kValidXy = 1u << 1,
    kValidMagnetic = 1u << 2,  // xy[] carries H [A/m] and B [T]
    kValidSolver = 1u << 3,
};
// Bits for TelemetryFrame::fallbackFlags.
enum TelemetryFallback : std::uint32_t {
    kFallbackNone = 0,
    kFallbackSolverDiverged = 1u << 0,
    kFallbackBypassed = 1u << 1,
    kFallbackLimiterActive = 1u << 2,
};

struct ComponentTelemetry {
    std::uint32_t componentId = 0;
    std::uint32_t flags = 0;  // bit0: record valid
    float temperatureK = 0.0f;
    float dissipationW = 0.0f;
    float currentA = 0.0f;
    float voltageV = 0.0f;
};

// Units of x/y depend on TelemetryFrame::xyKind (volts/volts, volts/kelvin, H A/m / B T).
struct XyPair {
    float x = 0.0f;
    float y = 0.0f;
};
enum class XyKind : std::uint32_t { InputOutputVolts = 0, VoltsKelvin = 1, MagneticHB = 2 };

struct TelemetryFrame {
    std::uint32_t schemaVersion = kTelemetrySchemaVersion;
    std::uint32_t graphGeneration = 0;
    std::uint32_t epoch = 0;
    std::uint32_t sequence = 0;  // assigned by the writer for every publish attempt
    std::int64_t sampleIndex = 0;  // sample timestamp (not arrival time)
    std::uint32_t validityFlags = 0;
    std::uint32_t fallbackFlags = 0;
    float railVolts = 0.0f;
    float railFraction = 0.0f;  // rail / V_nom
    float railNominalVolts = 0.0f;
    std::uint32_t solverIterations = 0;
    float solverResidual = 0.0f;
    std::uint32_t componentCount = 0;
    std::uint32_t xyCount = 0;
    std::uint32_t xyKind = 0;  // XyKind
    std::uint32_t reserved = 0;
    ComponentTelemetry components[kMaxTelemetryComponents];
    XyPair xy[kMaxTelemetryXy];
};
static_assert(std::is_trivially_copyable_v<TelemetryFrame>);
static_assert(std::is_standard_layout_v<TelemetryFrame>);
static_assert(sizeof(TelemetryFrame) % 8 == 0);

// UI-side converted view of a frame: temperatures in degrees Celsius.
struct DisplayComponent {
    std::uint32_t componentId = 0;
    double temperatureC = 0.0;
    double dissipationW = 0.0;
    double currentA = 0.0;
    double voltageV = 0.0;
};
struct DisplayFrame {
    TelemetryFrame raw{};
    DisplayComponent components[kMaxTelemetryComponents]{};
    std::uint32_t missedBefore = 0;  // frames dropped immediately before this one
    bool generationChanged = false;  // graphGeneration differs from previous frame: clear traces
    bool epochChanged = false;
    bool schemaMismatch = false;
};

inline constexpr double kKelvinOffset = 273.15;
inline double kelvinToCelsius(double k) noexcept { return k - kKelvinOffset; }

// Queue shared by one writer and one reader. Constructed (allocates) off the audio thread.
class TelemetryChannel {
public:
    explicit TelemetryChannel(std::size_t minCapacity = 64) : queue_(minCapacity) {}
    TelemetryChannel(const TelemetryChannel&) = delete;
    TelemetryChannel& operator=(const TelemetryChannel&) = delete;

private:
    friend class TelemetryWriter;
    friend class TelemetryReader;
    cd::state::SpscQueue<TelemetryFrame> queue_;
    std::atomic<bool> readerAttached_{true};
};

class TelemetryWriter {
public:
    explicit TelemetryWriter(TelemetryChannel& ch) noexcept : ch_(&ch) {}
    // Audio thread. Stamps schemaVersion/sequence; returns false if dropped. Wait-free.
    bool publish(TelemetryFrame frame) noexcept;
    std::uint64_t droppedFull() const noexcept { return droppedFull_.load(std::memory_order_relaxed); }
    std::uint64_t discardedNoReader() const noexcept { return discardedNoReader_.load(std::memory_order_relaxed); }
    std::uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }

private:
    TelemetryChannel* ch_;
    std::uint32_t seq_ = 0;
    std::atomic<std::uint64_t> droppedFull_{0}, discardedNoReader_{0}, published_{0};
};

class TelemetryReader {
public:
    explicit TelemetryReader(TelemetryChannel& ch) noexcept : ch_(&ch) {}
    ~TelemetryReader() { close(); }
    TelemetryReader(const TelemetryReader&) = delete;
    TelemetryReader& operator=(const TelemetryReader&) = delete;

    // Pops the next frame (oldest first). nowSeconds is a monotonic UI clock used only for stale
    // detection. Returns false when empty.
    bool poll(DisplayFrame& out, double nowSeconds) noexcept;
    // Drains to the most recent frame; gaps from skipped frames accumulate in out.missedBefore
    // only if actually lost; skipped-but-delivered frames are not counted as missed.
    bool pollLatest(DisplayFrame& out, double nowSeconds) noexcept;
    // True when no frame arrived within kStaleAfterSeconds of nowSeconds (or none ever).
    bool isStale(double nowSeconds) const noexcept;
    // Detach: the writer then discards instead of queueing. Idempotent.
    void close() noexcept;

    std::uint64_t totalMissed() const noexcept { return totalMissed_; }
    std::uint64_t framesRead() const noexcept { return framesRead_; }
    std::uint32_t lastGeneration() const noexcept { return lastGeneration_; }

private:
    static void convert(const TelemetryFrame& f, DisplayFrame& out) noexcept;
    TelemetryChannel* ch_;
    bool haveLast_ = false;
    std::uint32_t lastSeq_ = 0, lastGeneration_ = 0, lastEpoch_ = 0;
    double lastArrival_ = 0.0;
    std::uint64_t totalMissed_ = 0, framesRead_ = 0;
};

}  // namespace cd::analysis
