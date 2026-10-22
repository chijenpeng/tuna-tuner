#pragma once

#include "PitchEngine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cody_pitch {

// Worker-thread-only continuous source/filter resynthesis. The WORLD pulse
// train lives across analysis hops; no WORLD call runs on the audio callback.
class WorldRenderer {
public:
    WorldRenderer();
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    void prepare(int sampleRate, int contextSamples, int hopSamples,
                 size_t ringCapacity, int64_t origin = 0);
    void processWindow(const std::vector<float>& window,
                       const std::vector<Frame>& frames, int64_t next);
    int64_t readyUntil() const noexcept;
    float corrected(int64_t index, float dry) const noexcept;
    // Worker-only access for deciding when WORLD produced a non-harmonic burst.
    float synthesized(int64_t index) const noexcept;
    float sourceHz(int64_t index) const noexcept;
    float desiredHz(int64_t index) const noexcept;

    // Per-5-ms-frame diagnostics for TUNA_FRAMES_CSV (worker only).
    struct FrameDiag {
        bool valid = false;
        float sourceHz = 0, targetHz = 0, desiredHz = 0;
        bool voiced = false, eligible = false;
        float apLow = 0, apHigh = 0;
        float alignLag = 0; // TUNA_SWITCH_ALIGN: chosen crossfade-centre shift in samples
    };
    FrameDiag frameDiag(int64_t frameIndex) const noexcept;
    float blendAt(int64_t index) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cody_pitch
