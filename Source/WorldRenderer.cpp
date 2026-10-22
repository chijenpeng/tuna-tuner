#include "WorldRenderer.h"
#include "Experiment.h"

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/synthesisrealtime.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace cody_pitch {
namespace {

struct Chunk {
    Chunk(int frames, int bins)
        : f0(static_cast<size_t>(frames)), times(static_cast<size_t>(frames)),
          sourceF0(static_cast<size_t>(frames)),
          spectrum(static_cast<size_t>(frames), std::vector<double>(static_cast<size_t>(bins))),
          aperiodicity(static_cast<size_t>(frames), std::vector<double>(static_cast<size_t>(bins))) {
        for (int i = 0; i < frames; ++i) {
            spectrumRows.push_back(spectrum[static_cast<size_t>(i)].data());
            aperiodicityRows.push_back(aperiodicity[static_cast<size_t>(i)].data());
        }
    }
    std::vector<double> f0, times, sourceF0;
    std::vector<std::vector<double>> spectrum, aperiodicity;
    std::vector<double*> spectrumRows, aperiodicityRows;
};

} // namespace

struct WorldRenderer::Impl {
    explicit Impl(int sampleRate, int contextSamples, int hopSamples,
                  size_t ringCapacity, int64_t start)
        : rate(sampleRate), context(contextSamples), hop(hopSamples),
          capacity(ringCapacity), origin(start), produced(start),
          voiceCommitted(start), window(static_cast<size_t>(hop + 2 * context)),
          raw(capacity, 0.0f), sourcePitch(capacity, 0.0f),
          desiredPitch(capacity, 0.0f), voicePrefix(capacity, 0.0),
          activeFlag(capacity, 0), dryRing(capacity, 0.0f) {
        if (rate < 16000 || capacity < static_cast<size_t>(rate)
            || hop <= 0 || context <= 0 || origin < 0)
            throw std::invalid_argument("Invalid WORLD stream configuration");
        InitializeCheapTrickOption(rate, &cheap);
        cheap.f0_floor = 70.0;
        cheap.fft_size = GetFFTSizeForCheapTrick(rate, &cheap);
        InitializeD4COption(&d4c);
        xf = flags(); // copied once on the worker
        if (xf.d4cThreshold >= 0.0) d4c.threshold = xf.d4cThreshold;
        if (!xf.framesCsv.empty()) diag.resize(capacity * 200 / static_cast<size_t>(rate) + 8);
        synthBlock = std::max(1, rate / 200);
        framesPerHop = static_cast<int>(std::lround(hop * 200.0 / rate));
        InitializeSynthesizer(rate, 5.0, cheap.fft_size, synthBlock, 128, &synth);
        initialized = true;
        voicePrefix[slot(origin)] = 0.0;
    }

    ~Impl() {
        if (initialized) DestroySynthesizer(&synth);
    }

    size_t slot(int64_t index) const noexcept {
        return static_cast<size_t>(index) % capacity;
    }

    void push(const std::vector<float>& input, const std::vector<Frame>& frames,
              int64_t next) {
        if (next != voiceCommitted || input.size() != window.size())
            throw std::logic_error("WORLD analysis hops must be contiguous");
        const int firstFrame = static_cast<int>(std::lround(context * 200.0 / rate));
        if (firstFrame + framesPerHop > static_cast<int>(frames.size()))
            throw std::logic_error("Not enough F0 frames for WORLD hop");
        auto chunk = std::make_unique<Chunk>(framesPerHop, cheap.fft_size / 2 + 1);
        for (size_t i = 0; i < input.size(); ++i)
            window[i] = input[i];
        for (int i = 0; i < framesPerHop; ++i) {
            const auto& frame = frames[static_cast<size_t>(firstFrame + i)];
            chunk->times[static_cast<size_t>(i)] = frame.timeSeconds;
            chunk->sourceF0[static_cast<size_t>(i)] = frame.sourceHz;
            chunk->f0[static_cast<size_t>(i)] = frame.desiredHz;
        }
        // At high host rates, estimate the spectral envelope every 10 ms; the
        // desired F0 and synthesizer still update every 5 ms at native rate.
        // This halves the expensive native-rate FFT analysis without
        // downsampling the audible output or its top end.
        const int spectralStride = rate > 48000 ? 2 : 1;
        std::vector<double> spectralTimes, spectralF0;
        std::vector<double*> spectralRows, aperiodicityRows;
        for (int i = 0; i < framesPerHop; i += spectralStride) {
            spectralTimes.push_back(chunk->times[static_cast<size_t>(i)]);
            spectralF0.push_back(chunk->sourceF0[static_cast<size_t>(i)]);
            spectralRows.push_back(chunk->spectrumRows[static_cast<size_t>(i)]);
            aperiodicityRows.push_back(chunk->aperiodicityRows[static_cast<size_t>(i)]);
        }
        CheapTrick(window.data(), static_cast<int>(window.size()), rate,
                   spectralTimes.data(), spectralF0.data(),
                   static_cast<int>(spectralTimes.size()), &cheap, spectralRows.data());
        D4C(window.data(), static_cast<int>(window.size()), rate,
            spectralTimes.data(), spectralF0.data(),
            static_cast<int>(spectralTimes.size()), cheap.fft_size, &d4c,
            aperiodicityRows.data());
        for (int i = 0; i < framesPerHop; ++i) {
            if (i % spectralStride == 0) continue;
            const size_t source = static_cast<size_t>(i - i % spectralStride);
            std::copy(chunk->spectrum[source].begin(), chunk->spectrum[source].end(),
                      chunk->spectrum[static_cast<size_t>(i)].begin());
            std::copy(chunk->aperiodicity[source].begin(), chunk->aperiodicity[source].end(),
                      chunk->aperiodicity[static_cast<size_t>(i)].begin());
        }
        // Aperiodicity cap: cap aperiodicity in voiced frames.
        if (xf.apCap < 1.0)
            for (int i = 0; i < framesPerHop; ++i)
                if (chunk->f0[static_cast<size_t>(i)] > 0.0)
                    for (double& value : chunk->aperiodicity[static_cast<size_t>(i)])
                        value = std::min(value, xf.apCap);
        if (!diag.empty()) {
            const int64_t base = std::llround(next * 200.0 / rate);
            const int bins = cheap.fft_size / 2 + 1;
            const int split = std::min(bins, static_cast<int>(4000.0 * cheap.fft_size / rate) + 1);
            for (int i = 0; i < framesPerHop; ++i) {
                const auto& frame = frames[static_cast<size_t>(firstFrame + i)];
                const auto& ap = chunk->aperiodicity[static_cast<size_t>(i)];
                double lo = 0.0, hi = 0.0;
                for (int b = 0; b < split; ++b) lo += ap[static_cast<size_t>(b)];
                for (int b = split; b < bins; ++b) hi += ap[static_cast<size_t>(b)];
                DiagSlot& d = diag[static_cast<size_t>((base + i) % static_cast<int64_t>(diag.size()))];
                d.index = base + i; d.d.valid = true;
                d.d.sourceHz = frame.sourceHz; d.d.targetHz = frame.targetHz; d.d.desiredHz = frame.desiredHz;
                d.d.voiced = frame.voicedWeight > 0.0f; d.d.eligible = frame.eligible;
                d.d.apLow = static_cast<float>(lo / std::max(1, split));
                d.d.apHigh = static_cast<float>(hi / std::max(1, bins - split));
            }
        }
        if (!AddParameters(chunk->f0.data(), framesPerHop,
                           chunk->spectrumRows.data(), chunk->aperiodicityRows.data(), &synth))
            throw std::runtime_error("WORLD synthesis queue full");
        chunks.push_back(std::move(chunk));

        // The dry component must be exact for unvoiced, already-on-pitch and
        // explicit Bypass notes. For larger corrections use one resynthesized
        // voice, not a retained dry harmonic ladder under it.
        for (int sample = 0; sample < hop; ++sample) {
            const int frameIndex = std::min(framesPerHop - 1, sample * framesPerHop / hop);
            const auto& frame = frames[static_cast<size_t>(firstFrame + frameIndex)];
            sourcePitch[slot(next + sample)] = frame.sourceHz;
            desiredPitch[slot(next + sample)] = frame.desiredHz;
            double cents = 0.0;
            if (frame.sourceHz > 0.0f && frame.desiredHz > 0.0f) {
                cents = std::abs(1200.0 * std::log2(frame.desiredHz / frame.sourceHz));
            }
            // Constant partial dry/wet on a sustained vowel creates a second
            // pitch ladder and beats against the corrected voice. Use a small
            // hysteresis band instead; only note boundaries are crossfaded by
            // corrected()'s 10 ms prefix window.
            // Switch modes (0 = shipped 8/3 cents; 1 = every voiced eligible frame; 2 = custom cents).
            if (xf.switchMode == 1) {
                resynthesisActive = frame.eligible && frame.voicedWeight > 0.0f && frame.desiredHz > 0.0f;
            } else {
                const double onCents = xf.switchMode == 2 ? xf.switchOnCents : 8.0;
                const double offCents = xf.switchMode == 2 ? xf.switchOffCents : 3.0;
                if (cents >= onCents) resynthesisActive = true;
                else if (cents <= offCents) resynthesisActive = false;
            }
            voiceTotal += resynthesisActive ? 1.0 : 0.0;
            voicePrefix[slot(next + sample + 1)] = voiceTotal;
            if (xf.switchAlign == 1 && xf.switchMode == 1) { // boundary bookkeeping
                const int64_t at = next + sample;
                dryRing[slot(at)] = input[static_cast<size_t>(context + sample)];
                activeFlag[slot(at)] = resynthesisActive ? 1 : 0;
                if (resynthesisActive != previousActive && at > origin) {
                    const float hz = frame.sourceHz > 0.0f ? frame.sourceHz : frame.desiredHz;
                    const float other = sourcePitch[slot(at - 1)] > 0.0f ? sourcePitch[slot(at - 1)] : desiredPitch[slot(at - 1)];
                    const float use = hz > 0.0f ? hz : other;
                    if (use > 0.0f) boundaries.push_back({ at, 0, std::max(2, static_cast<int>(std::lround(rate / use))), resynthesisActive, false });
                }
                previousActive = resynthesisActive;
            }
        }
        voiceCommitted += hop;

        while (Synthesis2(&synth))
            for (int sample = 0; sample < synthBlock; ++sample)
                raw[slot(produced++)] = static_cast<float>(synth.buffer[sample]);

        while (firstChunkIndex < synth.current_pointer2 && !chunks.empty()) {
            chunks.pop_front();
            ++firstChunkIndex;
        }
        if (xf.switchAlign == 1 && xf.switchMode == 1) resolveBoundaries();
    }

    // Optional switch-boundary alignment: shift each dry<->wet boundary within +-0.5 source period to the
    // point where dry and synthesized waveforms correlate best over one period. The boundary is
    // moved by rewriting the per-sample active flag and rebuilding the prefix sum forward, which
    // is done on the worker before any sample near the boundary can be published (the published
    // range trails `produced` by more than the fade plus the correlation span).
    struct Boundary { int64_t position; int lag; int period; bool toWet; bool resolved; };
    void resolveBoundaries() {
        for (size_t k = 0; k < boundaries.size(); ++k) {
            Boundary& b = boundaries[k];
            if (b.resolved) continue;
            const int half = b.period / 2;
            if (produced < b.position + half + b.period / 2 + 2) continue; // synthesized signal not there yet
            if (b.position - half - b.period / 2 - 1 < origin) { b.resolved = true; continue; }
            // Neighbour limits so boundaries never cross.
            int64_t lowest = b.position - half, highest = b.position + half;
            if (k > 0) lowest = std::max(lowest, boundaries[k - 1].position + boundaries[k - 1].lag + 1);
            if (k + 1 < boundaries.size()) highest = std::min(highest, boundaries[k + 1].position - 1);
            double bestScore = -2.0; int bestShift = 0;
            for (int shift = -half; shift <= half; ++shift) {
                const int64_t centre = b.position + shift;
                if (centre < lowest || centre > highest) continue;
                double cross = 0.0, dd = 0.0, rr = 0.0;
                for (int64_t n = centre - b.period / 2; n < centre - b.period / 2 + b.period; ++n) {
                    const double a = dryRing[slot(n)], r = raw[slot(n)];
                    cross += a * r; dd += a * a; rr += r * r;
                }
                const double score = cross / std::sqrt(std::max(1.0e-18, dd * rr));
                if (score > bestScore) { bestScore = score; bestShift = shift; }
            }
            b.resolved = true;
            b.lag = bestShift;
            if (bestShift == 0) { logLag(b); continue; }
            const int64_t lo = std::min(b.position, b.position + bestShift);
            const int64_t hi = std::max(b.position, b.position + bestShift);
            // Samples between the nominal and chosen position take the state on the other side of the boundary.
            for (int64_t n = lo; n < hi; ++n) activeFlag[slot(n)] = (bestShift > 0) ? (b.toWet ? 0 : 1) : (b.toWet ? 1 : 0);
            double total = voicePrefix[slot(lo)];
            for (int64_t n = lo; n < voiceCommitted; ++n) {
                total += activeFlag[slot(n)];
                voicePrefix[slot(n + 1)] = total;
            }
            voiceTotal = total;
            logLag(b);
        }
        while (boundaries.size() > 64 && boundaries.front().resolved) boundaries.pop_front();
    }

    void logLag(const Boundary& b) {
        if (diag.empty()) return;
        const int64_t frame = std::llround(b.position * 200.0 / rate);
        DiagSlot& d = diag[static_cast<size_t>(frame % static_cast<int64_t>(diag.size()))];
        if (d.index == frame) d.d.alignLag = static_cast<float>(b.lag);
    }

    int64_t readyUntil() const noexcept {
        // Quality-gating needs about 15 ms of future pulse data and another
        // 15 ms to fade in before a bad frame. Keep that lookahead within the
        // existing 320 ms host latency rather than running it on the callback.
        const int64_t lookahead = std::max(1, rate * 9 / 250);
        return std::max(origin, std::min(produced, voiceCommitted) - lookahead);
    }

    double blendAt(int64_t index) const noexcept {
        // Fade length comes from TUNA_SWITCH_FADE_MS (default 10 ms == rate / 100).
        const int64_t fade = xf.switchFadeMs == 10.0 ? std::max(1, rate / 100)
            : std::max<int64_t>(1, std::llround(rate * xf.switchFadeMs / 1000.0));
        const int64_t lo = std::max(origin, index - fade);
        const int64_t hi = std::min(voiceCommitted, index + fade);
        return std::clamp((voicePrefix[slot(hi)] - voicePrefix[slot(lo)])
            / static_cast<double>(std::max<int64_t>(1, hi - lo)), 0.0, 1.0);
    }

    float corrected(int64_t index, float dry) const noexcept {
        if (index < origin || index >= voiceCommitted) return dry;
        const double blend = blendAt(index);
        return static_cast<float>(dry * (1.0 - blend) + raw[slot(index)] * (xf.wetGain * blend));
    }

    FrameDiag frameDiag(int64_t frame) const noexcept {
        if (diag.empty()) return {};
        const DiagSlot& d = diag[static_cast<size_t>(frame % static_cast<int64_t>(diag.size()))];
        return d.index == frame ? d.d : FrameDiag {};
    }

    float synthesized(int64_t index) const noexcept {
        return index >= origin && index < produced ? raw[slot(index)] : 0.0f;
    }

    float sourceHz(int64_t index) const noexcept {
        return index >= origin && index < voiceCommitted ? sourcePitch[slot(index)] : 0.0f;
    }

    float desiredHz(int64_t index) const noexcept {
        return index >= origin && index < voiceCommitted ? desiredPitch[slot(index)] : 0.0f;
    }

    struct DiagSlot { int64_t index = -1; FrameDiag d; };
    ExperimentFlags xf;
    std::vector<DiagSlot> diag;
    int rate, context, hop, synthBlock = 0, framesPerHop = 0;
    size_t capacity;
    int64_t origin, produced, voiceCommitted;
    double voiceTotal = 0.0;
    bool resynthesisActive = false;
    int firstChunkIndex = 0;
    bool initialized = false;
    CheapTrickOption cheap {};
    D4COption d4c {};
    WorldSynthesizer synth {};
    std::vector<double> window;
    std::vector<float> raw;
    std::vector<float> sourcePitch, desiredPitch;
    std::vector<double> voicePrefix;
    std::vector<uint8_t> activeFlag;
    std::vector<float> dryRing;
    std::deque<Boundary> boundaries;
    bool previousActive = false;
    std::deque<std::unique_ptr<Chunk>> chunks;
};

WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() = default;

void WorldRenderer::prepare(int sampleRate, int contextSamples, int hopSamples,
                            size_t ringCapacity, int64_t origin) {
    impl = std::make_unique<Impl>(sampleRate, contextSamples, hopSamples, ringCapacity, origin);
}

void WorldRenderer::processWindow(const std::vector<float>& window,
                                  const std::vector<Frame>& frames, int64_t next) {
    if (!impl) throw std::logic_error("WORLD renderer was not prepared");
    impl->push(window, frames, next);
}

int64_t WorldRenderer::readyUntil() const noexcept {
    return impl ? impl->readyUntil() : 0;
}

float WorldRenderer::corrected(int64_t index, float dry) const noexcept {
    return impl ? impl->corrected(index, dry) : dry;
}

WorldRenderer::FrameDiag WorldRenderer::frameDiag(int64_t frameIndex) const noexcept {
    return impl ? impl->frameDiag(frameIndex) : FrameDiag {};
}

float WorldRenderer::blendAt(int64_t index) const noexcept {
    return impl ? static_cast<float>(impl->blendAt(index)) : 0.0f;
}

float WorldRenderer::synthesized(int64_t index) const noexcept {
    return impl ? impl->synthesized(index) : 0.0f;
}

float WorldRenderer::sourceHz(int64_t index) const noexcept {
    return impl ? impl->sourceHz(index) : 0.0f;
}

float WorldRenderer::desiredHz(int64_t index) const noexcept {
    return impl ? impl->desiredHz(index) : 0.0f;
}

} // namespace cody_pitch
