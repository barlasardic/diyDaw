#pragma once

#include "LevelMeter.h"
#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Left-hand header of one arrangement lane (Ableton style).

    Contents: a thin strip in the track colour, the track name (double-click to rename via
    Project::setTrackName), the track activator button (lit = audible), solo button and a compact
    horizontal volume slider, all editing the Project (undoable), and a small stereo LevelMeter fed from the engine track's meter.

    Message thread only. */
class TrackHeader : public juce::Component
{
public:
    TrackHeader (Project& project, juce::ValueTree track);

    /** Called ~30 Hz by the owner: meter levels, and resync controls/name from the model. */
    void refresh();

    void setAutomationShown (bool shown);
    std::function<void (bool)> onAutomationToggled;

    juce::ValueTree getTrack() const { return track; }

    static constexpr int preferredWidth = 180;
    static constexpr int minWidth = 140, maxWidth = 360;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Project& project;
    juce::ValueTree track;

    juce::Label nameLabel;
    juce::TextButton activatorButton;
    juce::TextButton soloButton;
    juce::TextButton automationButton { "A" };
    juce::Slider volumeSlider;
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

} // namespace daw
