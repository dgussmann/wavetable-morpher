#pragma once

#include "PluginProcessor.h"
#include "Views.h"

/** Controls for one oscillator (A or B): shape menu, import, draw, frame picker and the waveform itself. */
class SourcePanel : public juce::Component
{
public:
    SourcePanel (WavetableMorpherProcessor& p, int slotIndex, juce::Colour colour, const juce::String& title);

    void refresh();
    /** Highlights where in this oscillator's table the synth is playing (negative = show the selected frame). */
    void setPlayingPosition (float position);
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
    wtm::WavetableView tableView;   // whole imported wavetable, in 3D
    int numFrames = 1;
    bool showingTable = false;
    std::unique_ptr<juce::FileChooser> chooser;
    void selectFrame (int frame);
};

/** Floating window that shows the loaded synth's own interface (Vital, Serum 2...). */
class HostedSynthWindow : public juce::DocumentWindow
{
public:
    HostedSynthWindow (juce::AudioProcessorEditor* synthEditor, const juce::String& title, std::function<void()> onClose);
    void closeButtonPressed() override { if (onCloseRequested) onCloseRequested(); }

private:
    std::function<void()> onCloseRequested;
};

/** Drag this onto a synth's oscillator (Serum 2, Vital...) to load the current wavetable there. */
class DragHandle : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<juce::Result (juce::File&)> prepareFile;
    std::function<void (const juce::String&)> onStatus;

    void paint (juce::Graphics& g) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override    { dragging = false; }

private:
    bool dragging = false;
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
    void refreshSynthControls();
    void populateSynthMenu();
    void synthChosen();
    void openSynthWindow();
    void closeSynthWindow();
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

    // Play-through synth
    juce::Label playWithLabel;
    juce::ComboBox synthBox, oscBox;
    juce::TextButton openSynthButton { "Open Synth" }, sendButton { "Send to Vital" };
    juce::ToggleButton autoSendToggle { "Auto-send" }, soloToggle { "Solo" }, savePatchToggle { "Save patch" };
    DragHandle dragHandle;
    juce::Array<juce::File> synthFiles;
    std::unique_ptr<HostedSynthWindow> synthWindow;
    bool loadingSynth = false;

    // What the big view shows: your morph, or the synth's oscillator 1
    juce::Label playingLabel;
    juce::TextButton playAButton { "Osc A" }, playMorphButton { "Morph" }, playBButton { "Osc B" };
    juce::TextButton showSynthButton { "Synth Osc 1" };
    juce::Slider frameScrub;
    int viewFrameCount = 1;
    void choosePlaySource (WavetableMorpherProcessor::PlaySource);
    juce::TextButton useAsAButton { "Use as A" }, useAsBButton { "Use as B" };
    bool showingSynth = false;
    float synthViewPosition = 0.0f;
    void setShowingSynth (bool);
    void updateTableView();

    // Result + preview
    wtm::WavetableView tableView;
    juce::Label previewTitle;
    juce::OwnedArray<LabelledKnob> knobs;
    juce::MidiKeyboardComponent keyboard;
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> folderChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableMorpherEditor)
};
