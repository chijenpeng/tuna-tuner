#include "PitchEngine.h"
#include "Experiment.h"

#include <world/dio.h>
#include <world/stonemask.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace cody_pitch {
namespace {

constexpr double frameMs = 5.0;
double hzToCents(double hz, double reference) { return 6900.0 + 1200.0 * std::log2(hz / reference); }
double centsToHz(double cents, double reference) { return reference * std::exp2((cents - 6900.0) / 1200.0); }

double f0At(const std::vector<double>& f0, int sample, int sampleRate) {
    const double position = sample * 1000.0 / (sampleRate * frameMs);
    const int before = std::clamp(static_cast<int>(position), 0, static_cast<int>(f0.size()) - 1);
    const int after = std::min(before + 1, static_cast<int>(f0.size()) - 1);
    const double first = f0[static_cast<size_t>(before)];
    const double second = f0[static_cast<size_t>(after)];
    if (first <= 0.0 || second <= 0.0)
        return f0[static_cast<size_t>(std::clamp(static_cast<int>(std::lround(position)),
                                                  0, static_cast<int>(f0.size()) - 1))];
    const double fraction = std::clamp(position - before, 0.0, 1.0);
    return first * (1.0 - fraction) + second * fraction;
}

float rmsAt(const std::vector<float>& x, int center, int radius) {
    const int lo = std::max(0, center - radius);
    const int hi = std::min(static_cast<int>(x.size()), center + radius);
    double energy = 0.0;
    for (int i = lo; i < hi; ++i)
        energy += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return hi > lo ? static_cast<float>(std::sqrt(energy / (hi - lo))) : 0.0f;
}

std::vector<float> localPeakEnvelope(const std::vector<float>& x, int radius) {
    std::vector<float> result(x.size(), 0.0f);
    std::deque<int> candidates;
    int added = 0;
    for (int center = 0; center < static_cast<int>(x.size()); ++center) {
        const int right = std::min(static_cast<int>(x.size()) - 1, center + radius);
        while (added <= right) {
            const float level = std::abs(x[static_cast<size_t>(added)]);
            while (!candidates.empty() && std::abs(x[static_cast<size_t>(candidates.back())]) <= level)
                candidates.pop_back();
            candidates.push_back(added++);
        }
        const int left = center - radius;
        while (!candidates.empty() && candidates.front() < left) candidates.pop_front();
        result[static_cast<size_t>(center)] = std::abs(x[static_cast<size_t>(candidates.front())]);
    }
    return result;
}

// A periodicity gate rejects breaths/noise even when an F0 tracker proposes a note.
double periodicityAt(const std::vector<float>& x, int center, int period) {
    const int radius = std::max(period * 2, 64);
    const int lo = std::max(0, center - radius);
    const int hi = std::min(static_cast<int>(x.size()) - period, center + radius);
    if (hi - lo < period * 2) return 0.0;
    double cross = 0.0, left = 0.0, right = 0.0;
    for (int i = lo; i < hi; ++i) {
        const double a = x[static_cast<size_t>(i)];
        const double b = x[static_cast<size_t>(i + period)];
        cross += a * b;
        left += a * a;
        right += b * b;
    }
    return cross / std::sqrt(std::max(1.0e-18, left * right));
}

double bestPeriodicityNear(const std::vector<float>& x, int center, int period, int spread) {
    double best = 0.0;
    for (int candidate = std::max(1, period - spread); candidate <= period + spread; candidate += 2)
        best = std::max(best, periodicityAt(x, center, candidate));
    return best;
}

std::vector<double> targetNotes(const Settings& settings) {
    std::vector<double> result;
    for (int midi = 24; midi <= 108; ++midi) {
        if (settings.allowedNotes[static_cast<size_t>(midi % 12)]) {
            const double hz = centsToHz(midi * 100.0, settings.referenceHz);
            if (hz >= settings.minHz * 0.8 && hz <= settings.maxHz * 1.2)
                result.push_back(midi * 100.0);
        }
    }
    return result;
}

// A whole-phrase path avoids frame-by-frame snapping between two nearby notes.
double noteDistanceCost(double source, double note) {
    const double semitones = std::abs(source - note) / 100.0;
    // Keep far notes distinguishable. Capping every distance at four semitones
    // made a 340 Hz singer snap to A2 instead of the much closer A4 when the
    // allowed-note mask contained only A's.
    return semitones <= 4.0 ? semitones * semitones : 16.0 + 8.0 * (semitones - 4.0);
}

void decodeTargets(const std::vector<double>& source, const std::vector<bool>& voiced,
                   const std::vector<double>& notes, std::vector<double>& targets,
                   double transitionPenalty) {
    const int count = static_cast<int>(source.size());
    const int states = static_cast<int>(notes.size());
    for (int start = 0; start < count;) {
        if (!voiced[static_cast<size_t>(start)]) { ++start; continue; }
        int end = start;
        while (end < count && voiced[static_cast<size_t>(end)]) ++end;

        std::vector<double> previous(static_cast<size_t>(states)), current(static_cast<size_t>(states));
        std::vector<int> back(static_cast<size_t>(end - start) * states);
        for (int s = 0; s < states; ++s)
            previous[static_cast<size_t>(s)] = noteDistanceCost(source[static_cast<size_t>(start)], notes[static_cast<size_t>(s)]);

        for (int frame = start + 1; frame < end; ++frame) {
            for (int s = 0; s < states; ++s) {
                double best = previous[static_cast<size_t>(s)];
                int chosen = s;
                for (int p = 0; p < states; ++p) {
                    if (p == s) continue;
                    const double transitionCost = transitionPenalty
                        + 0.5 * std::abs(notes[static_cast<size_t>(s)] - notes[static_cast<size_t>(p)]) / 100.0;
                    const double candidate = previous[static_cast<size_t>(p)] + transitionCost;
                    if (candidate < best) { best = candidate; chosen = p; }
                }
                current[static_cast<size_t>(s)] = best
                    + noteDistanceCost(source[static_cast<size_t>(frame)], notes[static_cast<size_t>(s)]);
                back[static_cast<size_t>(frame - start) * states + s] = chosen;
            }
            previous.swap(current);
        }
        int state = static_cast<int>(std::min_element(previous.begin(), previous.end()) - previous.begin());
        for (int frame = end - 1; frame >= start; --frame) {
            targets[static_cast<size_t>(frame)] = notes[static_cast<size_t>(state)];
            if (frame > start)
                state = back[static_cast<size_t>(frame - start) * states + state];
        }
        start = end;
    }
}

// The streaming worker re-analyzes overlapping 440 ms windows. A Viterbi path
// over each *entire* window can revise a target 40 ms later even though the
// source pitch at the published time has not changed. For natural speeds the
// 150 ms source-F0 median already provides the temporal evidence: choose its
// nearest note independently of the window's moving start and end. Keep the
// immediate Viterbi path for hard retuning, where it had no observed seam flips.
void decodeTargetsLocal(const std::vector<double>& source, const std::vector<bool>& voiced,
                        const std::vector<double>& notes, std::vector<double>& targets) {
    const int count = static_cast<int>(source.size());
    for (int i = 0; i < count; ++i) {
        if (!voiced[static_cast<size_t>(i)]) continue;
        double bestCost = std::numeric_limits<double>::infinity();
        double bestNote = 0.0;
        for (const double note : notes) {
            const double cost = noteDistanceCost(source[static_cast<size_t>(i)], note);
            if (cost < bestCost) { bestCost = cost; bestNote = note; }
        }
        targets[static_cast<size_t>(i)] = bestNote;
    }
}

// Optional target mode 1: symmetric majority filter of the note choice over +-radius
// frames. Uses only nearby frames, so published frames never see the window edge.
void modeFilterTargets(const std::vector<bool>& eligible, std::vector<double>& targets, int radius) {
    const int count = static_cast<int>(targets.size());
    const std::vector<double> input = targets;
    for (int i = 0; i < count; ++i) {
        if (!eligible[static_cast<size_t>(i)]) continue;
        double best = input[static_cast<size_t>(i)];
        int bestCount = 0;
        for (int a = std::max(0, i - radius); a <= std::min(count - 1, i + radius); ++a) {
            if (!eligible[static_cast<size_t>(a)]) continue;
            int n = 0;
            for (int b = std::max(0, i - radius); b <= std::min(count - 1, i + radius); ++b)
                if (eligible[static_cast<size_t>(b)] && input[static_cast<size_t>(b)] == input[static_cast<size_t>(a)]) ++n;
            if (n > bestCount || (n == bestCount && input[static_cast<size_t>(a)] == input[static_cast<size_t>(i)])) {
                bestCount = n; best = input[static_cast<size_t>(a)];
            }
        }
        targets[static_cast<size_t>(i)] = best;
    }
}

// Optional target mode 2: nearest note with hysteresis, causal inside each island.
void decodeTargetsHysteresis(const std::vector<double>& source, const std::vector<bool>& voiced,
                             const std::vector<double>& notes, std::vector<double>& targets,
                             double hysteresisCents) {
    const int count = static_cast<int>(source.size());
    double current = 0.0;
    bool have = false;
    for (int i = 0; i < count; ++i) {
        if (!voiced[static_cast<size_t>(i)]) { have = false; continue; }
        const double s = source[static_cast<size_t>(i)];
        double nearest = notes.front();
        for (const double note : notes)
            if (std::abs(s - note) < std::abs(s - nearest)) nearest = note;
        // Beyond the midpoint by H cents == distance to current exceeds distance to nearest by 2H.
        if (!have || (nearest != current && std::abs(s - current) - std::abs(s - nearest) >= 2.0 * hysteresisCents)) {
            current = nearest; have = true;
        }
        targets[static_cast<size_t>(i)] = current;
    }
}

// A local peak near each predicted period supplies pitch-synchronous grains.
std::vector<int> pitchMarks(const std::vector<float>& input, int sampleRate,
                            const std::vector<double>& f0, int start, int end,
                            int anchor = -1) {
    std::vector<int> marks;
    const auto refine = [&](int center, int period, int previous) {
        const int radius = std::max(2, previous < 0 ? period / 2 : period / 4);
        int best = std::clamp(center, start, end - 1);
        double bestScore = -std::numeric_limits<double>::infinity();
        float localPeak = 0.0f;
        for (int i = std::max(start, center - radius); i < std::min(end, center + radius + 1); ++i)
            localPeak = std::max(localPeak, std::abs(input[static_cast<size_t>(i)]));
        for (int i = std::max(start, center - radius); i < std::min(end, center + radius + 1); ++i) {
            const float value = input[static_cast<size_t>(i)];
            double score = value;
            if (previous >= 0) {
                // A vowel's largest positive peak moves with the formant mix;
                // following it alone creates pitch-mark jitter. Prefer the
                // next cycle that best matches the preceding waveform shape.
                const int half = std::max(8, period / 2);
                double cross = 0.0, energyA = 0.0, energyB = 0.0;
                for (int offset = -half; offset <= half; ++offset) {
                    const int aIndex = previous + offset;
                    const int bIndex = i + offset;
                    if (aIndex < start || aIndex >= end || bIndex < start || bIndex >= end) continue;
                    const double a = input[static_cast<size_t>(aIndex)];
                    const double b = input[static_cast<size_t>(bIndex)];
                    cross += a * b;
                    energyA += a * a;
                    energyB += b * b;
                }
                const double similarity = cross / std::sqrt(std::max(1.0e-18, energyA * energyB));
                score = similarity + 0.10 * value / std::max(0.001f, localPeak)
                    - 0.05 * std::abs(i - center) / static_cast<double>(period);
            }
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    };
    const auto extend = [&](int first, int direction, std::vector<int>& destination) {
        int current = first;
        while (true) {
            const double hz = f0At(f0, current, sampleRate);
            if (hz <= 0.0) break;
            const int period = std::clamp(static_cast<int>(std::lround(sampleRate / hz)),
                                          15, sampleRate / 55);
            const int predicted = current + direction * period;
            if (predicted <= start || predicted >= end) break;
            const int mark = refine(predicted, period, current);
            if (std::abs(mark - current) <= period / 2) break;
            destination.push_back(mark);
            current = mark;
        }
    };
    if (anchor >= start && anchor < end) {
        marks.push_back(anchor);
        extend(anchor, +1, marks);
        std::vector<int> before;
        extend(anchor, -1, before);
        std::reverse(before.begin(), before.end());
        marks.insert(marks.begin(), before.begin(), before.end());
    } else {
        const int first = start + static_cast<int>(sampleRate /
            std::max(80.0, f0At(f0, start, sampleRate)));
        if (first < end) {
            const int period = std::clamp(static_cast<int>(std::lround(sampleRate /
                std::max(80.0, f0At(f0, first, sampleRate)))), 15, sampleRate / 55);
            const int mark = refine(first, period, -1);
            marks.push_back(mark);
            extend(mark, +1, marks);
        }
    }
    return marks;
}

void addGrain(const std::vector<float>& input, std::vector<double>& wet,
              std::vector<double>& weight, int sourceCenter, int outputCenter, int radius) {
    constexpr double pi = 3.14159265358979323846;
    for (int offset = -radius; offset <= radius; ++offset) {
        const int src = sourceCenter + offset;
        const int dst = outputCenter + offset;
        if (src < 0 || src >= static_cast<int>(input.size()) || dst < 0 || dst >= static_cast<int>(wet.size())) continue;
        const double window = 0.5 + 0.5 * std::cos(pi * offset / radius);
        wet[static_cast<size_t>(dst)] += input[static_cast<size_t>(src)] * window;
        weight[static_cast<size_t>(dst)] += window;
    }
}

struct Lowpass {
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    Lowpass(int sampleRate, double cutoff, double q) {
        constexpr double pi = 3.14159265358979323846;
        const double k = std::tan(pi * cutoff / sampleRate);
        const double normal = 1.0 / (1.0 + k / q + k * k);
        b0 = k * k * normal;
        b1 = 2.0 * b0;
        b2 = b0;
        a1 = 2.0 * (k * k - 1.0) * normal;
        a2 = (1.0 - k / q + k * k) * normal;
    }
    double process(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

void zeroPhaseLowpass(std::vector<double>& signal, int sampleRate, double cutoff) {
    // The host already delays publication by 300 ms, so the worker can filter
    // both directions inside its 200 ms context. A causal low-pass in
    // dry + LP(wet) - LP(dry) leaves the original fundamental behind because
    // dry and LP(dry) have different phases, even far below the cutoff.
    Lowpass forward1(sampleRate, cutoff, 0.5411961001);
    Lowpass forward2(sampleRate, cutoff, 1.3065629649);
    for (double& sample : signal)
        sample = forward2.process(forward1.process(sample));
    Lowpass reverse1(sampleRate, cutoff, 0.5411961001);
    Lowpass reverse2(sampleRate, cutoff, 1.3065629649);
    for (auto it = signal.rbegin(); it != signal.rend(); ++it)
        *it = reverse2.process(reverse1.process(*it));
}

} // namespace

std::array<bool, 12> scaleMask(int root, int scaleIndex) {
    static constexpr int intervals[13][12] = {
        {0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1}, // major
        {0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1}, // natural minor
        {0, 2, 3, 5, 7, 8, 11, -1, -1, -1, -1, -1}, // harmonic minor
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},      // chromatic
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},      // custom is overridden by the note mask
        {0, 2, 3, 5, 7, 9, 11, -1, -1, -1, -1, -1}, // melodic minor (ascending)
        {0, 2, 3, 5, 7, 9, 10, -1, -1, -1, -1, -1}, // dorian
        {0, 1, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1}, // phrygian
        {0, 2, 4, 6, 7, 9, 11, -1, -1, -1, -1, -1}, // lydian
        {0, 2, 4, 5, 7, 9, 10, -1, -1, -1, -1, -1}, // mixolydian
        {0, 3, 5, 6, 7, 10, -1, -1, -1, -1, -1, -1}, // blues
        {0, 2, 4, 7, 9, -1, -1, -1, -1, -1, -1, -1}, // major pentatonic
        {0, 3, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1} // minor pentatonic
    };
    std::array<bool, 12> mask {};
    root = ((root % 12) + 12) % 12;
    scaleIndex = std::clamp(scaleIndex, 0, 12);
    for (int offset : intervals[scaleIndex])
        if (offset >= 0) mask[static_cast<size_t>((root + offset) % 12)] = true;
    return mask;
}

Result render(const std::vector<float>& mono, int sampleRate, const Settings& settings,
              const RenderTiming* timing, bool synthesizeAudio) {
    if (sampleRate < 16000 || mono.empty()) throw std::invalid_argument("Expected nonempty mono audio at 16 kHz or above");
    if (settings.minHz < 50 || settings.maxHz <= settings.minHz || settings.maxHz > sampleRate / 4)
        throw std::invalid_argument("Invalid vocal range");
    if (settings.referenceHz < 400 || settings.referenceHz > 480 || settings.vibratoGain < 0 || settings.vibratoGain > 2)
        throw std::invalid_argument("Invalid tuning reference or vibrato gain");

    Result result;
    if (synthesizeAudio) result.audio = mono;
    if (settings.amount <= 0.0f) return result;
    if (std::none_of(settings.allowedNotes.begin(), settings.allowedNotes.end(), [](bool value) { return value; })) {
        result.warning = "No target notes are enabled; audio passed through unchanged";
        return result;
    }

    const auto notes = targetNotes(settings);
    if (notes.empty()) throw std::invalid_argument("No allowed note falls within the vocal range");
    const int samples = static_cast<int>(mono.size());
    std::vector<double> input(mono.begin(), mono.end());
    DioOption option;
    InitializeDioOption(&option);
    option.f0_floor = settings.minHz;
    option.f0_ceil = settings.maxHz;
    option.frame_period = frameMs;
    const int count = GetSamplesForDIO(sampleRate, samples, frameMs);
    std::vector<double> times(static_cast<size_t>(count));
    std::vector<double> rawF0(static_cast<size_t>(count));
    std::vector<double> f0(static_cast<size_t>(count));
    Dio(input.data(), samples, sampleRate, &option, times.data(), rawF0.data());
    StoneMask(input.data(), samples, sampleRate, times.data(), rawF0.data(), count, f0.data());

    std::vector<double> cents(static_cast<size_t>(count), 0.0);
    std::vector<double> center(static_cast<size_t>(count), 0.0);
    std::vector<double> target(static_cast<size_t>(count), 0.0);
    std::vector<double> desiredF0(static_cast<size_t>(count), 0.0);
    std::vector<bool> voiced(static_cast<size_t>(count), false);
    const float peak = *std::max_element(mono.begin(), mono.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    const float energyFloor = std::max(0.0003f, std::abs(peak) * 0.003f);
    for (int i = 0; i < count; ++i) {
        const int sample = std::min(samples - 1, static_cast<int>(std::lround(times[static_cast<size_t>(i)] * sampleRate)));
        double hz = f0[static_cast<size_t>(i)];
        // DIO can lock onto the second harmonic of a bright vowel. Check the
        // waveform period before accepting an octave-high estimate; otherwise
        // a 400 Hz note can be incorrectly sent toward 800 Hz.
        if (hz >= settings.minHz * 2.0 && hz * 0.5 <= settings.maxHz) {
            const int period = static_cast<int>(std::lround(sampleRate / hz));
            // The octave error can be slightly sharp/flat, so searching only
            // the exact doubled DIO period misses the true lower fundamental.
            const double shortCycle = bestPeriodicityNear(mono, sample, period,
                std::max(2, period / 8));
            const double longCycle = bestPeriodicityNear(mono, sample, period * 2,
                std::max(4, period / 3));
            if (longCycle > 0.70 && longCycle > shortCycle + 0.12)
                hz *= 0.5;
        }
        f0[static_cast<size_t>(i)] = hz;
        voiced[static_cast<size_t>(i)] = hz >= settings.minHz && hz <= settings.maxHz
            && rmsAt(mono, sample, sampleRate / 100) >= energyFloor
            && periodicityAt(mono, sample, static_cast<int>(std::lround(sampleRate / std::max(1.0, hz)))) >= flags().periodicityGate;
        if (voiced[static_cast<size_t>(i)]) {
            cents[static_cast<size_t>(i)] = hzToCents(hz, settings.referenceHz);
        } else {
            f0[static_cast<size_t>(i)] = 0.0;
        }
    }

    // Reject isolated tracker hits before note decoding and rendering.
    for (int start = 0; start < count;) {
        if (!voiced[static_cast<size_t>(start)]) { ++start; continue; }
        int end = start;
        while (end < count && voiced[static_cast<size_t>(end)]) ++end;
        // A run cannot be longer than the window: at the shortest latency (70 ms window) the original 80 ms rule
        // would reject every vowel, so it is capped at half the window (unchanged for windows of 32 frames or more).
        const int minimumFrames = std::min(settings.retuneMs <= 2.0f ? 8 : 16, std::max(4, count / 2));
        if (end - start < minimumFrames)
            for (int i = start; i < end; ++i) { voiced[static_cast<size_t>(i)] = false; f0[static_cast<size_t>(i)] = 0.0; }
        start = end;
    }
    // A single missed 5 ms tracker frame inside an otherwise continuous
    // vowel must not become a WORLD voiced/unvoiced edge. Its resynthesis
    // impulse response spreads that tiny hole over tens of milliseconds and
    // is heard as a blip at note changes. Close only isolated, same-pitch
    // holes; real consonants and note separations remain unvoiced.
    for (int i = 1; i + 1 < count; ++i) {
        if (voiced[static_cast<size_t>(i)]
            || !voiced[static_cast<size_t>(i - 1)]
            || !voiced[static_cast<size_t>(i + 1)]) continue;
        const double before = cents[static_cast<size_t>(i - 1)];
        const double after = cents[static_cast<size_t>(i + 1)];
        if (std::abs(after - before) > 65.0) continue;
        const int sample = std::min(samples - 1,
            static_cast<int>(std::lround(times[static_cast<size_t>(i)] * sampleRate)));
        if (rmsAt(mono, sample, sampleRate / 100) < energyFloor) continue;
        cents[static_cast<size_t>(i)] = 0.5 * (before + after);
        f0[static_cast<size_t>(i)] = centsToHz(cents[static_cast<size_t>(i)], settings.referenceHz);
        voiced[static_cast<size_t>(i)] = true;
    }
    // Optional voicing-gap fill: bridge longer unvoiced gaps inside an island when both
    // sides are voiced, the pitch step across is <= 150 cents and every gap frame has energy.
    if (flags().voicingBridgeMs > 0.0 && settings.contextMs >= 70.0) { // needs context to judge both sides of the gap
        const int maxGap = static_cast<int>(std::lround(std::min(flags().voicingBridgeMs, settings.contextMs - 40.0) / frameMs));
        for (int i = 1; i < count;) {
            if (voiced[static_cast<size_t>(i)] || !voiced[static_cast<size_t>(i - 1)]) { ++i; continue; }
            int end = i;
            while (end < count && !voiced[static_cast<size_t>(end)]) ++end;
            const int length = end - i;
            if (flags().voicingBridgeMode == 1) {
                // Evidence fill: a gap frame is filled only if it has energy AND the waveform repeats at a
                // period near the cents-interpolated one; its F0 comes from the best lag. Stop at the first
                // frame that fails, so a genuine phonation break stays a break.
                if (end < count && length <= maxGap
                    && std::abs(cents[static_cast<size_t>(end)] - cents[static_cast<size_t>(i - 1)]) <= 150.0) {
                    const double before = cents[static_cast<size_t>(i - 1)];
                    const double after = cents[static_cast<size_t>(end)];
                    for (int j = i; j < end; ++j) {
                        const int sample = std::min(samples - 1,
                            static_cast<int>(std::lround(times[static_cast<size_t>(j)] * sampleRate)));
                        if (rmsAt(mono, sample, sampleRate / 100) < energyFloor) break;
                        const double t = static_cast<double>(j - i + 1) / (length + 1);
                        const double hzGuess = centsToHz(before + (after - before) * t, settings.referenceHz);
                        const double guess = sampleRate / hzGuess;
                        const int lo = std::max(2, static_cast<int>(std::floor(guess * 0.92)));
                        const int hi = static_cast<int>(std::ceil(guess * 1.08));
                        std::vector<double> score(static_cast<size_t>(hi - lo + 1));
                        int bestIndex = 0;
                        for (int lag = lo; lag <= hi; ++lag) {
                            score[static_cast<size_t>(lag - lo)] = periodicityAt(mono, sample, lag);
                            if (score[static_cast<size_t>(lag - lo)] > score[static_cast<size_t>(bestIndex)])
                                bestIndex = lag - lo;
                        }
                        if (score[static_cast<size_t>(bestIndex)] < flags().periodicityGate) break;
                        double lagRefined = lo + bestIndex;
                        if (bestIndex > 0 && bestIndex + 1 < static_cast<int>(score.size())) {
                            const double a = score[static_cast<size_t>(bestIndex - 1)];
                            const double b = score[static_cast<size_t>(bestIndex)];
                            const double c = score[static_cast<size_t>(bestIndex + 1)];
                            const double denominator = a - 2.0 * b + c;
                            if (std::abs(denominator) > 1.0e-12) lagRefined += 0.5 * (a - c) / denominator;
                        }
                        const double hz = sampleRate / lagRefined;
                        if (hz < settings.minHz || hz > settings.maxHz) break;
                        f0[static_cast<size_t>(j)] = hz;
                        cents[static_cast<size_t>(j)] = hzToCents(hz, settings.referenceHz);
                        voiced[static_cast<size_t>(j)] = true;
                    }
                }
                i = end;
                continue;
            }
            bool ok = end < count && length <= maxGap
                && std::abs(cents[static_cast<size_t>(end)] - cents[static_cast<size_t>(i - 1)]) <= 150.0;
            for (int j = i; ok && j < end; ++j) {
                const int sample = std::min(samples - 1,
                    static_cast<int>(std::lround(times[static_cast<size_t>(j)] * sampleRate)));
                ok = rmsAt(mono, sample, sampleRate / 100) >= energyFloor;
            }
            if (ok) {
                const double before = cents[static_cast<size_t>(i - 1)];
                const double after = cents[static_cast<size_t>(end)];
                for (int j = i; j < end; ++j) {
                    const double t = static_cast<double>(j - i + 1) / (length + 1);
                    cents[static_cast<size_t>(j)] = before + (after - before) * t;
                    f0[static_cast<size_t>(j)] = centsToHz(cents[static_cast<size_t>(j)], settings.referenceHz);
                    voiced[static_cast<size_t>(j)] = true;
                }
            }
            i = end;
        }
    }
    result.voicedFrames = static_cast<int>(std::count(voiced.begin(), voiced.end(), true));

    // Center is a 150 ms local median (+-75 ms). Its right half must lie inside the context that exists at this latency:
    // half-window = min(75 ms, context - 45 ms), or context - 25 ms below 70 ms of context; at the 200 ms context this
    // is the original +-15 frames.
    const double contextMs = settings.contextMs;
    const double boundMs = contextMs >= 70.0 ? std::min(75.0, contextMs - 45.0) : std::max(frameMs, contextMs - 25.0);
    const int centreHalf = std::max(1, static_cast<int>(std::lround(boundMs / frameMs)));
    std::vector<double> neighborhood;
    for (int i = 0; i < count; ++i) {
        if (!voiced[static_cast<size_t>(i)]) continue;
        neighborhood.clear();
        for (int j = std::max(0, i - centreHalf); j <= std::min(count - 1, i + centreHalf); ++j)
            if (voiced[static_cast<size_t>(j)]) neighborhood.push_back(cents[static_cast<size_t>(j)]);
        auto middle = neighborhood.begin() + static_cast<std::ptrdiff_t>(neighborhood.size() / 2);
        std::nth_element(neighborhood.begin(), middle, neighborhood.end());
        center[static_cast<size_t>(i)] = *middle;
    }
    // The target-centering window changes note selection timing without
    // changing correction or residual handling.
    std::vector<double> targetCenter = center;
    if (flags().targetCenterMs > 0.0) {
        const int half = std::max(0, std::min(centreHalf, static_cast<int>(std::lround(flags().targetCenterMs / (2.0 * frameMs)))));
        for (int i = 0; i < count; ++i) {
            if (!voiced[static_cast<size_t>(i)]) continue;
            neighborhood.clear();
            for (int j = std::max(0, i - half); j <= std::min(count - 1, i + half); ++j)
                if (voiced[static_cast<size_t>(j)]) neighborhood.push_back(cents[static_cast<size_t>(j)]);
            auto middle = neighborhood.begin() + static_cast<std::ptrdiff_t>(neighborhood.size() / 2);
            std::nth_element(neighborhood.begin(), middle, neighborhood.end());
            targetCenter[static_cast<size_t>(i)] = *middle;
        }
    }
    // Bypass belongs to the detected *source* note, not the chosen target. Break
    // the correction island here so PSOLA never resynthesizes its steady centre.
    std::vector<bool> correctionEligible = voiced;
    for (int i = 0; i < count; ++i) {
        if (!voiced[static_cast<size_t>(i)]) continue;
        const int sourceMidi = static_cast<int>(std::lround(center[static_cast<size_t>(i)] / 100.0));
        const int pitchClass = ((sourceMidi % 12) + 12) % 12;
        correctionEligible[static_cast<size_t>(i)] = !settings.bypassNotes[static_cast<size_t>(pitchClass)];
    }
    // At natural speeds, use the window-invariant median above. Faster retuning
    // decodes across frames so targets switch promptly at note boundaries.
    const double transitionPenalty = std::clamp(
        18.0 * static_cast<double>(settings.retuneMs) / 45.0, 0.25, 18.0);
    // Optional target-mode selection (0 uses the default path below).
    const int targetMode = flags().targetMode;
    if (targetMode == 3) {
        decodeTargets(targetCenter, correctionEligible, notes, target, transitionPenalty);
    } else if (targetMode == 2) {
        decodeTargetsHysteresis(targetCenter, correctionEligible, notes, target, flags().targetHystCents);
    } else if (targetMode == 1) {
        decodeTargetsLocal(targetCenter, correctionEligible, notes, target);
        modeFilterTargets(correctionEligible, target, 6);
    } else if (settings.retuneMs > 2.0f)
        decodeTargetsLocal(targetCenter, correctionEligible, notes, target);
    else
        decodeTargets(targetCenter, correctionEligible, notes, target, transitionPenalty);

    // Suspend correction while the source pitch moves quickly, then restore it
    // over the release time. Close immediately with one frame of lookahead. With
    // the gate closed the desired pitch equals the source, so a waveform renderer
    // passes it through.
    std::vector<double> velocityGate(static_cast<size_t>(count), 1.0);
    if (flags().velocityGateCpms > 0.0 && flags().velocityGateMode == 1) {
        // Robust mode: median-smoothed track, +-2 frame central difference, a
        // gesture must also be large (excursion) and persist two frames.
        const double high = flags().velocityGateCpms;
        const double low = 0.6 * high;
        const auto isVoiced = [&](int k) { return k >= 0 && k < count && voiced[static_cast<size_t>(k)]; };
        std::vector<double> smooth(cents.begin(), cents.begin() + count);
        for (int i = 1; i + 1 < count; ++i) {
            if (!isVoiced(i - 1) || !isVoiced(i) || !isVoiced(i + 1)) continue;
            const double a = cents[static_cast<size_t>(i - 1)], b = cents[static_cast<size_t>(i)], c = cents[static_cast<size_t>(i + 1)];
            smooth[static_cast<size_t>(i)] = std::max(std::min(a, b), std::min(std::max(a, b), c));
        }
        std::vector<double> speed(static_cast<size_t>(count), 0.0);
        std::vector<char> fast(static_cast<size_t>(count), 0);
        for (int i = 0; i < count; ++i) {
            if (!isVoiced(i)) continue;
            int before = i, after = i;
            while (before > i - 2 && isVoiced(before - 1)) --before;
            while (after < i + 2 && isVoiced(after + 1)) ++after;
            if (after == before) continue;
            const double excursion = std::abs(smooth[static_cast<size_t>(after)] - smooth[static_cast<size_t>(before)]);
            const double v = excursion / ((after - before) * frameMs);
            speed[static_cast<size_t>(i)] = v;
            fast[static_cast<size_t>(i)] = v >= high && excursion >= flags().velocityGateMinCents ? 1 : 0;
        }
        const double step = frameMs / std::max(frameMs, flags().velocityGateReleaseMs);
        double state = 1.0;
        bool held = false;
        int quiet = 0;
        for (int i = 0; i < count; ++i) {
            if (!isVoiced(i)) { state = 1.0; held = false; quiet = 0; continue; }
            const bool persistent = fast[static_cast<size_t>(i)]
                && ((isVoiced(i + 1) && fast[static_cast<size_t>(i + 1)]) || (isVoiced(i - 1) && fast[static_cast<size_t>(i - 1)]));
            if (persistent) { held = true; quiet = 0; state = 0.0; }
            else if (held) {
                quiet = speed[static_cast<size_t>(i)] < low ? quiet + 1 : 0;
                if (quiet >= 2) held = false;
            }
            if (!held && !persistent) state = std::min(1.0, state + step);
            velocityGate[static_cast<size_t>(i)] = state;
        }
    } else if (flags().velocityGateCpms > 0.0) {
        const double high = flags().velocityGateCpms;
        const double low = 0.6 * high;
        std::vector<double> speed(static_cast<size_t>(count), 0.0);
        for (int i = 0; i < count; ++i) {
            if (!voiced[static_cast<size_t>(i)]) continue;
            const int before = i > 0 && voiced[static_cast<size_t>(i - 1)] ? i - 1 : i;
            const int after = i + 1 < count && voiced[static_cast<size_t>(i + 1)] ? i + 1 : i;
            if (after == before) continue;
            speed[static_cast<size_t>(i)] = std::abs(cents[static_cast<size_t>(after)] - cents[static_cast<size_t>(before)])
                / ((after - before) * frameMs);
        }
        double state = 1.0;
        const double step = frameMs / std::max(frameMs, flags().velocityGateReleaseMs);
        for (int i = 0; i < count; ++i) {
            if (!voiced[static_cast<size_t>(i)]) { state = 1.0; continue; }
            const double v = std::max(speed[static_cast<size_t>(i)],
                i + 1 < count ? speed[static_cast<size_t>(i + 1)] : 0.0);
            const double position = std::clamp((v - low) / std::max(1e-9, high - low), 0.0, 1.0);
            const double open = 1.0 - position * position * (3.0 - 2.0 * position);
            state = std::min(open, state + step);
            velocityGate[static_cast<size_t>(i)] = state;
        }
    }

    double correction = 0.0;
    int noteAge = 0;
    double lastTarget = 0.0;
    for (int i = 0; i < count; ++i) {
        if (!correctionEligible[static_cast<size_t>(i)]) {
            correction = 0.0; noteAge = 0; lastTarget = 0.0;
            if (voiced[static_cast<size_t>(i)]) {
                target[static_cast<size_t>(i)] = cents[static_cast<size_t>(i)];
                desiredF0[static_cast<size_t>(i)] = f0[static_cast<size_t>(i)];
            }
            continue;
        }
        const double chosen = target[static_cast<size_t>(i)];
        if (chosen != lastTarget) { noteAge = 0; lastTarget = chosen; }
        ++noteAge;
        const double error = chosen - center[static_cast<size_t>(i)];
        const double tolerance = std::clamp(static_cast<double>(settings.toleranceCents), 0.0, 100.0);
        // Flex Tune is a protected region around the target, not a fixed
        // detuning offset on every corrected note. Outside that region the
        // full pitch error should be corrected; otherwise a 70-cent miss with
        // Flex at 20 can remain audibly 20 cents off even after settling.
        const double distance = std::abs(error);
        const double flexPosition = tolerance <= 0.0 ? 1.0
            : std::clamp((distance - tolerance) / tolerance, 0.0, 1.0);
        const double flexWeight = flexPosition * flexPosition * (3.0 - 2.0 * flexPosition);
        const double gated = error * flexWeight;
        const double transition = settings.transitionMs <= 0.01f ? 1.0
            : std::clamp(noteAge * frameMs / static_cast<double>(settings.transitionMs), 0.0, 1.0);
        const double command = std::clamp(static_cast<double>(settings.amount) * gated * transition, -450.0, 450.0);
        const double ageMs = noteAge * frameMs;
        // Streaming renders overlap 440 ms windows. The published centre begins
        // about 200 ms into each window, so a 250 ms onset made Humanize nearly
        // inaudible even on a held note. Begin relaxing after the first 100 ms.
        const double relax = settings.sustainRelax * std::clamp((ageMs - 100.0) / 200.0, 0.0, 1.0);
        const double tau = std::max(0.0, static_cast<double>(settings.retuneMs) * (1.0 + 4.0 * relax));
        const double alpha = tau <= 0.01 ? 1.0 : 1.0 - std::exp(-frameMs / tau);
        correction += alpha * (command - correction);
        const double residual = cents[static_cast<size_t>(i)] - center[static_cast<size_t>(i)];
        const double shifted = center[static_cast<size_t>(i)] + correction
            + residual * (1.0 - settings.amount * (1.0 - settings.expression)) * settings.vibratoGain;
        // With the velocity gate closed, the output follows the source.
        const double gate = velocityGate[static_cast<size_t>(i)];
        const double desired = cents[static_cast<size_t>(i)] + gate * (shifted - cents[static_cast<size_t>(i)])
            + settings.transposeSemitones * 100.0;
        desiredF0[static_cast<size_t>(i)] = centsToHz(desired, settings.referenceHz);
        if (std::abs(correction) > 5.0) ++result.correctedFrames;
    }

    result.frames.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        Frame frame;
        frame.timeSeconds = times[static_cast<size_t>(i)];
        frame.sourceHz = static_cast<float>(f0[static_cast<size_t>(i)]);
        frame.targetHz = voiced[static_cast<size_t>(i)] && !correctionEligible[static_cast<size_t>(i)]
            ? frame.sourceHz
            : target[static_cast<size_t>(i)] > 0
                ? static_cast<float>(centsToHz(target[static_cast<size_t>(i)] + settings.transposeSemitones * 100.0,
                                               settings.referenceHz)) : 0.0f;
        frame.desiredHz = static_cast<float>(desiredF0[static_cast<size_t>(i)]);
        frame.voicedWeight = voiced[static_cast<size_t>(i)] ? 1.0f : 0.0f;
        frame.eligible = correctionEligible[static_cast<size_t>(i)];
        result.frames.push_back(frame);
    }
    if (result.correctedFrames == 0) result.warning = "No stable out-of-tune vowel segment was found";
    if (!synthesizeAudio) return result;

    // A worker hop must not begin a new PSOLA pulse lattice. Anchor the pulse
    // phase at the publication boundary carried over from the previous hop,
    // then integrate the requested F0 in both directions through this window.
    // Independent 40 ms lattices created strong 25 Hz-spaced sidebands.
    const int phaseAnchor = timing ? std::clamp(timing->anchorSample, 0, samples - 1) : 0;
    std::vector<double> synthesisPhase(static_cast<size_t>(samples), 0.0);
    synthesisPhase[static_cast<size_t>(phaseAnchor)] = timing ? timing->anchorCycles : 0.0;
    for (int sample = phaseAnchor + 1; sample < samples; ++sample)
        synthesisPhase[static_cast<size_t>(sample)] = synthesisPhase[static_cast<size_t>(sample - 1)]
            + f0At(desiredF0, sample - 1, sampleRate) / sampleRate;
    for (int sample = phaseAnchor - 1; sample >= 0; --sample)
        synthesisPhase[static_cast<size_t>(sample)] = synthesisPhase[static_cast<size_t>(sample + 1)]
            - f0At(desiredF0, sample, sampleRate) / sampleRate;
    if (timing)
        result.handoffCycles = synthesisPhase[static_cast<size_t>(
            std::clamp(timing->handoffSample, 0, samples - 1))];

    // Pitch-synchronous overlap-add keeps the original waveform in each grain. The high-band
    // consonants outside periodic islands pass through dry without vocoder resynthesis.
    std::vector<double> wet(static_cast<size_t>(samples), 0.0);
    std::vector<double> weight(static_cast<size_t>(samples), 0.0);
    std::vector<double> blend(static_cast<size_t>(samples), 0.0);
    for (int start = 0; start < count;) {
        if (!correctionEligible[static_cast<size_t>(start)]) { ++start; continue; }
        int end = start;
        while (end < count && correctionEligible[static_cast<size_t>(end)]) ++end;
        const int sampleStart = std::clamp(static_cast<int>(std::lround(times[static_cast<size_t>(start)] * sampleRate)), 0, samples);
        const int sampleEnd = std::clamp(static_cast<int>(std::lround(times[static_cast<size_t>(std::min(end, count - 1))] * sampleRate)), 0, samples);
        const auto marks = pitchMarks(mono, sampleRate, f0, sampleStart, sampleEnd,
                                     timing ? timing->sourceAnchorSample : -1);
        if (timing && !marks.empty()) {
            const auto nearest = std::lower_bound(marks.begin(), marks.end(), timing->handoffSample);
            const int candidate = nearest == marks.end() ? marks.back()
                : (nearest == marks.begin() || *nearest - timing->handoffSample
                    < timing->handoffSample - *(nearest - 1) ? *nearest : *(nearest - 1));
            const int distance = std::abs(candidate - timing->handoffSample);
            if (distance <= sampleRate / 50
                && (result.sourceHandoffSample < 0
                    || distance < std::abs(result.sourceHandoffSample - timing->handoffSample)))
                result.sourceHandoffSample = candidate;
        }
        if (marks.size() >= 3) {
            for (int out = std::max(1, marks.front()); out <= marks.back(); ++out) {
                if (std::floor(synthesisPhase[static_cast<size_t>(out)])
                    <= std::floor(synthesisPhase[static_cast<size_t>(out - 1)])) continue;
                const auto it = std::lower_bound(marks.begin(), marks.end(), out);
                const int source = it == marks.end() ? marks.back()
                    : (it == marks.begin() || *it - out < out - *(it - 1) ? *it : *(it - 1));
                const double sourceHz = f0At(f0, source, sampleRate);
                if (sourceHz <= 0.0) continue;
                const int radius = std::clamp(static_cast<int>(std::lround(sampleRate / sourceHz)), 16, sampleRate / 50);
                addGrain(mono, wet, weight, source, out, radius);
            }
            const int edge = std::max(1, static_cast<int>(sampleRate
                * (settings.retuneMs <= 2.0f ? 0.008 : 0.025)));
            for (int sample = sampleStart; sample < sampleEnd; ++sample) {
                const double fadeIn = std::clamp((sample - sampleStart) / static_cast<double>(edge), 0.0, 1.0);
                const double fadeOut = std::clamp((sampleEnd - sample) / static_cast<double>(edge), 0.0, 1.0);
                blend[static_cast<size_t>(sample)] = std::min(fadeIn, fadeOut);
            }
        }
        start = end;
    }

    // A PSOLA island may contain samples with no usable grain weight. Switching the
    // correction off at one such sample makes a click even if the voiced island's
    // outer 25 ms fade is smooth. Fade *toward* every coverage gap before it occurs.
    // This also leaves genuine unpitched material bit-identical to the input.
    const int coverageFade = std::max(1, sampleRate / 200); // 5 ms
    std::vector<float> coverage(static_cast<size_t>(samples), 0.0f);
    int lastGap = -coverageFade;
    for (int sample = 0; sample < samples; ++sample) {
        if (weight[static_cast<size_t>(sample)] < 0.05) lastGap = sample;
        else coverage[static_cast<size_t>(sample)] = std::min(1.0f,
            static_cast<float>(sample - lastGap) / coverageFade);
    }
    int nextGap = samples + coverageFade;
    for (int sample = samples - 1; sample >= 0; --sample) {
        if (weight[static_cast<size_t>(sample)] < 0.05) nextGap = sample;
        else coverage[static_cast<size_t>(sample)] = std::min(coverage[static_cast<size_t>(sample)],
            static_cast<float>(nextGap - sample) / coverageFade);
    }

    // Filter the correction delta with zero phase. Otherwise the supposedly
    // cancelled source F0 leaks through the causal crossover and sounds like
    // a second voice under the tuned one.
    const double cutoff = std::min(7000.0, sampleRate * 0.38);
    std::vector<double> delta(static_cast<size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        const double w = weight[static_cast<size_t>(sample)];
        const double dry = mono[static_cast<size_t>(sample)];
        const double pitched = w >= 0.05 ? wet[static_cast<size_t>(sample)] / w : dry;
        delta[static_cast<size_t>(sample)] = pitched - dry;
    }
    zeroPhaseLowpass(delta, sampleRate, cutoff);
    std::vector<double> shiftMix(static_cast<size_t>(count), 0.0);
    for (int frame = 0; frame < count; ++frame) {
        const double sourceHz = f0[static_cast<size_t>(frame)];
        const double desiredHz = desiredF0[static_cast<size_t>(frame)];
        if (sourceHz > 0.0 && desiredHz > 0.0) {
            const double centsShift = std::abs(1200.0 * std::log2(desiredHz / sourceHz));
            shiftMix[static_cast<size_t>(frame)] = std::clamp((centsShift - 1.0) / 5.0, 0.0, 1.0);
        }
    }
    std::vector<float> appliedMix(static_cast<size_t>(samples), 0.0f);
    for (int sample = 0; sample < samples; ++sample) {
        const double dry = mono[static_cast<size_t>(sample)];
        const double framePosition = sample * 1000.0 / (sampleRate * frameMs);
        const int frame = std::clamp(static_cast<int>(framePosition), 0, count - 1);
        const int nextFrame = std::min(frame + 1, count - 1);
        const double fraction = std::clamp(framePosition - frame, 0.0, 1.0);
        const double change = shiftMix[static_cast<size_t>(frame)] * (1.0 - fraction)
                            + shiftMix[static_cast<size_t>(nextFrame)] * fraction;
        const double mix = blend[static_cast<size_t>(sample)] * coverage[static_cast<size_t>(sample)] * change;
        appliedMix[static_cast<size_t>(sample)] = static_cast<float>(mix);
        result.audio[static_cast<size_t>(sample)] = static_cast<float>(dry + mix * delta[static_cast<size_t>(sample)]);
    }

    // Some shifts reinforce the unchanged high band and create a vowel-level
    // jump even if the PSOLA low band alone is level matched. Keep each 20 ms
    // corrected window no louder than its original; dry/unvoiced samples stay exact.
    const int envelopeRadius = std::max(1, sampleRate / 100);
    std::vector<double> sourcePower(static_cast<size_t>(samples + 1), 0.0);
    std::vector<double> outputPower(static_cast<size_t>(samples + 1), 0.0);
    for (int sample = 0; sample < samples; ++sample) {
        const double dry = mono[static_cast<size_t>(sample)];
        const double out = result.audio[static_cast<size_t>(sample)];
        sourcePower[static_cast<size_t>(sample + 1)] = sourcePower[static_cast<size_t>(sample)] + dry * dry;
        outputPower[static_cast<size_t>(sample + 1)] = outputPower[static_cast<size_t>(sample)] + out * out;
    }
    for (int sample = 0; sample < samples; ++sample) {
        const float mix = appliedMix[static_cast<size_t>(sample)];
        if (mix <= 0.0f) continue;
        const int lo = std::max(0, sample - envelopeRadius);
        const int hi = std::min(samples, sample + envelopeRadius + 1);
        const double dryPower = sourcePower[static_cast<size_t>(hi)] - sourcePower[static_cast<size_t>(lo)];
        const double wetPower = outputPower[static_cast<size_t>(hi)] - outputPower[static_cast<size_t>(lo)];
        if (wetPower <= dryPower || wetPower <= 1.0e-12) continue;
        const double levelMatch = std::sqrt(std::max(0.0, dryPower) / wetPower);
        result.audio[static_cast<size_t>(sample)] *= static_cast<float>(1.0 - mix * (1.0 - levelMatch));
    }

    // RMS matching alone can still leave an isolated, over-tall PSOLA pulse.
    // Limit local peak growth to about +2 dB without flattening its waveform.
    const auto dryPeaks = localPeakEnvelope(mono, envelopeRadius);
    const auto outPeaks = localPeakEnvelope(result.audio, envelopeRadius);
    for (int sample = 0; sample < samples; ++sample) {
        const float mix = appliedMix[static_cast<size_t>(sample)];
        if (mix <= 0.0f) continue;
        const float peakLimit = std::max(0.03f, dryPeaks[static_cast<size_t>(sample)] * 1.25f);
        const float outPeak = outPeaks[static_cast<size_t>(sample)];
        if (outPeak <= peakLimit) continue;
        const float peakMatch = peakLimit / outPeak;
        result.audio[static_cast<size_t>(sample)] *= 1.0f - mix * (1.0f - peakMatch);
    }

    // PSOLA overlap can create isolated peaks larger than the source waveform.
    // Back off the *correction* around those peaks instead of clipping the vocal
    // or globally reducing its level. Unchanged/dry samples remain untouched.
    constexpr float ceiling = 0.98f;
    const int safetyRadius = std::max(1, sampleRate / 200); // 5 ms
    std::vector<float> safety(static_cast<size_t>(samples), 1.0f);
    for (int sample = 0; sample < samples; ++sample) {
        const float candidate = result.audio[static_cast<size_t>(sample)];
        if (std::abs(candidate) <= ceiling) continue;
        const float dry = mono[static_cast<size_t>(sample)];
        const float difference = candidate - dry;
        if (std::abs(difference) < 1.0e-8f) continue;
        const float limit = std::copysign(ceiling, candidate);
        const float gain = std::clamp((limit - dry) / difference, 0.0f, 1.0f);
        for (int nearby = std::max(0, sample - safetyRadius);
             nearby <= std::min(samples - 1, sample + safetyRadius); ++nearby) {
            const float distance = static_cast<float>(std::abs(nearby - sample)) / safetyRadius;
            const float envelope = 1.0f - (1.0f - gain) * 0.5f
                * (1.0f + std::cos(3.14159265358979323846f * distance));
            auto& current = safety[static_cast<size_t>(nearby)];
            current = std::min(current, envelope);
        }
    }
    for (int sample = 0; sample < samples; ++sample) {
        const auto index = static_cast<size_t>(sample);
        result.audio[index] = mono[index] + safety[index] * (result.audio[index] - mono[index]);
    }

    return result;
}

} // namespace cody_pitch
