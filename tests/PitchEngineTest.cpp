#include <cstdlib>
#include "TestEnvironment.h"
#include "PitchEngine.h"
#include <world/dio.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    // The tests below were written for the legacy path: set every flag explicitly instead of relying on defaults.
    setTestEnvironment("TUNA_RENDERER", "world"); setTestEnvironment("TUNA_SPLICE_PURE", "0");
    setTestEnvironment("TUNA_VOICING_BRIDGE_MS", "0"); setTestEnvironment("TUNA_VOICING_BRIDGE_MODE", "0");
    setTestEnvironment("TUNA_TARGET_CENTER_MS", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_CPMS", "0");
    setTestEnvironment("TUNA_VELOCITY_GATE_MODE", "0"); setTestEnvironment("TUNA_VELOCITY_GATE_RELEASE_MS", "20");

    constexpr int sampleRate = 48000;
    constexpr double pi = 3.14159265358979323846;
    const auto aMinor = cody_pitch::scaleMask(9, 1);
    const std::array<bool, 12> expected { true, false, true, false, true, true,
                                           false, true, false, true, false, true };
    if (aMinor != expected) { std::cerr << "A minor mask failed\n"; return 1; }

    std::vector<float> source(static_cast<size_t>(sampleRate));
    const double hz = 220.0 * std::exp2(45.0 / 1200.0);
    for (int i = 0; i < sampleRate * 3 / 4; ++i) {
        const double phase = 2.0 * pi * hz * i / sampleRate;
        source[static_cast<size_t>(i)] = static_cast<float>(0.18 * std::sin(phase)
            + 0.07 * std::sin(2 * phase) + 0.04 * std::sin(3 * phase));
    }
    // A short unpitched tail must pass through unchanged.
    for (int i = sampleRate * 3 / 4; i < sampleRate; ++i)
        source[static_cast<size_t>(i)] = (i % 2 == 0 ? 0.01f : -0.01f);

    cody_pitch::Settings settings;
    settings.allowedNotes = aMinor;
    settings.amount = 0.0f;
    auto bypass = cody_pitch::render(source, sampleRate, settings);
    if (bypass.audio != source) { std::cerr << "Zero amount is not exact bypass\n"; return 1; }

    settings.amount = 1.0f;
    settings.retuneMs = 2.0f;
    settings.toleranceCents = 0.0f;
    auto processed = cody_pitch::render(source, sampleRate, settings);
    if (processed.audio.size() != source.size() || processed.correctedFrames == 0) {
        std::cerr << "No correction or length mismatch: voiced=" << processed.voicedFrames
                  << " corrected=" << processed.correctedFrames << '\n'; return 1;
    }
    if (!std::all_of(processed.audio.begin(), processed.audio.end(), [](float v) { return std::isfinite(v); })) {
        std::cerr << "Non-finite output\n"; return 1;
    }
    for (int i = sampleRate * 9 / 10; i < sampleRate; ++i)
        if (processed.audio[static_cast<size_t>(i)] != source[static_cast<size_t>(i)]) {
            std::cerr << "Unpitched tail was changed\n"; return 1;
        }
    std::vector<double> shifted(processed.audio.begin(), processed.audio.end());
    DioOption option; InitializeDioOption(&option);
    option.f0_floor = settings.minHz; option.f0_ceil = settings.maxHz; option.frame_period = 5;
    const int n = GetSamplesForDIO(sampleRate, static_cast<int>(shifted.size()), 5);
    std::vector<double> times(static_cast<size_t>(n)), f0(static_cast<size_t>(n));
    Dio(shifted.data(), static_cast<int>(shifted.size()), sampleRate, &option, times.data(), f0.data());
    std::vector<double> middle;
    for (int i = 0; i < n; ++i)
        if (times[static_cast<size_t>(i)] > 0.3 && times[static_cast<size_t>(i)] < 0.6 && f0[static_cast<size_t>(i)] > 0)
            middle.push_back(f0[static_cast<size_t>(i)]);
    if (middle.empty()) { std::cerr << "Output has no tracked pitch\n"; return 1; }
    std::nth_element(middle.begin(), middle.begin() + middle.size() / 2, middle.end());
    const double outputCents = 1200.0 * std::log2(middle[middle.size() / 2] / 220.0);
    if (std::abs(outputCents) > 20.0) {
        std::cerr << "Corrected pitch missed A3: " << outputCents << " cents\n"; return 1;
    }

    // Flex protects small gestures near a note but must not leave a fixed
    // 20-cent residual on a note that started far outside that zone.
    cody_pitch::Settings flexSettings;
    flexSettings.allowedNotes[9] = true; // A3
    flexSettings.amount = 1.0f;
    flexSettings.retuneMs = 0.0f;
    flexSettings.transitionMs = 0.0f;
    flexSettings.toleranceCents = 20.0f;
    flexSettings.expression = 0.0f;
    for (const double sourceCents : { 10.0, 70.0 }) {
        std::vector<float> flexSource(static_cast<size_t>(sampleRate));
        const double sourceHz = 220.0 * std::exp2(sourceCents / 1200.0);
        for (int i = 0; i < sampleRate; ++i) {
            const double phase = 2.0 * pi * sourceHz * i / sampleRate;
            flexSource[static_cast<size_t>(i)] = static_cast<float>(
                0.18 * std::sin(phase) + 0.07 * std::sin(2.0 * phase));
        }
        const auto flex = cody_pitch::render(flexSource, sampleRate, flexSettings, nullptr, false);
        const auto& middleFrame = flex.frames[100];
        const double destinationCents = 1200.0 * std::log2(middleFrame.desiredHz / 220.0);
        const double expectedCents = sourceCents < 20.0 ? sourceCents : 0.0;
        if (middleFrame.sourceHz <= 0.0f || std::abs(destinationCents - expectedCents) > 5.0) {
            std::cerr << "Flex zone mapped " << sourceCents << "c to "
                      << destinationCents << "c, expected " << expectedCents << "c\n";
            return 1;
        }
    }

    // Edit Notes: Remove changes the destination; Bypass leaves the detected
    // source note bit-identical, including the normally corrected vowel.
    settings.allowedNotes[9] = false;
    auto removed = cody_pitch::render(source, sampleRate, settings);
    const auto& removedFrame = removed.frames[80];
    if (removedFrame.sourceHz <= 0.0f || removedFrame.targetHz < 240.0f || removedFrame.targetHz > 255.0f) {
        std::cerr << "Remove did not redirect A3 to B3\n"; return 1;
    }
    settings.bypassNotes[9] = true;
    auto bypassedNote = cody_pitch::render(source, sampleRate, settings);
    if (bypassedNote.audio != source || bypassedNote.voicedFrames == 0 || bypassedNote.correctedFrames != 0) {
        std::cerr << "Bypass note was corrected or lost voiced tracking\n"; return 1;
    }
    settings.allowedNotes.fill(false);
    auto noTargets = cody_pitch::render(source, sampleRate, settings);
    if (noTargets.audio != source || noTargets.warning.empty()) {
        std::cerr << "Zero-target state did not fail safely to dry\n"; return 1;
    }

    // Zero-ms retune and zero-ms note transition must still produce a bounded,
    // finite waveform for shifts larger than two semitones in either direction.
    for (const double offsetCents : { -250.0, 250.0 }) {
        std::vector<float> extremeSource(static_cast<size_t>(sampleRate));
        const double extremeHz = 220.0 * std::exp2(offsetCents / 1200.0);
        for (int i = 0; i < sampleRate; ++i) {
            const double phase = 2.0 * pi * extremeHz * i / sampleRate;
            extremeSource[static_cast<size_t>(i)] = static_cast<float>(0.18 * std::sin(phase)
                + 0.07 * std::sin(2 * phase) + 0.04 * std::sin(3 * phase));
        }
        cody_pitch::Settings extreme;
        extreme.allowedNotes[9] = true;
        extreme.amount = 1.0f;
        extreme.retuneMs = 0.0f;
        extreme.transitionMs = 0.0f;
        extreme.toleranceCents = 0.0f;
        extreme.expression = 0.0f;
        const auto output = cody_pitch::render(extremeSource, sampleRate, extreme);
        if (output.audio.size() != extremeSource.size()
            || !std::all_of(output.audio.begin(), output.audio.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 0.981f; })) {
            std::cerr << "Extreme shift yielded non-finite or over-ceiling output\n"; return 1;
        }
        std::vector<double> outputSamples(output.audio.begin(), output.audio.end());
        std::vector<double> pitchTimes(static_cast<size_t>(n)), pitchHz(static_cast<size_t>(n));
        Dio(outputSamples.data(), sampleRate, sampleRate, &option, pitchTimes.data(), pitchHz.data());
        std::vector<double> stable;
        for (int i = 0; i < n; ++i)
            if (pitchTimes[static_cast<size_t>(i)] > 0.3 && pitchTimes[static_cast<size_t>(i)] < 0.7
                && pitchHz[static_cast<size_t>(i)] > 0.0)
                stable.push_back(pitchHz[static_cast<size_t>(i)]);
        if (stable.empty()) { std::cerr << "Extreme shift has no tracked output pitch\n"; return 1; }
        std::nth_element(stable.begin(), stable.begin() + stable.size() / 2, stable.end());
        const double error = 1200.0 * std::log2(stable[stable.size() / 2] / 220.0);
        if (std::abs(error) > 35.0) {
            std::cerr << "Extreme shift missed A3 by " << error << " cents\n"; return 1;
        }
    }
    // A causal low-pass in the wet/dry crossover used to leave a second,
    // original-pitch voice at -13.5 dB even when correction was fully engaged.
    std::vector<float> leakageSource(static_cast<size_t>(2 * sampleRate));
    for (int i = 0; i < 2 * sampleRate; ++i) {
        const double phase = 2.0 * pi * 340.0 * i / sampleRate;
        leakageSource[static_cast<size_t>(i)] = static_cast<float>(
            0.18 * std::sin(phase) + 0.08 * std::sin(2 * phase) + 0.04 * std::sin(3 * phase));
    }
    cody_pitch::Settings leakageSettings;
    leakageSettings.allowedNotes[7] = true; // G4, 392 Hz
    leakageSettings.minHz = 300.0f;
    leakageSettings.maxHz = 500.0f;
    leakageSettings.amount = 1.0f;
    leakageSettings.retuneMs = 0.0f;
    leakageSettings.transitionMs = 0.0f;
    leakageSettings.toleranceCents = 0.0f;
    leakageSettings.expression = 0.0f;
    const auto leakageOutput = cody_pitch::render(leakageSource, sampleRate, leakageSettings);
    const auto lineMagnitude = [&](double frequency) {
        double real = 0.0, imaginary = 0.0;
        for (int i = sampleRate / 2; i < sampleRate * 3 / 2; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * pi * (i - sampleRate / 2) / sampleRate);
            const double angle = 2.0 * pi * frequency * (i - sampleRate / 2) / sampleRate;
            const double value = leakageOutput.audio[static_cast<size_t>(i)] * window;
            real += value * std::cos(angle);
            imaginary -= value * std::sin(angle);
        }
        return std::hypot(real, imaginary);
    };
    const double originalLine = lineMagnitude(340.0);
    const double correctedLine = lineMagnitude(392.0);
    if (correctedLine < 100.0 || originalLine / correctedLine > 0.02) {
        std::cerr << "Original-pitch leakage remains under fully engaged correction: "
                  << originalLine / correctedLine << '\n'; return 1;
    }
    leakageSettings.allowedNotes.fill(false);
    leakageSettings.allowedNotes[9] = true; // A4 is closer than A3 or A2.
    leakageSettings.minHz = 70.0f;
    leakageSettings.maxHz = 900.0f;
    const auto restrictedNotes = cody_pitch::render(leakageSource, sampleRate, leakageSettings);
    if (std::abs(restrictedNotes.frames[200].targetHz - 440.0f) > 1.0f) {
        std::cerr << "Far-note cost chose an octave away from the closest allowed note\n"; return 1;
    }

    // Overlapping worker windows must not revise a target at the same absolute
    // instant just because the later window contains 40 ms more future audio.
    // A slow A4 -> Bb4 glide crosses the ambiguous scale midpoint here.
    std::vector<float> glide(static_cast<size_t>(sampleRate));
    double phase = 0.0;
    for (int i = 0; i < sampleRate; ++i) {
        const double time = i / static_cast<double>(sampleRate);
        const double cents = 110.0 * std::clamp((time - 0.20) / 0.20, 0.0, 1.0);
        phase += 2.0 * pi * (440.0 * std::exp2(cents / 1200.0)) / sampleRate;
        glide[static_cast<size_t>(i)] = static_cast<float>(0.16 * std::sin(phase)
            + 0.06 * std::sin(2.0 * phase) + 0.03 * std::sin(3.0 * phase));
    }
    cody_pitch::Settings glideSettings;
    glideSettings.allowedNotes = cody_pitch::scaleMask(2, 1); // D minor includes A/Bb
    glideSettings.minHz = 130.0f;
    glideSettings.maxHz = 700.0f;
    glideSettings.retuneMs = 40.0f;
    glideSettings.toleranceCents = 30.0f;
    std::vector<float> first(glide.begin(), glide.begin() + 21120);
    std::vector<float> second(glide.begin() + 1920, glide.begin() + 23040);
    const auto planA = cody_pitch::render(first, sampleRate, glideSettings, nullptr, false);
    const auto planB = cody_pitch::render(second, sampleRate, glideSettings, nullptr, false);
    int compared = 0;
    for (int i = 40; i < 48; ++i) {
        const auto& earlier = planA.frames[static_cast<size_t>(i + 8)];
        const auto& later = planB.frames[static_cast<size_t>(i)];
        if (earlier.sourceHz <= 0.0f || later.sourceHz <= 0.0f) continue;
        ++compared;
        if (std::abs(earlier.targetHz - later.targetHz) > 0.01f
            || std::abs(1200.0 * std::log2(earlier.desiredHz / later.desiredHz)) > 25.0) {
            std::cerr << "Natural target changed between overlapping windows\n";
            return 1;
        }
    }
    if (compared < 4) { std::cerr << "Target seam test did not track glide\n"; return 1; }
    std::cout << "PASS: scale, corrected pitch, dry tail, remove, note bypass, zero-target safety\n";
    return 0;
}
