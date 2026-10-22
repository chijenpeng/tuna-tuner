#include "PluginEditor.h"

#include <BinaryData.h>

#include <algorithm>
#include <cmath>

namespace {
const juce::Colour background { 0xff0e0e0f };
const juce::Colour panel { 0xff1c1c1e };
const juce::Colour pale { 0xffcfeef8 };
const juce::Colour white { 0xfff7f8f9 };
const juce::Colour muted { 0xffa6a6a8 };
const juce::Colour blue { 0xff5b8cff };
constexpr const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const juce::Identifier helpId { "help" }, helpTitleId { "helpTitle" };

struct Fonts {
    Fonts()
        : inter(juce::Typeface::createSystemTypefaceFor(BinaryData::InterSemiBold_ttf,
                                                         BinaryData::InterSemiBold_ttfSize)),
          mono(juce::Typeface::createSystemTypefaceFor(BinaryData::JetBrainsMonoMedium_ttf,
                                                        BinaryData::JetBrainsMonoMedium_ttfSize)) {}
    juce::Typeface::Ptr inter, mono;
};
juce::Font font(const juce::Typeface::Ptr& face, float height, float tracking = 0.0f) {
    return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(tracking);
}

juce::Font labelFont(float height = 11.0f) {
    return font(juce::SharedResourcePointer<Fonts>()->inter, height, 0.09f);
}
juce::Font titleFont(float height) {
    return font(juce::SharedResourcePointer<Fonts>()->inter, height, 0.18f);
}
juce::Font valueFont(float height = 13.0f) {
    return font(juce::SharedResourcePointer<Fonts>()->mono, height);
}
juce::Font bodyFont(float height = 12.0f) {
    return font(juce::SharedResourcePointer<Fonts>()->inter, height, 0.01f);
}
void setHelp(juce::Component& control, const juce::String& title, const juce::String& help) {
    control.getProperties().set(helpTitleId, title.toUpperCase());
    control.getProperties().set(helpId, help);
}
juce::String noteName(float hz) {
    if (hz <= 0.0f) return "--";
    const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0)));
    return juce::String(names[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1);
}
}

void CodyPitchEditor::DarkLook::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                                  float position, float startAngle, float endAngle,
                                                  juce::Slider& slider) {
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const auto center = bounds.getCentre();
    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = std::max(2.5f, radius * 0.11f);
    g.setColour(slider.isMouseOverOrDragging() ? pale.brighter(0.1f) : pale);
    g.fillEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(center));
    g.setColour(background);
    g.fillEllipse(juce::Rectangle<float>(dot * 2.0f, dot * 2.0f)
        .withCentre(center.getPointOnCircumference(radius * 0.72f, angle)));
}

void CodyPitchEditor::DarkLook::drawPopupMenuBackground(juce::Graphics& g, int, int) {
    g.fillAll(panel);
}
void CodyPitchEditor::DarkLook::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
    bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
    const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*) {
    if (isSeparator) return;
    if (isHighlighted && isActive) {
        g.setColour(juce::Colour(0xff38383b));
        g.fillRect(area);
    }
    g.setColour(!isActive ? muted.withAlpha(0.4f) : isTicked ? pale : white);
    g.setFont(bodyFont(13));
    auto content = area.reduced(12, 0);
    g.drawText(text, content, juce::Justification::centredLeft);
    if (hasSubMenu) g.drawText(">", content, juce::Justification::centredRight);
}
juce::Font CodyPitchEditor::DarkLook::getPopupMenuFont() { return bodyFont(13); }

CodyPitchEditor::MenuChoice::MenuChoice(juce::RangedAudioParameter& parameter, juce::String labelToUse,
    juce::StringArray optionsToUse, std::vector<int> valuesToUse, juce::String help)
    : label(std::move(labelToUse)), options(std::move(optionsToUse)), values(std::move(valuesToUse)),
      attachment(parameter, [this](float value) {
          selected = static_cast<int>(std::lround(value));
          repaint();
      }, nullptr) {
    jassert(options.size() == static_cast<int>(values.size()));
    attachment.sendInitialUpdate();
    setHelp(*this, label, help);
}

void CodyPitchEditor::MenuChoice::paint(juce::Graphics& g) {
    g.setFont(labelFont());
    g.setColour(muted);
    const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(labelFont(), label));
    g.drawText(label, 0, 0, labelWidth + 2, getHeight(), juce::Justification::centredLeft);
    const int valueX = labelWidth + 12;
    juce::String selectedText = selected == 4 && label == "SCALE" ? "CUSTOM (OLD SESSION)"
        : selected == 4 && label == "VOCAL RANGE" ? "CUSTOM" : "--";
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] == selected) selectedText = options[static_cast<int>(i)].toUpperCase();
    g.setColour(hovered ? white : pale);
    g.drawText(selectedText, valueX, 0, getWidth() - valueX - 17, getHeight(), juce::Justification::centredLeft);
    const float ax = static_cast<float>(getWidth() - 12), ay = static_cast<float>(getHeight()) * 0.5f - 1.5f;
    juce::Path arrow;
    arrow.addTriangle(ax, ay, ax + 7.0f, ay, ax + 3.5f, ay + 4.0f);
    g.setColour(muted);
    g.fillPath(arrow);
}

void CodyPitchEditor::MenuChoice::mouseDown(const juce::MouseEvent&) {
    juce::PopupMenu menu;
    for (size_t i = 0; i < values.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, options[static_cast<int>(i)], true, values[i] == selected);
    menu.setLookAndFeel(&getLookAndFeel());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
        [safe = juce::Component::SafePointer<MenuChoice>(this)](int result) {
            if (safe != nullptr && result > 0)
                safe->attachment.setValueAsCompleteGesture(static_cast<float>(safe->values[static_cast<size_t>(result - 1)]));
        });
}
void CodyPitchEditor::MenuChoice::mouseEnter(const juce::MouseEvent&) { hovered = true; repaint(); }
void CodyPitchEditor::MenuChoice::mouseExit(const juce::MouseEvent&) { hovered = false; repaint(); }

CodyPitchEditor::Knob::Knob(juce::AudioProcessorValueTreeState& state, const char* parameterId,
                             juce::String name, juce::String unit, juce::String help, int diameterToUse)
    : title(std::move(name)), suffix(std::move(unit)), diameter(diameterToUse) {
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    const float halfSweep = juce::degreesToRadians(140.0f);
    const float full = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters(full - halfSweep, full + halfSweep, true);
    slider.setVelocityModeParameters(0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    if (auto* parameter = state.getParameter(parameterId))
        slider.setDoubleClickReturnValue(true, parameter->convertFrom0to1(parameter->getDefaultValue()));
    slider.textFromValueFunction = [displaySuffix = suffix](double value) {
        return juce::String(static_cast<int>(std::lround(value))) + displaySuffix;
    };
    slider.valueFromTextFunction = [](const juce::String& value) { return value.getDoubleValue(); };
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible(slider);

    editor.setJustification(juce::Justification::centred);
    editor.setFont(valueFont(12));
    editor.setIndents(0, 0);
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff38383b));
    editor.setColour(juce::TextEditor::textColourId, pale);
    editor.setColour(juce::TextEditor::highlightColourId, blue.withAlpha(0.5f));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.onReturnKey = [this] { finishEditing(true); };
    editor.onEscapeKey = [this] { finishEditing(false); };
    editor.onFocusLost = [this] { finishEditing(true); };
    addChildComponent(editor);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state, parameterId, slider);
    setHelp(*this, title, help);
}

void CodyPitchEditor::Knob::resized() {
    slider.setBounds(getWidth() / 2 - diameter / 2 - 1, 20, diameter + 2, diameter + 2);
    editor.setBounds(getWidth() / 2 - 40, getHeight() - 16, 80, 16);
}

void CodyPitchEditor::Knob::paint(juce::Graphics& g) {
    g.setColour(muted);
    g.setFont(labelFont());
    g.drawText(title, 0, 0, getWidth(), 16, juce::Justification::centred);
    g.setColour(pale);
    g.setFont(valueFont(12));
    g.drawText(juce::String(static_cast<int>(std::lround(slider.getValue()))) + suffix, 0, getHeight() - 16,
               getWidth(), 16, juce::Justification::centred);
}

void CodyPitchEditor::Knob::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.y < getHeight() - 16) return;
    editor.setText(juce::String(static_cast<int>(std::lround(slider.getValue()))) + suffix, false);
    editor.setVisible(true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void CodyPitchEditor::Knob::finishEditing(bool apply) {
    if (!editor.isVisible()) return;
    const auto entered = editor.getText().trim();
    editor.setVisible(false);
    if (apply && entered.containsAnyOf("0123456789"))
        slider.setValue(slider.getValueFromText(entered), juce::sendNotificationSync);
    repaint();
}


CodyPitchEditor::ChoiceRow::ChoiceRow(juce::RangedAudioParameter& parameter, juce::String labelToUse,
                                      juce::StringArray optionsToUse, juce::String help)
    : label(std::move(labelToUse)), options(std::move(optionsToUse)),
      attachment(parameter, [this](float value) { selected = static_cast<int>(std::lround(value)); repaint(); }, nullptr) {
    attachment.sendInitialUpdate();
    setHelp(*this, label, help);
}

int CodyPitchEditor::ChoiceRow::optionWidth(int index) const noexcept {
    return juce::roundToInt(juce::GlyphArrangement::getStringWidth(labelFont(), options[index])) + 2;
}
int CodyPitchEditor::ChoiceRow::optionX(int index) const noexcept {
    int x = juce::roundToInt(juce::GlyphArrangement::getStringWidth(labelFont(), label)) + 2 + 18;
    for (int i = 0; i < index; ++i) x += optionWidth(i) + 10;
    return x;
}
int CodyPitchEditor::ChoiceRow::optionAt(int x) const noexcept {
    for (int i = 0; i < options.size(); ++i)
        if (x >= optionX(i) && x < optionX(i) + optionWidth(i)) return i;
    return -1;
}

void CodyPitchEditor::ChoiceRow::paint(juce::Graphics& g) {
    g.setFont(labelFont());
    g.setColour(muted);
    g.drawText(label, 0, 0, optionX(0) - 18, getHeight(), juce::Justification::centredLeft);
    for (int i = 0; i < options.size(); ++i) {
        g.setColour(selected == i ? pale : hovered == i ? white : muted.withAlpha(0.7f));
        g.drawText(options[i], optionX(i), 0, optionWidth(i), getHeight(), juce::Justification::centredLeft);
    }
}

void CodyPitchEditor::ChoiceRow::mouseDown(const juce::MouseEvent& e) {
    const int choice = optionAt(e.x);
    if (choice >= 0) attachment.setValueAsCompleteGesture(static_cast<float>(choice));
}
void CodyPitchEditor::ChoiceRow::mouseMove(const juce::MouseEvent& e) {
    const int choice = optionAt(e.x);
    if (choice != hovered) { hovered = choice; repaint(); }
}
void CodyPitchEditor::ChoiceRow::mouseExit(const juce::MouseEvent&) { hovered = -1; repaint(); }

CodyPitchEditor::NoteRow::NoteRow(CodyPitchProcessor& p) : processor(p) {
    setHelp(*this, "Edit Notes", "Each pitch class applies in every octave. On is a tuning target; Remove redirects to another enabled note; Bypass leaves that detected note dry. Click a note to choose its state or return it to the selected scale.");
}

void CodyPitchEditor::NoteRow::paint(juce::Graphics& g) {
    g.setColour(muted);
    g.setFont(labelFont());
    g.drawText("EDIT NOTES", 0, 0, 86, getHeight(), juce::Justification::centredLeft);
    const int rootValue = static_cast<int>(processor.parameters.getRawParameterValue("root")->load());
    const int scaleValue = static_cast<int>(processor.parameters.getRawParameterValue("scale")->load());
    auto base = cody_pitch::scaleMask(rootValue, scaleValue);
    if (scaleValue == 4)
        for (int note = 0; note < 12; ++note)
            base[static_cast<size_t>(note)] = processor.parameters.getRawParameterValue("note" + juce::String(note))->load() > 0.5f;
    for (int note = 0; note < 12; ++note) {
        const int mode = static_cast<int>(processor.parameters.getRawParameterValue("noteMode" + juce::String(note))->load());
        const bool on = mode == 1 || (mode == 0 && base[static_cast<size_t>(note)]);
        const char* state = mode == 3 ? "BYP" : on ? "ON" : "OFF";
        const int x = 92 + note * 40;
        g.setColour(mode == 3 ? blue : on ? pale : muted.withAlpha(0.55f));
        g.setFont(valueFont(12));
        g.drawText(names[note], x, 1, 38, 16, juce::Justification::centred);
        g.setFont(valueFont(9.5f));
        g.drawText(state, x, 17, 38, 12, juce::Justification::centred);
    }
}

void CodyPitchEditor::NoteRow::mouseDown(const juce::MouseEvent& e) {
    const int note = (e.x - 92) / 40;
    if (e.x < 92 || note < 0 || note >= 12) return;
    auto* parameter = processor.parameters.getParameter("noteMode" + juce::String(note));
    if (parameter == nullptr) return;
    const int current = static_cast<int>(processor.parameters.getRawParameterValue("noteMode" + juce::String(note))->load());
    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(names[note]) + "  /  ALL OCTAVES");
    menu.addItem(1, "Follow Key & Scale", true, current == 0);
    menu.addItem(2, "On - tuning target", true, current == 1);
    menu.addItem(3, "Remove - redirect", true, current == 2);
    menu.addItem(4, "Bypass - leave dry", true, current == 3);
    menu.setLookAndFeel(&getLookAndFeel());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
        [safe = juce::Component::SafePointer<NoteRow>(this), note](int result) {
            if (safe == nullptr || result <= 0) return;
            if (auto* selected = safe->processor.parameters.getParameter("noteMode" + juce::String(note))) {
                selected->beginChangeGesture();
                selected->setValueNotifyingHost(selected->convertTo0to1(static_cast<float>(result - 1)));
                selected->endChangeGesture();
            }
            safe->repaint();
        });
}

CodyPitchEditor::CodyPitchEditor(CodyPitchProcessor& p)
    : juce::AudioProcessorEditor(&p), pitchProcessor(p),
      root(*p.parameters.getParameter("root"), "KEY",
           { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" },
           { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },
           "The song's tonal center. Choose it separately from Scale; F / Natural Minor is F minor."),
      scale(*p.parameters.getParameter("scale"), "SCALE",
            { "Major", "Natural Minor", "Harmonic Minor", "Chromatic", "Melodic Minor", "Dorian",
              "Phrygian", "Lydian", "Mixolydian", "Blues", "Major Pentatonic", "Minor Pentatonic" },
            { 0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12 },
            "Allowed notes for automatic correction. Chromatic accepts all twelve semitones. Old Custom Scale sessions remain loadable but cannot be newly selected here."),
      range(*p.parameters.getParameter("range"), "VOCAL RANGE",
            { "Bass", "Baritone", "Alto/Tenor", "Soprano" }, { 0, 1, 2, 3 },
            "Choose the broad voice category used by the pitch detector. Existing Custom sessions remain loadable, but its separate Hz bounds are not part of the Auto Mode control surface."),
      renderer(*p.parameters.getParameter("renderer"), "RENDERER", { "SPLICE", "PSOLA" },
               "Splice keeps your own waveform; PSOLA is an alternative waveform method."),
      noteRow(p) {
    setLookAndFeel(&look);
    for (auto* menu : { &root, &scale, &range }) addAndMakeVisible(*menu);

    addKnob("retune", "RETUNE SPEED", " ms", "How quickly a sung note is pulled toward the selected scale. At 0 ms, correction and its internal onset are immediate.");
    addKnob("sustain", "HUMANIZE", "%", "Relax correction on a held note; short notes stay tighter.");
    addKnob("tolerance", "FLEX TUNE", " ct", "Leave small pitch movements near the target unchanged.");
    addKnob("transpose", "TRANSPOSE", " st", "Shift the processed vocal in semitones after scale correction; this is not a vocal-range limit.", 36);
    addKnob("reference", "A4 REFERENCE", " Hz", "Global concert-pitch reference for note frequencies; normally 440 Hz, not a vocal-range limit.", 36);

    addAndMakeVisible(renderer);
    addAndMakeVisible(noteRow);
    status.setColour(juce::Label::textColourId, muted);
    status.setFont(valueFont(11));
    status.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(status);
    setHelp(status, "Current pitch", "The large note shows detected input pitch. The cents meter compares it with the chosen target; a bypassed note reads zero. Neither is an independent measurement of processed output.");
    showHelpFor(nullptr);
    setSize(900, 340);
    startTimerHz(12);
}

CodyPitchEditor::~CodyPitchEditor() { setLookAndFeel(nullptr); }

void CodyPitchEditor::addKnob(const char* id, const char* title, const juce::String& suffix,
                               const juce::String& help, int diameter) {
    auto knob = std::make_unique<Knob>(pitchProcessor.parameters, id, title, suffix, help, diameter);
    addAndMakeVisible(*knob);
    knobs.push_back(std::move(knob));
}

void CodyPitchEditor::showHelpFor(juce::Component* component) {
    juce::String title = "TUNA TUNER";
    juce::String description = "Select a key, scale and vocal range, then play your DAW. Hover over a control for details. Double-click a knob to reset it, or its value to type one.";
    for (auto* candidate = component; candidate != nullptr && candidate != this;
         candidate = candidate->getParentComponent()) {
        if (candidate->getProperties().contains(helpId)) {
            title = candidate->getProperties()[helpTitleId].toString();
            description = candidate->getProperties()[helpId].toString();
            break;
        }
    }
    if (title != helpTitle || description != helpText) {
        helpTitle = title;
        helpText = description;
        repaint(helpArea);
    }
}

void CodyPitchEditor::timerCallback() {
    const float input = pitchProcessor.detectedHz();
    const float target = pitchProcessor.targetHz();
    const float cents = input > 0.0f && target > 0.0f
        ? static_cast<float>(1200.0 * std::log2(input / target)) : 0.0f;
    status.setText(input > 0.0f
        ? juce::String(input, 1) + " Hz  /  "
          + juce::String(static_cast<int>(std::lround(cents))) + " ct"
        : "PLAY IN YOUR DAW",
        juce::dontSendNotification);
    noteRow.repaint();
    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    showHelpFor(under != nullptr && isParentOf(under) ? under : nullptr);
    repaint(juce::Rectangle<int>(630, 80, 250, 210));
}

void CodyPitchEditor::paint(juce::Graphics& g) {
    g.fillAll(background);
    g.setColour(pale);
    g.setFont(titleFont(17));
    g.drawText("TUNA TUNER", 24, 15, 180, 25, juce::Justification::centredLeft);
    g.setColour(muted);
    g.setFont(labelFont());
    g.drawText("CURRENT PITCH  /  INPUT", 645, 93, 225, 18, juce::Justification::centred);

    const float input = pitchProcessor.detectedHz(), target = pitchProcessor.targetHz();
    g.setColour(input > 0.0f ? pale : muted.withAlpha(0.4f));
    g.setFont(titleFont(78));
    g.drawText(noteName(input), 645, 119, 225, 87, juce::Justification::centred);

    constexpr float meterLeft = 674.0f, meterWidth = 166.0f;
    g.setColour(panel);
    g.fillRoundedRectangle(meterLeft, 223.0f, meterWidth, 5.0f, 2.0f);
    g.setColour(muted);
    g.fillRect(static_cast<int>(meterLeft + meterWidth * 0.5f), 218, 1, 16);
    g.setFont(valueFont(9));
    g.drawText("-100", 674, 234, 40, 16, juce::Justification::left);
    g.drawText("0", 737, 234, 40, 16, juce::Justification::centred);
    g.drawText("+100", 800, 234, 40, 16, juce::Justification::right);
    if (input > 0.0f && target > 0.0f) {
        const float cents = static_cast<float>(1200.0 * std::log2(input / target));
        const float x = meterLeft + meterWidth * 0.5f
            + std::clamp(cents / 100.0f, -1.0f, 1.0f) * meterWidth * 0.5f;
        g.setColour(blue);
        g.fillRoundedRectangle(x - 2.0f, 216.0f, 4.0f, 19.0f, 2.0f);
    }
    auto help = helpArea;
    g.setColour(pale);
    g.setFont(labelFont());
    g.drawText(helpTitle, help.removeFromTop(16), juce::Justification::topLeft);
    g.setColour(muted);
    g.setFont(bodyFont());
    g.drawFittedText(helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void CodyPitchEditor::resized() {
    root.setBounds(24, 62, 88, 18);
    scale.setBounds(126, 62, 190, 18);
    range.setBounds(329, 62, 200, 18);
    renderer.setBounds(24, 82, 200, 18);
    noteRow.setBounds(24, 230, 582, 31);
    status.setBounds(645, 257, 225, 20);
    helpArea = { 24, 272, 582, 56 };
    constexpr int x[3] = { 24, 138, 252 };
    for (int i = 0; i < 3; ++i) knobs[static_cast<size_t>(i)]->setBounds(x[i], 117, 104, 104);
    knobs[3]->setBounds(433, 105, 88, 76);  // Post-correction shift: utility, not a correction dial.
    knobs[4]->setBounds(539, 105, 88, 76);  // Reference tuning, near the scale/pitch area.
}

juce::AudioProcessorEditor* CodyPitchProcessor::createEditor() { return new CodyPitchEditor(*this); }
