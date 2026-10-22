// TUNA_VOICING_BRIDGE_MODE=1 (evidence fill) at the PitchEngine level: a continuous vowel keeps no unvoiced
#include "TestEnvironment.h"
// frames inside it, and a genuinely silent 60 ms gap stays unvoiced.
#include "PitchEngine.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

int main() {
    setTestEnvironment("TUNA_VOICING_BRIDGE_MS", "100");
    setTestEnvironment("TUNA_VOICING_BRIDGE_MODE", "1");
    constexpr int rate = 48000;
    constexpr double pi = 3.14159265358979323846;
    cody_pitch::Settings settings;
    settings.allowedNotes = cody_pitch::scaleMask(9, 1);
    settings.amount = 1.0f; settings.retuneMs = 0.0f; settings.transitionMs = 0.0f;
    settings.toleranceCents = 0.0f; settings.expression = 0.0f;
    settings.minHz = 130.0f; settings.maxHz = 700.0f;
    const auto vowel = [&](std::vector<float>& x, int from, int to, double hz, double& phase) {
        for (int i = from; i < to; ++i) {
            phase += 2 * pi * hz * (1.0 + 0.004 * std::sin(2 * pi * 5.0 * i / rate)) / rate;
            x[static_cast<size_t>(i)] = static_cast<float>(0.12 * std::sin(phase) + 0.07 * std::sin(2 * phase) + 0.04 * std::sin(3 * phase));
        }
    };
    int failures = 0;
    // 1. continuous 1.2 s vowel
    {
        std::vector<float> x(static_cast<size_t>(1.2 * rate));
        double phase = 0.0;
        vowel(x, 0, static_cast<int>(x.size()), 205.0, phase);
        const auto r = cody_pitch::render(x, rate, settings, nullptr, false);
        int first = -1, last = -1, holes = 0;
        for (size_t i = 0; i < r.frames.size(); ++i)
            if (r.frames[i].voicedWeight > 0.0f) { if (first < 0) first = static_cast<int>(i); last = static_cast<int>(i); }
        for (int i = first; i >= 0 && i <= last; ++i) holes += r.frames[static_cast<size_t>(i)].voicedWeight > 0.0f ? 0 : 1;
        std::cout << "continuous vowel: voiced frames " << first << ".." << last << ", unvoiced inside: " << holes << '\n';
        if (first < 0 || holes != 0) { std::cerr << "FAIL: unvoiced frames inside a continuous vowel\n"; ++failures; }
    }
    // 2. vowel, 60 ms digital silence, vowel
    {
        std::vector<float> x(static_cast<size_t>(1.6 * rate), 0.0f);
        double phase = 0.0;
        const int gapA = static_cast<int>(0.7 * rate), gapB = gapA + static_cast<int>(0.06 * rate);
        vowel(x, 0, gapA, 205.0, phase);
        vowel(x, gapB, static_cast<int>(x.size()), 205.0, phase);
        const auto r = cody_pitch::render(x, rate, settings, nullptr, false);
        int voicedInside = 0, total = 0;
        for (size_t i = 0; i < r.frames.size(); ++i) {
            const double t = i * 0.005;
            if (t >= 0.7 + 0.01 && t <= 0.76 - 0.01) { ++total; voicedInside += r.frames[i].voicedWeight > 0.0f; }
        }
        std::cout << "60 ms silent gap: voiced frames inside " << voicedInside << " of " << total << '\n';
        if (voicedInside != 0) { std::cerr << "FAIL: a silent gap was bridged\n"; ++failures; }
    }
    std::cout << (failures ? "voicing bridge test FAILED\n" : "voicing bridge test passed\n");
    return failures ? 1 : 0;
}
