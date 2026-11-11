#include "dsp/Oversampling.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace cd::dsp {
namespace {
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 500; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}
}  // namespace

// ---- PolyphaseHalfband2x ------------------------------------------------------------------------------

bool PolyphaseHalfband2x::design(double passEdge, double stopbandDb) {
    clear();
    if (!(passEdge > 0 && passEdge < 0.25) || !(stopbandDb >= 20 && stopbandDb <= 250)) return false;
    // Kaiser (S04): A = target + 10 dB margin; order = (A-7.95)/(14.36*df); beta = 0.1102(A-8.7).
    const double A = stopbandDb + 10.0, df = 0.5 - 2.0 * passEdge;
    const double beta = 0.1102 * (A - 8.7);
    const double requestedCenter = std::ceil((A - 7.95) / (14.36 * df) / 2.0);
    constexpr int maxCenter = (kMaxTaps - 1) / 2;
    // Check in floating point before conversion: narrow transition bands can exceed int range.
    if (!std::isfinite(requestedCenter) || requestedCenter < 1 || requestedCenter > maxCenter) return false;
    int c = static_cast<int>(requestedCenter);
    if (c % 2 == 0) ++c;  // c odd: even-index taps carry the sinc, centre tap is the only odd one
    h_.assign(2 * c + 1, 0.0);
    const double i0b = besselI0(beta);
    double oddSum = 0;
    for (int n = 0; n <= 2 * c; ++n) {
        const int k = n - c;
        if (k == 0 || k % 2 == 0) continue;
        const double r = double(k) / double(c + 1);
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h_[n] = std::sin(kPi * k / 2.0) / (kPi * k) * w;
        oddSum += h_[n];
    }
    for (double& v : h_) v *= 0.5 / oddSum;  // exact DC gain 1, centre stays exactly 0.5
    h_[c] = 0.5;
    c_ = c;
    E_ = c + 1;
    even_.resize(E_);
    for (int j = 0; j < E_; ++j) even_[j] = h_[2 * j];
    upLine_.assign(2 * E_, 0.0);
    dnEven_.assign(2 * E_, 0.0);
    oddLen_ = (c + 1) / 2 + 1;
    dnOdd_.assign(oddLen_, 0.0);
    reset();
    return true;
}

void PolyphaseHalfband2x::clear() noexcept {
    h_.clear(); even_.clear(); upLine_.clear(); dnEven_.clear(); dnOdd_.clear();
    c_ = E_ = upPos_ = dnPos_ = oddPos_ = oddLen_ = 0;
}

double PolyphaseHalfband2x::magnitude(double f) const noexcept {
    if (E_ == 0) return 0;
    double s = 0;
    for (int n = 0; n <= 2 * c_; ++n) s += h_[n] * std::cos(2 * kPi * f * (n - c_));
    return std::fabs(s);
}

// Plain left-to-right sum, deliberately NOT folded/reassociated: the electrical solver downstream
// amplifies last-bit input changes to ~1e-12 (Newton stops at residual 1e-7), so the FIR keeps its
// summation order to stay bit-identical (performance work is held to bit-exact golden outputs).
double PolyphaseHalfband2x::dotEven(const std::vector<double>& line, int pos) const noexcept {
    const double* x = line.data() + pos;
    const double* h = even_.data();
    double acc = 0;
    for (int j = 0; j < E_; ++j) acc += h[j] * x[j];
    return acc;
}

void PolyphaseHalfband2x::up(double x, double* out2) noexcept {
    if (E_ == 0) { out2[0] = out2[1] = 0; return; }
    upPos_ = (upPos_ == 0 ? E_ : upPos_) - 1;
    upLine_[upPos_] = upLine_[upPos_ + E_] = x;  // upLine_[upPos_+j] = x[m-j]
    out2[0] = 2.0 * dotEven(upLine_, upPos_);
    out2[1] = upLine_[upPos_ + (c_ - 1) / 2];    // 2*h[c]=1: odd phase is a pure delay
}

double PolyphaseHalfband2x::down(const double* in2) noexcept {
    if (E_ == 0) return 0;
    // Output at even instant 2m: even taps on even samples, centre tap on odd sample u[2m-c].
    dnPos_ = (dnPos_ == 0 ? E_ : dnPos_) - 1;
    dnEven_[dnPos_] = dnEven_[dnPos_ + E_] = in2[0];
    const int d = (c_ + 1) / 2;
    const int rd = oddPos_ + oddLen_ - d;                 // in [oddLen_-d, 2*oddLen_-d) with d <= oddLen_
    const double odd = dnOdd_[std::size_t(rd >= oddLen_ ? rd - oddLen_ : rd)];
    dnOdd_[oddPos_] = in2[1];
    oddPos_ = oddPos_ + 1 == oddLen_ ? 0 : oddPos_ + 1;
    return dotEven(dnEven_, dnPos_) + 0.5 * odd;
}

namespace {
constexpr int kBlockMin = 4;   // below this the per-sample path is cheaper than history copy-in/out
}

void PolyphaseHalfband2x::reserveBlock(int maxN) {
    linMaxN_ = 0;
    lin_.clear();
    if (E_ == 0 || maxN < kBlockMin) return;
    lin_.assign(std::size_t(E_ - 1) + 2 * std::size_t(maxN), 0.0);
    linMaxN_ = maxN;
}

// Four interleaved left-to-right sums over lin[base+i-j]; each output keeps the original order.
static inline void dotBlock4(const double* h, int E, const double* p, double* out) noexcept {
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (int j = 0; j < E; ++j) {
        const double hj = h[j];
        a0 += hj * p[-j]; a1 += hj * p[1 - j]; a2 += hj * p[2 - j]; a3 += hj * p[3 - j];
    }
    out[0] = a0; out[1] = a1; out[2] = a2; out[3] = a3;
}

// Reconstructs the doubled ring exactly as n sequential pushes would leave it.
static void writeBackRing(std::vector<double>& line, int& pos, int E, const double* lin, int H, int n) noexcept {
    const int posN = int(((std::int64_t(pos) - n) % E + E) % E);
    for (int t = 0; t < E; ++t) {
        const double v = lin[H + n - 1 - t];
        const int slot = (posN + t) % E;
        line[std::size_t(slot)] = line[std::size_t(slot) + std::size_t(E)] = v;
    }
    pos = posN;
}

void PolyphaseHalfband2x::upBlock(const double* in, double* out, int n) noexcept {
    if (n < kBlockMin || n > linMaxN_ || E_ == 0) {
        for (int i = 0; i < n; ++i) up(in[i], out + 2 * i);
        return;
    }
    const int H = E_ - 1;
    double* lin = lin_.data();
    for (int k = 0; k < H; ++k) lin[H - 1 - k] = upLine_[std::size_t(upPos_ + k)];
    for (int i = 0; i < n; ++i) lin[H + i] = in[i];
    const double* h = even_.data();
    const int oddBack = (c_ - 1) / 2;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        double d[4];
        dotBlock4(h, E_, lin + H + i, d);
        for (int q = 0; q < 4; ++q) out[2 * (i + q)] = 2.0 * d[q];
    }
    for (; i < n; ++i) {
        double acc = 0;
        const double* p = lin + H + i;
        for (int j = 0; j < E_; ++j) acc += h[j] * p[-j];
        out[2 * i] = 2.0 * acc;
    }
    for (int q = 0; q < n; ++q) out[2 * q + 1] = lin[H + q - oddBack];
    writeBackRing(upLine_, upPos_, E_, lin, H, n);
}

void PolyphaseHalfband2x::downBlock(const double* in2, double* out, int n) noexcept {
    if (n < kBlockMin || n > linMaxN_ || E_ == 0) {
        for (int i = 0; i < n; ++i) out[i] = down(in2 + 2 * i);
        return;
    }
    const int H = E_ - 1;
    double* lin = lin_.data();
    double* oddTerm = lin + H + n;   // n values after the block samples
    for (int k = 0; k < H; ++k) lin[H - 1 - k] = dnEven_[std::size_t(dnPos_ + k)];
    const int d = (c_ + 1) / 2;
    for (int i = 0; i < n; ++i) {
        lin[H + i] = in2[2 * i];
        const int rd = oddPos_ + oddLen_ - d;
        const double odd = dnOdd_[std::size_t(rd >= oddLen_ ? rd - oddLen_ : rd)];
        dnOdd_[std::size_t(oddPos_)] = in2[2 * i + 1];
        oddPos_ = oddPos_ + 1 == oddLen_ ? 0 : oddPos_ + 1;
        oddTerm[i] = 0.5 * odd;
    }
    const double* h = even_.data();
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        double dd[4];
        dotBlock4(h, E_, lin + H + i, dd);
        for (int q = 0; q < 4; ++q) out[i + q] = dd[q] + oddTerm[i + q];
    }
    for (; i < n; ++i) {
        double acc = 0;
        const double* p = lin + H + i;
        for (int j = 0; j < E_; ++j) acc += h[j] * p[-j];
        out[i] = acc + oddTerm[i];
    }
    writeBackRing(dnEven_, dnPos_, E_, lin, H, n);
}

void PolyphaseHalfband2x::reset() noexcept {
    std::fill(upLine_.begin(), upLine_.end(), 0.0);
    std::fill(dnEven_.begin(), dnEven_.end(), 0.0);
    std::fill(dnOdd_.begin(), dnOdd_.end(), 0.0);
    upPos_ = dnPos_ = oddPos_ = 0;
}

void PolyphaseHalfband2x::getState(double* out) const noexcept {
    double* o = std::copy(upLine_.begin(), upLine_.end(), out);
    o = std::copy(dnEven_.begin(), dnEven_.end(), o);
    o = std::copy(dnOdd_.begin(), dnOdd_.end(), o);
    o[0] = double(upPos_); o[1] = double(dnPos_); o[2] = double(oddPos_);
}

bool PolyphaseHalfband2x::stateValid(const double* in) const noexcept {
    const double* pos = in + upLine_.size() + dnEven_.size() + dnOdd_.size();
    auto okPos = [](double v, int limit) { return v >= 0 && v < double(limit > 0 ? limit : 1) && v == std::floor(v); };
    return okPos(pos[0], E_) && okPos(pos[1], E_) && okPos(pos[2], oddLen_);
}

bool PolyphaseHalfband2x::setState(const double* in) noexcept {
    if (!stateValid(in)) return false;
    const std::size_t a = upLine_.size(), b = dnEven_.size(), c = dnOdd_.size();
    const double* pos = in + a + b + c;
    std::copy(in, in + a, upLine_.begin());
    std::copy(in + a, in + a + b, dnEven_.begin());
    std::copy(in + a + b, in + a + b + c, dnOdd_.begin());
    upPos_ = int(pos[0]); dnPos_ = int(pos[1]); oddPos_ = int(pos[2]);
    return true;
}

// ---- HalfbandIir2x ---------------------------------------------------------------------------
// Elliptic halfband allpass coefficients (Valenzuela/Constantinides form; series evaluation as in
// public-domain hiir by L. de Soras). The response is verified numerically, not trusted.

namespace {
void iirCoefs(int n, double tbw, double* out) {
    double k = std::tan((1 - tbw * 2) * kPi / 4);
    k *= k;
    const double kks = std::pow(1 - k * k, 0.25);
    const double e = 0.5 * (1 - kks) / (1 + kks), e4 = e * e * e * e;
    const double q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
    const int order = n * 2 + 1;
    for (int idx = 0; idx < n; ++idx) {
        const int c = idx + 1;
        double num = 0, den = 0, t;
        int i = 0, j = 1;
        do {
            t = std::pow(q, double(i * (i + 1))) * std::sin((i * 2 + 1) * c * kPi / order) * j;
            num += t; j = -j; ++i;
        } while (std::fabs(t) > 1e-100 && i < 1000);
        i = 1; j = -1;
        do {
            t = std::pow(q, double(i * i)) * std::cos(i * 2 * c * kPi / order) * j;
            den += t; j = -j; ++i;
        } while (std::fabs(t) > 1e-100 && i < 1000);
        const double ww = num * std::pow(q, 0.25) / (den + 0.5), w2 = ww * ww;
        const double x = std::sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2);
        out[idx] = (1 - x) / (1 + x);
    }
}

std::complex<double> iirResponse(const double* a, int n, double f) {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * f), z2 = z1 * z1;
    std::complex<double> p0 = 1, p1 = z1;
    for (int i = 0; i < n; ++i) {
        const std::complex<double> s = (a[i] + z2) / (1.0 + a[i] * z2);
        (i % 2 == 0 ? p0 : p1) *= s;
    }
    return 0.5 * (p0 + p1);
}
}  // namespace

bool HalfbandIir2x::design(double passEdge, double stopbandDb) {
    n_ = nA_ = nB_ = 0;
    reset();
    if (!(passEdge > 0 && passEdge < 0.25) || !(stopbandDb >= 20 && stopbandDb <= 200)) return false;
    const double tbw = 0.5 - 2.0 * passEdge, stop = 0.5 - passEdge;
    for (int n = 1; n <= kMaxCoefs; ++n) {
        double a[kMaxCoefs];
        iirCoefs(n, tbw, a);
        bool valid = true;
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(a[i]) || a[i] < 0 || a[i] >= 1) { valid = false; break; }
        if (!valid) continue;
        double worst = 0;
        for (int g = 0; g <= 4000; ++g) {
            const double magnitude = std::abs(iirResponse(a, n, stop + (0.5 - stop) * g / 4000.0));
            // std::max would silently ignore a NaN and could report a perfect stopband.
            if (!std::isfinite(magnitude)) { valid = false; break; }
            worst = std::max(worst, magnitude);
        }
        if (!valid) continue;
        if (-20 * std::log10(worst + 1e-300) >= stopbandDb) {
            std::copy(a, a + n, coef_);
            n_ = n; nA_ = (n + 1) / 2; nB_ = n / 2;
            for (int i = 0; i < n; ++i) {
                Ap* bank[2][2] = {{upA_, dnA_}, {upB_, dnB_}};
                bank[i % 2][0][i / 2].a = bank[i % 2][1][i / 2].a = a[i];
            }
            reset();
            return true;
        }
    }
    n_ = 0;
    return false;
}

double HalfbandIir2x::magnitude(double f) const noexcept { return n_ == 0 ? 0 : std::abs(iirResponse(coef_, n_, f)); }

double HalfbandIir2x::groupDelayDc() const noexcept {
    if (n_ == 0) return 0;
    const double f = 1e-5;
    return -std::arg(iirResponse(coef_, n_, f)) / (2 * kPi * f);
}

double HalfbandIir2x::path(Ap* s, int count, double x) noexcept {
    for (int i = 0; i < count; ++i) {
        const double y = s[i].a * (x - s[i].y1) + s[i].x1;
        s[i].x1 = x; s[i].y1 = y; x = y;
    }
    return x;
}

void HalfbandIir2x::up(double x, double* out2) noexcept {
    if (n_ == 0) { out2[0] = out2[1] = 0; return; }
    out2[0] = path(upA_, nA_, x);
    out2[1] = path(upB_, nB_, x);
}

double HalfbandIir2x::down(const double* in2) noexcept {
    if (n_ == 0) return 0;
    return 0.5 * (path(dnA_, nA_, in2[1]) + path(dnB_, nB_, in2[0]));
}

void HalfbandIir2x::reset() noexcept {
    for (Ap* bank : {upA_, upB_, dnA_, dnB_})
        for (int i = 0; i < kMaxCoefs / 2; ++i) bank[i].x1 = bank[i].y1 = 0;
}

void HalfbandIir2x::getState(double* out) const noexcept {
    for (const Ap* bank : {upA_, upB_, dnA_, dnB_})
        for (int i = 0; i < kMaxCoefs / 2; ++i) { *out++ = bank[i].x1; *out++ = bank[i].y1; }
}

void HalfbandIir2x::setState(const double* in) noexcept {
    for (Ap* bank : {upA_, upB_, dnA_, dnB_})
        for (int i = 0; i < kMaxCoefs / 2; ++i) { bank[i].x1 = *in++; bank[i].y1 = *in++; }
}

// ---- Oversampler -----------------------------------------------------------------------------

bool Oversampler::prepare(const Spec& s, int maxBlock) {
    factor_ = stages_ = maxBlock_ = 0;
    latency_ = 0;
    int st = 0;
    while ((1 << st) < s.factor && st < kMaxStages) ++st;
    if (s.factor < 1 || (1 << st) != s.factor || maxBlock < 1 || maxBlock > kMaxBlockSize) return false;
    if (!(s.passFraction > 0 && s.passFraction < 0.5)) return false;
    if (s.family != OsFamily::Fir && s.family != OsFamily::Iir) return false;
    const double maxStopband = s.family == OsFamily::Fir ? 250 : 200;
    if (!(s.stopbandDb >= 20 && s.stopbandDb <= maxStopband)) return false;
    double lat = 0;
    for (int i = 0; i < st; ++i) {
        const double pe = s.passFraction / double(2 << i);  // normalized to this stage's high rate
        if (s.family == OsFamily::Fir) {
            if (!fir_[i].design(pe, s.stopbandDb)) return false;
            fir_[i].reserveBlock(maxBlock << i);
            lat += fir_[i].center() / double(1 << i);
        } else {
            if (!iir_[i].design(pe, s.stopbandDb)) return false;
            // Decimator output is taken at the odd instant: one high-rate sample earlier than nominal.
            lat += (iir_[i].groupDelayDc() - 0.5) / double(1 << i);
        }
    }
    if (!std::isfinite(lat) || lat < 0) return false;
    bufA_.assign(size_t(maxBlock) * s.factor, 0.0);
    bufB_.assign(size_t(maxBlock) * s.factor, 0.0);
    family_ = s.family; factor_ = s.factor; stages_ = st; maxBlock_ = maxBlock; latency_ = lat;
    reset();
    return true;
}

void Oversampler::up(const double* in, double* out, int n) noexcept {
    if (factor_ == 0 || n <= 0 || n > maxBlock_) return;
    if (stages_ == 0) { std::copy(in, in + n, out); return; }
    const double* src = in;
    int len = n;
    for (int s = 0; s < stages_; ++s) {
        double* dst = (s == stages_ - 1) ? out : ((s % 2) ? bufB_.data() : bufA_.data());
        if (family_ == OsFamily::Fir) {
            fir_[s].upBlock(src, dst, len);
        } else {
            for (int i = 0; i < len; ++i) iir_[s].up(src[i], dst + 2 * i);
        }
        src = dst; len *= 2;
    }
}

void Oversampler::down(const double* in, double* out, int n) noexcept {
    if (factor_ == 0 || n <= 0 || n > maxBlock_) return;
    if (stages_ == 0) { std::copy(in, in + n, out); return; }
    const double* src = in;
    int len = n * factor_;
    for (int s = stages_ - 1, k = 0; s >= 0; --s, ++k) {
        double* dst = (s == 0) ? out : ((k % 2) ? bufB_.data() : bufA_.data());
        if (family_ == OsFamily::Fir) {
            fir_[s].downBlock(src, dst, len / 2);
        } else {
            for (int i = 0; i < len / 2; ++i) dst[i] = iir_[s].down(src + 2 * i);
        }
        src = dst; len /= 2;
    }
}

void Oversampler::reset() noexcept {
    for (int s = 0; s < kMaxStages; ++s) { fir_[s].reset(); iir_[s].reset(); }
}

int Oversampler::stateSize() const noexcept {
    int n = 0;
    for (int s = 0; s < stages_; ++s) n += family_ == OsFamily::Fir ? fir_[s].stateSize() : HalfbandIir2x::stateSize();
    return n;
}

void Oversampler::getState(double* out) const noexcept {
    for (int s = 0; s < stages_; ++s) {
        if (family_ == OsFamily::Fir) { fir_[s].getState(out); out += fir_[s].stateSize(); }
        else { iir_[s].getState(out); out += HalfbandIir2x::stateSize(); }
    }
}

bool Oversampler::stateValid(const double* in) const noexcept {
    const double* p = in;
    for (int s = 0; s < stages_ && family_ == OsFamily::Fir; ++s) {
        if (!fir_[s].stateValid(p)) return false;
        p += fir_[s].stateSize();
    }
    return true;
}

bool Oversampler::setState(const double* in) noexcept {
    if (!stateValid(in)) return false;  // validate every stage first
    const double* p = in;
    for (int s = 0; s < stages_; ++s) {
        if (family_ == OsFamily::Fir) { fir_[s].setState(p); p += fir_[s].stateSize(); }
        else { iir_[s].setState(p); p += HalfbandIir2x::stateSize(); }
    }
    return true;
}

}  // namespace cd::dsp
