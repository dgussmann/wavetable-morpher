#pragma once

#include "WaveSources.h"

namespace wtm::vital
{
    /** Vital saves its patch as JSON text with every oscillator's wavetable inside
        (settings.wavetables[0..2] -> groups -> components -> keyframes { position 0..256, wave_data }),
        wave_data being base64 of 2048 little-endian float32 samples.

        When Vital runs inside this plugin, the state we get is wrapped twice:
          1. by JUCE's VST3 host: "VC2!" + XML <VST3PluginState><IComponent>base64</IComponent>...
          2. by Vital's own VST3 wrapper: optionally a VST2-style header ("VstW"/"CcnK"...) before the
             JSON and a "JUCEPrivateData" trailer after it.
        These functions accept the state in any of those forms (or bare JSON) and give it back in the
        same form, with the size fields in the headers corrected. */

    /** Replaces one oscillator's wavetable with our frames, keeping everything else in the patch,
        and turns that oscillator on. */
    juce::Result injectWavetable (const juce::MemoryBlock& state,
                                  const std::vector<Cycle>& frames,
                                  int oscillatorIndex,          // 0, 1 or 2
                                  const juce::String& tableName,
                                  juce::MemoryBlock& newState);

    /** Raw keyframes of one oscillator (for tests). */
    juce::Result readWavetable (const juce::MemoryBlock& state, int oscillatorIndex,
                                std::vector<int>& positions, std::vector<Cycle>& frames, juce::String& name);

    /** What an oscillator's wavetable looks like, for display: 257 frames (Vital's positions 0..256)
        built from its drawn/imported waves. `note` explains anything that couldn't be shown
        (e.g. tables made from audio files or line shapes, or modifiers). */
    juce::Result renderWavetable (const juce::MemoryBlock& state, int oscillatorIndex,
                                  std::vector<Cycle>& frames, juce::String& name, juce::String& note);

    /** A fingerprint of the patch's wavetable section, to cheaply detect changes. */
    juce::int64 wavetableFingerprint (const juce::MemoryBlock& state, int oscillatorIndex);

    constexpr int kVitalFramePositions = 257;
}
