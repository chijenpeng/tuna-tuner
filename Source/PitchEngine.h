#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cody_pitch {

struct Settings {
    // C=0, C#=1, ... B=11. The mask is repeated in every octave.
    std::array<bool, 12> allowedNotes {};
    // Pitch classes that pass through untouched instead of being retargeted.
    std::array<bool, 12> bypassNotes {};
    float amount = 0.85f;
    float retuneMs = 45.0f;
    float toleranceCents = 20.0f;
    float expression = 0.9f;
    float sustainRelax = 0.3f;
    float transitionMs = 55.0f;
    float minHz = 70.0f;
    float maxHz = 900.0f;
    float referenceHz = 440.0f;
    float transposeSemitones = 0.0f;
    float vibratoGain = 1.0f;
    int latencyMode = 0;       // 0 High Quality, 1 Low, 2 Lowest, 3 Live (TUNA_LATENCY_MODE overrides)
    float contextMs = 200.0f; // right/left context of the analysis window; set by the engine, limits the planner's lookahead
    int renderer = 0; // 0 Splice, 1 PSOLA; WORLD is reachable only through TUNA_RENDERER=world
};

struct Frame {
    double timeSeconds = 0.0;
    float sourceHz = 0.0f;
    float targetHz = 0.0f;
    float desiredHz = 0.0f;
    float voicedWeight = 0.0f;
    bool eligible = false; // voiced and correction-eligible (not Bypass)
};

struct Result {
    std::vector<float> audio;
    std::vector<Frame> frames;
    int voicedFrames = 0;
    int correctedFrames = 0;
    double handoffCycles = 0.0;
    int sourceHandoffSample = -1;
    std::string warning;
};

struct RenderTiming {
    int anchorSample = 0;
    double anchorCycles = 0.0;
    int handoffSample = 0;
    int sourceAnchorSample = -1;
};

std::array<bool, 12> scaleMask(int root, int scaleIndex);
Result render(const std::vector<float>& mono, int sampleRate, const Settings& settings,
              const RenderTiming* timing = nullptr, bool synthesizeAudio = true);

} // namespace cody_pitch
