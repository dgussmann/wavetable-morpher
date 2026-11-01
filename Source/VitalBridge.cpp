#include "VitalBridge.h"

namespace wtm::vital
{
    namespace
    {
        struct ParsedState
        {
            juce::var root;
            bool trailingNull = false;
        };

        juce::Result parseState (const juce::MemoryBlock& state, ParsedState& out)
        {
            if (state.getSize() == 0)
                return juce::Result::fail ("Vital returned an empty patch.");

            const auto* bytes = static_cast<const char*> (state.getData());
            size_t size = state.getSize();
            out.trailingNull = bytes[size - 1] == 0;
            while (size > 0 && bytes[size - 1] == 0)
                --size;

            const auto text = juce::String::fromUTF8 (bytes, (int) size);
            const int start = text.indexOfChar ('{');
            if (start < 0)
                return juce::Result::fail ("Vital's patch isn't in the expected format (no JSON found).");

            auto r = juce::JSON::parse (text.substring (start), out.root);
            if (r.failed() || ! out.root.isObject())
                return juce::Result::fail ("Couldn't read Vital's patch: " + r.getErrorMessage());

            return juce::Result::ok();
        }

        juce::Array<juce::var>* wavetablesOf (const juce::var& root)
        {
            const auto& settings = root["settings"];
            if (! settings.isObject())
                return nullptr;
            return settings["wavetables"].getArray();
        }

        /** Finds a "Wave Source" component with at least one keyframe to use as a template, so every
            field Vital expects (interpolation settings etc.) is present in what we write. */
        bool findTemplate (const juce::Array<juce::var>& tables, int preferred, juce::var& group, juce::var& component)
        {
            auto search = [&] (const juce::var& table)
            {
                if (auto* groups = table["groups"].getArray())
                    for (auto& g : *groups)
                        if (auto* comps = g["components"].getArray())
                            for (auto& c : *comps)
                                if (c["type"].toString() == "Wave Source"
                                    && c["keyframes"].isArray() && c["keyframes"].size() > 0)
                                {
                                    group = g;
                                    component = c;
                                    return true;
                                }
                return false;
            };

            if (juce::isPositiveAndBelow (preferred, tables.size()) && search (tables.getReference (preferred)))
                return true;
            for (auto& t : tables)
                if (search (t))
                    return true;
            return false;
        }

        juce::String encodeFrame (const Cycle& c)
        {
            // Vital stores raw little-endian float32 (Apple Silicon and Intel are both little-endian).
            return juce::Base64::toBase64 (c.data(), c.size() * sizeof (float));
        }

        bool decodeFrame (const juce::String& s, Cycle& out)
        {
            juce::MemoryOutputStream mo;
            if (! juce::Base64::convertFromBase64 (mo, s) || mo.getDataSize() != (size_t) kFrameSize * sizeof (float))
                return false;
            out.assign ((size_t) kFrameSize, 0.0f);
            std::memcpy (out.data(), mo.getData(), mo.getDataSize());
            return true;
        }
    }

    juce::Result injectWavetable (const juce::MemoryBlock& vitalState, const std::vector<Cycle>& frames,
                                  int osc, const juce::String& tableName, juce::MemoryBlock& newState)
    {
        if (frames.empty())
            return juce::Result::fail ("There's no wavetable to send yet.");

        ParsedState parsed;
        auto r = parseState (vitalState, parsed);
        if (r.failed())
            return r;

        auto* tables = wavetablesOf (parsed.root);
        if (tables == nullptr || tables->isEmpty())
            return juce::Result::fail ("Couldn't find the oscillators in Vital's patch.");
        if (! juce::isPositiveAndBelow (osc, tables->size()))
            return juce::Result::fail ("Vital's patch has no oscillator " + juce::String (osc + 1) + ".");

        juce::var templateGroup, templateComponent;
        if (! findTemplate (*tables, osc, templateGroup, templateComponent))
            return juce::Result::fail ("Vital's current patch has no basic wavetable to replace. "
                                       "Load Vital's Init preset (or any patch with a normal wavetable) and try again.");

        // Keyframes: spread our frames over Vital's 0..256 positions.
        const auto keyTemplate = templateComponent["keyframes"][0];
        const int n = (int) frames.size();
        juce::Array<juce::var> keyframes;
        keyframes.ensureStorageAllocated (n);

        for (int i = 0; i < n; ++i)
        {
            auto kf = keyTemplate.clone();
            const int position = n == 1 ? 0 : juce::roundToInt ((double) i * (kVitalFramePositions - 1) / (double) (n - 1));
            kf.getDynamicObject()->setProperty ("position", position);
            kf.getDynamicObject()->setProperty ("wave_data", encodeFrame (frames[(size_t) i]));
            keyframes.add (kf);
        }

        auto component = templateComponent.clone();
        component.getDynamicObject()->setProperty ("keyframes", keyframes);

        auto group = templateGroup.clone();
        group.getDynamicObject()->setProperty ("components", juce::Array<juce::var> { component });

        auto table = tables->getReference (osc).clone();
        if (auto* obj = table.getDynamicObject())
        {
            obj->setProperty ("name", tableName.isNotEmpty() ? tableName : juce::String ("Wavetable Morpher"));
            obj->setProperty ("author", "Wavetable Morpher");
            obj->setProperty ("full_normalize", false);
            obj->setProperty ("remove_all_dc", false);
            obj->setProperty ("groups", juce::Array<juce::var> { group });
        }
        else
        {
            return juce::Result::fail ("Vital's oscillator " + juce::String (osc + 1) + " has an unexpected format.");
        }

        tables->set (osc, table);

        // Make sure the oscillator you're sending to is switched on.
        if (auto* settings = parsed.root["settings"].getDynamicObject())
            settings->setProperty ("osc_" + juce::String (osc + 1) + "_on", 1.0);

        const auto json = juce::JSON::toString (parsed.root, true);
        newState.reset();
        newState.append (json.toRawUTF8(), json.getNumBytesAsUTF8());
        if (parsed.trailingNull)
            newState.append ("\0", 1);

        return juce::Result::ok();
    }

    juce::Result readWavetable (const juce::MemoryBlock& vitalState, int osc,
                                std::vector<int>& positions, std::vector<Cycle>& frames, juce::String& name)
    {
        ParsedState parsed;
        auto r = parseState (vitalState, parsed);
        if (r.failed())
            return r;

        auto* tables = wavetablesOf (parsed.root);
        if (tables == nullptr || ! juce::isPositiveAndBelow (osc, tables->size()))
            return juce::Result::fail ("no such oscillator");

        const auto& table = tables->getReference (osc);
        name = table["name"].toString();
        positions.clear();
        frames.clear();

        if (auto* groups = table["groups"].getArray())
            for (auto& g : *groups)
                if (auto* comps = g["components"].getArray())
                    for (auto& c : *comps)
                        if (auto* kfs = c["keyframes"].getArray())
                            for (auto& kf : *kfs)
                            {
                                Cycle cyc;
                                if (decodeFrame (kf["wave_data"].toString(), cyc))
                                {
                                    positions.push_back ((int) kf["position"]);
                                    frames.push_back (std::move (cyc));
                                }
                            }

        return juce::Result::ok();
    }
}
