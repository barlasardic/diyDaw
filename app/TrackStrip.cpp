#include "TrackStrip.h"
#include "DawLookAndFeel.h"
#include "ProjectControls.h"

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
TrackStrip::TrackStrip (Project& p, juce::ValueTree t)
    : project (p), track (t)
{
    styleStripHeader (nameLabel);
    addAndMakeVisible (nameLabel);

    panKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panKnob.setPopupDisplayEnabled (true, true, this);
    panKnob.textFromValueFunction = [] (double value) { return panText (value); };
    panKnob.setMouseClickGrabsKeyboardFocus (false);
    bindSliderToProject (panKnob, project, "Change Pan",
                         [this] (double v) { project.setTrackPan (track, (float) v); });
    addAndMakeVisible (panKnob);

    // Ableton-style track activator: lit means audible, dark means muted.
    styleStripButton (muteButton, colours::mute);
    bindToggleToProject (muteButton, project, "Toggle Track Activator",
                         [this] (bool lit) { project.setTrackMute (track, ! lit); });
    addAndMakeVisible (muteButton);

    styleStripButton (soloButton, colours::solo);
    bindToggleToProject (soloButton, project, "Toggle Solo",
                         [this] (bool on) { project.setTrackSolo (track, on); });
    addAndMakeVisible (soloButton);

    configureGainFader (fader);
    bindSliderToProject (fader, project, "Change Volume", [this] (double v)
    {
        project.setTrackGain (track, (float) v);
        gainLabel.setText (gainText (Project::getTrackGain (track)), juce::dontSendNotification);
    });
    addAndMakeVisible (fader);

    styleStripLabel (gainLabel, 10.0f, colours::textDim);
    addAndMakeVisible (gainLabel);

    addAndMakeVisible (meter);

    refresh();
}

void TrackStrip::refresh()
{
    auto& engineTrack = project.getEngineTrack (track);
    meter.setLevels (engineTrack.getMeter().consume (0), engineTrack.getMeter().consume (1));

    // Resync from the model without notifications, but never fight the user mid-drag.
    // Automated parameters show what the engine is playing.
    const auto gainAutomated = Project::isAutomated (track, "gain");
    const auto panAutomated = Project::isAutomated (track, "pan");
    showAutomationState (fader, gainAutomated);
    showAutomationState (panKnob, panAutomated);

    const auto gain = gainAutomated ? project.getEngineValue (track, "gain") : Project::getTrackGain (track);

    if (! fader.isMouseButtonDown())
    {
        fader.setValue (gain, juce::dontSendNotification);
        gainLabel.setText (gainText (gain), juce::dontSendNotification);
    }

    if (! panKnob.isMouseButtonDown())
        panKnob.setValue (panAutomated ? project.getEngineValue (track, "pan") : Project::getTrackPan (track),
                          juce::dontSendNotification);

    muteButton.setToggleState (! Project::isTrackMuted (track), juce::dontSendNotification);
    soloButton.setToggleState (Project::isTrackSoloed (track), juce::dontSendNotification);

    const auto name = track[ids::name].toString();

    if (nameLabel.getText() != name)
    {
        nameLabel.setText (name, juce::dontSendNotification);
        muteButton.setButtonText (trackNumberText (name));
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
MasterStrip::MasterStrip (Project& p) : project (p)
{
    nameLabel.setText ("Master", juce::dontSendNotification);
    styleStripHeader (nameLabel);
    addAndMakeVisible (nameLabel);

    configureGainFader (fader);
    bindSliderToProject (fader, project, "Change Master Volume", [this] (double v)
    {
        project.setMasterGain ((float) v);
        gainLabel.setText (gainText (project.getMasterGain()), juce::dontSendNotification);
    });
    addAndMakeVisible (fader);

    styleStripLabel (gainLabel, 10.0f, colours::textDim);
    addAndMakeVisible (gainLabel);

    addAndMakeVisible (meter);

    refresh();
}

void MasterStrip::refresh()
{
    auto& meterSource = project.getEngine().getMasterMeter();
    meter.setLevels (meterSource.consume (0), meterSource.consume (1));

    if (! fader.isMouseButtonDown())
    {
        fader.setValue (project.getMasterGain(), juce::dontSendNotification);
        gainLabel.setText (gainText (project.getMasterGain()), juce::dontSendNotification);
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
