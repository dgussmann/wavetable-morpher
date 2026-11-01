#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace wtm
{
    /** Every synth we export to (Serum 2, Vital, Massive XL) uses 2048 samples per frame. */
    constexpr int kFrameSize = 2048;
    constexpr int kMaxFrames = 256;

    using Cycle = std::vector<float>; // always kFrameSize samples

    /** Built-in starting shapes for an oscillator slot. */
    enum class Shape
    {
        Sine = 0,
        Triangle,
        Saw,
        Square,
        Pulse25,
        Pulse10,
        HalfSine,
        Organ,
        Formant,
        Hollow,
        NumShapes
    };

    juce::StringArray shapeNames();
    Cycle makeShape (Shape s);
    Cycle makeSilence();

    /** Audio loaded from disk, split into 2048-sample frames. */
    struct ImportedAudio
    {
        std::vector<Cycle> frames;
        juce::String fileName;
    };

    /** Loads WAV/AIFF/FLAC. Files shorter than one frame are treated as a single cycle and
        resampled to 2048 samples; longer files are cut into consecutive 2048-sample frames
        (the same rule Serum, Vital and Massive XL use). */
    juce::Result importAudioFile (const juce::File& file, ImportedAudio& out);

    void removeDC (Cycle& c);
    void normalise (Cycle& c, float targetPeak = 1.0f);
    float peakOf (const Cycle& c);

    juce::String cycleToBase64 (const Cycle& c);
    bool cycleFromBase64 (const juce::String& s, Cycle& out);
}
