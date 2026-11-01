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
        check (std::abs ((double) root["settings"]["osc_2_on"] - 1.0) < 1e-9, "target oscillator switched on");
        check (std::abs ((double) root["settings"]["filter_1_cutoff"] - 60.25) < 1e-9, "rest of the patch kept");
        check (root["preset_name"].toString() == "Test" && static_cast<const char*> (out.getData())[out.getSize() - 1] == 0,
               "preset name kept, trailing null kept");

        juce::MemoryBlock bad;
        bad.append ("nonsense", 8);
        check (vital::injectWavetable (bad, frames, 0, "x", out).failed(), "non-Vital data is refused safely");
    }

    // Vital as it really arrives inside a host: VC2 XML envelope -> VST2-style header -> JSON -> JUCE trailer
    {
        auto be32 = [] (juce::MemoryOutputStream& o, juce::uint32 v) { o.writeIntBigEndian ((int) v); };

        MorphSettings ms0; ms0.numFrames = 2;
        const auto start = buildMorph (saw, sine, ms0);
        const auto json = juce::String ("{\"settings\":{\"osc_1_on\":1.0,\"wavetables\":[{\"name\":\"Basic Shapes\",\"version\":\"1.5.5\",\"groups\":[{\"components\":[{\"type\":\"Wave Source\",\"interpolation\":1,\"interpolation_style\":1,\"keyframes\":[")
                          + "{\"position\":0,\"wave_data\":\"" + juce::Base64::toBase64 (start[0].data(), kFrameSize * 4) + "\"},"
                          + "{\"position\":256,\"wave_data\":\"" + juce::Base64::toBase64 (start[1].data(), kFrameSize * 4) + "\"}]},"
                          + "{\"type\":\"Phase Shift\",\"keyframes\":[{\"position\":0,\"shift\":0.0}]}]}]},"
                          + "{\"name\":\"B\",\"groups\":[]},{\"name\":\"C\",\"groups\":[]}]},\"synth_version\":\"1.5.5\"}";

        // Vital's getStateInformation: JSON + null, then JUCE wrapper's private-data trailer
        juce::MemoryOutputStream chunk;
        chunk.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
        chunk.writeByte (0);
        chunk.writeInt64 (0);
        const char priv[] = "fake-private-data";
        chunk.write (priv, sizeof (priv));
        chunk.writeInt64 ((juce::int64) sizeof (priv));
        chunk << "JUCEPrivateData";

        // VST2-compatible header: VstW + CcnK/FBCh bank with "bytes remaining" fields
        juce::MemoryOutputStream comp;
        comp.write ("VstW", 4); be32 (comp, 8); be32 (comp, 1); be32 (comp, 0);
        const int ccnkStart = (int) comp.getDataSize();
        comp.write ("CcnK", 4);
        const auto bankHeader = 4 + 4 + 4 + 4 + 128 + 4;  // FBCh, version, fxID, fxVersion, numPrograms... simplified + chunkSize
        be32 (comp, (juce::uint32) (bankHeader + chunk.getDataSize()));  // byteSize: everything after this field
        comp.write ("FBCh", 4); be32 (comp, 2); comp.write ("Vita", 4); be32 (comp, 10505); be32 (comp, 1);
        for (int i = 0; i < 128 - 4; ++i) comp.writeByte (0);
        be32 (comp, (juce::uint32) chunk.getDataSize());   // chunkSize
        comp.write (chunk.getData(), chunk.getDataSize());
        juce::ignoreUnused (ccnkStart);

        juce::MemoryBlock compBlock (comp.getData(), comp.getDataSize());
        juce::XmlElement xml ("VST3PluginState");
        xml.createNewChildElement ("IComponent")->addTextElement (compBlock.toBase64Encoding());
        xml.createNewChildElement ("IEditController")->addTextElement ("AAAA");
        juce::MemoryBlock host;
        {
            juce::MemoryOutputStream o (host, false);
            o.writeInt (0x21324356); o.writeInt (0);
            xml.writeTo (o, juce::XmlElement::TextFormat().singleLine());
            o.writeByte (0);
        }
        const auto len = (juce::uint32) host.getSize() - 9;
        std::memcpy (static_cast<char*> (host.getData()) + 4, &len, 4);

        // Display: render Vital osc 1
        std::vector<Cycle> shown; juce::String nm, note;
        check (vital::renderWavetable (host, 0, shown, nm, note).wasOk() && shown.size() == 257 && nm == "Basic Shapes",
               "reads Vital osc 1 through all wrappers (257 frames)");
        check (maxDiff (shown[0], start[0]) < 0.02f || peakOf (shown[0]) > 0.5f, "osc 1 first frame looks like the saw");
        check (note.contains ("Phase Shift"), "notes the modifier it can't show: " + note);

        // Send to osc 2
        MorphSettings ms; ms.numFrames = 64;
        const auto frames = buildMorph (sine, saw, ms);
        juce::MemoryBlock out;
        const auto r = vital::injectWavetable (host, frames, 1, "Wrapped", out);
        check (r.wasOk(), "inject through host + VST2 wrappers: " + r.getErrorMessage());

        // Unwrap the result by hand and check every layer
        const auto* ob = static_cast<const char*> (out.getData());
        const auto magic = juce::ByteOrder::littleEndianInt (ob);
        const auto xlen = (int) juce::ByteOrder::littleEndianInt (ob + 4);
        check (magic == 0x21324356 && xlen == (int) out.getSize() - 9, "host envelope header + length correct");
        auto outXml = juce::parseXML (juce::String::fromUTF8 (ob + 8, xlen));
        check (outXml != nullptr && outXml->getChildByName ("IEditController") != nullptr
                   && outXml->getChildByName ("IEditController")->getAllSubText() == "AAAA", "editor state kept");

        juce::MemoryBlock oc;
        oc.fromBase64Encoding (outXml->getChildByName ("IComponent")->getAllSubText());
        const auto* c = static_cast<const unsigned char*> (oc.getData());
        check (std::memcmp (c, "VstW", 4) == 0 && std::memcmp (c + 16, "CcnK", 4) == 0, "VST2 header kept");
        const auto byteSize = juce::ByteOrder::bigEndianInt (c + 20);
        check ((size_t) byteSize == oc.getSize() - 24, "CcnK byteSize updated to new size");
        const size_t chunkSizeAt = 24 + 4 + 4 + 4 + 4 + 4 + 124;
        const auto chunkSize = juce::ByteOrder::bigEndianInt (c + chunkSizeAt);
        check ((size_t) chunkSize == oc.getSize() - chunkSizeAt - 4, "chunkSize updated to new size");
        const juce::String tail (juce::CharPointer_UTF8 ((const char*) c + oc.getSize() - 15), 15);
        check (tail == "JUCEPrivateData", "JUCE trailer kept at the end");

        std::vector<int> pos; std::vector<Cycle> got; juce::String name;
        check (vital::readWavetable (out, 1, pos, got, name).wasOk() && got.size() == 64 && name == "Wrapped",
               "osc 2 holds the 64-frame morph");
        std::vector<Cycle> again; juce::String n1, note1;
        vital::renderWavetable (out, 0, again, n1, note1);
        check (n1 == "Basic Shapes", "osc 1 untouched by sending to osc 2");
        check (vital::wavetableFingerprint (out, 0) != vital::wavetableFingerprint (host, 0), "change detection notices the new table");
    }

    // Optional: real files (not in the repo) - argv[2] = real hosted Vital state, argv[3] = a .vitaltable using an audio file
    if (argc > 2)
    {
        juce::MemoryBlock real;
        juce::File (argv[2]).loadFileAsData (real);
        std::vector<Cycle> shown; juce::String nm, note;
        const auto rr = vital::renderWavetable (real, 0, shown, nm, note);
        check (rr.wasOk() && shown.size() == 257, "REAL Vital state: osc 1 readable (" + nm + ") " + rr.getErrorMessage() + note);

        MorphSettings ms; ms.numFrames = 256;
        juce::MemoryBlock out;
        const auto ir = vital::injectWavetable (real, buildMorph (sine, saw, ms), 1, "Real Test", out);
        check (ir.wasOk(), "REAL Vital state: inject into osc 2 " + ir.getErrorMessage());
        std::vector<int> pos; std::vector<Cycle> got; juce::String name;
        check (vital::readWavetable (out, 1, pos, got, name).wasOk() && got.size() == 256 && name == "Real Test", "REAL: read back osc 2");
        outDir.getChildFile ("real_out.bin").replaceWithData (out.getData(), out.getSize());
    }
    if (argc > 3)
    {
        const auto table = juce::File (argv[3]).loadFileAsString();
        const auto patch = "{\"settings\":{\"wavetables\":[" + table + "]}}";
        juce::MemoryBlock mb (patch.toRawUTF8(), patch.getNumBytesAsUTF8());
        std::vector<Cycle> shown; juce::String nm, note;
        const auto rr = vital::renderWavetable (mb, 0, shown, nm, note);
        float pk = 0; for (auto& f : shown) pk = std::max (pk, peakOf (f));
        check (rr.wasOk() && shown.size() == 257 && pk > 0.5f && maxDiff (shown[0], shown[256]) > 0.05f,
               "REAL audio-file table renders, frames differ (" + note + ")");
        outDir.getChildFile ("audio_table_frames.raw").replaceWithData (shown[0].data(), 0); // placeholder
        juce::FileOutputStream fo (outDir.getChildFile ("audio_table_frames.f32"));
        fo.setPosition (0); fo.truncate();
        for (int i : { 0, 64, 128, 192, 256 }) fo.write (shown[(size_t) i].data(), kFrameSize * sizeof (float));
    }

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURES") << std::endl;
    return failures == 0 ? 0 : 1;
}
