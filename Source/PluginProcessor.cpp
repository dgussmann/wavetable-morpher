#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
/** Builds wavetables off the message thread so the UI never stalls, even at 256 frames. */
class WavetableMorpherProcessor::Worker : public juce::Thread
{
public:
    explicit Worker (WavetableMorpherProcessor& p) : juce::Thread ("Wavetable builder"), owner (p) {}
    void run() override
    {
        while (! threadShouldExit())
        {
            owner.rebuildWorker();
            wait (-1);
        }
    }

private:
    WavetableMorpherProcessor& owner;
};

//==============================================================================
juce::String WavetableMorpherProcessor::SourceSlot::describe() const
{
    switch (kind)
    {
        case Kind::Shape:    return wtm::shapeNames()[shapeIndex];
        case Kind::Imported: return importedName + (importedFrames.size() > 1 ? " (frame " + juce::String (importedFrame + 1) + ")" : juce::String());
        case Kind::Drawn:    return "Drawn";
    }
    return {};
}

//==============================================================================
WavetableMorpherProcessor::WavetableMorpherProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    slots[0].shapeIndex = (int) wtm::Shape::Sine;
    slots[0].cycle = wtm::makeShape (wtm::Shape::Sine);
    slots[1].shapeIndex = (int) wtm::Shape::Saw;
    slots[1].cycle = wtm::makeShape (wtm::Shape::Saw);

    for (int i = 0; i < 8; ++i)
        synth.addVoice (new wtm::WavetableVoice (renderContext));
    synth.addSound (new wtm::WavetableSound());

    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

    worker = std::make_unique<Worker> (*this);
    lastRequestedSettings = getMorphSettings();
    requestRebuild();
    worker->startThread();

    startTimerHz (10);
}

WavetableMorpherProcessor::~WavetableMorpherProcessor()
{
    stopTimer();
    onHostedSynthWillChange = nullptr;
    onVitalSent = nullptr;
    unloadHostedSynth();
    worker->signalThreadShouldExit();
    worker->notify();
    worker->stopThread (4000);
}

juce::AudioProcessorValueTreeState::ParameterLayout WavetableMorpherProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto seconds = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    };

    auto fmt = [] (std::function<String (float)> f)
    {
        return AudioParameterFloatAttributes().withStringFromValueFunction ([f] (float v, int) { return f (v); });
    };
    auto percent = fmt ([] (float v) { return String (juce::roundToInt (v * 100.0f)) + " %"; });
    auto timeAttr = fmt ([] (float v) { return v < 1.0f ? String (juce::roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; });

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::position, 1 }, "Morph Position",
                                                       NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::scanRate, 1 }, "Scan Rate",
                                                       seconds (0.02f, 10.0f, 0.5f), 0.25f,
                                                       fmt ([] (float v) { return String (v, 2) + " Hz"; })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::scanDepth, 1 }, "Scan Depth",
                                                       NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::attack, 1 }, "Attack",
                                                       seconds (0.001f, 5.0f, 0.2f), 0.005f, timeAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::decay, 1 }, "Decay",
                                                       seconds (0.001f, 5.0f, 0.4f), 0.3f, timeAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::sustain, 1 }, "Sustain",
                                                       NormalisableRange<float> (0.0f, 1.0f), 0.8f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::release, 1 }, "Release",
                                                       seconds (0.001f, 10.0f, 0.5f), 0.3f, timeAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::volume, 1 }, "Volume",
                                                       NormalisableRange<float> (-48.0f, 6.0f), -12.0f,
                                                       fmt ([] (float v) { return String (v, 1) + " dB"; })));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::frames, 1 }, "Frames",
                                                        wtm::frameCountNames(), 5)); // 256, like Serum
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::morphMode, 1 }, "Morph Mode",
                                                        wtm::morphModeNames(), 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::curve, 1 }, "Morph Curve",
                                                        wtm::morphCurveNames(), 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::normalise, 1 }, "Normalize Frames", true));

    return layout;
}

//==============================================================================
void WavetableMorpherProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = std::max (32, samplesPerBlock);
    synth.setCurrentPlaybackSampleRate (sampleRate);
    keyboardState.reset();
    lfoPhase = 0.0;
    heldNotes = 0;
    prepared = true;

    const juce::ScopedLock sl (hostLock);
    prepareHosted();
}

void WavetableMorpherProcessor::releaseResources()
{
    const juce::ScopedLock sl (hostLock);
    if (hosted != nullptr)
        hosted->releaseResources();
}

void WavetableMorpherProcessor::prepareHosted()
{
    // Caller holds hostLock.
    if (hosted == nullptr)
        return;

    hosted->setRateAndBufferSizeDetails (currentSampleRate, currentBlockSize);
    hosted->prepareToPlay (currentSampleRate, currentBlockSize);
    const int channels = std::max ({ 2, hosted->getTotalNumInputChannels(), hosted->getTotalNumOutputChannels() });
    hostBuffer.setSize (channels, currentBlockSize * 2, false, true, false);
    setLatencySamples (hosted->getLatencySamples());
}

bool WavetableMorpherProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void WavetableMorpherProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Pick up a freshly built wavetable if the builder has one ready.
    {
        const juce::SpinLock::ScopedTryLockType lock (tableLock);
        if (lock.isLocked() && pendingTable != activeTable)
            activeTable = pendingTable;
    }

    const int numSamples = buffer.getNumSamples();

    // Morph position = knob + optional scanning LFO (so you can hear the whole table move).
    const float base  = apvts.getRawParameterValue (ParamIDs::position)->load();
    const float depth = apvts.getRawParameterValue (ParamIDs::scanDepth)->load();
    const float rate  = apvts.getRawParameterValue (ParamIDs::scanRate)->load();

    lfoPhase += (double) rate * (double) numSamples / currentSampleRate;
    lfoPhase -= std::floor (lfoPhase);
    const float lfo = (float) std::sin (lfoPhase * juce::MathConstants<double>::twoPi);
    const float position = juce::jlimit (0.0f, 1.0f, base + depth * lfo * (depth > 0.0f ? std::max (base, 1.0f - base) : 0.0f));

    renderContext.table = activeTable.get();
    renderContext.positionStart = lastPosition;
    renderContext.positionEnd = position;
    renderContext.gain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ParamIDs::volume)->load());
    renderContext.envelope.attack  = apvts.getRawParameterValue (ParamIDs::attack)->load();
    renderContext.envelope.decay   = apvts.getRawParameterValue (ParamIDs::decay)->load();
    renderContext.envelope.sustain = apvts.getRawParameterValue (ParamIDs::sustain)->load();
    renderContext.envelope.release = apvts.getRawParameterValue (ParamIDs::release)->load();

    keyboardState.processNextMidiBuffer (midi, 0, numSamples, true);

    // Track held notes, so Vital is only updated between notes (updating it cuts sound).
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())                              heldNotes.fetch_add (1);
        else if (m.isNoteOff() && heldNotes.load() > 0) heldNotes.fetch_sub (1);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) heldNotes = 0;
    }

    if (hostedActive.load())
    {
        // Play through the loaded synth (Vital, Serum 2...). Silence for a moment while it's being swapped/updated.
        const juce::ScopedTryLock tl (hostLock);
        if (tl.isLocked() && hosted != nullptr)
        {
            if (hostBuffer.getNumSamples() < numSamples)
                hostBuffer.setSize (hostBuffer.getNumChannels(), numSamples, false, false, true);

            juce::AudioBuffer<float> view (hostBuffer.getArrayOfWritePointers(), hostBuffer.getNumChannels(), numSamples);
            view.clear();
            hosted->processBlock (view, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.copyFrom (ch, 0, view, std::min (ch, view.getNumChannels() - 1), 0, numSamples);
        }

        midi.clear();
        displayPosition.store (base);
        return;
    }

    synth.renderNextBlock (buffer, midi, 0, numSamples);

    lastPosition = position;
    displayPosition.store (position);
}

//==============================================================================
wtm::MorphSettings WavetableMorpherProcessor::getMorphSettings() const
{
    wtm::MorphSettings s;
    s.numFrames = wtm::frameCountForIndex ((int) apvts.getRawParameterValue (ParamIDs::frames)->load());
    s.mode  = (wtm::MorphMode) (int) apvts.getRawParameterValue (ParamIDs::morphMode)->load();
    s.curve = (wtm::MorphCurve) (int) apvts.getRawParameterValue (ParamIDs::curve)->load();
    s.normaliseFrames = apvts.getRawParameterValue (ParamIDs::normalise)->load() > 0.5f;
    return s;
}

void WavetableMorpherProcessor::timerCallback()
{
    // Structural settings change rarely; polling keeps all rebuild work off the audio thread.
    const auto s = getMorphSettings();
    if (s.numFrames != lastRequestedSettings.numFrames || s.mode != lastRequestedSettings.mode
        || s.curve != lastRequestedSettings.curve || s.normaliseFrames != lastRequestedSettings.normaliseFrames)
        requestRebuild();

    // Vital auto-send: once the morph has settled for a moment and no notes are held.
    if (autoSend && hostedIsVital && hosted != nullptr && lastSentVersion != tableVersion.load()
        && heldNotes.load() == 0
        && juce::Time::getMillisecondCounterHiRes() - lastTableChangeMs.load() > 250.0)
    {
        const auto r = sendToVital();
        if (onVitalSent)
            onVitalSent (r.wasOk() ? juce::String() : r.getErrorMessage());
    }
}

void WavetableMorpherProcessor::requestRebuild()
{
    lastRequestedSettings = getMorphSettings();
    {
        const juce::ScopedLock sl (jobLock);
        jobA = slots[0].cycle;
        jobB = slots[1].cycle;
        jobSettings = lastRequestedSettings;
        jobPending = true;
    }
    if (worker != nullptr)
        worker->notify();
}

void WavetableMorpherProcessor::rebuildWorker()
{
    for (;;)
    {
        wtm::Cycle a, b;
        wtm::MorphSettings s;
        {
            const juce::ScopedLock sl (jobLock);
            if (! jobPending)
                return;
            a = jobA; b = jobB; s = jobSettings;
            jobPending = false;
        }

        auto frames = wtm::buildMorph (a, b, s);
        auto table = std::make_shared<const wtm::PlaybackTable> (frames);

        // Drop tables nobody uses any more (never the one we're about to publish).
        keepAlive.erase (std::remove_if (keepAlive.begin(), keepAlive.end(),
                                         [] (const auto& t) { return t.use_count() == 1; }),
                         keepAlive.end());
        keepAlive.push_back (table);

        {
            const juce::SpinLock::ScopedLockType lock (tableLock);
            pendingTable = table;
        }
        {
            const juce::ScopedLock sl (displayLock);
            displayFrames = std::move (frames);
        }

        lastTableChangeMs = juce::Time::getMillisecondCounterHiRes();
        tableVersion.fetch_add (1);
        sendChangeMessage();
    }
}

std::vector<wtm::Cycle> WavetableMorpherProcessor::getMorphFrames() const
{
    const juce::ScopedLock sl (displayLock);
    return displayFrames;
}

//==============================================================================
void WavetableMorpherProcessor::setShape (int slot, int shapeIndex)
{
    auto& s = slots[(size_t) slot];
    s.kind = SourceSlot::Kind::Shape;
    s.shapeIndex = juce::jlimit (0, (int) wtm::Shape::NumShapes - 1, shapeIndex);
    s.cycle = wtm::makeShape ((wtm::Shape) s.shapeIndex);
    requestRebuild();
}

juce::Result WavetableMorpherProcessor::importFile (int slot, const juce::File& file)
{
    wtm::ImportedAudio imported;
    auto r = wtm::importAudioFile (file, imported);
    if (r.failed())
        return r;

    auto& s = slots[(size_t) slot];
    s.kind = SourceSlot::Kind::Imported;
    s.importedName = imported.fileName;
    s.importedFile = file;
    s.importedFrames = std::move (imported.frames);
    s.importedFrame = 0;
    s.cycle = s.importedFrames.front();
    setLastImportFolder (file.getParentDirectory());
    requestRebuild();
    return r;
}

juce::Result WavetableMorpherProcessor::stepSource (int slot, int direction, juce::String& description)
{
    auto& s = slots[(size_t) slot];
    direction = direction < 0 ? -1 : 1;

    if (s.kind == SourceSlot::Kind::Imported && s.importedFile.getParentDirectory().isDirectory())
    {
        const auto files = wtm::audioFilesIn (s.importedFile.getParentDirectory());
        if (files.isEmpty())
            return juce::Result::fail ("No wavetables left in " + s.importedFile.getParentDirectory().getFileName() + ".");

        int index = files.indexOf (s.importedFile);
        const int count = files.size();
        juce::String lastError;

        // Skip over any files that won't load (wrong format, too short...), but never loop forever.
        for (int tries = 0; tries < count; ++tries)
        {
            index = index < 0 ? (direction > 0 ? 0 : count - 1) : (index + direction + count) % count;
            const auto r = importFile (slot, files.getReference (index));
            if (r.wasOk())
            {
                description = juce::String (index + 1) + " / " + juce::String (count) + " in "
                              + files.getReference (index).getParentDirectory().getFileName() + ":  "
                              + files.getReference (index).getFileNameWithoutExtension();
                return r;
            }
            lastError = r.getErrorMessage();
        }

        return juce::Result::fail (lastError);
    }

    // Built-in shapes (or a drawn wave): step through the shape list.
    const int numShapes = (int) wtm::Shape::NumShapes;
    const int next = s.kind == SourceSlot::Kind::Shape ? (s.shapeIndex + direction + numShapes) % numShapes
                                                       : (direction > 0 ? 0 : numShapes - 1);
    setShape (slot, next);
    description = wtm::shapeNames()[next];
    return juce::Result::ok();
}

juce::File WavetableMorpherProcessor::getLastImportFolder()
{
    const auto path = settings().getValue ("lastImportFolder");
    const juce::File f = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    return f.isDirectory() ? f : juce::File();
}

void WavetableMorpherProcessor::setLastImportFolder (const juce::File& folder)
{
    auto& st = settings();
    st.setValue ("lastImportFolder", folder.getFullPathName());
    st.saveIfNeeded();
}

void WavetableMorpherProcessor::setImportedFrame (int slot, int frame)
{
    auto& s = slots[(size_t) slot];
    if (s.kind != SourceSlot::Kind::Imported || s.importedFrames.empty())
        return;

    s.importedFrame = juce::jlimit (0, (int) s.importedFrames.size() - 1, frame);
    s.cycle = s.importedFrames[(size_t) s.importedFrame];
    requestRebuild();
}

void WavetableMorpherProcessor::setDrawnCycle (int slot, const wtm::Cycle& cycle)
{
    auto& s = slots[(size_t) slot];
    s.kind = SourceSlot::Kind::Drawn;
    s.cycle = cycle;
    wtm::removeDC (s.cycle);
    requestRebuild();
}

void WavetableMorpherProcessor::swapSlots()
{
    std::swap (slots[0], slots[1]);
    requestRebuild();
}

//==============================================================================
juce::PropertiesFile& WavetableMorpherProcessor::settings()
{
    if (settingsFile == nullptr)
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "WavetableMorpher";
        o.filenameSuffix = "settings";
        o.folderName = "WavetableMorpher";
        o.osxLibrarySubFolder = "Application Support";
        settingsFile = std::make_unique<juce::PropertiesFile> (o);
    }
    settingsFile->reload();
    return *settingsFile;
}

juce::File WavetableMorpherProcessor::getDestinationFolder (wtm::Destination d)
{
    const auto chosen = settings().getValue ("folder_" + wtm::destinationKey (d));
    if (chosen.isNotEmpty() && juce::File::isAbsolutePath (chosen))
        return juce::File (chosen);
    return wtm::detectDefaultFolder (d);
}

void WavetableMorpherProcessor::setDestinationFolder (wtm::Destination d, const juce::File& folder)
{
    auto& s = settings();
    s.setValue ("folder_" + wtm::destinationKey (d), folder.getFullPathName());
    s.saveIfNeeded();
}

bool WavetableMorpherProcessor::isDestinationEnabled (wtm::Destination d)
{
    const bool defaultOn = d == wtm::Destination::Serum2 || d == wtm::Destination::Vital;
    return settings().getBoolValue ("send_" + wtm::destinationKey (d), defaultOn);
}

void WavetableMorpherProcessor::setDestinationEnabled (wtm::Destination d, bool enabled)
{
    auto& s = settings();
    s.setValue ("send_" + wtm::destinationKey (d), enabled);
    s.saveIfNeeded();
}

//==============================================================================
void WavetableMorpherProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    juce::ValueTree sources ("SOURCES");
    sources.setProperty ("tableName", tableName, nullptr);

    for (int i = 0; i < 2; ++i)
    {
        const auto& s = slots[(size_t) i];
        juce::ValueTree slot ("SLOT");
        slot.setProperty ("index", i, nullptr);
        slot.setProperty ("kind", (int) s.kind, nullptr);
        slot.setProperty ("shape", s.shapeIndex, nullptr);
        slot.setProperty ("importedName", s.importedName, nullptr);
        slot.setProperty ("importedFile", s.importedFile.getFullPathName(), nullptr);
        slot.setProperty ("cycle", wtm::cycleToBase64 (s.cycle), nullptr);
        sources.appendChild (slot, nullptr);
    }

    state.appendChild (sources, nullptr);

    {
        juce::ValueTree host ("HOST");
        host.setProperty ("vitalOsc", vitalOsc, nullptr);
        host.setProperty ("autoSend", autoSend, nullptr);

        const juce::ScopedLock sl (lifetimeLock);
        if (hosted != nullptr)
        {
            juce::MemoryBlock hostedState;
            hosted->getStateInformation (hostedState);
            host.setProperty ("file", hostedFile.getFullPathName(), nullptr);
            host.setProperty ("state", hostedState.toBase64Encoding(), nullptr);
        }
        state.appendChild (host, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void WavetableMorpherProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.hasType (apvts.state.getType()))
        return;

    auto sources = state.getChildWithName ("SOURCES");
    state.removeChild (sources, nullptr);
    auto host = state.getChildWithName ("HOST");
    state.removeChild (host, nullptr);
    apvts.replaceState (state);

    if (host.isValid())
    {
        vitalOsc = juce::jlimit (0, 2, (int) host.getProperty ("vitalOsc", 0));
        autoSend = (bool) host.getProperty ("autoSend", true);

        const auto path = host.getProperty ("file", "").toString();
        juce::MemoryBlock hostedState;
        hostedState.fromBase64Encoding (host.getProperty ("state", "").toString());

        // Plugin loading must happen on the message thread.
        juce::WeakReference<WavetableMorpherProcessor> weak (this);
        juce::MessageManager::callAsync ([weak, path, hostedState]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;

            if (path.isEmpty() || ! juce::File::isAbsolutePath (path) || ! juce::File (path).exists())
            {
                self->unloadHostedSynth();
                return;
            }

            self->loadHostedSynth (juce::File (path), [weak, path] (juce::Result r)
            {
                if (auto* p = weak.get(); p != nullptr && r.failed() && p->onVitalSent)
                    p->onVitalSent ("Couldn't reload " + juce::File (path).getFileNameWithoutExtension() + ": " + r.getErrorMessage());
            }, &hostedState);
        });
    }

    if (sources.isValid())
    {
        tableName = sources.getProperty ("tableName", tableName).toString();

        for (auto slot : sources)
        {
            const int i = juce::jlimit (0, 1, (int) slot.getProperty ("index", 0));
            auto& s = slots[(size_t) i];
            s.kind = (SourceSlot::Kind) juce::jlimit (0, 2, (int) slot.getProperty ("kind", 0));
            s.shapeIndex = (int) slot.getProperty ("shape", 0);
            s.importedName = slot.getProperty ("importedName", "").toString();
            const auto path = slot.getProperty ("importedFile", "").toString();
            s.importedFile = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
            s.importedFrames.clear();
            s.importedFrame = 0;

            wtm::Cycle c;
            if (wtm::cycleFromBase64 (slot.getProperty ("cycle", "").toString(), c))
                s.cycle = c;
            else
                s.cycle = wtm::makeShape ((wtm::Shape) juce::jlimit (0, (int) wtm::Shape::NumShapes - 1, s.shapeIndex));
        }
    }

    requestRebuild();
    sendChangeMessage();
}

//==============================================================================
juce::Array<juce::File> WavetableMorpherProcessor::findSynthPlugins() const
{
    juce::Array<juce::File> result;
    const juce::File roots[] = {
        juce::File ("/Library/Audio/Plug-Ins/VST3"),
        juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"),
    };

    for (const auto& root : roots)
        for (const auto& f : root.findChildFiles (juce::File::findDirectories, true, "*.vst3"))
            if (! f.getFileName().containsIgnoreCase ("Wavetable Morpher")
                && ! f.getParentDirectory().getFileName().endsWithIgnoreCase (".vst3"))
                result.addIfNotAlreadyThere (f);

    struct Order
    {
        static int rank (const juce::File& f)
        {
            const auto n = f.getFileNameWithoutExtension().toLowerCase();
            if (n.startsWith ("vital")) return 0;
            if (n.startsWith ("serum 2") || n.startsWith ("serum2")) return 1;
            if (n.startsWith ("serum")) return 2;
            return 3;
        }
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            const int ra = rank (a), rb = rank (b);
            if (ra != rb) return ra < rb ? -1 : 1;
            return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension());
        }
    } order;
    result.sort (order);
    return result;
}

void WavetableMorpherProcessor::loadHostedSynth (const juce::File& bundle, std::function<void (juce::Result)> done,
                                                 const juce::MemoryBlock* stateToRestore)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto* format = formatManager.getFormat (0);
    juce::OwnedArray<juce::PluginDescription> types;
    format->findAllTypesForFile (types, bundle.getFullPathName());

    const juce::PluginDescription* chosen = nullptr;
    for (auto* t : types)
        if (t->isInstrument) { chosen = t; break; }

    if (chosen == nullptr)
    {
        if (done) done (juce::Result::fail (types.isEmpty() ? "Couldn't open " + bundle.getFileNameWithoutExtension() + "."
                                                            : bundle.getFileNameWithoutExtension() + " isn't an instrument."));
        return;
    }

    const auto description = *chosen;
    const int generation = ++loadGeneration;
    std::shared_ptr<juce::MemoryBlock> restore = stateToRestore != nullptr ? std::make_shared<juce::MemoryBlock> (*stateToRestore) : nullptr;
    juce::WeakReference<WavetableMorpherProcessor> weak (this);

    formatManager.createPluginInstanceAsync (description, currentSampleRate, currentBlockSize,
        [weak, generation, bundle, description, restore, done] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            auto* self = weak.get();
            if (self == nullptr || generation != self->loadGeneration)
                return; // processor gone, or the user picked something else meanwhile

            if (instance == nullptr)
            {
                if (done) done (juce::Result::fail ("Couldn't load " + description.name + ": " + error));
                return;
            }

            // Only the main stereo output is used.
            auto layout = instance->getBusesLayout();
            for (int i = 1; i < layout.outputBuses.size(); ++i)
                layout.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();
            if (! layout.outputBuses.isEmpty())
                layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            for (auto& in : layout.inputBuses)
                in = juce::AudioChannelSet::disabled();
            instance->setBusesLayout (layout); // if the synth refuses, it keeps its own layout - that's fine

            if (restore != nullptr && restore->getSize() > 0)
                instance->setStateInformation (restore->getData(), (int) restore->getSize());

            self->unloadHostedSynth();

            {
                const juce::ScopedLock l1 (self->lifetimeLock);
                const juce::ScopedLock l2 (self->hostLock);
                self->hosted = std::move (instance);
                self->hostedFile = bundle;
                self->hostedIsVital = description.name.containsIgnoreCase ("vital")
                                      || description.manufacturerName.containsIgnoreCase ("vital");
                self->prepareHosted();
                self->hostedActive = true;
            }

            self->lastSentVersion = restore != nullptr ? self->tableVersion.load() : -1;
            self->heldNotes = 0;
            if (done) done (juce::Result::ok());
            self->sendChangeMessage();
        });
}

void WavetableMorpherProcessor::unloadHostedSynth()
{
    if (hosted == nullptr)
        return;

    if (onHostedSynthWillChange)
        onHostedSynthWillChange(); // closes the synth's window first

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::ScopedLock l1 (lifetimeLock);
        const juce::ScopedLock l2 (hostLock);
        hostedActive = false;
        old = std::move (hosted);
        hostedFile = juce::File();
        hostedIsVital = false;
        setLatencySamples (0);
    }

    old->releaseResources();
    old.reset();
    sendChangeMessage();
}

juce::Result WavetableMorpherProcessor::sendToVital()
{
    if (hosted == nullptr || ! hostedIsVital)
        return juce::Result::fail ("Load Vital first.");

    const int version = tableVersion.load();
    const auto frames = getMorphFrames();

    juce::MemoryBlock current, updated;
    hosted->getStateInformation (current);

    auto r = wtm::vital::injectWavetable (current, frames, vitalOsc, tableName, updated);
    if (r.failed())
        return r;

    {
        const juce::ScopedLock sl (hostLock);
        hosted->setStateInformation (updated.getData(), (int) updated.getSize());
    }

    lastSentVersion = version;
    return juce::Result::ok();
}

juce::Result WavetableMorpherProcessor::writeDragFile (juce::File& result)
{
    const auto name = juce::File::createLegalFileName (tableName.trim().isNotEmpty() ? tableName.trim() : juce::String ("My Morph"));
    result = getDestinationFolder (wtm::Destination::Library).getChildFile ("Dragged").getChildFile (name + ".wav");
    return wtm::writeWavetableWav (result, getMorphFrames());
}

//==============================================================================
juce::AudioProcessorEditor* WavetableMorpherProcessor::createEditor()
{
    return new WavetableMorpherEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WavetableMorpherProcessor();
}
