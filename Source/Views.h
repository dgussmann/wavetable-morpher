#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveSources.h"

namespace wtm
{
    namespace Colours
    {
        inline const juce::Colour background { 0xff12141a };
        inline const juce::Colour panel      { 0xff1b1e27 };
        inline const juce::Colour panelEdge  { 0xff2a2e3a };
        inline const juce::Colour text       { 0xffdfe3ec };
        inline const juce::Colour dimText    { 0xff8b91a1 };
        inline const juce::Colour accentA    { 0xff35d0ff }; // oscillator A
        inline const juce::Colour accentB    { 0xffff6a9a }; // oscillator B
        inline const juce::Colour accent     { 0xffb889ff }; // morph / highlights
    }

    /** Shows one single-cycle waveform. In draw mode, drag with the mouse to draw a new shape. */
    class WaveformView : public juce::Component
    {
    public:
        explicit WaveformView (juce::Colour c) : colour (c) {}

        void setCycle (const Cycle& c)  { cycle = c; repaint(); }
        void setDrawMode (bool on)      { drawMode = on; setMouseCursor (on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor); repaint(); }
        bool isDrawMode() const         { return drawMode; }

        std::function<void (const Cycle&)> onDrawn;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        void drawTo (juce::Point<float> p);

        juce::Colour colour;
        Cycle cycle = makeSilence();
        bool drawMode = false, drawing = false;
        int lastIndex = -1;
        float lastValue = 0.0f;
    };

    /** The finished wavetable drawn as a stack of frames in pseudo-3D, with the current morph position lit up. */
    class WavetableView : public juce::Component
    {
    public:
        void setFrames (std::vector<Cycle> f) { frames = std::move (f); repaint(); }
        void setEmptyText (const juce::String& t) { if (t != emptyText) { emptyText = t; repaint(); } }
        void setPosition (float p)
        {
            if (std::abs (p - position) > 0.0005f) { position = p; repaint(); }
        }

        /** Click or drag on the view to set the morph position. */
        std::function<void (float)> onPositionDragged;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
        void mouseDrag (const juce::MouseEvent& e) override;

    private:
        std::vector<Cycle> frames;
        float position = 0.0f;
        juce::String emptyText { "Building wavetable..." };
    };

    class MorphLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        MorphLookAndFeel();
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    };
}
