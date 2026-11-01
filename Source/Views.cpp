#include "Views.h"

namespace wtm
{
    //==============================================================================
    void WaveformView::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (drawMode ? colour.withAlpha (0.7f) : Colours::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, drawMode ? 1.5f : 1.0f);

        auto area = r.reduced (6.0f, 8.0f);
        g.setColour (Colours::panelEdge);
        g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

        if (cycle.empty())
            return;

        juce::Path p;
        const int step = std::max (1, kFrameSize / std::max (1, (int) area.getWidth() * 2));
        for (int i = 0; i < kFrameSize; i += step)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) (kFrameSize - 1);
            const float y = area.getCentreY() - cycle[(size_t) i] * area.getHeight() * 0.5f;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }

        g.setColour (colour.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (drawMode && ! drawing)
        {
            g.setColour (Colours::dimText);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("drag to draw", getLocalBounds().reduced (8, 4), juce::Justification::bottomRight);
        }
    }

    void WaveformView::drawTo (juce::Point<float> p)
    {
        auto area = getLocalBounds().toFloat().reduced (6.0f, 8.0f);
        const int index = juce::jlimit (0, kFrameSize - 1, (int) ((p.x - area.getX()) / area.getWidth() * (float) kFrameSize));
        const float value = juce::jlimit (-1.0f, 1.0f, (area.getCentreY() - p.y) / (area.getHeight() * 0.5f));

        if (lastIndex < 0)
        {
            cycle[(size_t) index] = value;
        }
        else
        {
            // Fill the gap since the last mouse event so fast strokes stay continuous.
            const int from = std::min (lastIndex, index), to = std::max (lastIndex, index);
            for (int i = from; i <= to; ++i)
            {
                const float t = to == from ? 1.0f : (float) (i - lastIndex) / (float) (index - lastIndex);
                cycle[(size_t) i] = lastValue + t * (value - lastValue);
            }
        }

        lastIndex = index;
        lastValue = value;
        repaint();
    }

    void WaveformView::mouseDown (const juce::MouseEvent& e)
    {
        if (! drawMode) return;
        drawing = true;
        lastIndex = -1;
        drawTo (e.position);
    }

    void WaveformView::mouseDrag (const juce::MouseEvent& e)
    {
        if (drawing) drawTo (e.position);
    }

    void WaveformView::mouseUp (const juce::MouseEvent&)
    {
        if (! drawing) return;
        drawing = false;
        lastIndex = -1;
        if (onDrawn) onDrawn (cycle);
        repaint();
    }

    //==============================================================================
    void WavetableView::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (Colours::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

        if (frames.empty())
        {
            g.setColour (Colours::dimText);
            g.drawFittedText (emptyText, getLocalBounds().reduced (40, 20), juce::Justification::centred, 3);
            return;
        }

        // Back frames are shifted up and right to fake depth.
        auto area = r.reduced (14.0f, 12.0f);
        const float depthX = area.getWidth() * 0.22f, depthY = area.getHeight() * 0.42f;
        const float waveW = area.getWidth() - depthX, waveH = (area.getHeight() - depthY) * 0.9f;

        const int numFrames = (int) frames.size();
        const int maxDrawn = 48;
        const int stride = std::max (1, numFrames / maxDrawn);
        const int highlighted = juce::roundToInt (position * (float) (numFrames - 1));

        auto pathFor = [&] (int f)
        {
            const float d = numFrames > 1 ? (float) f / (float) (numFrames - 1) : 0.0f;
            const float ox = area.getX() + depthX * d;
            const float baseY = area.getBottom() - waveH * 0.5f - depthY * d;
            const auto& c = frames[(size_t) f];

            juce::Path p;
            const int step = std::max (1, kFrameSize / (int) std::max (32.0f, waveW));
            for (int i = 0; i < kFrameSize; i += step)
            {
                const float x = ox + waveW * (float) i / (float) (kFrameSize - 1);
                const float y = baseY - c[(size_t) i] * waveH * 0.5f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };

        for (int f = numFrames - 1; f >= 0; --f)
        {
            if (f % stride != 0 && f != numFrames - 1)
                continue;

            const float t = numFrames > 1 ? (float) f / (float) (numFrames - 1) : 0.0f;
            const auto colour = Colours::accentA.interpolatedWith (Colours::accentB, t);
            g.setColour (colour.withAlpha (0.10f + 0.25f * (1.0f - t)));
            g.strokePath (pathFor (f), juce::PathStrokeType (1.0f));
        }

        const auto hp = pathFor (highlighted);
        g.setColour (juce::Colours::white.withAlpha (0.20f));
        g.strokePath (hp, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white);
        g.strokePath (hp, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (Colours::dimText);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Frame " + juce::String (highlighted + 1) + " / " + juce::String (numFrames),
                    getLocalBounds().reduced (12, 8), juce::Justification::topLeft);
        g.drawText ("click or drag to scan", getLocalBounds().reduced (12, 8), juce::Justification::topRight);
    }

    void WavetableView::mouseDrag (const juce::MouseEvent& e)
    {
        // Horizontal drag maps straight to morph position.
        const float p = juce::jlimit (0.0f, 1.0f, (e.position.x - 14.0f) / std::max (1.0f, (float) getWidth() - 28.0f));
        if (onPositionDragged) onPositionDragged (p);
    }

    //==============================================================================
    MorphLookAndFeel::MorphLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
        setColour (juce::Label::textColourId, Colours::text);
        setColour (juce::Slider::textBoxTextColourId, Colours::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, Colours::accent.withAlpha (0.4f));
        setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
        setColour (juce::Slider::trackColourId, Colours::accent);
        setColour (juce::Slider::backgroundColourId, Colours::panelEdge);
        setColour (juce::Slider::thumbColourId, Colours::text);
        setColour (juce::ComboBox::backgroundColourId, Colours::background);
        setColour (juce::ComboBox::outlineColourId, Colours::panelEdge);
        setColour (juce::ComboBox::textColourId, Colours::text);
        setColour (juce::ComboBox::arrowColourId, Colours::dimText);
        setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent.withAlpha (0.35f));
        setColour (juce::PopupMenu::textColourId, Colours::text);
        setColour (juce::TextButton::buttonColourId, Colours::panelEdge);
        setColour (juce::TextButton::buttonOnColourId, Colours::accent.withAlpha (0.55f));
        setColour (juce::TextButton::textColourOffId, Colours::text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::TextEditor::backgroundColourId, Colours::background);
        setColour (juce::TextEditor::outlineColourId, Colours::panelEdge);
        setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
        setColour (juce::TextEditor::textColourId, Colours::text);
        setColour (juce::ToggleButton::textColourId, Colours::text);
        setColour (juce::ToggleButton::tickColourId, Colours::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, Colours::dimText);
    }

    void MorphLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                             float startAngle, float endAngle, juce::Slider& s)
    {
        auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
        const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float lineW = std::max (2.5f, radius * 0.14f);
        const float arcR = radius - lineW * 0.5f;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (Colours::panelEdge);
        g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const juce::Point<float> tip (centre.x + (arcR - lineW * 1.6f) * std::sin (angle),
                                      centre.y - (arcR - lineW * 1.6f) * std::cos (angle));
        g.setColour (Colours::text);
        g.drawLine ({ centre, tip }, 2.0f);
    }

    void MorphLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                                                 bool highlighted, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
        if (down) c = c.brighter (0.2f); else if (highlighted) c = c.brighter (0.08f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 5.0f);
    }
}
