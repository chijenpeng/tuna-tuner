#pragma once

#include "RealtimeEngine.h"
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

class CodyPitchProcessor final : public juce::AudioProcessor {
public:
    CodyPitchProcessor();
    ~CodyPitchProcessor() override;

    void prepareToPlay(double sampleRate, int) override;
    void releaseResources() override { realtime.stop(); }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.3; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    void setAudition(bool enabled) noexcept { audition.store(enabled); }
    bool isAuditioning() const noexcept { return audition.load(); }
    float detectedHz() const noexcept { return realtime.detectedHz(); }
    float targetHz() const noexcept { return realtime.targetHz(); }
    int underrunCount() const noexcept { return realtime.underrunCount(); }
    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    cody_pitch::Settings readSettings() const;

    cody_pitch::RealtimeEngine realtime;
    std::atomic<bool> audition { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CodyPitchProcessor)
};
