#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

CodyPitchProcessor::CodyPitchProcessor()
    : juce::AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::mono(), true)
                                           .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      parameters(*this, nullptr, "CodyPitchState", createLayout()) {
}

CodyPitchProcessor::~CodyPitchProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout CodyPitchProcessor::createLayout() {
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> values;
    const auto addFloat = [&](const char* id, const char* name, float low, float high, float initial) {
        values.push_back(std::make_unique<AudioParameterFloat>(ParameterID(id, 1), name,
            NormalisableRange<float>(low, high), initial));
    };
    values.push_back(std::make_unique<AudioParameterChoice>(ParameterID("root", 1), "Root",
        StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 2));
    values.push_back(std::make_unique<AudioParameterChoice>(ParameterID("scale", 1), "Scale",
        StringArray { "Major", "Natural Minor", "Harmonic Minor", "Chromatic", "Custom",
                      "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Blues",
                      "Major Pentatonic", "Minor Pentatonic" }, 1));
    values.push_back(std::make_unique<AudioParameterChoice>(ParameterID("range", 1), "Vocal Range",
        StringArray { "Bass", "Baritone", "Alto/Tenor", "Soprano", "Custom" }, 2));
    addFloat("amount", "Amount", 0, 100, 85);
    addFloat("retune", "Retune ms", 0, 250, 45);
    addFloat("tolerance", "Tolerance cents", 0, 100, 20);
    addFloat("expression", "Expression", 0, 100, 90);
    addFloat("sustain", "Sustain Relax", 0, 100, 30);
    addFloat("transition", "Transition ms", 0, 150, 55);
    addFloat("minHz", "Voice Low Hz", 50, 300, 240);
    addFloat("maxHz", "Voice High Hz", 250, 1200, 700);
    addFloat("reference", "A4 Tuning Hz", 430, 450, 440);
    values.push_back(std::make_unique<AudioParameterInt>(ParameterID("transpose", 1), "Transpose semitones", -12, 12, 0));
    addFloat("vibrato", "Vibrato amount", 0, 200, 100);
    for (int note = 0; note < 12; ++note)
        values.push_back(std::make_unique<AudioParameterBool>(ParameterID("note" + String(note), 1),
            "Custom note " + String(note), true));
    for (int note = 0; note < 12; ++note)
        values.push_back(std::make_unique<AudioParameterChoice>(ParameterID("noteMode" + String(note), 1),
            "Note " + String(note) + " mode", StringArray { "Follow Scale", "On", "Remove", "Bypass" }, 0));
    // Appended after every earlier parameter (index 38); sessions saved without it load as Splice.
    values.push_back(std::make_unique<AudioParameterChoice>(ParameterID("renderer", 1), "Renderer",
        StringArray { "Splice", "PSOLA" }, 0));
    return { values.begin(), values.end() };
}

void CodyPitchProcessor::prepareToPlay(double sampleRate, int) {
    realtime.prepare(static_cast<int>(std::lround(sampleRate)), [this] { return readSettings(); });
    setLatencySamples(realtime.latencySamples());
}

bool CodyPitchProcessor::isBusesLayoutSupported(const BusesLayout& layout) const {
    const auto input = layout.getMainInputChannelSet();
    return input == layout.getMainOutputChannelSet()
        && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void CodyPitchProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    if (buffer.getNumChannels() == 1)
        realtime.process(buffer.getWritePointer(0), buffer.getNumSamples(), audition.load(), isNonRealtime());
    else if (buffer.getNumChannels() == 2)
        realtime.processStereo(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples(), audition.load(), isNonRealtime());
}

cody_pitch::Settings CodyPitchProcessor::readSettings() const {
    const auto value = [this](const char* id) { return parameters.getRawParameterValue(id)->load(); };
    cody_pitch::Settings settings;
    const int root = static_cast<int>(value("root"));
    const int scale = static_cast<int>(value("scale"));
    settings.allowedNotes = cody_pitch::scaleMask(root, scale);
    if (scale == 4)
        for (int note = 0; note < 12; ++note)
            settings.allowedNotes[static_cast<size_t>(note)] = parameters.getRawParameterValue("note" + juce::String(note))->load() > 0.5f;
    for (int note = 0; note < 12; ++note) {
        const int mode = static_cast<int>(parameters.getRawParameterValue("noteMode" + juce::String(note))->load());
        if (mode == 1) settings.allowedNotes[static_cast<size_t>(note)] = true;
        if (mode == 2 || mode == 3) settings.allowedNotes[static_cast<size_t>(note)] = false;
        if (mode == 3) settings.bypassNotes[static_cast<size_t>(note)] = true;
    }
    // Amount, Expression and Transition remain in saved state for compatibility but are fixed internally
    // at 100 %, 0 %, and 0 ms and are not passed to the engine.
    settings.amount = 1.0f;
    settings.retuneMs = value("retune");
    settings.toleranceCents = value("tolerance");
    settings.expression = 0.0f;
    settings.sustainRelax = value("sustain") / 100.0f;
    settings.transitionMs = 0.0f;
    settings.minHz = value("minHz");
    settings.maxHz = value("maxHz");
    switch (static_cast<int>(value("range"))) {
        case 0: settings.minHz = 55; settings.maxHz = 500; break;
        case 1: settings.minHz = 70; settings.maxHz = 700; break;
        case 2: settings.minHz = 130; settings.maxHz = 700; break;
        case 3: settings.minHz = 140; settings.maxHz = 1200; break;
        default: break;
    }
    // Legacy Custom sessions may contain equal or inverted bounds. Keep their state loadable
    // without sending an invalid detector range into the background renderer.
    if (settings.maxHz <= settings.minHz) {
        settings.minHz = 130;
        settings.maxHz = 700;
    }
    settings.referenceHz = value("reference");
    settings.transposeSemitones = value("transpose");
    settings.vibratoGain = value("vibrato") / 100.0f;
    settings.renderer = static_cast<int>(value("renderer"));
    return settings;
}

void CodyPitchProcessor::getStateInformation(juce::MemoryBlock& data) {
    auto state = parameters.copyState();
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, data);
}

void CodyPitchProcessor::setStateInformation(const void* data, int bytes) {
    if (auto xml = getXmlFromBinary(data, bytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CodyPitchProcessor(); }
