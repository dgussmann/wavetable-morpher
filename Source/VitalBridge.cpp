#include "VitalBridge.h"
#include "PluginState.h"

namespace wtm::vital
{
    using wtm::state::HostEnvelope;
    using wtm::state::unwrapHost;
    using wtm::state::rewrapHost;
    using wtm::state::fixRemainingSizeFields;

    namespace
    {
        //==============================================================================
        /** Layer 2: whatever Vital's own wrapper puts around the JSON. We keep the bytes before and after
            the JSON exactly, only fixing "bytes remaining" size fields in the header. */
        bool findJson (const char* p, size_t n, size_t& start, size_t& end)
        {
            // The JSON starts at '{"'. (A lone '{' can appear by chance inside a binary size field.)
            size_t i = 0;
            while (i + 1 < n && ! (p[i] == '{' && (p[i + 1] == '"' || p[i + 1] == '}'))) ++i;
            if (i + 1 >= n) return false;

            start = i;
            int depth = 0;
            bool inString = false, escaped = false;

            for (; i < n; ++i)
            {
                const char c = p[i];
                if (inString)
                {
                    if (escaped)        escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"')  inString = false;
                    continue;
                }

                if (c == '"') inString = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0)
                {
                    end = i + 1;
                    return true;
                }
            }
            return false;
        }

        struct ParsedState
        {
            HostEnvelope envelope;
            juce::MemoryBlock prefix, suffix;
            juce::var root;
        };

        juce::Result parseState (const juce::MemoryBlock& state, ParsedState& out)
        {
            if (state.getSize() == 0)
                return juce::Result::fail ("Vital returned an empty patch.");

            unwrapHost (state, out.envelope);

            const auto* bytes = static_cast<const char*> (out.envelope.component.getData());
            const auto size = out.envelope.component.getSize();

            size_t start = 0, end = 0;
            if (! findJson (bytes, size, start, end))
                return juce::Result::fail ("Vital's patch isn't in the expected format (no JSON found).");

            out.prefix = juce::MemoryBlock (bytes, start);
            out.suffix = juce::MemoryBlock (bytes + end, size - end);

            auto r = juce::JSON::parse (juce::String::fromUTF8 (bytes + start, (int) (end - start)), out.root);
            if (r.failed() || ! out.root.isObject())
                return juce::Result::fail ("Couldn't read Vital's patch: " + r.getErrorMessage());

            return juce::Result::ok();
        }

        juce::MemoryBlock rebuildState (const ParsedState& parsed, const juce::String& json)
        {
            const auto oldJsonSize = (juce::int64) parsed.envelope.component.getSize()
                                     - (juce::int64) parsed.prefix.getSize() - (juce::int64) parsed.suffix.getSize();
            const auto newJsonSize = (juce::int64) json.getNumBytesAsUTF8();
            const auto delta = newJsonSize - oldJsonSize;
            const auto oldTotal = (juce::int64) parsed.envelope.component.getSize();

            juce::MemoryBlock prefix (parsed.prefix);
            fixRemainingSizeFields (prefix, oldTotal, delta);

            juce::MemoryBlock component;
            component.append (prefix.getData(), prefix.getSize());
            component.append (json.toRawUTF8(), json.getNumBytesAsUTF8());
            component.append (parsed.suffix.getData(), parsed.suffix.getSize());

            return rewrapHost (parsed.envelope, component);
        }

        //==============================================================================
        /** Vital names component types in patches; older files store a number instead. */
        juce::String componentType (const juce::var& c)
        {
            const auto& t = c["type"];
            if (t.isString())
                return t.toString();

            static const char* names[] = { "Wave Source", "Line Source", "Audio File Source", "Phase Shift", "Wave Window",
                                           "Frequency Filter", "Slew Limiter", "Wave Folder", "Wave Warp" };
            const int i = (int) t;
            return juce::isPositiveAndBelow (i, (int) std::size (names)) ? juce::String (names[i]) : juce::String (i);
        }

        juce::Array<juce::var>* wavetablesOf (const juce::var& root)
        {
            const auto& settings = root["settings"];
            return settings.isObject() ? settings["wavetables"].getArray() : nullptr;
        }

        bool findTemplate (const juce::Array<juce::var>& tables, int preferred, juce::var& group, juce::var& component)
        {
            auto search = [&] (const juce::var& table)
            {
                if (auto* groups = table["groups"].getArray())
                    for (auto& g : *groups)
                        if (auto* comps = g["components"].getArray())
                            for (auto& c : *comps)
                                if (componentType (c) == "Wave Source" && c["keyframes"].isArray() && c["keyframes"].size() > 0)
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

        /** Audio-file wavetables: a recording (16-bit samples) plus, per keyframe, where in it the
            single-cycle window starts. Each Vital position gets its window, stretched to 2048 samples. */
        void renderAudioFileSource (const juce::var& c, std::vector<std::pair<int, Cycle>>& keys)
        {
            juce::MemoryOutputStream raw;
            if (! juce::Base64::convertFromBase64 (raw, c["audio_file"].toString()) || raw.getDataSize() < 4)
                return;

            const auto numSamples = (int) (raw.getDataSize() / 2);
            const auto* pcm = static_cast<const juce::int16*> (raw.getData());

            std::vector<std::pair<int, std::pair<double, double>>> marks; // position -> (start, window)
            const double componentWindow = c.hasProperty ("window_size") ? (double) c["window_size"] : 2048.0;
            if (auto* kfs = c["keyframes"].getArray())
                for (auto& kf : *kfs)
                {
                    const double window = componentWindow > 1.0 ? componentWindow
                                                                : (kf.hasProperty ("window_size") ? (double) kf["window_size"] : 2048.0);
                    marks.push_back ({ juce::jlimit (0, kVitalFramePositions - 1, (int) kf["position"]),
                                       { (double) kf["start_position"], std::max (2.0, window) } });
                }

            if (marks.empty())
                return;
            std::sort (marks.begin(), marks.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

            auto sampleAt = [&] (double pos)
            {
                pos = juce::jlimit (0.0, (double) numSamples - 1.001, pos);
                const int i = (int) pos;
                const auto f = (float) (pos - i);
                const float a = (float) pcm[i] / 32768.0f, b = (float) pcm[i + 1] / 32768.0f; // little-endian int16
                return a + f * (b - a);
            };

            size_t k = 0;
            for (int p = 0; p < kVitalFramePositions; ++p)
            {
                while (k + 1 < marks.size() && marks[k + 1].first <= p)
                    ++k;

                double start = marks[k].second.first, window = marks[k].second.second;
                if (p > marks[k].first && k + 1 < marks.size())
                {
                    const double t = (double) (p - marks[k].first) / std::max (1, marks[k + 1].first - marks[k].first);
                    start  += t * (marks[k + 1].second.first - start);
                    window += t * (marks[k + 1].second.second - window);
                }

                Cycle cyc ((size_t) kFrameSize);
                for (int i = 0; i < kFrameSize; ++i)
                    cyc[(size_t) i] = sampleAt (start + window * (double) i / (double) kFrameSize);
                removeDC (cyc);
                keys.emplace_back (p, std::move (cyc));
            }
        }

        juce::String encodeFrame (const Cycle& c)
        {
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

    //==============================================================================
    juce::Result injectWavetable (const juce::MemoryBlock& state, const std::vector<Cycle>& frames,
                                  int osc, const juce::String& tableName, juce::MemoryBlock& newState, bool soloOsc)
    {
        if (frames.empty())
            return juce::Result::fail ("There's no wavetable to send yet.");

        ParsedState parsed;
        auto r = parseState (state, parsed);
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
        auto* obj = table.getDynamicObject();
        if (obj == nullptr)
            return juce::Result::fail ("Vital's oscillator " + juce::String (osc + 1) + " has an unexpected format.");

        obj->setProperty ("name", tableName.isNotEmpty() ? tableName : juce::String ("Wavetable Morpher"));
        obj->setProperty ("author", "Wavetable Morpher");
        obj->setProperty ("full_normalize", false);
        obj->setProperty ("remove_all_dc", false);
        obj->setProperty ("groups", juce::Array<juce::var> { group });
        tables->set (osc, table);

        if (auto* settings = parsed.root["settings"].getDynamicObject())
        {
            settings->setProperty ("osc_" + juce::String (osc + 1) + "_on", 1.0);
            if (soloOsc)
            {
                for (int i = 0; i < 3; ++i)
                    if (i != osc)
                        settings->setProperty ("osc_" + juce::String (i + 1) + "_on", 0.0);
                settings->setProperty ("sample_on", 0.0);
            }
        }

        newState = rebuildState (parsed, juce::JSON::toString (parsed.root, true));
        return juce::Result::ok();
    }

    juce::Result readWavetable (const juce::MemoryBlock& state, int osc,
                                std::vector<int>& positions, std::vector<Cycle>& frames, juce::String& name)
    {
        ParsedState parsed;
        auto r = parseState (state, parsed);
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

    juce::Result renderWavetable (const juce::MemoryBlock& state, int osc,
                                  std::vector<Cycle>& out, juce::String& name, juce::String& note)
    {
        ParsedState parsed;
        auto r = parseState (state, parsed);
        if (r.failed())
            return r;

        auto* tables = wavetablesOf (parsed.root);
        if (tables == nullptr || ! juce::isPositiveAndBelow (osc, tables->size()))
            return juce::Result::fail ("Vital's patch has no oscillator " + juce::String (osc + 1) + ".");

        const auto& table = tables->getReference (osc);
        name = table["name"].toString();
        note.clear();

        out.assign ((size_t) kVitalFramePositions, Cycle ((size_t) kFrameSize, 0.0f));
        juce::StringArray skipped;
        int wavesUsed = 0;

        if (auto* groups = table["groups"].getArray())
        {
            for (auto& g : *groups)
            {
                auto* comps = g["components"].getArray();
                if (comps == nullptr)
                    continue;

                for (auto& c : *comps)
                {
                    const auto type = componentType (c);
                    std::vector<std::pair<int, Cycle>> keys;

                    if (type == "Wave Source")
                    {
                        if (auto* kfs = c["keyframes"].getArray())
                            for (auto& kf : *kfs)
                            {
                                Cycle cyc;
                                if (decodeFrame (kf["wave_data"].toString(), cyc))
                                    keys.emplace_back (juce::jlimit (0, kVitalFramePositions - 1, (int) kf["position"]), std::move (cyc));
                            }
                    }
                    else if (type == "Audio File Source")
                    {
                        renderAudioFileSource (c, keys);
                    }
                    else
                    {
                        skipped.addIfNotAlreadyThere (type);
                        continue;
                    }

                    if (keys.empty())
                        continue;

                    std::sort (keys.begin(), keys.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
                    ++wavesUsed;

                    size_t k = 0;
                    for (int p = 0; p < kVitalFramePositions; ++p)
                    {
                        while (k + 1 < keys.size() && keys[k + 1].first <= p)
                            ++k;

                        const auto& a = keys[k];
                        auto& dest = out[(size_t) p];

                        if (p <= a.first || k + 1 >= keys.size())
                        {
                            for (size_t i = 0; i < dest.size(); ++i) dest[i] += a.second[i];
                        }
                        else
                        {
                            const auto& b = keys[k + 1];
                            const float t = (float) (p - a.first) / (float) std::max (1, b.first - a.first);
                            for (size_t i = 0; i < dest.size(); ++i)
                                dest[i] += a.second[i] + t * (b.second[i] - a.second[i]);
                        }
                    }
                }
            }
        }

        if (wavesUsed == 0)
        {
            out.clear();
            note = "This Vital wavetable is built from "
                   + (skipped.isEmpty() ? juce::String ("something this view can't read") : skipped.joinIntoString (" / "))
                   + ", which can't be shown here.";
            return juce::Result::ok();
        }

        // Keep it in -1..1 for display.
        float peak = 0.0f;
        for (auto& f : out) peak = std::max (peak, peakOf (f));
        if (peak > 1.0e-6f)
            for (auto& f : out) for (auto& v : f) v /= peak;

        if (! skipped.isEmpty())
            note = "Shown without: " + skipped.joinIntoString (", ") + ".";

        return juce::Result::ok();
    }

    juce::String tableAuthor (const juce::MemoryBlock& state, int osc)
    {
        ParsedState parsed;
        if (parseState (state, parsed).failed())
            return {};
        auto* tables = wavetablesOf (parsed.root);
        if (tables == nullptr || ! juce::isPositiveAndBelow (osc, tables->size()))
            return {};
        return tables->getReference (osc)["author"].toString();
    }

    juce::int64 wavetableFingerprint (const juce::MemoryBlock& state, int osc)
    {
        HostEnvelope env;
        unwrapHost (state, env);
        const auto* bytes = static_cast<const char*> (env.component.getData());
        size_t start = 0, end = 0;
        if (! findJson (bytes, env.component.getSize(), start, end))
            return 0;

        // Only the wavetables section matters; find it textually to avoid a full parse.
        const auto text = juce::String::fromUTF8 (bytes + start, (int) (end - start));
        const int at = text.indexOf ("\"wavetables\"");
        return (juce::int64) (at >= 0 ? text.substring (at) : text).hashCode64() ^ (juce::int64) osc;
    }
}
