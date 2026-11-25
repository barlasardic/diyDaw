#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace daw::colours
{
    inline const juce::Colour background   { 0xff1b1c1e };
    inline const juce::Colour panel        { 0xff2a2b2e };
    inline const juce::Colour panelRaised  { 0xff36383c };
    inline const juce::Colour outline      { 0xff111213 };
    inline const juce::Colour text         { 0xffd9dadc };
    inline const juce::Colour textDim      { 0xff8b8d91 };
    inline const juce::Colour accent       { 0xffffa53a }; // selection / fader caps / active
    inline const juce::Colour play         { 0xff5fcf6b };
    inline const juce::Colour mute         { 0xfff2c94c }; // "track active" yellow, lit when NOT muted
    inline const juce::Colour solo         { 0xff4fa3ff };
    inline const juce::Colour meterLow     { 0xff4cd964 };
    inline const juce::Colour meterMid     { 0xfff2d04c };
    inline const juce::Colour meterHigh    { 0xffff4d4d };
    inline const juce::Colour automation   { 0xffff5f57 }; // controls driven by automation
}

namespace daw
{

/** Flat, dark, Ableton-flavoured theme used by the whole app. */
class DawLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DawLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
};

} // namespace daw
