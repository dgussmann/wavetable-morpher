#pragma once

#include "WaveSources.h"

namespace wtm::vital
{
    /** Vital saves its whole patch as JSON text, with every oscillator's wavetable inside
        (settings.wavetables[0..2] -> groups -> components -> keyframes { position 0..256, wave_data }).
        wave_data is base64 of 2048 little-endian float32 samples.

        This takes Vital's current state, replaces one oscillator's wavetable with our frames
        (keeping everything else in the patch), turns that oscillator on, and returns the new state. */
    juce::Result injectWavetable (const juce::MemoryBlock& vitalState,
                                  const std::vector<Cycle>& frames,
                                  int oscillatorIndex,          // 0, 1 or 2
                                  const juce::String& tableName,
                                  juce::MemoryBlock& newState);

    /** For tests and diagnostics: reads back one oscillator's keyframes. */
    juce::Result readWavetable (const juce::MemoryBlock& vitalState, int oscillatorIndex,
                                std::vector<int>& positions, std::vector<Cycle>& frames, juce::String& name);

    constexpr int kVitalFramePositions = 257; // Vital tables run from position 0 to 256
}
