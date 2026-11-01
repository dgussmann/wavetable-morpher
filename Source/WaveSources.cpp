#include "WaveSources.h"

#include <cmath>

namespace wtm
{
    namespace
    {
        constexpr double twoPi = juce::MathConstants<double>::twoPi;
    }

    juce::StringArray shapeNames()
    {
        return { "Sine", "Triangle", "Saw", "Square", "Pulse 25%", "Pulse 10%",
                 "Half Sine", "Organ", "Formant", "Hollow" };
    }

    Cycle makeSilence()
    {
        return Cycle ((size_t) kFrameSize, 0.0f);
    }

    Cycle makeShape (Shape s)
    {
        Cycle c ((size_t) kFrameSize, 0.0f);

        for (int i = 0; i < kFrameSize; ++i)
        {
            const double p = (double) i / (double) kFrameSize; // 0..1
            const double x = p * twoPi;
            double v = 0.0;

            switch (s)
            {
                case Shape::Sine:      v = std::sin (x); break;
                case Shape::Triangle:  v = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0); break;
                case Shape::Saw:       v = p < 0.5 ? 2.0 * p : 2.0 * p - 2.0; break;           // rises from 0, starts at zero crossing
                case Shape::Square:    v = p < 0.5 ? 1.0 : -1.0; break;
                case Shape::Pulse25:   v = p < 0.25 ? 1.0 : -1.0; break;
                case Shape::Pulse10:   v = p < 0.10 ? 1.0 : -1.0; break;
                case Shape::HalfSine:  v = p < 0.5 ? std::sin (x) * 2.0 - 0.6366 : -0.6366; break;
                case Shape::Organ:     v = std::sin (x) + 0.5 * std::sin (2 * x) + 0.33 * std::sin (3 * x) + 0.25 * std::sin (4 * x) + 0.12 * std::sin (6 * x); break;
                case Shape::Formant:
                {
                    const double window = 0.5 - 0.5 * std::cos (x);
                    v = std::sin (7.0 * x) * window + 0.35 * std::sin (x);
                    break;
                }
                case Shape::Hollow:    v = std::sin (x) + 0.6 * std::sin (3 * x) + 0.45 * std::sin (5 * x) + 0.3 * std::sin (9 * x); break;
                case Shape::NumShapes: break;
            }

            c[(size_t) i] = (float) v;
        }

        removeDC (c);
        normalise (c);
        return c;
    }

    float peakOf (const Cycle& c)
    {
        float peak = 0.0f;
        for (auto v : c)
            peak = std::max (peak, std::abs (v));
        return peak;
    }

    void removeDC (Cycle& c)
    {
        if (c.empty())
            return;

        double sum = 0.0;
        for (auto v : c)
            sum += v;

        const auto mean = (float) (sum / (double) c.size());
        for (auto& v : c)
            v -= mean;
    }

    void normalise (Cycle& c, float targetPeak)
    {
        const auto peak = peakOf (c);
        if (peak < 1.0e-6f)
            return;

        const auto g = targetPeak / peak;
        for (auto& v : c)
            v *= g;
    }

    static Cycle resampleToFrame (const float* data, int numSamples)
    {
        Cycle c ((size_t) kFrameSize, 0.0f);
        if (numSamples <= 0)
            return c;

        for (int i = 0; i < kFrameSize; ++i)
        {
            const double pos = (double) i * (double) numSamples / (double) kFrameSize;
            const int i0 = (int) pos;
            const int i1 = (i0 + 1) % numSamples;
            const auto frac = (float) (pos - i0);
            c[(size_t) i] = data[i0 % numSamples] + frac * (data[i1] - data[i0 % numSamples]);
        }

        return c;
    }

    juce::Result importAudioFile (const juce::File& file, ImportedAudio& out)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
            return juce::Result::fail ("Couldn't read \"" + file.getFileName() + "\". Use a WAV, AIFF or FLAC file.");

        const auto totalSamples = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) kFrameSize * kMaxFrames);
        if (totalSamples <= 0)
            return juce::Result::fail ("\"" + file.getFileName() + "\" is empty.");

        const int numChannels = (int) std::max (1u, reader->numChannels);
        juce::AudioBuffer<float> buffer (numChannels, totalSamples);
        reader->read (&buffer, 0, totalSamples, 0, true, true);

        // Mix to mono.
        std::vector<float> mono ((size_t) totalSamples, 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = buffer.getReadPointer (ch);
            for (int i = 0; i < totalSamples; ++i)
                mono[(size_t) i] += src[i] / (float) numChannels;
        }

        out.frames.clear();
        out.fileName = file.getFileNameWithoutExtension();

        if (totalSamples < kFrameSize)
        {
            // A short single-cycle file (e.g. 600 samples) - stretch it to one frame.
            out.frames.push_back (resampleToFrame (mono.data(), totalSamples));
        }
        else
        {
            const int numFrames = std::min (kMaxFrames, totalSamples / kFrameSize);
            for (int f = 0; f < numFrames; ++f)
            {
                Cycle c (mono.begin() + f * kFrameSize, mono.begin() + (f + 1) * kFrameSize);
                out.frames.push_back (std::move (c));
            }
        }

        for (auto& c : out.frames)
        {
            removeDC (c);
            normalise (c);
        }

        return juce::Result::ok();
    }

    juce::String cycleToBase64 (const Cycle& c)
    {
        juce::MemoryBlock mb (c.data(), c.size() * sizeof (float));
        return mb.toBase64Encoding();
    }

    bool cycleFromBase64 (const juce::String& s, Cycle& out)
    {
        juce::MemoryBlock mb;
        if (! mb.fromBase64Encoding (s) || mb.getSize() != (size_t) kFrameSize * sizeof (float))
            return false;

        out.assign ((size_t) kFrameSize, 0.0f);
        std::memcpy (out.data(), mb.getData(), mb.getSize());
        return true;
    }
}
