#include "WavetableExporter.h"

namespace wtm
{
    namespace
    {
        void writeChunk (juce::OutputStream& out, const char* id, const void* data, size_t size)
        {
            out.write (id, 4);
            out.writeInt ((int) size);   // little-endian
            out.write (data, size);
            if (size & 1)
                out.writeByte (0);       // RIFF chunks are padded to an even length
        }
    }

    juce::Result writeWavetableWav (const juce::File& file, const std::vector<Cycle>& frames, int sampleRate)
    {
        if (frames.empty())
            return juce::Result::fail ("There's no wavetable to save yet.");

        // Sample data
        juce::MemoryOutputStream samples;
        for (const auto& frame : frames)
        {
            jassert ((int) frame.size() == kFrameSize);
            for (auto v : frame)
                samples.writeFloat (juce::jlimit (-1.0f, 1.0f, v));
        }

        // fmt: IEEE float, mono, 32-bit
        juce::MemoryOutputStream fmt;
        fmt.writeShort (3);                 // WAVE_FORMAT_IEEE_FLOAT
        fmt.writeShort (1);                 // channels
        fmt.writeInt (sampleRate);
        fmt.writeInt (sampleRate * 4);      // byte rate
        fmt.writeShort (4);                 // block align
        fmt.writeShort (32);                // bits per sample

        // clm: frame-size marker understood by Serum and Vital
        const juce::String clmText = "<!>" + juce::String (kFrameSize) + " 00000000 wavetable (Wavetable Morpher)";
        const auto clm = clmText.toStdString();

        // fact: required for non-PCM WAVs by the spec
        juce::MemoryOutputStream fact;
        fact.writeInt ((int) (frames.size() * (size_t) kFrameSize));

        juce::MemoryOutputStream body;
        body.write ("WAVE", 4);
        writeChunk (body, "fmt ", fmt.getData(), fmt.getDataSize());
        writeChunk (body, "fact", fact.getData(), fact.getDataSize());
        writeChunk (body, "clm ", clm.data(), clm.size());
        writeChunk (body, "data", samples.getData(), samples.getDataSize());

        juce::MemoryOutputStream whole;
        writeChunk (whole, "RIFF", body.getData(), body.getDataSize());

        const auto parent = file.getParentDirectory();
        if (! parent.isDirectory())
        {
            auto r = parent.createDirectory();
            if (r.failed())
                return juce::Result::fail ("Couldn't create the folder " + parent.getFullPathName() + " (" + r.getErrorMessage() + ")");
        }

        juce::TemporaryFile temp (file);
        if (! temp.getFile().replaceWithData (whole.getData(), whole.getDataSize()))
            return juce::Result::fail ("Couldn't write to " + parent.getFullPathName() + ". Check that the folder exists and that you have permission to write there.");

        if (! temp.overwriteTargetFileWithTemporary())
            return juce::Result::fail ("Couldn't replace " + file.getFullPathName() + ".");

        return juce::Result::ok();
    }

    //==============================================================================
    juce::String destinationName (Destination d)
    {
        switch (d)
        {
            case Destination::Library:   return "Library";
            case Destination::Serum2:    return "Serum 2";
            case Destination::Vital:     return "Vital";
            case Destination::MassiveXL: return "Massive XL";
            case Destination::NumDestinations: break;
        }
        return {};
    }

    juce::String destinationKey (Destination d)
    {
        switch (d)
        {
            case Destination::Library:   return "library";
            case Destination::Serum2:    return "serum2";
            case Destination::Vital:     return "vital";
            case Destination::MassiveXL: return "massivexl";
            case Destination::NumDestinations: break;
        }
        return {};
    }

    static juce::File home()      { return juce::File::getSpecialLocation (juce::File::userHomeDirectory); }
    static juce::File documents() { return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory); }
    static juce::File music()     { return juce::File::getSpecialLocation (juce::File::userMusicDirectory); }

    static juce::File findVitalDataDirectory()
    {
        // Vital records its data folder in its config file; honour it if the user moved it.
        const juce::File configs[] = {
            home().getChildFile ("Library/Application Support/vital/Vital.config"),
            home().getChildFile ("Library/Application Support/Vital/Vital.config"),
        };

        for (const auto& config : configs)
        {
            if (! config.existsAsFile())
                continue;

            const auto json = juce::JSON::parse (config);
            const auto dir = json.getProperty ("data_directory", {}).toString();
            if (dir.isNotEmpty() && juce::File::isAbsolutePath (dir) && juce::File (dir).isDirectory())
                return juce::File (dir);
        }

        const juce::File candidates[] = {
            music().getChildFile ("Vital"),
            documents().getChildFile ("Vital"),
            juce::File ("/Library/Audio/Presets/Vital"),
        };

        for (const auto& c : candidates)
            if (c.isDirectory())
                return c;

        return {};
    }

    juce::File detectDefaultFolder (Destination d)
    {
        switch (d)
        {
            case Destination::Library:
                return music().getChildFile ("Wavetable Morpher");

            case Destination::Serum2:
            {
                const juce::File presetRoots[] = {
                    juce::File ("/Library/Audio/Presets/Xfer Records/Serum 2 Presets"),
                    home().getChildFile ("Library/Audio/Presets/Xfer Records/Serum 2 Presets"),
                    documents().getChildFile ("Xfer/Serum 2 Presets"),
                };

                for (const auto& root : presetRoots)
                    if (root.isDirectory())
                        return root.getChildFile ("Tables").getChildFile ("Wavetable Morpher");

                return {};
            }

            case Destination::Vital:
            {
                const auto data = findVitalDataDirectory();
                return data == juce::File() ? juce::File() : data.getChildFile ("User").getChildFile ("Wavetables");
            }

            case Destination::MassiveXL:
            {
                // Massive XL's User Content folder - if none of these exist, the plugin asks the user to pick it once.
                const juce::File candidates[] = {
                    documents().getChildFile ("Native Instruments/User Content/Massive XL"),
                    documents().getChildFile ("Native Instruments/Massive XL"),
                    documents().getChildFile ("Native Instruments/Massive XL/User Content"),
                };

                for (const auto& c : candidates)
                    if (c.getChildFile ("Wavetables").isDirectory())
                        return c.getChildFile ("Wavetables");

                return {};
            }

            case Destination::NumDestinations:
                break;
        }

        return {};
    }

    juce::String destinationHelp (Destination d)
    {
        switch (d)
        {
            case Destination::Serum2:
                return "In Serum 2, open the menu and choose \"Show Serum Presets Folder\", then pick its Tables folder. "
                       "New tables appear after Serum rescans (or reopen Serum).";
            case Destination::Vital:
                return "In Vital, open a wavetable browser, right-click the User folder and choose \"Open File Location\", then pick that Wavetables folder.";
            case Destination::MassiveXL:
                return "In Massive XL, open Settings > Show User Content Folder, then pick its Wavetables folder. "
                       "Afterwards choose Settings > Rescan Content in Massive XL. (Original Massive and Massive X can't load custom wavetables.)";
            case Destination::Library:
            case Destination::NumDestinations:
                break;
        }
        return {};
    }
}
