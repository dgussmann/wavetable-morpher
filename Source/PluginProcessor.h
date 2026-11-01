#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>

#include "PreviewSynth.h"
#include "WavetableEngine.h"
#include "WavetableExporter.h"
#include "VitalBridge.h"
#include "SerumBridge.h"

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
        juce::File importedFile;                // so the arrows can step to the next file in its folder
        std::vector<wtm::Cycle> importedFrames; // whole imported file (not saved with the session)
        int importedFrame = 0;
        wtm::Cycle cycle;                       // the frame used for the morph

        juce::String describe() const;

        /** The whole wavetable this oscillator holds: every frame of an imported table, else one wave. */
        std::vector<wtm::Cycle> table() const
        {
            if (kind == Kind::Imported && ! importedFrames.empty())
                return importedFrames;
            return { cycle };
        }
        juce::String tableName() const;
    };

    WavetableMorpherProcessor();
    ~WavetableMorpherProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
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

    /** Steps to the previous/next wavetable file in the same folder (or the previous/next built-in shape).
        Returns a short description for the status line. */
    juce::Result stepSource (int slot, int direction, juce::String& description);

    juce::File getLastImportFolder();
    void setLastImportFolder (const juce::File& folder);
    void setDrawnCycle (int slot, const wtm::Cycle& cycle);
    void swapSlots();

    std::vector<wtm::Cycle> getMorphFrames() const;

    /** What the synth (built-in or loaded) plays: Oscillator A's table, the morph, or Oscillator B's table. */
    enum class PlaySource { OscA = 0, Morph = 1, OscB = 2 };
    PlaySource getPlaySource() const noexcept { return playSource; }
    void setPlaySource (PlaySource p);
    std::vector<wtm::Cycle> getPlayFrames() const;
    juce::String getPlayName() const;

    /** Position (0..1) in the playing table. Follows the loaded synth's own wavetable-position knob when it has one. */
    float getPlayPosition() const;
    void setPlayPosition (float position);
    wtm::MorphSettings getMorphSettings() const;
    float getDisplayPosition() const noexcept { return displayPosition.load(); }

    juce::String getTableName() const { return tableName; }
    void setTableName (const juce::String& n) { tableName = n; }

    /** Folder for a destination: the user's choice if they've set one, otherwise auto-detected. */
    juce::File getDestinationFolder (wtm::Destination d);
    void setDestinationFolder (wtm::Destination d, const juce::File& folder);
    bool isDestinationEnabled (wtm::Destination d);
    void setDestinationEnabled (wtm::Destination d, bool enabled);

    //============================================================================== hosted synth (message thread)
    /** VST3 instruments installed on this Mac (Vital and Serum first). */
    juce::Array<juce::File> findSynthPlugins() const;

    /** Loads a VST3 synth to play the wavetable through. `done` is called on the message thread. */
    void loadHostedSynth (const juce::File& bundle, std::function<void (juce::Result)> done,
                          const juce::MemoryBlock* stateToRestore = nullptr);
    void unloadHostedSynth();

    juce::AudioPluginInstance* getHostedSynth() const noexcept { return hosted.get(); }
    bool isRestoringSynth() const noexcept { return restoringSynth || loadingHosted; }
    juce::File getHostedSynthFile() const { return hostedFile; }
    juce::String getHostedSynthName() const { return hosted != nullptr ? hosted->getName() : juce::String(); }
    bool isHostedVital() const noexcept { return hostedIsVital; }
    bool isHostedSerum() const noexcept { return hostedIsSerum; }
    /** Vital and Serum 2 can be given our wavetable automatically. */
    bool canSendToSynth() const noexcept { return hostedIsVital || hostedIsSerum; }
    juce::String getHostedShortName() const { return hostedIsVital ? "Vital" : hostedIsSerum ? "Serum 2" : getHostedSynthName(); }
    juce::String oscillatorLabel (int i) const { return hostedIsSerum ? juce::String::charToString ((juce::juce_wchar) ('A' + i)) : juce::String (i + 1); }

    /** Silence the synth's other oscillators (and noise/sub/sample) so only our table is heard. */
    bool getSolo() const noexcept { return solo; }
    void setSolo (bool s) { solo = s; lastSentVersion = -1; }

    /** Puts the current morph into the chosen oscillator of the loaded Vital. */
    juce::Result sendToSynth();
    int getVitalOscillator() const noexcept { return vitalOsc; }
    void setVitalOscillator (int i) { vitalOsc = juce::jlimit (0, 2, i); lastSentVersion = -1; findHostedFrameParam(); }
    bool getAutoSend() const noexcept { return autoSend; }
    void setAutoSend (bool b) { autoSend = b; }
    bool isVitalUpToDate() const noexcept { return lastSentVersion == tableVersion.load(); }

    /** The wavetable currently in the loaded synth's oscillator 1 (Vital only - Serum 2's patches can't be read). */
    const std::vector<wtm::Cycle>& getSynthTable() const noexcept { return synthTable; }
    juce::String getSynthTableName() const { return synthTableName; }
    juce::String getSynthTableNote() const { return synthTableNote; }
    void pollSynthTable (bool force = false);

    /** Uses the loaded synth's oscillator-1 wavetable as a source for oscillator A or B. */
    void useSynthTable (int slot, int frameIndex);

    /** Saves the loaded synth's current sound - with the playing wavetable - as a preset in that synth's
        own preset browser (Serum 2: .SerumPreset, Vital: .vital). Returns the file written in `written`. */
    juce::Result saveSynthPatch (const juce::String& name, juce::File& written);
    juce::File synthPatchFile (const juce::String& name) const;
    bool getSavePatch() const noexcept { return savePatch; }
    void setSavePatch (bool b) { savePatch = b; }

    /** Writes the current morph to a WAV for dragging onto a synth. */
    juce::Result writeDragFile (juce::File& result);

    /** The editor uses this to close the synth's window before the synth is removed. */
    std::function<void()> onHostedSynthWillChange;
    /** Called (message thread) after Vital was updated automatically, with an error or empty string. */
    std::function<void (const juce::String&)> onVitalSent;

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
    std::vector<wtm::Cycle> jobPlayTable; // empty = play the morph
    wtm::MorphSettings jobSettings;
    bool jobPending = false;
    wtm::MorphSettings lastRequestedSettings;

    // Results (worker -> audio / UI)
    juce::SpinLock tableLock;
    std::shared_ptr<const wtm::PlaybackTable> pendingTable;
    std::shared_ptr<const wtm::PlaybackTable> activeTable;          // audio thread only
    std::vector<std::shared_ptr<const wtm::PlaybackTable>> keepAlive; // worker only: stops the audio thread freeing memory

    mutable juce::CriticalSection displayLock;
    std::vector<wtm::Cycle> displayFrames, playFrames;
    PlaySource playSource = PlaySource::Morph;

    // The loaded synth's wavetable-position parameter for the oscillator we send to (-1 = none found)
    int hostedFrameParam = -1;
    void findHostedFrameParam();

    // Preview synth
    juce::Synthesiser synth;
    wtm::RenderContext renderContext;
    double currentSampleRate = 44100.0;
    double lfoPhase = 0.0;
    float lastPosition = 0.0f;
    std::atomic<float> displayPosition { 0.0f };

    std::unique_ptr<juce::PropertiesFile> settingsFile;

    // Hosted synth
    void prepareHosted();
    void saveDiagnostic (const juce::MemoryBlock& data, const juce::String& name);
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> hosted;
    juce::File hostedFile;
    bool hostedIsVital = false, hostedIsSerum = false;
    bool solo = true, savePatch = true;
    bool restoringSynth = false, loadingHosted = false;
    int serumSendCounter = 0;
    juce::Result sendToVitalNow (const std::vector<wtm::Cycle>& frames);
    juce::Result sendToSerumNow (const std::vector<wtm::Cycle>& frames);
    void applySerumSolo();
    std::atomic<bool> hostedActive { false };
    juce::CriticalSection hostLock;      // audio thread vs. swapping / Vital updates
    juce::CriticalSection lifetimeLock;  // state saving vs. unloading
    juce::AudioBuffer<float> hostBuffer;
    int currentBlockSize = 512;
    bool prepared = false;
    int loadGeneration = 0;

    // Vital auto-send
    int vitalOsc = 0; // which synth oscillator receives our table (Vital 1-3, Serum A-C)
    bool autoSend = true;
    std::atomic<int> tableVersion { 0 };
    int lastSentVersion = -1;
    std::atomic<double> lastTableChangeMs { 0.0 };
    std::atomic<int> heldNotes { 0 };

    // Synth's oscillator 1, for display
    std::vector<wtm::Cycle> synthTable;
    juce::String synthTableName, synthTableNote;
    juce::int64 synthFingerprint = 0;
    int pollCounter = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (WavetableMorpherProcessor)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableMorpherProcessor)
};
