#pragma once

// Glottal-epoch tracker for the waveform renderers. It combines a zero-frequency filter
// with LPC-residual refinement and keeps all analysis in double precision.
//
// Worker-thread only. prepare() sizes every buffer; analyse() does not allocate for windows
// up to the prepared size.

#include <cstdint>
#include <vector>

namespace cody_pitch {

class EpochTracker {
public:
    void prepare(int sampleRate, int maxWindowSamples);

    // x: window samples. frameTimeS[i]: centre of frame i in seconds relative to x[0].
    // srcHz[i] > 0 marks a valid (voiced) frame, 0 an invalid one.
    void analyse(const float* x, int n, const double* frameTimeS, const double* srcHz, int frameCount);

    int size() const noexcept { return static_cast<int>(pos_.size()); }
    double position(int i) const noexcept { return pos_[static_cast<size_t>(i)]; }
    bool reliable(int i) const noexcept { return reliable_[static_cast<size_t>(i)] != 0; }
    bool fallback(int i) const noexcept { return fallback_[static_cast<size_t>(i)] != 0; }
    int run(int i) const noexcept { return run_[static_cast<size_t>(i)]; }
    // Distance to the next epoch in the window (the last one repeats the previous distance;
    // 0 when there is a single epoch). Crosses run boundaries, as the reference does.
    double period(int i) const noexcept;

    struct Stats { int zff = 0, droppedClose = 0, inserted = 0, irregular = 0; };
    const Stats& stats() const noexcept { return stats_; }

private:
    void perSamplePeriod(int n, const double* frameTimeS, const double* srcHz, int frameCount);
    void zffCrossings(const float* x, int n);
    void lpcResidual(int n);
    void lpcCoefficients(const double* frame, int length, double* a) const;

    int fs_ = 48000;
    int decim_ = 4;
    int maxN_ = 0;
    std::vector<double> kernel_;      // anti-alias FIR for the decimator
    std::vector<double> lagWindow_;   // Gaussian lag window for the LPC autocorrelation
    std::vector<double> smooth_;      // normalised Hann smoothing kernel for the residual
    std::vector<double> hann1200_;    // symmetric Hann for full LPC frames
    std::vector<double> xd_, y_, cs_;
    std::vector<int> half_;
    std::vector<double> x64_, pre_, res_, sres_, per_, wframe_;
    std::vector<uint8_t> inVoiced_;
    std::vector<int> frameOf_;
    std::vector<int> runOfFrame_;
    std::vector<double> crossings_;
    std::vector<double> runEpochs_;
    std::vector<double> outPos_;
    std::vector<uint8_t> outRel_, outFb_;
    std::vector<double> pos_;
    std::vector<uint8_t> reliable_, fallback_;
    std::vector<int> run_;
    Stats stats_;
};

} // namespace cody_pitch
