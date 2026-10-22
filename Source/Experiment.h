#pragma once

// Optional diagnostic and renderer overrides read once from environment variables. Every default
// reproduces the shipped behaviour exactly. Nothing here is read per sample or on the
// audio callback; flags() is first touched from RealtimeEngine::prepare.

#include <algorithm>
#include <cstdlib>
#include <string>

namespace cody_pitch {

struct ExperimentFlags {
    double d4cThreshold = -1.0;      // TUNA_D4C_THRESHOLD; negative = leave D4COption untouched
    double apCap = 1.0;              // TUNA_AP_CAP; >= 1 = off
    int switchMode = 0;              // TUNA_SWITCH_MODE: 0 hysteresis, 1 eligible-voiced, 2 custom hysteresis
    double switchOnCents = 8.0;      // TUNA_SWITCH_ON_CENTS (mode 2)
    double switchOffCents = 3.0;     // TUNA_SWITCH_OFF_CENTS (mode 2)
    double switchFadeMs = 10.0;      // TUNA_SWITCH_FADE_MS
    double wetGain = 0.84;           // TUNA_WET_GAIN
    int qualityGate = 1;             // TUNA_QUALITY_GATE
    double voicingBridgeMs = 60.0;   // TUNA_VOICING_BRIDGE_MS (0 = off)
    int voicingBridgeMode = 1;       // TUNA_VOICING_BRIDGE_MODE: 0 unconditional fill, 1 evidence fill (energy + periodicity)
    double periodicityGate = 0.55;   // TUNA_PERIODICITY_GATE
    int targetMode = 0;              // TUNA_TARGET_MODE: 0 shipped, 1 nearest+mode filter, 2 hysteresis, 3 Viterbi always
    double targetHystCents = 15.0;   // TUNA_TARGET_HYST_CENTS (mode 2)
    int gateSkipSteps = 0;           // TUNA_GATE_SKIP_STEPS: skip the gate where one period is not a valid assumption
    int switchAlign = 0;             // TUNA_SWITCH_ALIGN: align island-edge crossfade centres (switch mode 1)
    std::string framesCsv;           // TUNA_FRAMES_CSV
    std::string renderer = "splice"; // TUNA_RENDERER: world | splice | psola (waveform renderers, no gate/hysteresis); overrides the plug-in parameter when set
    bool rendererSet = false;
    int spliceTrim = 0;              // TUNA_SPLICE_TRIM: 1 = exit rate trim
    int spliceFormant = 0;           // TUNA_SPLICE_FORMANT: 0 off, 1 envelope correction full band, 2 below 5 kHz
    int spliceRatioPerCycle = 0;     // TUNA_SPLICE_RATIO_PER_CYCLE
    int psolaStepReuse = 0;          // TUNA_PSOLA_STEP_REUSE
    int psolaSmoothNorm = 0;         // TUNA_PSOLA_SMOOTH_NORM
    int spliceFormantNorm = 1;       // TUNA_SPLICE_FORMANT_NORM: 0 disables the per-frame RMS normalisation
    double spliceFormantHopMs = 5.0; // TUNA_SPLICE_FORMANT_HOP_MS
    int splicePure = 1;              // TUNA_SPLICE_PURE: splice_pure2 (no seams in islands, drift absorbed in gaps)
    double splicePureMaxDriftMs = 80.0; // TUNA_SPLICE_PURE_MAX_DRIFT_MS: forced two-period seam beyond this drift
    int latencyMode = 0;             // TUNA_LATENCY_MODE: 0 High Quality, 1 Low, 2 Lowest, 3 Live; overrides the plug-in parameter when set
    bool latencyModeSet = false;
    double waveLpHz = 8000.0;        // TUNA_WAVE_LP_HZ: low-pass on the waveform renderers' correction; 0 = full band
    double velocityGateCpms = 12.0;  // TUNA_VELOCITY_GATE_CPMS: source pitch speed (cents/ms) above which correction is suspended; 0 = off
    double velocityGateReleaseMs = 15.0; // TUNA_VELOCITY_GATE_RELEASE_MS: time to restore full correction after fast motion ends
    int velocityGateMode = 1;        // TUNA_VELOCITY_GATE_MODE: 0 = raw 10 ms speed, 1 = robust (median, 20 ms span, excursion + persistence)
    double velocityGateMinCents = 60.0; // TUNA_VELOCITY_GATE_MIN_CENTS: mode 1 minimum excursion over the 20 ms span
    double targetCenterMs = 50.0;    // TUNA_TARGET_CENTER_MS: median window used only for target-note decoding; 0 = the 150 ms centre

    static ExperimentFlags load() {
        ExperimentFlags f;
        const auto number = [](const char* name, double fallback) {
            const char* text = std::getenv(name);
            if (!text || !*text) return fallback;
            char* end = nullptr;
            const double value = std::strtod(text, &end);
            return end == text ? fallback : value;
        };
        f.d4cThreshold = number("TUNA_D4C_THRESHOLD", f.d4cThreshold);
        f.apCap = number("TUNA_AP_CAP", f.apCap);
        f.switchMode = static_cast<int>(number("TUNA_SWITCH_MODE", f.switchMode));
        f.switchOnCents = number("TUNA_SWITCH_ON_CENTS", f.switchOnCents);
        f.switchOffCents = number("TUNA_SWITCH_OFF_CENTS", f.switchOffCents);
        f.switchFadeMs = number("TUNA_SWITCH_FADE_MS", f.switchFadeMs);
        f.wetGain = number("TUNA_WET_GAIN", f.wetGain);
        f.qualityGate = static_cast<int>(number("TUNA_QUALITY_GATE", f.qualityGate));
        f.voicingBridgeMs = number("TUNA_VOICING_BRIDGE_MS", f.voicingBridgeMs);
        f.voicingBridgeMode = static_cast<int>(number("TUNA_VOICING_BRIDGE_MODE", f.voicingBridgeMode));
        f.periodicityGate = number("TUNA_PERIODICITY_GATE", f.periodicityGate);
        f.targetMode = static_cast<int>(number("TUNA_TARGET_MODE", f.targetMode));
        f.targetHystCents = number("TUNA_TARGET_HYST_CENTS", f.targetHystCents);
        f.gateSkipSteps = static_cast<int>(number("TUNA_GATE_SKIP_STEPS", f.gateSkipSteps));
        f.switchAlign = static_cast<int>(number("TUNA_SWITCH_ALIGN", f.switchAlign));
        if (const char* path = std::getenv("TUNA_FRAMES_CSV"))
            if (*path) f.framesCsv = path;
        if (const char* name = std::getenv("TUNA_RENDERER"))
            if (std::string(name) == "splice" || std::string(name) == "psola" || std::string(name) == "world") { f.renderer = name; f.rendererSet = true; }
        f.spliceTrim = static_cast<int>(number("TUNA_SPLICE_TRIM", f.spliceTrim));
        f.spliceFormant = static_cast<int>(number("TUNA_SPLICE_FORMANT", f.spliceFormant));
        f.spliceRatioPerCycle = static_cast<int>(number("TUNA_SPLICE_RATIO_PER_CYCLE", f.spliceRatioPerCycle));
        f.psolaStepReuse = static_cast<int>(number("TUNA_PSOLA_STEP_REUSE", f.psolaStepReuse));
        f.psolaSmoothNorm = static_cast<int>(number("TUNA_PSOLA_SMOOTH_NORM", f.psolaSmoothNorm));
        f.spliceFormantNorm = static_cast<int>(number("TUNA_SPLICE_FORMANT_NORM", f.spliceFormantNorm));
        f.spliceFormantHopMs = number("TUNA_SPLICE_FORMANT_HOP_MS", f.spliceFormantHopMs);
        f.splicePure = static_cast<int>(number("TUNA_SPLICE_PURE", f.splicePure));
        f.splicePureMaxDriftMs = number("TUNA_SPLICE_PURE_MAX_DRIFT_MS", f.splicePureMaxDriftMs);
        if (const char* text = std::getenv("TUNA_LATENCY_MODE"))
            if (*text) { f.latencyMode = std::clamp(std::atoi(text), 0, 3); f.latencyModeSet = true; }
        f.waveLpHz = number("TUNA_WAVE_LP_HZ", f.waveLpHz);
        f.velocityGateCpms = number("TUNA_VELOCITY_GATE_CPMS", f.velocityGateCpms);
        f.velocityGateReleaseMs = number("TUNA_VELOCITY_GATE_RELEASE_MS", f.velocityGateReleaseMs);
        f.velocityGateMode = static_cast<int>(number("TUNA_VELOCITY_GATE_MODE", f.velocityGateMode));
        f.velocityGateMinCents = number("TUNA_VELOCITY_GATE_MIN_CENTS", f.velocityGateMinCents);
        f.targetCenterMs = number("TUNA_TARGET_CENTER_MS", f.targetCenterMs);
        return f;
    }
};

inline const ExperimentFlags& flags() {
    static const ExperimentFlags loaded = ExperimentFlags::load();
    return loaded;
}

} // namespace cody_pitch
