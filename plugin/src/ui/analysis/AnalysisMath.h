// CircuitDrift plugin UI: analysis math (UX-04/05/06, T17). Pure std C++: no JUCE, no audio
// coupling, fully unit-testable. Message thread only. Every number derived here comes from
// TelemetryFrame rings; nothing is synthesised.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace cd::plugin::ui::analysis {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStaleMs = 250.0;  // 06: stale data >250 ms gets a Stale badge

// ---- UX-04 transfer XY: equal volts-per-pixel mapping ------------------------------------------
struct XyMap {
    double cx = 0, cy = 0, pxPerUnit = 1;
    double px(double v) const noexcept { return cx + v * pxPerUnit; }
    double py(double v) const noexcept { return cy - v * pxPerUnit; }  // screen y grows downward
};
// Fixed scale: +-fullScale maps to the shorter side of the plot rectangle, same factor on both axes.
XyMap makeEqualScaleMap(double x, double y, double w, double h, double fullScale) noexcept;
// Endpoints (pixels) of the reference line y = gain * x over +-fullScale (the dashed unity guide).
void referenceLine(const XyMap& m, double gain, double fullScale, double& x0, double& y0, double& x1, double& y1) noexcept;
// Perpendicular pixel distance of (px,py) from the line through (x0,y0)-(x1,y1).
double distanceToLine(double px, double py, double x0, double y0, double x1, double y1) noexcept;
// Max pixel distance of the mapped trace (in[i], out[i]) from the reference line of `gain`.
double maxPixelErrorToReference(const XyMap& m, const double* in, const double* out, int n, double gain, double fullScale) noexcept;
// Least-squares slope dOut/dIn of the points with |in| <= window (NaN when fewer than 3 points).
double smallSignalSlope(const double* in, const double* out, int n, double window) noexcept;
// Pixel slope (rise over run, up positive) of the mapped trace; equals the gain on an equal-scale map.
double pixelSlope(const XyMap& m, const double* in, const double* out, int n) noexcept;

// Ordered extrema-preserving decimation (UX-04: <= 2048 points, never sorted by amplitude): each
// bucket keeps its first, y-min and y-max points in original time order.
void decimateOrderedExtrema(const double* x, const double* y, int n, int maxPoints, std::vector<double>& ox, std::vector<double>& oy);
// Copies a ring oldest-first (`write` is the index of the next write = oldest entry).
void unrollRing(const double* ring, int size, int write, double* out) noexcept;

// ---- UX-05 rail history: 1 ms bins, 10 s ---------------------------------------------------------
struct RailBin { float vmin = 0, vmean = 0, vmax = 0; bool valid = false; };
class RailHistory {
public:
    static constexpr int kBins = 10000;
    RailHistory();
    void clear();
    // `ring[(ringWrite + k) % n]`, k = 0..n-1 oldest -> newest; the newest sample is at host sample
    // `sampleIndexNewest`; consecutive entries are `spacingSamples` host samples apart.
    // Returns true when the history was cleared because time moved backwards (seek/graph change).
    bool ingest(std::int64_t sampleIndexNewest, double sampleRate, const double* ring, int ringWrite, int n, int spacingSamples);
    bool empty() const noexcept { return newest_ < 0; }
    std::int64_t newestBin() const noexcept { return newest_; }
    std::int64_t oldestBin() const noexcept { return newest_ < 0 ? 0 : std::max<std::int64_t>(0, newest_ - kBins + 1); }
    bool binAt(std::int64_t absBin, RailBin& out) const noexcept;
private:
    std::vector<RailBin> bins_;
    std::vector<std::int64_t> stamp_;
    std::int64_t newest_ = -1;
};
struct EnvelopeColumn { float vmin = 0, vmean = 0, vmax = 0; bool valid = false; };
// Display-column decimation with a min/max envelope over [t0Bin, t1Bin) (gaps stay invalid).
void decimateEnvelope(const RailHistory& h, std::int64_t t0Bin, std::int64_t t1Bin, int columns, std::vector<EnvelopeColumn>& out);

struct RecoveryResult {
    enum class Status { NoEvent, Recovered, NotRecovered, RejectedOverlap, RejectedGap } status = Status::NoEvent;
    double minVolts = 0, preLevelVolts = 0, recoveryMs = 0;  // recoveryMs: min -> 95% of the droop
    std::int64_t minBin = 0;
};
// Minimum rail and time to recover 95% of the droop relative to the pre-event level (max of the
// mean over the 300 ms before the minimum). Overlapping events/gaps are rejected, not misreported.
RecoveryResult measureRecovery(const RailHistory& h, std::int64_t t0Bin, std::int64_t t1Bin, double minDroopFraction = 0.02);

// ---- UX-06 cross spectrum / phase / coherence ----------------------------------------------------
class Fft {
public:
    explicit Fft(int size);  // power of two
    int size() const noexcept { return n_; }
    void forward(std::vector<std::complex<double>>& data) const;
private:
    int n_;
    std::vector<std::complex<double>> tw_;
    std::vector<int> rev_;
};
struct PhaseBin { double freqHz = 0, phaseRad = 0, coherence = 0, balance = 0, levelDbfs = -200; };
class CrossSpectrumAnalyzer {
public:
    // UX-06 uses fftSize 2048, hop 512 and tau 0.25 s; frame mode (ring-limited telemetry) may use
    // a smaller window, documented at the call site.
    CrossSpectrumAnalyzer(int fftSize, int hop, double sampleRate, double tauSeconds = 0.25);
    void reset();
    void setSampleRate(double sr) noexcept { fs_ = sr; }
    // Continuous stream: processes a frame every `hop` new samples.
    void push(const double* a, const double* b, int n);
    // Exactly fftSize samples; dtSeconds is the time since the previous frame (smoothing constant).
    void processFrame(const double* a, const double* b, double dtSeconds);
    int frames() const noexcept { return frames_; }
    // True once at least one time constant of data has been smoothed (coherence is meaningful).
    bool settled() const noexcept { return elapsedSeconds_ >= tau_; }
    int fftSize() const noexcept { return fftSize_; }
    void bins(std::vector<PhaseBin>& out) const;  // positive-frequency bins 1..N/2 (DC excluded)
private:
    int fftSize_, hop_;
    double fs_, tau_, elapsedSeconds_ = 0;
    int frames_ = 0, pos_ = 0, count_ = 0, sinceHop_ = 0;
    Fft fft_;
    std::vector<double> window_, ringA_, ringB_;
    std::vector<std::complex<double>> xa_, xb_, sLR_;
    std::vector<double> sLL_, sRR_;
    double ampScale_ = 1;
};
double wrapPi(double rad) noexcept;
// Visibility rule: hide bins below -70 dBFS or coherence < 0.1.
bool binVisible(const PhaseBin& b) noexcept;
struct ScatterPoint { double x = 0, y = 0, radius01 = 0, angle = 0; };
// Polar mapping: radius = log(f/20)/log(fmax/20) clamped 0..1, angle = phase (0 = +x axis, CCW).
ScatterPoint scatterPoint(double freqHz, double phaseRad, double fmaxHz, double cx, double cy, double radiusPx) noexcept;
// Fullband normalized correlation (cosine similarity) of two equal-length buffers; 0 when silent.
double normalizedCorrelation(const double* a, const double* b, int n) noexcept;

// ---- Stale detection, hold meters, history -------------------------------------------------------
class StaleTracker {
public:
    // Call once per UI frame. `valid` false means no frame was available.
    void update(std::uint64_t generation, std::int64_t sampleIndex, bool valid, double nowMs) noexcept;
    bool stale(double nowMs) const noexcept;
    double ageMs(double nowMs) const noexcept;
    void reset() noexcept { everValid_ = false; lastValid_ = false; }
private:
    bool everValid_ = false, lastValid_ = false;
    std::uint64_t gen_ = 0;
    std::int64_t sample_ = 0;
    double lastAdvanceMs_ = 0;
};
// Peak hold 1 s, clip indicator 2 s (UX-05 gain meters).
struct PeakHold {
    double held = 0, holdAgeMs = 1e9, clipAgeMs = 1e9;
    void update(double peak, double dtMs) noexcept;
    bool clipLit() const noexcept { return clipAgeMs < 2000.0; }
};
// Time series of one value per frame with explicit gaps (NaN) so plots never bridge missing data.
class SeriesHistory {
public:
    explicit SeriesHistory(int capacity = 600) : cap_(capacity) {}
    void clear() { t_.clear(); v_.clear(); }
    void add(double tSeconds, double v);   // v = NaN marks a gap
    int size() const noexcept { return static_cast<int>(t_.size()); }
    double t(int i) const { return t_[static_cast<std::size_t>(i)]; }
    double v(int i) const { return v_[static_cast<std::size_t>(i)]; }
private:
    int cap_;
    std::vector<double> t_, v_;
};

double toDb(double linear, double floorDb = -120.0) noexcept;

}  // namespace cd::plugin::ui::analysis
