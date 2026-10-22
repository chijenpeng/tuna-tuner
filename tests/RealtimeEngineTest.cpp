#include <cstdlib>
#include "TestEnvironment.h"
#include "RealtimeEngine.h"

#include <world/dio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    // The tests below were written for the legacy path: set every flag explicitly instead of relying on defaults.
    setTestEnvironment("TUNA_RENDERER", "world"); setTestEnvironment("TUNA_SPLICE_PURE", "0");
    setTestEnvironment("TUNA_VOICING_BRIDGE_MS", "0"); setTestEnvironment("TUNA_VOICING_BRIDGE_MODE", "0");
    setTestEnvironment("TUNA_TARGET_CENTER_MS", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_CPMS", "0");
    setTestEnvironment("TUNA_VELOCITY_GATE_MODE", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_RELEASE_MS", "20");

    constexpr int sampleRate = 48000;
    constexpr int block = 256;
    constexpr int duration = 3 * sampleRate;
    constexpr double pi = 3.14159265358979323846;
    const double inputHz = 220.0 * std::exp2(45.0 / 1200.0);
    cody_pitch::Settings settings;
    settings.allowedNotes = cody_pitch::scaleMask(9, 1);
    settings.amount = 1.0f;
    // A zero-time correction setting is the highest seam-risk case.
    settings.retuneMs = 0.0f;
    settings.transitionMs = 0.0f;
    settings.toleranceCents = 0.0f;
    settings.expression = 0.0f;
    cody_pitch::RealtimeEngine engine;
    engine.prepare(sampleRate, [&settings] { return settings; });
    if (engine.latencySamples() != sampleRate * 8 / 25) {
        std::cerr << "Unexpected DAW-reported latency\n"; return 1;
    }

    std::vector<double> playback(static_cast<size_t>(duration));
    const auto playbackStart = std::chrono::steady_clock::now();
    int64_t maxFeedLateMicros = 0;
    for (int offset = 0; offset < duration; offset += block) {
        const auto ideal = playbackStart + std::chrono::microseconds(
            static_cast<int64_t>(offset) * 1000000 / sampleRate);
        maxFeedLateMicros = std::max(maxFeedLateMicros,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - ideal).count());
        const int count = std::min(block, duration - offset);
        float samples[block] {};
        for (int i = 0; i < count; ++i) {
            const double phase = 2.0 * pi * inputHz * (offset + i) / sampleRate;
            samples[i] = static_cast<float>(0.18 * std::sin(phase) + 0.07 * std::sin(2 * phase));
        }
        engine.process(samples, count, true);
        for (int i = 0; i < count; ++i) playback[static_cast<size_t>(offset + i)] = samples[i];
        // The host supplies one block per block-duration, not 2 ms per 5.3 ms
        // block. A faster-than-real-time feed belongs to the offline test below.
        std::this_thread::sleep_until(playbackStart + std::chrono::microseconds(
            static_cast<int64_t>(offset + count) * 1000000 / sampleRate));
    }
    const int underruns = engine.underrunCount();
    const auto longestRenderMicros = engine.longestRenderMicros();
    engine.stop();
    if (underruns != 0) {
        std::cerr << "Realtime worker underruns: " << underruns
                  << "; longest render: " << longestRenderMicros / 1000.0
                  << " ms; max worker CPU: " << engine.longestWorkerCpuMicros() / 1000.0
                  << " ms; first/last missing source samples: " << engine.firstUnderrunSample() << '/'
                  << engine.lastUnderrunSample() << "; max feed lateness: " << maxFeedLateMicros / 1000.0 << " ms\n";
        return 1;
    }
    for (int seam = engine.latencySamples() + 1; seam < duration; ++seam) {
        const double jump = std::abs(playback[static_cast<size_t>(seam)] - playback[static_cast<size_t>(seam - 1)]);
        if (jump > 0.12) {
            std::cerr << "Audible-sized processing seam at " << seam << ": " << jump << '\n'; return 1;
        }
    }
    DioOption option; InitializeDioOption(&option);
    option.f0_floor = 70; option.f0_ceil = 900; option.frame_period = 5;
    const int n = GetSamplesForDIO(sampleRate, duration, 5);
    std::vector<double> times(static_cast<size_t>(n)), f0(static_cast<size_t>(n));
    Dio(playback.data(), duration, sampleRate, &option, times.data(), f0.data());
    std::vector<double> middle;
    for (int i = 0; i < n; ++i)
        if (times[static_cast<size_t>(i)] > 1.6 && times[static_cast<size_t>(i)] < 2.3 && f0[static_cast<size_t>(i)] > 0)
            middle.push_back(f0[static_cast<size_t>(i)]);
    if (middle.empty()) { std::cerr << "No pitch in live output\n"; return 1; }
    std::nth_element(middle.begin(), middle.begin() + middle.size() / 2, middle.end());
    const double errorCents = 1200.0 * std::log2(middle[middle.size() / 2] / 220.0);
    if (std::abs(errorCents) > 25.0) {
        std::cerr << "Live output missed A3 by " << errorCents << " cents\n"; return 1;
    }
    cody_pitch::RealtimeEngine offline;
    offline.prepare(sampleRate, [&settings] { return settings; });
    for (int offset = 0; offset < duration; offset += block) {
        const int count = std::min(block, duration - offset);
        float samples[block] {};
        for (int i = 0; i < count; ++i) {
            const double phase = 2.0 * pi * inputHz * (offset + i) / sampleRate;
            samples[i] = static_cast<float>(0.18 * std::sin(phase) + 0.07 * std::sin(2 * phase));
        }
        offline.process(samples, count, true, true); // deliberately no wall-clock pacing
    }
    if (offline.underrunCount() != 0) {
        std::cerr << "Offline bounce worker underruns: " << offline.underrunCount() << '\n'; return 1;
    }
    offline.stop();
    cody_pitch::RealtimeEngine impulse;
    impulse.prepare(sampleRate, [&settings] { return settings; });
    const int delay = impulse.latencySamples();
    for (int offset = 0; offset <= delay + block; offset += block) {
        float left[block] {}, right[block] {};
        if (offset == 0) { left[0] = 0.7f; right[0] = -0.2f; }
        impulse.processStereo(left, right, block, false);
        for (int i = 0; i < block; ++i) {
            const bool arrival = offset + i == delay;
            if (std::abs(left[i] - (arrival ? 0.7f : 0.0f)) > 1.0e-6f
                || std::abs(right[i] - (arrival ? -0.2f : 0.0f)) > 1.0e-6f) {
                std::cerr << "Dry stereo impulse does not match reported latency\n"; return 1;
            }
        }
    }
    impulse.stop();

    // A zero-correction setting is exact delayed dry, with no synthetic voice
    // and no spurious underrun count. This also covers old sessions that have
    // the Amount automation set to zero.
    cody_pitch::Settings zeroSettings = settings;
    zeroSettings.amount = 0.0f;
    cody_pitch::RealtimeEngine zeroEngine;
    zeroEngine.prepare(sampleRate, [zeroSettings] { return zeroSettings; });
    const int zeroDelay = zeroEngine.latencySamples();
    for (int offset = 0; offset < sampleRate + zeroDelay; offset += block) {
        const int count = std::min(block, sampleRate + zeroDelay - offset);
        float samples[block] {};
        for (int i = 0; i < count && offset + i < sampleRate; ++i)
            samples[i] = static_cast<float>(0.1 * std::sin(2.0 * pi * 257.0 * (offset + i) / sampleRate));
        zeroEngine.process(samples, count, true, true);
        for (int i = 0; i < count; ++i) {
            const int source = offset + i - zeroDelay;
            const float expected = source >= 0 && source < sampleRate
                ? static_cast<float>(0.1 * std::sin(2.0 * pi * 257.0 * source / sampleRate)) : 0.0f;
            if (std::abs(samples[i] - expected) > 1.0e-6f) {
                std::cerr << "Zero Amount changed delayed dry at " << source << '\n'; return 1;
            }
        }
    }
    if (zeroEngine.underrunCount() != 0) {
        std::cerr << "Zero Amount caused an underrun\n"; return 1;
    }
    zeroEngine.stop();

    cody_pitch::Settings emptySettings = settings;
    emptySettings.allowedNotes.fill(false);
    cody_pitch::RealtimeEngine emptyEngine;
    emptyEngine.prepare(sampleRate, [emptySettings] { return emptySettings; });
    for (int offset = 0; offset < sampleRate; offset += block) {
        float samples[block] {};
        for (int i = 0; i < block; ++i)
            samples[i] = static_cast<float>(0.1 * std::sin(2.0 * pi * 257.0 * (offset + i) / sampleRate));
        emptyEngine.process(samples, block, true, true);
        for (int i = 0; i < block; ++i) {
            const int source = offset + i - emptyEngine.latencySamples();
            const float expected = source >= 0
                ? static_cast<float>(0.1 * std::sin(2.0 * pi * 257.0 * source / sampleRate)) : 0.0f;
            if (std::abs(samples[i] - expected) > 1.0e-6f) {
                std::cerr << "Empty target mask changed delayed dry\n"; return 1;
            }
        }
    }
    if (emptyEngine.underrunCount() != 0) {
        std::cerr << "Empty target mask caused an underrun\n"; return 1;
    }
    emptyEngine.stop();

    // At 96 kHz the worker must keep up with actual host pacing, not only
    // with offline waitFor(). The full synthesis rate stays at the host rate.
    constexpr int highRate = 96000;
    cody_pitch::RealtimeEngine highRateEngine;
    highRateEngine.prepare(highRate, [&settings] { return settings; });
    const auto highRateStart = std::chrono::steady_clock::now();
    for (int offset = 0; offset < 2 * highRate; offset += block) {
        float samples[block] {};
        for (int i = 0; i < block; ++i)
            samples[i] = static_cast<float>(0.15 * std::sin(2.0 * pi * inputHz * (offset + i) / highRate));
        highRateEngine.process(samples, block, true);
        std::this_thread::sleep_until(highRateStart + std::chrono::microseconds(
            static_cast<int64_t>(offset + block) * 1000000 / highRate));
    }
    const int highRateUnderruns = highRateEngine.underrunCount();
    const auto highRateLongest = highRateEngine.longestRenderMicros();
    highRateEngine.stop();
    if (highRateUnderruns != 0) {
        std::cerr << "96 kHz realtime underruns: " << highRateUnderruns
                  << "; longest render: " << highRateLongest / 1000.0 << " ms\n";
        return 1;
    }

    // The right channel is not a second singer: stereo side must retain its
    // original delayed waveform while the common mid is corrected once.
    cody_pitch::RealtimeEngine stereoWet;
    stereoWet.prepare(sampleRate, [&settings] { return settings; });
    const int stereoDelay = stereoWet.latencySamples();
    for (int offset = 0; offset < 2 * sampleRate + stereoDelay; offset += block) {
        const int count = std::min(block, 2 * sampleRate + stereoDelay - offset);
        float left[block] {}, right[block] {};
        for (int i = 0; i < count && offset + i < 2 * sampleRate; ++i) {
            const double t = (offset + i) / static_cast<double>(sampleRate);
            const float mid = static_cast<float>(0.14 * std::sin(2.0 * pi * inputHz * t));
            const float side = static_cast<float>(0.015 * std::sin(2.0 * pi * 997.0 * t));
            left[i] = mid + side;
            right[i] = mid - side;
        }
        stereoWet.processStereo(left, right, count, true, true);
        for (int i = 0; i < count; ++i) {
            const int source = offset + i - stereoDelay;
            const float expectedDifference = source >= 0 && source < 2 * sampleRate
                ? static_cast<float>(0.03 * std::sin(2.0 * pi * 997.0 * source / sampleRate)) : 0.0f;
            if (std::abs(left[i] - right[i] - expectedDifference) > 2.0e-6f) {
                std::cerr << "Wet stereo side was altered at " << source << '\n'; return 1;
            }
        }
    }
    if (stereoWet.underrunCount() != 0) {
        std::cerr << "Wet stereo caused an underrun\n"; return 1;
    }
    stereoWet.stop();

    std::atomic<bool> hardMode { false };
    cody_pitch::RealtimeEngine automation;
    automation.prepare(sampleRate, [&] {
        auto current = settings;
        current.retuneMs = hardMode.load() ? 0.0f : 45.0f;
        current.transitionMs = hardMode.load() ? 0.0f : 55.0f;
        current.expression = hardMode.load() ? 0.0f : 0.9f;
        return current;
    });
    const int automationDelay = automation.latencySamples();
    std::vector<float> switched(static_cast<size_t>(duration));
    for (int offset = 0; offset < duration + automationDelay; offset += block) {
        if (offset >= sampleRate) hardMode.store(true);
        const int count = std::min(block, duration + automationDelay - offset);
        float samples[block] {};
        for (int i = 0; i < count; ++i) {
            const int sourceSample = offset + i;
            if (sourceSample >= duration) break;
            const double phase = 2.0 * pi * inputHz * sourceSample / sampleRate;
            samples[i] = static_cast<float>(0.18 * std::sin(phase) + 0.07 * std::sin(2 * phase));
        }
        automation.process(samples, count, true, true);
        for (int i = 0; i < count; ++i) {
            const int aligned = offset + i - automationDelay;
            if (aligned >= 0 && aligned < duration) switched[static_cast<size_t>(aligned)] = samples[i];
        }
    }
    if (automation.underrunCount() != 0) {
        std::cerr << "Automation-switch underrun: " << automation.underrunCount() << '\n'; return 1;
    }
    automation.stop();
    for (int i = sampleRate / 2; i < duration - 1; ++i) {
        const double curvature = std::abs(switched[static_cast<size_t>(i + 1)]
            - 2.0 * switched[static_cast<size_t>(i)] + switched[static_cast<size_t>(i - 1)]);
        if (curvature > 0.12) {
            std::cerr << "Automation switch produced a one-sample spike at " << i << ": " << curvature << '\n';
            return 1;
        }
    }

    // A continuous synthesizer must not reintroduce the original voice or a
    // 40 ms hop-rate sideband under the intended G4.
    cody_pitch::Settings coherentSettings = settings;
    coherentSettings.allowedNotes = cody_pitch::scaleMask(2, 1);
    coherentSettings.minHz = 240.0f;
    coherentSettings.maxHz = 700.0f;
    std::vector<float> coherentSource(static_cast<size_t>(duration));
    for (int i = 0; i < duration; ++i) {
        const double phase = 2.0 * pi * 380.0 * i / sampleRate;
        coherentSource[static_cast<size_t>(i)] = static_cast<float>(
            0.18 * std::sin(phase) + 0.08 * std::sin(2 * phase) + 0.04 * std::sin(3 * phase));
    }
    cody_pitch::RealtimeEngine coherentEngine;
    coherentEngine.prepare(sampleRate, [coherentSettings] { return coherentSettings; });
    const int coherentDelay = coherentEngine.latencySamples();
    std::vector<float> coherentStream(static_cast<size_t>(duration));
    for (int offset = 0; offset < duration + coherentDelay; offset += block) {
        const int count = std::min(block, duration + coherentDelay - offset);
        float samples[block] {};
        for (int i = 0; i < count; ++i)
            if (offset + i < duration)
                samples[i] = coherentSource[static_cast<size_t>(offset + i)];
        coherentEngine.process(samples, count, true, true);
        for (int i = 0; i < count; ++i) {
            const int aligned = offset + i - coherentDelay;
            if (aligned >= 0 && aligned < duration)
                coherentStream[static_cast<size_t>(aligned)] = samples[i];
        }
    }
    if (coherentEngine.underrunCount() != 0) {
        std::cerr << "Coherent render underruns: " << coherentEngine.underrunCount() << '\n'; return 1;
    }
    coherentEngine.stop();
    const auto lineMagnitude = [&](double frequency) {
        double real = 0.0, imaginary = 0.0;
        for (int i = sampleRate; i < sampleRate * 2; ++i) {
            const double position = (i - sampleRate) / static_cast<double>(sampleRate);
            const double window = 0.5 - 0.5 * std::cos(2.0 * pi * position);
            const double phase = 2.0 * pi * frequency * position;
            real += coherentStream[static_cast<size_t>(i)] * window * std::cos(phase);
            imaginary -= coherentStream[static_cast<size_t>(i)] * window * std::sin(phase);
        }
        return std::hypot(real, imaginary);
    };
    const double targetLine = lineMagnitude(392.0);
    const double originalLine = lineMagnitude(380.0);
    const double lowerHopSideband = lineMagnitude(367.0);
    const double upperHopSideband = lineMagnitude(417.0);
    std::cout << "synthetic lines: source/target=" << originalLine / targetLine
              << " lower/target=" << lowerHopSideband / targetLine
              << " upper/target=" << upperHopSideband / targetLine << '\n';
    if (targetLine < 100.0 || originalLine / targetLine > 0.1
        || lowerHopSideband / targetLine > 0.2 || upperHopSideband / targetLine > 0.2) {
        std::cerr << "Original voice or hop sideband remains in synthetic correction\n";
        return 1;
    }
    for (int semitones : { -12, 12 }) {
        auto transposedSettings = settings;
        transposedSettings.allowedNotes.fill(false);
        transposedSettings.allowedNotes[9] = true; // A3 input
        transposedSettings.transposeSemitones = static_cast<float>(semitones);
        cody_pitch::RealtimeEngine transposedEngine;
        transposedEngine.prepare(sampleRate, [transposedSettings] { return transposedSettings; });
        const int delaySamples = transposedEngine.latencySamples();
        std::vector<double> transposed(static_cast<size_t>(duration));
        for (int offset = 0; offset < duration + delaySamples; offset += block) {
            const int count = std::min(block, duration + delaySamples - offset);
            float samples[block] {};
            for (int i = 0; i < count && offset + i < duration; ++i) {
                const double phase = 2.0 * pi * 220.0 * (offset + i) / sampleRate;
                samples[i] = static_cast<float>(0.18 * std::sin(phase)
                    + 0.07 * std::sin(2.0 * phase));
            }
            transposedEngine.process(samples, count, true, true);
            for (int i = 0; i < count; ++i) {
                const int aligned = offset + i - delaySamples;
                if (aligned >= 0 && aligned < duration)
                    transposed[static_cast<size_t>(aligned)] = samples[i];
                if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 0.981f) {
                    std::cerr << "Transposed output is nonfinite or above safety ceiling\n"; return 1;
                }
            }
        }
        if (transposedEngine.underrunCount() != 0) {
            std::cerr << "Transposed render underrun\n"; return 1;
        }
        transposedEngine.stop();
        std::vector<double> transposedTimes(static_cast<size_t>(n));
        std::vector<double> transposedF0(static_cast<size_t>(n));
        Dio(transposed.data(), duration, sampleRate, &option,
            transposedTimes.data(), transposedF0.data());
        std::vector<double> stable;
        for (int i = 0; i < n; ++i)
            if (transposedTimes[static_cast<size_t>(i)] > 1.0
                && transposedTimes[static_cast<size_t>(i)] < 2.0
                && transposedF0[static_cast<size_t>(i)] > 0)
                stable.push_back(transposedF0[static_cast<size_t>(i)]);
        if (stable.empty()) { std::cerr << "No transposed pitch\n"; return 1; }
        std::nth_element(stable.begin(), stable.begin() + stable.size() / 2, stable.end());
        const double expected = 220.0 * std::exp2(semitones / 12.0);
        const double error = 1200.0 * std::log2(stable[stable.size() / 2] / expected);
        if (std::abs(error) > 35.0) {
            std::cerr << "Transpose " << semitones << " missed by " << error << " cents\n";
            return 1;
        }
    }
    std::cout << "PASS: continuous processing, no capture, " << errorCents
              << " cents from A3; longest render " << longestRenderMicros / 1000.0 << " ms\n";
    return 0;
}
