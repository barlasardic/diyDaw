#include "DawLookAndFeel.h"

namespace daw
{

DawLookAndFeel::DawLookAndFeel()
{
    using namespace colours;

    setColourScheme ({ background, panel, panelRaised, outline, text, accent,
                       juce::Colours::black, accent, text });

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::TextButton::buttonColourId, panelRaised);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, panelRaised);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, outline);
    setColour (juce::Slider::thumbColourId, accent);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, outline);
    setColour (juce::ScrollBar::thumbColourId, panelRaised);
}

void DawLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                           bool isMouseOverButton, bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = backgroundColour;

    if (isButtonDown)
        colour = colour.darker (0.2f);
    else if (isMouseOverButton)
        colour = colour.brighter (0.08f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
}

void DawLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto lineW = juce::jmax (2.0f, radius * 0.18f);
    const auto arcRadius = radius - lineW * 0.5f;
    const auto angle = startAngle + pos * (endAngle - startAngle);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (colours::outline);
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar sliders (e.g. pan) fill from the centre, others from the start.
    const auto bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const auto fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, fromAngle, angle, true);
    g.setColour (slider.isEnabled() ? colours::accent : colours::textDim);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto tip = centre.getPointOnCircumference (arcRadius - lineW * 1.5f, angle);
    g.setColour (colours::text);
    g.drawLine ({ centre, tip }, 1.5f);
}

void DawLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float minSliderPos, float maxSliderPos,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto vertical = style == juce::Slider::LinearVertical;
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto trackW = 4.0f;

    auto trackRect = vertical ? area.withSizeKeepingCentre (trackW, area.getHeight())
                              : area.withSizeKeepingCentre (area.getWidth(), trackW);
    g.setColour (colours::outline);
    g.fillRoundedRectangle (trackRect, 2.0f);

    auto filled = vertical ? trackRect.withTop (sliderPos) : trackRect.withRight (sliderPos);
    g.setColour (colours::accent.withAlpha (0.6f));
    g.fillRoundedRectangle (filled, 2.0f);

    // Fader cap
    const auto capW = vertical ? juce::jmin (26.0f, area.getWidth()) : 10.0f;
    const auto capH = vertical ? 10.0f : juce::jmin (26.0f, area.getHeight());
    const auto cap = vertical ? juce::Rectangle<float> (capW, capH).withCentre ({ area.getCentreX(), sliderPos })
                              : juce::Rectangle<float> (capW, capH).withCentre ({ sliderPos, area.getCentreY() });
    g.setColour (colours::panelRaised.brighter (0.15f));
    g.fillRoundedRectangle (cap, 2.0f);
    g.setColour (colours::accent);
    if (vertical)
        g.fillRect (cap.withSizeKeepingCentre (capW - 4.0f, 2.0f));
    else
        g.fillRect (cap.withSizeKeepingCentre (2.0f, capH - 4.0f));
}

juce::Font DawLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (13.0f, (float) buttonHeight * 0.6f), juce::Font::bold);
}

juce::Font DawLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

} // namespace daw
