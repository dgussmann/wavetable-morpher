#pragma once

#include "PluginProcessor.h"
#include "Views.h"

/** Controls for one oscillator (A or B): shape menu, import, draw, frame picker and the waveform itself. */
class SourcePanel : public juce::Component
{
public:
    SourcePanel (WavetableMorpherProcessor& p, int slotIndex, juce::Colour colour, const juce::String& title);

    void refresh();
    void paint (juce::Graphics& g) override;
    void resized() override;

    std::function<void (const juce::String&)> onStatus;

private:
    void importClicked();
    void browseFrom (const juce::File& folder);
    void step (int direction);

    WavetableMorpherProcessor& proc;
    const int slot;
    const juce::Colour colour;

    juce::Label titleLabel, frameLabel;
    juce::ComboBox shapeBox;
    juce::TextButton importButton { "Import" }, drawButton { "Draw" };
    juce::TextButton prevButton, nextButton;
    juce::Slider frameSlider;
    wtm::WaveformView waveform;
    std::unique_ptr<juce::FileChooser> chooser;
};

/** A rotary knob with its name underneath, attached to a parameter. */
class LabelledKnob : public juce::Component
{
public:
    LabelledKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& name);
    void resized() override;

    juce::Slider slider;

private:
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

class WavetableMorpherEditor : public juce::AudioProcessorEditor,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    explicit WavetableMorpherEditor (WavetableMorpherProcessor&);
    ~WavetableMorpherEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void refreshAll();
    void setStatus (const juce::String& text, bool isError = false);

    void saveTable();
    void writeAll (const std::vector<wtm::Cycle>& frames, const juce::String& fileName);
    void chooseFolderFor (wtm::Destination d, std::function<void()> then, bool disableOnCancel = true);
    void showFoldersMenu();

    static std::vector<wtm::Destination> sendTargets();
    juce::ToggleButton& toggleFor (wtm::Destination d);

    WavetableMorpherProcessor& proc;
    wtm::MorphLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };

    // Header
    juce::Label titleLabel, nameLabel, sendLabel;
    juce::TextEditor nameEditor;
    juce::TextButton saveButton { "Save Wavetable" }, foldersButton { "Folders" };
    juce::ToggleButton serumToggle { "Serum 2" }, vitalToggle { "Vital" }, massiveToggle { "Massive XL" };

    // Sources + morph settings
    SourcePanel panelA, panelB;
    juce::Label morphTitle, framesLabel, modeLabel, curveLabel;
    juce::ComboBox framesBox, modeBox, curveBox;
    juce::ToggleButton normaliseToggle { "Normalize each frame" };
    juce::TextButton swapButton { "Swap A / B" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> framesAttach, modeAttach, curveAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> normaliseAttach;

    // Result + preview
    wtm::WavetableView tableView;
    juce::Label previewTitle;
    juce::OwnedArray<LabelledKnob> knobs;
    juce::MidiKeyboardComponent keyboard;
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> folderChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableMorpherEditor)
};
