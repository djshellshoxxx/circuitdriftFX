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

// ---- Halfband2x ------------------------------------------------------------------------------

bool Halfband2x::design(double passEdge, double stopbandDb) {
    if (!(passEdge > 0 && passEdge < 0.25) || !(stopbandDb >= 20 && stopbandDb <= 250)) return false;
    // Kaiser (S04): A = target + 10 dB margin; order = (A-7.95)/(14.36*df); beta = 0.1102(A-8.7).
    const double A = stopbandDb + 10.0, df = 0.5 - 2.0 * passEdge;
    const double beta = 0.1102 * (A - 8.7);
    int c = int(std::ceil((A - 7.95) / (14.36 * df) / 2.0));
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

double Halfband2x::magnitude(double f) const noexcept {
    double s = 0;
    for (int n = 0; n <= 2 * c_; ++n) s += h_[n] * std::cos(2 * kPi * f * (n - c_));
    return std::fabs(s);
}

double Halfband2x::dotEven(const std::vector<double>& line, int pos) const noexcept {
    const double* x = line.data() + pos;
    double acc = 0;
    for (int j = 0; j < E_; ++j) acc += even_[j] * x[j];
    return acc;
}

void Halfband2x::up(double x, double* out2) noexcept {
    upPos_ = (upPos_ == 0 ? E_ : upPos_) - 1;
    upLine_[upPos_] = upLine_[upPos_ + E_] = x;  // upLine_[upPos_+j] = x[m-j]
    out2[0] = 2.0 * dotEven(upLine_, upPos_);
    out2[1] = upLine_[upPos_ + (c_ - 1) / 2];    // 2*h[c]=1: odd phase is a pure delay
}

double Halfband2x::down(const double* in2) noexcept {
    // Output at even instant 2m: even taps on even samples, centre tap on odd sample u[2m-c].
    dnPos_ = (dnPos_ == 0 ? E_ : dnPos_) - 1;
    dnEven_[dnPos_] = dnEven_[dnPos_ + E_] = in2[0];
    const int d = (c_ + 1) / 2;
    const double odd = dnOdd_[(oddPos_ + oddLen_ - d) % oddLen_];
    dnOdd_[oddPos_] = in2[1];
    oddPos_ = (oddPos_ + 1) % oddLen_;
    return dotEven(dnEven_, dnPos_) + 0.5 * odd;
}

void Halfband2x::reset() noexcept {
    std::fill(upLine_.begin(), upLine_.end(), 0.0);
    std::fill(dnEven_.begin(), dnEven_.end(), 0.0);
    std::fill(dnOdd_.begin(), dnOdd_.end(), 0.0);
    upPos_ = dnPos_ = oddPos_ = 0;
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
    if (!(passEdge > 0 && passEdge < 0.25) || !(stopbandDb >= 20 && stopbandDb <= 200)) return false;
    const double tbw = 0.5 - 2.0 * passEdge, stop = 0.5 - passEdge;
    for (int n = 1; n <= kMaxCoefs; ++n) {
        double a[kMaxCoefs];
        iirCoefs(n, tbw, a);
        double worst = 0;
        for (int g = 0; g <= 4000; ++g)
            worst = std::max(worst, std::abs(iirResponse(a, n, stop + (0.5 - stop) * g / 4000.0)));
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

double HalfbandIir2x::magnitude(double f) const noexcept { return std::abs(iirResponse(coef_, n_, f)); }

double HalfbandIir2x::groupDelayDc() const noexcept {
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
    out2[0] = path(upA_, nA_, x);
    out2[1] = path(upB_, nB_, x);
}

double HalfbandIir2x::down(const double* in2) noexcept {
    return 0.5 * (path(dnA_, nA_, in2[1]) + path(dnB_, nB_, in2[0]));
}

void HalfbandIir2x::reset() noexcept {
    for (Ap* bank : {upA_, upB_, dnA_, dnB_})
        for (int i = 0; i < kMaxCoefs / 2; ++i) bank[i].x1 = bank[i].y1 = 0;
}

// ---- Oversampler -----------------------------------------------------------------------------

bool Oversampler::prepare(const Spec& s, int maxBlock) {
    int st = 0;
    while ((1 << st) < s.factor && st < kMaxStages) ++st;
    if (s.factor < 1 || (1 << st) != s.factor || maxBlock < 1) return false;
    if (!(s.passFraction > 0 && s.passFraction < 0.5)) return false;
    double lat = 0;
    for (int i = 0; i < st; ++i) {
        const double pe = s.passFraction / double(2 << i);  // normalized to this stage's high rate
        if (s.family == OsFamily::Fir) {
            if (!fir_[i].design(pe, s.stopbandDb)) return false;
            lat += fir_[i].center() / double(1 << i);
        } else {
            if (!iir_[i].design(pe, s.stopbandDb)) return false;
            // Decimator output is taken at the odd instant: one high-rate sample earlier than nominal.
            lat += (iir_[i].groupDelayDc() - 0.5) / double(1 << i);
        }
    }
    bufA_.assign(size_t(maxBlock) * s.factor, 0.0);
    bufB_.assign(size_t(maxBlock) * s.factor, 0.0);
    family_ = s.family; factor_ = s.factor; stages_ = st; maxBlock_ = maxBlock; latency_ = lat;
    reset();
    return true;
}

void Oversampler::up(const double* in, double* out, int n) noexcept {
    if (stages_ == 0) { std::copy(in, in + n, out); return; }
    const double* src = in;
    int len = n;
    for (int s = 0; s < stages_; ++s) {
        double* dst = (s == stages_ - 1) ? out : ((s % 2) ? bufB_.data() : bufA_.data());
        for (int i = 0; i < len; ++i) {
            if (family_ == OsFamily::Fir) fir_[s].up(src[i], dst + 2 * i);
            else iir_[s].up(src[i], dst + 2 * i);
        }
        src = dst; len *= 2;
    }
}

void Oversampler::down(const double* in, double* out, int n) noexcept {
    if (stages_ == 0) { std::copy(in, in + n, out); return; }
    const double* src = in;
    int len = n * factor_;
    for (int s = stages_ - 1, k = 0; s >= 0; --s, ++k) {
        double* dst = (s == 0) ? out : ((k % 2) ? bufB_.data() : bufA_.data());
        for (int i = 0; i < len / 2; ++i)
            dst[i] = (family_ == OsFamily::Fir) ? fir_[s].down(src + 2 * i) : iir_[s].down(src + 2 * i);
        src = dst; len /= 2;
    }
}

void Oversampler::reset() noexcept {
    for (int s = 0; s < kMaxStages; ++s) { fir_[s].reset(); iir_[s].reset(); }
}

}  // namespace cd::dsp
