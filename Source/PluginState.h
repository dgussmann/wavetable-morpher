#pragma once

#include <juce_core/juce_core.h>

/** Plugin state as it comes out of a hosted VST3 (shared by the Vital and Serum bridges).
    JUCE's VST3 host hands back "VC2!" + XML <VST3PluginState><IComponent>base64</IComponent>...;
    inside IComponent is the synth's own data, sometimes behind a VST2-style header. */
namespace wtm::state
{
    struct HostEnvelope
    {
        std::unique_ptr<juce::XmlElement> xml;   // null when the state wasn't wrapped
        juce::MemoryBlock component;             // the plugin's own (IComponent) state
    };

    void unwrapHost (const juce::MemoryBlock& in, HostEnvelope& out);
    juce::MemoryBlock rewrapHost (const HostEnvelope& env, const juce::MemoryBlock& component);

    /** Header fields such as VST2 "CcnK" byteSize / chunkSize hold "bytes remaining after this field".
        After the payload that follows `prefix` grows by `delta` bytes, adds `delta` to any such field.
        `oldTotal` is the size of the whole block (prefix + payload + suffix) before the change. */
    void fixRemainingSizeFields (juce::MemoryBlock& prefix, juce::int64 oldTotal, juce::int64 delta);
}
