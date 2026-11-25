#include "TrackHeader.h"
#include "DawLookAndFeel.h"
#include "ProjectControls.h"

#include <cmath>

namespace daw
{

namespace
{
    juce::String gainText (float db)
    {
        if (db <= AudioTrack::minGainDb + 0.05f)
            return "-inf";

        return juce::String::formatted ("%.1f dB", db);
    }

    /** Horizontal dB fader with 0 dB at ~75% of the travel (same curve as TrackStrip's). */
    void configureVolumeSlider (juce::Slider& slider)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);

        constexpr double lo = AudioTrack::minGainDb;
        constexpr double hi = AudioTrack::maxGainDb;

        // Skew such that the normalised value 0.75 maps to 0 dB.
        const auto zeroProportion = (0.0 - lo) / (hi - lo);
        const auto skew = std::log (0.75) / std::log (zeroProportion);
        const auto midDb = lo + (hi - lo) * std::pow (0.5, 1.0 / skew);

        juce::NormalisableRange<double> range (lo, hi, 0.1);
        range.setSkewForCentre (midDb);
        slider.setNormalisableRange (range);

        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setPopupDisplayEnabled (true, true, nullptr);
        slider.setMouseClickGrabsKeyboardFocus (false);
    }

    void styleHeaderButton (juce::TextButton& button, juce::Colour onColour)
    {
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonColourId, colours::panelRaised);
        button.setColour (juce::TextButton::buttonOnColourId, onColour);
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        button.setColour (juce::TextButton::textColourOffId, colours::textDim);
        button.setMouseClickGrabsKeyboardFocus (false);
    }
}

TrackHeader::TrackHeader (Project& p, juce::ValueTree t)
    : project (p), track (t)
{
    // ---- name -----------------------------------------------------------
    nameLabel.setText (track[ids::name].toString(), juce::dontSendNotification);
    nameLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    nameLabel.setColour (juce::Label::textWhenEditingColourId, colours::text);
    nameLabel.setColour (juce::Label::backgroundWhenEditingColourId, colours::panelRaised);
    nameLabel.setColour (juce::Label::outlineWhenEditingColourId, colours::accent);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setEditable (false, true);
    nameLabel.setMouseClickGrabsKeyboardFocus (false);
    nameLabel.onTextChange = [this]
    {
        project.beginTransaction ("Rename Track");
        project.setTrackName (track, nameLabel.getText());
    };
    addAndMakeVisible (nameLabel);

    // ---- track activator (lit = audible) --------------------------------
    styleHeaderButton (activatorButton, colours::mute);
    bindToggleToProject (activatorButton, project, "Toggle Track Activator",
                         [this] (bool lit) { project.setTrackMute (track, ! lit); });
    addAndMakeVisible (activatorButton);

    // ---- solo -----------------------------------------------------------
    soloButton.setButtonText ("S");
    styleHeaderButton (soloButton, colours::solo);
    bindToggleToProject (soloButton, project, "Toggle Solo",
                         [this] (bool on) { project.setTrackSolo (track, on); });
    addAndMakeVisible (soloButton);

    // ---- volume ---------------------------------------------------------
    configureVolumeSlider (volumeSlider);
    volumeSlider.textFromValueFunction = [] (double value) { return gainText ((float) value); };
    bindSliderToProject (volumeSlider, project, "Change Volume", [this] (double v)
    {
        project.setTrackGain (track, (float) v);
        volumeSlider.setTooltip (gainText (Project::getTrackGain (track)));
    });
    addAndMakeVisible (volumeSlider);

    // ---- meter ----------------------------------------------------------
    addAndMakeVisible (meter);

    refresh();
}

void TrackHeader::refresh()
{
    auto& audioTrack = project.getEngineTrack (track);

    meter.setLevels (audioTrack.getMeter().consume (0), audioTrack.getMeter().consume (1));

    // Resync from the model without notifications, but never fight the user mid-drag.
    if (! volumeSlider.isMouseButtonDown())
    {
        volumeSlider.setValue (Project::getTrackGain (track), juce::dontSendNotification);
        volumeSlider.setTooltip (gainText (Project::getTrackGain (track)));
    }

    activatorButton.setToggleState (! Project::isTrackMuted (track), juce::dontSendNotification);
    soloButton.setToggleState (Project::isTrackSoloed (track), juce::dontSendNotification);

    const auto index = project.indexOfTrack (track);
    const auto numberText = index >= 0 ? juce::String (index + 1) : juce::String ("?");

    if (activatorButton.getButtonText() != numberText)
        activatorButton.setButtonText (numberText);

    const auto modelName = track[ids::name].toString();

    if (! nameLabel.isBeingEdited() && nameLabel.getText() != modelName)
        nameLabel.setText (modelName, juce::dontSendNotification);
}

void TrackHeader::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    g.setColour (colours::panel);
    g.fillRect (bounds);

    g.setColour (Project::getTrackColour (track));
    g.fillRect (bounds.withWidth (4));

    g.setColour (colours::outline);
    g.fillRect (0, bounds.getBottom() - 1, bounds.getWidth(), 1);
}

void TrackHeader::resized()
{
    auto area = getLocalBounds();
    area.removeFromLeft (4);
    area = area.reduced (6, 4);

    // Meter hugs the right edge; the rest is name row + controls row.
    meter.setBounds (area.removeFromRight (10).reduced (0, 2));
    area.removeFromRight (5);

    // Centre the two rows vertically so taller lanes don't look top-heavy.
    area = area.withSizeKeepingCentre (area.getWidth(), juce::jlimit (0, 46, area.getHeight()));

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    auto controls = area.removeFromTop (22);
    activatorButton.setBounds (controls.removeFromLeft (24));
    controls.removeFromLeft (4);
    soloButton.setBounds (controls.removeFromLeft (22));
    controls.removeFromLeft (6);
    volumeSlider.setBounds (controls);
}

} // namespace daw
