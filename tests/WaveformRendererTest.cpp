// Golden equivalence, identity, step, and realtime tests for the waveform renderers.
// Optional golden vectors are loaded from .deps/quality/proto/golden/<case>/.
#include "TestEnvironment.h"
#include "RealtimeEngine.h"
#include "WaveformRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WAVEFORM_RESULTS_DIR
#define WAVEFORM_RESULTS_DIR "."
#endif
using cody_pitch::WaveformRenderer;

namespace {

constexpr int kRate = 48000, kHop = 1920, kCtx = 9600;
constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS " : "  FAIL ") << what << '\n';
    if (!ok) ++failures;
}

std::vector<float> readF32(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto bytes = f.tellg();
    std::vector<float> v(static_cast<size_t>(bytes) / sizeof(float));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), bytes);
    return v;
}

std::vector<float> readWav24(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<float> out;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const uint32_t size = static_cast<uint8_t>(b[pos + 4]) | (static_cast<uint8_t>(b[pos + 5]) << 8)
            | (static_cast<uint8_t>(b[pos + 6]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b[pos + 7])) << 24);
        if (std::string(b.data() + pos, 4) == "data") {
            for (size_t i = pos + 8; i + 3 <= pos + 8 + size && i + 3 <= b.size(); i += 3) {
                int32_t v = static_cast<uint8_t>(b[i]) | (static_cast<uint8_t>(b[i + 1]) << 8) | (static_cast<int32_t>(static_cast<int8_t>(b[i + 2])) << 16);
                out.push_back(static_cast<float>(v / 8388608.0));
            }
            break;
        }
        pos += 8 + size + (size & 1);
    }
    return out;
}

struct Frames { std::vector<double> src, des; };

Frames readFrames(const std::string& path) {
    Frames fr;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string cell;
        double v[5] {};
        for (int i = 0; i < 5 && std::getline(ss, cell, ','); ++i) v[i] = std::strtod(cell.c_str(), nullptr);
        const bool valid = v[3] > 0 && v[1] > 0;
        fr.src.push_back(valid ? v[1] : 0.0);
        fr.des.push_back(valid ? v[2] : 0.0);
    }
    return fr;
}

// The plug-in's own frames CSV (full precision) found by header name; the golden vectors were made
// from these, while golden/<case>/frames.csv carries them rounded to 6 decimals.
Frames readFramesByHeader(const std::string& path) {
    Frames fr;
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return fr;
    std::vector<std::string> names;
    { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) names.push_back(c); }
    const auto col = [&](const char* n) { return static_cast<int>(std::find(names.begin(), names.end(), n) - names.begin()); };
    const int cs = col("source_hz"), cd = col("desired_hz"), cv = col("voiced");
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string cell;
        std::vector<double> v;
        while (std::getline(ss, cell, ',')) v.push_back(std::strtod(cell.c_str(), nullptr));
        const bool valid = (cv >= static_cast<int>(names.size()) || v[static_cast<size_t>(cv)] > 0) && v[static_cast<size_t>(cs)] > 0;
        fr.src.push_back(valid ? v[static_cast<size_t>(cs)] : 0.0);
        fr.des.push_back(valid ? v[static_cast<size_t>(cd)] : 0.0);
    }
    return fr;
}

std::vector<double> readEpochs(const std::string& path) {
    std::vector<double> e;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string cell;
        std::getline(ss, cell, ',');
        std::getline(ss, cell, ',');
        e.push_back(std::strtod(cell.c_str(), nullptr));
    }
    return e;
}

struct RunResult {
    std::vector<float> out, aligned;
    std::vector<WaveformRenderer::EpochRow> epochs;
    WaveformRenderer::Stats stats;
};

RunResult run(const std::vector<float>& x, const Frames& fr, WaveformRenderer::Mode mode, bool trim,
              int rate = kRate, size_t keep = 4096, int formant = 0, bool perCycle = false,
              bool stepReuse = false, bool smoothNorm = false, bool pure = false) {
    const int hop = rate / 25, ctx = rate / 5;
    WaveformRenderer wr;
    wr.setMode(mode);
    wr.setSpliceTrim(trim);
    wr.setPruneKeep(keep);
    wr.setSpliceFormant(formant);
    wr.setSplicePerCycle(perCycle);
    if (pure) {
        float peak = 0.0f;
        for (const float v : x) peak = std::max(peak, std::abs(v));
        wr.setSplicePure(true, 80.0);
        wr.setPeakHint(peak);
    }
    wr.setPsolaOptions(stepReuse, smoothNorm);
    wr.setStreamEnd(static_cast<int64_t>(x.size()));
    wr.enableEpochLog(true);
    wr.prepare(rate, ctx, hop, static_cast<size_t>(rate) * 8, 0);
    const int64_t n = static_cast<int64_t>(x.size());
    const int64_t blocks = (n + hop - 1) / hop;
    RunResult r;
    r.out.assign(x.size(), 0.0f);
    r.aligned.assign(x.size(), 0.0f);
    std::vector<float> window(static_cast<size_t>(hop + 2 * ctx));
    const int nFrames = static_cast<int>(std::ceil(window.size() * 200.0 / rate)) + 1;
    std::vector<double> src(static_cast<size_t>(nFrames)), des(static_cast<size_t>(nFrames));
    int64_t published = 0;
    for (int64_t k = 0; k < blocks; ++k) {
        const int64_t next = k * hop, start = next - ctx;
        for (size_t i = 0; i < window.size(); ++i) {
            const int64_t idx = start + static_cast<int64_t>(i);
            window[i] = idx >= 0 && idx < n ? x[static_cast<size_t>(idx)] : 0.0f;
        }
        const int64_t f0 = std::llround(start * 200.0 / rate);
        for (int i = 0; i < nFrames; ++i) {
            const int64_t f = f0 + i;
            const bool in = f >= 0 && f < static_cast<int64_t>(fr.src.size());
            src[static_cast<size_t>(i)] = in ? fr.src[static_cast<size_t>(f)] : 0.0;
            des[static_cast<size_t>(i)] = in ? fr.des[static_cast<size_t>(f)] : 0.0;
        }
        wr.processWindowHz(window, src.data(), des.data(), nFrames, next);
        const int64_t ready = wr.readyUntil();
        for (int64_t i = published; i < ready; ++i)
            { r.out[static_cast<size_t>(i)] = wr.corrected(i, x[static_cast<size_t>(i)]); r.aligned[static_cast<size_t>(i)] = wr.alignedDry(i, x[static_cast<size_t>(i)]); }
        published = ready;
    }
    r.epochs = wr.epochLog();
    r.stats = wr.stats();
    return r;
}

double nullDb(const std::vector<float>& a, const std::vector<float>& ref, const std::vector<float>& mask01,
              bool onlyMasked) {
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < std::min(a.size(), ref.size()); ++i) {
        if (onlyMasked && mask01[i] == 0.0f) continue;
        const double d = static_cast<double>(a[i]) - ref[i];
        num += d * d; den += static_cast<double>(ref[i]) * ref[i];
    }
    return 10.0 * std::log10(std::max(num, 1e-300) / std::max(den, 1e-300));
}

std::vector<float> voicedMask(const Frames& fr, size_t n) {
    std::vector<float> m(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const size_t f = std::min(fr.src.size() - 1, static_cast<size_t>(std::nearbyint(i / 240.0)));
        m[i] = fr.src[f] > 0 ? 1.0f : 0.0f;
    }
    return m;
}

double epochMatch(std::vector<double> a, const std::vector<WaveformRenderer::EpochRow>& mine) {
    std::vector<double> b;
    for (const auto& m : mine) b.push_back(m.position);
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    size_t j = 0, matched = 0;
    for (const double g : a) {
        while (j < b.size() && b[j] < g - 1.0 - 1e-6) ++j;
        if (j < b.size() && std::abs(b[j] - g) <= 1.0 + 1e-6) { ++matched; ++j; }
    }
    return a.empty() ? 1.0 : static_cast<double>(matched) / static_cast<double>(a.size());
}

// normalised autocorrelation pitch of x[from, to) near an expected frequency
double autocorrHz(const std::vector<float>& x, int from, int to, double expectHz, int rate = kRate) {
    const int lo = static_cast<int>(rate / (expectHz * 1.15)), hi = static_cast<int>(rate / (expectHz / 1.15));
    std::vector<double> c(static_cast<size_t>(hi + 2), 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag) {
        double cross = 0, e0 = 0, e1 = 0;
        for (int i = from; i + lag < to; ++i) { cross += x[i] * x[i + lag]; e0 += x[i] * x[i]; e1 += x[i + lag] * x[i + lag]; }
        c[static_cast<size_t>(lag)] = cross / std::sqrt(std::max(1e-18, e0 * e1));
    }
    int best = lo;
    for (int lag = lo; lag <= hi; ++lag) if (c[static_cast<size_t>(lag)] > c[static_cast<size_t>(best)]) best = lag;
    const double y0 = c[static_cast<size_t>(best - 1)], y1 = c[static_cast<size_t>(best)], y2 = c[static_cast<size_t>(best + 1)];
    const double denom = y0 - 2 * y1 + y2;
    const double shift = std::abs(denom) > 1e-12 ? 0.5 * (y0 - y2) / denom : 0.0;
    return rate / (best + shift);
}

const char* modeName(WaveformRenderer::Mode m) { return m == WaveformRenderer::Mode::Splice ? "splice" : "psola"; }

} // namespace

int main() {
    // Set renderer overrides before RealtimeEngine reads them.
    setTestEnvironment("TUNA_RENDERER", "splice");
    setTestEnvironment("TUNA_SPLICE_PURE", "1");
    // Set planner overrides explicitly so this test isolates renderer behavior.
    setTestEnvironment("TUNA_VOICING_BRIDGE_MS", "0"); setTestEnvironment("TUNA_VOICING_BRIDGE_MODE", "0");
    setTestEnvironment("TUNA_TARGET_CENTER_MS", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_CPMS", "0");
    setTestEnvironment("TUNA_VELOCITY_GATE_MODE", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_RELEASE_MS", "20");
    const std::string framesCsvPath = (std::filesystem::temp_directory_path() / "waveform_renderer_test_frames.csv").string();
    setTestEnvironment("TUNA_FRAMES_CSV", framesCsvPath.c_str());
    std::string root = __FILE__;
    root = root.substr(0, root.find_last_of('/'));
    root = root.substr(0, root.find_last_of('/'));
    const char* envDir = std::getenv("TUNA_GOLDEN_DIR");
    const std::string golden = envDir ? envDir : root + "/.deps/quality/proto/golden";

    const char* cases[] = { "synthetic-step100c", "slither-hard-tunatraj", "devotion-hard-tunatraj" };
    bool haveGolden = true;
    std::ostringstream results;
    results << "case | frames CSV | mode | epochs matched % (mine/golden) | null voiced dB | null all dB | trim-off splice vs golden voiced dB\n";
    std::vector<float> synthIn; Frames synthFr;
    for (const char* name : cases) {
        const std::string dir = golden + "/" + name;
        const auto x = readF32(dir + "/input.f32");
        if (x.empty()) { std::cout << "golden vectors missing (" << dir << "); equivalence tests skipped\n"; haveGolden = false; break; }
        Frames fr = readFrames(dir + "/frames.csv");
        const std::string song = std::string(name).substr(0, std::string(name).find('-'));
        const Frames full = readFramesByHeader(root + "/.deps/quality/ablation/" + song + "-hard0-base.csv");
        const bool fullPrecision = !full.src.empty() && full.src.size() >= fr.src.size();
        if (fullPrecision) fr = full;
        std::cout << "  frames: " << (fullPrecision ? "full-precision plug-in CSV" : "golden frames.csv (6 decimals)") << '\n';
        const auto gold = readEpochs(dir + "/epochs.csv");
        const auto goldPsola = readF32(dir + "/psola_out.f32");
        const auto goldSplice = readF32(dir + "/splice_fx_out.f32");
        const auto mask = voicedMask(fr, x.size());
        std::cout << name << ": " << x.size() << " samples, " << gold.size() << " golden epochs\n";
        const auto sp = run(x, fr, WaveformRenderer::Mode::Splice, true);
        const auto ps = run(x, fr, WaveformRenderer::Mode::Psola, true);
        const double em = epochMatch(gold, sp.epochs), emP = epochMatch(gold, ps.epochs);
        const double nsV = nullDb(sp.out, goldSplice, mask, true), nsA = nullDb(sp.out, goldSplice, mask, false);
        const double npV = nullDb(ps.out, goldPsola, mask, true), npA = nullDb(ps.out, goldPsola, mask, false);
        std::cout << std::fixed << std::setprecision(2)
                  << "  epochs matched: splice " << 100 * em << " % (" << sp.epochs.size() << " mine), psola " << 100 * emP
                  << " % (" << ps.epochs.size() << " mine)\n"
                  << "  null vs golden (voiced / all): splice_fx " << nsV << " / " << nsA << " dB, psola " << npV << " / " << npA << " dB\n"
                  << "  islands splice " << sp.stats.islands << " psola " << ps.stats.islands
                  << ", renderer hop ms max/mean/p99: splice " << sp.stats.hopMsMax << '/' << sp.stats.hopMsMean << '/' << sp.stats.hopMsP99
                  << ", psola " << ps.stats.hopMsMax << '/' << ps.stats.hopMsMean << '/' << ps.stats.hopMsP99 << '\n';
        for (const auto& pair : { std::make_pair(&ps.out, &goldPsola), std::make_pair(&sp.out, &goldSplice) }) {
            size_t count = 0, first = 0, last = 0; double worst = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double d = std::abs((*pair.first)[i] - (*pair.second)[i]);
                if (d > 1e-4) { if (!count) first = i; last = i; ++count; }
                worst = std::max(worst, d);
            }
            std::cout << "  (information) " << (pair.first == &ps.out ? "psola" : "splice") << " samples differing by > 1e-4: " << count
                      << " first " << first << " last " << last << " max abs " << worst << '\n';
        }
        check(em >= 0.99, std::string(name) + " splice epochs >= 99 % within +-1 sample");
        check(emP >= 0.99, std::string(name) + " psola epochs >= 99 % within +-1 sample");
        check(nsV <= -60.0, std::string(name) + " splice_fx null <= -60 dB on voiced regions");
        check(npV <= -60.0, std::string(name) + " psola null <= -60 dB on voiced regions");
        const auto spNoTrim = run(x, fr, WaveformRenderer::Mode::Splice, false);
        const std::string framesUsed = fullPrecision ? "full-precision plug-in CSV (.deps/quality/ablation)" : "golden frames.csv (6 decimals)";
        results << std::fixed << std::setprecision(2)
                << name << " | " << framesUsed << " | splice | " << 100 * em << " (" << sp.epochs.size() << "/" << gold.size() << ") | "
                << nsV << " | " << nsA << " | " << nullDb(spNoTrim.out, goldSplice, mask, true) << "\n"
                << name << " | " << framesUsed << " | psola | " << 100 * emP << " (" << ps.epochs.size() << "/" << gold.size() << ") | "
                << npV << " | " << npA << " | -\n";
        std::cout << "  (information) trim-off splice vs trimmed golden: voiced "
                  << nullDb(spNoTrim.out, goldSplice, mask, true) << " dB, all " << nullDb(spNoTrim.out, goldSplice, mask, false) << " dB\n";
        if (std::string(name) == "synthetic-step100c") { synthIn = x; synthFr = fr; }
    }

    {
        std::ofstream resultsFile(std::string(WAVEFORM_RESULTS_DIR) + "/waveform_golden_results.txt");
        resultsFile << results.str();
    }
    if (haveGolden) {
        std::cout << "identity (desired = source) on the synthetic vowel\n";
        Frames same = synthFr;
        same.des = same.src;
        const auto mask = voicedMask(same, synthIn.size());
        for (const auto mode : { WaveformRenderer::Mode::Splice, WaveformRenderer::Mode::Psola }) {
            const auto r = run(synthIn, same, mode, false);
            const double db = nullDb(r.out, synthIn, mask, true);
            std::cout << "  " << modeName(mode) << " null vs input: " << db << " dB\n";
            check(db <= -60.0, std::string(modeName(mode)) + " identity <= -60 dB");
        }
        std::cout << "step: +100 cents at 1.0 s, pitch measured by autocorrelation 20-60 ms after the step\n";
        for (const auto mode : { WaveformRenderer::Mode::Splice, WaveformRenderer::Mode::Psola }) {
            const auto r = run(synthIn, synthFr, mode, false);
            const int from = kRate + kRate * 20 / 1000 + 480, to = from + kRate * 40 / 1000;
            double expect = 0.0; int count = 0;
            for (int f = from / 240; f <= to / 240; ++f) { expect += synthFr.des[static_cast<size_t>(f)]; ++count; }
            expect /= count;
            const double hz = autocorrHz(r.out, from, to, expect);
            const double cents = 1200.0 * std::log2(hz / expect);
            std::cout << "  " << modeName(mode) << " measured " << hz << " Hz, expected " << expect << " Hz, error " << cents << " cents\n";
            check(std::abs(cents) <= 10.0, std::string(modeName(mode)) + " +100 cent step within 10 cents");
        }
    }

    // ---- optional splice envelope-correction comparisons
    {
        const std::string pyDir = root + "/outputs/proto/formant";
        const std::string trajDir = root + "/.deps/quality/port/bridge";
        const struct { const char* song; const char* setting; const char* src; } cases[] = {
            { "slither", "hard0", "slither-matrix-0-9.f32" }, { "slither", "natural40", "slither-matrix-0-9.f32" },
            { "devotion", "hard0", "devotion-matrix-40-50.f32" }, { "devotion", "natural40", "devotion-matrix-40-50.f32" } };
        for (const auto& c : cases) {
            const std::string tag = std::string(c.song) + "-" + c.setting;
            const auto x = readF32(root + "/.deps/quality/" + c.src);
            const Frames fr = readFramesByHeader(trajDir + "/" + tag + "-fill60-frames.csv");
            const auto pySplice = readWav24(pyDir + "/" + tag + "-splice.wav");
            const auto pyLf = readWav24(pyDir + "/" + tag + "-splice_formant_lf.wav");
            const auto pyFull = readWav24(pyDir + "/" + tag + "-splice_formant.wav");
            if (x.empty() || fr.src.empty() || pyLf.empty()) { std::cout << "formant " << tag << ": inputs missing, skipped\n"; continue; }
            const auto mask = voicedMask(fr, x.size());
            const auto off = run(x, fr, WaveformRenderer::Mode::Splice, true);
            const auto lf = run(x, fr, WaveformRenderer::Mode::Splice, true, kRate, 4096, 2);
            const auto full = run(x, fr, WaveformRenderer::Mode::Splice, true, kRate, 4096, 1);
            std::cout << "formant " << tag << ": null vs Python on voiced frames (dB)  splice " << nullDb(off.out, pySplice, mask, true)
                      << "  formant lf " << nullDb(lf.out, pyLf, mask, true) << "  formant full " << nullDb(full.out, pyFull, mask, true)
                      << "  | renderer hop ms max/p99 (lf) " << lf.stats.hopMsMax << "/" << lf.stats.hopMsP99
                      << ", off " << off.stats.hopMsMax << "/" << off.stats.hopMsP99 << '\n';
            for (const auto& item : { std::make_pair("off", &off), std::make_pair("lf", &lf), std::make_pair("full", &full) }) {
                std::ofstream f(std::string(WAVEFORM_RESULTS_DIR) + "/formant-" + tag + "-" + item.first + ".f32", std::ios::binary);
                f.write(reinterpret_cast<const char*>(item.second->out.data()), static_cast<std::streamsize>(item.second->out.size() * sizeof(float)));
            }
            bool finite = true;
            for (const float v : lf.out) finite = finite && std::isfinite(v);
            check(finite, "formant " + tag + " output finite");
        }
    }

    // formant stage self-check: with desired = source the splice output is the source and the read position is the
    // output position, so source and output envelopes coincide and the correction must leave the signal alone
    if (haveGolden) {
        Frames same = synthFr;
        same.des = same.src;
        const auto mask = voicedMask(same, synthIn.size());
        for (const int mode : { 1, 2 }) {
            const auto r = run(synthIn, same, WaveformRenderer::Mode::Splice, false, kRate, 4096, mode);
            const double db = nullDb(r.out, synthIn, mask, true);
            std::cout << "formant mode " << mode << " identity null vs input: " << db << " dB\n";
            check(db <= -60.0, "formant mode " + std::to_string(mode) + " leaves an identity render unchanged");
        }
    }

    // ---- step options against the Python step_study renders (hard0, fill60 frames; whole-file epochs there)
    {
        const std::string pyDir = root + "/.deps/quality/port/step-py";
        const std::string trajDir = root + "/.deps/quality/port/bridge";
        for (const char* song : { "slither", "devotion" }) {
            const std::string tag = std::string(song) + "-hard0";
            const auto x = readF32(root + "/.deps/quality/" + (std::string(song) == "slither" ? "slither-matrix-0-9.f32" : "devotion-matrix-40-50.f32"));
            const Frames fr = readFramesByHeader(trajDir + "/" + tag + "-fill60-frames.csv");
            if (x.empty() || fr.src.empty()) continue;
            const auto mask = voicedMask(fr, x.size());
            const struct { const char* name; WaveformRenderer::Mode mode; bool trim, pc, reuse, smooth; } v[] = {
                { "psola-base", WaveformRenderer::Mode::Psola, false, false, false, false },
                { "psola-a_smooth", WaveformRenderer::Mode::Psola, false, false, false, true },
                { "psola-c", WaveformRenderer::Mode::Psola, false, false, true, false },
                { "psola-a_smooth+c", WaveformRenderer::Mode::Psola, false, false, true, true },
                { "splice-base", WaveformRenderer::Mode::Splice, true, false, false, false },
                { "splice-b_pc", WaveformRenderer::Mode::Splice, true, true, false, false } };
            std::cout << "step options " << tag << ": null vs Python on voiced frames (dB):";
            for (const auto& c : v) {
                const auto py = readWav24(pyDir + "/" + tag + "-" + c.name + ".wav");
                if (py.empty()) continue;
                const auto r = run(x, fr, c.mode, c.trim, kRate, 4096, 0, c.pc, c.reuse, c.smooth);
                std::cout << "  " << c.name << " " << nullDb(r.out, py, mask, true);
            }
            std::cout << '\n';
        }
    }

    // ---- splice_pure2 (TUNA_SPLICE_PURE) against the Python streaming golden vectors
    {
        const std::string gdir = golden + "/../golden-pure";
        const char* pureCases[] = { "synthetic-step100c", "slither-hard0", "devotion-hard0" };
        for (const char* name : pureCases) {
            const std::string dir = gdir + "/" + name;
            const auto x = readF32(dir + "/input.f32");
            if (x.empty()) { std::cout << "pure golden " << name << " missing, skipped\n"; continue; }
            Frames fr = readFrames(dir + "/frames.csv");
            const std::string song = std::string(name).substr(0, std::string(name).find('-'));
            const Frames full = readFramesByHeader(root + "/.deps/quality/port/vgate2/" + song + "-hard0-m1-v12-c60.csv");
            const bool fullPrecision = std::string(name) != "synthetic-step100c" && full.src.size() >= fr.src.size();
            if (fullPrecision) fr = full;
            const auto gold = readEpochs(dir + "/epochs.csv");
            const auto goldOut = readF32(dir + "/splice_pure_out.f32");
            const auto mask = voicedMask(fr, x.size());
            const auto r = run(x, fr, WaveformRenderer::Mode::Splice, false, kRate, 4096, 0, false, false, false, true);
            const double em = epochMatch(gold, r.epochs);
            const double nv = nullDb(r.out, goldOut, mask, true), na = nullDb(r.out, goldOut, mask, false);
            size_t count = 0, first = 0; double worst = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double d = std::abs(r.out[i] - goldOut[i]);
                if (d > 1e-4) { if (!count) first = i; ++count; }
                worst = std::max(worst, d);
            }
            std::cout << "pure " << name << " (" << (fullPrecision ? "full-precision CSV" : "golden frames.csv") << "): epochs " << 100 * em << " %, null voiced "
                      << nv << " dB, all " << na << " dB; samples differing > 1e-4: " << count << " (first " << first << "), max abs " << worst
                      << "; silence " << r.stats.pureSilence << " wsola " << r.stats.pureWsola << " forced " << r.stats.pureForced
                      << " late exit ramps " << r.stats.pureLateExit << ", max |offset| " << r.stats.maxOffsetSamples * 1000.0 / kRate
                      << " ms, hop ms max/p99 " << r.stats.hopMsMax << "/" << r.stats.hopMsP99 << '\n';
            check(em >= 0.99, std::string("pure ") + name + " epochs >= 99 %");
            check(nv <= -60.0, std::string("pure ") + name + " null <= -60 dB on voiced frames");
        }
    }

    // ---- splice_pure2: identity inside islands, and a click in a gap must survive untouched
    {
        const int rate = kRate;
        const size_t n = static_cast<size_t>(2.4 * rate);
        std::vector<float> x(n, 0.0f);
        double phase = 0.0;
        const auto vowel = [&](double t0, double t1) {
            for (size_t i = static_cast<size_t>(t0 * rate); i < static_cast<size_t>(t1 * rate); ++i) {
                const double t = i / static_cast<double>(rate);
                phase += 2 * kPi * 200.0 * (1.0 + 0.004 * std::sin(2 * kPi * 5.0 * t)) / rate;
                double v = 0.0;
                for (int h = 1; h <= 12; ++h) v += std::sin(h * phase) / h;
                x[i] = static_cast<float>(0.15 * v * std::min(1.0, std::min((i - t0 * rate) / 480.0, (t1 * rate - i) / 480.0)));
            }
        };
        vowel(0.1, 0.9); vowel(1.5, 2.3);
        const size_t clickAt = static_cast<size_t>(1.2 * rate);
        for (int k = 0; k < 6; ++k) x[clickAt + static_cast<size_t>(k)] = 0.8f * std::exp(-0.7f * k) * (k % 2 ? -1.0f : 1.0f);
        Frames same, shifted;
        for (size_t f = 0; f < static_cast<size_t>(2.4 * 200) + 4; ++f) {
            const double t = f * 0.005;
            const bool voiced = (t >= 0.1 + 0.02 && t <= 0.9 - 0.02) || (t >= 1.5 + 0.02 && t <= 2.3 - 0.02);
            const double hz = voiced ? 200.0 * (1.0 + 0.004 * std::sin(2 * kPi * 5.0 * t)) : 0.0;
            same.src.push_back(hz); same.des.push_back(hz);
            shifted.src.push_back(hz); shifted.des.push_back(t < 1.0 ? hz * std::exp2(50.0 / 1200.0) : hz);
        }
        const auto id = run(x, same, WaveformRenderer::Mode::Splice, false, rate, 4096, 0, false, false, false, true);
        double worst = 0.0;
        // the reference exit ramp reads the dry one sample late (x[round(p(u1) + u - (u1 - 1))]); compare outside it
        const auto inExit = [&](size_t i) { return (i >= static_cast<size_t>(0.85 * rate) && i < static_cast<size_t>(0.95 * rate))
                                                 || (i >= static_cast<size_t>(2.25 * rate) && i < static_cast<size_t>(2.35 * rate)); };
        for (size_t i = 0; i < n; ++i) if (!inExit(i)) worst = std::max(worst, static_cast<double>(std::abs(id.out[i] - x[i])));
        { size_t firstBad = 0, nBad = 0; for (size_t i = 0; i < n; ++i) if (id.out[i] != x[i]) { if (!nBad) firstBad = i; ++nBad; }
          std::cout << "pure identity: " << nBad << " samples differ, first at " << firstBad << " (" << firstBad / 48.0 << " ms)\n"; }
        std::cout << "pure identity: max |out - dry| " << worst << ", gap edits " << id.stats.pureSilence + id.stats.pureWsola
                  << ", forced seams " << id.stats.pureForced << '\n';
        double alignedWorst = 0.0;
        for (size_t i = 0; i < n; ++i) if (!inExit(i)) alignedWorst = std::max(alignedWorst, static_cast<double>(std::abs(id.aligned[i] - x[i])));
        std::cout << "pure identity: aligned dry max |aligned - dry| " << alignedWorst << '\n';
        check(alignedWorst == 0.0, "pure identity: aligned dry equals the dry");
        check(worst == 0.0 && id.stats.pureSilence + id.stats.pureWsola + id.stats.pureForced == 0, "pure identity is bit-exact dry with no gap edits");
        const auto cl = run(x, shifted, WaveformRenderer::Mode::Splice, false, rate, 4096, 0, false, false, false, true);
        int srcLoud = 0, outLoud = 0; float outPeak = 0.0f;
        for (size_t i = 0; i < n; ++i) { srcLoud += std::abs(x[i]) > 0.3f && i >= clickAt - 100 && i < clickAt + 100; }
        for (size_t i = static_cast<size_t>(1.0 * rate); i < static_cast<size_t>(1.45 * rate); ++i) { outLoud += std::abs(cl.out[i]) > 0.3f; outPeak = std::max(outPeak, std::abs(cl.out[i])); }
        std::cout << "pure click: loud samples in " << srcLoud << " out " << outLoud << ", peak " << outPeak << ", silence edits " << cl.stats.pureSilence
                  << ", wsola " << cl.stats.pureWsola << '\n';
        check(outLoud == srcLoud && std::abs(outPeak - 0.8f) < 1e-6f && cl.stats.pureSilence + cl.stats.pureWsola > 0,
              "pure: a click in a gap passes once, untouched, while the gap still absorbed drift");
    }

    // ---- pure2 hybrid: the > 8 kHz band must come from the drift-aligned dry, not from the dry at the output index
    if (haveGolden) {
        const std::string dir = golden + "/../golden-pure/synthetic-step100c";
        const auto x = readF32(dir + "/input.f32");
        Frames fr = readFrames(dir + "/frames.csv");
        if (!x.empty()) {
            const auto r = run(x, fr, WaveformRenderer::Mode::Splice, false, kRate, 4096, 0, false, false, false, true);
            // the engine's 129-tap Hann-windowed sinc at 8 kHz
            std::vector<double> lp(129);
            double total = 0.0;
            const double norm = 2.0 * 8000.0 / kRate;
            for (int k = -64; k <= 64; ++k) {
                const double sinc = k == 0 ? norm : std::sin(kPi * norm * k) / (kPi * k);
                lp[static_cast<size_t>(k + 64)] = sinc * 0.5 * (1.0 + std::cos(kPi * k / 64.0));
                total += lp[static_cast<size_t>(k + 64)];
            }
            for (auto& v : lp) v /= total;
            const auto filt = [&](const std::vector<double>& in, size_t i) {
                double sum = 0.0;
                for (int k = -64; k <= 64; ++k) {
                    const long at = static_cast<long>(i) + k;
                    if (at >= 0 && at < static_cast<long>(in.size())) sum += lp[static_cast<size_t>(k + 64)] * in[static_cast<size_t>(at)];
                }
                return sum;
            };
            std::vector<double> diff(x.size()), aligned(x.size()), hybrid(x.size()), hybridWrong(x.size());
            for (size_t i = 0; i < x.size(); ++i) { diff[i] = static_cast<double>(r.out[i]) - r.aligned[i]; aligned[i] = r.aligned[i]; }
            std::vector<double> diffWrong(x.size());
            for (size_t i = 0; i < x.size(); ++i) diffWrong[i] = static_cast<double>(r.out[i]) - x[i];
            double num = 0, den = 0, numWrong = 0, shift = 0;
            for (size_t i = 0; i < x.size(); ++i) { hybrid[i] = aligned[i] + filt(diff, i); hybridWrong[i] = x[i] + filt(diffWrong, i); }
            for (size_t i = 200; i + 200 < x.size(); ++i) {
                const double hpAligned = aligned[i] - filt(aligned, i);
                const double hpHybrid = hybrid[i] - filt(hybrid, i);
                const double hpWrong = hybridWrong[i] - filt(hybridWrong, i);
                num += (hpHybrid - hpAligned) * (hpHybrid - hpAligned); den += hpAligned * hpAligned;
                numWrong += (hpWrong - hpAligned) * (hpWrong - hpAligned);
                shift += std::abs(aligned[i] - x[i]);
            }
            std::cout << "pure hybrid: > 8 kHz band of aligned-dry hybrid vs aligned dry " << 10 * std::log10(std::max(num, 1e-300) / den)
                      << " dB (dry-at-output-index hybrid would give " << 10 * std::log10(std::max(numWrong, 1e-300) / den)
                      << " dB); mean |aligned - dry| " << shift / x.size() << '\n';
            // the 129-tap low-pass leaks about -45 dB of the difference signal past 8 kHz; the unaligned hybrid gives 0 dB
            check(10 * std::log10(std::max(num, 1e-300) / den) <= -40.0, "pure hybrid keeps the > 8 kHz band of the aligned dry (<= -40 dB, filter leakage)");
        }
    }

    // ---- epoch pruning: a tiny keep limit must not change the output
    if (haveGolden) {
        const std::string dir = golden + "/slither-hard-tunatraj";
        const auto x = readF32(dir + "/input.f32");
        Frames fr = readFrames(dir + "/frames.csv");
        const Frames full = readFramesByHeader(root + "/.deps/quality/ablation/slither-hard0-base.csv");
        if (!full.src.empty()) fr = full;
        std::cout << "pruning: keep 4096 vs keep 128 epochs per island (slither)\n";
        for (const auto mode : { WaveformRenderer::Mode::Splice, WaveformRenderer::Mode::Psola }) {
            const auto ref = run(x, fr, mode, true);
            const auto pruned = run(x, fr, mode, true, kRate, 128);
            double worst = 0.0;
            for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(ref.out[i] - pruned.out[i])));
            std::cout << "  " << modeName(mode) << ": prunes " << pruned.stats.prunes << ", longest island " << ref.stats.maxIslandEpochs
                      << " epochs, max abs difference " << worst << '\n';
            check(pruned.stats.prunes > 0, std::string(modeName(mode)) + " pruning path exercised");
            check(worst == 0.0, std::string(modeName(mode)) + " pruned output identical");
        }
    }

    // ---- one very long island at the default keep: 60 s sustained vowel, +50 cents from 30 s on
    {
        const int rate = kRate;
        const size_t n = static_cast<size_t>(60) * rate;
        std::vector<float> x(n);
        Frames fr;
        double phase = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double t = i / static_cast<double>(rate);
            phase += 2 * kPi * 190.0 * (1.0 + 0.005 * std::sin(2 * kPi * 4.0 * t)) / rate;
            double v = 0.0;
            for (int h = 1; h <= 10; ++h) v += std::sin(h * phase) / h;
            x[i] = static_cast<float>(0.15 * v);
        }
        for (size_t f = 0; f < static_cast<size_t>(60 * 200 + 4); ++f) {
            const double t = f * 0.005;
            const double hz = 190.0 * (1.0 + 0.005 * std::sin(2 * kPi * 4.0 * t));
            fr.src.push_back(hz);
            fr.des.push_back(t >= 30.0 ? hz * std::exp2(50.0 / 1200.0) : hz);
        }
        std::cout << "long island: 60 s sustained vowel, default keep\n";
        for (const auto mode : { WaveformRenderer::Mode::Splice, WaveformRenderer::Mode::Psola }) {
            const auto r = run(x, fr, mode, false);
            bool finite = true; float peak = 0;
            for (const float v : r.out) { finite = finite && std::isfinite(v); peak = std::max(peak, std::abs(v)); }
            const int from = 50 * rate, to = from + rate / 10;
            double expect = 0; int cnt = 0;
            for (int f = from / 240; f <= to / 240; ++f) { expect += fr.des[static_cast<size_t>(f)]; ++cnt; }
            expect /= cnt;
            const double cents = 1200.0 * std::log2(autocorrHz(r.out, from, to, expect) / expect);
            std::cout << "  " << modeName(mode) << ": longest island " << r.stats.maxIslandEpochs << " epochs, prunes " << r.stats.prunes
                      << ", peak " << peak << ", pitch error at 50 s " << cents << " cents\n";
            check(r.stats.prunes > 0 && finite && peak <= 0.98f && std::abs(cents) <= 10.0,
                  std::string(modeName(mode)) + " 60 s island: pruning exercised, finite, pitch holds");
        }
    }

    // ---- sample rates: renderer on analytic frames (5 ms frames in seconds, fractional stride at 44.1 kHz)
    std::cout << "sample rates: synthetic vowel, +100 cent step at 1.0 s, identity\n";
    for (const int rate : { 44100, 48000, 96000 }) {
        const double seconds = 2.5;
        const size_t n = static_cast<size_t>(seconds * rate);
        std::vector<float> x(n);
        Frames fr, same;
        const size_t nfr = static_cast<size_t>(seconds * 200) + 4;
        double phase = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double t = i / static_cast<double>(rate);
            phase += 2 * kPi * 200.0 * (1.0 + 0.01 * std::sin(2 * kPi * 5.0 * t)) / rate;
            double v = 0.0;
            for (int h = 1; h <= 20; ++h) v += std::sin(h * phase) / h;
            x[i] = static_cast<float>(0.12 * v);
        }
        for (size_t f = 0; f < nfr; ++f) {
            const double t = f * 0.005;
            const double hz = 200.0 * (1.0 + 0.01 * std::sin(2 * kPi * 5.0 * t));
            fr.src.push_back(hz);
            fr.des.push_back(t >= 1.0 ? hz * std::exp2(100.0 / 1200.0) : hz);
        }
        same.src = fr.src; same.des = fr.src;
        for (const auto mode : { WaveformRenderer::Mode::Splice, WaveformRenderer::Mode::Psola }) {
            const auto idr = run(x, same, mode, false, rate);
            std::vector<float> mask(n, 0.0f);
            for (size_t i = static_cast<size_t>(rate / 4); i + static_cast<size_t>(rate / 4) < n; ++i) mask[i] = 1.0f;
            const double db = nullDb(idr.out, x, mask, true);
            const auto st = run(x, fr, mode, false, rate);
            const int from = rate + rate * 20 / 1000 + rate / 100, to = from + rate * 40 / 1000;
            double expect = 0.0; int count = 0;
            for (int f = static_cast<int>(from * 200.0 / rate); f <= static_cast<int>(to * 200.0 / rate); ++f) { expect += fr.des[static_cast<size_t>(f)]; ++count; }
            expect /= count;
            const double cents = 1200.0 * std::log2(autocorrHz(st.out, from, to, expect, rate) / expect);
            std::cout << "  " << rate << " Hz " << modeName(mode) << ": identity " << db << " dB, step error " << cents
                      << " cents, renderer hop ms max " << st.stats.hopMsMax << '\n';
            check(db <= -60.0, std::to_string(rate) + " " + modeName(mode) + " identity <= -60 dB");
            check(std::abs(cents) <= 10.0, std::to_string(rate) + " " + modeName(mode) + " +100 cent step within 10 cents");
        }
    }

    // ---- through the engine (TUNA_RENDERER=splice) at each host rate, offline
    std::cout << "engine, offline, TUNA_RENDERER=splice: 205 Hz vowel retuned to G3 (196 Hz)\n";
    for (const int rate : { 44100, 48000, 96000 }) {
        cody_pitch::Settings settings;
        settings.allowedNotes = cody_pitch::scaleMask(9, 1);
        settings.amount = 1.0f; settings.retuneMs = 0.0f; settings.transitionMs = 0.0f;
        settings.toleranceCents = 0.0f; settings.expression = 0.0f;
        cody_pitch::RealtimeEngine engine;
        engine.prepare(rate, [&settings] { return settings; });
        const int duration = 3 * rate, block = 256, delay = engine.latencySamples();
        std::vector<float> out(static_cast<size_t>(duration));
        double phase = 0.0;
        for (int offset = 0; offset < duration + delay; offset += block) {
            float samples[256] {};
            const int count = std::min(block, duration + delay - offset);
            for (int i = 0; i < count && offset + i < duration; ++i) {
                phase += 2 * kPi * 205.0 / rate;
                double v = 0.0;
                for (int h = 1; h <= 12; ++h) v += std::sin(h * phase) / h;
                samples[i] = static_cast<float>(0.12 * v);
            }
            engine.process(samples, count, true, true);
            for (int i = 0; i < count; ++i) {
                const int aligned = offset + i - delay;
                if (aligned >= 0 && aligned < duration) out[static_cast<size_t>(aligned)] = samples[i];
            }
        }
        const int underruns = engine.underrunCount();
        const double rendererMs = engine.longestRendererMicros() / 1000.0;
        engine.stop();
        bool finite = true;
        for (const float v : out) finite = finite && std::isfinite(v) && std::abs(v) <= 0.98f;
        const double hz = autocorrHz(out, rate * 2, rate * 2 + rate / 10, 196.0, rate);
        const double cents = 1200.0 * std::log2(hz / 196.0);
        std::cout << "  " << rate << " Hz: " << hz << " Hz (" << cents << " cents from G3), underruns " << underruns
                  << ", longest renderer " << rendererMs << " ms\n";
        check(underruns == 0 && finite, std::to_string(rate) + " Hz engine: no underruns, finite, peak <= 0.98");
        check(std::abs(cents) <= 15.0, std::to_string(rate) + " Hz engine output at G3");
    }

    // ---- TUNA_FRAMES_CSV in waveform mode (the file belongs to the last engine above, flushed on stop)
    {
        std::ifstream f(framesCsvPath);
        std::string line;
        std::getline(f, line);
        std::cout << "frames CSV header: " << line << '\n';
        int rows = 0, active = 0, withIsland = 0, jumps = 0, activeNoSource = 0;
        bool sane = true;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::string cell;
            std::vector<double> v;
            while (std::getline(ss, cell, ',')) v.push_back(std::strtod(cell.c_str(), nullptr));
            if (v.size() != 7) { sane = false; continue; }
            ++rows; active += v[4] > 0; withIsland += v[3] >= 0; jumps += v[5] > 0;
            if (v[4] > 0 && v[3] < 0) sane = false;
            activeNoSource += v[4] > 0 && v[1] <= 0;
        }
        std::cout << "  rows " << rows << ", active " << active << ", with island " << withIsland << ", jump frames " << jumps << '\n';
        check(rows > 100 && active > 50 && sane && activeNoSource * 10 <= active, "waveform frames CSV has time/source/desired/island/active/jump columns filled");
    }

    // ---- stereo: side exactly delayed, mid corrected
    {
        std::cout << "stereo path, TUNA_RENDERER=splice\n";
        cody_pitch::Settings settings;
        settings.allowedNotes = cody_pitch::scaleMask(9, 1);
        settings.amount = 1.0f; settings.retuneMs = 0.0f; settings.transitionMs = 0.0f;
        settings.toleranceCents = 0.0f; settings.expression = 0.0f;
        const double inputHz = 220.0 * std::exp2(45.0 / 1200.0);
        cody_pitch::RealtimeEngine engine;
        engine.prepare(kRate, [&settings] { return settings; });
        const int delay = engine.latencySamples(), duration = 3 * kRate, block = 256;
        std::vector<float> mid(static_cast<size_t>(duration), 0.0f);
        double worstSide = 0.0, phase = 0.0;
        for (int offset = 0; offset < duration + delay; offset += block) {
            const int count = std::min(block, duration + delay - offset);
            float left[256] {}, right[256] {};
            for (int i = 0; i < count && offset + i < duration; ++i) {
                const double t = (offset + i) / static_cast<double>(kRate);
                phase += 2 * kPi * inputHz / kRate;
                const float m = static_cast<float>(0.14 * std::sin(phase) + 0.05 * std::sin(2 * phase));
                const float sd = static_cast<float>(0.015 * std::sin(2.0 * kPi * 997.0 * t));
                left[i] = m + sd; right[i] = m - sd;
            }
            engine.processStereo(left, right, count, true, true);
            for (int i = 0; i < count; ++i) {
                const int source = offset + i - delay;
                const double expect = source >= 0 && source < duration ? 0.03 * std::sin(2.0 * kPi * 997.0 * source / kRate) : 0.0;
                worstSide = std::max(worstSide, std::abs(left[i] - right[i] - expect));
                if (source >= 0 && source < duration) mid[static_cast<size_t>(source)] = 0.5f * (left[i] + right[i]);
            }
        }
        const int underruns = engine.underrunCount();
        engine.stop();
        const double hz = autocorrHz(mid, kRate * 2, kRate * 2 + kRate / 10, 220.0);
        const double cents = 1200.0 * std::log2(hz / 220.0);
        std::cout << "  max side error " << worstSide << ", mid " << hz << " Hz (" << cents << " cents from A3), underruns " << underruns << '\n';
        check(worstSide <= 2.0e-6 && underruns == 0, "splice stereo: side exactly delayed");
        check(std::abs(cents) <= 25.0, "splice stereo: mid corrected to A3");
    }

    // ---- amount = 0 and an empty target mask are exact delayed dry in splice mode
    for (const bool zeroAmount : { true, false }) {
        cody_pitch::Settings settings;
        settings.allowedNotes = cody_pitch::scaleMask(9, 1);
        settings.amount = zeroAmount ? 0.0f : 1.0f; settings.retuneMs = 0.0f;
        if (!zeroAmount) settings.allowedNotes.fill(false);
        cody_pitch::RealtimeEngine engine;
        engine.prepare(kRate, [settings] { return settings; });
        const int delay = engine.latencySamples(), block = 256;
        double worst = 0.0;
        for (int offset = 0; offset < kRate + delay; offset += block) {
            const int count = std::min(block, kRate + delay - offset);
            float samples[256] {};
            for (int i = 0; i < count && offset + i < kRate; ++i)
                samples[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 257.0 * (offset + i) / kRate));
            engine.process(samples, count, true, true);
            for (int i = 0; i < count; ++i) {
                const int source = offset + i - delay;
                const float expected = source >= 0 && source < kRate ? static_cast<float>(0.1 * std::sin(2.0 * kPi * 257.0 * source / kRate)) : 0.0f;
                worst = std::max(worst, static_cast<double>(std::abs(samples[i] - expected)));
            }
        }
        const int underruns = engine.underrunCount();
        engine.stop();
        std::cout << "  " << (zeroAmount ? "amount 0" : "empty mask") << ": max deviation from delayed dry " << worst << ", underruns " << underruns << '\n';
        check(worst <= 1.0e-6 && underruns == 0, std::string(zeroAmount ? "amount 0" : "empty mask") + " is exact delayed dry (splice)");
    }

    // ---- Bypass: two-note phrase, the second pitch class is Bypass
    {
        std::cout << "bypass: A3+45c corrected, then B3+21c Bypass, through the splice engine\n";
        cody_pitch::Settings settings;
        settings.allowedNotes = cody_pitch::scaleMask(9, 1);
        settings.bypassNotes[11] = true; // B
        settings.amount = 1.0f; settings.retuneMs = 0.0f; settings.transitionMs = 0.0f;
        settings.toleranceCents = 0.0f; settings.expression = 0.0f;
        cody_pitch::RealtimeEngine engine;
        engine.prepare(kRate, [&settings] { return settings; });
        const int delay = engine.latencySamples(), duration = 3 * kRate, block = 256;
        const double noteA = 220.0 * std::exp2(45.0 / 1200.0), noteB = 246.94 * std::exp2(21.0 / 1200.0);
        std::vector<float> dry(static_cast<size_t>(duration)), out(static_cast<size_t>(duration));
        double phase = 0.0;
        for (int i = 0; i < duration; ++i) {
            phase += 2 * kPi * (i < 3 * kRate / 2 ? noteA : noteB) / kRate;
            dry[static_cast<size_t>(i)] = static_cast<float>(0.14 * std::sin(phase) + 0.06 * std::sin(2 * phase) + 0.03 * std::sin(3 * phase));
        }
        for (int offset = 0; offset < duration + delay; offset += block) {
            const int count = std::min(block, duration + delay - offset);
            float samples[256] {};
            for (int i = 0; i < count && offset + i < duration; ++i) samples[i] = dry[static_cast<size_t>(offset + i)];
            engine.process(samples, count, true, true);
            for (int i = 0; i < count; ++i) {
                const int aligned = offset + i - delay;
                if (aligned >= 0 && aligned < duration) out[static_cast<size_t>(aligned)] = samples[i];
            }
        }
        const int underruns = engine.underrunCount();
        engine.stop();
        // note B, away from the boundary. The island carries a sub-period read offset from note A's retuning
        // (drift is only corrected beyond half a period), so the waveform is a shifted copy of the dry, not
        // sample-identical: compare level and pitch instead.
        double num = 0.0, den = 0.0;
        const int b0 = 3 * kRate / 2 + kRate / 4, b1 = duration - kRate / 4;
        for (int i = b0; i < b1; ++i) {
            num += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
            den += static_cast<double>(dry[static_cast<size_t>(i)]) * dry[static_cast<size_t>(i)];
        }
        const double levelDb = 10.0 * std::log10(num / den);
        const double hzB = autocorrHz(out, b0, b0 + kRate / 10, noteB);
        const double bypassCents = 1200.0 * std::log2(hzB / noteB);
        const double hzA = autocorrHz(out, kRate / 2, kRate / 2 + kRate / 10, 220.0);
        // glitch metric: largest second difference around the boundary against the dry's own
        double curvOut = 0.0, curvDry = 0.0;
        for (int i = 3 * kRate / 2 - kRate / 10; i < 3 * kRate / 2 + kRate / 5; ++i) {
            curvOut = std::max(curvOut, static_cast<double>(std::abs(out[i + 1] - 2 * out[i] + out[i - 1])));
            curvDry = std::max(curvDry, static_cast<double>(std::abs(dry[i + 1] - 2 * dry[i] + dry[i - 1])));
        }
        std::cout << std::setprecision(4) << "  note A " << hzA << " Hz, Bypass note " << hzB << " Hz (" << bypassCents
                  << " cents from dry), level " << levelDb << " dB, boundary curvature out/dry "
                  << curvOut << '/' << curvDry << ", underruns " << underruns << '\n';
        check(underruns == 0, "bypass phrase: no underruns");
        check(std::abs(1200.0 * std::log2(hzA / 220.0)) <= 25.0, "bypass phrase: first note corrected");
        check(std::abs(bypassCents) <= 5.0 && std::abs(levelDb) <= 0.5, "bypass phrase: Bypass note keeps its own pitch and level");
        check(curvOut <= 2.0 * curvDry + 0.005, "bypass phrase: no glitch at the eligibility boundary");
    }

    // ---- realtime pacing with TUNA_RENDERER=splice; skipped on a busy machine
    std::cout << "realtime pacing, TUNA_RENDERER=splice, 48 kHz, 10 s synthetic vowel\n";
    const double load = systemLoadEquivalentCores();
    if (load > 4.0) {
        std::cout << "  SKIPPED: system busy estimate " << load << " is above 4 equivalent cores, so pacing results would not be attributable\n";
    } else {
        cody_pitch::Settings settings;
        settings.allowedNotes = cody_pitch::scaleMask(9, 1);
        settings.amount = 1.0f; settings.retuneMs = 0.0f; settings.transitionMs = 0.0f;
        settings.toleranceCents = 0.0f; settings.expression = 0.0f;
        cody_pitch::RealtimeEngine engine;
        engine.prepare(kRate, [&settings] { return settings; });
        const int duration = 10 * kRate, block = 256;
        const auto t0 = std::chrono::steady_clock::now();
        double phase = 0.0;
        for (int offset = 0; offset < duration; offset += block) {
            float samples[256] {};
            const int count = std::min(block, duration - offset);
            for (int i = 0; i < count; ++i) {
                const double t = (offset + i) / static_cast<double>(kRate);
                phase += 2 * kPi * 205.0 * (1.0 + 0.01 * std::sin(2 * kPi * 5.0 * t)) / kRate;
                double v = 0.0;
                for (int h = 1; h <= 20; ++h) v += std::sin(h * phase) / h;
                samples[i] = static_cast<float>(0.12 * v);
            }
            engine.process(samples, count, true);
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(offset + count) * 1000000 / kRate));
        }
        const int underruns = engine.underrunCount();
        std::cout << "  load " << load << ", underruns=" << underruns << " longest_render_ms=" << engine.longestRenderMicros() / 1000.0
                  << " longest_renderer_ms=" << engine.longestRendererMicros() / 1000.0
                  << " longest_analysis_ms=" << engine.longestAnalysisMicros() / 1000.0 << '\n';
        engine.stop();
        check(underruns == 0, "no realtime underruns with the splice renderer");
    }
    std::cout << (failures ? "waveform renderer test FAILED\n" : "waveform renderer test passed\n");
    return failures ? 1 : 0;
}
