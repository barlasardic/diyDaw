#include "TransportBar.h"
#include "DawLookAndFeel.h"
#include "ProjectControls.h"

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

TransportBar::TransportBar (Project& p) : project (p), engine (p.getEngine())
{
    auto& transport = engine.getTransport();

    fileButton.setMouseClickGrabsKeyboardFocus (false);
    fileButton.setTooltip ("New, open and save projects");
    fileButton.onClick = [this] { if (onFileMenu) onFileMenu(); };
    addAndMakeVisible (fileButton);

    styleIconButton (playButton, makePlayIcon(), colours::play);
    playButton.shouldUseOnColours (true);
    playButton.setClickingTogglesState (false);
    playButton.onClick = [this] { if (onPlay) onPlay(); else engine.getTransport().play(); };
    addAndMakeVisible (playButton);

    // Ableton behaviour: first press pauses, second press returns to the start.
    styleIconButton (stopButton, makeStopIcon(), colours::text);
    stopButton.onClick = [this]
    {
        if (onStop)
        {
            onStop();
            return;
        }

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
    bindSliderToProject (bpmSlider, project, "Change Tempo", [this] (double v) { project.setBpm (v); });
    addAndMakeVisible (bpmSlider);

    bpmCaption.setColour (juce::Label::textColourId, colours::textDim);
    bpmCaption.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (bpmCaption);

    // FL-style mode switch: PAT plays only the clip open in the piano roll, SONG the arrangement.
    for (auto* b : { &patternButton, &songButton })
    {
        b->setClickingTogglesState (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        b->setColour (juce::TextButton::buttonOnColourId, colours::accent);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        addAndMakeVisible (*b);
    }
    patternButton.setTooltip ("Pattern mode: loop only the clip open in the piano roll");
    songButton.setTooltip ("Song mode: play the arrangement");
    patternButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    songButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    patternButton.onClick = [this] { if (onModeChanged) onModeChanged (true); };
    songButton.onClick = [this] { if (onModeChanged) onModeChanged (false); };
    setPatternMode (false);

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
    addTrackButton.setTooltip ("New audio track (Ctrl+T)");
    addTrackButton.onClick = [this] { if (onAddTrack) onAddTrack(); };
    addAndMakeVisible (addTrackButton);

    addMidiTrackButton.setMouseClickGrabsKeyboardFocus (false);
    addMidiTrackButton.setTooltip ("New MIDI track with a synth (Ctrl+Shift+T)");
    addMidiTrackButton.onClick = [this] { if (onAddMidiTrack) onAddMidiTrack(); };
    addAndMakeVisible (addMidiTrackButton);

    viewButton.setMouseClickGrabsKeyboardFocus (false);
    viewButton.setTooltip ("Switch between arrangement and mixer (Tab)");
    viewButton.onClick = [this] { if (onToggleView) onToggleView(); };
    addAndMakeVisible (viewButton);

    settingsButton.setMouseClickGrabsKeyboardFocus (false);
    settingsButton.onClick = [this] { if (onAudioSettings) onAudioSettings(); };
    addAndMakeVisible (settingsButton);

    refresh();
}

void TransportBar::setPatternMode (bool isPattern)
{
    patternButton.setToggleState (isPattern, juce::dontSendNotification);
    songButton.setToggleState (! isPattern, juce::dontSendNotification);
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

    fileButton.setBounds (area.removeFromLeft (52).reduced (0, 2));
    area.removeFromLeft (12);

    auto left = area.removeFromLeft (220);
    bpmCaption.setBounds (left.removeFromLeft (30));
    bpmSlider.setBounds (left.removeFromLeft (70).reduced (0, 2));
    left.removeFromLeft (8);
    metronomeButton.setBounds (left.removeFromLeft (56).reduced (0, 2));

    settingsButton.setBounds (area.removeFromRight (80).reduced (0, 2));
    area.removeFromRight (6);
    addMidiTrackButton.setBounds (area.removeFromRight (70).reduced (0, 2));
    area.removeFromRight (6);
    addTrackButton.setBounds (area.removeFromRight (70).reduced (0, 2));
    area.removeFromRight (6);
    viewButton.setBounds (area.removeFromRight (100).reduced (0, 2));

    // Centre cluster: pattern/song, play, stop, position
    auto centre = area.withSizeKeepingCentre (300, area.getHeight());
    auto modes = centre.removeFromLeft (92).reduced (0, 4);
    patternButton.setBounds (modes.removeFromLeft (46));
    songButton.setBounds (modes);
    centre.removeFromLeft (8);
    playButton.setBounds (centre.removeFromLeft (36));
    stopButton.setBounds (centre.removeFromLeft (36));
    centre.removeFromLeft (8);
    positionLabel.setBounds (centre.reduced (0, 2));
}

} // namespace daw
