#include "TransportBar.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

namespace
{
    juce::Path makePlayIcon()
    {
        juce::Path p;
        p.addTriangle (0.0f, 0.0f, 0.0f, 1.0f, 0.866f, 0.5f);
        return p;
    }

    juce::Path makeStopIcon()
    {
        juce::Path p;
        p.addRectangle (0.0f, 0.0f, 1.0f, 1.0f);
        return p;
    }

    void styleIconButton (juce::ShapeButton& b, const juce::Path& shape, juce::Colour onColour)
    {
        b.setShape (shape, false, true, false);
        b.setColours (colours::textDim, colours::text, colours::text.darker (0.2f));
        b.setOnColours (onColour, onColour.brighter (0.2f), onColour.darker (0.2f));
        b.setBorderSize (juce::BorderSize<int> (7));
        b.setMouseClickGrabsKeyboardFocus (false);
    }
}

TransportBar::TransportBar (Engine& e) : engine (e)
{
    auto& transport = engine.getTransport();

    styleIconButton (playButton, makePlayIcon(), colours::play);
    playButton.shouldUseOnColours (true);
    playButton.setClickingTogglesState (false);
    playButton.onClick = [this] { engine.getTransport().play(); };
    addAndMakeVisible (playButton);

    // Ableton behaviour: first press pauses, second press returns to the start.
    styleIconButton (stopButton, makeStopIcon(), colours::text);
    stopButton.onClick = [this]
    {
        auto& t = engine.getTransport();
        if (t.isPlaying())
            t.stop();
        else
            t.setPositionBeats (0.0);
    };
    addAndMakeVisible (stopButton);

    bpmSlider.setSliderStyle (juce::Slider::LinearBarVertical);
    bpmSlider.setRange (Transport::minBpm, Transport::maxBpm, 0.01);
    bpmSlider.setValue (transport.getBpm(), juce::dontSendNotification);
    bpmSlider.setNumDecimalPlacesToDisplay (2);
    bpmSlider.setMouseDragSensitivity (600);
    bpmSlider.setVelocityBasedMode (false);
    bpmSlider.setDoubleClickReturnValue (true, 120.0);
    bpmSlider.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    bpmSlider.onValueChange = [this] { engine.getTransport().setBpm (bpmSlider.getValue()); };
    addAndMakeVisible (bpmSlider);

    bpmCaption.setColour (juce::Label::textColourId, colours::textDim);
    bpmCaption.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (bpmCaption);

    metronomeButton.setClickingTogglesState (true);
    metronomeButton.setMouseClickGrabsKeyboardFocus (false);
    metronomeButton.onClick = [this] { engine.getMetronome().setEnabled (metronomeButton.getToggleState()); };
    addAndMakeVisible (metronomeButton);

    positionLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 18.0f, juce::Font::plain));
    positionLabel.setJustificationType (juce::Justification::centred);
    positionLabel.setColour (juce::Label::backgroundColourId, colours::background);
    positionLabel.setColour (juce::Label::outlineColourId, colours::outline);
    addAndMakeVisible (positionLabel);

    addTrackButton.setMouseClickGrabsKeyboardFocus (false);
    addTrackButton.onClick = [this] { if (onAddTrack) onAddTrack(); };
    addAndMakeVisible (addTrackButton);

    settingsButton.setMouseClickGrabsKeyboardFocus (false);
    settingsButton.onClick = [this] { if (onAudioSettings) onAudioSettings(); };
    addAndMakeVisible (settingsButton);

    refresh();
}

juce::String TransportBar::formatPosition (double beats, int beatsPerBar)
{
    beats = juce::jmax (0.0, beats);
    // Small epsilon so exact grid positions don't display as the previous sixteenth.
    const auto sixteenths = (long long) std::floor (beats * 4.0 + 1.0e-9);
    const auto perBar = (long long) beatsPerBar * 4;

    const auto bar = sixteenths / perBar + 1;
    const auto beat = (sixteenths % perBar) / 4 + 1;
    const auto sixteenth = sixteenths % 4 + 1;

    return juce::String (bar) + "." + juce::String (beat) + "." + juce::String (sixteenth);
}

void TransportBar::refresh()
{
    auto& transport = engine.getTransport();

    playButton.setToggleState (transport.isPlaying(), juce::dontSendNotification);
    metronomeButton.setToggleState (engine.getMetronome().isEnabled(), juce::dontSendNotification);
    positionLabel.setText (formatPosition (transport.getPositionBeats(), transport.getBeatsPerBar()),
                           juce::dontSendNotification);

    if (! bpmSlider.isMouseButtonDown())
        bpmSlider.setValue (transport.getBpm(), juce::dontSendNotification);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto left = area.removeFromLeft (220);
    bpmCaption.setBounds (left.removeFromLeft (30));
    bpmSlider.setBounds (left.removeFromLeft (70).reduced (0, 2));
    left.removeFromLeft (8);
    metronomeButton.setBounds (left.removeFromLeft (56).reduced (0, 2));

    settingsButton.setBounds (area.removeFromRight (80).reduced (0, 2));
    area.removeFromRight (6);
    addTrackButton.setBounds (area.removeFromRight (80).reduced (0, 2));

    // Centre cluster: play, stop, position
    auto centre = area.withSizeKeepingCentre (200, area.getHeight());
    playButton.setBounds (centre.removeFromLeft (36));
    stopButton.setBounds (centre.removeFromLeft (36));
    centre.removeFromLeft (8);
    positionLabel.setBounds (centre.reduced (0, 2));
}

} // namespace daw
