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
    drawButton.onClick = [this]
    {
        waveform.setDrawMode (drawButton.getToggleState());
        refresh();
        resized();
    };
    addAndMakeVisible (drawButton);

    frameLabel.setText ("Frame", juce::dontSendNotification);
    styleLabel (frameLabel, 12.0f, Colours::dimText);
    addChildComponent (frameLabel);

    frameSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    frameSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    frameSlider.onValueChange = [this] { selectFrame ((int) frameSlider.getValue() - 1); };
    addChildComponent (frameSlider);

    tableView.setEmptyText ({});
    tableView.onPositionDragged = [this] (float pos) { selectFrame (juce::roundToInt (pos * (float) (numFrames - 1))); };
    addChildComponent (tableView);

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

    // A whole wavetable shows in 3D (click to pick the frame used for the morph); a single wave shows flat (and can be drawn on).
    numFrames = multiFrame ? (int) s.importedFrames.size() : 1;
    showingTable = multiFrame && ! drawButton.getToggleState();
    tableView.setVisible (showingTable);
    waveform.setVisible (! showingTable);
    if (showingTable)
        tableView.setFrames (s.importedFrames);
    setPlayingPosition (-1.0f);
}

void SourcePanel::selectFrame (int frame)
{
    proc.setImportedFrame (slot, frame);
    const auto& s = proc.getSlot (slot);
    frameSlider.setValue (s.importedFrame + 1, juce::dontSendNotification);
    waveform.setCycle (s.cycle);
    shapeBox.setText (s.describe(), juce::dontSendNotification);
    setPlayingPosition (-1.0f);
}

void SourcePanel::setPlayingPosition (float position)
{
    if (! showingTable)
        return;

    if (position < 0.0f)
        position = numFrames > 1 ? (float) proc.getSlot (slot).importedFrame / (float) (numFrames - 1) : 0.0f;
    tableView.setPosition (position);
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
    tableView.setBounds (r);
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
        if (showingSynth)
        {
            synthViewPosition = pos;
            tableView.setPosition (pos);
            return;
        }

        proc.setPlayPosition (pos);
    };
    addAndMakeVisible (tableView);

    // What the synth plays (and the big view shows)
    playingLabel.setText ("Playing:", juce::dontSendNotification);
    styleLabel (playingLabel, 13.0f, Colours::dimText);
    playingLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (playingLabel);

    using PS = WavetableMorpherProcessor::PlaySource;
    playAButton.setTooltip ("The synth plays Oscillator A's whole wavetable");
    playMorphButton.setTooltip ("The synth plays the morph from A to B");
    playBButton.setTooltip ("The synth plays Oscillator B's whole wavetable");
    playAButton.onClick     = [this] { choosePlaySource (PS::OscA); };
    playMorphButton.onClick = [this] { choosePlaySource (PS::Morph); };
    playBButton.onClick     = [this] { choosePlaySource (PS::OscB); };
    playAButton.setColour (juce::TextButton::buttonOnColourId, Colours::accentA.withAlpha (0.55f));
    playBButton.setColour (juce::TextButton::buttonOnColourId, Colours::accentB.withAlpha (0.55f));
    for (auto* b : { &playAButton, &playMorphButton, &playBButton })
        addAndMakeVisible (b);

    showSynthButton.setClickingTogglesState (true);
    showSynthButton.onClick = [this] { setShowingSynth (showSynthButton.getToggleState()); };
    addChildComponent (showSynthButton);

    // Frame slider under the big view
    frameScrub.setSliderStyle (juce::Slider::LinearHorizontal);
    frameScrub.setRange (0.0, 1.0, 0.0);
    frameScrub.setTextBoxStyle (juce::Slider::TextBoxRight, true, 110, 22);
    frameScrub.textFromValueFunction = [this] (double v)
    {
        return "Frame " + juce::String (juce::roundToInt (v * (viewFrameCount - 1)) + 1) + " / " + juce::String (viewFrameCount);
    };
    frameScrub.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    frameScrub.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    frameScrub.setTooltip ("Scroll through the frames. While Vital is playing this also moves Vital's wavetable position.");
    frameScrub.onValueChange = [this]
    {
        if (! frameScrub.isMouseButtonDown())
            return;
        // Snap to whole frames
        const auto v = viewFrameCount > 1 ? std::round (frameScrub.getValue() * (viewFrameCount - 1)) / (viewFrameCount - 1) : 0.0;
        if (showingSynth)
        {
            synthViewPosition = (float) v;
            tableView.setPosition ((float) v);
        }
        else
        {
            proc.setPlayPosition ((float) v);
            tableView.setPosition ((float) v);
        }
    };
    addAndMakeVisible (frameScrub);
    showSynthButton.setTooltip ("Show the wavetable that's in the loaded synth's oscillator 1. Pick a wavetable in Vital and it appears here.");

    useAsAButton.setTooltip ("Use the highlighted frame of the synth's wavetable as Oscillator A");
    useAsBButton.setTooltip ("Use the highlighted frame of the synth's wavetable as Oscillator B");
    auto useFrame = [this] (int slot)
    {
        const auto& t = proc.getSynthTable();
        if (t.empty()) return;
        const int frame = juce::roundToInt (synthViewPosition * (float) ((int) t.size() - 1));
        proc.useSynthTable (slot, frame);
        setStatus (juce::String (slot == 0 ? "Oscillator A" : "Oscillator B") + " now uses frame " + juce::String (frame + 1)
                   + " of Vital's \"" + proc.getSynthTableName() + "\". Use its Frame slider to pick another.");
        refreshAll();
    };
    useAsAButton.onClick = [useFrame] { useFrame (0); };
    useAsBButton.onClick = [useFrame] { useFrame (1); };
    addChildComponent (useAsAButton);
    addChildComponent (useAsBButton);

    // (No built-in synth: the wavetable always plays through Serum 2, Vital or another loaded synth.)

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

    sendButton.setTooltip ("Load the playing wavetable into the synth's oscillator now");
    sendButton.onClick = [this]
    {
        const auto r = proc.sendToSynth();
        setStatus (r.wasOk() ? "Sent \"" + proc.getPlayName() + "\" to " + proc.getHostedShortName() + " oscillator " + proc.oscillatorLabel (proc.getVitalOscillator()) + "."
                             : r.getErrorMessage(), r.failed());
        refreshSynthControls();
    };
    addAndMakeVisible (sendButton);

    autoSendToggle.setTooltip ("Update Vital automatically whenever the morph changes (between notes)");
    autoSendToggle.onClick = [this] { proc.setAutoSend (autoSendToggle.getToggleState()); };
    addAndMakeVisible (autoSendToggle);

    oscBox.addItemList ({ "Osc 1", "Osc 2", "Osc 3" }, 1);
    oscBox.setTooltip ("Which of the synth's oscillators plays the wavetable");

    soloToggle.setTooltip ("Switch off the synth's other oscillators (and noise / sub / sample) so you hear only this wavetable");
    soloToggle.onClick = [this] { proc.setSolo (soloToggle.getToggleState()); };
    addAndMakeVisible (soloToggle);
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
        else setStatus (proc.getHostedShortName() + " updated: \"" + proc.getPlayName() + "\" is playing in oscillator "
                        + proc.oscillatorLabel (proc.getVitalOscillator()) + (proc.getSolo() ? " (solo)." : "."));
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
    setStatus ("Pick two waveforms, choose what plays (Osc A, Morph or Osc B), and play the keyboard.");

    proc.addChangeListener (this);
    tableView.setFrames (proc.getMorphFrames());

    refreshAll(); // show the current state straight away (play source, synth controls, slider readout)

    // Nothing to play through yet (a fresh instance)? Load Serum 2 - or Vital - straight away.
    if (proc.getHostedSynth() == nullptr && ! proc.isRestoringSynth() && ! synthFiles.isEmpty())
    {
        int pick = 0;
        for (int i = 0; i < synthFiles.size(); ++i)
            if (synthFiles.getReference (i).getFileNameWithoutExtension().startsWithIgnoreCase ("Serum 2")) { pick = i; break; }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<WavetableMorpherEditor> (this), pick]
        {
            if (safe != nullptr && safe->proc.getHostedSynth() == nullptr && ! safe->proc.isRestoringSynth())
            {
                safe->synthBox.setSelectedId (100 + pick, juce::dontSendNotification);
                safe->synthChosen();
            }
        });
    }
    else if (proc.getHostedSynth() == nullptr)
    {
        setStatus ("Choose a synth in \"Play with\" (Serum 2 or Vital) to hear your wavetable.");
    }

    setResizable (true, true);
    setResizeLimits (940, 680, 1600, 1150);
    setSize (1020, 720);
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

    // Wavetable view + synth row
    auto synthRow = r.removeFromBottom (28);
    playWithLabel.setBounds (synthRow.removeFromLeft (62));
    synthBox.setBounds (synthRow.removeFromLeft (190));
    synthRow.removeFromLeft (8);
    openSynthButton.setBounds (synthRow.removeFromLeft (92));
    synthRow.removeFromLeft (14);
    dragHandle.setBounds (synthRow.removeFromRight (132));
    synthRow.removeFromRight (12);
    sendButton.setBounds (synthRow.removeFromLeft (124));
    synthRow.removeFromLeft (8);
    oscBox.setBounds (synthRow.removeFromLeft (84));
    synthRow.removeFromLeft (8);
    autoSendToggle.setBounds (synthRow.removeFromLeft (92));
    soloToggle.setBounds (synthRow.removeFromLeft (64));

    r.removeFromBottom (8);
    frameScrub.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (6);
    tableView.setBounds (r);

    // "Playing: Osc A | Morph | Osc B" over the top-centre of the view; synth-view toggle to its right.
    auto overlay = r.reduced (10, 6).removeFromTop (24);
    auto centre = overlay.withSizeKeepingCentre (330, 24);
    playingLabel.setBounds (centre.removeFromLeft (66));
    centre.removeFromLeft (6);
    playAButton.setBounds (centre.removeFromLeft (84));
    playMorphButton.setBounds (centre.removeFromLeft (90));
    playBButton.setBounds (centre.removeFromLeft (84));
    showSynthButton.setBounds (juce::Rectangle<int> (playBButton.getRight() + 14, centre.getY(), 110, 24));
    auto bottom = r.reduced (10, 8).removeFromBottom (24);
    useAsBButton.setBounds (bottom.removeFromRight (80));
    bottom.removeFromRight (6);
    useAsAButton.setBounds (bottom.removeFromRight (80));
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
    updateTableView();

    if (! nameEditor.hasKeyboardFocus (true) && nameEditor.getText() != proc.getTableName())
        nameEditor.setText (proc.getTableName(), false);

    refreshSynthControls();
}

//==============================================================================
void WavetableMorpherEditor::populateSynthMenu()
{
    synthFiles = proc.findSynthPlugins();
    synthBox.clear (juce::dontSendNotification);
    synthBox.setTextWhenNothingSelected ("Choose a synth...");

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
    const bool vital = proc.canSendToSynth(); // Vital or Serum 2

    if (! loadingSynth)
    {
        int id = 0;
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
        if (id > 0) synthBox.setSelectedId (id, juce::dontSendNotification);
        else        synthBox.setSelectedId (0, juce::dontSendNotification);
    }

    openSynthButton.setEnabled (hosted);
    sendButton.setVisible (vital);
    oscBox.setVisible (vital);
    autoSendToggle.setVisible (vital);
    for (int i = 0; i < 3; ++i)
        oscBox.changeItemText (i + 1, "Osc " + proc.oscillatorLabel (i));
    oscBox.setSelectedId (proc.getVitalOscillator() + 1, juce::dontSendNotification);
    soloToggle.setVisible (vital);
    soloToggle.setToggleState (proc.getSolo(), juce::dontSendNotification);
    autoSendToggle.setToggleState (proc.getAutoSend(), juce::dontSendNotification);
    sendButton.setButtonText (vital && proc.isVitalUpToDate() ? juce::String::fromUTF8 ("Sent \xe2\x9c\x93") : "Send to " + proc.getHostedShortName());
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

    if (id <= 0)
        return;

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
        auto& pr = safe->proc;
        safe->setStatus (pr.canSendToSynth()
                             ? "Playing through " + pr.getHostedShortName() + ": your wavetable goes into oscillator "
                                   + pr.oscillatorLabel (pr.getVitalOscillator()) + (pr.getSolo() ? " (solo)" : "")
                                   + " automatically. Use the frame slider to move through it."
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

void WavetableMorpherEditor::choosePlaySource (WavetableMorpherProcessor::PlaySource p)
{
    proc.setPlaySource (p);
    showingSynth = false;
    updateTableView();

    using PS = WavetableMorpherProcessor::PlaySource;
    const auto what = p == PS::OscA ? "Oscillator A's wavetable (" + proc.getSlot (0).tableName() + ")"
                    : p == PS::OscB ? "Oscillator B's wavetable (" + proc.getSlot (1).tableName() + ")"
                                    : juce::String ("the morph");
    setStatus ("Now playing " + what + (proc.canSendToSynth() ? " through " + proc.getHostedShortName() + " oscillator "
                                            + proc.oscillatorLabel (proc.getVitalOscillator()) + "." : "."));
}

void WavetableMorpherEditor::setShowingSynth (bool shouldShow)
{
    showingSynth = shouldShow && proc.getHostedSynth() != nullptr;
    updateTableView();

    if (showingSynth)
    {
        const auto note = proc.getSynthTableNote();
        if (proc.getSynthTable().empty())
            setStatus (note.isNotEmpty() ? note : "Reading the synth's oscillator 1...", note.isNotEmpty());
        else
            setStatus (proc.getHostedShortName() + (proc.isHostedSerum() ? " Osc A" : " Osc 1") + ": \"" + proc.getSynthTableName() + "\". Click or drag in the view to pick a frame, then Use as A / B."
                       + (note.isNotEmpty() ? "  (" + note + ")" : juce::String()));
    }
}

void WavetableMorpherEditor::updateTableView()
{
    const bool hosted = proc.getHostedSynth() != nullptr;
    if (! hosted && showingSynth)
        showingSynth = false;

    using PS = WavetableMorpherProcessor::PlaySource;
    const auto ps = proc.getPlaySource();
    playAButton.setToggleState (! showingSynth && ps == PS::OscA, juce::dontSendNotification);
    playMorphButton.setToggleState (! showingSynth && ps == PS::Morph, juce::dontSendNotification);
    playBButton.setToggleState (! showingSynth && ps == PS::OscB, juce::dontSendNotification);

    showSynthButton.setVisible (hosted);
    showSynthButton.setButtonText (proc.isHostedSerum() ? "View Serum Osc A" : proc.isHostedVital() ? "View Vital Osc 1" : "View Synth Osc 1");
    showSynthButton.setToggleState (showingSynth, juce::dontSendNotification);

    const bool haveSynthTable = ! proc.getSynthTable().empty();
    useAsAButton.setVisible (showingSynth && haveSynthTable);
    useAsBButton.setVisible (showingSynth && haveSynthTable);

    if (showingSynth)
    {
        const auto note = proc.getSynthTableNote();
        tableView.setEmptyText (note.isNotEmpty() ? note : "Reading the synth's oscillator 1...");
        tableView.setFrames (proc.getSynthTable());
        tableView.setPosition (synthViewPosition);
    }
    else
    {
        tableView.setEmptyText ("Building wavetable...");
        tableView.setFrames (proc.getPlayFrames());
    }

    viewFrameCount = std::max (1, showingSynth ? (int) proc.getSynthTable().size() : (int) proc.getPlayFrames().size());
    frameScrub.updateText();
}

void WavetableMorpherEditor::timerCallback()
{
    // Where the synth is in the playing table: the loaded synth's own position knob, or the built-in one.
    const float pos = proc.getHostedSynth() != nullptr ? proc.getPlayPosition() : proc.getDisplayPosition();

    if (! showingSynth)
    {
        tableView.setPosition (pos);
        if (! frameScrub.isMouseButtonDown())
            frameScrub.setValue (pos, juce::dontSendNotification);
    }

    using PS = WavetableMorpherProcessor::PlaySource;
    const auto ps = proc.getPlaySource();
    panelA.setPlayingPosition (ps == PS::OscA ? pos : -1.0f);
    panelB.setPlayingPosition (ps == PS::OscB ? pos : -1.0f);
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
