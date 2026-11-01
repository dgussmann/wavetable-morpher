#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>

#include "PreviewSynth.h"
#include "WavetableEngine.h"
#include "WavetableExporter.h"

namespace ParamIDs
{
    inline constexpr const char* position  = "position";
    inline constexpr const char* scanRate  = "scanRate";
    inline constexpr const char* scanDepth = "scanDepth";
    inline constexpr const char* attack    = "attack";
    inline constexpr const char* decay     = "decay";
    inline constexpr const char* sustain   = "sustain";
    inline constexpr const char* release   = "release";
    inline constexpr const char* volume    = "volume";
    inline constexpr const char* frames    = "frames";
    inline constexpr const char* morphMode = "morphMode";
    inline constexpr const char* curve     = "curve";
    inline constexpr const char* normalise = "normalise";
}

class WavetableMorpherProcessor : public juce::AudioProcessor,
                                  public juce::ChangeBroadcaster,
                                  private juce::Timer
{
public:
    /** One of the two oscillators being morphed. */
    struct SourceSlot
    {
        enum class Kind { Shape, Imported, Drawn };

        Kind kind = Kind::Shape;
        int shapeIndex = 0;
        juce::String importedName;
        std::vector<wtm::Cycle> importedFrames; // whole imported file (not saved with the session)
        int importedFrame = 0;
        wtm::Cycle cycle;                       // the waveform actually used

        juce::String describe() const;
    };

    WavetableMorpherProcessor();
    ~WavetableMorpherProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //============================================================================== message thread
    const SourceSlot& getSlot (int slot) const { return slots[(size_t) juce::jlimit (0, 1, slot)]; }
    void setShape (int slot, int shapeIndex);
    juce::Result importFile (int slot, const juce::File& file);
    void setImportedFrame (int slot, int frame);
    void setDrawnCycle (int slot, const wtm::Cycle& cycle);
    void swapSlots();

    std::vector<wtm::Cycle> getMorphFrames() const;
    wtm::MorphSettings getMorphSettings() const;
    float getDisplayPosition() const noexcept { return displayPosition.load(); }

    juce::String getTableName() const { return tableName; }
    void setTableName (const juce::String& n) { tableName = n; }

    /** Folder for a destination: the user's choice if they've set one, otherwise auto-detected. */
    juce::File getDestinationFolder (wtm::Destination d);
    void setDestinationFolder (wtm::Destination d, const juce::File& folder);
    bool isDestinationEnabled (wtm::Destination d);
    void setDestinationEnabled (wtm::Destination d, bool enabled);

    juce::MidiKeyboardState keyboardState;
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void requestRebuild();
    void rebuildWorker();
    juce::PropertiesFile& settings();

    class Worker;
    std::unique_ptr<Worker> worker;

    std::array<SourceSlot, 2> slots;
    juce::String tableName { "My Morph" };

    // Rebuild job hand-off (message thread -> worker)
    juce::CriticalSection jobLock;
    wtm::Cycle jobA, jobB;
    wtm::MorphSettings jobSettings;
    bool jobPending = false;
    wtm::MorphSettings lastRequestedSettings;

    // Results (worker -> audio / UI)
    juce::SpinLock tableLock;
    std::shared_ptr<const wtm::PlaybackTable> pendingTable;
    std::shared_ptr<const wtm::PlaybackTable> activeTable;          // audio thread only
    std::vector<std::shared_ptr<const wtm::PlaybackTable>> keepAlive; // worker only: stops the audio thread freeing memory

    mutable juce::CriticalSection displayLock;
    std::vector<wtm::Cycle> displayFrames;

    // Preview synth
    juce::Synthesiser synth;
    wtm::RenderContext renderContext;
    double currentSampleRate = 44100.0;
    double lfoPhase = 0.0;
    float lastPosition = 0.0f;
    std::atomic<float> displayPosition { 0.0f };

    std::unique_ptr<juce::PropertiesFile> settingsFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableMorpherProcessor)
};
