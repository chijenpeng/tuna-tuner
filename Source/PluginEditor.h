#pragma once

#include "PluginProcessor.h"

#include <functional>
#include <memory>
#include <vector>

class CodyPitchEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit CodyPitchEditor(CodyPitchProcessor&);
    ~CodyPitchEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class DarkLook final : public juce::LookAndFeel_V4 {
    public:
        void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float,
                              juce::Slider&) override;
        void drawPopupMenuBackground(juce::Graphics&, int, int) override;
        void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>&, bool, bool, bool,
                               bool, bool, const juce::String&, const juce::String&,
                               const juce::Drawable*, const juce::Colour*) override;
        juce::Font getPopupMenuFont() override;
    } look;

    class MenuChoice final : public juce::Component {
    public:
        MenuChoice(juce::RangedAudioParameter&, juce::String label, juce::StringArray options,
                   std::vector<int> values, juce::String help);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseEnter(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        int selectedValue() const noexcept { return selected; }
    private:
        juce::String label;
        juce::StringArray options;
        std::vector<int> values;
        juce::ParameterAttachment attachment;
        int selected = 0;
        bool hovered = false;
    };

    class Knob final : public juce::Component {
    public:
        Knob(juce::AudioProcessorValueTreeState&, const char* parameterId,
             juce::String title, juce::String suffix, juce::String help, int diameter);
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        juce::Slider slider;

    private:
        void finishEditing(bool apply);
        juce::String title, suffix;
        int diameter;
        juce::TextEditor editor;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    class ChoiceRow final : public juce::Component {
    public:
        ChoiceRow(juce::RangedAudioParameter&, juce::String label, juce::StringArray options, juce::String help);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
    private:
        int optionAt(int x) const noexcept;
        int optionX(int index) const noexcept;
        int optionWidth(int index) const noexcept;
        juce::String label;
        juce::StringArray options;
        juce::ParameterAttachment attachment;
        int selected = 0, hovered = -1;
    };

    class NoteRow final : public juce::Component {
    public:
        explicit NoteRow(CodyPitchProcessor&);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
    private:
        CodyPitchProcessor& processor;
    };

    void addKnob(const char* id, const char* title, const juce::String& suffix,
                 const juce::String& help, int diameter = 64);
    void showHelpFor(juce::Component*);
    void timerCallback() override;

    CodyPitchProcessor& pitchProcessor;
    MenuChoice root, scale, range;
    ChoiceRow renderer;
    NoteRow noteRow;
    juce::Label status;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::Rectangle<int> helpArea;
    juce::String helpTitle, helpText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CodyPitchEditor)
};
