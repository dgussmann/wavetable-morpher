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

    worker = std::make_unique<Worker> (*this);
    lastRequestedSettings = getMorphSettings();
    requestRebuild();
    worker->startThread();

    startTimerHz (10);
}

WavetableMorpherProcessor::~WavetableMorpherProcessor()
{
    stopTimer();
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
                                                        wtm::frameCountNames(), 3));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::morphMode, 1 }, "Morph Mode",
                                                        wtm::morphModeNames(), 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::curve, 1 }, "Morph Curve",
                                                        wtm::morphCurveNames(), 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::normalise, 1 }, "Normalize Frames", true));

    return layout;
}

//==============================================================================
void WavetableMorpherProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);
    keyboardState.reset();
    lfoPhase = 0.0;
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
    s.importedFrames = std::move (imported.frames);
    s.importedFrame = 0;
    s.cycle = s.importedFrames.front();
    requestRebuild();
    return r;
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
        slot.setProperty ("cycle", wtm::cycleToBase64 (s.cycle), nullptr);
        sources.appendChild (slot, nullptr);
    }

    state.appendChild (sources, nullptr);

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
    apvts.replaceState (state);

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
juce::AudioProcessorEditor* WavetableMorpherProcessor::createEditor()
{
    return new WavetableMorpherEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WavetableMorpherProcessor();
}
