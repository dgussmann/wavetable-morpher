#pragma once

#include <juce_core/juce_core.h>

/** Serum 2 keeps its patch as  "XferJson\0" + u64 length + JSON header (with an MD5 "hash")
    + u32 uncompressed size + u32 version + a zstd-compressed CBOR document.

    Its wavetable oscillators don't embed audio: they reference a WAV inside Serum's Tables folder:
        Oscillator<N>/WTOsc<N>/relativePathToWT   e.g. "Analog/Acid.wav"
        Oscillator<N>/WTOsc<N>/numFrames          total samples (frames x 2048)
    So to make Serum play our table we save it into the Tables folder and point the oscillator at it. */
namespace wtm::serum
{
    /** Points oscillator `osc` (0 = A, 1 = B, 2 = C) at `relativePath` (relative to Serum's Tables folder). */
    juce::Result setOscillatorWavetable (const juce::MemoryBlock& state, int osc,
                                         const juce::String& relativePath, int numSamples,
                                         juce::MemoryBlock& newState);

    /** Which table an oscillator uses. */
    juce::Result getOscillatorWavetable (const juce::MemoryBlock& state, int osc,
                                         juce::String& relativePath, int& numSamples);

    /** A .SerumPreset file made from Serum's current state: oscillator `osc` points at `relativePath`
        and the preset is named `presetName`. Everything else (filters, FX, modulation...) is kept. */
    juce::Result makePreset (const juce::MemoryBlock& state, int osc, const juce::String& relativePath, int numSamples,
                             const juce::String& presetName, juce::MemoryBlock& presetFile);

    /** The name of the preset currently loaded in Serum (empty if none). */
    juce::String getPresetName (const juce::MemoryBlock& state);

    /** Whether this looks like Serum 2 data at all. */
    bool looksLikeSerumState (const juce::MemoryBlock& state);
}
