#include "PluginProcessor.h"

#include <cmath>
#include <iostream>
#include <vector>

// Freeze the development source's public parameter/state contract independently
// of the release AU/VST3 identity. Do not obtain expected values from createLayout.
int main() {
    juce::ScopedJuceInitialiser_GUI initialise;
    CodyPitchProcessor source;
    std::vector<juce::String> ids { "root", "scale", "range", "amount", "retune", "tolerance",
        "expression", "sustain", "transition", "minHz", "maxHz", "reference", "transpose", "vibrato" };
    for (int i = 0; i < 12; ++i) ids.push_back("note" + juce::String(i));
    for (int i = 0; i < 12; ++i) ids.push_back("noteMode" + juce::String(i));
    ids.push_back("renderer");
    const auto& parameters = source.getParameters();
    if (parameters.size() != static_cast<int>(ids.size())) return 1;
    const float low[] { 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 250, 430, -12, 0 };
    const float high[] { 11, 12, 4, 100, 250, 100, 100, 100, 150, 300, 1200, 450, 12, 200 };
    const float initial[] { 2, 1, 2, 85, 45, 20, 90, 30, 55, 240, 700, 440, 0, 100 };
    for (size_t i = 0; i < ids.size(); ++i) {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters[static_cast<int>(i)]);
        if (!parameter || parameter->getParameterID() != ids[i]) return 1;
        const auto range = parameter->getNormalisableRange();
        const float expectedLow = i < 14 ? low[i] : 0;
        const float expectedHigh = i < 14 ? high[i] : i < 26 ? 1 : i < 38 ? 3 : 1;
        const float expectedDefault = i < 14 ? initial[i] : i < 26 ? 1 : 0;
        if (!juce::exactlyEqual(range.start, expectedLow) || !juce::exactlyEqual(range.end, expectedHigh)
            || std::abs(parameter->convertFrom0to1(parameter->getDefaultValue()) - expectedDefault) > 0.0001f)
            return 1;
        const auto* choices = dynamic_cast<juce::AudioParameterChoice*>(parameter);
        if (ids[i] == "scale" && (!choices || choices->choices != juce::StringArray {
                "Major", "Natural Minor", "Harmonic Minor", "Chromatic", "Custom", "Melodic Minor",
                "Dorian", "Phrygian", "Lydian", "Mixolydian", "Blues", "Major Pentatonic", "Minor Pentatonic" })) return 1;
        if (i >= 26 && i < 38 && (!choices || choices->choices != juce::StringArray {
                "Follow Scale", "On", "Remove", "Bypass" })) return 1;
        if (ids[i] == "renderer" && (!choices || choices->choices != juce::StringArray { "Splice", "PSOLA" })) return 1;
        parameter->setValueNotifyingHost(parameter->convertTo0to1(expectedLow + 0.73f * (expectedHigh - expectedLow)));
    }
    juce::MemoryBlock saved;
    source.getStateInformation(saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    if (!xml || !xml->hasTagName("CodyPitchState")) return 1;
    CodyPitchProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    for (const auto& id : ids)
        if (!juce::exactlyEqual(source.parameters.getRawParameterValue(id)->load(), restored.parameters.getRawParameterValue(id)->load())) return 1;

    // Pre-renderer development sessions contain the same root and first 38 IDs.
    // Loading one into a fresh release instance must retain those values and use
    // Splice for the appended renderer parameter.
    auto legacyState = juce::ValueTree::fromXml(*xml);
    for (int i = legacyState.getNumChildren(); --i >= 0;)
        if (legacyState.getChild(i).getProperty("id").toString() == "renderer") legacyState.removeChild(i, nullptr);
    auto legacyXml = legacyState.createXml();
    juce::MemoryBlock legacyBytes;
    juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacyBytes);
    CodyPitchProcessor legacy;
    legacy.setStateInformation(legacyBytes.getData(), static_cast<int>(legacyBytes.getSize()));
    for (size_t i = 0; i < ids.size() - 1; ++i)
        if (!juce::exactlyEqual(source.parameters.getRawParameterValue(ids[i])->load(), legacy.parameters.getRawParameterValue(ids[i])->load())) return 1;
    if (legacy.parameters.getRawParameterValue("renderer")->load() != 0.0f) return 1;
    const auto layout = [](int channels) {
        juce::AudioProcessor::BusesLayout buses;
        buses.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        buses.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        return buses;
    };
    if (!restored.isBusesLayoutSupported(layout(1)) || !restored.isBusesLayoutSupported(layout(2))
        || restored.isBusesLayoutSupported(layout(3))) return 1;
    std::cout << "PASS: 39 development parameter IDs/order/ranges/defaults, state round-trip, legacy renderer omission, mono/stereo buses\n";
}
