#pragma once

#include "WaveSources.h"

namespace wtm
{
    /** Writes a 32-bit float mono WAV with 2048-sample frames and a "clm " chunk
        ("<!>2048 ...") - the marker Serum and Vital read to detect the frame size.
        Massive XL simply slices any audio into 2048-sample blocks, so the same file works there. */
    juce::Result writeWavetableWav (const juce::File& file, const std::vector<Cycle>& frames, int sampleRate = 44100);

    /** Where a saved wavetable can go. */
    enum class Destination
    {
        Library = 0, // the plugin's own folder: ~/Music/Wavetable Morpher
        Serum2,
        Vital,
        MassiveXL,
        NumDestinations
    };

    juce::String destinationName (Destination d);
    juce::String destinationKey (Destination d); // used in the settings file

    /** The folder this destination would use if the user hasn't chosen one.
        Returns an empty File when the synth's folder couldn't be found on this Mac. */
    juce::File detectDefaultFolder (Destination d);

    /** Extra advice shown if a write fails or no folder is found. */
    juce::String destinationHelp (Destination d);
}
