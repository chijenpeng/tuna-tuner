#include "EpochTracker.h"

#include <algorithm>
#include <cmath>

namespace cody_pitch {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kLpcOrder = 24;
constexpr double kZffTauS = 0.05;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

double sincPi(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// numpy.hanning(m): symmetric.
double hannAt(int k, int m) {
    if (m == 1) return 1.0;
    return 0.5 + 0.5 * std::cos(kPi * static_cast<double>(1 - m + 2 * k) / static_cast<double>(m - 1));
}

} // namespace

double EpochTracker::period(int i) const noexcept {
    const int n = size();
    if (n < 2) return 0.0;
    if (i + 1 < n) return pos_[static_cast<size_t>(i + 1)] - pos_[static_cast<size_t>(i)];
    return pos_[static_cast<size_t>(n - 1)] - pos_[static_cast<size_t>(n - 2)];
}

void EpochTracker::prepare(int sampleRate, int maxWindowSamples) {
    fs_ = sampleRate;
    maxN_ = maxWindowSamples + 8;
    decim_ = std::max(1, static_cast<int>(std::lround(sampleRate / 12000.0)));
    // scipy.signal.resample_poly(x, 1, decim): firwin(2*10*decim+1, 1/decim, kaiser(5)).
    const int taps = 20 * decim_ + 1;
    const double alpha = 0.5 * (taps - 1), cutoff = 1.0 / decim_, beta = 5.0;
    kernel_.assign(static_cast<size_t>(taps), 0.0);
    double total = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double m = i - alpha;
        const double ideal = cutoff * sincPi(cutoff * m);
        const double r = m / alpha;
        const double win = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
        kernel_[static_cast<size_t>(i)] = ideal * win;
        total += ideal * win;
    }
    for (auto& v : kernel_) v /= total;

    lagWindow_.resize(kLpcOrder + 1);
    for (int k = 0; k <= kLpcOrder; ++k) {
        const double w = 2.0 * kPi * 40.0 / fs_ * k;
        lagWindow_[static_cast<size_t>(k)] = std::exp(-0.5 * w * w);
    }
    const int smoothLen = std::max(3, static_cast<int>(0.8e-3 * fs_));
    smooth_.resize(static_cast<size_t>(smoothLen));
    double s = 0.0;
    for (int k = 0; k < smoothLen; ++k) { smooth_[static_cast<size_t>(k)] = hannAt(k, smoothLen); s += smooth_[static_cast<size_t>(k)]; }
    for (auto& v : smooth_) v /= s;
    const int frameLen = static_cast<int>(0.025 * fs_);
    hann1200_.resize(static_cast<size_t>(frameLen));
    for (int k = 0; k < frameLen; ++k) hann1200_[static_cast<size_t>(k)] = hannAt(k, frameLen);

    const size_t n = static_cast<size_t>(maxN_);
    const size_t nd = n / static_cast<size_t>(decim_) + 4;
    xd_.assign(nd, 0.0); y_.assign(nd, 0.0); cs_.assign(nd + 1, 0.0); half_.assign(nd, 1);
    x64_.assign(n, 0.0); pre_.assign(n, 0.0); res_.assign(n, 0.0); sres_.assign(n, 0.0); per_.assign(n, 0.0);
    wframe_.assign(static_cast<size_t>(frameLen) + 8, 0.0);
    inVoiced_.assign(n, 0); frameOf_.assign(n, 0);
    runOfFrame_.assign(2048, 0);
    const size_t epochCap = n / 8 + 256;
    crossings_.reserve(nd); runEpochs_.reserve(epochCap); outPos_.reserve(epochCap);
    outRel_.reserve(epochCap); outFb_.reserve(epochCap);
    pos_.reserve(epochCap); reliable_.reserve(epochCap); fallback_.reserve(epochCap); run_.reserve(epochCap);
}

void EpochTracker::perSamplePeriod(int n, const double* t, const double* f0, int nf) {
    int nv = 0;
    for (int i = 0; i < nf; ++i) nv += f0[i] > 0.0;
    if (nv == 0) {
        for (int i = 0; i < n; ++i) per_[static_cast<size_t>(i)] = fs_ / 150.0;
        return;
    }
    // np.interp over the voiced frames only.
    auto nextVoiced = [&](int from) { while (from < nf && !(f0[from] > 0.0)) ++from; return from; };
    const int first = nextVoiced(0);
    int last = nf - 1; while (last >= 0 && !(f0[last] > 0.0)) --last;
    int cur = first, nxt = nextVoiced(first + 1);
    for (int i = 0; i < n; ++i) {
        const double ts = static_cast<double>(i) / fs_;
        double f;
        if (ts <= t[first]) f = f0[first];
        else if (ts >= t[last]) f = f0[last];
        else {
            while (nxt < nf && ts >= t[nxt]) { cur = nxt; nxt = nextVoiced(nxt + 1); }
            if (nxt >= nf) f = f0[cur];
            else {
                const double slope = (f0[nxt] - f0[cur]) / (t[nxt] - t[cur]);
                f = slope * (ts - t[cur]) + f0[cur];
            }
        }
        per_[static_cast<size_t>(i)] = fs_ / f;
    }
}

void EpochTracker::zffCrossings(const float* x, int n) {
    (void) x;
    const int nd = (n + decim_ - 1) / decim_;
    const int taps = static_cast<int>(kernel_.size());
    const int halfLen = (taps - 1) / 2;
    for (int m = 0; m < nd; ++m) {
        double sum = 0.0;
        const int centre = decim_ * m + halfLen;
        const int iLo = std::max(0, centre - (taps - 1));
        const int iHi = std::min(n - 1, centre);
        for (int idx = iLo; idx <= iHi; ++idx) sum += kernel_[static_cast<size_t>(centre - idx)] * x64_[static_cast<size_t>(idx)];
        xd_[static_cast<size_t>(m)] = sum;
    }
    const double fsd = static_cast<double>(fs_) / decim_;
    const double r = 1.0 - 1.0 / (kZffTauS * fsd);
    y_[0] = 0.0;
    for (int m = 1; m < nd; ++m) y_[static_cast<size_t>(m)] = xd_[static_cast<size_t>(m)] - xd_[static_cast<size_t>(m - 1)];
    for (int pass = 0; pass < 4; ++pass) {
        double state = 0.0;
        for (int m = 0; m < nd; ++m) {
            const double out = y_[static_cast<size_t>(m)] + state;
            y_[static_cast<size_t>(m)] = out;
            state = r * out;
        }
    }
    for (int m = 0; m < nd; ++m) {
        const double v = std::nearbyint(0.75 * per_[static_cast<size_t>(m * decim_)] / decim_);
        half_[static_cast<size_t>(m)] = std::max(1, static_cast<int>(v));
    }
    for (int pass = 0; pass < 2; ++pass) {
        cs_[0] = 0.0;
        for (int m = 0; m < nd; ++m) cs_[static_cast<size_t>(m + 1)] = cs_[static_cast<size_t>(m)] + y_[static_cast<size_t>(m)];
        for (int m = 0; m < nd; ++m) {
            const int h = half_[static_cast<size_t>(m)];
            const int lo = std::clamp(m - h, 0, nd), hi = std::clamp(m + h + 1, 0, nd);
            const double mean = (cs_[static_cast<size_t>(hi)] - cs_[static_cast<size_t>(lo)]) / std::max(hi - lo, 1);
            y_[static_cast<size_t>(m)] -= mean;
        }
    }
    crossings_.clear();
    for (int m = 0; m + 1 < nd; ++m) {
        const double a = y_[static_cast<size_t>(m)], b = y_[static_cast<size_t>(m + 1)];
        if (a < 0.0 && b >= 0.0) crossings_.push_back((m + (-a) / (b - a)) * decim_);
    }
}

void EpochTracker::lpcCoefficients(const double* frame, int length, double* a) const {
    // Autocorrelation LPC: Hann window, r0 white-noise correction, lag window,
    // Levinson-Durbin.
    double r[kLpcOrder + 1];
    const double* w = frame; // already windowed by the caller
    for (int k = 0; k <= kLpcOrder; ++k) {
        double sum = 0.0;
        for (int i = k; i < length; ++i) sum += w[i] * w[i - k];
        r[k] = sum;
    }
    for (int k = 0; k <= kLpcOrder; ++k) a[k] = 0.0;
    a[0] = 1.0;
    if (r[0] <= 1e-12) return;
    r[0] *= 1.0 + 1e-6;
    for (int k = 0; k <= kLpcOrder; ++k) r[k] *= lagWindow_[static_cast<size_t>(k)];
    double err = r[0];
    double tmp[kLpcOrder + 1];
    for (int i = 1; i <= kLpcOrder; ++i) {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const double k = -acc / err;
        if (!(std::abs(k) < 1.0) || !(err > 0.0)) { for (int j = 1; j <= kLpcOrder; ++j) a[j] = 0.0; return; }
        for (int j = 1; j < i; ++j) tmp[j] = a[j] + k * a[i - j];
        for (int j = 1; j < i; ++j) a[j] = tmp[j];
        a[i] = k;
        err *= 1.0 - k * k;
    }
}

void EpochTracker::lpcResidual(int n) {
    pre_[0] = x64_[0];
    for (int i = 1; i < n; ++i) pre_[static_cast<size_t>(i)] = x64_[static_cast<size_t>(i)] - 0.97 * x64_[static_cast<size_t>(i - 1)];
    const int frame = static_cast<int>(0.025 * fs_), hop = static_cast<int>(0.01 * fs_);
    for (int i = 0; i < n; ++i) res_[static_cast<size_t>(i)] = 0.0;
    double a[kLpcOrder + 1];
    for (int c = 0; c < n; c += hop) {
        const int lo = c, hi = std::min(n, c + hop);
        bool active = false;
        for (int i = lo; i < hi && !active; ++i) active = inVoiced_[static_cast<size_t>(i)] != 0;
        if (!active) continue;
        const int a0 = std::max(0, c + hop / 2 - frame / 2), a1 = std::min(n, c + hop / 2 + frame / 2);
        const int len = a1 - a0;
        for (int i = 0; i < len; ++i) {
            const double w = len == frame ? hann1200_[static_cast<size_t>(i)] : hannAt(i, len);
            wframe_[static_cast<size_t>(i)] = pre_[static_cast<size_t>(a0 + i)] * w;
        }
        lpcCoefficients(wframe_.data(), len, a);
        for (int i = lo; i < hi; ++i) {
            double sum = 0.0;
            for (int k = 0; k <= kLpcOrder && i - k >= 0; ++k) sum += a[k] * pre_[static_cast<size_t>(i - k)];
            res_[static_cast<size_t>(i)] = sum;
        }
    }
    // np.convolve(res, kernel, 'same'): out[i] = sum_k res[i - M/2 + k] * kernel[M-1-k]
    const int M = static_cast<int>(smooth_.size());
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        const int k0 = std::max(0, M / 2 - i), k1 = std::min(M, n - i + M / 2);
        for (int k = k0; k < k1; ++k) sum += res_[static_cast<size_t>(i - M / 2 + k)] * smooth_[static_cast<size_t>(M - 1 - k)];
        sres_[static_cast<size_t>(i)] = sum;
    }
    res_.swap(sres_);
}

void EpochTracker::analyse(const float* x, int n, const double* t, const double* f0, int nf) {
    pos_.clear(); reliable_.clear(); fallback_.clear(); run_.clear();
    stats_ = Stats {};
    if (n <= 0 || nf <= 0 || n > maxN_) return;
    if (static_cast<int>(runOfFrame_.size()) < nf) runOfFrame_.resize(static_cast<size_t>(nf));
    for (int i = 0; i < n; ++i) x64_[static_cast<size_t>(i)] = x[i];
    perSamplePeriod(n, t, f0, nf);
    const double hopS = nf > 1 ? t[1] - t[0] : 0.005;
    for (int i = 0; i < n; ++i) {
        const double v = std::nearbyint((static_cast<double>(i) / fs_ - t[0]) / hopS);
        const int fi = static_cast<int>(std::clamp(v, 0.0, static_cast<double>(nf - 1)));
        frameOf_[static_cast<size_t>(i)] = fi;
        inVoiced_[static_cast<size_t>(i)] = f0[fi] > 0.0;
    }
    int id = 0;
    runOfFrame_[0] = 0;
    for (int j = 1; j < nf; ++j) {
        if (f0[j] > 0.0 && !(f0[j - 1] > 0.0)) ++id;
        runOfFrame_[static_cast<size_t>(j)] = id;
    }
    auto runAt = [&](int sample) {
        return inVoiced_[static_cast<size_t>(sample)] ? runOfFrame_[static_cast<size_t>(frameOf_[static_cast<size_t>(sample)])] : -1;
    };
    zffCrossings(x, n);
    // keep crossings inside the window and in voiced samples
    size_t kept = 0;
    for (const double z : crossings_) {
        if (!(z >= 0.0 && z < n - 1)) continue;
        if (!inVoiced_[static_cast<size_t>(std::nearbyint(z))]) continue;
        crossings_[kept++] = z;
    }
    crossings_.resize(kept);
    stats_.zff = static_cast<int>(kept);
    lpcResidual(n);

    const auto periodAt = [&](double p) { return per_[static_cast<size_t>(std::clamp(static_cast<int>(p), 0, n - 1))]; };
    size_t zi = 0;
    while (zi < crossings_.size()) {
        const int rid = runAt(static_cast<int>(std::nearbyint(crossings_[zi])));
        size_t zj = zi;
        while (zj < crossings_.size() && runAt(static_cast<int>(std::nearbyint(crossings_[zj]))) == rid) ++zj;
        const size_t count = zj - zi;
        if (count >= 3) {
            // polarity of the run: sign of the summed strongest residual peak near each crossing
            runEpochs_.clear();
            double sumPicks = 0.0;
            for (size_t k = zi; k < zj; ++k) {
                const double p = crossings_[k];
                const int h = std::max(2, static_cast<int>(0.1 * periodAt(p)));
                const int a = std::max(static_cast<int>(p) - h, 0), b = std::min(static_cast<int>(p) + h + 1, n);
                int best = a; double bv = std::abs(res_[static_cast<size_t>(a)]);
                for (int i = a + 1; i < b; ++i) { const double v = std::abs(res_[static_cast<size_t>(i)]); if (v > bv) { bv = v; best = i; } }
                sumPicks += res_[static_cast<size_t>(best)];
            }
            const double s = sumPicks > 0.0 ? 1.0 : sumPicks < 0.0 ? -1.0 : 1.0;
            for (size_t k = zi; k < zj; ++k) {
                const double p = crossings_[k];
                const int h = std::max(2, static_cast<int>(0.1 * periodAt(p)));
                const int a = std::max(static_cast<int>(p) - h, 0), b = std::min(static_cast<int>(p) + h + 1, n);
                int best = a; double bv = s * res_[static_cast<size_t>(a)];
                for (int i = a + 1; i < b; ++i) { const double v = s * res_[static_cast<size_t>(i)]; if (v > bv) { bv = v; best = i; } }
                runEpochs_.push_back(static_cast<double>(best));
            }
            // regularisation
            outPos_.clear(); outRel_.clear(); outFb_.clear();
            outPos_.push_back(runEpochs_[0]); outRel_.push_back(1); outFb_.push_back(0);
            for (size_t k = 1; k < runEpochs_.size(); ++k) {
                const double q = runEpochs_[k];
                const double T = periodAt(outPos_.back());
                const double d = q - outPos_.back();
                if (d < 0.6 * T) { ++stats_.droppedClose; outRel_.back() = 0; continue; }
                if (d >= 0.75 * T && d <= 1.25 * T) { outPos_.push_back(q); outRel_.push_back(1); outFb_.push_back(0); continue; }
                ++stats_.irregular;
                const int kk = static_cast<int>(std::nearbyint(d / T));
                if (kk >= 2) {
                    const double step = d / kk;
                    for (int m = 1; m < kk; ++m) {
                        const double tgt = outPos_.back() + step;
                        const int h = std::max(2, static_cast<int>(0.15 * step));
                        const int a = std::max(static_cast<int>(tgt) - h, 0), b = std::min(static_cast<int>(tgt) + h + 1, n);
                        int best = a; double bv = s * res_[static_cast<size_t>(a)];
                        for (int i = a + 1; i < b; ++i) { const double v = s * res_[static_cast<size_t>(i)]; if (v > bv) { bv = v; best = i; } }
                        outPos_.push_back(static_cast<double>(best)); outRel_.push_back(0); outFb_.push_back(1);
                        ++stats_.inserted;
                    }
                    outPos_.push_back(q); outRel_.push_back(0); outFb_.push_back(0);
                } else {
                    outRel_.back() = 0;
                    outPos_.push_back(q); outRel_.push_back(0); outFb_.push_back(0);
                }
            }
            for (size_t k = 0; k < outPos_.size(); ++k) {
                pos_.push_back(outPos_[k]); reliable_.push_back(outRel_[k]); fallback_.push_back(outFb_[k]); run_.push_back(rid);
            }
        }
        zi = zj;
    }
    // reliability also needs both neighbour spacings regular
    const int total = size();
    if (total > 2) {
        std::vector<uint8_t>& rel = reliable_;
        // ok[i]: spacing between epoch i and i+1 is regular and within one run
        auto ok = [&](int i) {
            const double T = periodAt(pos_[static_cast<size_t>(i)]);
            const double d = pos_[static_cast<size_t>(i + 1)] - pos_[static_cast<size_t>(i)];
            return d > 0.75 * T && d < 1.25 * T && run_[static_cast<size_t>(i + 1)] == run_[static_cast<size_t>(i)];
        };
        for (int i = 0; i < total; ++i) {
            const bool edgeL = i == 0 || run_[static_cast<size_t>(i)] != run_[static_cast<size_t>(i - 1)];
            const bool edgeR = i == total - 1 || run_[static_cast<size_t>(i + 1)] != run_[static_cast<size_t>(i)];
            const bool prevOk = i > 0 && ok(i - 1);
            const bool nextOk = i < total - 1 && ok(i);
            const bool keep = (prevOk || edgeL) && (nextOk || edgeR) && !fallback_[static_cast<size_t>(i)];
            rel[static_cast<size_t>(i)] = rel[static_cast<size_t>(i)] && keep;
        }
    }
}

} // namespace cody_pitch
