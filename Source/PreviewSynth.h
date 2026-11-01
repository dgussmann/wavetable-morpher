#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "WavetableEngine.h"

namespace wtm
{
    /** Values shared by all voices for the current audio block. Set by the processor before rendering. */
    struct RenderContext
    {
        const PlaybackTable* table = nullptr;
        float positionStart = 0.0f; // morph position at the start of the block (0..1)
        float positionEnd = 0.0f;   // ...and at the end, so scanning is smooth
        float gain = 0.5f;
        juce::ADSR::Parameters envelope;
    };

    class WavetableSound : public juce::SynthesiserSound
    {
    public:
        bool appliesToNote (int) override    { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    /** One preview-synth voice: reads the morphing wavetable at the note's pitch. */
    class WavetableVoice : public juce::SynthesiserVoice
    {
    public:
        explicit WavetableVoice (const RenderContext& ctx) : context (ctx) {}

        bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<WavetableSound*> (s) != nullptr; }

        void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int pitchWheel) override;
        void stopNote (float velocity, bool allowTailOff) override;
        void pitchWheelMoved (int newValue) override;
        void controllerMoved (int, int) override {}
        void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override;

    private:
        void updateFrequency();

        const RenderContext& context;
        juce::ADSR adsr;
        int noteNumber = 60;
        double bendSemitones = 0.0;
        double phase = 0.0, phaseIncrement = 0.0;
        int level = 0;
        float noteGain = 0.0f;
    };
}
