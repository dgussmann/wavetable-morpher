#include "SerumBridge.h"
#include "PluginState.h"

#include <juce_cryptography/juce_cryptography.h>
#include <zstd.h>

namespace wtm::serum
{
    namespace
    {
        const char kMagic[] = "XferJson"; // followed by a 0 byte

        //==============================================================================
        /** Just enough CBOR to find a value by its path of map keys and know its byte span. */
        struct Cbor
        {
            const juce::uint8* p;
            size_t n;

            bool head (size_t& pos, int& major, juce::uint64& value, bool& indefinite) const
            {
                if (pos >= n) return false;
                const auto b = p[pos++];
                major = b >> 5;
                const int ai = b & 31;
                indefinite = false;

                if (ai < 24)       { value = (juce::uint64) ai; return true; }
                if (ai == 31)      { indefinite = true; value = 0; return true; }
                if (ai > 27)       return false;

                const int bytes = 1 << (ai - 24); // 1, 2, 4, 8
                if (pos + (size_t) bytes > n) return false;
                value = 0;
                for (int i = 0; i < bytes; ++i)
                    value = (value << 8) | p[pos++];
                return true;
            }

            bool skip (size_t& pos, int depth = 0) const
            {
                if (depth > 64) return false;
                int major; juce::uint64 v; bool indef;
                if (! head (pos, major, v, indef)) return false;

                switch (major)
                {
                    case 0: case 1: return true;
                    case 2: case 3:
                        if (indef)
                        {
                            while (pos < n && p[pos] != 0xff)
                                if (! skip (pos, depth + 1)) return false;
                            return ++pos <= n;
                        }
                        pos += (size_t) v;
                        return pos <= n;
                    case 4: case 5:
                    {
                        const juce::uint64 items = major == 5 ? v * 2 : v;
                        if (indef)
                        {
                            while (pos < n && p[pos] != 0xff)
                                if (! skip (pos, depth + 1)) return false;
                            return ++pos <= n;
                        }
                        for (juce::uint64 i = 0; i < items; ++i)
                            if (! skip (pos, depth + 1)) return false;
                        return true;
                    }
                    case 6: return skip (pos, depth + 1);
                    case 7:
                        if (indef) return false; // stray "break"
                        return true;             // head() already consumed the float/simple payload
                    default: return false;
                }
            }

            bool readText (size_t& pos, juce::String& out) const
            {
                int major; juce::uint64 v; bool indef;
                const auto start = pos;
                if (! head (pos, major, v, indef) || major != 3 || indef || pos + (size_t) v > n)
                {
                    pos = start;
                    return false;
                }
                out = juce::String::fromUTF8 (reinterpret_cast<const char*> (p + pos), (int) v);
                pos += (size_t) v;
                return true;
            }

            /** Finds the value at `path` (map keys) starting from the item at `pos`. */
            bool find (size_t pos, const juce::StringArray& path, int depth, size_t& valueStart, size_t& valueEnd) const
            {
                int major; juce::uint64 v; bool indef;
                if (! head (pos, major, v, indef) || major != 5)
                    return false;

                for (juce::uint64 i = 0; indef || i < v; ++i)
                {
                    if (indef && pos < n && p[pos] == 0xff)
                        break;

                    juce::String key;
                    const bool isText = readText (pos, key);
                    if (! isText && ! skip (pos))
                        return false;

                    const auto vStart = pos;
                    if (isText && key == path[depth])
                    {
                        if (depth == path.size() - 1)
                        {
                            valueStart = vStart;
                            valueEnd = vStart;
                            return skip (valueEnd);
                        }
                        return find (vStart, path, depth + 1, valueStart, valueEnd);
                    }

                    if (! skip (pos))
                        return false;
                }
                return false;
            }
        };

        void encodeHead (juce::MemoryOutputStream& out, int major, juce::uint64 value)
        {
            const auto m = (juce::uint8) (major << 5);
            if (value < 24)               { out.writeByte ((char) (m | (juce::uint8) value)); }
            else if (value <= 0xff)       { out.writeByte ((char) (m | 24)); out.writeByte ((char) value); }
            else if (value <= 0xffff)     { out.writeByte ((char) (m | 25)); out.writeShortBigEndian ((short) value); }
            else if (value <= 0xffffffff) { out.writeByte ((char) (m | 26)); out.writeIntBigEndian ((int) value); }
            else                          { out.writeByte ((char) (m | 27)); out.writeInt64BigEndian ((juce::int64) value); }
        }

        juce::MemoryBlock encodeText (const juce::String& s)
        {
            juce::MemoryOutputStream out;
            encodeHead (out, 3, s.getNumBytesAsUTF8());
            out.write (s.toRawUTF8(), s.getNumBytesAsUTF8());
            return out.getMemoryBlock();
        }

        juce::MemoryBlock encodeUInt (juce::uint64 v)
        {
            juce::MemoryOutputStream out;
            encodeHead (out, 0, v);
            return out.getMemoryBlock();
        }

        //==============================================================================
        struct Parsed
        {
            state::HostEnvelope envelope;
            size_t magicAt = 0;          // offset of "XferJson" in the component
            juce::String headerJson;
            juce::uint32 version = 2;
            juce::MemoryBlock cbor;      // decompressed document
            size_t endOfFrame = 0;       // where the zstd data ends in the component
        };

        juce::Result parse (const juce::MemoryBlock& state, Parsed& out)
        {
            state::unwrapHost (state, out.envelope);
            const auto& comp = out.envelope.component;
            const auto* b = static_cast<const char*> (comp.getData());
            const auto size = comp.getSize();

            size_t magic = std::string::npos;
            for (size_t i = 0; i + 9 <= size; ++i)
                if (std::memcmp (b + i, kMagic, 8) == 0 && b[i + 8] == 0) { magic = i; break; }

            if (magic == std::string::npos)
                return juce::Result::fail ("Serum's patch isn't in the expected format (no XferJson header).");

            if (magic + 17 > size)
                return juce::Result::fail ("Serum's patch is truncated.");

            const auto jsonLen = (size_t) juce::ByteOrder::littleEndianInt64 (b + magic + 9);
            const auto jsonAt = magic + 17;
            if (jsonAt + jsonLen + 8 > size)
                return juce::Result::fail ("Serum's patch header is damaged.");

            out.magicAt = magic;
            out.headerJson = juce::String::fromUTF8 (b + jsonAt, (int) jsonLen);

            const auto o = jsonAt + jsonLen;
            const auto rawSize = (size_t) juce::ByteOrder::littleEndianInt (b + o);
            out.version = juce::ByteOrder::littleEndianInt (b + o + 4);
            const auto zAt = o + 8;

            auto frameSize = ZSTD_findFrameCompressedSize (b + zAt, size - zAt);
            if (ZSTD_isError (frameSize))
                return juce::Result::fail ("Serum's patch data isn't compressed the expected way.");

            out.endOfFrame = zAt + frameSize;
            out.cbor.setSize (rawSize);
            const auto got = ZSTD_decompress (out.cbor.getData(), rawSize, b + zAt, frameSize);
            if (ZSTD_isError (got) || got != rawSize)
                return juce::Result::fail ("Couldn't unpack Serum's patch.");

            return juce::Result::ok();
        }

        juce::MemoryBlock rebuild (const Parsed& parsed, const juce::MemoryBlock& newCbor)
        {
            // Compress the edited document
            juce::MemoryBlock z (ZSTD_compressBound (newCbor.getSize()));
            const auto zSize = ZSTD_compress (z.getData(), z.getSize(), newCbor.getData(), newCbor.getSize(), 3);
            z.setSize (ZSTD_isError (zSize) ? 0 : zSize);

            // Header "hash" is the MD5 of the compressed data
            auto json = parsed.headerJson;
            const auto hash = juce::MD5 (z.getData(), z.getSize()).toHexString();
            const int h = json.indexOf ("\"hash\"");
            if (h >= 0)
            {
                const int q1 = json.indexOfChar (h + 6, '"');
                const int q2 = q1 >= 0 ? json.indexOfChar (q1 + 1, '"') : -1;
                if (q1 >= 0 && q2 > q1)
                    json = json.substring (0, q1 + 1) + hash + json.substring (q2);
            }

            const auto& comp = parsed.envelope.component;
            const auto* b = static_cast<const char*> (comp.getData());

            juce::MemoryOutputStream payload;
            payload.write (kMagic, 8);
            payload.writeByte (0);
            payload.writeInt64 ((juce::int64) json.getNumBytesAsUTF8());
            payload.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
            payload.writeInt ((int) newCbor.getSize());
            payload.writeInt ((int) parsed.version);
            payload.write (z.getData(), z.getSize());

            juce::MemoryBlock prefix (b, parsed.magicAt);
            const auto oldPayload = (juce::int64) parsed.endOfFrame - (juce::int64) parsed.magicAt;
            state::fixRemainingSizeFields (prefix, (juce::int64) comp.getSize(), (juce::int64) payload.getDataSize() - oldPayload);

            juce::MemoryBlock newComp;
            newComp.append (prefix.getData(), prefix.getSize());
            newComp.append (payload.getData(), payload.getDataSize());
            newComp.append (b + parsed.endOfFrame, comp.getSize() - parsed.endOfFrame);

            return state::rewrapHost (parsed.envelope, newComp);
        }

        juce::StringArray pathFor (int osc, const char* leaf)
        {
            return { "Oscillator" + juce::String (osc), "WTOsc" + juce::String (osc), leaf };
        }
    }

    //==============================================================================
    bool looksLikeSerumState (const juce::MemoryBlock& state)
    {
        Parsed p;
        return parse (state, p).wasOk();
    }

    juce::Result getOscillatorWavetable (const juce::MemoryBlock& state, int osc, juce::String& relativePath, int& numSamples)
    {
        Parsed parsed;
        auto r = parse (state, parsed);
        if (r.failed())
            return r;

        const Cbor cbor { static_cast<const juce::uint8*> (parsed.cbor.getData()), parsed.cbor.getSize() };
        size_t s = 0, e = 0;
        if (! cbor.find (0, pathFor (osc, "relativePathToWT"), 0, s, e))
            return juce::Result::fail ("Serum's oscillator " + juce::String::charToString ((juce::juce_wchar) ('A' + osc)) + " isn't using a wavetable.");

        size_t pos = s;
        if (! cbor.readText (pos, relativePath))
            return juce::Result::fail ("Unexpected wavetable entry in Serum's patch.");

        numSamples = 0;
        if (cbor.find (0, pathFor (osc, "numFrames"), 0, s, e))
        {
            int major; juce::uint64 v; bool indef;
            pos = s;
            if (cbor.head (pos, major, v, indef) && major == 0)
                numSamples = (int) v;
        }
        return juce::Result::ok();
    }

    juce::Result setOscillatorWavetable (const juce::MemoryBlock& state, int osc, const juce::String& relativePath,
                                         int numSamples, juce::MemoryBlock& newState)
    {
        Parsed parsed;
        auto r = parse (state, parsed);
        if (r.failed())
            return r;

        const Cbor cbor { static_cast<const juce::uint8*> (parsed.cbor.getData()), parsed.cbor.getSize() };

        struct Edit { size_t start, end; juce::MemoryBlock bytes; };
        std::vector<Edit> edits;

        size_t s = 0, e = 0;
        if (! cbor.find (0, pathFor (osc, "relativePathToWT"), 0, s, e))
            return juce::Result::fail ("Serum's oscillator " + juce::String::charToString ((juce::juce_wchar) ('A' + osc))
                                       + " isn't a wavetable oscillator in this patch. Set it to Wavetable mode in Serum and try again.");
        edits.push_back ({ s, e, encodeText (relativePath) });

        if (cbor.find (0, pathFor (osc, "numFrames"), 0, s, e))
            edits.push_back ({ s, e, encodeUInt ((juce::uint64) numSamples) });

        // Apply from the end so earlier offsets stay valid. (CBOR maps count entries, not bytes,
        // so replacing a value with one of a different length needs no other changes.)
        std::sort (edits.begin(), edits.end(), [] (const Edit& a, const Edit& b) { return a.start > b.start; });

        juce::MemoryBlock doc (parsed.cbor);
        for (const auto& ed : edits)
        {
            juce::MemoryBlock next;
            next.append (doc.getData(), ed.start);
            next.append (ed.bytes.getData(), ed.bytes.getSize());
            next.append (static_cast<const char*> (doc.getData()) + ed.end, doc.getSize() - ed.end);
            doc = std::move (next);
        }

        newState = rebuild (parsed, doc);
        return juce::Result::ok();
    }
}
