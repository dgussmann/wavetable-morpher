#include "PreviewSynth.h"

namespace wtm
{
    void WavetableVoice::startNote (int midiNote, float velocity, juce::SynthesiserSound*, int pitchWheel)
    {
        noteNumber = midiNote;
        noteGain = 0.25f + 0.75f * velocity;
        phase = 0.0;
        pitchWheelMoved (pitchWheel);

        adsr.setSampleRate (getSampleRate());
        adsr.setParameters (context.envelope);
        adsr.reset();
        adsr.noteOn();
    }

    void WavetableVoice::stopNote (float, bool allowTailOff)
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void WavetableVoice::pitchWheelMoved (int newValue)
    {
        bendSemitones = 2.0 * ((double) newValue - 8192.0) / 8192.0; // +/- 2 semitones
        updateFrequency();
    }

    void WavetableVoice::updateFrequency()
    {
        const double freq = 440.0 * std::pow (2.0, ((double) noteNumber - 69.0 + bendSemitones) / 12.0);
        const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;
        phaseIncrement = freq / sr;
        level = PlaybackTable::levelForFrequency (freq, sr);
    }

    void WavetableVoice::renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples)
    {
        if (! isVoiceActive())
            return;

        const auto* table = context.table;
        if (table == nullptr || table->getNumFrames() < 1)
            return;

        adsr.setParameters (context.envelope);

        const int lastFrame = table->getNumFrames() - 1;
        const int blockLength = std::max (1, output.getNumSamples());
        const float posDelta = (context.positionEnd - context.positionStart) / (float) blockLength;
        const int numChannels = output.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            const int s = startSample + i;
            const float position = juce::jlimit (0.0f, 1.0f, context.positionStart + posDelta * (float) s);
            const float framePos = position * (float) lastFrame;
            const int f0 = (int) framePos;
            const int f1 = std::min (f0 + 1, lastFrame);
            const float frameFrac = framePos - (float) f0;

            const double readPos = phase * (double) kFrameSize;
            const int idx = (int) readPos;
            const auto frac = (float) (readPos - (double) idx);

            const float* t0 = table->getFrame (level, f0);
            const float* t1 = table->getFrame (level, f1);
            const float a = t0[idx] + frac * (t0[idx + 1] - t0[idx]);
            const float b = t1[idx] + frac * (t1[idx + 1] - t1[idx]);
            const float sample = (a + frameFrac * (b - a)) * adsr.getNextSample() * noteGain * context.gain;

            for (int ch = 0; ch < numChannels; ++ch)
                output.addSample (ch, s, sample);

            phase += phaseIncrement;
            if (phase >= 1.0)
                phase -= std::floor (phase);

            if (! adsr.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }
}
