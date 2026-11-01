// Checks the morph engine and the exported file format. Run: WavetableMorpherTests [output-folder]
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/WaveSources.h"
#include "../Source/WavetableEngine.h"
#include "../Source/WavetableExporter.h"
#include "../Source/VitalBridge.h"

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

    // Folder stepping order: natural sort, audio files only
    {
        auto dir = outDir.getChildFile ("step_test");
        dir.deleteRecursively();
        dir.createDirectory();
        MorphSettings s; s.numFrames = 4;
        const auto fr = buildMorph (sine, saw, s);
        for (auto n : { "Table 10", "Table 2", "Table 1" })
            writeWavetableWav (dir.getChildFile (juce::String (n) + ".wav"), fr);
        dir.getChildFile ("notes.txt").replaceWithText ("x");
        const auto files = audioFilesIn (dir);
        check (files.size() == 3 && files[0].getFileName() == "Table 1.wav" && files[1].getFileName() == "Table 2.wav"
                   && files[2].getFileName() == "Table 10.wav",
               "arrow order: Table 1, Table 2, Table 10 (non-audio skipped)");
    }

    // 256-frame spectral build stays fast enough to feel instant
    {
        MorphSettings s; s.numFrames = 256; s.mode = MorphMode::Spectral;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        PlaybackTable t (buildMorph (sine, saw, s));
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::cout << "  (256-frame spectral build + playback tables: " << (int) ms << " ms)" << std::endl;
        check (t.getNumFrames() == 256, "256-frame table builds");
    }

    // Vital: write a morph into a mock Vital patch and read it back
    {
        auto makeTable = [&] (const juce::String& name, const Cycle& c)
        {
            return "{\"author\":\"\",\"full_normalize\":true,\"groups\":[{\"components\":[{\"interpolation\":1,"
                   "\"interpolation_style\":1,\"keyframes\":[{\"position\":0,\"wave_data\":\""
                   + juce::Base64::toBase64 (c.data(), c.size() * sizeof (float))
                   + "\"}],\"type\":\"Wave Source\"}]}],\"name\":\"" + name + "\",\"remove_all_dc\":true,\"version\":\"1.5.5\"}";
        };
        const auto json = "{\"author\":\"me\",\"preset_name\":\"Test\",\"settings\":{\"osc_1_on\":1.0,\"osc_2_on\":0.0,"
                          "\"filter_1_cutoff\":60.25,\"wavetables\":["
                          + makeTable ("Init", saw) + "," + makeTable ("Other", sine) + "," + makeTable ("Third", sine)
                          + "]},\"synth_version\":\"1.5.5\"}";
        juce::MemoryBlock state;
        state.append (json.toRawUTF8(), json.getNumBytesAsUTF8());
        state.append ("\0", 1);

        MorphSettings ms; ms.numFrames = 256;
        const auto frames = buildMorph (sine, saw, ms);
        juce::MemoryBlock out;
        const auto r = vital::injectWavetable (state, frames, 1, "Test Morph", out);
        check (r.wasOk(), "inject into Vital osc 2: " + r.getErrorMessage());

        std::vector<int> pos; std::vector<Cycle> got; juce::String name;
        check (vital::readWavetable (out, 1, pos, got, name).wasOk() && got.size() == 256, "Vital osc 2 now holds 256 keyframes");
        check (name == "Test Morph", "Vital table named after your wavetable");
        check (! pos.empty() && pos.front() == 0 && pos.back() == 256, "keyframes span Vital positions 0..256");
        bool increasing = true; for (size_t i = 1; i < pos.size(); ++i) increasing &= pos[i] > pos[i - 1];
        check (increasing, "keyframe positions strictly increase");
        check (got.size() == 256 && maxDiff (got[0], frames[0]) < 1e-7f && maxDiff (got[255], frames[255]) < 1e-7f, "frames survive exactly");

        std::vector<int> p0; std::vector<Cycle> g0; juce::String n0;
        vital::readWavetable (out, 0, p0, g0, n0);
        check (n0 == "Init" && g0.size() == 1 && maxDiff (g0[0], saw) < 1e-7f, "other oscillators untouched");

        juce::var root;
        const auto text = juce::String::fromUTF8 (static_cast<const char*> (out.getData()), (int) out.getSize() - 1);
        check (juce::JSON::parse (text, root).wasOk(), "result is valid JSON");
        check ((double) root["settings"]["osc_2_on"] == 1.0, "target oscillator switched on");
        check (std::abs ((double) root["settings"]["filter_1_cutoff"] - 60.25) < 1e-9, "rest of the patch kept");
        check (root["preset_name"].toString() == "Test" && static_cast<const char*> (out.getData())[out.getSize() - 1] == 0,
               "preset name kept, trailing null kept");

        juce::MemoryBlock bad;
        bad.append ("nonsense", 8);
        check (vital::injectWavetable (bad, frames, 0, "x", out).failed(), "non-Vital data is refused safely");
    }

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURES") << std::endl;
    return failures == 0 ? 0 : 1;
}
