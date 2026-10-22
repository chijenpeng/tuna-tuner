// The plug-in ships at one delay (320 ms = hop 40 + context 200 + headroom 80): the measured delay of the dry path
// must equal the reported latency at every rate, mono and stereo.
#include "RealtimeEngine.h"
#include "TestEnvironment.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
const char* modeName[4] = { "High Quality", "Low", "Lowest", "Live" };
int failures = 0;

cody_pitch::Settings baseSettings() {
    cody_pitch::Settings s;
    s.allowedNotes = cody_pitch::scaleMask(9, 1);
    s.amount = 1.0f; s.retuneMs = 0.0f; s.transitionMs = 0.0f; s.toleranceCents = 0.0f; s.expression = 0.0f;
    return s;
}

// dry-path impulse: returns the arrival index (or -1)
int measure(cody_pitch::RealtimeEngine& engine, int rate, bool stereo) {
    const int delay = engine.latencySamples();
    const int block = 256, total = delay + 4 * block;
    int arrival = -1;
    for (int offset = 0; offset < total; offset += block) {
        std::vector<float> left(block, 0.0f), right(block, 0.0f);
        if (offset == 0) { left[0] = 0.7f; right[0] = stereo ? -0.2f : 0.7f; }
        if (stereo) engine.processStereo(left.data(), right.data(), block, false);
        else engine.process(left.data(), block, false);
        for (int i = 0; i < block; ++i)
            if (std::abs(left[i]) > 0.3f && arrival < 0) arrival = offset + i;
    }
    (void) rate;
    return arrival;
}
} // namespace

int main(int argc, char** argv) {
    const bool pacing = argc > 1 && std::strcmp(argv[1], "--pacing") == 0;
    const bool world = std::getenv("TUNA_RENDERER") && std::string(std::getenv("TUNA_RENDERER")) == "world";
    for (const int rate : { 44100, 48000, 96000 }) {
        for (int mode = 0; mode < 1; ++mode) {
            for (const bool stereo : { false, true }) {
                auto settings = baseSettings();
                settings.latencyMode = mode;
                cody_pitch::RealtimeEngine engine;
                engine.prepare(rate, [settings] { return settings; });
                const int expected = cody_pitch::RealtimeEngine::latencyFor(rate, mode, world);
                const int measured = measure(engine, rate, stereo);
                engine.stop();
                const bool ok = engine.latencySamples() == expected && measured == expected;
                if (!ok) ++failures;
                std::cout << (ok ? "PASS " : "FAIL ") << rate << " Hz " << modeName[mode] << (stereo ? " stereo" : " mono")
                          << ": reported " << engine.latencySamples() << ", formula " << expected << ", measured " << measured
                          << " (" << 1000.0 * measured / rate << " ms)\n";
            }
        }
    }
    if (pacing) {
        const double load = systemLoadEquivalentCores();
        if (load > 4.0) std::cout << "pacing SKIPPED: load equivalent " << load << " cores exceeds 4\n";
        else
            for (const int rate : { 44100, 48000, 96000 })
                for (int mode = 0; mode < 1; ++mode) {
                    auto settings = baseSettings();
                    settings.latencyMode = mode;
                    cody_pitch::RealtimeEngine engine;
                    engine.prepare(rate, [settings] { return settings; });
                    const int duration = 6 * rate, block = 256;
                    const auto t0 = std::chrono::steady_clock::now();
                    double phase = 0.0;
                    for (int offset = 0; offset < duration; offset += block) {
                        float samples[256] {};
                        const int count = std::min(block, duration - offset);
                        for (int i = 0; i < count; ++i) {
                            phase += 2 * 3.14159265358979 * 205.0 * (1.0 + 0.01 * std::sin(2 * 3.14159265358979 * 5.0 * (offset + i) / rate)) / rate;
                            double v = 0.0;
                            for (int h = 1; h <= 20; ++h) v += std::sin(h * phase) / h;
                            samples[i] = static_cast<float>(0.12 * v);
                        }
                        engine.process(samples, count, true);
                        std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(offset + count) * 1000000 / rate));
                    }
                    std::cout << "pacing " << rate << " Hz " << modeName[mode] << ": underruns " << engine.underrunCount()
                              << ", longest_render_ms " << engine.longestRenderMicros() / 1000.0 << ", longest_renderer_ms "
                              << engine.longestRendererMicros() / 1000.0 << ", worker cpu ms " << engine.longestWorkerCpuMicros() / 1000.0 << '\n';
                    engine.stop();
                }
    }
    std::cout << (failures ? "latency test FAILED\n" : "latency test passed\n");
    return failures ? 1 : 0;
}
