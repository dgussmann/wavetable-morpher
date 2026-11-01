#include "WavetableEngine.h"

#include <cmath>
#include <complex>

namespace wtm
{
    using Complex = std::complex<double>;

    namespace
    {
        constexpr double pi = juce::MathConstants<double>::pi;

        /** In-place iterative radix-2 FFT. inverse=true gives the unscaled inverse. */
        void fft (std::vector<Complex>& a, bool inverse)
        {
            const size_t n = a.size();

            for (size_t i = 1, j = 0; i < n; ++i)
            {
                size_t bit = n >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap (a[i], a[j]);
            }

            for (size_t len = 2; len <= n; len <<= 1)
            {
                const double angle = 2.0 * pi / (double) len * (inverse ? 1.0 : -1.0);
                const Complex wlen (std::cos (angle), std::sin (angle));

                for (size_t i = 0; i < n; i += len)
                {
                    Complex w (1.0, 0.0);
                    for (size_t j = 0; j < len / 2; ++j)
                    {
                        const auto u = a[i + j];
                        const auto v = a[i + j + len / 2] * w;
                        a[i + j] = u + v;
                        a[i + j + len / 2] = u - v;
                        w *= wlen;
                    }
                }
            }
        }

        std::vector<Complex> spectrumOf (const Cycle& c)
        {
            std::vector<Complex> s ((size_t) kFrameSize);
            for (size_t i = 0; i < (size_t) kFrameSize; ++i)
                s[i] = Complex (c[i], 0.0);
            fft (s, false);
            return s;
        }

        /** Rebuilds a real cycle from bins 1..maxHarmonic (DC and everything above dropped). */
        void cycleFromSpectrum (const std::vector<Complex>& spectrum, int maxHarmonic, float* dest)
        {
            const int half = kFrameSize / 2;
            maxHarmonic = juce::jlimit (1, half - 1, maxHarmonic);

            std::vector<Complex> s ((size_t) kFrameSize, Complex (0.0, 0.0));
            for (int k = 1; k <= maxHarmonic; ++k)
            {
                s[(size_t) k] = spectrum[(size_t) k];
                s[(size_t) (kFrameSize - k)] = std::conj (spectrum[(size_t) k]);
            }

            fft (s, true);
            for (int i = 0; i < kFrameSize; ++i)
                dest[i] = (float) (s[(size_t) i].real() / (double) kFrameSize);
        }

        double wrapPhase (double p)
        {
            while (p > pi)  p -= 2.0 * pi;
            while (p < -pi) p += 2.0 * pi;
            return p;
        }
    }

    juce::StringArray morphModeNames()  { return { "Crossfade", "Spectral" }; }
    juce::StringArray morphCurveNames() { return { "Linear", "Ease In-Out", "Exponential", "Logarithmic" }; }
    juce::StringArray frameCountNames() { return { "8", "16", "32", "64", "128", "256" }; }

    int frameCountForIndex (int index)
    {
        static const int counts[] = { 8, 16, 32, 64, 128, 256 };
        return counts[juce::jlimit (0, 5, index)];
    }

    float applyCurve (float t, MorphCurve curve)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        switch (curve)
        {
            case MorphCurve::EaseInOut:   return t * t * (3.0f - 2.0f * t);
            case MorphCurve::Exponential: return t * t * t;
            case MorphCurve::Logarithmic: return 1.0f - std::pow (1.0f - t, 3.0f);
            case MorphCurve::Linear:
            case MorphCurve::NumCurves:   break;
        }
        return t;
    }

    std::vector<Cycle> buildMorph (const Cycle& a, const Cycle& b, const MorphSettings& settings)
    {
        const int numFrames = juce::jlimit (2, kMaxFrames, settings.numFrames);
        std::vector<Cycle> frames ((size_t) numFrames, Cycle ((size_t) kFrameSize, 0.0f));

        std::vector<Complex> specA, specB;
        if (settings.mode == MorphMode::Spectral)
        {
            specA = spectrumOf (a);
            specB = spectrumOf (b);
        }

        const int half = kFrameSize / 2;

        for (int f = 0; f < numFrames; ++f)
        {
            const float t = applyCurve ((float) f / (float) (numFrames - 1), settings.curve);
            auto& out = frames[(size_t) f];

            if (settings.mode == MorphMode::Crossfade)
            {
                for (size_t i = 0; i < (size_t) kFrameSize; ++i)
                    out[i] = a[i] + t * (b[i] - a[i]);
            }
            else
            {
                std::vector<Complex> mixed ((size_t) kFrameSize, Complex (0.0, 0.0));
                const double maxMag = (double) kFrameSize * 1.0e-6;

                for (int k = 1; k < half; ++k)
                {
                    const auto ca = specA[(size_t) k];
                    const auto cb = specB[(size_t) k];
                    const double magA = std::abs (ca), magB = std::abs (cb);
                    const double mag = magA + (double) t * (magB - magA);

                    double phase;
                    if (magA < maxMag)      phase = std::arg (cb);
                    else if (magB < maxMag) phase = std::arg (ca);
                    else                    phase = std::arg (ca) + (double) t * wrapPhase (std::arg (cb) - std::arg (ca));

                    mixed[(size_t) k] = std::polar (mag, phase);
                }

                cycleFromSpectrum (mixed, half - 1, out.data());
            }

            // The end frames should be exactly the sources the user picked.
            if (f == 0)              out = a;
            if (f == numFrames - 1)  out = b;

            removeDC (out);
            if (settings.normaliseFrames)
                normalise (out);
        }

        if (! settings.normaliseFrames)
        {
            // Still keep the whole table inside -1..1 so nothing clips on export.
            float peak = 0.0f;
            for (auto& fr : frames)
                peak = std::max (peak, peakOf (fr));
            if (peak > 1.0f)
                for (auto& fr : frames)
                    for (auto& v : fr)
                        v /= peak;
        }

        return frames;
    }

    //==============================================================================
    static int maxHarmonicForLevel (int level)
    {
        return level == 0 ? (kFrameSize / 2 - 1) : std::max (1, (kFrameSize / 2) >> level);
    }

    PlaybackTable::PlaybackTable (const std::vector<Cycle>& frames)
        : numFrames ((int) frames.size())
    {
        data.assign ((size_t) numLevels * (size_t) numFrames * (size_t) stride, 0.0f);

        for (int f = 0; f < numFrames; ++f)
        {
            const auto spectrum = spectrumOf (frames[(size_t) f]);

            for (int level = 0; level < numLevels; ++level)
            {
                auto* dest = const_cast<float*> (getFrame (level, f));
                cycleFromSpectrum (spectrum, maxHarmonicForLevel (level), dest);
                dest[kFrameSize] = dest[0]; // guard sample
            }
        }
    }

    int PlaybackTable::levelForFrequency (double freqHz, double sampleRate) noexcept
    {
        if (freqHz <= 0.0 || sampleRate <= 0.0)
            return 0;

        const double allowed = 0.5 * sampleRate / freqHz;
        for (int level = 0; level < numLevels; ++level)
            if ((double) maxHarmonicForLevel (level) <= allowed)
                return level;

        return numLevels - 1;
    }
}
