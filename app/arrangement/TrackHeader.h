#pragma once

#include "LevelMeter.h"
#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace daw
{

/** Left-hand header of one arrangement lane (Ableton style).

    Contents: a thin strip in the track colour, the track name (double-click to rename via
    Project::setTrackName), the track activator button (lit = audible; drives
    AudioTrack::setMute), solo button, a compact horizontal volume slider bound to
    AudioTrack::setGainDb, and a small stereo LevelMeter fed from the engine track's meter.

    Message thread only. */
class TrackHeader : public juce::Component
{
public:
    TrackHeader (Project& project, juce::ValueTree track);

    /** Called ~30 Hz by the owner: meter levels, and resync controls/name from the model. */
    void refresh();

    juce::ValueTree getTrack() const { return track; }

    static constexpr int preferredWidth = 180;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Project& project;
    juce::ValueTree track;

    juce::Label nameLabel;
    juce::TextButton activatorButton;
    juce::TextButton soloButton;
    juce::Slider volumeSlider;
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

} // namespace daw
