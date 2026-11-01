#pragma once

#include "WaveSources.h"
#include <memory>

namespace wtm
{
    enum class MorphMode
    {
        Crossfade = 0, // sample-by-sample blend: simple, can thin out in the middle
        Spectral,      // blends each harmonic's level and phase: smoother, Serum-style "spectral morph"
        NumModes
    };

    enum class MorphCurve
    {
        Linear = 0,
        EaseInOut,
        Exponential, // stays near A longer, then rushes to B
        Logarithmic, // leaves A quickly, settles slowly into B
        NumCurves
    };

    juce::StringArray morphModeNames();
    juce::StringArray morphCurveNames();
    juce::StringArray frameCountNames();
    int frameCountForIndex (int index);

    struct MorphSettings
    {
        int numFrames = 64;
        MorphMode mode = MorphMode::Spectral;
        MorphCurve curve = MorphCurve::Linear;
        bool normaliseFrames = true;
    };

    /** Shapes a 0..1 morph amount with the chosen curve. */
    float applyCurve (float t, MorphCurve curve);

    /** Builds the wavetable: frame 0 is A, the last frame is B, everything between morphs. */
    std::vector<Cycle> buildMorph (const Cycle& a, const Cycle& b, const MorphSettings& settings);

    /** Band-limited copies of every frame (one per octave) so the preview synth doesn't alias. */
    class PlaybackTable
    {
    public:
        static constexpr int numLevels = 11;    // level 0 = up to 1023 harmonics ... level 10 = fundamental only
        static constexpr int stride = kFrameSize + 1; // one guard sample for interpolation

        explicit PlaybackTable (const std::vector<Cycle>& frames);

        int getNumFrames() const noexcept { return numFrames; }
        const float* getFrame (int level, int frame) const noexcept
        {
            return data.data() + ((size_t) level * (size_t) numFrames + (size_t) frame) * (size_t) stride;
        }

        /** The highest-quality level that won't alias at this note frequency. */
        static int levelForFrequency (double freqHz, double sampleRate) noexcept;

    private:
        int numFrames = 0;
        std::vector<float> data;
    };
}
