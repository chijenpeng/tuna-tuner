#include "RealtimeEngine.h"
#include "WorldRenderer.h"
#include "WaveformRenderer.h"
#include "Experiment.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#if defined(__APPLE__)
#include <pthread.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cody_pitch {
namespace {

uint64_t threadCpuTimeNanos() noexcept {
#if defined(_WIN32)
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER kernelTicks {}, userTicks {};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return (kernelTicks.QuadPart + userTicks.QuadPart) * 100ULL;
#else
    timespec cpu {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0) return 0;
    return static_cast<uint64_t>(cpu.tv_sec) * 1000000000ULL + static_cast<uint64_t>(cpu.tv_nsec);
#endif
}

std::vector<float> lowpassKernel(int sampleRate, double cutoffHz = 8000.0) {
    constexpr int radius = 64;
    constexpr double pi = 3.14159265358979323846;
    const double normalized = 2.0 * std::min(cutoffHz, sampleRate * 0.4) / sampleRate;
    std::vector<float> kernel(static_cast<size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const double sinc = offset == 0 ? normalized
            : std::sin(pi * normalized * offset) / (pi * offset);
        const double window = 0.5 * (1.0 + std::cos(pi * offset / radius));
        const double value = sinc * window;
        kernel[static_cast<size_t>(offset + radius)] = static_cast<float>(value);
        sum += value;
    }
    for (auto& value : kernel) value = static_cast<float>(value / sum);
    return kernel;
}

} // namespace

namespace {
struct Timing { double hopMs, contextMs, headroomMs; };
// Headroom is what is left for render time and scheduling; WORLD needs more of it.
Timing timingFor(int mode, bool world) noexcept {
    switch (mode) {
        case 1: return { 40.0, 120.0, world ? 60.0 : 40.0 };
        case 2: return { 20.0, 80.0, world ? 50.0 : 30.0 };
        case 3: return { 20.0, 30.0, world ? 40.0 : 20.0 };
        default: return { 40.0, 200.0, 80.0 };
    }
}
int toSamples(int rate, double ms) noexcept { return std::max(1, static_cast<int>(std::lround(rate * ms / 1000.0))); }
} // namespace

int RealtimeEngine::latencyFor(int rate, int mode, bool worldRenderer) noexcept {
    rate = std::max(16000, rate);
    if (worldRenderer) mode = std::min(mode, 2); // WORLD's publish lookahead does not fit the Live context
    const Timing t = timingFor(mode, worldRenderer);
    return toSamples(rate, t.hopMs + t.contextMs + t.headroomMs);
}

RealtimeEngine::~RealtimeEngine() { stop(); }

void RealtimeEngine::prepare(int sampleRate, std::function<Settings()> settingsProvider) {
    stop();
    (void) flags(); // Read environment overrides once, off the audio thread
    rate = std::max(16000, sampleRate);
    // The worker needs one hop plus right context before it can publish a block.
    // 320 ms leaves roughly 80 ms of startup scheduling/render headroom at
    // 48 kHz. 300 ms occasionally missed a block on a busy host even though
    // steady-state processing was faster than the 40 ms analysis hop.
    settings = std::move(settingsProvider);
    const bool worldRenderer = flags().rendererSet && flags().renderer == "world";
    int mode = flags().latencyModeSet ? flags().latencyMode : std::clamp(settings().latencyMode, 0, 3);
    if (worldRenderer) mode = std::min(mode, 2); // WORLD's 36 ms publish lookahead does not fit the 30 ms Live context
    const Timing timing = timingFor(mode, worldRenderer);
    latencyMode.store(mode);
    latencyNow.store(latencyFor(rate, mode, worldRenderer));
    audioLatency = fadeFromLatency = latencyNow.load();
    latencyFadeLeft = 0;
    hop = toSamples(rate, timing.hopMs);
    context = toSamples(rate, timing.contextMs);
    capacity = static_cast<size_t>(rate) * 8;
    input = std::make_unique<std::atomic<float>[]>(capacity);
    side = std::make_unique<std::atomic<float>[]>(capacity);
    output = std::make_unique<std::atomic<float>[]>(capacity);
    for (size_t i = 0; i < capacity; ++i) {
        input[i].store(0.0f, std::memory_order_relaxed);
        side[i].store(0.0f, std::memory_order_relaxed);
        output[i].store(0.0f, std::memory_order_relaxed);
    }
    written.store(0);
    readyFrom.store(0);
    readyUntil.store(0);
    detected.store(0.0f);
    target.store(0.0f);
    underruns.store(0);
    firstUnderrun.store(-1);
    lastUnderrun.store(-1);
    maxRenderMicros.store(0);
    maxAnalysisMicros.store(0);
    maxPostMicros.store(0);
    maxRendererMicros.store(0);
    maxWorkerCpuMicros.store(0);
    previousWetReady = false;
    transitionRemaining = 0;
    transitionFrom = 0.0f;
    lastCorrection = 0.0f;
    // Prime the actual plug-in path before audio starts. A legacy render() warmup
    // leaves WORLD's FFT/synthesis allocation cold and can miss the first block.
    try {
        const std::vector<float> silence(static_cast<size_t>(hop + 2 * context), 0.0f);
        Settings warmSettings = settings();
        warmSettings.contextMs = static_cast<float>(1000.0 * context / rate);
        const auto analysis = render(silence, rate, warmSettings, nullptr, false);
        WorldRenderer warm;
        warm.prepare(rate, context, hop, capacity);
        warm.processWindow(silence, analysis.frames, 0);
    } catch (...) {
        // A bad setting must not prevent the host from instantiating the plug-in.
    }
    running.store(true, std::memory_order_release);
    worker = std::thread([this] { run(); });
}

void RealtimeEngine::waitFor(int64_t end) noexcept {
    if (end <= 0) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (running.load(std::memory_order_acquire)
           && readyUntil.load(std::memory_order_acquire) < end
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

float RealtimeEngine::blendWet(float dry, float processed, bool ready) noexcept {
    const int fadeSamples = std::max(1, rate / 200); // 5 ms
    if (ready != previousWetReady) {
        previousWetReady = ready;
        transitionRemaining = fadeSamples;
        transitionFrom = lastCorrection;
    }
    const float requested = ready ? processed - dry : 0.0f;
    float correction = requested;
    if (transitionRemaining > 0) {
        const float t = 1.0f - static_cast<float>(transitionRemaining) / fadeSamples;
        correction = transitionFrom * (1.0f - t) + requested * t;
        --transitionRemaining;
    }
    lastCorrection = correction;
    return dry + correction;
}

void RealtimeEngine::beginBlockLatency() noexcept {
    const int now = latencyNow.load(std::memory_order_acquire);
    if (now != audioLatency) { fadeFromLatency = audioLatency; audioLatency = now; latencyFadeLeft = std::max(1, rate / 200); }
}

void RealtimeEngine::processStereo(float* left, float* right, int count, bool wet, bool offline) noexcept {
    if (capacity == 0 || count <= 0) return;
    const int64_t base = written.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        const int64_t now = base + i;
        const float mid = 0.5f * (left[i] + right[i]);
        const float sideValue = 0.5f * (left[i] - right[i]);
        input[slot(now)].store(mid, std::memory_order_relaxed);
        side[slot(now)].store(sideValue, std::memory_order_relaxed);
    }
    written.store(base + count, std::memory_order_release);
    beginBlockLatency();
    if (offline && wet) waitFor(base + count - audioLatency);
    for (int i = 0; i < count; ++i) {
        const int64_t now = base + i;
        const int64_t source = now - audioLatency;
        if (source < 0) { left[i] = 0.0f; right[i] = 0.0f; continue; }
        float dry = input[slot(source)].load(std::memory_order_relaxed);
        float delayedSide = side[slot(source)].load(std::memory_order_relaxed);
        if (latencyFadeLeft > 0) { // 5 ms crossfade between the old and the new delay
            const int64_t oldSource = now - fadeFromLatency;
            const float t = 1.0f - static_cast<float>(latencyFadeLeft--) / static_cast<float>(std::max(1, rate / 200));
            if (oldSource >= 0) {
                dry = input[slot(oldSource)].load(std::memory_order_relaxed) * (1.0f - t) + dry * t;
                delayedSide = side[slot(oldSource)].load(std::memory_order_relaxed) * (1.0f - t) + delayedSide * t;
            }
        }
        const bool ready = source >= readyFrom.load(std::memory_order_acquire)
            && source < readyUntil.load(std::memory_order_acquire);
        const bool useWet = wet && ready;
        const float processed = useWet ? output[slot(source)].load(std::memory_order_relaxed) : dry;
        const float outMid = blendWet(dry, processed, useWet);
        left[i] = outMid + delayedSide;
        right[i] = outMid - delayedSide;
        if (wet && !ready) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            int64_t unset = -1;
            firstUnderrun.compare_exchange_strong(unset, source, std::memory_order_relaxed);
            lastUnderrun.store(source, std::memory_order_relaxed);
        }
    }
}

void RealtimeEngine::stop() {
    running.store(false, std::memory_order_release);
    if (worker.joinable()) worker.join();
}

void RealtimeEngine::process(float* samples, int count, bool wet, bool offline) noexcept {
    if (capacity == 0 || count <= 0) return;
    const int64_t base = written.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        const int64_t now = base + i;
        input[slot(now)].store(samples[i], std::memory_order_relaxed);
    }
    written.store(base + count, std::memory_order_release);
    beginBlockLatency();
    if (offline && wet) waitFor(base + count - audioLatency);
    for (int i = 0; i < count; ++i) {
        const int64_t now = base + i;
        const int64_t source = now - audioLatency;
        if (source < 0) { samples[i] = 0.0f; continue; }
        float dry = input[slot(source)].load(std::memory_order_relaxed);
        if (latencyFadeLeft > 0) { // 5 ms crossfade between the old and the new delay
            const int64_t oldSource = now - fadeFromLatency;
            const float t = 1.0f - static_cast<float>(latencyFadeLeft--) / static_cast<float>(std::max(1, rate / 200));
            if (oldSource >= 0) dry = input[slot(oldSource)].load(std::memory_order_relaxed) * (1.0f - t) + dry * t;
        }
        const bool ready = source >= readyFrom.load(std::memory_order_acquire)
            && source < readyUntil.load(std::memory_order_acquire);
        const bool useWet = wet && ready;
        const float processed = useWet ? output[slot(source)].load(std::memory_order_relaxed) : dry;
        samples[i] = blendWet(dry, processed, useWet);
        if (wet && !ready) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            int64_t unset = -1;
            firstUnderrun.compare_exchange_strong(unset, source, std::memory_order_relaxed);
            lastUnderrun.store(source, std::memory_order_relaxed);
        }
    }
}

void RealtimeEngine::run() {
#if defined(__APPLE__)
    // Best effort: this is the deadline-bearing analysis worker, not the audio
    // callback. Keep it ahead of background work without requesting RT policy.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    // Optional bounded timing trace. Accumulate on the worker and write only after
    // it stops, so diagnostic file I/O cannot consume an audio deadline.
    struct HopTiming {
        int64_t next, available, readyBefore, startUs, finishUs, cpuUs, availableAfter, readyAfter;
        int underruns;
    };
    const char* timingPath = std::getenv("TUNA_TIMING_CSV");
    std::vector<HopTiming> timings;
    constexpr size_t timingLimit = 4096;
    if (timingPath) timings.reserve(timingLimit);
    const auto clockMicros = [](auto time) {
        return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    };
    int64_t next = 0;
    std::vector<float> window(static_cast<size_t>(hop + 2 * context));
    const int analysisFactor = rate > 48000 ? std::max(2, rate / 48000) : 1;
    std::vector<float> analysisWindow((window.size() + analysisFactor - 1) / analysisFactor);
    WorldRenderer world;
    const ExperimentFlags& experiment = flags();
    // Renderer choice: 0 Splice, 1 PSOLA (plug-in parameter), 2 WORLD (TUNA_RENDERER=world).
    // Waveform renderers replace WORLD, the quality gate and the hysteresis. A change takes effect between hops: the
    // worker publishes delayed dry for that hop and re-prepares the chosen renderer (no allocation on the callback).
    const auto chosenRenderer = [&experiment](const Settings& current) {
        if (experiment.rendererSet)
            return experiment.renderer == "world" ? 2 : experiment.renderer == "psola" ? 1 : 0;
        return std::clamp(current.renderer, 0, 1);
    };
    const auto chosenLatency = [&experiment](const Settings& current) {
        const int mode = experiment.latencyModeSet ? experiment.latencyMode : std::clamp(current.latencyMode, 0, 3);
        return experiment.rendererSet && experiment.renderer == "world" ? std::min(mode, 2) : mode;
    };
    int stalledHops = 0;
    bool rendererBroken = false;
    int activeMode = latencyMode.load();
    int activeRenderer = chosenRenderer(settings());
    bool waveform = activeRenderer != 2;
    WaveformRenderer wave;
    const auto configureWave = [&]() {
        wave.setMode(activeRenderer == 1 ? WaveformRenderer::Mode::Psola : WaveformRenderer::Mode::Splice);
        wave.setSpliceTrim(experiment.spliceTrim != 0);
        wave.setSpliceFormant(experiment.spliceFormant);
        // the read pointer may lead the output by the drift; keep it inside the available context
        wave.setSplicePure(experiment.splicePure != 0, std::min(experiment.splicePureMaxDriftMs, std::max(5.0, 1000.0 * context / rate - 10.0)));
        wave.setFormantTuning(experiment.spliceFormantNorm != 0, experiment.spliceFormantHopMs);
        wave.setSplicePerCycle(experiment.spliceRatioPerCycle != 0);
        wave.setPsolaOptions(experiment.psolaStepReuse != 0, experiment.psolaSmoothNorm != 0);
    };
    if (waveform) configureWave();
    const auto prepareRenderer = [&](int64_t origin) {
        if (waveform) wave.prepare(rate, context, hop, capacity, origin);
        else world.prepare(rate, context, hop, capacity, origin);
    };
    try { prepareRenderer(0); }
    catch (...) { return; }
    const auto lowpass = lowpassKernel(rate);
    const auto waveLowpass = experiment.waveLpHz > 0.0 ? lowpassKernel(rate, experiment.waveLpHz) : std::vector<float> {};
    const int waveRadius = static_cast<int>(waveLowpass.size() / 2);
    const int filterRadius = static_cast<int>(lowpass.size() / 2);
    std::vector<float> correctionWindow, alignedWindow, alignedLog;
    std::vector<double> positionLog;
    const char* alignedDump = std::getenv("TUNA_ALIGNED_DRY_OUT"); // measurement: the dry at the renderer's read position
    int64_t published = 0;
    bool worldActive = true;
    // Optional per-frame diagnostics run on the worker thread.
    std::ofstream framesCsv;
    if (!experiment.framesCsv.empty()) {
        framesCsv.open(experiment.framesCsv, std::ios::trunc);
        framesCsv << std::setprecision(10);
        if (waveform)
            framesCsv << "time_s,source_hz,desired_hz,island,active,jump,underrun\n";
        else
            framesCsv << "time_s,source_hz,target_hz,desired_hz,voiced,eligible,resynth_blend,"
                         "quality_suppress,ap_mean_0_4k,ap_mean_4_8k,underrun,gate_skipped,align_lag\n";
    }
    const auto logFrames = [&](int64_t from, int64_t until, const std::vector<float>* suppressed,
                               int firstSuppressFrame, bool dryFallback,
                               const std::vector<bool>* skipped = nullptr) {
        if (!framesCsv.is_open()) return;
        const auto centre = [&](int64_t frame) { return static_cast<int64_t>(std::lround(frame * rate / 200.0)); };
        int64_t frame = static_cast<int64_t>(std::floor(from * 200.0 / rate)) - 1;
        for (; centre(frame) < until; ++frame) {
            const int64_t c = centre(frame);
            if (c < from) continue;
            if (waveform) {
                const auto d = dryFallback ? WaveformRenderer::FrameDiag {} : wave.frameDiag(frame);
                framesCsv << frame * 0.005 << ',' << d.sourceHz << ',' << d.desiredHz << ',' << d.island << ','
                          << (d.active ? 1 : 0) << ',' << (d.jump ? 1 : 0) << ',' << (dryFallback ? 1 : 0) << '\n';
                continue;
            }
            WorldRenderer::FrameDiag d;
            if (!dryFallback) d = world.frameDiag(frame);
            float suppress = 0.0f;
            if (suppressed && !suppressed->empty())
                suppress = (*suppressed)[static_cast<size_t>(std::clamp<int64_t>(
                    frame - firstSuppressFrame, 0, static_cast<int64_t>(suppressed->size()) - 1))];
            const bool wasSkipped = skipped && !skipped->empty() && (*skipped)[static_cast<size_t>(std::clamp<int64_t>(
                frame - firstSuppressFrame, 0, static_cast<int64_t>(skipped->size()) - 1))];
            framesCsv << frame * 0.005 << ',' << d.sourceHz << ',' << d.targetHz << ',' << d.desiredHz << ','
                      << (d.voiced ? 1 : 0) << ',' << (d.eligible ? 1 : 0) << ','
                      << (dryFallback ? 0.0f : world.blendAt(c)) << ',' << suppress << ','
                      << d.apLow << ',' << d.apHigh << ',' << (dryFallback ? 1 : 0) << ','
                      << (wasSkipped ? 1 : 0) << ',' << d.alignLag << '\n';
        }
    };
    const auto publishDry = [&](int64_t until) {
        logFrames(published, until, nullptr, 0, true);
        const int fade = std::max(1, rate / 200);
        const float last = published > 0
            ? output[slot(published - 1)].load(std::memory_order_relaxed)
                - input[slot(published - 1)].load(std::memory_order_relaxed)
            : 0.0f;
        for (int64_t index = published; index < until; ++index) {
            const float dry = input[slot(index)].load(std::memory_order_relaxed);
            const float t = std::clamp(static_cast<float>(index - published) / fade, 0.0f, 1.0f);
            output[slot(index)].store(dry + last * (1.0f - t), std::memory_order_relaxed);
        }
        published = until;
        readyUntil.store(until, std::memory_order_release);
    };
    while (running.load(std::memory_order_acquire)) {
        const int64_t available = written.load(std::memory_order_acquire);
        if (available < next + hop + context) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const int64_t readyBefore = published;
        Settings currentSettings = settings();
        if (const int wantMode = chosenLatency(currentSettings); wantMode != activeMode) {
            // Re-time the engine on the worker: new hop/context/delay, resynchronised start, renderer re-prepared on the
            // next pass. The audio thread crossfades the delayed dry between the two delays.
            const Timing t = timingFor(wantMode, activeRenderer == 2);
            activeMode = wantMode;
            hop = toSamples(rate, t.hopMs);
            context = toSamples(rate, t.contextMs);
            window.assign(static_cast<size_t>(hop + 2 * context), 0.0f);
            analysisWindow.assign((window.size() + static_cast<size_t>(analysisFactor) - 1) / static_cast<size_t>(analysisFactor), 0.0f);
            if (waveform) configureWave();
            latencyMode.store(wantMode);
            latencyNow.store(latencyFor(rate, wantMode, activeRenderer == 2), std::memory_order_release);
            next = std::max<int64_t>(0, ((available - latencyNow.load()) / hop) * hop);
            readyFrom.store(next, std::memory_order_release);
            readyUntil.store(next, std::memory_order_release);
            published = next;
            worldActive = false; rendererBroken = false; stalledHops = 0;
            detected.store(0.0f, std::memory_order_relaxed);
            target.store(0.0f, std::memory_order_relaxed);
            continue;
        }
        currentSettings.contextMs = static_cast<float>(1000.0 * context / rate);
        if (rendererBroken) { // the renderer cannot publish at this timing: delayed dry, see the logged error
            publishDry(next + hop);
            next += hop;
            continue;
        }
        if (available - next > static_cast<int64_t>(capacity) - static_cast<int64_t>(window.size())) {
            next = std::max<int64_t>(0, ((available - latencyNow.load()) / hop) * hop);
            readyFrom.store(next, std::memory_order_release);
            readyUntil.store(next, std::memory_order_release);
            try { prepareRenderer(next); }
            catch (...) { return; }
            published = next;
            worldActive = true;
        }
        const int64_t start = next - context;
        for (size_t i = 0; i < window.size(); ++i) {
            const int64_t index = start + static_cast<int64_t>(i);
            window[i] = index < 0 ? 0.0f : input[slot(index)].load(std::memory_order_relaxed);
        }
        if (const int wanted = chosenRenderer(currentSettings); wanted != activeRenderer) {
            publishDry(next + hop);
            activeRenderer = wanted;
            waveform = wanted != 2;
            if (waveform) configureWave();
            worldActive = false;
            detected.store(0.0f, std::memory_order_relaxed);
            target.store(0.0f, std::memory_order_relaxed);
            next += hop;
            continue;
        }
        if (currentSettings.amount <= 0.0f
            || std::none_of(currentSettings.allowedNotes.begin(), currentSettings.allowedNotes.end(),
                            [](bool allowed) { return allowed; })) {
            publishDry(next + hop);
            worldActive = false;
            detected.store(0.0f, std::memory_order_relaxed);
            target.store(0.0f, std::memory_order_relaxed);
            next += hop;
            continue;
        }
        if (!worldActive) {
            try { prepareRenderer(next); }
            catch (...) { return; }
            worldActive = true;
            published = next;
        }
        const auto renderStart = std::chrono::steady_clock::now();
        const auto cpuStart = threadCpuTimeNanos();
        try {
            if (analysisFactor > 1) {
                for (size_t i = 0; i < analysisWindow.size(); ++i) {
                    double sum = 0.0;
                    int count = 0;
                    for (int j = 0; j < analysisFactor; ++j) {
                        const size_t source = i * analysisFactor + static_cast<size_t>(j);
                        if (source >= window.size()) break;
                        sum += window[source];
                        ++count;
                    }
                    analysisWindow[i] = static_cast<float>(sum / count);
                }
            }
            const auto result = render(analysisFactor > 1 ? analysisWindow : window,
                                       rate / analysisFactor, currentSettings, nullptr, false);
            const auto rendererStart = std::chrono::steady_clock::now();
            if (waveform) wave.processWindow(window, result.frames, next);
            else world.processWindow(window, result.frames, next);
            const auto rendererMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - rendererStart).count();
            auto previousRendererMax = maxRendererMicros.load(std::memory_order_relaxed);
            while (rendererMicros > previousRendererMax && !maxRendererMicros.compare_exchange_weak(
                previousRendererMax, rendererMicros, std::memory_order_relaxed)) {}
            if (!result.frames.empty()) {
                const auto frame = std::min(result.frames.size() - 1,
                    static_cast<size_t>(std::lround((context + hop / 2.0) * 200.0 / rate)));
                detected.store(result.frames[frame].sourceHz, std::memory_order_relaxed);
                target.store(result.frames[frame].targetHz, std::memory_order_relaxed);
            }
        } catch (...) {
            // A failed worker hop is published as delayed dry. Reset the WORLD
            // phase rather than continuing with a partially updated queue.
            publishDry(next + hop);
            worldActive = false;
            detected.store(0.0f, std::memory_order_relaxed);
            target.store(0.0f, std::memory_order_relaxed);
        }
        const auto postStart = std::chrono::steady_clock::now();
        const auto analysisMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            postStart - renderStart).count();
        auto previousAnalysisMax = maxAnalysisMicros.load(std::memory_order_relaxed);
        while (analysisMicros > previousAnalysisMax && !maxAnalysisMicros.compare_exchange_weak(
            previousAnalysisMax, analysisMicros, std::memory_order_relaxed)) {}
        const auto rendererEnd = [&]() { return waveform ? wave.readyUntil() - waveRadius : world.readyUntil(); };
        const int64_t end = !worldActive ? published
            : waveform ? std::max(published, wave.readyUntil() - waveRadius) : world.readyUntil();
        if (worldActive && end <= published) {
            if (++stalledHops >= 8) {
                std::cerr << "RealtimeEngine: renderer cannot reach the publish point at this latency (ready " << rendererEnd()
                          << ", published " << published << "); falling back to delayed dry\n";
                publishDry(next + hop);
                rendererBroken = true; worldActive = false; stalledHops = 0;
            }
        } else stalledHops = 0;
        if (waveform && end > published) {
            // Waveform renderers: output = dry + LP(rendered - dry); no quality gate, no hysteresis.
            const int64_t count = end - published;
            logFrames(published, end, nullptr, 0, false);
            correctionWindow.resize(static_cast<size_t>(count + 2 * waveRadius));
            alignedWindow.resize(correctionWindow.size());
            for (int64_t offset = 0; offset < count + 2 * waveRadius; ++offset) {
                const int64_t index = published + offset - waveRadius;
                if (index < 0) { correctionWindow[static_cast<size_t>(offset)] = 0.0f; alignedWindow[static_cast<size_t>(offset)] = 0.0f; continue; }
                const float dry = wave.alignedDry(index, input[slot(index)].load(std::memory_order_relaxed));
                alignedWindow[static_cast<size_t>(offset)] = dry;
                correctionWindow[static_cast<size_t>(offset)] = wave.corrected(index, dry) - dry;
            }
            for (int64_t offset = 0; offset < count; ++offset) {
                double correction = correctionWindow[static_cast<size_t>(offset + waveRadius)];
                if (waveRadius > 0) {
                    correction = 0.0;
                    for (size_t tap = 0; tap < waveLowpass.size(); ++tap)
                        correction += waveLowpass[tap] * correctionWindow[static_cast<size_t>(offset) + tap];
                }
                const int64_t index = published + offset;
                const float dry = alignedWindow[static_cast<size_t>(offset + waveRadius)];
                const float result = dry + static_cast<float>(correction);
                output[slot(index)].store(result, std::memory_order_relaxed);
                if (alignedDump) {
                    if (static_cast<size_t>(index) >= alignedLog.size()) alignedLog.resize(static_cast<size_t>(index) + 1, 0.0f);
                    alignedLog[static_cast<size_t>(index)] = dry;
                    if (static_cast<size_t>(index) >= positionLog.size()) positionLog.resize(static_cast<size_t>(index) + 1, 0.0);
                    positionLog[static_cast<size_t>(index)] = wave.readPosition(index);
                }
            }
        } else if (end > published) {
            const int64_t count = end - published;
            // A brief WORLD voiced/unvoiced or note-step failure can sound
            // like a blip even when its amplitude is below a click detector's
            // threshold. Compare periodicity of the synthesized voice at the
            // requested F0 with the original at its source F0. Only if the
            // original is clearly harmonic and the synthesized voice is not,
            // return to dry for that short region. This is evaluated on the
            // worker with lookahead; no analysis runs on the audio callback.
            const int firstQualityFrame = std::max(0, static_cast<int>(
                std::floor((published - filterRadius) * 200.0 / rate)) - 4);
            const int lastQualityFrame = static_cast<int>(
                std::ceil((end + filterRadius) * 200.0 / rate)) + 2;
            std::vector<bool> badFrame(static_cast<size_t>(lastQualityFrame - firstQualityFrame + 1), false);
            const auto periodicity = [&](int64_t center, int period, bool synthesized) {
                if (period <= 0 || center < 2 * period) return 0.0;
                double cross = 0.0, left = 0.0, right = 0.0;
                for (int64_t sample = center - 2 * period; sample < center + 2 * period; ++sample) {
                    const float a = synthesized ? world.synthesized(sample)
                        : input[slot(sample)].load(std::memory_order_relaxed);
                    const float b = synthesized ? world.synthesized(sample + period)
                        : input[slot(sample + period)].load(std::memory_order_relaxed);
                    cross += static_cast<double>(a) * b;
                    left += static_cast<double>(a) * a;
                    right += static_cast<double>(b) * b;
                }
                return cross / std::sqrt(std::max(1.0e-18, left * right));
            };
            // Optional step-gate override: desired-F0 steps (>= 50 cents between consecutive
            // frames), computed a few frames beyond the gated range.
            std::vector<bool> skippedFrame(badFrame.size(), false);
            std::vector<bool> stepAt; // index = frame - (firstQualityFrame - 6)
            if (experiment.gateSkipSteps) {
                stepAt.assign(badFrame.size() + 12, false);
                for (size_t i = 1; i < stepAt.size(); ++i) {
                    const int f = firstQualityFrame - 6 + static_cast<int>(i);
                    const double a = world.desiredHz(static_cast<int64_t>(std::lround((f - 1) * rate / 200.0)));
                    const double b = world.desiredHz(static_cast<int64_t>(std::lround(f * rate / 200.0)));
                    stepAt[i] = a > 0.0 && b > 0.0 && std::abs(1200.0 * std::log2(b / a)) >= 50.0;
                }
            }
            for (int frame = firstQualityFrame; frame <= lastQualityFrame; ++frame) {
                const int64_t center = static_cast<int64_t>(std::lround(frame * rate / 200.0));
                const double sourceHz = world.sourceHz(center);
                const double desiredHz = world.desiredHz(center);
                if (!experiment.qualityGate) continue; // Disable the WORLD quality gate with TUNA_QUALITY_GATE=0
                // Below 130 Hz the longer autocorrelation window needs more
                // lookahead than this latency budget; leave the normal WORLD
                // boundary handling in place for bass voices.
                if (sourceHz < 130.0 || desiredHz < 130.0) continue;
                const int sourcePeriod = std::max(1, static_cast<int>(std::lround(rate / sourceHz)));
                const int desiredPeriod = std::max(1, static_cast<int>(std::lround(rate / desiredHz)));
                if (experiment.gateSkipSteps) {
                    const int span = std::max(sourcePeriod, desiredPeriod);
                    bool skip = false;
                    const int reachFrames = static_cast<int>(std::ceil(2.0 * span * 200.0 / rate)) + 1;
                    for (int g = frame - reachFrames; g <= frame + reachFrames && !skip; ++g) {
                        const size_t i = static_cast<size_t>(g - (firstQualityFrame - 6));
                        if (i < stepAt.size() && stepAt[i]
                            && std::abs(static_cast<double>(std::lround(g * rate / 200.0)) - center) <= 2.0 * span)
                            skip = true;
                    }
                    if (!skip && sourceHz > 0.0) {
                        double lo = 1.0e9, hi = 0.0;
                        const int stepSamples = std::max(1, rate / 800);
                        for (int64_t n = center - 2 * span; n <= center + 3 * span; n += stepSamples) {
                            const double hz = world.desiredHz(n);
                            if (hz <= 0.0) { lo = 0.0; hi = 1.0e9; break; }
                            lo = std::min(lo, hz); hi = std::max(hi, hz);
                        }
                        if (lo <= 0.0 || 1200.0 * std::log2(hi / lo) > 10.0) skip = true;
                    }
                    if (skip) { skippedFrame[static_cast<size_t>(frame - firstQualityFrame)] = true; continue; }
                }
                const double dryScore = periodicity(center, sourcePeriod, false);
                const double wetScore = periodicity(center, desiredPeriod, true);
                badFrame[static_cast<size_t>(frame - firstQualityFrame)] =
                    dryScore > 0.85 && wetScore < 0.75 && wetScore < dryScore - 0.15;
            }
            std::vector<float> suppress(badFrame.size(), 0.0f);
            for (size_t frame = 0; frame < badFrame.size(); ++frame) {
                const size_t lo = frame > 2 ? frame - 2 : 0;
                const size_t hi = std::min(badFrame.size(), frame + 3);
                suppress[frame] = std::any_of(badFrame.begin() + static_cast<std::ptrdiff_t>(lo),
                    badFrame.begin() + static_cast<std::ptrdiff_t>(hi), [](bool bad) { return bad; }) ? 1.0f : 0.0f;
            }
            logFrames(published, end, &suppress, firstQualityFrame, false, &skippedFrame);
            correctionWindow.resize(static_cast<size_t>(count + 2 * filterRadius));
            for (int64_t offset = 0; offset < count + 2 * filterRadius; ++offset) {
                const int64_t index = published + offset - filterRadius;
                if (index < 0) { correctionWindow[static_cast<size_t>(offset)] = 0.0f; continue; }
                const float dry = input[slot(index)].load(std::memory_order_relaxed);
                const double position = index * 200.0 / rate - firstQualityFrame;
                const size_t frame = static_cast<size_t>(std::clamp(
                    static_cast<int>(std::floor(position)), 0, static_cast<int>(suppress.size()) - 2));
                const float fraction = static_cast<float>(std::clamp(position - frame, 0.0, 1.0));
                const float dryGuard = suppress[frame] * (1.0f - fraction)
                    + suppress[frame + 1] * fraction;
                correctionWindow[static_cast<size_t>(offset)] =
                    (world.corrected(index, dry) - dry) * (1.0f - dryGuard);
            }
            for (int64_t offset = 0; offset < count; ++offset) {
                double lowCorrection = 0.0;
                for (size_t tap = 0; tap < lowpass.size(); ++tap)
                    lowCorrection += lowpass[tap] * correctionWindow[static_cast<size_t>(offset) + tap];
                const int64_t index = published + offset;
                const float dry = input[slot(index)].load(std::memory_order_relaxed);
                output[slot(index)].store(dry + static_cast<float>(lowCorrection), std::memory_order_relaxed);
            }
        }
        published = end;
        readyUntil.store(end, std::memory_order_release);
        const auto finish = std::chrono::steady_clock::now();
        const auto cpuFinish = threadCpuTimeNanos();
        const auto cpuMicros = static_cast<int64_t>(cpuFinish >= cpuStart ? (cpuFinish - cpuStart) / 1000ULL : 0ULL);
        auto previousCpuMax = maxWorkerCpuMicros.load(std::memory_order_relaxed);
        while (cpuMicros > previousCpuMax && !maxWorkerCpuMicros.compare_exchange_weak(
            previousCpuMax, cpuMicros, std::memory_order_relaxed)) {}
        const auto postMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            finish - postStart).count();
        auto previousPostMax = maxPostMicros.load(std::memory_order_relaxed);
        while (postMicros > previousPostMax && !maxPostMicros.compare_exchange_weak(
            previousPostMax, postMicros, std::memory_order_relaxed)) {}
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            finish - renderStart).count();
        auto previousMax = maxRenderMicros.load(std::memory_order_relaxed);
        while (micros > previousMax && !maxRenderMicros.compare_exchange_weak(
            previousMax, micros, std::memory_order_relaxed)) {}
        if (timingPath && timings.size() < timingLimit)
            timings.push_back({ next, available, readyBefore, clockMicros(renderStart), clockMicros(finish),
                cpuMicros, written.load(std::memory_order_acquire), end, underruns.load(std::memory_order_relaxed) });
        next += hop;
    }
    if (timingPath) {
        std::ofstream trace(timingPath, std::ios::trunc);
        trace << "next,available,ready_before,start_us,finish_us,cpu_us,available_after,ready_after,underruns\n";
        for (const auto& row : timings)
            trace << row.next << ',' << row.available << ',' << row.readyBefore << ',' << row.startUs << ','
                  << row.finishUs << ',' << row.cpuUs << ',' << row.availableAfter << ',' << row.readyAfter
                  << ',' << row.underruns << '\n';
    }
    if (alignedDump && !alignedLog.empty()) {
        std::ofstream dump(alignedDump, std::ios::binary | std::ios::trunc);
        dump.write(reinterpret_cast<const char*>(alignedLog.data()), static_cast<std::streamsize>(alignedLog.size() * sizeof(float)));
        std::ofstream positions(std::string(alignedDump) + ".pos", std::ios::binary | std::ios::trunc);
        positions.write(reinterpret_cast<const char*>(positionLog.data()), static_cast<std::streamsize>(positionLog.size() * sizeof(double)));
    }
    if (waveform && std::getenv("TUNA_WAVE_STATS")) {
        const auto st = wave.stats();
        std::cerr << "wave_stats hops=" << st.hops << " islands=" << st.islands << " silence=" << st.pureSilence << " wsola=" << st.pureWsola
                  << " forced=" << st.pureForced << " late_exit=" << st.pureLateExit << " max_offset_ms=" << st.maxOffsetSamples * 1000.0 / rate
                  << " hop_ms_max=" << st.hopMsMax << " hop_ms_mean=" << st.hopMsMean << " hop_ms_p99=" << st.hopMsP99 << '\n';
    }
}

} // namespace cody_pitch
