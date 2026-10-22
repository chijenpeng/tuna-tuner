#include "WaveformRenderer.h"
#include "EpochTracker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cody_pitch {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMergeMin = 0.6, kMergeSame = 1.75, kMergeFill = 2.6, kStalePeriods = 5.0;
constexpr int kChunk = 2048;
constexpr double kMaxTrim = 0.03;
constexpr int64_t kNoLimit = std::numeric_limits<int64_t>::max() / 4;

size_t pow2AtLeast(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
int64_t roundHalfEven(double v) { return static_cast<int64_t>(std::nearbyint(v)); }

// Growing list of epochs with an absolute index; the oldest entries are dropped in bulk.
struct Seq {
    std::vector<double> v;
    int64_t base = 0;
    int64_t size() const noexcept { return base + static_cast<int64_t>(v.size()); }
    double operator[](int64_t i) const noexcept { return v[static_cast<size_t>(i - base)]; }
    void push(double x) { v.push_back(x); }
    void clear() { v.clear(); base = 0; }
    bool prune(size_t keep) {
        if (v.size() > 2 * keep) {
            const size_t drop = v.size() - keep;
            v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(drop));
            base += static_cast<int64_t>(drop);
            return true;
        }
        return false;
    }
    // first absolute index with value >= x (size() if none)
    int64_t lowerBound(double x) const noexcept {
        return base + (std::lower_bound(v.begin(), v.end(), x) - v.begin());
    }
    // argmin |v - t|, first on ties
    int64_t nearest(double t) const noexcept {
        const int64_t i = lowerBound(t);
        if (i <= base) return base;
        if (i >= size()) return size() - 1;
        return std::abs((*this)[i - 1] - t) <= std::abs((*this)[i] - t) ? i - 1 : i;
    }
};

struct Island {
    int id = 0;
    Seq e;
};

struct Target { double t; int64_t j; };

} // namespace

struct WaveformRenderer::Impl {
    // ---- configuration
    int fs, ctx, hop, ahead, gpad;
    double stride;
    int g5;
    Mode mode;
    bool trim, logEpochs;
    size_t pruneKeep = 4096;
    // ---- splice envelope correction
    int formant = 0;
    bool fNorm = true; double fHopMs = 5.0;
    bool perCycle = false, stepReuse = false, smoothNorm = false;
    std::vector<double> cents;
    int smoothSize = 3;
    std::vector<double> smoothScratch;
    int hopF = 240, winF = 960, lpcLen = 1200, halfL = 600;
    std::vector<double> yfinR, ybR, accF, wsF, rposR, hannWin, hannLpc, lpF, fSeg, fRes, fOut, fXp, fYp;
    int64_t lpDone = 0, cNext = 0, outPtr = 0;
    double dePrev = 0.0;
    std::vector<double> lpcWin;
    int64_t origin, streamEnd;

    // ---- rings
    std::vector<float> out, alignedOut;
    std::vector<double> posOut; // pure2: source position read for each output sample (measurement)
    size_t outCap;
    std::vector<float> xr;
    size_t xmask;
    int64_t xEnd = 0;
    std::vector<double> rfr;
    std::vector<double> srcR, desR;
    size_t fmask;
    int64_t frameLo = -1, frameEnd = 0, frameCommitted = std::numeric_limits<int64_t>::min();
    std::vector<double> acc, wsum, mask, ybuf;
    size_t amask;
    int64_t done;
    int64_t nextExpected, readyEnd;

    // ---- epochs and islands
    EpochTracker tracker;
    std::vector<Island> isls;
    int nextId = 0, lastOpen = -1, lastClosed = -1;
    bool hasE = false;
    double lastE = -1e9;
    int newestId = -1;

    struct State {
        bool active = false;
        int id = 0;
        // splice
        int64_t u = 0, n0 = 0;
        double p = 0.0;
        // psola
        bool tInit = false;
        double t = 0.0;
        int64_t jprev = 0;
        std::vector<Target> tgt;
        int64_t tbase = 0;     // absolute index of tgt[0]
        int64_t added = 0;     // absolute index of the next target without a grain
        double tg0 = 0.0, tg1 = 0.0; // first and second target (entry ramp / mask)
        // shared
        int64_t plateauTo = 0;
    } st;

    // ---- scratch
    std::vector<double> rbuf, pbuf, r2buf, p2buf, winT, winSrc;
    double ck[32], sk[32];

    // ---- per-frame diagnostics and splice jump intervals
    std::vector<int64_t> diagFrame;
    std::vector<int> diagIsland;
    std::vector<uint8_t> diagFlags; // bit0 active, bit1 jump
    std::vector<std::pair<int64_t, int64_t>> jumpSpans;
    int closedId = -1;
    int64_t closedEnd = 0;

    // ---- stats
    Stats s;
    std::vector<EpochRow> log;
    std::vector<uint32_t> hist; // 0.1 ms buckets
    double hopMsSum = 0.0;
    int hopIndex = 0;

    Impl(int rate, int context, int hopSamples, size_t ringCapacity, int64_t start, Mode m, bool tr,
         bool lg, int64_t end)
        : fs(rate), ctx(context), hop(hopSamples), mode(m), trim(tr), logEpochs(lg),
          origin(start), streamEnd(end) {
        if (rate < 16000 || ringCapacity < static_cast<size_t>(rate) || hop <= 0 || ctx <= 0 || start < 0)
            throw std::invalid_argument("Invalid waveform stream configuration");
        ahead = static_cast<int>(0.16 * fs);
        gpad = static_cast<int>(0.06 * fs);
        stride = fs / 200.0;
        g5 = (fs % 200 == 0) ? fs / 200 : 0;
        outCap = ringCapacity;
        out.assign(outCap, 0.0f);
        alignedOut.assign(outCap, 0.0f);
        posOut.assign(outCap, 0.0);
        const size_t xcap = pow2AtLeast(static_cast<size_t>(fs) * 3);
        xr.assign(xcap, 0.0f); xmask = xcap - 1;
        const size_t fcap = 4096;
        rfr.assign(fcap, 1.0); cents.assign(fcap, 0.0); srcR.assign(fcap, 0.0); desR.assign(fcap, 0.0);
        diagFrame.assign(fcap, -1); diagIsland.assign(fcap, -1); diagFlags.assign(fcap, 0); fmask = fcap - 1;
        const size_t acap = pow2AtLeast(static_cast<size_t>(fs) * 3 / 2);
        acc.assign(acap, 0.0); wsum.assign(acap, 0.0); mask.assign(acap, 0.0); ybuf.assign(acap, 0.0);
        amask = acap - 1;
        done = start; nextExpected = start; readyEnd = start;
        const int window = hop + 2 * ctx;
        tracker.prepare(fs, window);
        rbuf.reserve(16384); pbuf.reserve(16384); r2buf.reserve(16384); p2buf.reserve(16384);
        winT.assign(static_cast<size_t>(window / std::max(1.0, stride)) + 8, 0.0);
        winSrc.assign(winT.size(), 0.0);
        for (int i = 0; i < 32; ++i) {
            const double a = kPi * (i - 15) / 16.0;
            ck[i] = std::cos(a); sk[i] = std::sin(a);
        }
        hist.assign(2000, 0);
        rposR.assign(acap, std::numeric_limits<double>::quiet_NaN());
        isls.reserve(16);
        smoothScratch.reserve(512);
    }

    // ------------------------------------------------------------------ small helpers
    double xs(int64_t i) const noexcept {
        if (i < 0) i = 0;
        if (i >= xEnd) i = xEnd - 1;
        return static_cast<double>(xr[static_cast<size_t>(i) & xmask]);
    }
    bool inAcc(int64_t i) const noexcept { return i >= done && i < done + static_cast<int64_t>(amask + 1); }
    void setMask(int64_t i, double v) noexcept { if (inAcc(i)) mask[static_cast<size_t>(i) & amask] = v; }
    void fillMask(int64_t lo, int64_t hi, double v) noexcept {
        for (int64_t i = std::max(lo, done); i < hi && inAcc(i); ++i) mask[static_cast<size_t>(i) & amask] = v;
    }
    void setY(int64_t i, double v) noexcept { if (inAcc(i)) ybuf[static_cast<size_t>(i) & amask] = v; }

    double ratioAtPos(double t) const noexcept { // t in samples
        if (frameEnd <= 0 || frameLo < 0) return 1.0;
        const double fpos = t / stride;
        if (fpos <= static_cast<double>(frameLo)) return rfr[static_cast<size_t>(frameLo) & fmask];
        if (fpos >= static_cast<double>(frameEnd - 1)) return rfr[static_cast<size_t>(frameEnd - 1) & fmask];
        const int64_t j = static_cast<int64_t>(std::floor(fpos));
        const double fr = fpos - static_cast<double>(j);
        const size_t a = static_cast<size_t>(j) & fmask, b = static_cast<size_t>(j + 1) & fmask;
        if (!stepReuse) return rfr[a] + (rfr[b] - rfr[a]) * fr;
        // cents_step: linear in cents between frame centres, held (switch at the midpoint) across >= 50 cent steps
        const double ca = cents[a], cb = cents[b];
        if (std::abs(cb - ca) >= 50.0) return fr < 0.5 ? rfr[a] : rfr[b];
        return std::exp2((ca + (cb - ca) * fr) / 1200.0);
    }
    // a desired-F0 step of >= 50 cents between frames f and f+1, at the frame midpoint
    bool stepBetween(double t0, double t1) const noexcept { // t0 < step time <= t1 (samples)
        for (int64_t f = static_cast<int64_t>(std::floor(t0 / stride - 0.5)); f <= static_cast<int64_t>(std::ceil(t1 / stride)); ++f) {
            if (f < frameLo || f + 1 >= frameEnd) continue;
            const double mid = (static_cast<double>(f) + 0.5) * stride;
            if (!(mid > t0 && mid <= t1)) continue;
            const double d0 = desR[static_cast<size_t>(f) & fmask], d1 = desR[static_cast<size_t>(f + 1) & fmask];
            if (d0 > 0.0 && d1 > 0.0 && std::abs(1200.0 * std::log2(d1 / d0)) >= 50.0) return true;
        }
        return false;
    }

    // windowed-sinc reader (32 taps about floor(pos)); exact analytic taps, no table interpolation
    double sincRead(double pos, double c) const noexcept {
        const double fl = std::floor(pos);
        const int64_t i0 = static_cast<int64_t>(fl);
        const double f = pos - fl;
        const double cf = std::cos(kPi * f / 16.0), sf = std::sin(kPi * f / 16.0);
        double sum = 0.0;
        if (c == 1.0) {
            const double sinF = std::sin(kPi * f);
            for (int idx = 0; idx < 32; ++idx) {
                const int k = idx - 15;
                const double d = k - f;
                double sinc;
                if (std::abs(d) < 1e-2) sinc = d == 0.0 ? 1.0 : std::sin(kPi * d) / (kPi * d);
                else sinc = ((k & 1) ? sinF : -sinF) / (kPi * d);
                const double w = 0.5 + 0.5 * (ck[idx] * cf + sk[idx] * sf);
                sum += sinc * w * xs(i0 + k);
            }
            return sum;
        }
        const double A = kPi * c, sA = std::sin(A), cA = std::cos(A);
        const double sAf = std::sin(A * f), cAf = std::cos(A * f);
        double sKp[17], cKp[17];
        sKp[0] = 0.0; cKp[0] = 1.0;
        for (int k = 0; k < 16; ++k) {
            sKp[k + 1] = sKp[k] * cA + cKp[k] * sA;
            cKp[k + 1] = cKp[k] * cA - sKp[k] * sA;
        }
        for (int idx = 0; idx < 32; ++idx) {
            const int k = idx - 15;
            const double d = k - f;
            const double x = c * d;
            double sinc;
            if (std::abs(x) < 1e-2) sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            else {
                const double sA_k = k >= 0 ? sKp[k] : -sKp[-k];
                const double cA_k = k >= 0 ? cKp[k] : cKp[-k];
                sinc = (sA_k * cAf - cA_k * sAf) / (kPi * x);
            }
            const double w = 0.5 + 0.5 * (ck[idx] * cf + sk[idx] * sf);
            sum += c * sinc * w * xs(i0 + k);
        }
        return sum;
    }
    static double cutoffFor(double r) noexcept { return std::min(1.0, 1.0 / std::max(r, 1.0)); }

    // np.interp(p, mid, traw) with a monotone cursor
    double interpTraw(const Seq& e, double p, int64_t& cur) const noexcept {
        const int64_t K = e.size();
        const int64_t last = K - 2; // last mid/traw index
        auto mid = [&](int64_t k) { return 0.5 * (e[k + 1] + e[k]); };
        auto tr = [&](int64_t k) { return e[k + 1] - e[k]; };
        if (cur < e.base) cur = e.base;
        if (p <= mid(e.base)) return tr(e.base);
        if (p >= mid(last)) return tr(last);
        while (cur > e.base && mid(cur) > p) --cur;
        while (cur + 1 < last && mid(cur + 1) <= p) ++cur;
        const double slope = (tr(cur + 1) - tr(cur)) / (mid(cur + 1) - mid(cur));
        return slope * (p - mid(cur)) + tr(cur);
    }

    // ------------------------------------------------------------------ splice
    void fillRates(std::vector<double>& r, std::vector<double>& p, int64_t u0, int64_t cnt, double p0,
                   bool useTrim, int64_t trimUs, double trimDelta, const Seq* epochs = nullptr) {
        r.resize(static_cast<size_t>(cnt)); p.resize(static_cast<size_t>(cnt));
        double accum = 0.0;
        for (int64_t i = 0; i < cnt; ++i) {
            const int64_t u = u0 + i;
            double rate = ratioAtPos(static_cast<double>(u));
            if (useTrim) rate -= trimDelta * (u >= trimUs ? 1.0 : 0.0);
            r[static_cast<size_t>(i)] = rate;
            p[static_cast<size_t>(i)] = p0 + accum;
            accum += rate;
        }
        if (perCycle && epochs && epochs->size() >= 2) {
            // r = ratio at the epoch that starts the source cycle containing p (resample_splice._rp, SPLICE_PC)
            const Seq& e = *epochs;
            accum = 0.0;
            for (int64_t i = 0; i < cnt; ++i) {
                const int64_t u = u0 + i;
                int64_t k = (std::upper_bound(e.v.begin(), e.v.end(), p[static_cast<size_t>(i)]) - e.v.begin()) + e.base - 1;
                k = std::clamp<int64_t>(k, e.base, e.size() - 2);
                double rate = ratioAtPos(e[k]);
                if (useTrim) rate -= trimDelta * (u >= trimUs ? 1.0 : 0.0);
                r[static_cast<size_t>(i)] = rate;
                p[static_cast<size_t>(i)] = p0 + accum;
                accum += rate;
            }
        }
    }

    void emitPlain(int64_t u0, int64_t cnt, const std::vector<double>& p, const std::vector<double>& r) {
        for (int64_t i = 0; i < cnt; ++i)
            if (inAcc(u0 + i)) {
                setY(u0 + i, sincRead(p[static_cast<size_t>(i)], cutoffFor(r[static_cast<size_t>(i)])));
                rposR[static_cast<size_t>(u0 + i) & amask] = p[static_cast<size_t>(i)];
            }
    }

    // rs.advance: generate [st.u, uStop) of the island whose committed epochs are e
    int64_t jumpsThisCall = 0;
    void advance(State& state, const Seq& e, int64_t uStop, bool read, bool useTrim, int64_t trimUs,
                 double trimDelta, int64_t hard) {
        const int64_t K = e.size();
        const int64_t stop = std::min(uStop, hard);
        while (state.u < stop) {
            const int64_t L = std::min<int64_t>(stop - state.u, kChunk);
            fillRates(rbuf, pbuf, state.u, L, state.p, useTrim, trimUs, trimDelta, &e);
            int64_t hit = -1;
            if (K >= 3) {
                int64_t cur = e.base;
                const double eLim = e[K - 2];
                for (int64_t i = 0; i < L; ++i) {
                    const double pi = pbuf[static_cast<size_t>(i)];
                    if (!(pi < eLim)) continue;
                    const double Tl = interpTraw(e, pi, cur);
                    if (std::abs(pi - static_cast<double>(state.u + i)) > 0.5 * Tl) { hit = i; break; }
                }
            }
            int64_t j = 0;
            bool okHit = false;
            if (hit >= 0) {
                j = e.lowerBound(pbuf[static_cast<size_t>(hit)]);
                okHit = j >= 1 && j + 1 < K;
            }
            const auto plain = [&]() {
                if (read) emitPlain(state.u, L, pbuf, rbuf);
                state.p = pbuf[static_cast<size_t>(L - 1)] + rbuf[static_cast<size_t>(L - 1)];
                state.u += L;
            };
            if (!okHit) { plain(); continue; }
            const double trawJ = e[j + 1] - e[j];
            const int64_t L2 = hit + static_cast<int64_t>(6.0 * trawJ) + 48;
            fillRates(r2buf, p2buf, state.u, L2, state.p, useTrim, trimUs, trimDelta, &e);
            const int64_t kUs = std::lower_bound(p2buf.begin(), p2buf.end(), e[j] + 0.0) - p2buf.begin();
            const double rUs = r2buf[static_cast<size_t>(std::min<int64_t>(kUs, L2 - 1))];
            const int64_t Lx = std::max<int64_t>(8, roundHalfEven(1.0 * (e[j + 1] - e[j]) / std::max(rUs, 0.5)));
            if (kUs + Lx >= L2 || state.u + kUs + Lx > hard) { plain(); continue; }
            const double drift = pbuf[static_cast<size_t>(hit)] - static_cast<double>(state.u + hit);
            const double shift = drift > 0 ? -(e[j] - e[j - 1]) : (e[j + 1] - e[j]);
            if (read) {
                emitPlain(state.u, kUs, p2buf, r2buf);
                for (int64_t k = 0; k < Lx; ++k) {
                    const size_t q = static_cast<size_t>(kUs + k);
                    const double w = 0.5 - 0.5 * std::cos(kPi * (static_cast<double>(k) + 0.5) / static_cast<double>(Lx));
                    if (!inAcc(state.u + kUs + k)) continue;
                    const double cut = cutoffFor(r2buf[q]);
                    const double ya = sincRead(p2buf[q], cut), yb = sincRead(p2buf[q] + shift, cut);
                    setY(state.u + kUs + k, ya * (1.0 - w) + yb * w);
                    rposR[static_cast<size_t>(state.u + kUs + k) & amask] = p2buf[q];
                }
                ++jumpsThisCall;
                jumpSpans.emplace_back(state.u + kUs, state.u + kUs + Lx);
            }
            const size_t lastQ = static_cast<size_t>(kUs + Lx - 1);
            state.p = p2buf[lastQ] + r2buf[lastQ] + shift;
            state.u += kUs + Lx;
        }
    }

    // rs.finish_island; returns u_end
    int64_t finishSplice(State& state, const Seq& e) {
        const int64_t K = e.size();
        const int64_t uEnd = static_cast<int64_t>(std::ceil(e[K - 1]));
        const double Tend = e[K - 1] - e[K - 2];
        bool useTrim = false; int64_t trimUs = 0; double trimDelta = 0.0;
        if (trim && uEnd - state.u > 16) {
            State sim = state;
            advance(sim, e, uEnd, false, false, 0, 0.0, uEnd);
            const double D = sim.p - static_cast<double>(sim.u);
            int64_t m = static_cast<int64_t>(std::min(0.06 * fs, std::max(4.0 * Tend, std::abs(D) / kMaxTrim)));
            m = std::min(m, uEnd - state.u);
            if (m >= 8 && std::abs(D) / static_cast<double>(m) <= 2 * kMaxTrim) {
                useTrim = true; trimUs = uEnd - m; trimDelta = D / static_cast<double>(m);
            }
        }
        advance(state, e, uEnd, true, useTrim, trimUs, trimDelta, uEnd);
        return uEnd;
    }

    // ------------------------------------------------------------------ psola
    void addGrain(const Seq& e, double tk, int64_t jk) {
        const int64_t K = e.size();
        const double c = e[jk];
        const double lp = jk > 0 ? c - e[jk - 1] : e[jk + 1] - c;
        const double rp = jk + 1 < K ? e[jk + 1] - c : c - e[jk - 1];
        const int64_t a = roundHalfEven(c - lp), b = roundHalfEven(c + rp);
        const int64_t li = roundHalfEven(c) - a, ri = b - roundHalfEven(c);
        if (li < 2 || ri < 2 || a < 0 || b + 1 > xEnd) return;
        const int64_t o = roundHalfEven(tk) - li;
        if (o < 0) return;
        const int64_t len = li + ri + 1;
        for (int64_t k = 0; k < len; ++k) {
            const int64_t at = o + k;
            if (!inAcc(at)) continue;
            const double w = k < li ? 0.5 * (1.0 - std::cos(kPi * static_cast<double>(k) / static_cast<double>(li)))
                                    : 0.5 * (1.0 + std::cos(kPi * static_cast<double>(k - li) / static_cast<double>(ri)));
            const size_t slot = static_cast<size_t>(at) & amask;
            acc[slot] += static_cast<double>(xr[static_cast<size_t>(a + k) & xmask]) * w;
            wsum[slot] += w;
        }
    }

    void psolaGenerate(State& state, const Seq& e, bool ended, double tLimit) {
        const int64_t K = e.size();
        if (K < 3) return;
        const auto traw = [&](int64_t j) { return j < K - 1 ? e[j + 1] - e[j] : e[K - 1] - e[K - 2]; };
        if (!state.tInit) {
            state.tInit = true;
            state.t = e[0]; state.tgt.clear(); state.tbase = 0;
            state.tgt.push_back({ e[0], 0 });
            state.tg0 = e[0];
            state.tgt.reserve(16384);
            state.jprev = 0; state.added = 0;
        }
        double t = state.t;
        const double trawLast = e[K - 1] - e[K - 2];
        while (true) {
            const int64_t j = std::max(e.nearest(t), state.jprev - 1);
            if (!ended && (j + 2 >= K || t >= e[K - 1] - 1.5 * trawLast)) break;
            const double T = traw(j) / ratioAtPos(t);
            const double tNext = t + T;
            if (ended) { if (tNext > e[K - 1] + 0.5 * trawLast) break; }
            else if (tNext >= tLimit) break;
            int64_t jn = std::max(e.nearest(tNext), state.jprev - 1);
            if (stepReuse && stepBetween(t, tNext)) jn = state.jprev;
            state.tgt.push_back({ tNext, jn });
            if (state.tbase + static_cast<int64_t>(state.tgt.size()) == 2) state.tg1 = tNext;
            state.jprev = jn; t = tNext;
        }
        state.t = t;
    }
    int64_t tgtCount(const State& state) const noexcept { return state.tbase + static_cast<int64_t>(state.tgt.size()); }
    const Target& tgtAt(const State& state, int64_t i) const noexcept { return state.tgt[static_cast<size_t>(i - state.tbase)]; }

    void psolaClose(State& state, const Seq& e) {
        const int64_t n = tgtCount(state);
        if (n < 3) return;
        const int64_t first = std::max<int64_t>(2, n - 4);
        int64_t ke = first;
        double best = std::abs(tgtAt(state, first).t - e[tgtAt(state, first).j]);
        for (int64_t i = first + 1; i < n; ++i) {
            const double v = std::abs(tgtAt(state, i).t - e[tgtAt(state, i).j]);
            if (v < best) { best = v; ke = i; }
        }
        for (int64_t i = state.added; i <= ke; ++i) addGrain(e, tgtAt(state, i).t, tgtAt(state, i).j);
        state.added = ke + 1;
        const int64_t tl0 = roundHalfEven(tgtAt(state, ke - 1).t), tl1 = roundHalfEven(tgtAt(state, ke).t);
        const int64_t t1 = roundHalfEven(state.tg1);
        const int64_t lo = std::max(t1, state.plateauTo);
        if (tl0 > lo) fillMask(lo, tl0, 1.0);
        if (tl1 > tl0)
            for (int64_t i = tl0; i < tl1; ++i)
                setMask(i, 0.5 + 0.5 * std::cos(kPi * static_cast<double>(i - tl0) / static_cast<double>(tl1 - tl0)));
        state.plateauTo = tl1;
    }

    // ------------------------------------------------------------------ islands
    Island* findIsland(int id) {
        for (auto& i : isls) if (i.id == id) return &i;
        return nullptr;
    }
    void appendEpoch(double pos, int id) {
        if (isls.empty() || isls.back().id != id) { isls.emplace_back(); isls.back().id = id; isls.back().e.v.reserve(64); }
        isls.back().e.push(pos);
        hasE = true; newestId = id;
    }

    void closeState(int64_t a) {
        (void) a;
        if (pure) { pureClose(); return; }
        Island* isl = findIsland(st.id);
        if (isl) {
            const Seq& e = isl->e;
            if (mode == Mode::Psola) {
                psolaGenerate(st, e, true, 1e12);
                psolaClose(st, e);
                closedId = st.id; closedEnd = st.plateauTo;
            } else {
                jumpsThisCall = 0;
                const int64_t uEnd = finishSplice(st, e);
                s.jumps += static_cast<int>(jumpsThisCall);
                closedId = st.id; closedEnd = uEnd;
                const int64_t span = uEnd - st.n0;
                if (span >= 32) {
                    const int64_t K = e.size();
                    const int64_t Tx = std::clamp<int64_t>(roundHalfEven(e[K - 1] - e[K - 2]), 16, span / 2);
                    if (uEnd - Tx < st.plateauTo) s.trimLate += static_cast<int>(st.plateauTo - (uEnd - Tx));
                    const bool keepMin = st.plateauTo > uEnd - Tx;
                    for (int64_t i = 0; i < Tx; ++i) {
                        const double ramp = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(Tx));
                        const int64_t at = uEnd - Tx + i;
                        if (!inAcc(at)) continue;
                        double& m = mask[static_cast<size_t>(at) & amask];
                        m = keepMin ? std::min(m, ramp) : ramp;
                    }
                    fillMask(std::max(st.plateauTo, st.n0 + 1), uEnd - Tx, 1.0);
                }
            }
        }
        lastClosed = st.id;
        st.active = false;
        st.tgt.clear();
    }

    void commitEpochs(int hopIdx, int64_t wa, double commitEnd) {
        double last = hasE ? lastE : -1e9;
        for (int i = 0; i < tracker.size(); ++i) {
            const double p = tracker.position(i) + static_cast<double>(wa);
            const double pr = tracker.period(i);
            if (p <= last || p >= commitEnd) continue;
            if (p < static_cast<double>(origin)) continue;
            if (hasE && p - last < kMergeMin * pr) { ++s.droppedClose; continue; }
            bool same = false;
            if (hasE && p - last <= kMergeSame * pr) same = true;
            else if (hasE && p - last <= kMergeFill * pr) {
                const int kk = static_cast<int>(std::nearbyint((p - last) / pr));
                for (int m = 1; m < kk; ++m) {
                    const double fp = last + (p - last) * m / kk;
                    appendEpoch(fp, nextId);
                    if (logEpochs) log.push_back({ hopIdx, fp, pr, 0, nextId });
                }
                s.inserted += kk - 1;
                same = true;
            }
            if (hasE && !same) ++s.mergeNewIsland;
            if (hasE && same && nextId == lastClosed) { same = false; ++s.orphanRestart; }
            if (!same || !hasE) ++nextId;
            appendEpoch(p, nextId);
            lastE = p; last = p;
            if (logEpochs) log.push_back({ hopIdx, p, pr, tracker.reliable(i) ? 1 : 0, nextId });
        }
    }

    // ------------------------------------------------------------------ envelope correction
    void configureFormant() {
        hopF = std::max(1, static_cast<int>(std::lround(fs * fHopMs / 1000.0)));
        winF = 4 * std::max(1, static_cast<int>(std::lround(fs / 200.0)));
        lpcLen = static_cast<int>(0.025 * fs);
        halfL = lpcLen / 2;
        hannWin.resize(static_cast<size_t>(winF));
        for (int k = 0; k < winF; ++k) hannWin[static_cast<size_t>(k)] = 0.5 - 0.5 * std::cos(2.0 * kPi * k / winF);
        hannLpc.resize(static_cast<size_t>(lpcLen));
        for (int k = 0; k < lpcLen; ++k)
            hannLpc[static_cast<size_t>(k)] = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(1 - lpcLen + 2 * k) / (lpcLen - 1));
        const size_t cap = amask + 1;
        yfinR.assign(cap, 0.0); ybR.assign(cap, 0.0); accF.assign(cap, 0.0); wsF.assign(cap, 0.0);
        if (formant == 2) {
            const int n = 129;
            lpF.assign(n, 0.0);
            const double cutoff = std::min(0.99, 5000.0 / (fs / 2.0));
            double total = 0.0;
            for (int i = 0; i < n; ++i) {
                const double m = i - 0.5 * (n - 1);
                const double ideal = m == 0.0 ? cutoff : std::sin(kPi * cutoff * m) / (kPi * m);
                const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (n - 1));
                lpF[static_cast<size_t>(i)] = ideal * w; total += ideal * w;
            }
            for (auto& v : lpF) v /= total;
        }
        lpcWin.assign(static_cast<size_t>(lpcLen), 0.0);
        fSeg.reserve(winF + 8); fRes.reserve(winF + 8); fOut.reserve(winF + 8);
        fXp.assign(static_cast<size_t>(lpcLen) + 8, 0.0); fYp.assign(static_cast<size_t>(lpcLen) + 8, 0.0);
        lpDone = outPtr = origin;
        cNext = (origin + hopF - 1) / hopF;
    }

    // Autocorrelation LPC with r0 regularization, a 40 Hz Gaussian lag window, and Levinson recursion.
    void lpcOrder28(const double* frame, double* a) const {
        constexpr int order = 28;
        double r[order + 1];
        std::vector<double>& windowed = const_cast<Impl*>(this)->lpcWin;
        for (int i = 0; i < lpcLen; ++i) windowed[static_cast<size_t>(i)] = frame[i] * hannLpc[static_cast<size_t>(i)];
        for (int k = 0; k <= order; ++k) {
            double sum = 0.0;
            for (int i = k; i < lpcLen; ++i) sum += windowed[static_cast<size_t>(i)] * windowed[static_cast<size_t>(i - k)];
            r[k] = sum;
        }
        for (int k = 0; k <= order; ++k) a[k] = 0.0;
        a[0] = 1.0;
        if (r[0] <= 1e-12) return;
        r[0] *= 1.0 + 1e-6;
        for (int k = 0; k <= order; ++k) {
            const double wv = 2.0 * kPi * 40.0 / fs * k;
            r[k] *= std::exp(-0.5 * wv * wv);
        }
        double err = r[0], tmp[order + 1];
        for (int i = 1; i <= order; ++i) {
            double acc = r[i];
            for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
            const double k = -acc / err;
            if (!(std::abs(k) < 1.0)) { for (int j = 1; j <= order; ++j) a[j] = 0.0; return; }
            for (int j = 1; j < i; ++j) tmp[j] = a[j] + k * a[i - j];
            for (int j = 1; j < i; ++j) a[j] = tmp[j];
            a[i] = k;
            err *= 1.0 - k * k;
        }
    }

    double yfinAt(int64_t i) const noexcept { return i < origin || i >= done ? 0.0 : yfinR[static_cast<size_t>(i) & amask]; }
    double ybAt(int64_t i) const noexcept { return i < origin ? 0.0 : ybR[static_cast<size_t>(i) & amask]; }
    // source signal that is corrected towards: x, or its 5 kHz low-pass for mode 2
    double xbAt(int64_t i) const noexcept {
        if (i < 0) return 0.0;
        if (formant != 2) return i >= xEnd ? 0.0 : static_cast<double>(xr[static_cast<size_t>(i) & xmask]);
        double sum = 0.0;
        for (int j = 0; j < 129; ++j) {
            const int64_t at = i + 64 - j;
            if (at < 0 || at >= xEnd) continue;
            sum += lpF[static_cast<size_t>(j)] * static_cast<double>(xr[static_cast<size_t>(at) & xmask]);
        }
        return sum;
    }

    // Advance the correction over the newly finalised block. flush = end of a finite stream.
    void formantAdvance(bool flush) {
        constexpr double pre = 0.97;
        // 1. y' = y (mode 1) or its low-pass (mode 2), valid where the low-pass has its right neighbours
        const int64_t lpEnd = (flush || formant == 1) ? done : done - 64;
        for (int64_t n = lpDone; n < lpEnd; ++n) {
            double v;
            if (formant == 1) v = yfinAt(n);
            else {
                v = 0.0;
                for (int j = 0; j < 129; ++j) v += lpF[static_cast<size_t>(j)] * yfinAt(n + 64 - j);
            }
            ybR[static_cast<size_t>(n) & amask] = v;
        }
        lpDone = std::max(lpDone, lpEnd);
        // 2. frames
        const int64_t total = streamEnd >= 0 ? streamEnd : std::numeric_limits<int64_t>::max() / 4;
        double aOut[29], aSrc[29];
        while (true) {
            const int64_t c = cNext * hopF;
            if (flush ? c >= done : c + halfL > lpDone) break;
            const int64_t a0 = c - winF / 2;
            const int64_t lo = std::max<int64_t>(a0, origin), hi = std::min<int64_t>(a0 + winF, flush ? done : lpDone);
            ++cNext;
            if (hi - lo < 16) continue;
            const int len = static_cast<int>(hi - lo);
            fSeg.resize(static_cast<size_t>(len));
            for (int i = 0; i < len; ++i) fSeg[static_cast<size_t>(i)] = ybAt(lo + i) - pre * ybAt(lo + i - 1);
            // lfilter keeps the first sample's pre-emphasis against zero history at the stream start
            if (lo == 0) fSeg[0] = ybAt(0);
            double& rpSlot = rposR[static_cast<size_t>(c) & amask];
            const double rp = rpSlot;
            for (int i = 0; i < hopF; ++i) rposR[static_cast<size_t>(c + i) & amask] = std::numeric_limits<double>::quiet_NaN();
            bool correct = std::isfinite(rp) && c >= halfL && c + halfL <= total;
            int64_t rpi = 0;
            if (correct) {
                rpi = roundHalfEven(rp);
                correct = rpi - halfL >= 0 && rpi + halfL <= xEnd && c + halfL <= (flush ? done : lpDone);
            }
            if (correct) {
                for (int i = 0; i < lpcLen; ++i) {
                    fYp[static_cast<size_t>(i)] = ybAt(c - halfL + i) - pre * ybAt(c - halfL + i - 1);
                    fXp[static_cast<size_t>(i)] = xbAt(rpi - halfL + i) - pre * xbAt(rpi - halfL + i - 1);
                }
                if (c - halfL == 0) fYp[0] = ybAt(0);
                if (rpi - halfL == 0) fXp[0] = xbAt(0);
                lpcOrder28(fYp.data(), aOut);
                lpcOrder28(fXp.data(), aSrc);
                fRes.resize(static_cast<size_t>(len)); fOut.resize(static_cast<size_t>(len));
                double e0 = 1e-12, e1 = 1e-12;
                for (int i = 0; i < len; ++i) {
                    double v = 0.0;
                    for (int k = 0; k <= 28 && k <= i; ++k) v += aOut[k] * fSeg[static_cast<size_t>(i - k)];
                    fRes[static_cast<size_t>(i)] = v;
                    e0 += fSeg[static_cast<size_t>(i)] * fSeg[static_cast<size_t>(i)];
                }
                for (int i = 0; i < len; ++i) {
                    double v = fRes[static_cast<size_t>(i)];
                    for (int k = 1; k <= 28 && k <= i; ++k) v -= aSrc[k] * fOut[static_cast<size_t>(i - k)];
                    fOut[static_cast<size_t>(i)] = v;
                    e1 += v * v;
                }
                const double g = fNorm ? std::clamp(std::sqrt(e0 / e1), 0.25, 4.0) : 1.0;
                for (int i = 0; i < len; ++i) fSeg[static_cast<size_t>(i)] = fOut[static_cast<size_t>(i)] * g;
            }
            for (int i = 0; i < len; ++i) {
                const double w = hannWin[static_cast<size_t>(lo + i - a0)];
                const size_t slot = static_cast<size_t>(lo + i) & amask;
                accF[slot] += w * fSeg[static_cast<size_t>(i)];
                wsF[slot] += w;
            }
        }
        // 3. finished samples: every frame covering n has been added; de-emphasise and (mode 2) add the high band back
        const int64_t finished = flush ? done : std::min<int64_t>(done, cNext * hopF - winF / 2);
        for (; outPtr < finished; ++outPtr) {
            const size_t slot = static_cast<size_t>(outPtr) & amask;
            const double ypN = ybAt(outPtr) - pre * ybAt(outPtr - 1);
            const double v = wsF[slot] > 1e-6 ? accF[slot] / std::max(wsF[slot], 1e-6) : ypN;
            const double o = v + pre * dePrev;
            dePrev = o;
            const double y = formant == 2 ? o + (yfinAt(outPtr) - ybAt(outPtr)) : o;
            out[static_cast<size_t>(outPtr) % outCap] = static_cast<float>(y);
            accF[slot] = 0.0; wsF[slot] = 0.0;
        }
        readyEnd = outPtr;
    }

    // ------------------------------------------------------------------ pure splice
    // Pure resampling inside islands, drift absorbed only in unvoiced gaps. Output time u, source read position p,
    // drift d = p - u. The engine runs once per hop up to the block end b; G/Y/M are the dry stream, the island
    // signal and its mask (rings addressed like the other accumulators).
    struct PIsl { int id = 0; Seq e; bool closed = false, started = false; };
    bool pure = false;
    // Below 70 ms of context there is no room for the +-20 ms classification lookahead: a frame is classified from
    // the past only (rises already seen within the last 5 frames), so the class of the newest frames is provisional.
    bool causalClass = false;
    double maxDriftMs = 80.0, peakHint = -1.0, runPeak = 0.0;
    std::vector<double> gR, hpF, traceP, joinA, joinB, clsSeg, pA, pB, ncRef, ncCand;
    std::vector<PIsl> pq;
    int64_t eu = 0;                    // engine output position
    double ed = 0.0;                   // engine drift
    bool inIsland = false;
    int curId = -1, pureLastId = -1;
    struct PState { int64_t u = 0, n0 = 0; double p = 0.0; } pst;
    int64_t lastJumpU = -1000000000, islandEndU = -1000000000, entryEnd = 0, islU0 = 0;
    // frame classes
    std::vector<uint8_t> clsR, perR;
    std::vector<double> rmsR;
    int64_t rawN = 0, clsN = 0;        // frames with rms/periodicity computed, frames with a final class
    static constexpr uint8_t kSil = 0, kNoi = 1, kTra = 2;

    double floorNow() const noexcept { return std::max(0.0003, 0.003 * (peakHint > 0.0 ? peakHint : runPeak)); }
    double xz(int64_t i) const noexcept {
        if (i < 0 || i >= xEnd) return 0.0;
        return static_cast<double>(xr[static_cast<size_t>(i) & xmask]);
    }
    std::vector<double> gapPos;
    void setG(int64_t i, double v, double pos = 0.0) noexcept { if (inAcc(i)) { gR[static_cast<size_t>(i) & amask] = v; gapPos[static_cast<size_t>(i) & amask] = pos; } }

    static std::vector<double> makeLowpass(int taps, double cutoffHz, double rate) {
        std::vector<double> h(static_cast<size_t>(taps));
        const double cutoff = std::min(0.99, cutoffHz / (rate / 2.0));
        double total = 0.0;
        for (int i = 0; i < taps; ++i) {
            const double m = i - 0.5 * (taps - 1);
            const double ideal = m == 0.0 ? cutoff : std::sin(kPi * cutoff * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (taps - 1));
            h[static_cast<size_t>(i)] = ideal * w; total += ideal * w;
        }
        for (auto& v : h) v /= total;
        return h;
    }
    double hpAt(int64_t i) const noexcept { // x - LP3k(x), zero padded (np.convolve 'same')
        double sum = 0.0;
        for (int j = 0; j < 129; ++j) sum += hpF[static_cast<size_t>(j)] * xz(i + 64 - j);
        return xz(i) - sum;
    }
    double ncc(const double* a, const double* b, int n) const noexcept {
        double ma = 0.0, mb = 0.0;
        for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= n; mb /= n;
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (int i = 0; i < n; ++i) { const double x = a[i] - ma, y = b[i] - mb; ab += x * y; aa += x * x; bb += y * y; }
        const double d = std::sqrt(aa * bb);
        return d > 1e-12 ? ab / d : 0.0;
    }

    void classReset(int64_t firstSample) {
        const size_t cap = 4096;
        clsR.assign(cap, kNoi); perR.assign(cap, 0); rmsR.assign(cap, 0.0);
        rawN = clsN = (firstSample + 479) / 240 + 1;
        runPeak = 0.0;
    }
    // frames computed as data arrives; a class needs rms up to 5 frames later (about 32 ms ahead)
    void classUpdate(bool flush) {
        const int64_t N = streamEnd >= 0 ? std::min(xEnd, streamEnd) : xEnd;
        if (peakHint <= 0.0) {
            for (int64_t i = std::max<int64_t>(0, N - 4 * hop - 2 * ctx); i < N; ++i) runPeak = std::max(runPeak, std::abs(xz(i)));
        }
        const int64_t total = N / 240; // frames in a finite stream
        while (true) {
            const int64_t i = rawN;
            if (flush ? i >= total : i * 240 + 480 > N) break;
            const int64_t lo = std::max<int64_t>(i * 240 - 120, 0), hi = std::min<int64_t>(i * 240 + 360, N);
            double sum = 0.0;
            for (int64_t q = lo; q < hi; ++q) { const double v = xz(q); sum += v * v; }
            const double rms = std::sqrt(sum / 480.0);
            const size_t slot = static_cast<size_t>(i) & 4095;
            rmsR[slot] = rms;
            perR[slot] = 0;
            const int64_t a = i * 240 - 480;
            if (!(a < 0 || a + 960 > N || rms < 3.0 * floorNow())) {
                double mean = 0.0;
                for (int q = 0; q < 960; ++q) mean += xz(a + q);
                mean /= 960.0;
                std::vector<double>& seg = clsSeg;
                seg.resize(960);
                for (int q = 0; q < 960; ++q) seg[static_cast<size_t>(q)] = xz(a + q) - mean;
                double ac0 = 0.0;
                for (int q = 0; q < 960; ++q) ac0 += seg[static_cast<size_t>(q)] * seg[static_cast<size_t>(q)];
                double best = -1e300;
                for (int lag = 60; lag < 400; ++lag) {
                    double ac = 0.0;
                    for (int q = lag; q < 960; ++q) ac += seg[static_cast<size_t>(q)] * seg[static_cast<size_t>(q - lag)];
                    best = std::max(best, ac);
                }
                if (ac0 > 0.0 && best / ac0 > 0.6) perR[slot] = 1;
            }
            ++rawN;
        }
        const int64_t finalTo = (flush || causalClass) ? rawN : rawN - 5;
        const double fl = floorNow();
        while (clsN < finalTo) {
            const int64_t i = clsN;
            bool trans = false;
            for (int64_t q = std::max<int64_t>(i - 5, 0); q <= (causalClass ? i - 2 : i + 3) && !trans; ++q) { // frames whose rise marks i
                if (q + 2 >= rawN) break;
                trans = rmsR[static_cast<size_t>(q + 2) & 4095] > 2.0 * std::max(rmsR[static_cast<size_t>(q) & 4095], fl);
            }
            const size_t slot = static_cast<size_t>(i) & 4095;
            uint8_t c = rmsR[slot] < 3.0 * fl ? kSil : kNoi;
            if (trans || perR[slot]) c = kTra;
            clsR[slot] = c;
            ++clsN;
        }
    }
    // classes of the frames [a, b] (samples); anything not final yet protects the span
    void spanClasses(int64_t a, int64_t b, bool& anyTra, bool& allSil) const noexcept {
        const int64_t lo = std::max<int64_t>(a >= 0 ? a / 240 : -((-a + 239) / 240), 0);
        int64_t hi = b >= 0 ? b / 240 : 0;
        anyTra = false; allSil = true;
        if (hi > clsN - 1) { anyTra = streamEnd < 0; hi = clsN - 1; if (anyTra) { allSil = false; return; } }
        for (int64_t f = lo; f <= hi; ++f) {
            const uint8_t c = clsR[static_cast<size_t>(f) & 4095];
            if (c == kTra) anyTra = true;
            if (c != kSil) allSil = false;
        }
    }

    // resample_splice.advance in its pure form: ratio read at the read position (two fixed-point passes), seam only
    // when |drift| > max_drift, at the cheapest cycle within 10 cycles; two-period crossfade.
    void pureRates(std::vector<double>& r, std::vector<double>& p, int64_t u0, int64_t cnt, double p0) {
        fillRates(r, p, u0, cnt, p0, false, 0, 0.0, nullptr);
        for (int pass = 0; pass < 2; ++pass) {
            double accum = 0.0;
            for (int64_t i = 0; i < cnt; ++i) {
                const double rate = ratioAtPos(p[static_cast<size_t>(i)]);
                r[static_cast<size_t>(i)] = rate;
                p[static_cast<size_t>(i)] = p0 + accum;
                accum += rate;
            }
        }
    }
    void traceRecord(int64_t u0, const std::vector<double>& p, int64_t cnt) {
        for (int64_t i = 0; i < cnt; ++i) traceP[static_cast<size_t>(u0 + i) & amask] = p[static_cast<size_t>(i)];
    }
    void pureAdvance(PState& state, const Seq& e, int64_t uStop, int64_t& jumps) {
        const int64_t K = e.size();
        const double maxDrift = maxDriftMs * fs / 1000.0;
        while (state.u < uStop) {
            const int64_t L = std::min<int64_t>(uStop - state.u, kChunk);
            pureRates(rbuf, pbuf, state.u, L, state.p);
            int64_t hit = -1;
            if (K >= 3) {
                const double eLim = e[K - 2];
                for (int64_t i = 0; i < L; ++i) {
                    const double pi = pbuf[static_cast<size_t>(i)];
                    if (std::abs(pi - static_cast<double>(state.u + i)) > maxDrift && pi < eLim) { hit = i; break; }
                }
            }
            const auto plain = [&]() {
                emitPlain(state.u, L, pbuf, rbuf);
                traceRecord(state.u, pbuf, L);
                state.p = pbuf[static_cast<size_t>(L - 1)] + rbuf[static_cast<size_t>(L - 1)];
                state.u += L;
            };
            if (hit < 0) { plain(); continue; }
            const int64_t j = e.lowerBound(pbuf[static_cast<size_t>(hit)]);
            if (!(j >= 1 && j + 1 < K)) { plain(); continue; }
            const double trawJ = e[j + 1] - e[j];
            const int64_t L2 = hit + static_cast<int64_t>(6.0 * trawJ) + 48 + static_cast<int64_t>(13.0 * trawJ) + 64;
            pureRates(r2buf, p2buf, state.u, L2, state.p);
            // cheapest cycle among the next 10: rms(A) (1 + HF fraction) max(1 - NCC(A, B), 0.02)
            double bestCost = 0.0; bool have = false;
            int64_t bKu = 0, bLx = 0, bKc = 0; double bSdir = 0.0;
            std::vector<double>& pBbest = joinB; std::vector<double>& pBtry = joinA;
            const int64_t jjEnd = std::min<int64_t>(j + 11, K - 2);
            for (int64_t jj = j; jj < jjEnd; ++jj) {
                const int64_t ku = std::lower_bound(p2buf.begin(), p2buf.end(), e[jj]) - p2buf.begin();
                const double trawJJ = e[jj + 1] - e[jj];
                const int64_t lx = std::max<int64_t>(8, roundHalfEven(2.0 * trawJJ / std::max(r2buf[static_cast<size_t>(std::min<int64_t>(ku, L2 - 1))], 0.5)));
                if (ku + lx >= L2 - 5) break;
                const double Dc = p2buf[static_cast<size_t>(ku)] - static_cast<double>(state.u + ku);
                int64_t kc = roundHalfEven(std::abs(Dc) / std::max(trawJJ, 1.0));
                const double sdir = Dc > 0 ? -1.0 : 1.0;
                kc = sdir < 0 ? std::min<int64_t>(kc, jj) : std::min<int64_t>(kc, K - 2 - jj);
                if (kc < 1) continue;
                pBtry.resize(static_cast<size_t>(lx));
                double sa = 0.0, sb = 0.0, sh = 0.0, ma = 0.0, mb = 0.0;
                std::vector<double>& A = pA; std::vector<double>& B = pB;
                A.resize(static_cast<size_t>(lx)); B.resize(static_cast<size_t>(lx));
                for (int64_t k = 0; k < lx; ++k) {
                    const double pa = p2buf[static_cast<size_t>(ku + k)];
                    // position of the same path moved kc epochs: interp(interp(pa; e -> index) + sdir*kc; index -> e)
                    int64_t q = (std::upper_bound(e.v.begin(), e.v.end(), pa) - e.v.begin()) + e.base - 1;
                    double qi;
                    if (pa <= e[e.base]) qi = static_cast<double>(e.base);
                    else if (pa >= e[K - 1]) qi = static_cast<double>(K - 1);
                    else { q = std::clamp<int64_t>(q, e.base, K - 2); qi = q + (pa - e[q]) / (e[q + 1] - e[q]); }
                    double t = qi + sdir * kc, pb;
                    if (t <= e.base) pb = e[e.base];
                    else if (t >= K - 1) pb = e[K - 1];
                    else { const int64_t f = static_cast<int64_t>(std::floor(t)); pb = e[f] + (e[f + 1] - e[f]) * (t - f); }
                    pBtry[static_cast<size_t>(k)] = pb;
                    const int64_t ia = std::clamp<int64_t>(roundHalfEven(pa), 0, xEnd - 1), ib = std::clamp<int64_t>(roundHalfEven(pb), 0, xEnd - 1);
                    A[static_cast<size_t>(k)] = xz(ia); B[static_cast<size_t>(k)] = xz(ib);
                    sa += A[static_cast<size_t>(k)] * A[static_cast<size_t>(k)]; sb += B[static_cast<size_t>(k)] * B[static_cast<size_t>(k)];
                    const double hp = hpAt(ia); sh += hp * hp; ma += A[static_cast<size_t>(k)]; mb += B[static_cast<size_t>(k)];
                }
                const double ra = std::sqrt(sa / lx) + 1e-9;
                const double hfe = (sh / lx) / (sa / lx + 1e-12);
                ma /= lx; mb /= lx;
                double ab = 0.0, aa = 0.0, bb = 0.0;
                for (int64_t k = 0; k < lx; ++k) { const double x = A[static_cast<size_t>(k)] - ma, y = B[static_cast<size_t>(k)] - mb; ab += x * y; aa += x * x; bb += y * y; }
                const double nccv = ab / (std::sqrt(aa * bb) + 1e-12);
                const double cost = ra * (1.0 + std::min(hfe, 1.0)) * std::max(1.0 - nccv, 0.02);
                if (!have || cost < bestCost) {
                    have = true; bestCost = cost; bKu = ku; bLx = lx; bKc = kc; bSdir = sdir;
                    pBbest.assign(pBtry.begin(), pBtry.begin() + lx);
                }
            }
            if (!have) { plain(); continue; }
            emitPlain(state.u, bKu, p2buf, r2buf);
            for (int64_t k = 0; k < bLx; ++k) {
                const size_t q = static_cast<size_t>(bKu + k);
                const double w = 0.5 - 0.5 * std::cos(kPi * (static_cast<double>(k) + 0.5) / static_cast<double>(bLx));
                if (!inAcc(state.u + bKu + k)) continue;
                const double cut = cutoffFor(r2buf[q]);
                const double ya = sincRead(p2buf[q], cut), yb = sincRead(pBbest[static_cast<size_t>(k)], cut);
                setY(state.u + bKu + k, ya * (1.0 - w) + yb * w);
            }
            ++jumps;
            s.pureForced += 1; jumpSpans.emplace_back(state.u + bKu, state.u + bKu + bLx);
            traceRecord(state.u, p2buf, bKu + bLx);
            state.p = pBbest[static_cast<size_t>(bLx - 1)] + r2buf[static_cast<size_t>(bKu + bLx - 1)];
            state.u += bKu + bLx;
            (void) bKc; (void) bSdir;
        }
    }

    PIsl* nextIsland() {
        for (auto& p : pq) if (!p.started && p.e.size() >= 4) return &p;
        return nullptr;
    }
    PIsl* currentIsland() {
        for (auto& p : pq) if (p.id == curId) return &p;
        return nullptr;
    }

    void gapChunk(int64_t b, PIsl* nxt) {
        const int64_t n = xEnd;
        const int64_t s0 = roundHalfEven(static_cast<double>(eu) + ed);
        int64_t u0 = 0; bool haveU0 = false;
        if (nxt) { u0 = static_cast<int64_t>(std::ceil(nxt->e[0])) - roundHalfEven(ed); haveU0 = true; }
        int64_t L = std::min<int64_t>(240, std::max<int64_t>(b - eu, 1));
        if (haveU0) L = std::min<int64_t>(L, std::max<int64_t>(u0 - eu, 1));
        const int64_t J = static_cast<int64_t>(std::min(std::abs(ed), 240.0));
        const int64_t sgn = ed > 0 ? -1 : 1;
        const bool guardIn = nxt && static_cast<int64_t>(std::ceil(nxt->e[0])) - s0 <= 960 + 480;
        const bool guardOut = eu - islandEndU < 480;
        bool did = false;
        if (std::abs(ed) >= 48 && !guardIn && !guardOut && 0 <= s0 - 240 - 48 && s0 + 240 + 48 + 480 < n) {
            bool anyTra, allSil;
            spanClasses(s0 - 288, s0 + J + 480 + 48 + 240, anyTra, allSil);
            if (!anyTra) {
                if (allSil) {
                    const int64_t Lj = 96;
                    const int64_t s1 = s0 + sgn * J;
                    for (int64_t k = 0; k < Lj; ++k) {
                        const double w = 0.5 - 0.5 * std::cos(kPi * (static_cast<double>(k) + 0.5) / Lj);
                        setG(eu + k, xz(s0 + k) * (1.0 - w) + xz(s1 + k) * w, static_cast<double>(s0 + k));
                    }
                    ++s.pureSilence; jumpSpans.emplace_back(eu, eu + Lj);
                    ed = static_cast<double>(s1 - eu); eu += Lj; did = true;
                } else if (eu - lastJumpU >= 480) {
                    std::vector<double>& ref = ncRef; std::vector<double>& cand = ncCand;
                    ref.resize(240); cand.resize(240);
                    for (int k = 0; k < 240; ++k) ref[static_cast<size_t>(k)] = xz(s0 + k);
                    int best = 0; double bs = -2.0;
                    for (int off = -48; off <= 48; off += 2) {
                        const int64_t c0 = s0 + sgn * J + off;
                        if (c0 < 0 || c0 + 240 > n) continue;
                        for (int k = 0; k < 240; ++k) cand[static_cast<size_t>(k)] = xz(c0 + k);
                        const double sc = ncc(cand.data(), ref.data(), 240) - 0.001 * std::abs(off) / 48.0;
                        if (sc > bs) { bs = sc; best = off; }
                    }
                    const int64_t s1 = s0 + sgn * J + best;
                    for (int64_t k = 0; k < 240; ++k) {
                        const double w = 0.5 - 0.5 * std::cos(kPi * (static_cast<double>(k) + 0.5) / 240.0);
                        setG(eu + k, xz(s0 + k) * (1.0 - w) + xz(s1 + k) * w, static_cast<double>(s0 + k));
                    }
                    ++s.pureWsola; jumpSpans.emplace_back(eu, eu + 240);
                    ed = static_cast<double>(s1 - eu); lastJumpU = eu; eu += 240; did = true;
                }
            }
        }
        if (!did) {
            for (int64_t k = 0; k < L; ++k) {
                const int64_t src = s0 + k;
                setG(eu + k, (s0 >= 0 && s0 < n && src < n) ? xz(src) : 0.0, static_cast<double>(src));
            }
            eu += L;
        }
    }

    void startIsland(PIsl& isl) {
        const Seq& e = isl.e;
        const int64_t n0 = static_cast<int64_t>(std::ceil(e[0]));
        isl.started = true; curId = isl.id; inIsland = true;
        pst.u = eu; pst.p = static_cast<double>(n0); pst.n0 = n0;
        ed = static_cast<double>(n0 - eu);
        const int64_t Ti = std::clamp<int64_t>(roundHalfEven(e[1] - e[0]), 16, 1024);
        for (int64_t k = 0; k < Ti; ++k) {
            setG(eu + k, xz(std::clamp<int64_t>(n0 + k, 0, xEnd - 1)), static_cast<double>(n0 + k));
            setMask(eu + k, 0.5 - 0.5 * std::cos(kPi * static_cast<double>(k) / Ti));
        }
        entryEnd = eu + Ti; islU0 = eu;
        s.maxOffsetSamples = std::max(s.maxOffsetSamples, std::abs(ed));
    }

    void finishIsland() {
        PIsl* isl = currentIsland();
        const Seq& e = isl->e;
        const int64_t K = e.size();
        const double target = e[K - 1];
        const int64_t mFrom = pst.u;
        int guard = 0;
        int64_t jumps = 0;
        while (pst.p < target && guard < 4096) { pureAdvance(pst, e, pst.u + 2048, jumps); ++guard; }
        // first sample whose read position reaches the island end (the trace ring holds the recent past)
        int64_t u1 = pst.u - 1;
        const int64_t floorU = std::max<int64_t>(islU0, pst.u - static_cast<int64_t>(amask));
        if (traceP[static_cast<size_t>(u1) & amask] < target) u1 = pst.u - 1; // reached only at the end (loop guard)
        while (u1 - 1 >= floorU && traceP[static_cast<size_t>(u1 - 1) & amask] >= target) --u1;
        const double p1 = traceP[static_cast<size_t>(u1) & amask];
        fillMask(std::max(mFrom, entryEnd), u1, 1.0);
        const int64_t span = u1 - islU0;
        if (span >= 32) {
            const int64_t Tx = std::clamp<int64_t>(roundHalfEven(e[K - 1] - e[K - 2]), 16, span / 2);
            for (int64_t k = 0; k < Tx; ++k) {
                const int64_t at = u1 - Tx + k;
                if (!inAcc(at)) { if (at < done) ++s.pureLateExit; continue; }
                setG(at, xz(std::clamp<int64_t>(roundHalfEven(p1 + static_cast<double>(at - (u1 - 1))), 0, xEnd - 1)), p1 + static_cast<double>(at - (u1 - 1)));
                double& m = mask[static_cast<size_t>(at) & amask];
                m = std::min(m, 0.5 + 0.5 * std::cos(kPi * static_cast<double>(k) / Tx));
            }
            for (int64_t k = 0; k < Tx; ++k) setMask(u1 + k, 0.0);
        }
        ed = p1 - static_cast<double>(u1); eu = u1; islandEndU = u1;
        s.maxOffsetSamples = std::max(s.maxOffsetSamples, std::abs(ed));
        inIsland = false; pureLastId = curId; curId = -1;
        // forget finished islands
        const int doneId = isl->id;
        pq.erase(std::remove_if(pq.begin(), pq.end(), [doneId](const PIsl& p) { return p.id == doneId; }), pq.end());
    }

    void pureEngineAdvance(int64_t b) {
        int guard = 0;
        while (eu < b && guard++ < 100000) {
            if (inIsland) {
                PIsl* isl = currentIsland();
                if (!isl) { inIsland = false; continue; }
                if (isl->closed) { finishIsland(); continue; }
                const int64_t before = pst.u;
                int64_t jumps = 0;
                pureAdvance(pst, isl->e, b, jumps);
                if (pst.u > before) fillMask(std::max(before, entryEnd), pst.u, 1.0);
                eu = pst.u;
                s.maxOffsetSamples = std::max(s.maxOffsetSamples, std::abs(pst.p - static_cast<double>(pst.u)));
                if (eu < b) { ++s.underruns; break; }
            } else {
                PIsl* nxt = nextIsland();
                if (nxt) {
                    const int64_t n0 = static_cast<int64_t>(std::ceil(nxt->e[0]));
                    const int64_t u0 = n0 - roundHalfEven(ed);
                    if (eu >= u0) { startIsland(*nxt); continue; }
                }
                gapChunk(b, nxt);
            }
        }
    }

    void configurePure() {
        const size_t cap = amask + 1;
        gR.assign(cap, 0.0); traceP.assign(cap, 0.0); gapPos.assign(cap, 0.0);
        hpF = makeLowpass(129, 3000.0, fs);
        causalClass = ctx * 1000.0 < 70.0 * fs;
        joinA.reserve(8192); joinB.reserve(8192); pA.reserve(8192); pB.reserve(8192);
        ncRef.reserve(256); ncCand.reserve(256); clsSeg.reserve(1024);
        pq.reserve(8);
        eu = origin; ed = 0.0; inIsland = false; curId = -1;
        classReset(std::max<int64_t>(0, origin - ctx));
    }
    void pureClose() {
        Island* isl = findIsland(st.id);
        for (auto& p : pq) if (p.id == st.id) {
            if (isl) for (int64_t q = p.e.size(); q < isl->e.size(); ++q) p.e.push(isl->e[q]);
            p.closed = true;
        }
        lastClosed = st.id;
        st.active = false;
    }
    void pureSync(Island* isl) {
        for (auto& p : pq) if (p.id == st.id)
            for (int64_t q = p.e.size(); q < isl->e.size(); ++q) p.e.push(isl->e[q]);
    }

    // ------------------------------------------------------------------ hop
    void push(const float* win, size_t winSize, const double* srcF, const double* desF, int nFr, int64_t next) {
        if (next != nextExpected || winSize != static_cast<size_t>(hop + 2 * ctx))
            throw std::logic_error("Waveform analysis hops must be contiguous");
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t start = next - ctx;
        for (size_t i = 0; i < winSize; ++i) {
            const int64_t idx = start + static_cast<int64_t>(i);
            if (idx >= 0) xr[static_cast<size_t>(idx) & xmask] = win[i];
        }
        const int64_t windowEnd = start + static_cast<int64_t>(winSize);
        xEnd = streamEnd >= 0 ? std::min(windowEnd, streamEnd) : windowEnd;

        // frames: left context only until committed, centre committed, lookahead tentative
        const int64_t f0 = std::llround(static_cast<double>(start) * 200.0 / fs);
        const int firstCentral = static_cast<int>(std::lround(ctx * 200.0 / fs));
        const int perHop = static_cast<int>(std::lround(hop * 200.0 / fs));
        const int nWin = static_cast<int>(std::ceil(static_cast<double>(winSize) / stride));
        const int nUse = std::min(nFr, nWin);
        for (int i = 0; i < nUse; ++i) {
            const int64_t f = f0 + i;
            if (f < 0) continue;
            if (i < firstCentral && f < frameCommitted) continue;
            const size_t slot = static_cast<size_t>(f) & fmask;
            const double sv = srcF[i], dv = desF[i];
            srcR[slot] = sv;
            const double rho = (sv > 0.0 && dv > 0.0) ? dv / std::max(sv, 1e-9) : 1.0;
            rfr[slot] = rho;
            cents[slot] = 1200.0 * std::log2(rho);
            desR[slot] = dv;
        }
        if (frameLo < 0) frameLo = std::max<int64_t>(0, f0);
        frameEnd = f0 + nUse;
        frameCommitted = std::max(frameCommitted, f0 + firstCentral + perHop);

        const int64_t a = next;
        const int64_t b = streamEnd >= 0 ? std::min<int64_t>(next + hop, streamEnd) : next + hop;
        if (b <= a) { nextExpected += hop; return; }
        int64_t wa = std::max<int64_t>(0, a - ctx);
        if (g5 > 0) wa -= wa % g5;
        const int64_t wb = streamEnd >= 0 ? std::min<int64_t>(streamEnd, b + ctx) : b + ctx;
        const bool lastBlock = streamEnd >= 0 && b >= streamEnd;
        const double commitEnd = (streamEnd >= 0 && (lastBlock || wb >= streamEnd))
            ? static_cast<double>(streamEnd) : static_cast<double>(std::min<int64_t>(wb, b + ahead));
        done = a; // writes below the block start are never audible
        if (pure) classUpdate(lastBlock);

        if (wb - wa > static_cast<int64_t>(0.06 * fs)) {
            const int64_t i0 = static_cast<int64_t>(std::floor(static_cast<double>(wa) / stride + 1e-9));
            const int64_t i1 = std::min<int64_t>(frameEnd, static_cast<int64_t>(std::ceil(static_cast<double>(wb) / stride - 1e-9)));
            const int nf = static_cast<int>(std::min<int64_t>(i1 - i0, static_cast<int64_t>(winT.size())));
            if (nf > 0) {
                for (int i = 0; i < nf; ++i) {
                    winT[static_cast<size_t>(i)] = (static_cast<double>(i0 + i) * stride - static_cast<double>(wa)) / fs;
                    winSrc[static_cast<size_t>(i)] = srcR[static_cast<size_t>(i0 + i) & fmask];
                }
                tracker.analyse(win + (wa - start), static_cast<int>(wb - wa), winT.data(), winSrc.data(), nf);
                commitEpochs(hopIndex, wa, commitEnd);
            }
        }

        // ---- island management
        if (st.active) {
            Island* isl = findIsland(st.id);
            const bool newer = newestId != st.id;
            bool stale = false;
            if (isl && !newer) {
                const int64_t K = isl->e.size();
                stale = (commitEnd - lastE) > kStalePeriods * (isl->e[K - 1] - isl->e[K - 2]);
            }
            if (newer || stale || !isl) closeState(a);
        }
        if (!st.active && hasE) {
            // ids ascending; skip small ones that are not the newest
            for (size_t k = 0; k < isls.size(); ++k) {
                Island& cand = isls[k];
                if (cand.id <= lastOpen) continue;
                const bool newest = cand.id == newestId;
                if (cand.e.size() >= 4) {
                    st = State {};
                    st.active = true; st.id = cand.id;
                    if (pure) {
                        PIsl q; q.id = cand.id; q.e = cand.e; pq.push_back(std::move(q));
                    } else if (mode == Mode::Splice) {
                        st.n0 = static_cast<int64_t>(std::ceil(cand.e[0]));
                        st.u = st.n0; st.p = static_cast<double>(st.n0);
                        const int64_t Ti = std::clamp<int64_t>(roundHalfEven(cand.e[1] - cand.e[0]), 16, 4096);
                        for (int64_t i = 0; i < Ti; ++i)
                            setMask(st.n0 + i, 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(Ti)));
                        st.plateauTo = st.n0 + Ti;
                    }
                    ++s.islands;
                }
                if (st.active || !newest) lastOpen = cand.id;
                if (st.active || newest) break;
            }
        }
        if (st.active) {
            Island* isl = findIsland(st.id);
            const Seq& e = isl->e;
            const int64_t K = e.size();
            const double tl = e[K - 1] - e[K - 2];
            const bool ended = newestId != st.id || (commitEnd - e[K - 1] > kStalePeriods * tl) || lastBlock;
            if (ended) closeState(a);
            else if (pure) pureSync(isl);
            else if (mode == Mode::Psola) {
                if (smoothNorm) {
                    std::vector<double>& d = smoothScratch;
                    d.clear();
                    for (int64_t q = std::max(e.base, e.size() - 257); q + 1 < e.size(); ++q) d.push_back(e[q + 1] - e[q]);
                    if (!d.empty()) {
                        std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
                        smoothSize = std::max(3, static_cast<int>(2.0 * d[d.size() / 2]));
                    }
                }
                psolaGenerate(st, e, false, static_cast<double>(b + gpad));
                const int64_t n = tgtCount(st);
                for (int64_t i = st.added; i < std::max(st.added, n - 4); ++i) {
                    addGrain(e, tgtAt(st, i).t, tgtAt(st, i).j);
                    st.added = i + 1;
                }
                if (n >= 2) {
                    const int64_t t1 = roundHalfEven(st.tg1), t0 = roundHalfEven(st.tg0);
                    if (st.plateauTo == 0 && t1 > t0) {
                        for (int64_t i = t0; i < t1; ++i)
                            setMask(i, 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i - t0) / static_cast<double>(t1 - t0)));
                        st.plateauTo = t1;
                    }
                    const double safeT = tgtAt(st, std::max<int64_t>(0, n - 5)).t;
                    const int64_t upto = static_cast<int64_t>(std::min<double>(static_cast<double>(b), safeT - (smoothNorm ? smoothSize / 2 : 0)));
                    if (upto > st.plateauTo) { fillMask(st.plateauTo, upto, 1.0); st.plateauTo = upto; }
                    if (upto < b) ++s.underruns;
                }
                // bounded memory: drop targets whose grains are long since added
                if (st.tgt.size() > 2 * pruneKeep) {
                    const size_t drop = std::min<size_t>(st.tgt.size() - 8, static_cast<size_t>(st.added - st.tbase));
                    st.tgt.erase(st.tgt.begin(), st.tgt.begin() + static_cast<std::ptrdiff_t>(drop));
                    st.tbase += static_cast<int64_t>(drop);
                }
                if (isl->e.prune(pruneKeep)) ++s.prunes;
            } else {
                const int64_t uBefore = st.u;
                jumpsThisCall = 0;
                advance(st, e, b, true, false, 0, 0.0, kNoLimit);
                s.jumps += static_cast<int>(jumpsThisCall);
                if (st.u > uBefore) {
                    fillMask(std::max(st.plateauTo, uBefore), st.u, 1.0);
                    st.plateauTo = std::max(st.plateauTo, st.u);
                }
                if (st.u < b) ++s.underruns;
                if (isl->e.prune(pruneKeep)) ++s.prunes;
            }
            s.maxIslandEpochs = std::max(s.maxIslandEpochs, isl->e.size());
        }
        if (pure) pureEngineAdvance(b);
        // drop islands that can no longer matter
        while (!isls.empty() && isls.front().id <= lastOpen && !(st.active && isls.front().id == st.id) && isls.size() > 1)
            isls.erase(isls.begin());

        // ---- per-frame diagnostics (before the masks are cleared)
        while (!jumpSpans.empty() && jumpSpans.front().second < a - 4 * static_cast<int64_t>(stride)) jumpSpans.erase(jumpSpans.begin());
        for (int64_t f = static_cast<int64_t>(std::floor(static_cast<double>(a) * 200.0 / fs)) - 1;; ++f) {
            const int64_t c = std::llround(static_cast<double>(f) * fs / 200.0);
            if (c >= b) break;
            if (c < a || f < 0) continue;
            const size_t slot = static_cast<size_t>(f) & fmask;
            const bool active = mask[static_cast<size_t>(c) & amask] > 0.0;
            bool jump = false;
            for (const auto& j : jumpSpans)
                if (j.first < c + stride / 2 && j.second > c - stride / 2) { jump = true; break; }
            int island = -1;
            if (active) island = pure ? (inIsland ? curId : pureLastId) : (closedId >= 0 && c < closedEnd) ? closedId : (st.active ? st.id : closedId);
            diagFrame[slot] = f; diagIsland[slot] = island;
            diagFlags[slot] = static_cast<uint8_t>((active ? 1 : 0) | (jump ? 2 : 0));
        }

        // ---- finalise [a, b)
        for (int64_t i = a; i < b; ++i) {
            const size_t slot = static_cast<size_t>(i) & amask;
            const double m = mask[slot];
            const double x = static_cast<double>(xr[static_cast<size_t>(i) & xmask]);
            double y = x;
            if (pure) {
                y = gR[slot] * (1.0 - m) + ybuf[slot] * m;
                // the dry at the read position: the gap stream where nothing is rendered, the sinc read at the
                // island's read position (ratio 1) where it is
                const double aligned = m != 0.0 ? gR[slot] * (1.0 - m) + sincRead(traceP[slot], 1.0) * m : gR[slot];
                alignedOut[static_cast<size_t>(i) % outCap] = static_cast<float>(aligned);
                posOut[static_cast<size_t>(i) % outCap] = m >= 0.5 ? traceP[slot] : gapPos[slot];
            }
            else if (m != 0.0) {
                double ps;
                if (mode == Mode::Psola) {
                    double wn = wsum[slot];
                    if (smoothNorm) { // uniform_filter1d(wsum, 2 periods): mean over [i - S/2, i - S/2 + S)
                        double sum = 0.0;
                        for (int k = 0; k < smoothSize; ++k) sum += wsum[static_cast<size_t>(i - smoothSize / 2 + k) & amask];
                        wn = sum / smoothSize;
                    }
                    ps = wsum[slot] > 1e-3 ? acc[slot] / std::max(wn, 1e-3) : 0.0;
                }
                else ps = ybuf[slot];
                y = x * (1.0 - m) + ps * m;
            }
            if (formant) yfinR[slot] = y;
            else out[static_cast<size_t>(i) % outCap] = static_cast<float>(y);
            acc[slot] = mask[slot] = ybuf[slot] = 0.0;
            if (pure) gR[slot] = 0.0;
            if (smoothNorm && mode == Mode::Psola) wsum[static_cast<size_t>(i - 4096) & amask] = 0.0;
            else wsum[slot] = 0.0;
        }
        done = b;
        if (formant) formantAdvance(streamEnd >= 0 && b >= streamEnd);
        else readyEnd = b;
        nextExpected += hop;
        ++hopIndex;

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        s.hopMsMax = std::max(s.hopMsMax, ms);
        hopMsSum += ms; ++s.hops;
        ++hist[std::min<size_t>(hist.size() - 1, static_cast<size_t>(ms * 10.0))];
    }

    Stats snapshot() const {
        Stats r = s;
        if (r.hops > 0) {
            r.hopMsMean = hopMsSum / r.hops;
            const long target = static_cast<long>(std::ceil(0.99 * r.hops));
            long seen = 0;
            for (size_t i = 0; i < hist.size(); ++i) {
                seen += hist[i];
                if (seen >= target) { r.hopMsP99 = (i + 1) / 10.0; break; }
            }
        }
        return r;
    }
};

WaveformRenderer::WaveformRenderer() = default;
WaveformRenderer::~WaveformRenderer() = default;

void WaveformRenderer::setMode(Mode m) { mode_ = m; }
void WaveformRenderer::setSpliceTrim(bool on) { trim_ = on; }
void WaveformRenderer::setSpliceFormant(int mode) { formant_ = std::clamp(mode, 0, 2); }
void WaveformRenderer::setFormantTuning(bool rmsNormalise, double hopMs) { fNorm_ = rmsNormalise; fHopMs_ = std::clamp(hopMs, 1.0, 10.0); }
void WaveformRenderer::setSplicePerCycle(bool on) { perCycle_ = on; }
void WaveformRenderer::setPsolaOptions(bool stepReuse, bool smoothNorm) { stepReuse_ = stepReuse; smoothNorm_ = smoothNorm; }
void WaveformRenderer::setSplicePure(bool on, double maxDriftMs) { pure_ = on; maxDriftMs_ = maxDriftMs; }
void WaveformRenderer::setPeakHint(double peak) { peakHint_ = peak; }
void WaveformRenderer::setPruneKeep(size_t epochs) { pruneKeep_ = std::max<size_t>(16, epochs); }
void WaveformRenderer::setStreamEnd(int64_t n) { streamEnd_ = n; }
void WaveformRenderer::enableEpochLog(bool on) { log_ = on; }

void WaveformRenderer::prepare(int sampleRate, int contextSamples, int hopSamples,
                               size_t ringCapacity, int64_t origin) {
    impl = std::make_unique<Impl>(sampleRate, contextSamples, hopSamples, ringCapacity, origin,
                                  mode_, trim_, log_, streamEnd_);
    impl->pruneKeep = pruneKeep_;
    if (pure_ && mode_ == Mode::Splice && formant_ == 0) {
        impl->pure = true; impl->maxDriftMs = maxDriftMs_; impl->peakHint = peakHint_; impl->configurePure();
    }
    impl->perCycle = perCycle_ && mode_ == Mode::Splice;
    impl->stepReuse = stepReuse_ && mode_ == Mode::Psola;
    impl->smoothNorm = smoothNorm_ && mode_ == Mode::Psola;
    if (mode_ == Mode::Splice && formant_ > 0) { impl->formant = formant_; impl->fNorm = fNorm_; impl->fHopMs = fHopMs_; impl->configureFormant(); }
}

void WaveformRenderer::processWindowHz(const std::vector<float>& window, const double* srcHz,
                                       const double* desHz, int frameCount, int64_t next) {
    if (!impl) throw std::logic_error("Waveform renderer was not prepared");
    impl->push(window.data(), window.size(), srcHz, desHz, frameCount, next);
}

void WaveformRenderer::processWindow(const std::vector<float>& window,
                                     const std::vector<Frame>& frames, int64_t next) {
    if (!impl) throw std::logic_error("Waveform renderer was not prepared");
    std::vector<double> src(frames.size()), des(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame& f = frames[i];
        const bool valid = f.voicedWeight > 0.0f && f.sourceHz > 0.0f;
        src[i] = valid ? f.sourceHz : 0.0;
        des[i] = valid ? f.desiredHz : 0.0;
    }
    impl->push(window.data(), window.size(), src.data(), des.data(), static_cast<int>(frames.size()), next);
}

double WaveformRenderer::readPosition(int64_t index) const noexcept {
    if (!impl || !impl->pure || index < impl->origin || index >= impl->readyEnd
        || index < impl->readyEnd - static_cast<int64_t>(impl->outCap)) return static_cast<double>(index);
    return impl->posOut[static_cast<size_t>(index) % impl->outCap];
}

float WaveformRenderer::alignedDry(int64_t index, float dry) const noexcept {
    if (!impl || !impl->pure || index < impl->origin || index >= impl->readyEnd
        || index < impl->readyEnd - static_cast<int64_t>(impl->outCap)) return dry;
    return impl->alignedOut[static_cast<size_t>(index) % impl->outCap];
}

int64_t WaveformRenderer::readyUntil() const noexcept { return impl ? impl->readyEnd : 0; }

float WaveformRenderer::corrected(int64_t index, float dry) const noexcept {
    if (!impl || index < impl->origin || index >= impl->readyEnd
        || index < impl->readyEnd - static_cast<int64_t>(impl->outCap)) return dry;
    return impl->out[static_cast<size_t>(index) % impl->outCap];
}

WaveformRenderer::FrameDiag WaveformRenderer::frameDiag(int64_t frame) const noexcept {
    if (!impl || frame < 0) return {};
    const size_t slot = static_cast<size_t>(frame) & impl->fmask;
    if (impl->diagFrame[slot] != frame) return {};
    FrameDiag d;
    d.valid = true;
    d.sourceHz = static_cast<float>(impl->srcR[slot]);
    d.desiredHz = static_cast<float>(impl->desR[slot]);
    d.island = impl->diagIsland[slot];
    d.active = (impl->diagFlags[slot] & 1) != 0;
    d.jump = (impl->diagFlags[slot] & 2) != 0;
    return d;
}

WaveformRenderer::Stats WaveformRenderer::stats() const { return impl ? impl->snapshot() : Stats {}; }
const std::vector<WaveformRenderer::EpochRow>& WaveformRenderer::epochLog() const {
    static const std::vector<EpochRow> empty;
    return impl ? impl->log : empty;
}

} // namespace cody_pitch
