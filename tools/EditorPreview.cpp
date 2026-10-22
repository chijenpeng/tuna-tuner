#include "PluginProcessor.h"

#include <juce_graphics/juce_graphics.h>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const bool demo = argc == 3 && juce::String(argv[2]) == "--demo";
    if (argc == 3 && !demo) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    CodyPitchProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    if (demo) {
        processor.setNonRealtime(true);
        constexpr int rate = 48000, block = 256;
        constexpr double pi = 3.14159265358979323846;
        const double hz = 440.0 * std::exp2(35.0 / 1200.0);
        juce::AudioBuffer<float> audio(1, block);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < rate * 2; offset += block) {
            for (int i = 0; i < block; ++i) {
                const double phase = 2.0 * pi * hz * (offset + i) / rate;
                audio.setSample(0, i, static_cast<float>(0.2 * std::sin(phase) + 0.05 * std::sin(2.0 * phase)));
            }
            processor.processBlock(audio, midi);
        }
        for (int i = 0; i < 10; ++i) {
            juce::Thread::sleep(110);
            juce::Timer::callPendingTimersSynchronously();
        }
    }
    auto image = editor->createComponentSnapshot(editor->getLocalBounds());
    juce::MemoryOutputStream output;
    if (!juce::PNGImageFormat().writeImageToStream(image, output)) return 1;
    if (!juce::File { argv[1] }.replaceWithData(output.getData(), output.getDataSize())) return 1;
    processor.releaseResources();
    return 0;
}
