#include "PluginEditor.h"

using namespace wtm;

namespace
{
    void styleLabel (juce::Label& l, float size, juce::Colour c, bool bold = false)
    {
        auto f = juce::FontOptions (size);
        if (bold) f = f.withStyle ("Bold");
        l.setFont (f);
        l.setColour (juce::Label::textColourId, c);
    }
}

//==============================================================================
SourcePanel::SourcePanel (WavetableMorpherProcessor& p, int slotIndex, juce::Colour c, const juce::String& title)
    : proc (p), slot (slotIndex), colour (c), waveform (c)
{
    titleLabel.setText (title, juce::dontSendNotification);
    styleLabel (titleLabel, 15.0f, colour, true);
    addAndMakeVisible (titleLabel);

    shapeBox.addItemList (shapeNames(), 1);
    shapeBox.setTooltip ("Pick a starting waveform");
    shapeBox.onChange = [this]
    {
        if (shapeBox.getSelectedId() > 0)
        {
            drawButton.setToggleState (false, juce::dontSendNotification);
            waveform.setDrawMode (false);
            proc.setShape (slot, shapeBox.getSelectedId() - 1);
            refresh();
        }
    };
    addAndMakeVisible (shapeBox);

    importButton.setTooltip ("Load a wave or wavetable (WAV, AIFF, FLAC) - jump straight to your Serum 2 or Vital tables");
    importButton.onClick = [this] { importClicked(); };
    addAndMakeVisible (importButton);

    prevButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x80"));   // ◀
    nextButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6"));   // ▶
    prevButton.setTooltip ("Previous wavetable in this folder (or previous shape)");
    nextButton.setTooltip ("Next wavetable in this folder (or next shape)");
    prevButton.onClick = [this] { step (-1); };
    nextButton.onClick = [this] { step (+1); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    drawButton.setClickingTogglesState (true);
    drawButton.setTooltip ("Draw your own waveform with the mouse");
    drawButton.onClick = [this] { waveform.setDrawMode (drawButton.getToggleState()); };
    addAndMakeVisible (drawButton);

    frameLabel.setText ("Frame", juce::dontSendNotification);
    styleLabel (frameLabel, 12.0f, Colours::dimText);
    addChildComponent (frameLabel);

    frameSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    frameSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    frameSlider.onValueChange = [this]
    {
        proc.setImportedFrame (slot, (int) frameSlider.getValue() - 1);
        waveform.setCycle (proc.getSlot (slot).cycle);
        shapeBox.setText (proc.getSlot (slot).describe(), juce::dontSendNotification);
    };
    addChildComponent (frameSlider);

    waveform.onDrawn = [this] (const Cycle& drawn)
    {
        proc.setDrawnCycle (slot, drawn);
        refresh();
    };
    addAndMakeVisible (waveform);

    refresh();
}

void SourcePanel::refresh()
{
    const auto& s = proc.getSlot (slot);

    if (s.kind == WavetableMorpherProcessor::SourceSlot::Kind::Shape)
        shapeBox.setSelectedId (s.shapeIndex + 1, juce::dontSendNotification);
    else
        shapeBox.setText (s.describe(), juce::dontSendNotification);

    const bool multiFrame = s.kind == WavetableMorpherProcessor::SourceSlot::Kind::Imported && s.importedFrames.size() > 1;
    frameLabel.setVisible (multiFrame);
    frameSlider.setVisible (multiFrame);
    if (multiFrame)
    {
        frameSlider.setRange (1.0, (double) s.importedFrames.size(), 1.0);
        frameSlider.setValue (s.importedFrame + 1, juce::dontSendNotification);
    }

    waveform.setCycle (s.cycle);
}

void SourcePanel::importClicked()
{
    juce::PopupMenu menu;
    const auto serum   = detectBrowseFolder (Destination::Serum2);
    const auto vital   = detectBrowseFolder (Destination::Vital);
    const auto massive = detectBrowseFolder (Destination::MassiveXL);
    const auto library = proc.getDestinationFolder (Destination::Library);
    const auto last    = proc.getLastImportFolder();

    menu.addSectionHeader ("Import a wave or wavetable from...");
    menu.addItem (1, "Serum 2 Tables" + juce::String (serum.isDirectory() ? "" : "  (not found)"), serum.isDirectory());
    menu.addItem (2, "Vital Wavetables" + juce::String (vital.isDirectory() ? "" : "  (not found)"), vital.isDirectory());
    if (massive.isDirectory())
        menu.addItem (3, "Massive XL Wavetables");
    menu.addItem (4, "My saved morphs", library.isDirectory());
    if (last.isDirectory())
        menu.addItem (5, "Last folder: " + last.getFileName());
    menu.addSeparator();
    menu.addItem (6, "Browse...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&importButton),
                        [this, serum, vital, massive, library, last] (int result)
                        {
                            switch (result)
                            {
                                case 1: browseFrom (serum); break;
                                case 2: browseFrom (vital); break;
                                case 3: browseFrom (massive); break;
                                case 4: browseFrom (library); break;
                                case 5: browseFrom (last); break;
                                case 6: browseFrom (last.isDirectory() ? last : juce::File::getSpecialLocation (juce::File::userMusicDirectory)); break;
                                default: break;
                            }
                        });
}

void SourcePanel::browseFrom (const juce::File& folder)
{
    chooser = std::make_unique<juce::FileChooser> ("Import a waveform or wavetable", folder, audioWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;

                              const auto r = proc.importFile (slot, file);
                              if (r.failed())
                              {
                                  if (onStatus) onStatus (r.getErrorMessage());
                                  return;
                              }

                              drawButton.setToggleState (false, juce::dontSendNotification);
                              waveform.setDrawMode (false);
                              refresh();
                              resized();

                              const auto n = proc.getSlot (slot).importedFrames.size();
                              if (onStatus)
                                  onStatus ("Imported \"" + file.getFileName() + "\""
                                            + (n > 1 ? " - " + juce::String ((int) n) + " frames, pick one with the Frame slider. Use the arrows to try the next table in this folder."
                                                     : juce::String (". Use the arrows to try the next file in this folder.")));
                          });
}

void SourcePanel::step (int direction)
{
    juce::String description;
    const auto r = proc.stepSource (slot, direction, description);

    drawButton.setToggleState (false, juce::dontSendNotification);
    waveform.setDrawMode (false);
    refresh();
    resized();

    if (onStatus)
        onStatus (r.wasOk() ? (slot == 0 ? "Oscillator A: " : "Oscillator B: ") + description : r.getErrorMessage());
}

void SourcePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (colour.withAlpha (0.35f));
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
}

void SourcePanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (26);
    titleLabel.setBounds (top.removeFromLeft (120));
    drawButton.setBounds (top.removeFromRight (60));
    top.removeFromRight (6);
    importButton.setBounds (top.removeFromRight (76));

    r.removeFromTop (6);
    auto selector = r.removeFromTop (26);
    prevButton.setBounds (selector.removeFromLeft (30));
    nextButton.setBounds (selector.removeFromRight (30));
    shapeBox.setBounds (selector.reduced (6, 0));

    r.removeFromTop (8);
    if (frameSlider.isVisible())
    {
        auto row = r.removeFromBottom (24);
        frameLabel.setBounds (row.removeFromLeft (48));
        frameSlider.setBounds (row);
        r.removeFromBottom (4);
    }
    waveform.setBounds (r);
}

//==============================================================================
LabelledKnob::LabelledKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& name)
    : attachment (state, paramID, slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (slider);

    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    styleLabel (label, 12.0f, Colours::dimText);
    addAndMakeVisible (label);
}

void LabelledKnob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

//==============================================================================
HostedSynthWindow::HostedSynthWindow (juce::AudioProcessorEditor* synthEditor, const juce::String& title, std::function<void()> onClose)
    : DocumentWindow (title, Colours::background, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      onCloseRequested (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    setContentOwned (synthEditor, true);
    setResizable (synthEditor->isResizable(), false);
    setAlwaysOnTop (true); // keeps it in front of Ableton's main window
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

void DragHandle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool over = isMouseOver (true);
    g.setColour (Colours::accent.withAlpha (over ? 0.30f : 0.16f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (Colours::accent.withAlpha (0.7f));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    // grip dots
    g.setColour (Colours::text.withAlpha (0.8f));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse (10.0f + (float) col * 5.0f, r.getCentreY() - 6.0f + (float) row * 5.0f, 2.6f, 2.6f);

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("Drag to synth", getLocalBounds().withTrimmedLeft (26), juce::Justification::centredLeft);
}

void DragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 4 || ! prepareFile)
        return;

    dragging = true;
    juce::File file;
    const auto r = prepareFile (file);
    if (r.failed())
    {
        if (onStatus) onStatus (r.getErrorMessage());
        return;
    }

    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [safe = juce::Component::SafePointer<DragHandle> (this)]
                                                                {
                                                                    if (safe != nullptr) safe->dragging = false;
                                                                });
    if (onStatus)
        onStatus ("Drop it on the oscillator's wavetable display in Serum or Vital.");
}

//==============================================================================
WavetableMorpherEditor::WavetableMorpherEditor (WavetableMorpherProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      panelA (p, 0, Colours::accentA, "Oscillator A"),
      panelB (p, 1, Colours::accentB, "Oscillator B"),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lookAndFeel);

    // Header ---------------------------------------------------------------
    titleLabel.setText ("WAVETABLE MORPHER", juce::dontSendNotification);
    styleLabel (titleLabel, 18.0f, Colours::text, true);
    addAndMakeVisible (titleLabel);

    nameLabel.setText ("Name", juce::dontSendNotification);
    styleLabel (nameLabel, 13.0f, Colours::dimText);
    addAndMakeVisible (nameLabel);

    nameEditor.setText (proc.getTableName(), false);
    nameEditor.setFont (juce::FontOptions (15.0f));
    nameEditor.setIndents (8, 6);
    nameEditor.onTextChange = [this] { proc.setTableName (nameEditor.getText()); };
    nameEditor.onReturnKey = [this] { saveTable(); };
    addAndMakeVisible (nameEditor);

    saveButton.setColour (juce::TextButton::buttonColourId, Colours::accent.withAlpha (0.8f));
    saveButton.onClick = [this] { saveTable(); };
    addAndMakeVisible (saveButton);

    sendLabel.setText ("Also send to:", juce::dontSendNotification);
    styleLabel (sendLabel, 13.0f, Colours::dimText);
    addAndMakeVisible (sendLabel);

    for (auto d : sendTargets())
    {
        auto& t = toggleFor (d);
        t.setToggleState (proc.isDestinationEnabled (d), juce::dontSendNotification);
        t.setTooltip ("Also save into " + destinationName (d) + "'s wavetable folder");
        t.onClick = [this, d] { proc.setDestinationEnabled (d, toggleFor (d).getToggleState()); };
        addAndMakeVisible (t);
    }

    foldersButton.onClick = [this] { showFoldersMenu(); };
    addAndMakeVisible (foldersButton);

    // Sources + morph -------------------------------------------------------
    panelA.onStatus = panelB.onStatus = [this] (const juce::String& s) { setStatus (s); };
    addAndMakeVisible (panelA);
    addAndMakeVisible (panelB);

    morphTitle.setText ("Morph", juce::dontSendNotification);
    styleLabel (morphTitle, 15.0f, Colours::accent, true);
    morphTitle.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (morphTitle);

    auto setupBox = [this] (juce::Label& label, const juce::String& text, juce::ComboBox& box, const juce::StringArray& items)
    {
        label.setText (text, juce::dontSendNotification);
        styleLabel (label, 12.0f, Colours::dimText);
        addAndMakeVisible (label);
        box.addItemList (items, 1);
        addAndMakeVisible (box);
    };

    setupBox (framesLabel, "Frames", framesBox, frameCountNames());
    setupBox (modeLabel, "Morph mode", modeBox, morphModeNames());
    setupBox (curveLabel, "Curve", curveBox, morphCurveNames());
    modeBox.setTooltip ("Crossfade blends samples directly. Spectral blends each harmonic, which usually sounds smoother.");
    curveBox.setTooltip ("How the morph is spread across the frames");

    framesAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, ParamIDs::frames, framesBox);
    modeAttach   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, ParamIDs::morphMode, modeBox);
    curveAttach  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, ParamIDs::curve, curveBox);

    normaliseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, ParamIDs::normalise, normaliseToggle);
    addAndMakeVisible (normaliseToggle);

    swapButton.onClick = [this] { proc.swapSlots(); refreshAll(); };
    addAndMakeVisible (swapButton);

    // Result + preview -------------------------------------------------------
    tableView.onPositionDragged = [this] (float pos)
    {
        if (auto* param = proc.apvts.getParameter (ParamIDs::position))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (pos));
            param->endChangeGesture();
        }
    };
    addAndMakeVisible (tableView);

    previewTitle.setText ("Preview", juce::dontSendNotification);
    styleLabel (previewTitle, 15.0f, Colours::accent, true);
    addAndMakeVisible (previewTitle);

    const std::pair<const char*, const char*> knobDefs[] = {
        { ParamIDs::position, "Position" }, { ParamIDs::scanDepth, "Scan Depth" }, { ParamIDs::scanRate, "Scan Rate" },
        { ParamIDs::attack, "Attack" },     { ParamIDs::decay, "Decay" },          { ParamIDs::sustain, "Sustain" },
        { ParamIDs::release, "Release" },   { ParamIDs::volume, "Volume" },
    };
    for (const auto& [id, name] : knobDefs)
        addAndMakeVisible (knobs.add (new LabelledKnob (proc.apvts, id, name)));

    // Play-through synth ---------------------------------------------------
    playWithLabel.setText ("Play with", juce::dontSendNotification);
    styleLabel (playWithLabel, 13.0f, Colours::dimText);
    addAndMakeVisible (playWithLabel);

    synthBox.setTooltip ("Hear your wavetable in a real synth. Vital updates automatically; for Serum, use Drag to synth.");
    synthBox.onChange = [this] { synthChosen(); };
    addAndMakeVisible (synthBox);

    openSynthButton.setTooltip ("Show the synth's own window, to shape the sound");
    openSynthButton.onClick = [this] { openSynthWindow(); };
    addAndMakeVisible (openSynthButton);

    sendButton.setTooltip ("Put the current morph into Vital's oscillator now");
    sendButton.onClick = [this]
    {
        const auto r = proc.sendToVital();
        setStatus (r.wasOk() ? "Sent \"" + proc.getTableName() + "\" to Vital oscillator " + juce::String (proc.getVitalOscillator() + 1) + "."
                             : r.getErrorMessage(), r.failed());
        refreshSynthControls();
    };
    addAndMakeVisible (sendButton);

    autoSendToggle.setTooltip ("Update Vital automatically whenever the morph changes (between notes)");
    autoSendToggle.onClick = [this] { proc.setAutoSend (autoSendToggle.getToggleState()); };
    addAndMakeVisible (autoSendToggle);

    oscBox.addItemList ({ "Osc 1", "Osc 2", "Osc 3" }, 1);
    oscBox.setTooltip ("Which Vital oscillator receives the wavetable");
    oscBox.onChange = [this] { proc.setVitalOscillator (oscBox.getSelectedId() - 1); };
    addAndMakeVisible (oscBox);

    dragHandle.setTooltip ("Drag onto Serum 2's (or Vital's) oscillator display to load this wavetable");
    dragHandle.prepareFile = [this] (juce::File& f) { return proc.writeDragFile (f); };
    dragHandle.onStatus = [this] (const juce::String& s) { setStatus (s); };
    addAndMakeVisible (dragHandle);

    proc.onHostedSynthWillChange = [this] { closeSynthWindow(); };
    proc.onVitalSent = [this] (const juce::String& error)
    {
        if (error.isNotEmpty()) setStatus (error, true);
        else setStatus ("Vital updated: \"" + proc.getTableName() + "\" is in oscillator " + juce::String (proc.getVitalOscillator() + 1) + ".");
        refreshSynthControls();
    };
    populateSynthMenu();

    keyboard.setLowestVisibleKey (36);
    keyboard.setKeyWidth (22.0f);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Colours::accent.withAlpha (0.6f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Colours::accent.withAlpha (0.25f));
    addAndMakeVisible (keyboard);

    styleLabel (statusLabel, 13.0f, Colours::dimText);
    addAndMakeVisible (statusLabel);
    setStatus ("Pick two waveforms, play the keyboard to hear them, then name and save your wavetable.");

    proc.addChangeListener (this);
    tableView.setFrames (proc.getMorphFrames());

    setResizable (true, true);
    setResizeLimits (940, 720, 1600, 1100);
    setSize (1020, 770);
    startTimerHz (30);
}

WavetableMorpherEditor::~WavetableMorpherEditor()
{
    proc.onHostedSynthWillChange = nullptr;
    proc.onVitalSent = nullptr;
    closeSynthWindow();
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void WavetableMorpherEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    // Morph column card
    auto morphCard = juce::Rectangle<int> (framesBox.getX() - 12, morphTitle.getY() - 10,
                                           framesBox.getWidth() + 24, panelA.getHeight()).toFloat();
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (morphCard, 10.0f);
    g.setColour (Colours::accent.withAlpha (0.3f));
    g.drawRoundedRectangle (morphCard.reduced (0.5f), 10.0f, 1.0f);

    // Little arrows showing A -> morph -> B
    g.setColour (Colours::dimText);
    const float y = (float) panelA.getBounds().getCentreY();
    g.drawArrow ({ (float) panelA.getRight() + 1.0f, y, morphCard.getX() - 1.0f, y }, 1.5f, 7.0f, 7.0f);
    g.drawArrow ({ (float) panelB.getX() - 1.0f, y, morphCard.getRight() + 1.0f, y }, 1.5f, 7.0f, 7.0f);
}

void WavetableMorpherEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);

    // Header
    auto header = r.removeFromTop (34);
    titleLabel.setBounds (header.removeFromLeft (200));
    foldersButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (10);
    massiveToggle.setBounds (header.removeFromRight (100));
    vitalToggle.setBounds (header.removeFromRight (66));
    serumToggle.setBounds (header.removeFromRight (82));
    sendLabel.setBounds (header.removeFromRight (92));
    header.removeFromRight (10);
    saveButton.setBounds (header.removeFromRight (130).reduced (0, 2));
    header.removeFromRight (8);
    nameLabel.setBounds (header.removeFromLeft (44));
    nameEditor.setBounds (header.reduced (0, 2));

    r.removeFromTop (10);

    // Status + keyboard at the bottom
    statusLabel.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);
    keyboard.setBounds (r.removeFromBottom (64));
    r.removeFromBottom (10);

    // Sources row
    auto sources = r.removeFromTop (std::max (272, (int) ((float) r.getHeight() * 0.44f)));
    const int morphW = 180, gap = 22;
    const int panelW = (sources.getWidth() - morphW - 2 * gap) / 2;
    panelA.setBounds (sources.removeFromLeft (panelW));
    panelB.setBounds (sources.removeFromRight (panelW));
    sources.removeFromLeft (gap);
    sources.removeFromRight (gap);

    auto morph = sources.reduced (12, 10);
    morphTitle.setBounds (morph.removeFromTop (22));
    morph.removeFromTop (4);
    auto addRow = [&morph] (juce::Label& l, juce::Component& c)
    {
        l.setBounds (morph.removeFromTop (15));
        c.setBounds (morph.removeFromTop (24));
        morph.removeFromTop (5);
    };
    addRow (framesLabel, framesBox);
    addRow (modeLabel, modeBox);
    addRow (curveLabel, curveBox);
    normaliseToggle.setBounds (morph.removeFromTop (24));
    swapButton.setBounds (morph.removeFromBottom (26));

    r.removeFromTop (12);

    // Wavetable view + synth row + preview knobs
    auto knobArea = r.removeFromBottom (92);
    r.removeFromBottom (6);
    auto synthRow = r.removeFromBottom (28);
    playWithLabel.setBounds (synthRow.removeFromLeft (66));
    synthBox.setBounds (synthRow.removeFromLeft (220));
    synthRow.removeFromLeft (8);
    openSynthButton.setBounds (synthRow.removeFromLeft (96));
    synthRow.removeFromLeft (16);
    dragHandle.setBounds (synthRow.removeFromRight (140));
    synthRow.removeFromRight (16);
    sendButton.setBounds (synthRow.removeFromLeft (110));
    synthRow.removeFromLeft (8);
    oscBox.setBounds (synthRow.removeFromLeft (80));
    synthRow.removeFromLeft (8);
    autoSendToggle.setBounds (synthRow.removeFromLeft (96));
    previewTitle.setBounds (knobArea.removeFromLeft (80).withTrimmedTop (8));
    const int kw = knobArea.getWidth() / std::max (1, knobs.size());
    for (auto* k : knobs)
        k->setBounds (knobArea.removeFromLeft (kw).reduced (4, 0));

    r.removeFromBottom (8);
    tableView.setBounds (r);
}

//==============================================================================
void WavetableMorpherEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void WavetableMorpherEditor::refreshAll()
{
    panelA.refresh();
    panelB.refresh();
    panelA.resized();
    panelB.resized();
    tableView.setFrames (proc.getMorphFrames());

    if (! nameEditor.hasKeyboardFocus (true) && nameEditor.getText() != proc.getTableName())
        nameEditor.setText (proc.getTableName(), false);

    refreshSynthControls();
}

//==============================================================================
void WavetableMorpherEditor::populateSynthMenu()
{
    synthFiles = proc.findSynthPlugins();
    synthBox.clear (juce::dontSendNotification);
    synthBox.addItem ("Built-in preview synth", 1);
    synthBox.addSeparator();

    for (int i = 0; i < synthFiles.size(); ++i)
        synthBox.addItem (synthFiles.getReference (i).getFileNameWithoutExtension(), 100 + i);

    if (synthFiles.isEmpty())
        synthBox.addItem ("(no VST3 plugins found)", 3), synthBox.setItemEnabled (3, false);

    synthBox.addSeparator();
    synthBox.addItem ("Rescan plugins", 2);
    refreshSynthControls();
}

void WavetableMorpherEditor::refreshSynthControls()
{
    const auto file = proc.getHostedSynthFile();
    const bool hosted = proc.getHostedSynth() != nullptr;
    const bool vital = proc.isHostedVital();

    if (! loadingSynth)
    {
        int id = 1;
        if (hosted)
        {
            const int index = synthFiles.indexOf (file);
            if (index >= 0) id = 100 + index;
            else
            {
                // Loaded from a saved project but not in the list (e.g. moved): show it anyway.
                synthFiles.add (file);
                synthBox.addItem (file.getFileNameWithoutExtension(), 100 + synthFiles.size() - 1);
                id = 100 + synthFiles.size() - 1;
            }
        }
        synthBox.setSelectedId (id, juce::dontSendNotification);
    }

    openSynthButton.setEnabled (hosted);
    sendButton.setVisible (vital);
    oscBox.setVisible (vital);
    autoSendToggle.setVisible (vital);
    oscBox.setSelectedId (proc.getVitalOscillator() + 1, juce::dontSendNotification);
    autoSendToggle.setToggleState (proc.getAutoSend(), juce::dontSendNotification);
    sendButton.setButtonText (vital && proc.isVitalUpToDate() ? juce::String::fromUTF8 ("Sent \xe2\x9c\x93") : juce::String ("Send to Vital"));

    // The built-in synth's knobs do nothing while a real synth is playing.
    for (auto* k : knobs)
        k->setEnabled (! hosted);
    previewTitle.setText (hosted ? "Built-in" : "Preview", juce::dontSendNotification);
}

void WavetableMorpherEditor::synthChosen()
{
    const int id = synthBox.getSelectedId();

    if (id == 2) // rescan
    {
        populateSynthMenu();
        setStatus ("Found " + juce::String (synthFiles.size()) + " VST3 plugins.");
        return;
    }

    if (id == 1)
    {
        proc.unloadHostedSynth();
        setStatus ("Using the built-in preview synth.");
        refreshSynthControls();
        return;
    }

    const int index = id - 100;
    if (! juce::isPositiveAndBelow (index, synthFiles.size()))
        return;

    const auto file = synthFiles.getReference (index);
    if (file == proc.getHostedSynthFile())
        return;

    loadingSynth = true;
    setStatus ("Loading " + file.getFileNameWithoutExtension() + "...");

    juce::Component::SafePointer<WavetableMorpherEditor> safe (this);
    proc.loadHostedSynth (file, [safe, file] (juce::Result r)
    {
        if (safe == nullptr)
            return;

        safe->loadingSynth = false;
        if (r.failed())
        {
            safe->setStatus (r.getErrorMessage(), true);
            safe->refreshSynthControls();
            return;
        }

        const auto name = file.getFileNameWithoutExtension();
        safe->setStatus (safe->proc.isHostedVital()
                             ? "Playing through Vital. Your morph goes into oscillator " + juce::String (safe->proc.getVitalOscillator() + 1)
                                   + " automatically - play a note, and use Vital's Position knob to scan it."
                             : "Playing through " + name + ". Drag the wavetable onto " + name + "'s oscillator using \"Drag to synth\".");
        safe->refreshSynthControls();
        safe->openSynthWindow();
    });
}

void WavetableMorpherEditor::openSynthWindow()
{
    auto* synth = proc.getHostedSynth();
    if (synth == nullptr)
        return;

    if (synthWindow != nullptr)
    {
        synthWindow->setVisible (true);
        synthWindow->toFront (true);
        return;
    }

    auto* ed = synth->createEditorAndMakeActive();
    if (ed == nullptr)
    {
        setStatus (synth->getName() + " has no window to show.", true);
        return;
    }

    juce::Component::SafePointer<WavetableMorpherEditor> safe (this);
    synthWindow = std::make_unique<HostedSynthWindow> (ed, synth->getName() + "  -  Wavetable Morpher", [safe]
    {
        // Close later: the window can't delete itself inside its own button callback.
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeSynthWindow(); });
    });
}

void WavetableMorpherEditor::closeSynthWindow()
{
    synthWindow.reset(); // deletes the synth's editor before the synth itself can go away
}

void WavetableMorpherEditor::timerCallback()
{
    tableView.setPosition (proc.getDisplayPosition());
}

void WavetableMorpherEditor::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? juce::Colour (0xffff8a7a) : Colours::dimText);
}

//==============================================================================
std::vector<Destination> WavetableMorpherEditor::sendTargets()
{
    return { Destination::Serum2, Destination::Vital, Destination::MassiveXL };
}

juce::ToggleButton& WavetableMorpherEditor::toggleFor (Destination d)
{
    if (d == Destination::Vital)     return vitalToggle;
    if (d == Destination::MassiveXL) return massiveToggle;
    return serumToggle;
}

void WavetableMorpherEditor::saveTable()
{
    const auto name = nameEditor.getText().trim();
    if (name.isEmpty())
    {
        setStatus ("Type a name for your wavetable first.", true);
        nameEditor.grabKeyboardFocus();
        return;
    }
    proc.setTableName (name);

    const auto frames = proc.getMorphFrames();
    if (frames.empty())
    {
        setStatus ("The wavetable is still being built - try again in a moment.", true);
        return;
    }

    // Any enabled synth whose folder we couldn't find? Ask for it once (it's remembered).
    for (auto d : sendTargets())
    {
        if (proc.isDestinationEnabled (d) && proc.getDestinationFolder (d) == juce::File())
        {
            chooseFolderFor (d, [this] { saveTable(); });
            return;
        }
    }

    const auto fileName = juce::File::createLegalFileName (name) + ".wav";

    juce::StringArray existing;
    if (proc.getDestinationFolder (Destination::Library).getChildFile (fileName).existsAsFile())
        existing.add (destinationName (Destination::Library));
    for (auto d : sendTargets())
        if (proc.isDestinationEnabled (d) && proc.getDestinationFolder (d).getChildFile (fileName).existsAsFile())
            existing.add (destinationName (d));

    if (existing.isEmpty())
    {
        writeAll (frames, fileName);
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Replace \"" + name + "\"?")
                                      .withMessage ("A wavetable with this name already exists in: " + existing.joinIntoString (", ") + ".")
                                      .withButton ("Replace")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [safeThis = juce::Component::SafePointer<WavetableMorpherEditor> (this), frames, fileName] (int result)
                                  {
                                      if (safeThis != nullptr && result == 1)
                                          safeThis->writeAll (frames, fileName);
                                  });
}

void WavetableMorpherEditor::writeAll (const std::vector<Cycle>& frames, const juce::String& fileName)
{
    std::vector<Destination> targets { Destination::Library };
    for (auto d : sendTargets())
        if (proc.isDestinationEnabled (d))
            targets.push_back (d);

    juce::StringArray saved, problems;
    for (auto d : targets)
    {
        const auto file = proc.getDestinationFolder (d).getChildFile (fileName);
        const auto r = writeWavetableWav (file, frames);
        if (r.wasOk())
            saved.add (destinationName (d));
        else
            problems.add (destinationName (d) + ": " + r.getErrorMessage()
                          + (destinationHelp (d).isNotEmpty() ? "\n" + destinationHelp (d) : juce::String()));
    }

    const auto name = fileName.upToLastOccurrenceOf (".", false, false);
    if (! saved.isEmpty())
        setStatus ("Saved \"" + name + "\" (" + juce::String ((int) frames.size()) + " frames) to " + saved.joinIntoString (", ")
                   + ". If it doesn't show in your synth yet, reopen its wavetable browser.");

    if (! problems.isEmpty())
    {
        setStatus ("Some copies couldn't be saved - see the message.", true);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Couldn't save everywhere")
                                          .withMessage (problems.joinIntoString ("\n\n")
                                                        + "\n\nYou can pick a different folder from the Folders menu.")
                                          .withButton ("OK")
                                          .withAssociatedComponent (this),
                                      nullptr);
    }
}

void WavetableMorpherEditor::chooseFolderFor (Destination d, std::function<void()> then, bool disableOnCancel)
{
    setStatus ("Couldn't find " + destinationName (d) + "'s wavetable folder. " + destinationHelp (d));

    folderChooser = std::make_unique<juce::FileChooser> ("Choose " + destinationName (d) + "'s wavetable folder",
                                                         juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [this, d, then, disableOnCancel] (const juce::FileChooser& fc)
                                {
                                    const auto folder = fc.getResult();
                                    if (folder == juce::File() && disableOnCancel)
                                    {
                                        // Cancelled: stop sending there so we don't keep asking.
                                        proc.setDestinationEnabled (d, false);
                                        toggleFor (d).setToggleState (false, juce::dontSendNotification);
                                        setStatus ("Turned off sending to " + destinationName (d) + ".");
                                    }
                                    else if (folder != juce::File())
                                    {
                                        proc.setDestinationFolder (d, folder);
                                    }

                                    if (then) then();
                                });
}

void WavetableMorpherEditor::showFoldersMenu()
{
    juce::PopupMenu menu;
    std::vector<Destination> all { Destination::Library, Destination::Serum2, Destination::Vital, Destination::MassiveXL };

    for (size_t i = 0; i < all.size(); ++i)
    {
        const auto d = all[i];
        const auto folder = proc.getDestinationFolder (d);
        const int base = (int) i * 10;

        juce::PopupMenu sub;
        sub.addItem (base + 1, folder == juce::File() ? "Not found yet" : folder.getFullPathName(), false);
        sub.addSeparator();
        sub.addItem (base + 2, "Choose folder...");
        sub.addItem (base + 3, "Show in Finder", folder.isDirectory());
        sub.addItem (base + 4, "Reset to automatic");
        menu.addSubMenu (destinationName (d), sub);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&foldersButton),
                        [this, all] (int result)
                        {
                            if (result <= 0)
                                return;

                            const auto d = all[(size_t) (result / 10)];
                            switch (result % 10)
                            {
                                case 2: chooseFolderFor (d, [this, d]
                                                         {
                                                             setStatus (destinationName (d) + " folder: " + proc.getDestinationFolder (d).getFullPathName());
                                                         }, false);
                                        break;
                                case 3: proc.getDestinationFolder (d).startAsProcess(); break;
                                case 4: proc.setDestinationFolder (d, juce::File());
                                        setStatus (destinationName (d) + " folder reset to automatic.");
                                        break;
                                default: break;
                            }
                        });
}
