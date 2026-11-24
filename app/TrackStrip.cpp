#include "TrackStrip.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

namespace
{
    /** "Audio 3" -> "3"; falls back to "On" if the name has no number. */
    juce::String trackNumberText (const juce::String& name)
    {
        const auto digits = name.retainCharacters ("0123456789");
        return digits.isNotEmpty() ? digits : juce::String ("On");
    }

    juce::String gainText (float db)
    {
        if (db <= AudioTrack::minGainDb + 0.05f)
            return "-inf";

        return juce::String::formatted ("%.1f dB", db);
    }

    /** "C", "37L", "50R" ... */
    juce::String panText (double pan)
    {
        const auto amount = juce::roundToInt (std::abs (pan) * 100.0);

        if (amount == 0)
            return "C";

        return juce::String (amount) + (pan < 0.0 ? "L" : "R");
    }

    /** Vertical dB fader with 0 dB at ~75% of the travel. */
    void configureGainFader (juce::Slider& fader)
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);

        constexpr double lo = AudioTrack::minGainDb;
        constexpr double hi = AudioTrack::maxGainDb;

        // Skew such that the normalised value 0.75 maps to 0 dB.
        const auto zeroProportion = (0.0 - lo) / (hi - lo);
        const auto skew = std::log (0.75) / std::log (zeroProportion);
        const auto midDb = lo + (hi - lo) * std::pow (0.5, 1.0 / skew);

        juce::NormalisableRange<double> range (lo, hi, 0.1);
        range.setSkewForCentre (midDb);
        fader.setNormalisableRange (range);

        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setMouseClickGrabsKeyboardFocus (false);
    }

    void styleStripButton (juce::TextButton& button, juce::Colour onColour)
    {
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonColourId, colours::panelRaised);
        button.setColour (juce::TextButton::buttonOnColourId, onColour);
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        button.setColour (juce::TextButton::textColourOffId, colours::textDim);
        button.setMouseClickGrabsKeyboardFocus (false);
    }

    void styleStripLabel (juce::Label& label, float fontHeight, juce::Colour textColour)
    {
        label.setFont (juce::FontOptions (fontHeight));
        label.setColour (juce::Label::textColourId, textColour);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
    }

    void styleStripHeader (juce::Label& label)
    {
        label.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        label.setColour (juce::Label::backgroundColourId, colours::panelRaised);
        label.setColour (juce::Label::textColourId, colours::text);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
    }
}

//==============================================================================
TrackStrip::TrackStrip (AudioTrack& audioTrack)
    : track (audioTrack)
{
    nameLabel.setText (track.getName(), juce::dontSendNotification);
    styleStripHeader (nameLabel);
    addAndMakeVisible (nameLabel);

    panKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setValue (track.getPan(), juce::dontSendNotification);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panKnob.setPopupDisplayEnabled (true, true, this);
    panKnob.textFromValueFunction = [] (double value) { return panText (value); };
    panKnob.setMouseClickGrabsKeyboardFocus (false);
    panKnob.onValueChange = [this] { track.setPan ((float) panKnob.getValue()); };
    addAndMakeVisible (panKnob);

    // Ableton-style track activator: lit means audible, dark means muted.
    muteButton.setButtonText (trackNumberText (track.getName()));
    styleStripButton (muteButton, colours::mute);
    muteButton.setToggleState (! track.isMuted(), juce::dontSendNotification);
    muteButton.onClick = [this] { track.setMute (! muteButton.getToggleState()); };
    addAndMakeVisible (muteButton);

    styleStripButton (soloButton, colours::solo);
    soloButton.setToggleState (track.isSoloed(), juce::dontSendNotification);
    soloButton.onClick = [this] { track.setSolo (soloButton.getToggleState()); };
    addAndMakeVisible (soloButton);

    configureGainFader (fader);
    fader.setValue (track.getGainDb(), juce::dontSendNotification);
    fader.onValueChange = [this]
    {
        track.setGainDb ((float) fader.getValue());
        gainLabel.setText (gainText (track.getGainDb()), juce::dontSendNotification);
    };
    addAndMakeVisible (fader);

    styleStripLabel (gainLabel, 10.0f, colours::textDim);
    gainLabel.setText (gainText (track.getGainDb()), juce::dontSendNotification);
    addAndMakeVisible (gainLabel);

    addAndMakeVisible (meter);

    refresh();
}

void TrackStrip::refresh()
{
    meter.setLevels (track.getMeter().consume (0), track.getMeter().consume (1));

    // Resync from the track without notifications, but never fight the user mid-drag.
    if (! fader.isMouseButtonDown())
    {
        fader.setValue (track.getGainDb(), juce::dontSendNotification);
        gainLabel.setText (gainText (track.getGainDb()), juce::dontSendNotification);
    }

    if (! panKnob.isMouseButtonDown())
        panKnob.setValue (track.getPan(), juce::dontSendNotification);

    muteButton.setToggleState (! track.isMuted(), juce::dontSendNotification);
    soloButton.setToggleState (track.isSoloed(), juce::dontSendNotification);

    if (nameLabel.getText() != track.getName())
    {
        nameLabel.setText (track.getName(), juce::dontSendNotification);
        muteButton.setButtonText (trackNumberText (track.getName()));
    }
}

void TrackStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

void TrackStrip::resized()
{
    auto area = getLocalBounds().reduced (5);

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (5);


    auto panRow = area.removeFromTop (40);
    panKnob.setBounds (panRow.withSizeKeepingCentre (juce::jmin (36, panRow.getWidth()), 36));
    area.removeFromTop (4);

    auto buttonRow = area.removeFromTop (20);
    const auto buttonWidth = (buttonRow.getWidth() - 4) / 2;
    muteButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (4);
    soloButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    area.removeFromTop (6);

    gainLabel.setBounds (area.removeFromBottom (14));
    area.removeFromBottom (2);

    meter.setBounds (area.removeFromRight (16));
    area.removeFromRight (4);

    fader.setBounds (area);
}

//==============================================================================
MasterStrip::MasterStrip (Engine& e) : engine (e)
{
    nameLabel.setText ("Master", juce::dontSendNotification);
    styleStripHeader (nameLabel);
    addAndMakeVisible (nameLabel);

    configureGainFader (fader);
    fader.setValue (engine.getMasterGainDb(), juce::dontSendNotification);
    fader.onValueChange = [this]
    {
        engine.setMasterGainDb ((float) fader.getValue());
        gainLabel.setText (gainText (engine.getMasterGainDb()), juce::dontSendNotification);
    };
    addAndMakeVisible (fader);

    styleStripLabel (gainLabel, 10.0f, colours::textDim);
    gainLabel.setText (gainText (engine.getMasterGainDb()), juce::dontSendNotification);
    addAndMakeVisible (gainLabel);

    addAndMakeVisible (meter);

    refresh();
}

void MasterStrip::refresh()
{
    meter.setLevels (engine.getMasterMeter().consume (0), engine.getMasterMeter().consume (1));

    if (! fader.isMouseButtonDown())
    {
        fader.setValue (engine.getMasterGainDb(), juce::dontSendNotification);
        gainLabel.setText (gainText (engine.getMasterGainDb()), juce::dontSendNotification);
    }
}

void MasterStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced (5);

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    gainLabel.setBounds (area.removeFromBottom (14));
    area.removeFromBottom (2);

    meter.setBounds (area.removeFromRight (16));
    area.removeFromRight (4);

    fader.setBounds (area);
}

} // namespace daw
