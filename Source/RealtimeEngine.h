#pragma once

#include "PitchEngine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace cody_pitch {

// Continuously renders overlapping analysis windows on a worker. The audio callback only
// writes/reads preallocated rings; playback is delayed by a fixed, host-reported latency.
class RealtimeEngine {
public:
    RealtimeEngine() = default;
    ~RealtimeEngine();
    RealtimeEngine(const RealtimeEngine&) = delete;
    RealtimeEngine& operator=(const RealtimeEngine&) = delete;

    void prepare(int sampleRate, std::function<Settings()> settingsProvider);
    void stop();
    void process(float* samples, int count, bool wet, bool offline = false) noexcept;
    void processStereo(float* left, float* right, int count, bool wet, bool offline = false) noexcept;
    // Latency modes: 0 High Quality, 1 Low, 2 Lowest, 3 Live. Reported latency = hop + context + headroom.
    static int latencyFor(int rate, int mode, bool worldRenderer) noexcept;
    int latencySamples() const noexcept { return latencyNow.load(std::memory_order_relaxed); }
    int activeLatencyMode() const noexcept { return latencyMode.load(std::memory_order_relaxed); }
    float detectedHz() const noexcept { return detected.load(std::memory_order_relaxed); }
    float targetHz() const noexcept { return target.load(std::memory_order_relaxed); }
    int underrunCount() const noexcept { return underruns.load(std::memory_order_relaxed); }
    int64_t firstUnderrunSample() const noexcept { return firstUnderrun.load(std::memory_order_relaxed); }
    int64_t lastUnderrunSample() const noexcept { return lastUnderrun.load(std::memory_order_relaxed); }
    int64_t longestRenderMicros() const noexcept { return maxRenderMicros.load(std::memory_order_relaxed); }
    int64_t longestAnalysisMicros() const noexcept { return maxAnalysisMicros.load(std::memory_order_relaxed); }
    // Time inside the renderer (WORLD or waveform processWindow) alone, to tell a renderer cost from a host stall.
    int64_t longestRendererMicros() const noexcept { return maxRendererMicros.load(std::memory_order_relaxed); }
    int64_t longestPostMicros() const noexcept { return maxPostMicros.load(std::memory_order_relaxed); }
    int64_t longestWorkerCpuMicros() const noexcept { return maxWorkerCpuMicros.load(std::memory_order_relaxed); }

private:
    void run();
    void waitFor(int64_t end) noexcept;
    void beginBlockLatency() noexcept;
    float blendWet(float dry, float processed, bool ready) noexcept;
    size_t slot(int64_t index) const noexcept { return static_cast<size_t>(index % static_cast<int64_t>(capacity)); }

    int rate = 48000;
    std::atomic<int> latencyNow { 48000 };   // read by the audio thread once per block
    std::atomic<int> latencyMode { 0 };
    int audioLatency = 48000, fadeFromLatency = 48000, latencyFadeLeft = 0; // audio thread only
    int hop = 12000;
    int context = 6000;
    size_t capacity = 0;
    std::unique_ptr<std::atomic<float>[]> input;
    std::unique_ptr<std::atomic<float>[]> side;
    std::unique_ptr<std::atomic<float>[]> output;
    std::function<Settings()> settings;
    std::thread worker;
    std::atomic<bool> running { false };
    std::atomic<int64_t> written { 0 };
    std::atomic<int64_t> readyFrom { 0 };
    std::atomic<int64_t> readyUntil { 0 };
    std::atomic<float> detected { 0.0f };
    std::atomic<float> target { 0.0f };
    std::atomic<int> underruns { 0 };
    std::atomic<int64_t> firstUnderrun { -1 };
    std::atomic<int64_t> lastUnderrun { -1 };
    std::atomic<int64_t> maxRenderMicros { 0 };
    std::atomic<int64_t> maxAnalysisMicros { 0 };
    std::atomic<int64_t> maxPostMicros { 0 };
    std::atomic<int64_t> maxRendererMicros { 0 };
    std::atomic<int64_t> maxWorkerCpuMicros { 0 };
    bool previousWetReady = false;
    int transitionRemaining = 0;
    float transitionFrom = 0.0f;
    float lastCorrection = 0.0f;
};

} // namespace cody_pitch
