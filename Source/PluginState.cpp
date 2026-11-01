#include "PluginState.h"

namespace wtm::state
{
    constexpr juce::uint32 kHostXmlMagic = 0x21324356; // "VC2!" written by juce::AudioProcessor::copyXmlToBinary

    void unwrapHost (const juce::MemoryBlock& in, HostEnvelope& out)
    {
        const auto* bytes = static_cast<const char*> (in.getData());
        const auto size = in.getSize();

        if (size > 8 && juce::ByteOrder::littleEndianInt (bytes) == kHostXmlMagic)
        {
            const auto length = (int) juce::ByteOrder::littleEndianInt (bytes + 4);
            const auto text = juce::String::fromUTF8 (bytes + 8, juce::jmin ((int) size - 8, length));

            if (auto xml = juce::parseXML (text); xml != nullptr && xml->getChildByName ("IComponent") != nullptr)
            {
                juce::MemoryBlock component;
                if (component.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText()))
                {
                    out.xml = std::move (xml);
                    out.component = std::move (component);
                    return;
                }
            }
        }

        out.xml.reset();
        out.component = in;
    }

    juce::MemoryBlock rewrapHost (const HostEnvelope& env, const juce::MemoryBlock& component)
    {
        if (env.xml == nullptr)
            return component;

        auto* child = env.xml->getChildByName ("IComponent");
        child->deleteAllTextElements();
        child->addTextElement (component.toBase64Encoding());

        // Same layout as juce::AudioProcessor::copyXmlToBinary
        juce::MemoryBlock out;
        {
            juce::MemoryOutputStream stream (out, false);
            stream.writeInt ((int) kHostXmlMagic);
            stream.writeInt (0);
            env.xml->writeTo (stream, juce::XmlElement::TextFormat().singleLine());
            stream.writeByte (0);
        }
        const auto length = juce::ByteOrder::swapIfBigEndian ((juce::uint32) out.getSize() - 9);
        std::memcpy (static_cast<char*> (out.getData()) + 4, &length, 4);
        return out;
    }


    void fixRemainingSizeFields (juce::MemoryBlock& prefix, juce::int64 oldTotal, juce::int64 delta)
    {
        auto* p = static_cast<unsigned char*> (prefix.getData());
        for (size_t o = 0; o + 4 <= prefix.getSize(); ++o)
        {
            const auto remaining = oldTotal - (juce::int64) (o + 4);
            if (remaining <= 16)
                break;

            const auto be = (juce::int64) juce::ByteOrder::bigEndianInt (p + o);
            const auto le = (juce::int64) juce::ByteOrder::littleEndianInt (p + o);

            if (be == remaining)
            {
                const auto v = juce::ByteOrder::swapIfLittleEndian ((juce::uint32) (be + delta));
                std::memcpy (p + o, &v, 4);
                o += 3;
            }
            else if (le == remaining)
            {
                const auto v = juce::ByteOrder::swapIfBigEndian ((juce::uint32) (le + delta));
                std::memcpy (p + o, &v, 4);
                o += 3;
            }
        }
    }
}
