#pragma once

#include "PitchEngine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cody_pitch {

// Worker-thread-only waveform-domain pitch renderer using ZFF glottal epochs and voiced islands.
// Splice uses continuous resampling; PSOLA uses epoch-synchronous overlap-add. Both share the
// WorldRenderer hop interface:
// one processWindow() per hop, window = [next - context, next + hop + context).
class WaveformRenderer {
public:
    enum class Mode { Splice, Psola };

    WaveformRenderer();
    ~WaveformRenderer();
    WaveformRenderer(const WaveformRenderer&) = delete;
    WaveformRenderer& operator=(const WaveformRenderer&) = delete;

    // Options take effect at the next prepare().
    void setMode(Mode mode);
    void setSpliceTrim(bool on);    // exit rate trim (matches the golden vectors when on)
    // Optional envelope correction after splice. 0 off, 1 full band, 2 below 5 kHz.
    // Delays readyUntil() by about 14 ms.
    void setSpliceFormant(int mode);
    void setFormantTuning(bool rmsNormalise, double hopMs); // Configure per-frame gain matching and overlap-add hop.
    // Configure cycle-held Splice ratios, PSOLA grain reuse across target steps, and
    // smoothed window-sum normalisation.
    void setSplicePerCycle(bool on);
    void setPsolaOptions(bool stepReuse, bool smoothNorm);
    // Pure Splice mode preserves voiced islands until drift exceeds maxDriftMs, when it uses a
    // forced two-period seam. peakHint > 0 fixes the level floor (tests); otherwise a running peak.
    void setSplicePure(bool on, double maxDriftMs = 80.0);
    void setPeakHint(double peak);
    void setPruneKeep(size_t epochs); // test hook: epochs kept per island (default 4096)
    void setStreamEnd(int64_t totalSamples); // offline/test only: clip like a finite file (-1 = endless)
    void enableEpochLog(bool on);

    void prepare(int sampleRate, int contextSamples, int hopSamples,
                 size_t ringCapacity, int64_t origin = 0);
    void processWindow(const std::vector<float>& window,
                       const std::vector<Frame>& frames, int64_t next);
    // Full-precision entry used by the equivalence test: srcHz is 0 for invalid frames and desHz
    // is already 0 where the source is invalid.
    void processWindowHz(const std::vector<float>& window, const double* srcHz, const double* desHz,
                         int frameCount, int64_t next);
    // Dry signal at the position the renderer is reading (pure2 shifts time by up to the drift); the plain dry otherwise.
    // The hybrid low-pass must use it for both terms: aligned + LP(rendered - aligned).
    float alignedDry(int64_t index, float dry) const noexcept;
    double readPosition(int64_t index) const noexcept; // source sample read for output sample index (measurement)
    int64_t readyUntil() const noexcept;
    float corrected(int64_t index, float dry) const noexcept;

    struct EpochRow { int hop; double position; double period; int reliable; int island; };
    struct Stats {
        int islands = 0, mergeNewIsland = 0, droppedClose = 0, orphanRestart = 0, inserted = 0;
        int underruns = 0, trimLate = 0, jumps = 0, hops = 0, prunes = 0;
        int pureSilence = 0, pureWsola = 0, pureForced = 0, pureLateExit = 0;
        double maxOffsetSamples = 0.0; // largest |read position - output position| seen by the pure splice
        int64_t maxIslandEpochs = 0; // longest island seen, in epochs
        double hopMsMax = 0.0, hopMsMean = 0.0, hopMsP99 = 0.0;
    };
    // Per 5 ms frame (centre sample inside a finalised block), for TUNA_FRAMES_CSV.
    struct FrameDiag {
        bool valid = false;
        float sourceHz = 0, desiredHz = 0;
        int island = -1;      // island id whose output covers the frame centre, -1 = dry
        bool active = false;  // renderer output (mask > 0) at the centre
        bool jump = false;    // a splice drop/repeat crossfade overlaps the frame
    };
    FrameDiag frameDiag(int64_t frame) const noexcept;
    Stats stats() const;
    const std::vector<EpochRow>& epochLog() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Mode mode_ = Mode::Splice;
    bool trim_ = false, log_ = false;
    size_t pruneKeep_ = 4096;
    int formant_ = 0;
    bool pure_ = false; double maxDriftMs_ = 80.0, peakHint_ = -1.0;
    bool fNorm_ = true; double fHopMs_ = 5.0;
    bool perCycle_ = false, stepReuse_ = false, smoothNorm_ = false;
    int64_t streamEnd_ = -1;
};

} // namespace cody_pitch
