// Checks the morph engine and the exported file format. Run: WavetableMorpherTests [output-folder]
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/WaveSources.h"
#include "../Source/WavetableEngine.h"
#include "../Source/WavetableExporter.h"

using namespace wtm;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

static float maxDiff (const Cycle& a, const Cycle& b)
{
    float d = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

int main (int argc, char** argv)
{
    const juce::File outDir = argc > 1 ? juce::File (argv[1]) : juce::File::getCurrentWorkingDirectory();

    const auto sine = makeShape (Shape::Sine);
    const auto saw = makeShape (Shape::Saw);
    check ((int) sine.size() == kFrameSize && std::abs (peakOf (sine) - 1.0f) < 1e-4f, "shapes are 2048 samples, normalised");

    for (int mode = 0; mode < (int) MorphMode::NumModes; ++mode)
    {
        for (int curve = 0; curve < (int) MorphCurve::NumCurves; ++curve)
        {
            MorphSettings s;
            s.numFrames = 64;
            s.mode = (MorphMode) mode;
            s.curve = (MorphCurve) curve;
            const auto frames = buildMorph (sine, saw, s);
            const auto label = morphModeNames()[mode] + " / " + morphCurveNames()[curve];
            check ((int) frames.size() == 64, label + ": 64 frames");
            check (maxDiff (frames.front(), sine) < 1e-3f && maxDiff (frames.back(), saw) < 1e-3f, label + ": ends are exactly A and B");

            bool inRange = true;
            for (auto& f : frames) inRange &= peakOf (f) <= 1.0001f;
            check (inRange, label + ": every frame within -1..1");
        }
    }

    // Spectral morph of a wave with itself must give the same wave back (FFT round trip).
    {
        MorphSettings s; s.numFrames = 8; s.mode = MorphMode::Spectral; s.normaliseFrames = false;
        const auto sq = makeShape (Shape::Square);
        const auto frames = buildMorph (sq, sq, s);
        check (maxDiff (frames[3], frames[0]) < 0.12f, "spectral morph of identical waves stays the same (within band-limit ripple)");
    }

    // Band-limited playback levels
    {
        MorphSettings s; s.numFrames = 16;
        PlaybackTable t (buildMorph (sine, saw, s));
        check (t.getNumFrames() == 16, "playback table has all frames");
        check (PlaybackTable::levelForFrequency (20.0, 48000.0) == 0, "low notes use the full-bandwidth table");
        check (PlaybackTable::levelForFrequency (4000.0, 48000.0) > 5, "high notes use a filtered table (no aliasing)");
        const float* top = t.getFrame (PlaybackTable::numLevels - 1, 15);
        float pk = 0; for (int i = 0; i < kFrameSize; ++i) pk = std::max (pk, std::abs (top[i]));
        check (pk > 0.1f && pk < 1.5f, "fundamental-only level is a sensible sine");
    }

    // Export + re-import
    for (int n : { 8, 256 })
    {
        MorphSettings s; s.numFrames = n;
        const auto frames = buildMorph (sine, saw, s);
        const auto file = outDir.getChildFile ("test_" + juce::String (n) + ".wav");
        const auto r = writeWavetableWav (file, frames);
        check (r.wasOk(), "write " + juce::String (n) + "-frame WAV: " + r.getErrorMessage());

        juce::MemoryBlock mb;
        file.loadFileAsData (mb);
        const std::string raw (static_cast<const char*> (mb.getData()), std::min<size_t> (mb.getSize(), 200));
        check (raw.rfind ("RIFF", 0) == 0 && raw.find ("WAVE") == 8, "RIFF/WAVE header");
        check (raw.find ("clm ") != std::string::npos && raw.find ("<!>2048") != std::string::npos, "has Serum/Vital clm frame-size marker");

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        check (reader != nullptr, "file opens in a standard WAV reader");
        if (reader)
        {
            check (reader->numChannels == 1 && reader->bitsPerSample == 32 && reader->usesFloatingPointData, "mono 32-bit float");
            check (reader->lengthInSamples == (juce::int64) n * kFrameSize, "length = frames x 2048");
        }

        ImportedAudio back;
        check (importAudioFile (file, back).wasOk() && (int) back.frames.size() == n, "re-imports as " + juce::String (n) + " frames");
    }

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURES") << std::endl;
    return failures == 0 ? 0 : 1;
}
